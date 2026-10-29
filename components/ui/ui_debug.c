/*
 * Development console on stdin: screenshots and synthetic touch input, so the UI can be
 * checked from the host (tools/ui_remote.py) without looking at the device.
 */
#include "sdkconfig.h"
#if CONFIG_UI_DEBUG_CONSOLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <math.h>
#include "audio.h"
#include "esp_log.h"
#include "display.h"
#include "sensor_hub.h"
#include "board.h"
#include "datastore.h"
#include "sdlog.h"
#include "lvgl.h"
#include "mbedtls/base64.h"
#include "ui_debug.h"

#define LINE_MAX_LEN 64

typedef struct {
    int32_t x1, y1, x2, y2;
    uint32_t t0, dur;
    bool active;
    bool release_next;
    bool hold;
    bool swallow;
    bool started;
} gesture_t;

static gesture_t s_g;
static QueueHandle_t s_lines;

/* One line per 57 bytes: "S<seq>:<base64>*<sum>"; the host checks seq and sum and retries if needed */
static uint32_t s_seq;

static void emit_b64(const uint8_t *data, size_t len)
{
    char line[8 + 4 * 57 / 3 + 8];
    for (size_t off = 0; off < len; off += 57) {
        size_t n = len - off < 57 ? len - off : 57;
        int h = snprintf(line, sizeof(line), "S%04lx:", (unsigned long)(s_seq++ & 0xFFFF));
        size_t olen;
        mbedtls_base64_encode((unsigned char *)line + h, sizeof(line) - h - 5, &olen, data + off, n);
        uint8_t sum = 0;
        for (size_t i = 0; i < olen; i++) sum += (uint8_t)line[h + i];
        snprintf(line + h + olen, 5, "*%02x\n", sum);
        /* straight to the driver, waiting for space: stdout drops data when the TX buffer is full */
        usb_serial_jtag_write_bytes(line, h + olen + 4, portMAX_DELAY);
    }
}

static void screenshot(void)
{
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!snap) {
        printf("SHOT_ERR\n");
        return;
    }
    /* dialogs live on the top layer: blend it over the screen */
    if (lv_obj_get_child_count(lv_layer_top()) > 0) {
        lv_draw_buf_t *top = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
        if (top) {
            for (uint32_t y = 0; y < snap->header.h && y < top->header.h; y++) {
                uint16_t *d = (uint16_t *)(snap->data + y * snap->header.stride);
                const uint8_t *t = top->data + y * top->header.stride;
                for (uint32_t x = 0; x < snap->header.w && x < top->header.w; x++, t += 4) {
                    uint32_t a = t[3];
                    if (!a) continue;
                    uint32_t r = (d[x] >> 11) << 3, g = ((d[x] >> 5) & 0x3F) << 2, b = (d[x] & 0x1F) << 3;
                    r = (t[2] * a + r * (255 - a)) / 255;
                    g = (t[1] * a + g * (255 - a)) / 255;
                    b = (t[0] * a + b * (255 - a)) / 255;
                    d[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
            }
            lv_draw_buf_destroy(top);
        }
    }
    const uint32_t w = snap->header.w, h = snap->header.h, stride = snap->header.stride;
    esp_log_level_set("*", ESP_LOG_NONE);   /* fewer interleaved logs */
    s_seq = 0;
    fflush(stdout);
    char hdr[48];
    int hl = snprintf(hdr, sizeof(hdr), "\nSHOT_BEGIN %lu %lu\n", w, h);
    usb_serial_jtag_write_bytes(hdr, hl, portMAX_DELAY);

    /* RLE of RGB565 pixels: (count u16 LE, pixel u16 LE), streamed in chunks */
    static uint8_t chunk[57 * 64];
    size_t used = 0;
    uint16_t run_px = 0;
    uint32_t run = 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)(snap->data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint16_t px = row[x];
            if (run && (px != run_px || run == 0xFFFF)) {
                chunk[used++] = run & 0xFF; chunk[used++] = run >> 8;
                chunk[used++] = run_px & 0xFF; chunk[used++] = run_px >> 8;
                run = 0;
                if (used + 4 > sizeof(chunk)) {
                    emit_b64(chunk, used);
                    used = 0;
                }
            }
            run_px = px;
            run++;
        }
    }
    chunk[used++] = run & 0xFF; chunk[used++] = run >> 8;
    chunk[used++] = run_px & 0xFF; chunk[used++] = run_px >> 8;
    emit_b64(chunk, used);
    usb_serial_jtag_write_bytes("SHOT_END\n", 9, portMAX_DELAY);
    esp_log_level_set("*", ESP_LOG_INFO);
    lv_draw_buf_destroy(snap);
}

