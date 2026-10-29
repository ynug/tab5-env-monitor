/*
 * Graph screen: one metric at a time, the last 12 h of 1-minute data.
 *
 * The plot is rendered into a canvas (PSRAM) only when data or series selection
 * change; normal refreshes just blit it. The crosshair and tooltip are small objects on top,
 * so dragging only redraws around them.
 */
#include "pages.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "datastore.h"
#include "ui_theme.h"

#define MAX_SER     4
#define MAX_BUCKETS 600

#define CANVAS_X    UI_PAD
#define CANVAS_Y    170
#define CANVAS_W    (1280 - UI_PAD * 2)
#define CANVAS_H    (720 - CANVAS_Y - 12)
#define PLOT_L      76
#define PLOT_R      24
#define PLOT_T      16
#define PLOT_B      36
#define PLOT_W      (CANVAS_W - PLOT_L - PLOT_R)
#define PLOT_H      (CANVAS_H - PLOT_T - PLOT_B)
#define REFRESH_MS  2000
#define BTN_H       56
#define TAB_MIN_W   150

typedef struct {
    int src;
    const char *label;
    bool on;
} gser_def_t;

typedef struct {
    float v;
    const char *label;
} gthr_t;

typedef struct {
    const char *tab;
    const char *title;
    const char *unit;
    const char *fmt;
    float min_span;
    int nser;
    gser_def_t ser[MAX_SER];
    int nthr;
    gthr_t thr[3];
} graph_def_t;

static const graph_def_t GRAPHS[UI_GRAPH_COUNT] = {
    [UI_GRAPH_CO2] = {"CO2", "CO2", "ppm", "%.0f", 100.0f, 1, {{S_CO2, "SCD41", true}},
                      2, {{1000, "注意 1000"}, {1500, "警告 1500"}}},
    [UI_GRAPH_TEMP] = {"温度", "温度", "℃", "%.1f", 1.0f, 4,
                       {{S_T_SHT, "SHT35", true}, {S_T_SCD, "SCD41（補正）", true}, {S_T_BMP, "BMP581", true},
                        {S_T_SCD_RAW, "SCD41（生値）", false}},
                       1, {{16, "低温 16"}}},
    [UI_GRAPH_HUM] = {"湿度", "湿度", "%", "%.1f", 5.0f, 3,
                      {{S_RH_SHT, "SHT35", true}, {S_RH_SCD, "SCD41（補正）", true}, {S_RH_SCD_RAW, "SCD41（生値）", false}},
                      2, {{35, "乾燥 35"}, {70, "多湿 70"}}},
    [UI_GRAPH_PRES] = {"気圧", "気圧（現地）", "hPa", "%.1f", 2.0f, 1, {{S_P_BMP, "BMP581", true}}, 0, {{0}}},
};

/* Categorical slots (dark), validated against the card surface */
static const uint32_t SER_COLOR[MAX_SER] = {0x3987E5, 0xD95926, 0x199E70, 0xC98500};

#define RANGE_MIN   720u
#define TICK_MIN    120u

typedef struct {
    float avg[MAX_BUCKETS], lo[MAX_BUCKETS], hi[MAX_BUCKETS];
    bool ok[MAX_BUCKETS];
} bucket_series_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_tabs[UI_GRAPH_COUNT];
static lv_obj_t *s_legend;
static lv_obj_t *s_chips[MAX_SER];
static lv_obj_t *s_stats;
static lv_obj_t *s_canvas;
static lv_draw_buf_t *s_draw_buf;
static lv_obj_t *s_cross;
static lv_obj_t *s_tip, *s_tip_time, *s_tip_row[MAX_SER], *s_tip_dot[MAX_SER], *s_tip_val[MAX_SER];

static ui_graph_t s_graph;
static bool s_on[UI_GRAPH_COUNT][MAX_SER];
static lv_obj_t *s_back_to;

/* computed data */
static bucket_series_t s_b[MAX_SER];
static int s_nb;
static uint32_t s_start_key, s_now_key, s_drawn_key, s_drawn_latest;
static bool s_abs;
static float s_ymin, s_ymax;

/* ---------- data ---------- */

static bool entry_value(const hist_entry_t *e, int src, float *v, float *lo, float *hi)
{
    const stat_t *st = &e->s[src];
    if (!st->n) return false;
    *v = st->avg;
    *lo = st->min;
    *hi = st->max;
    return true;
}

