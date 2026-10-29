#include "sdlog.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "bsp/m5stack_tab5.h"
#include "csvlog.h"
#include "datastore.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "settings.h"

static const char *TAG = "sdlog";

#define TZ_MIN          (9 * 60)        /* JST, as set in main */
#define TAIL_LEN        1024
#define OUT_BUF         (32 * 1024)
#define TASK_STACK      (8 * 1024)
#define TASK_PRIO       2
#define TASK_CORE       1
#define HOUR_OFFSET_S   30              /* run at hh:00:30, after the minute record */

typedef enum { REQ_CHECK, REQ_EXPORT } req_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_mutex;
static sdlog_status_t s_st;
static bool s_mounted;
static char *s_buf;

static void set_msg(const char *msg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_st.msg, sizeof(s_st.msg), "%s", msg);
    xSemaphoreGive(s_mutex);
}

static void set_state(sdlog_state_t st)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_st.state = st;
    xSemaphoreGive(s_mutex);
}

static void update_space(void)
{
    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free) == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_st.total_mb = (uint32_t)(total >> 20);
        s_st.free_mb = (uint32_t)(free >> 20);
        xSemaphoreGive(s_mutex);
    }
}

static void unmount(void)
{
    if (s_mounted) {
        bsp_sdcard_unmount();
        s_mounted = false;
    }
    set_state(SDLOG_NO_CARD);
}

/* Mount a new card, or check that the mounted one still answers */
static bool probe(void)
{
    if (s_mounted) {
        if (sdmmc_get_status(bsp_sdcard_get_handle()) == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "card removed");
        unmount();
    }
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        /* frees the LDO channel claimed by the failed mount */
        bsp_sdcard_unmount();
        ESP_LOGI(TAG, "no card (%s)", esp_err_to_name(err));
        set_state(SDLOG_NO_CARD);
        return false;
    }
    s_mounted = true;
    sdmmc_card_t *card = bsp_sdcard_get_handle();
    ESP_LOGI(TAG, "card %s mounted", card->cid.name);
    set_state(SDLOG_READY);
    update_space();
    return true;
}

static bool flush(FILE *f, size_t *used)
{
    if (*used && fwrite(s_buf, 1, *used, f) != *used) {
        return false;
    }
    *used = 0;
    return true;
}

/* Append the minutes [from, to] of one local day; returns lines written or -1 */
static int export_day(const char *dir, uint32_t from, uint32_t to)
{
    char name[16], path[48];
    csvlog_day_name(name, sizeof(name), from, TZ_MIN);
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    /* resume after the last valid line of the file */
    bool exists = false, nl = true;
    FILE *f = fopen(path, "rb");
    if (f) {
        exists = true;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        long off = size > TAIL_LEN ? size - TAIL_LEN : 0;
        fseek(f, off, SEEK_SET);
        size_t n = fread(s_buf, 1, TAIL_LEN, f);
        fclose(f);
        uint32_t last;
        if (csvlog_last_minute(s_buf, n, TZ_MIN, &last, &nl) && last >= from) {
            from = last + 1;
        }
    }
    if (from > to) {
        return 0;
    }

    f = fopen(path, "ab");
    if (!f) {
        ESP_LOGE(TAG, "open %s: %d", path, errno);
        return -1;
    }
    size_t used = 0;
    if (!exists) {
        used += (size_t)snprintf(s_buf, OUT_BUF, "%s", CSV_HEADER);
    } else if (!nl) {
        s_buf[used++] = '\n';       /* close a line cut by a power loss */
    }
    int lines = 0;
    bool ok = true;
    for (uint32_t k = from; k <= to && ok; k++) {
        hist_entry_t e;
        bool have = false;
        const history_t *h = datastore_lock_real();
        const hist_entry_t *p = h->abs_time ? history_get(h, k) : NULL;
        if (p) {
            e = *p;
            have = true;
        }
        datastore_unlock();
        if (!have) {
            continue;
        }
        if (used + CSV_LINE_MAX > OUT_BUF) {
            ok = flush(f, &used);
        }
        used += (size_t)csvlog_format(s_buf + used, OUT_BUF - used, k, TZ_MIN, &e);
        lines++;
    }
    ok = ok && flush(f, &used);
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        ESP_LOGE(TAG, "write %s failed: %d", path, errno);
        return -1;
    }
    ESP_LOGI(TAG, "%s: %d lines", path, lines);
    return lines;
}