static void command(char *line)
{
    int a, b, c, d, e;
    if (strcmp(line, "ping") == 0) {
        printf("pong\n");
    } else if (strcmp(line, "shot") == 0) {
        screenshot();
    } else if (sscanf(line, "tap %d %d", &a, &b) == 2) {
        s_g = (gesture_t){.x1 = a, .y1 = b, .x2 = a, .y2 = b, .t0 = lv_tick_get(), .dur = 80, .active = true};
    } else if (sscanf(line, "drag %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) {
        s_g = (gesture_t){.x1 = a, .y1 = b, .x2 = c, .y2 = d, .t0 = lv_tick_get(), .dur = e, .active = true};
    } else if (strncmp(line, "clock ", 6) == 0) {
        /* clock UNIX_SECONDS: same as the settings page, for setting the time from the host */
        long long t = strtoll(line + 6, NULL, 10);
        bool rtc = board_rtc_write(t);
        datastore_set_clock(t);
        printf("clock set to %lld (rtc %s)\n", t, rtc ? "ok" : "failed");
    } else if (sscanf(line, "demo %d %d", &a, &b) == 2) {
        sensor_hub_demo_start((demo_scn_t)a, b);
        printf("demo %d x%d\n", a, b);
    } else if (strcmp(line, "demo stop") == 0) {
        sensor_hub_demo_stop();
        printf("demo stop\n");
    } else if (strcmp(line, "sd export") == 0) {
        sdlog_request_export();
        printf("sd export requested\n");
    } else if (sscanf(line, "sound %d", &a) == 1) {
        audio_play((sound_t)a);
    } else if (strncmp(line, "sim ", 4) == 0) {
        /* sim co2|temp|hum|pres VALUE|off */
        static const char *names[M_COUNT] = {"co2", "temp", "hum", "pres"};
        char name[8], val[16];
        if (sscanf(line + 4, "%7s %15s", name, val) == 2) {
            for (int m = 0; m < M_COUNT; m++) {
                if (strcmp(name, names[m]) == 0) {
                    sensor_hub_sim_override((metric_id_t)m, strcmp(val, "off") == 0 ? NAN : strtof(val, NULL));
                    printf("sim %s = %s\n", name, val);
                }
            }
        }
    } else if (sscanf(line, "press %d %d", &a, &b) == 2) {
        s_g = (gesture_t){.x1 = a, .y1 = b, .x2 = a, .y2 = b, .t0 = lv_tick_get(), .dur = 0, .active = true, .hold = true};
    } else if (strcmp(line, "release") == 0) {
        s_g.hold = false;
    } else if (line[0]) {
        printf("UI_DEBUG unknown: %s\n", line);
    }
}

/* Reads the USB Serial/JTAG port and hands complete lines to the LVGL thread */
static void reader_task(void *arg)
{
    char line[LINE_MAX_LEN];
    int len = 0;
    uint8_t buf[64];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (len) {
                    line[len] = 0;
                    xQueueSend(s_lines, line, 0);
                    len = 0;
                }
            } else if (len < LINE_MAX_LEN - 1) {
                line[len++] = (char)buf[i];
            }
        }
    }
}

static void poll_cb(lv_timer_t *t)
{
    char line[LINE_MAX_LEN];
    while (xQueueReceive(s_lines, line, 0) == pdTRUE) {
        command(line);
    }
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (!s_g.active) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (!s_g.started) {
        s_g.started = true;
        s_g.swallow = display_touch_began();
    }
    if (s_g.swallow) {
        data->state = LV_INDEV_STATE_RELEASED;
        if (lv_tick_elaps(s_g.t0) >= s_g.dur && !s_g.hold) s_g.active = false;
        return;
    }
    uint32_t el = lv_tick_elaps(s_g.t0);
    if (el >= s_g.dur && s_g.hold) {
        data->point.x = s_g.x2;
        data->point.y = s_g.y2;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }
    if (el >= s_g.dur) {
        data->point.x = s_g.x2;
        data->point.y = s_g.y2;
        /* one more pressed sample at the end point, then release */
        data->state = s_g.release_next ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
        if (s_g.release_next) {
            s_g.active = false;
            s_g.release_next = false;
        } else {
            s_g.release_next = true;
        }
        return;
    }
    data->point.x = s_g.x1 + (s_g.x2 - s_g.x1) * (int32_t)el / (int32_t)s_g.dur;
    data->point.y = s_g.y1 + (s_g.y2 - s_g.y1) * (int32_t)el / (int32_t)s_g.dur;
    data->state = LV_INDEV_STATE_PRESSED;
}

void ui_debug_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    s_lines = xQueueCreate(4, LINE_MAX_LEN);
    xTaskCreate(reader_task, "ui_debug", 3072, NULL, 2, NULL);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    lv_timer_create(poll_cb, 50, NULL);
}

#else
void ui_debug_init(void) {}
#endif