static void compute(void)
{
    const graph_def_t *g = &GRAPHS[s_graph];
    const uint32_t r = RANGE_MIN;
    s_nb = r < (uint32_t)(PLOT_W / 2) ? (int)r : PLOT_W / 2;
    if (s_nb > MAX_BUCKETS) s_nb = MAX_BUCKETS;

    const history_t *h = datastore_lock();
    s_abs = h->abs_time;
    s_now_key = datastore_now_key(h);
    s_drawn_latest = h->have ? h->latest_key : 0;
    s_start_key = s_now_key - r + 1;

    for (int k = 0; k < g->nser; k++) {
        bucket_series_t *b = &s_b[k];
        for (int i = 0; i < s_nb; i++) {
            uint32_t m0 = s_start_key + (uint32_t)((uint64_t)i * r / s_nb);
            uint32_t m1 = s_start_key + (uint32_t)((uint64_t)(i + 1) * r / s_nb);
            double sum = 0;
            int n = 0;
            float lo = INFINITY, hi = -INFINITY;
            for (uint32_t m = m0; m < m1; m++) {
                const hist_entry_t *e = history_get(h, m);
                float v, l, u;
                if (e && entry_value(e, g->ser[k].src, &v, &l, &u)) {
                    sum += v;
                    n++;
                    if (l < lo) lo = l;
                    if (u > hi) hi = u;
                }
            }
            b->ok[i] = n > 0;
            if (n) {
                b->avg[i] = (float)(sum / n);
                b->lo[i] = lo;
                b->hi[i] = hi;
            }
        }
    }
    datastore_unlock();

    /* y range over the visible series (band only for the primary one) */
    float ymin = INFINITY, ymax = -INFINITY;
    for (int k = 0; k < g->nser; k++) {
        if (!s_on[s_graph][k]) continue;
        for (int i = 0; i < s_nb; i++) {
            if (!s_b[k].ok[i]) continue;
            float lo = k == 0 ? s_b[k].lo[i] : s_b[k].avg[i];
            float hi = k == 0 ? s_b[k].hi[i] : s_b[k].avg[i];
            if (lo < ymin) ymin = lo;
            if (hi > ymax) ymax = hi;
        }
    }
    if (!isfinite(ymin)) {
        ymin = 0;
        ymax = 1;
    }
    float span = ymax - ymin;
    if (span < g->min_span) {
        float mid = 0.5f * (ymin + ymax);
        ymin = mid - g->min_span / 2;
        ymax = mid + g->min_span / 2;
        span = g->min_span;
    }
    s_ymin = ymin - span * 0.08f;
    s_ymax = ymax + span * 0.08f;
}

/* ---------- drawing ---------- */

static float nice_step(float span)
{
    float raw = span / 5.0f;
    float p = powf(10.0f, floorf(log10f(raw)));
    float f = raw / p;
    return (f < 1.5f ? 1 : f < 3.5f ? 2 : f < 7.5f ? 5 : 10) * p;
}

static int32_t y_px(float v)
{
    return PLOT_T + (int32_t)lroundf((s_ymax - v) / (s_ymax - s_ymin) * PLOT_H);
}

static int32_t x_px(int i)
{
    return PLOT_L + (int32_t)lroundf((i + 0.5f) * PLOT_W / s_nb);
}

static void draw_text(lv_layer_t *layer, const char *txt, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      lv_text_align_t align, lv_color_t color)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = &font_jp_16;
    d.color = color;
    d.align = align;
    d.text = txt;
    d.text_local = 1;
    const lv_area_t a = {x1, y1, x2, y2};
    lv_draw_label(layer, &d, &a);
}

static void draw_line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, int w,
                      lv_opa_t opa, int dash)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.color = c;
    d.width = w;
    d.opa = opa;
    d.round_start = d.round_end = w > 1;
    if (dash) {
        d.dash_width = dash;
        d.dash_gap = dash;
    }
    lv_draw_line(layer, &d);
}

static void format_tick(char *buf, size_t n, uint32_t key)
{
    if (s_abs) {
        time_t t = (time_t)key * 60;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(buf, n, "%H:%M", &tm);
    } else {
        uint32_t ago = s_now_key - key;
        if (ago == 0) snprintf(buf, n, "現在");
        else if (ago % 60 == 0) snprintf(buf, n, "-%lu時間", ago / 60);
        else snprintf(buf, n, "-%lu分", ago);
    }
}