static void do_export(bool manual)
{
    if (!clock_valid()) {
        set_msg("時刻が未設定のため書き出せません（設定の「時計」で合わせてください）");
        return;
    }
    datastore_stats_t ds;
    datastore_get_stats(&ds);
    if (!ds.abs_time) {
        set_msg("記録の準備中です。少し待ってからお試しください");
        return;
    }
    const char *dir = ds.sim ? BSP_SD_MOUNT_POINT "/ENVSIM" : BSP_SD_MOUNT_POINT "/ENVLOG";
    struct stat sb;
    if (stat(dir, &sb) != 0 && mkdir(dir, 0775) != 0) {
        ESP_LOGE(TAG, "mkdir %s: %d", dir, errno);
        set_msg("フォルダを作れませんでした。カードを確認してください");
        probe();
        return;
    }

    set_state(SDLOG_BUSY);
    const uint32_t now = (uint32_t)(time(NULL) / 60);
    const uint32_t last = now - 1;                      /* the current minute is not complete */
    uint32_t from = now - HIST_MINUTES;
    int total = 0;
    bool ok = true;
    while (from <= last && ok) {
        uint32_t day_end = csvlog_day_start(from, TZ_MIN) + 1439;
        uint32_t to = day_end < last ? day_end : last;
        int n = export_day(dir, from, to);
        ok = n >= 0;
        total += ok ? n : 0;
        from = to + 1;
    }

    if (!ok) {
        set_msg("書き込みに失敗しました。カードを確認してください");
        if (!probe()) {
            return;
        }
    } else {
        char msg[96];
        if (total) {
            snprintf(msg, sizeof(msg), "%d 分ぶんを書き出しました", total);
        } else {
            snprintf(msg, sizeof(msg), manual ? "新しい記録はありません（書き出し済み）" : "");
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_st.last_export = time(NULL);
        s_st.last_lines = (uint32_t)total;
        if (manual || total) {
            snprintf(s_st.msg, sizeof(s_st.msg), "%s", msg);
        }
        xSemaphoreGive(s_mutex);
    }
    set_state(SDLOG_READY);
    update_space();
}

static TickType_t ticks_to_next_hour(void)
{
    int64_t s;
    if (clock_valid()) {
        time_t t = time(NULL);
        s = 3600 - (t % 3600) + HOUR_OFFSET_S;
    } else {
        s = 3600;   /* only probes the card while the clock is not set */
    }
    return pdMS_TO_TICKS(s * 1000);
}

static void task(void *arg)
{
    probe();
    for (;;) {
        req_t req;
        if (xQueueReceive(s_queue, &req, ticks_to_next_hour()) == pdTRUE) {
            if (!probe()) {
                set_msg("カードが見つかりません");
            } else if (req == REQ_EXPORT) {
                do_export(true);
            } else {
                set_msg("カードを確認しました");
            }
            continue;
        }
        /* hourly */
        app_settings_t st;
        settings_get(&st);
        if (st.sd_auto_export && probe() && clock_valid()) {
            do_export(false);
        }
    }
}

void sdlog_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(4, sizeof(req_t));
    s_buf = heap_caps_malloc(OUT_BUF, MALLOC_CAP_SPIRAM);
    if (!s_mutex || !s_queue || !s_buf) {
        ESP_LOGE(TAG, "no memory");
        return;
    }
    xTaskCreatePinnedToCore(task, "sdlog", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE);
}

void sdlog_get_status(sdlog_status_t *out)
{
    if (!s_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_mutex);
}

static void request(req_t r)
{
    if (s_queue) {
        xQueueSend(s_queue, &r, 0);
    }
}

void sdlog_request_check(void)
{
    request(REQ_CHECK);
}

void sdlog_request_export(void)
{
    request(REQ_EXPORT);
}
