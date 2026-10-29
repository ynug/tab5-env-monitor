/*
 * Tear-free landscape display for the Tab5 portrait MIPI-DSI panel.
 *
 * LVGL renders in PARTIAL mode into two 32-line strip buffers in internal SRAM (rendering into
 * PSRAM was ~30 % slower).
 * Each finished strip is rotated by the PPA (asynchronously) straight into the DPI frame buffer
 * that is not being scanned out, while the CPU renders the next strip. At the end of a frame the panel
 * switches to that buffer at its next frame start.
 *
 * The back buffer also lacks the previous frames' changes; LVGL is asked to render those areas
 * too (refr_start_cb). Only what is invalidated later than that is copied from the front buffer
 * (PPA, no rotation), skipping what this frame redraws anyway.
 * Three frame buffers rotate: the one written next left the screen two swaps ago, so the
 * renderer rarely waits. A buffer is only written once it has been off-screen for two vsyncs,
 * so the panel never shows a half-written frame.
 */
#include "display.h"

#include <string.h>
#include "board.h"
#include "bsp/m5stack_tab5.h"
#include "bsp/display.h"
#include "esp_lcd_touch.h"
#include "bsp/touch.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"

static const char *TAG = "display";

#define PANEL_W         BSP_LCD_H_RES   /* 720  */
#define PANEL_H         BSP_LCD_V_RES   /* 1280 */
#define BPP             2               /* RGB565 */
#define STRIP_LINES     32  /* keep a multiple of 8, see rounder_cb */
#define MAX_AREAS       LV_INV_BUF_SIZE
#define NUM_FB          3
#define LVGL_TASK_PRIO  5
#define LVGL_TASK_STACK (8 * 1024)
#define ST7121_LANE_MBPS 965
#define PERF_LOG        0   /* 1: log frame timing every 2 s */

typedef struct {
    lv_area_t a[MAX_AREAS];
    int n;
} area_list_t;

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_disp;
static ppa_client_handle_t s_ppa_async;     /* strip rotation */
static ppa_client_handle_t s_ppa_sync;      /* front -> back catch-up copies */
static SemaphoreHandle_t s_ppa_done;
static void *s_fb[NUM_FB];
static size_t s_fb_size;

static volatile uint32_t s_vsync;
static SemaphoreHandle_t s_vsync_sem;
static uint32_t s_free_at[NUM_FB];
static uint8_t s_front, s_back = 1;
static bool s_in_frame;

/* Areas changed per frame; a buffer written at frame f misses frames f+1 .. now-1 */
static uint32_t s_frame = 1;
static uint32_t s_fb_frame[NUM_FB];     /* 0 = never written */
static area_list_t s_hist[NUM_FB];      /* indexed by frame % NUM_FB */
static area_list_t *s_cur;
static bool s_flipped;
static int s_brightness = 80;
static bool s_backlight_on;
static display_touch_hook_t s_touch_hook;

#if PERF_LOG
static int64_t s_refr_t0, s_win_t0, s_max_refr;
static uint32_t s_frames, s_strips;
static int64_t s_wait_us, s_copy_us, s_fbwait_us;
static int64_t s_m_wait, s_m_copy, s_m_fbwait, s_max_big;
static int64_t s_big_sum, s_big_wait_sum;
static uint32_t s_big_n;
static uint32_t s_m_strips;
#define PT(x) int64_t x = esp_timer_get_time()
#else
#define PT(x)
#endif

static IRAM_ATTR bool on_vsync(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *ctx)
{
    s_vsync++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync_sem, &woken);
    return woken == pdTRUE;
}

static bool on_ppa_done(ppa_client_handle_t client, ppa_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_ppa_done, &woken);
    return woken == pdTRUE;
}

static void wait_fb_free(uint8_t i)
{
    while ((int32_t)(s_vsync - s_free_at[i]) < 0) {
        xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(40));
    }
}

/* Landscape area -> portrait rectangle (offset + size) for the current rotation */
static void map_area(const lv_area_t *a, uint32_t *ox, uint32_t *oy, uint32_t *w, uint32_t *h)
{
    if (!s_flipped) {
        /* 90 deg CCW: (x, y) -> (y, W-1-x) */
        *ox = a->y1;
        *oy = DISPLAY_W - 1 - a->x2;
    } else {
        /* 270 deg CCW: (x, y) -> (H-1-y, x) */
        *ox = DISPLAY_H - 1 - a->y2;
        *oy = a->x1;
    }
    *w = lv_area_get_height(a);
    *h = lv_area_get_width(a);
}