/* Tooltip time: clock time, or "N時間M分前" while the clock is not synced */
static void format_point(char *buf, size_t n, uint32_t key)
{
    if (s_abs) {
        format_tick(buf, n, key);
        return;
    }
    uint32_t ago = s_now_key - key;
    if (ago == 0) snprintf(buf, n, "現在");
    else if (ago < 60) snprintf(buf, n, "%lu分前", ago);
    else if (ago % 60 == 0) snprintf(buf, n, "%lu時間前", ago / 60);
    else snprintf(buf, n, "%lu時間%lu分前", ago / 60, ago % 60);
}

static void render(void)
{
    const graph_def_t *g = &GRAPHS[s_graph];
    lv_layer_t layer;
    lv_canvas_fill_bg(s_canvas, UI_COLOR_CARD, LV_OPA_COVER);
    lv_canvas_init_layer(s_canvas, &layer);
    char buf[32];

    /* horizontal grid + y labels */
    float step = nice_step(s_ymax - s_ymin);
    for (float v = ceilf(s_ymin / step) * step; v <= s_ymax; v += step) {
        int32_t y = y_px(v);
        draw_line(&layer, PLOT_L, y, PLOT_L + PLOT_W, y, UI_COLOR_BORDER, 1, LV_OPA_COVER, 0);
        if (g->unit[0] && y < 28) {
            continue;   /* the unit is shown there */
        }
        snprintf(buf, sizeof(buf), step < 1.0f ? "%.1f" : "%.0f", v);
        draw_text(&layer, buf, 0, y - 10, PLOT_L - 10, y + 10, LV_TEXT_ALIGN_RIGHT, UI_COLOR_SUBTEXT);
    }

    if (g->unit[0]) {
        draw_text(&layer, g->unit, 0, 0, PLOT_L - 10, 18, LV_TEXT_ALIGN_RIGHT, UI_COLOR_SUBTEXT);
    }

    /* vertical grid + time labels, aligned to local time when the clock is valid */
    const uint32_t r = RANGE_MIN, tick = TICK_MIN;
    const uint32_t tz = s_abs ? 9 * 60 : 0;     /* JST */
    uint32_t first = s_abs ? ((s_start_key + tz + tick - 1) / tick) * tick - tz
                           : s_now_key - ((s_now_key - s_start_key) / tick) * tick;
    for (uint32_t k = first; k <= s_now_key; k += tick) {
        int32_t x = PLOT_L + (int32_t)((uint64_t)(k - s_start_key) * PLOT_W / r);
        draw_line(&layer, x, PLOT_T, x, PLOT_T + PLOT_H, UI_COLOR_BORDER, 1, LV_OPA_COVER, 0);
        format_tick(buf, sizeof(buf), k);
        draw_text(&layer, buf, x - 50, PLOT_T + PLOT_H + 8, x + 50, CANVAS_H, LV_TEXT_ALIGN_CENTER, UI_COLOR_SUBTEXT);
    }

    /* thresholds */
    for (int t = 0; t < g->nthr; t++) {
        float v = g->thr[t].v;
        if (v <= s_ymin || v >= s_ymax) continue;
        int32_t y = y_px(v);
        draw_line(&layer, PLOT_L, y, PLOT_L + PLOT_W, y, UI_COLOR_SUBTEXT, 1, LV_OPA_70, 6);
        draw_text(&layer, g->thr[t].label, PLOT_L + PLOT_W - 300, y - 22, PLOT_L + PLOT_W - 4, y - 2,
                  LV_TEXT_ALIGN_RIGHT, UI_COLOR_SUBTEXT);
    }

    /* min-max band of the primary series */
    bool any = false;
    if (s_on[s_graph][0]) {
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = lv_color_hex(SER_COLOR[0]);
        rd.bg_opa = LV_OPA_30;
        for (int i = 0; i < s_nb; i++) {
            if (!s_b[0].ok[i]) continue;
            int32_t xa = PLOT_L + (int32_t)((int64_t)i * PLOT_W / s_nb);
            int32_t xb = PLOT_L + (int32_t)((int64_t)(i + 1) * PLOT_W / s_nb) - 1;
            int32_t ya = y_px(s_b[0].hi[i]), yb = y_px(s_b[0].lo[i]);
            if (yb - ya < 1) yb = ya + 1;
            const lv_area_t a = {xa, ya, xb < xa ? xa : xb, yb};
            lv_draw_rect(&layer, &rd, &a);
        }
    }

    /* lines: secondary series first, primary on top; gaps stay open */
    for (int k = g->nser - 1; k >= 0; k--) {
        if (!s_on[s_graph][k]) continue;
        const bucket_series_t *b = &s_b[k];
        lv_color_t c = lv_color_hex(SER_COLOR[k]);
        for (int i = 0; i < s_nb; i++) {
            if (!b->ok[i]) continue;
            any = true;
            bool next = i + 1 < s_nb && b->ok[i + 1];
            bool prev = i > 0 && b->ok[i - 1];
            if (next) {
                draw_line(&layer, x_px(i), y_px(b->avg[i]), x_px(i + 1), y_px(b->avg[i + 1]), c, 2, LV_OPA_COVER, 0);
            } else if (!prev) {
                /* isolated point (a zero-length line draws nothing) */
                lv_draw_rect_dsc_t dot;
                lv_draw_rect_dsc_init(&dot);
                dot.bg_color = c;
                dot.radius = LV_RADIUS_CIRCLE;
                const lv_area_t a = {x_px(i) - 3, y_px(b->avg[i]) - 3, x_px(i) + 3, y_px(b->avg[i]) + 3};
                lv_draw_rect(&layer, &dot, &a);
            }
        }
    }

    if (!any) {
        draw_text(&layer, "この期間のデータはまだありません", PLOT_L, PLOT_T + PLOT_H / 2 - 12, PLOT_L + PLOT_W,
                  PLOT_T + PLOT_H / 2 + 12, LV_TEXT_ALIGN_CENTER, UI_COLOR_SUBTEXT);
    }

    lv_canvas_finish_layer(s_canvas, &layer);
    s_drawn_key = s_now_key;
}

