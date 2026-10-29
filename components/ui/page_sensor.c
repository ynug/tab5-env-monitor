#include "pages.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "datastore.h"
#include "derived.h"
#include "sensor_hub.h"
#include "ui_theme.h"

#define CARD_GAP    16
#define CARD_W      ((1280 - UI_PAD * 2 - CARD_GAP) / 2)
#define DOTS_H      40      /* page indicator below the cards */
#define CARD_H      ((720 - UI_STATUS_H - UI_PAD - DOTS_H - CARD_GAP) / 2)
#define REFRESH_MS  500
#define HIST_MS     10000
#define SPARK_W     150
#define SPARK_H     84
#define SPARK_MIN   60
#define TREND_MIN   10
#define CO2_CAUTION 1000.0f
#define CO2_WARNING 1500.0f

typedef struct {
    const char *title;
    const char *unit;
    const char *fmt;
    float res;          /* display resolution */
    float spark_span;   /* minimum y span of the sparkline */
    float trend;        /* change over TREND_MIN minutes that counts as rising / falling */
    ui_graph_t graph;
} metric_def_t;

static const metric_def_t METRICS[M_COUNT] = {
    [M_CO2]  = {"CO2", "ppm", "%.0f", 1.0f, 50.0f, 30.0f, UI_GRAPH_CO2},
    [M_TEMP] = {"温度", "℃", "%.1f", 0.1f, 0.5f, 0.3f, UI_GRAPH_TEMP},
    [M_HUM]  = {"湿度", "%", "%.1f", 0.1f, 2.0f, 1.5f, UI_GRAPH_HUM},
    [M_PRES] = {"気圧（現地）", "hPa", "%.1f", 0.1f, 1.0f, 0.5f, UI_GRAPH_PRES},
};

typedef struct {
    lv_obj_t *value, *note, *trend, *spark;
    lv_draw_buf_t *spark_buf;
    float shown;        /* value currently displayed (hysteresis reference) */
} card_t;

/* Grid position of each card (row-major): temperature, humidity / pressure, CO2 */
static const int CARD_POS[M_COUNT] = {[M_TEMP] = 0, [M_HUM] = 1, [M_PRES] = 2, [M_CO2] = 3};

static card_t s_card[M_COUNT];
static lv_obj_t *s_co2_dot, *s_co2_state;
static lv_obj_t *s_wbgt_dot, *s_wbgt_state;

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

static const char *source_name(source_t s)
{
    switch (s) {
    case SRC_SHT: return "SHT35";
    case SRC_BMP: return "BMP581（代替）";
    case SRC_SCD: return "SCD41（代替）";
    default:      return "データ待ち";
    }
}

static void open_cb(lv_event_t *e)
{
    graph_open((ui_graph_t)(intptr_t)lv_event_get_user_data(e));
}

static void make_tappable(lv_obj_t *o, ui_graph_t g)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, UI_COLOR_BORDER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(o, open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)g);
}

