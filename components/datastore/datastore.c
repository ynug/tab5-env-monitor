#include "datastore.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "histlog.h"
#include "nvs.h"

static const char *TAG = "datastore";

#define PART_SUBTYPE    0x40
#define PART_LABEL      "histlog"
#define CLOCK_MIN_UNIX  1735689600LL  /* 2025-01-01 */
#define CLOCK_MAX_UNIX  4102444800LL  /* 2100-01-01 */
#define TASK_PRIO       6
#define TASK_STACK      (6 * 1024)
#define TASK_CORE       1
#define QUEUE_LEN       16

typedef struct {
    enum { MSG_MIN, MSG_10S, MSG_CLOCK } type;
    union {
        minute_rec_t min;
        sample10_t s10;
        int64_t unix_s;
    };
} msg_t;

static const esp_partition_t *s_part;
static histlog_t s_log;
static bool s_log_ok;
static history_t s_hist;
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static uint32_t s_boot_id;
static bool s_sim;
static bool s_clock_was_valid;

/* demo: a separate RAM-only history shown instead of the real one (never written to flash) */
static history_t s_demo;
static bool s_demo_alloc, s_demo_on;
static uint32_t s_demo_now;

bool clock_valid(void)
{
    time_t t = time(NULL);
    return t >= CLOCK_MIN_UNIX && t < CLOCK_MAX_UNIX;
}

static uint32_t uptime_min(void)
{
    return (uint32_t)(esp_timer_get_time() / 60000000);
}

/* ---------- flash ops on the raw partition ---------- */

static int p_read(void *c, uint32_t off, void *buf, uint32_t len)
{
    return esp_partition_read(s_part, off, buf, len) == ESP_OK ? 0 : -1;
}

static int p_write(void *c, uint32_t off, const void *buf, uint32_t len)
{
    return esp_partition_write(s_part, off, buf, len) == ESP_OK ? 0 : -1;
}

static int p_erase(void *c, uint32_t off, uint32_t len)
{
    return esp_partition_erase_range(s_part, off, len) == ESP_OK ? 0 : -1;
}

/* ---------- ---------- */

static uint32_t next_boot_id(void)
{
    nvs_handle_t nvs;
    uint32_t id = 0;
    if (nvs_open("datastore", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "boot_id", &id);
        id++;
        nvs_set_u32(nvs, "boot_id", id);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return id;
}

static void rebuild(void)
{
    hist_rebuild_t *rb = heap_caps_malloc(sizeof(*rb), MALLOC_CAP_SPIRAM);
    if (!rb) {
        ESP_LOGE(TAG, "rebuild: no memory");
        return;
    }
    uint32_t now = clock_valid() ? (uint32_t)(time(NULL) / 60) : 0;
    int64_t t0 = esp_timer_get_time();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    history_rebuild_begin(rb, &s_hist, now, uptime_min());
    if (s_log_ok) {
        histlog_foreach(&s_log, history_rebuild_feed, rb);
    }
    history_rebuild_next_pass(rb);
    if (s_log_ok) {
        histlog_foreach(&s_log, history_rebuild_feed, rb);
    }
    history_rebuild_end(rb);
    uint32_t n = history_count(&s_hist);
    xSemaphoreGive(s_mutex);

    heap_caps_free(rb);
    ESP_LOGI(TAG, "history rebuilt: %lu minutes, %s time, %lld ms", n, now ? "absolute" : "relative",
             (esp_timer_get_time() - t0) / 1000);
}

static void write_anchor(void)
{
    log_rec_t a = {.type = REC_TYPE_ANCHOR};
    a.m.boot_id = s_boot_id;
    a.m.uptime_min = uptime_min();
    a.m.unix_min = (uint32_t)(time(NULL) / 60);
    a.m.flags = s_sim ? REC_FLAG_SIM : 0;
    if (s_log_ok && histlog_append(&s_log, &a) != 0) {
        ESP_LOGE(TAG, "anchor write failed");
    }
}

static void task(void *arg)
{
    msg_t m;
    for (;;) {
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (m.type == MSG_MIN) {
                log_rec_t r = {.type = REC_TYPE_DATA, .m = m.min};
                if (s_log_ok && histlog_append(&s_log, &r) != 0) {
                    ESP_LOGE(TAG, "record write failed");
                }
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                history_add_minute(&s_hist, &m.min);
                xSemaphoreGive(s_mutex);
            } else if (m.type == MSG_10S) {
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                history_add_10s(&s_hist, &m.s10);
                xSemaphoreGive(s_mutex);
            } else {
                /* set by hand: the new anchor of this boot replaces any earlier one */
                struct timeval tv = {.tv_sec = m.unix_s};
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "clock set to %lld: anchoring boot %lu", (long long)m.unix_s, s_boot_id);
                write_anchor();
                rebuild();
                s_clock_was_valid = clock_valid();
                continue;
            }
        }

        bool valid = clock_valid();
        if (valid && !s_clock_was_valid) {
            ESP_LOGI(TAG, "clock became valid: anchoring boot %lu", s_boot_id);
            write_anchor();
            rebuild();
        }
        s_clock_was_valid = valid;
    }
}