static void update_stats(void)
{
    const graph_def_t *g = &GRAPHS[s_graph];
    const bucket_series_t *b = &s_b[0];
    float lo = INFINITY, hi = -INFINITY, cur = NAN;
    double sum = 0;
    int n = 0;
    for (int i = 0; i < s_nb; i++) {
        if (!b->ok[i]) continue;
        if (b->lo[i] < lo) lo = b->lo[i];
        if (b->hi[i] > hi) hi = b->hi[i];
        sum += b->avg[i];
        n++;
        cur = b->avg[i];
    }
    char buf[224], v1[16], v2[16], v3[16], v4[16], name[64];
    if (strcmp(g->ser[0].label, g->tab) == 0) {
        snprintf(name, sizeof(name), "%s", g->title);
    } else {
        snprintf(name, sizeof(name), "%s  %s", g->title, g->ser[0].label);
    }
    if (!n) {
        snprintf(buf, sizeof(buf), "%s：データなし", name);
    } else {
        snprintf(v1, sizeof(v1), g->fmt, cur);
        snprintf(v2, sizeof(v2), g->fmt, lo);
        snprintf(v3, sizeof(v3), g->fmt, hi);
        snprintf(v4, sizeof(v4), g->fmt, sum / n);
        snprintf(buf, sizeof(buf), "%s   最新 %s%s   最小 %s   最大 %s   平均 %s", name, v1, g->unit, v2, v3, v4);
    }
    lv_label_set_text(s_stats, buf);
}

static void redraw(void)
{
    compute();
    render();
    update_stats();
}


/* ---------- crosshair ---------- */

static void tip_hide(void)
{
    lv_obj_add_flag(s_cross, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tip, LV_OBJ_FLAG_HIDDEN);
}