static void metric_card_create(lv_obj_t *page, int idx)
{
    const metric_def_t *m = &METRICS[idx];
    card_t *c = &s_card[idx];
    c->shown = NAN;

    lv_obj_t *card = ui_card_create(page);
    lv_obj_set_size(card, CARD_W, CARD_H);
    const int pos = CARD_POS[idx];
    lv_obj_set_pos(card, UI_PAD + (pos % 2) * (CARD_W + CARD_GAP), UI_PAD + (pos / 2) * (CARD_H + CARD_GAP));
    make_tappable(card, m->graph);

    lv_obj_t *title = ui_label_create(card, &font_jp_24, UI_COLOR_SUBTEXT, m->title);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    c->trend = ui_label_create(card, &font_jp_24, UI_COLOR_SUBTEXT, "");
    lv_obj_align_to(c->trend, title, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    /* value + unit; the unit follows the value width */
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_align(row, LV_ALIGN_LEFT_MID, 0, 8);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    c->value = ui_label_create(row, &font_num_96, UI_COLOR_TEXT, "---");
    lv_obj_t *unit = ui_label_create(row, &font_jp_32, UI_COLOR_SUBTEXT, m->unit);
    lv_obj_set_style_pad_bottom(unit, 14, 0);

    c->note = ui_label_create(card, &font_jp_16, UI_COLOR_SUBTEXT, "データ待ち");
    lv_obj_align(c->note, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* last hour: drawn into a small canvas only when the data changes (a live polyline is
     * re-rendered for every strip and was the most expensive thing on screen) */
    c->spark_buf = lv_draw_buf_create(SPARK_W, SPARK_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    c->spark = lv_canvas_create(card);
    lv_canvas_set_draw_buf(c->spark, c->spark_buf);
    lv_canvas_fill_bg(c->spark, UI_COLOR_CARD, LV_OPA_COVER);
    lv_obj_align(c->spark, LV_ALIGN_RIGHT_MID, 0, 8);
    lv_obj_remove_flag(c->spark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c->spark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *cap = ui_label_create(card, &font_jp_16, UI_COLOR_SUBTEXT, "直近1時間");
    lv_obj_align(cap, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    if (idx == M_CO2 || idx == M_TEMP) {
        lv_obj_t *chip = lv_obj_create(card);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(chip, 8, 0);
        lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, 0, 2);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *dot = lv_obj_create(chip);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, UI_COLOR_BORDER, 0);
        lv_obj_t *state = ui_label_create(chip, &font_jp_20, UI_COLOR_TEXT, "");
        if (idx == M_CO2) {
            s_co2_dot = dot;
            s_co2_state = state;
        } else {
            s_wbgt_dot = dot;
            s_wbgt_state = state;
        }
    }
}

/* Displayed value only moves once the value is 3/4 of a digit away from what is shown */
static void show_value(int m, const display_vals_t *d)
{
    card_t *c = &s_card[m];
    char buf[24];
    if (!d->valid[m]) {
        c->shown = NAN;
        set_text(c->value, "---");
        return;
    }
    float v = d->value[m];
    if (isnan(c->shown) || fabsf(v - c->shown) >= 0.75f * METRICS[m].res) {
        c->shown = roundf(v / METRICS[m].res) * METRICS[m].res;
    }
    snprintf(buf, sizeof(buf), METRICS[m].fmt, c->shown);
    set_text(c->value, buf);
}

static void show_co2_state(const display_vals_t *d)
{
    if (!d->valid[M_CO2]) {
        set_text(s_co2_state, "");
        lv_obj_set_style_bg_color(s_co2_dot, UI_COLOR_BORDER, 0);
        return;
    }
    float v = s_card[M_CO2].shown;
    const char *txt = v >= CO2_WARNING ? "警告" : v >= CO2_CAUTION ? "注意" : "良好";
    lv_color_t col = v >= CO2_WARNING ? UI_COLOR_BAD : v >= CO2_CAUTION ? UI_COLOR_WARN : UI_COLOR_GOOD;
    set_text(s_co2_state, txt);
    lv_obj_set_style_bg_color(s_co2_dot, col, 0);
}

/* Heat stroke risk (WBGT estimated from the temperature and humidity) in the temperature card */
static void show_wbgt(const display_vals_t *d)
{
    static const char *const LABEL[5] = {"ほぼ安全", "注意", "警戒", "厳重警戒", "危険"};
    if (!d->valid[M_TEMP] || !d->valid[M_HUM]) {
        set_text(s_wbgt_state, "");
        lv_obj_set_style_bg_color(s_wbgt_dot, UI_COLOR_BORDER, 0);
        return;
    }
    float w = derived_wbgt(d->value[M_TEMP], d->value[M_HUM]);
    int lv = derived_wbgt_level(w);
    char buf[48];
    snprintf(buf, sizeof(buf), "暑さ指数 %.0f %s", floorf(w), LABEL[lv]);
    set_text(s_wbgt_state, buf);
    lv_obj_set_style_bg_color(s_wbgt_dot, lv >= 3 ? UI_COLOR_BAD : lv >= 1 ? UI_COLOR_WARN : UI_COLOR_GOOD, 0);
}

static void refresh(lv_timer_t *t)
{
    sensor_live_t live;
    sensor_hub_get_live(&live);
    const display_vals_t *d = &live.disp;
    char buf[64];

    for (int m = 0; m < M_COUNT; m++) {
        show_value(m, d);
    }
    show_co2_state(d);
    show_wbgt(d);

    const char *sim = live.demo ? "・デモ" : live.sim ? "・模擬" : "";
    snprintf(buf, sizeof(buf), "%s%s", d->valid[M_CO2] ? "SCD41" : "データ待ち", sim);
    set_text(s_card[M_CO2].note, buf);
    snprintf(buf, sizeof(buf), "%s%s", source_name(d->t_src), sim);
    set_text(s_card[M_TEMP].note, buf);
    snprintf(buf, sizeof(buf), "%s%s", source_name(d->rh_src), sim);
    set_text(s_card[M_HUM].note, buf);
    snprintf(buf, sizeof(buf), "%s%s", d->valid[M_PRES] ? "BMP581" : "データ待ち", sim);
    set_text(s_card[M_PRES].note, buf);

}

static float metric_of(const hist_entry_t *e, int m)
{
    switch (m) {
    case M_CO2:  return e->s[S_CO2].n ? e->s[S_CO2].avg : NAN;
    case M_TEMP: return history_primary_temp(e);
    case M_HUM:  return history_primary_rh(e);
    default:     return e->s[S_P_BMP].n ? e->s[S_P_BMP].avg : NAN;
    }
}

/* Sparklines and trend arrows from the last hour of 1-minute data */
static void history_refresh(lv_timer_t *t)
{
    static float v[M_COUNT][SPARK_MIN];

    const history_t *h = datastore_lock();
    uint32_t now = datastore_now_key(h);
    for (int m = 0; m < M_COUNT; m++) {
        for (int i = 0; i < SPARK_MIN; i++) {
            const hist_entry_t *e = history_get(h, now - (SPARK_MIN - 1) + i);
            v[m][i] = e ? metric_of(e, m) : NAN;
        }
    }
    datastore_unlock();

    for (int m = 0; m < M_COUNT; m++) {
        card_t *c = &s_card[m];
        float lo = INFINITY, hi = -INFINITY;
        int n = 0, last = -1;
        for (int i = 0; i < SPARK_MIN; i++) {
            if (isnan(v[m][i])) continue;
            if (v[m][i] < lo) lo = v[m][i];
            if (v[m][i] > hi) hi = v[m][i];
            last = i;
            n++;
        }
        if (n < 2) {
            lv_obj_add_flag(c->spark, LV_OBJ_FLAG_HIDDEN);
            set_text(c->trend, "");
            continue;
        }
        if (hi - lo < METRICS[m].spark_span) {
            float mid = 0.5f * (hi + lo);
            lo = mid - METRICS[m].spark_span / 2;
            hi = mid + METRICS[m].spark_span / 2;
        }
        lv_canvas_fill_bg(c->spark, UI_COLOR_CARD, LV_OPA_COVER);
        lv_layer_t layer;
        lv_canvas_init_layer(c->spark, &layer);
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.color = lv_color_hex(0x3987E5);
        ld.width = 2;
        ld.round_start = ld.round_end = 1;
        bool have_prev = false;
        for (int i = 0; i < SPARK_MIN; i++) {
            if (isnan(v[m][i])) continue;
            lv_value_precise_t x = (lv_value_precise_t)(i * (SPARK_W - 3) / (SPARK_MIN - 1) + 1);
            lv_value_precise_t y = (lv_value_precise_t)((hi - v[m][i]) / (hi - lo) * (SPARK_H - 4) + 2);
            if (have_prev) {
                ld.p2.x = x;
                ld.p2.y = y;
                lv_draw_line(&layer, &ld);
            }
            ld.p1.x = x;
            ld.p1.y = y;
            have_prev = true;
        }
        lv_canvas_finish_layer(c->spark, &layer);
        lv_obj_remove_flag(c->spark, LV_OBJ_FLAG_HIDDEN);

        /* trend: latest vs. the nearest value about TREND_MIN minutes earlier */
        const char *arrow = "";
        for (int i = last - TREND_MIN; i >= 0 && i >= last - TREND_MIN - 3; i--) {
            if (isnan(v[m][i])) continue;
            float dv = v[m][last] - v[m][i];
            arrow = dv >= METRICS[m].trend ? "↗" : dv <= -METRICS[m].trend ? "↘" : "→";
            break;
        }
        set_text(c->trend, arrow);
    }
}

void page_sensor_create(lv_obj_t *page)
{
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < M_COUNT; i++) {
        metric_card_create(page, i);
    }


    refresh(NULL);
    history_refresh(NULL);
    lv_timer_create(refresh, REFRESH_MS, NULL);
    lv_timer_create(history_refresh, HIST_MS, NULL);
}