esp_err_t datastore_init(bool sim)
{
    s_sim = sim;
    s_boot_id = next_boot_id();
    s_mutex = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    ESP_RETURN_ON_FALSE(s_mutex && s_queue, ESP_ERR_NO_MEM, TAG, "rtos");

    hist_entry_t *min_buf = heap_caps_calloc(HIST_MINUTES, sizeof(hist_entry_t), MALLOC_CAP_SPIRAM);
    sample10_t *s10_buf = heap_caps_calloc(HIST_10S, sizeof(sample10_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(min_buf && s10_buf, ESP_ERR_NO_MEM, TAG, "history buffers");
    history_init(&s_hist, min_buf, s10_buf, s_boot_id, sim ? REC_FLAG_SIM : 0);

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUBTYPE, PART_LABEL);
    if (s_part) {
        const flash_ops_t ops = {
            .read = p_read, .write = p_write, .erase = p_erase,
            .size = s_part->size, .sector = s_part->erase_size,
        };
        int64_t t0 = esp_timer_get_time();
        s_log_ok = histlog_open(&s_log, &ops) == 0;
        ESP_LOGI(TAG, "histlog: %lu/%lu records, next lid %lu, scan %lld ms", s_log.count,
                 histlog_capacity(&s_log), s_log.next_lid, (esp_timer_get_time() - t0) / 1000);
    } else {
        ESP_LOGE(TAG, "partition '%s' not found, history is RAM only", PART_LABEL);
    }

    /* The RTC keeps the system time across resets; anything implausible is discarded */
    if (time(NULL) >= CLOCK_MAX_UNIX) {
        ESP_LOGW(TAG, "discarding implausible system time %lld", (long long)time(NULL));
        struct timeval tv = {0};
        settimeofday(&tv, NULL);
    }
    s_clock_was_valid = clock_valid();
    if (s_clock_was_valid) {
        write_anchor();
    }
    rebuild();

    BaseType_t ok = xTaskCreatePinnedToCore(task, "datastore", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "boot %lu (%s)", s_boot_id, sim ? "sim" : "real");
    return ESP_OK;
}

uint32_t datastore_boot_id(void)
{
    return s_boot_id;
}

void datastore_push_minute(const minute_rec_t *r)
{
    msg_t m = {.type = MSG_MIN, .min = *r};
    if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, minute dropped");
    }
}

void datastore_push_10s(const sample10_t *s)
{
    msg_t m = {.type = MSG_10S, .s10 = *s};
    xQueueSend(s_queue, &m, 0);
}

void datastore_set_clock(int64_t unix_s)
{
    msg_t m = {.type = MSG_CLOCK, .unix_s = unix_s};
    xQueueSend(s_queue, &m, portMAX_DELAY);
}

uint32_t datastore_now_key(const history_t *h)
{
    if (h == &s_demo) {
        return s_demo_now;
    }
    return h->abs_time ? (uint32_t)(time(NULL) / 60) : h->rel_base + uptime_min();
}

const history_t *datastore_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    return s_demo_on ? &s_demo : &s_hist;
}

const history_t *datastore_lock_real(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    return &s_hist;
}

/* ---------- demo ---------- */

esp_err_t datastore_demo_begin(uint32_t now_key)
{
    if (!s_demo_alloc) {
        hist_entry_t *min_buf = heap_caps_calloc(HIST_MINUTES, sizeof(hist_entry_t), MALLOC_CAP_SPIRAM);
        sample10_t *s10_buf = heap_caps_calloc(HIST_10S, sizeof(sample10_t), MALLOC_CAP_SPIRAM);
        if (!min_buf || !s10_buf) {
            heap_caps_free(min_buf);
            heap_caps_free(s10_buf);
            return ESP_ERR_NO_MEM;
        }
        s_demo.min = min_buf;
        s_demo.s10 = s10_buf;
        s_demo_alloc = true;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    history_init(&s_demo, s_demo.min, s_demo.s10, 0, REC_FLAG_SIM);
    s_demo.abs_time = true;
    s_demo_now = now_key;
    s_demo_on = true;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void datastore_demo_put_minute(uint32_t key, const stat_t s[S_COUNT])
{
    minute_rec_t r = {.unix_min = key, .flags = REC_FLAG_SIM};
    memcpy(r.s, s, sizeof(r.s));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_demo_on) {
        history_add_minute(&s_demo, &r);
    }
    xSemaphoreGive(s_mutex);
}

void datastore_demo_push_10s(const sample10_t *s, uint32_t now_key)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_demo_on) {
        history_add_10s(&s_demo, s);
        s_demo_now = now_key;
    }
    xSemaphoreGive(s_mutex);
}

void datastore_demo_end(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_demo_on = false;
    xSemaphoreGive(s_mutex);
}

void datastore_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

void datastore_get_stats(datastore_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    st->boot_id = s_boot_id;
    st->sim = s_sim;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    st->abs_time = s_hist.abs_time;
    st->ram_minutes = history_count(&s_hist);
    st->flash_records = s_log.count;
    st->flash_capacity = s_log_ok ? histlog_capacity(&s_log) : 0;
    st->flash_writes = s_log.writes;
    st->flash_erases = s_log.erases;
    st->flash_errors = s_log.errors;
    xSemaphoreGive(s_mutex);
}

void datastore_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_log_ok) {
        histlog_clear(&s_log);
    }
    xSemaphoreGive(s_mutex);
    if (clock_valid()) {
        write_anchor();
    }
    rebuild();
    ESP_LOGW(TAG, "history cleared");
}