static void rotate_strip_async(const lv_area_t *a, const void *px)
{
    uint32_t ox, oy, w, h;
    map_area(a, &ox, &oy, &w, &h);
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = px,
            .pic_w = lv_area_get_width(a),
            .pic_h = lv_area_get_height(a),
            .block_w = lv_area_get_width(a),
            .block_h = lv_area_get_height(a),
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_fb[s_back],
            .buffer_size = s_fb_size,
            .pic_w = PANEL_W,
            .pic_h = PANEL_H,
            .block_offset_x = ox,
            .block_offset_y = oy,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = s_flipped ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(s_ppa_async, &op));
}

static void copy_front_to_back(const lv_area_t *a)
{
    uint32_t ox, oy, w, h;
    map_area(a, &ox, &oy, &w, &h);
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = s_fb[s_front],
            .pic_w = PANEL_W,
            .pic_h = PANEL_H,
            .block_w = w,
            .block_h = h,
            .block_offset_x = ox,
            .block_offset_y = oy,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_fb[s_back],
            .buffer_size = s_fb_size,
            .pic_w = PANEL_W,
            .pic_h = PANEL_H,
            .block_offset_x = ox,
            .block_offset_y = oy,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(s_ppa_sync, &op));
}

/* A stuck PPA transfer must not freeze the UI: give up after a while and count it */
#define PPA_TIMEOUT_MS 200
static uint32_t s_ppa_timeouts;

static void wait_ppa(const char *where)
{
    if (xSemaphoreTake(s_ppa_done, pdMS_TO_TICKS(PPA_TIMEOUT_MS)) != pdTRUE) {
        s_ppa_timeouts++;
        ESP_LOGE(TAG, "PPA timeout (%s), %lu so far", where, s_ppa_timeouts);
    }
}

static bool area_inside(const lv_area_t *in, const lv_area_t *out)
{
    return in->x1 >= out->x1 && in->y1 >= out->y1 && in->x2 <= out->x2 && in->y2 <= out->y2;
}

/* First strip of a frame: record this frame's invalidated areas and bring the back buffer up to date */
static void frame_begin(lv_display_t *disp)
{
    s_cur = &s_hist[s_frame % NUM_FB];
    s_cur->n = 0;
    for (uint32_t i = 0; i < disp->inv_p && s_cur->n < MAX_AREAS; i++) {
        if (!disp->inv_area_joined[i]) {
            s_cur->a[s_cur->n++] = disp->inv_areas[i];
        }
    }

    PT(a0);
    wait_fb_free(s_back);
    PT(a1);

    const uint32_t last = s_fb_frame[s_back];
    /* the history ring holds NUM_FB frames: this one plus up to NUM_FB - 1 missed ones */
    if (last == 0 || s_frame - last > NUM_FB) {
        const lv_area_t all = {0, 0, DISPLAY_W - 1, DISPLAY_H - 1};
        copy_front_to_back(&all);
    } else {
        /* Copy each missed area unless a later frame (missed or this one) changed it again */
        for (uint32_t f = last + 1; f < s_frame; f++) {
            const area_list_t *missed = &s_hist[f % NUM_FB];
            for (int k = 0; k < missed->n; k++) {
                bool covered = false;
                for (uint32_t g = f + 1; g <= s_frame && !covered; g++) {
                    const area_list_t *later = &s_hist[g % NUM_FB];
                    for (int j = 0; j < later->n && !covered; j++) {
                        covered = area_inside(&missed->a[k], &later->a[j]);
                    }
                }
                if (!covered) {
                    copy_front_to_back(&missed->a[k]);
                }
            }
        }
    }
#if PERF_LOG
    PT(a2);
    s_fbwait_us += a1 - a0;
    s_copy_us += a2 - a1;
#endif
}

/*
 * Before rendering: the back buffer misses what the last NUM_FB - 1 frames changed. Have LVGL
 * render those areas as part of this frame instead of copying them from the front buffer in
 * frame_begin(): a full-screen PSRAM to PSRAM copy takes ~45 ms, rendering it ~30 ms.
 */