static void tip_show(int32_t cx)
{
    const graph_def_t *g = &GRAPHS[s_graph];
    int i = (int)((int64_t)(cx - PLOT_L) * s_nb / PLOT_W);
    if (i < 0) i = 0;
    if (i >= s_nb) i = s_nb - 1;

    int32_t x = x_px(i);
    lv_obj_set_pos(s_cross, CANVAS_X + x - 1, CANVAS_Y + PLOT_T);
    lv_obj_remove_flag(s_cross, LV_OBJ_FLAG_HIDDEN);

    const uint32_t r = RANGE_MIN;
    uint32_t k0 = s_start_key + (uint32_t)((uint64_t)i * r / s_nb);
    uint32_t k1 = s_start_key + (uint32_t)((uint64_t)(i + 1) * r / s_nb) - 1;
    char a[24], b[24], buf[64];
    format_point(a, sizeof(a), k0);
    if (k1 > k0) {
        format_point(b, sizeof(b), k1);
        snprintf(buf, sizeof(buf), "%s〜%s", a, b);
    } else {
        snprintf(buf, sizeof(buf), "%s", a);
    }
    lv_label_set_text(s_tip_time, buf);

    for (int k = 0; k < MAX_SER; k++) {
        if (k >= g->nser || !s_on[s_graph][k]) {
            lv_obj_add_flag(s_tip_row[k], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_tip_row[k], LV_OBJ_FLAG_HIDDEN);
        char v[16];
        if (s_b[k].ok[i]) {
            snprintf(v, sizeof(v), g->fmt, s_b[k].avg[i]);
        } else {
            snprintf(v, sizeof(v), "---");
        }
        snprintf(buf, sizeof(buf), "%s  %s%s", g->ser[k].label, v, s_b[k].ok[i] ? g->unit : "");
        lv_label_set_text(s_tip_val[k], buf);
    }

    lv_obj_update_layout(s_tip);
    int32_t w = lv_obj_get_width(s_tip);
    int32_t tx = CANVAS_X + x + 16;
    if (tx + w > 1280 - UI_PAD) tx = CANVAS_X + x - 16 - w;
    lv_obj_set_pos(s_tip, tx, CANVAS_Y + PLOT_T + 8);
    lv_obj_remove_flag(s_tip, LV_OBJ_FLAG_HIDDEN);
}

static void canvas_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        tip_show(p.x - CANVAS_X);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        tip_hide();
    }
}

/* ---------- controls ---------- */

static void set_checked(lv_obj_t *o, bool on)
{
    if (on) lv_obj_add_state(o, LV_STATE_CHECKED);
    else lv_obj_remove_state(o, LV_STATE_CHECKED);
}