static void refr_start_cb(lv_event_t *e)
{
    lv_display_t *disp = lv_event_get_target(e);
    if (disp->inv_p == 0) {
        return;     /* nothing to draw this time */
    }
    const uint32_t last = s_fb_frame[s_back];
    if (last == 0 || s_frame - last > NUM_FB) {
        const lv_area_t all = {0, 0, DISPLAY_W - 1, DISPLAY_H - 1};
        lv_inv_area(disp, &all);
        return;
    }
    for (uint32_t f = last + 1; f < s_frame; f++) {
        const area_list_t *missed = &s_hist[f % NUM_FB];
        for (int k = 0; k < missed->n; k++) {
            lv_inv_area(disp, &missed->a[k]);
        }
    }
}

static void frame_end(void)
{
    /* Pointer inside a DPI frame buffer -> the driver switches buffers, no copy. One line is
     * passed to keep its cache write-back trivial (the PPA wrote via DMA). */
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PANEL_W, 1, s_fb[s_back]);
    s_fb_frame[s_back] = s_frame++;
    s_free_at[s_front] = s_vsync + 2;
    s_front = s_back;
    s_back = (s_back + 1) % NUM_FB;

    if (!s_backlight_on) {
        s_backlight_on = true;
        bsp_display_brightness_set(s_brightness);
    }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (!s_in_frame) {
        s_in_frame = true;
        frame_begin(disp);
    }
    rotate_strip_async(area, px_map);
#if PERF_LOG
    s_strips++;
#endif

    if (lv_display_flush_is_last(disp)) {
        PT(w0);
        wait_ppa("last strip");
#if PERF_LOG
        s_wait_us += esp_timer_get_time() - w0;
#endif
        s_in_frame = false;
        frame_end();
        lv_display_flush_ready(disp);
    }
    /* otherwise LVGL renders the next strip and waits in flush_wait_cb */
}

/*
 * The PPA SRM sometimes never completes a rotated transfer of a narrow area (seen with 608x66
 * strips of a pressed card; full-width strips never failed, also under scroll load). Widen
 * every invalidated area to the full width; LVGL also applies this when it sizes the strips,
 * so they are always 1280 px wide. Rotated widths (strip heights) of 24 and 66 px also hung, while
 * 10/16/20/32/42/74 did not; areas are therefore aligned to 8 lines and strips are 16 lines.
 */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 = 0;
    a->y1 &= ~7;
    a->x2 = DISPLAY_W - 1;
    a->y2 |= 7;
}

static void flush_wait_cb(lv_display_t *disp)
{
    PT(w0);
    wait_ppa("strip");
#if PERF_LOG
    s_wait_us += esp_timer_get_time() - w0;
#endif
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int32_t last_x, last_y;
    static bool was_pressed, swallow;
    esp_lcd_touch_point_data_t p;
    uint8_t n = 0;

    esp_lcd_touch_read_data(s_touch);
    bool pressed = esp_lcd_touch_get_data(s_touch, &p, &n, 1) == ESP_OK && n > 0;
    if (pressed && !was_pressed) {
        /* the hook may claim the whole press (e.g. waking a dark screen) */
        swallow = display_touch_began();
    }
    was_pressed = pressed;

    if (pressed && !swallow) {
        if (!s_flipped) {
            last_x = DISPLAY_W - 1 - p.y;
            last_y = p.x;
        } else {
            last_x = p.y;
            last_y = DISPLAY_H - 1 - p.x;
        }
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

#if PERF_LOG
static void refr_event_cb(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s_refr_t0 = now;
        s_wait_us = s_copy_us = s_fbwait_us = 0;
        s_strips = 0;
        return;
    }
    if (s_strips > 5) {
        s_big_sum += now - s_refr_t0;
        s_big_wait_sum += s_wait_us;
        s_big_n++;
        if (now - s_refr_t0 > s_max_big) {
            s_max_big = now - s_refr_t0;
        }
    }
    if (now - s_refr_t0 > s_max_refr) {
        s_max_refr = now - s_refr_t0;
        s_m_wait = s_wait_us; s_m_copy = s_copy_us; s_m_fbwait = s_fbwait_us; s_m_strips = s_strips;
    }
    s_frames++;
    if (now - s_win_t0 > 2000000) {
        if (s_frames > 3) {
            ESP_LOGI(TAG, "PERF %lu frames / %lld ms, slowest %lld ms (ppa wait %lld, catch-up copy %lld, fb wait %lld, %lu strips), slowest full redraw %lld ms",
                     s_frames, (now - s_win_t0) / 1000, s_max_refr / 1000, s_m_wait / 1000, s_m_copy / 1000,
                     s_m_fbwait / 1000, s_m_strips, s_max_big / 1000);
            if (s_big_n) {
                ESP_LOGI(TAG, "PERF full redraws: %lu, avg %.1f ms (ppa wait %.1f), max %lld ms", s_big_n,
                         s_big_sum / 1000.0 / s_big_n, s_big_wait_sum / 1000.0 / s_big_n, s_max_big / 1000);
            }
            s_max_big = 0;
            s_big_sum = s_big_wait_sum = 0;
            s_big_n = 0;
        }
        s_win_t0 = now;
        s_frames = 0;
        s_max_refr = 0;
    }
}
#endif

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t ms = lv_timer_handler();
        if (ms == LV_NO_TIMER_READY || ms > 20) {
            ms = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
    }
}

static esp_err_t panel_init(void)
{
    bsp_display_config_t cfg = {
        .dsi_bus = {
            .phy_clk_src = 0,
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };
    if (board_lcd_type() == BOARD_LCD_ST7121) {
        cfg.dsi_bus.lane_bit_rate_mbps = ST7121_LANE_MBPS;
    }
    bsp_lcd_handles_t h;
    ESP_RETURN_ON_ERROR(bsp_display_new_with_handles(&cfg, &h), TAG, "panel");
    s_panel = h.panel;

    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, NUM_FB, &s_fb[0], &s_fb[1], &s_fb[2]), TAG, "fb");
    s_fb_size = PANEL_W * PANEL_H * BPP;

    s_vsync_sem = xSemaphoreCreateBinary();
    s_ppa_done = xSemaphoreCreateCounting(4, 0);
    ESP_RETURN_ON_FALSE(s_vsync_sem && s_ppa_done, ESP_ERR_NO_MEM, TAG, "sem");
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_refresh_done = on_vsync};
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL), TAG, "cb");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "on");

    const ppa_client_config_t async_cfg = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 2};
    ESP_RETURN_ON_ERROR(ppa_register_client(&async_cfg, &s_ppa_async), TAG, "ppa");
    const ppa_event_callbacks_t ppa_cbs = {.on_trans_done = on_ppa_done};
    ESP_RETURN_ON_ERROR(ppa_client_register_event_callbacks(s_ppa_async, &ppa_cbs), TAG, "ppa cb");
    const ppa_client_config_t sync_cfg = {.oper_type = PPA_OPERATION_SRM};
    ESP_RETURN_ON_ERROR(ppa_register_client(&sync_cfg, &s_ppa_sync), TAG, "ppa");
    return ESP_OK;
}

esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel init");
    ESP_RETURN_ON_ERROR(bsp_touch_new(NULL, &s_touch), TAG, "touch");

    lv_init();
    lv_tick_set_cb(tick_cb);

    /* Strips in internal SRAM: rendering into PSRAM is much slower */
    ESP_LOGI(TAG, "internal RAM free %u KB before strips", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    const size_t strip_size = DISPLAY_W * STRIP_LINES * BPP;
    void *buf1 = heap_caps_aligned_calloc(LV_DRAW_BUF_ALIGN, 1, strip_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void *buf2 = heap_caps_aligned_calloc(LV_DRAW_BUF_ALIGN, 1, strip_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "strip buffers");

    s_disp = lv_display_create(DISPLAY_W, DISPLAY_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, buf1, buf2, strip_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(s_disp, refr_start_cb, LV_EVENT_REFR_START, NULL);
#if PERF_LOG
    lv_display_add_event_cb(s_disp, refr_event_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(s_disp, refr_event_cb, LV_EVENT_REFR_READY, NULL);
#endif

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    BaseType_t ok = xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "lvgl task");

    ESP_LOGI(TAG, "ready: %dx%d landscape on %s, internal RAM free %u KB", DISPLAY_W, DISPLAY_H, board_lcd_name(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return ESP_OK;
}

void display_set_brightness(int percent)
{
    s_brightness = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    if (s_backlight_on) {
        bsp_display_brightness_set(s_brightness);
    }
}

int display_get_brightness(void)
{
    return s_brightness;
}

void display_set_flipped(bool flipped)
{
    lv_lock();
    if (flipped != s_flipped) {
        s_flipped = flipped;
        /* Redraw everything; following frames copy it into the other buffers */
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
    }
    lv_unlock();
}

bool display_is_flipped(void)
{
    return s_flipped;
}

bool display_touch_began(void)
{
    return s_touch_hook && s_touch_hook();
}

void display_set_touch_hook(display_touch_hook_t hook)
{
    s_touch_hook = hook;
}