static void legend_rebuild(void)
{
    const graph_def_t *g = &GRAPHS[s_graph];
    for (int k = 0; k < MAX_SER; k++) {
        if (k >= g->nser || g->nser < 2) {
            lv_obj_add_flag(s_chips[k], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_chips[k], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(s_chips[k], 1), g->ser[k].label);
        lv_obj_set_style_opa(s_chips[k], s_on[s_graph][k] ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

static void select_graph(ui_graph_t id)
{
    s_graph = id;
    for (int i = 0; i < UI_GRAPH_COUNT; i++) set_checked(s_tabs[i], i == (int)id);
    for (int k = 0; k < MAX_SER; k++) {
        lv_obj_set_style_bg_color(s_tip_dot[k], lv_color_hex(SER_COLOR[k]), 0);
    }
    legend_rebuild();
    tip_hide();
    redraw();
}

static void tab_cb(lv_event_t *e)
{
    select_graph((ui_graph_t)(intptr_t)lv_event_get_user_data(e));
}

static void chip_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    s_on[s_graph][k] = !s_on[s_graph][k];
    legend_rebuild();
    tip_hide();
    redraw();
}

static void back(void)
{
    tip_hide();
    lv_screen_load(s_back_to);
}

static void back_cb(lv_event_t *e)
{
    back();
}

static void gesture_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        back();
    }
}

static void refresh_cb(lv_timer_t *t)
{
    if (lv_screen_active() != s_scr) {
        return;
    }
    const history_t *h = datastore_lock();
    uint32_t now = datastore_now_key(h);
    bool latest_changed = (h->have ? h->latest_key : 0) != s_drawn_latest;
    datastore_unlock();
    if (now != s_drawn_key || latest_changed) {
        bool tip = !lv_obj_has_flag(s_tip, LV_OBJ_FLAG_HIDDEN);
        redraw();
        if (tip) tip_hide();
    }
}

static lv_obj_t *seg_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, BTN_H);
    lv_obj_set_style_min_width(b, 96, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_radius(b, BTN_H / 2, 0);
    /* the 8 px gaps between buttons also count as the nearest button */
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_set_style_bg_color(b, UI_COLOR_BORDER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_color(b, UI_COLOR_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_color(b, UI_COLOR_BG, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(b, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &font_jp_20, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

static lv_obj_t *row_box(lv_obj_t *parent, int32_t x, int32_t y)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_set_pos(r, x, y);
    return r;
}

static void create(void)
{
    for (int g = 0; g < UI_GRAPH_COUNT; g++) {
        for (int k = 0; k < GRAPHS[g].nser; k++) s_on[g][k] = GRAPHS[g].ser[k].on;
    }

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, UI_COLOR_BG, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    /* top: back + metric tabs + clock */
    lv_obj_t *back_btn = seg_button(s_scr, LV_SYMBOL_LEFT " 戻る", back_cb, 0);
    lv_obj_set_pos(back_btn, UI_PAD, 6);
    lv_obj_t *tabs = row_box(s_scr, 172, 6);
    /* same order as the cards on the sensor page */
    static const ui_graph_t TAB_ORDER[UI_GRAPH_COUNT] = {UI_GRAPH_TEMP, UI_GRAPH_HUM, UI_GRAPH_PRES, UI_GRAPH_CO2};
    for (int j = 0; j < UI_GRAPH_COUNT; j++) {
        ui_graph_t i = TAB_ORDER[j];
        s_tabs[i] = seg_button(tabs, GRAPHS[i].tab, tab_cb, i);
        lv_obj_set_style_min_width(s_tabs[i], TAB_MIN_W, 0);
    }
    lv_obj_t *clock = ui_label_create(s_scr, &font_jp_24, UI_COLOR_TEXT, "--:--");
    lv_obj_align(clock, LV_ALIGN_TOP_RIGHT, -UI_PAD, 20);
    ui_clock_attach(clock);

    /* second row: legend (series on/off) + period */
    s_legend = row_box(s_scr, UI_PAD, 70);
    lv_obj_t *period = ui_label_create(s_scr, &font_jp_20, UI_COLOR_SUBTEXT, "直近 12 時間");
    lv_obj_align(period, LV_ALIGN_TOP_RIGHT, -UI_PAD, 86);
    for (int k = 0; k < MAX_SER; k++) {
        lv_obj_t *chip = seg_button(s_legend, "", chip_cb, k);
        lv_obj_t *lab = lv_obj_get_child(chip, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(chip, 8, 0);
        lv_obj_t *sw = lv_obj_create(chip);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 18, 4);
        lv_obj_set_style_radius(sw, 2, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(SER_COLOR[k]), 0);
        lv_obj_move_to_index(sw, 0);
        (void)lab;
        s_chips[k] = chip;
    }

    s_stats = ui_label_create(s_scr, &font_jp_20, UI_COLOR_SUBTEXT, "");
    lv_obj_set_pos(s_stats, UI_PAD, 136);

    /* canvas */
    s_draw_buf = lv_draw_buf_create(CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    s_canvas = lv_canvas_create(s_scr);
    lv_canvas_set_draw_buf(s_canvas, s_draw_buf);
    lv_obj_set_pos(s_canvas, CANVAS_X, CANVAS_Y);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_canvas, canvas_event_cb, LV_EVENT_ALL, NULL);

    /* crosshair + tooltip */
    s_cross = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_cross);
    lv_obj_set_size(s_cross, 2, PLOT_H);
    lv_obj_set_style_bg_opa(s_cross, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cross, UI_COLOR_TEXT, 0);
    lv_obj_remove_flag(s_cross, LV_OBJ_FLAG_CLICKABLE);

    s_tip = ui_card_create(s_scr);
    lv_obj_set_size(s_tip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_tip, UI_COLOR_BG, 0);
    lv_obj_set_style_pad_all(s_tip, 12, 0);
    lv_obj_set_flex_flow(s_tip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_tip, 6, 0);
    lv_obj_remove_flag(s_tip, LV_OBJ_FLAG_CLICKABLE);
    s_tip_time = ui_label_create(s_tip, &font_jp_20, UI_COLOR_TEXT, "");
    for (int k = 0; k < MAX_SER; k++) {
        lv_obj_t *row = lv_obj_create(s_tip);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        s_tip_dot[k] = lv_obj_create(row);
        lv_obj_remove_style_all(s_tip_dot[k]);
        lv_obj_set_size(s_tip_dot[k], 12, 12);
        lv_obj_set_style_radius(s_tip_dot[k], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_tip_dot[k], LV_OPA_COVER, 0);
        s_tip_val[k] = ui_label_create(row, &font_jp_20, UI_COLOR_TEXT, "");
        s_tip_row[k] = row;
    }
    tip_hide();

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
}

void graph_open(ui_graph_t id)
{
    if (!s_scr) {
        create();
    }
    s_back_to = lv_screen_active();
    select_graph(id);
    lv_screen_load(s_scr);
}
