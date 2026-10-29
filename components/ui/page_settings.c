#include "pages.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "board.h"
#include "controller.h"
#include "settings.h"
#include "datastore.h"
#include "sensor_hub.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdlog.h"
#include "sdkconfig.h"
#include "ui_theme.h"

#define SECTION_W   (1280 - UI_PAD * 2)

enum {
    DIAG_LCD, DIAG_EXT5V, DIAG_SOC_TEMP, DIAG_CPU, DIAG_PSRAM, DIAG_SRAM, DIAG_UPTIME, DIAG_FW,
    DIAG_SOURCE, DIAG_SENSORS, DIAG_CLOCK, DIAG_RAM_HIST, DIAG_FLASH_HIST, DIAG_SCD_OFFSET, DIAG_REJECTS, DIAG_COUNT
};

static const char *DIAG_NAMES[DIAG_COUNT] = {
    "液晶", "PORT.A 5V出力", "SoC温度", "CPU", "PSRAM 空き", "内部RAM 空き", "稼働時間", "ファームウェア",
    "データ源", "センサー（PORT.A）", "時刻", "記録（RAM・24時間）", "記録（フラッシュ）", "SCD41 温度補正", "除外した値（範囲外／外れ値）",
};

static lv_obj_t *s_diag_val[DIAG_COUNT];

static lv_obj_t *section_create(lv_obj_t *page, const char *title)
{
    lv_obj_t *card = ui_card_create(page);
    lv_obj_set_width(card, SECTION_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);
    ui_label_create(card, &font_jp_24, UI_COLOR_ACCENT, title);
    return card;
}

static lv_obj_t *row_create(lv_obj_t *parent, const char *name)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_label_create(row, &font_jp_20, UI_COLOR_TEXT, name);
    return row;
}

static void brightness_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        display_set_brightness(v);      /* preview */
        return;
    }
    app_settings_t st;
    settings_get(&st);
    st.brightness = v;
    settings_set(&st);                  /* saved on release */
    controller_refresh_backlight();
}

static void flip_cb(lv_event_t *e)
{
    app_settings_t st;
    settings_get(&st);
    st.flipped = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    settings_set(&st);
}

static void set_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void clock_now_update(void);
static void sd_update(void);

static void diag_update(lv_timer_t *t)
{
    char buf[96];

    float soc = board_soc_temp();
    if (isnan(soc)) {
        snprintf(buf, sizeof(buf), "---");
    } else {
        snprintf(buf, sizeof(buf), "%.1f ℃", soc);
    }
    set_if_changed(s_diag_val[DIAG_SOC_TEMP], buf);

    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    set_if_changed(s_diag_val[DIAG_PSRAM], buf);
    snprintf(buf, sizeof(buf), "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    set_if_changed(s_diag_val[DIAG_SRAM], buf);

    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(buf, sizeof(buf), "%lu日 %02lu:%02lu:%02lu", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
    set_if_changed(s_diag_val[DIAG_UPTIME], buf);

    sensor_live_t live;
    sensor_hub_get_live(&live);
    datastore_stats_t ds;
    datastore_get_stats(&ds);

    snprintf(buf, sizeof(buf), "%s（起動 #%lu）", live.sim ? "模擬センサー" : "実センサー", ds.boot_id);
    set_if_changed(s_diag_val[DIAG_SOURCE], buf);
    if (live.sim) {
        snprintf(buf, sizeof(buf), "応答なし（起動時に見つからず、模擬センサーで動作中）");
    } else {
        snprintf(buf, sizeof(buf), "SHT35 %s ／ BMP581 %s ／ SCD41 %s",
                 live.present & SENSOR_PRESENT_SHT35 ? "応答あり" : "応答なし",
                 live.present & SENSOR_PRESENT_BMP581 ? "応答あり" : "応答なし",
                 live.present & SENSOR_PRESENT_SCD41 ? "応答あり" : "応答なし");
    }
    set_if_changed(s_diag_val[DIAG_SENSORS], buf);
    set_if_changed(s_diag_val[DIAG_CLOCK], clock_valid() ? "設定済み（時計IC で保持）" : "未設定（上の「時計」で合わせてください）");
    snprintf(buf, sizeof(buf), "%lu 分", ds.ram_minutes);
    set_if_changed(s_diag_val[DIAG_RAM_HIST], buf);
    snprintf(buf, sizeof(buf), "%lu / %lu 件（書込 %lu・消去 %lu・失敗 %lu）", ds.flash_records, ds.flash_capacity,
             ds.flash_writes, ds.flash_erases, ds.flash_errors);
    set_if_changed(s_diag_val[DIAG_FLASH_HIST], buf);
    if (live.offset_known) {
        snprintf(buf, sizeof(buf), "−%.2f ℃", live.scd_t_offset);
    } else {
        snprintf(buf, sizeof(buf), "学習中");
    }
    set_if_changed(s_diag_val[DIAG_SCD_OFFSET], buf);
    uint32_t rr = 0, ro = 0;
    for (int i = 0; i < RAW_COUNT; i++) {
        rr += live.stats.rejected_range[i];
        ro += live.stats.rejected_outlier[i];
    }
    snprintf(buf, sizeof(buf), "%lu ／ %lu", rr, ro);
    set_if_changed(s_diag_val[DIAG_REJECTS], buf);

    clock_now_update();
    sd_update();
}


/* ---------- generic controls ---------- */

static lv_obj_t *seg_create(lv_obj_t *parent, const char *const *labels, int n, int sel, lv_event_cb_t cb)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(box, 8, 0);
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_button_create(box);
        lv_obj_set_height(b, 52);
        lv_obj_set_style_min_width(b, 120, 0);
        lv_obj_set_style_pad_hor(b, 18, 0);
        lv_obj_set_style_radius(b, 26, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_ext_click_area(b, 4);
        lv_obj_set_style_bg_color(b, UI_COLOR_BG, 0);
        lv_obj_set_style_bg_color(b, UI_COLOR_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(b, UI_COLOR_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(b, UI_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_text_color(b, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_color(b, UI_COLOR_BG, LV_STATE_CHECKED);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &font_jp_20, 0);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
        if (i == sel) lv_obj_add_state(b, LV_STATE_CHECKED);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    return box;
}

/* Check the clicked button of a segmented control, return its index */
static int seg_select(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target_obj(e);
    lv_obj_t *box = lv_obj_get_parent(b);
    for (uint32_t i = 0; i < lv_obj_get_child_count(box); i++) {
        lv_obj_t *c = lv_obj_get_child(box, i);
        if (c == b) lv_obj_add_state(c, LV_STATE_CHECKED);
        else lv_obj_remove_state(c, LV_STATE_CHECKED);
    }
    return (int)(intptr_t)lv_event_get_user_data(e);
}

static lv_obj_t *switch_create(lv_obj_t *row, bool on, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 72, 40);
    lv_obj_set_ext_click_area(sw, 12);
    lv_obj_set_style_opa(sw, LV_OPA_40, LV_STATE_DISABLED);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, (void *)ud);
    return sw;
}

static bool sw_on(lv_event_t *e)
{
    return lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
}

static lv_obj_t *slider_create(lv_obj_t *row, int lo, int hi, int val, lv_event_cb_t cb)
{
    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_width(sl, 480);
    lv_obj_set_ext_click_area(sl, 16);
    lv_slider_set_range(sl, lo, hi);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_RELEASED, NULL);
    return sl;
}

static lv_obj_t *action_button(lv_obj_t *row, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_height(b, 52);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_color(b, UI_COLOR_BORDER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = ui_label_create(b, &font_jp_20, UI_COLOR_TEXT, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *note_create(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = ui_label_create(parent, &font_jp_16, UI_COLOR_SUBTEXT, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

/* ---------- alarm ---------- */

static const char *const ALARM_LABEL[AL_COUNT] = {
    [AL_CO2] = "CO2（換気）", [AL_HEAT] = "熱中症", [AL_HUM_LOW] = "乾燥", [AL_HUM_HIGH] = "多湿",
    [AL_TEMP_LOW] = "低温", [AL_PRES_DROP] = "気圧の急低下",
};

static void alarm_sw_cb(lv_event_t *e)
{
    app_settings_t st;
    settings_get(&st);
    st.alarm_enabled[(int)(intptr_t)lv_event_get_user_data(e)] = sw_on(e);
    settings_set(&st);
}

static void volume_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    app_settings_t st;
    settings_get(&st);
    st.volume = lv_slider_get_value(lv_event_get_target_obj(e));
    settings_set(&st);
}

/* ---------- clock ---------- */

#define YEAR_FIRST  2025
#define YEAR_COUNT  16

enum { CLK_Y, CLK_MO, CLK_D, CLK_H, CLK_MI, CLK_COUNT };

static lv_obj_t *s_clk[CLK_COUNT];
static lv_obj_t *s_clk_now;
static char s_opt_year[YEAR_COUNT * 5], s_opt_mon[12 * 3], s_opt_day[31 * 3], s_opt_hour[24 * 3], s_opt_min[60 * 3];

static void fill_options(char *buf, size_t n, int first, int count, int width)
{
    size_t o = 0;
    for (int i = 0; i < count; i++) {
        o += (size_t)snprintf(buf + o, n - o, "%s%0*d", i ? "\n" : "", width, first + i);
    }
}

static int days_in_month(int y, int m)
{
    static const int D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 29 : D[m - 1];
}

/* Rollers to the current time, or to the build date while the clock is not set */
static void clock_rollers_load(void)
{
    struct tm tm = {0};
    if (clock_valid()) {
        time_t t = time(NULL);
        localtime_r(&t, &tm);
    } else {
        static const char MON[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char mon[4] = {0};
        int d = 1, y = YEAR_FIRST;
        sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
        const char *p = strstr(MON, mon);
        tm.tm_year = y - 1900;
        tm.tm_mon = p ? (int)(p - MON) / 3 : 0;
        tm.tm_mday = d;
        tm.tm_hour = 12;
    }
    int y = tm.tm_year + 1900 - YEAR_FIRST;
    lv_roller_set_selected(s_clk[CLK_Y], y < 0 ? 0 : y >= YEAR_COUNT ? YEAR_COUNT - 1 : y, LV_ANIM_OFF);
    lv_roller_set_selected(s_clk[CLK_MO], tm.tm_mon, LV_ANIM_OFF);
    lv_roller_set_selected(s_clk[CLK_D], tm.tm_mday - 1, LV_ANIM_OFF);
    lv_roller_set_selected(s_clk[CLK_H], tm.tm_hour, LV_ANIM_OFF);
    lv_roller_set_selected(s_clk[CLK_MI], tm.tm_min, LV_ANIM_OFF);
}

static void clock_now_update(void)
{
    char buf[48];
    if (clock_valid()) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M", &tm);
    } else {
        snprintf(buf, sizeof(buf), "未設定");
    }
    set_if_changed(s_clk_now, buf);
}

static void clock_set_cb(lv_event_t *e)
{
    int y = YEAR_FIRST + (int)lv_roller_get_selected(s_clk[CLK_Y]);
    int mo = (int)lv_roller_get_selected(s_clk[CLK_MO]) + 1;
    int d = (int)lv_roller_get_selected(s_clk[CLK_D]) + 1;
    if (d > days_in_month(y, mo)) {
        d = days_in_month(y, mo);
        lv_roller_set_selected(s_clk[CLK_D], d - 1, LV_ANIM_OFF);
    }
    struct tm tm = {
        .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
        .tm_hour = (int)lv_roller_get_selected(s_clk[CLK_H]), .tm_min = (int)lv_roller_get_selected(s_clk[CLK_MI]),
        .tm_isdst = -1,
    };
    time_t t = mktime(&tm);
    bool rtc = board_rtc_write(t);
    datastore_set_clock(t);
    char buf[192];
    snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:00 に合わせました。%s", y, mo, d, tm.tm_hour, tm.tm_min,
             rtc ? "" : "\n時計IC への書き込みに失敗したため、電源を切ると失われます。");
    ui_message("時計", buf);
}

static lv_obj_t *clock_roller(lv_obj_t *parent, const char *opts, int w)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, w);
    lv_obj_set_style_text_font(r, &font_jp_20, 0);
    return r;
}

/* ---------- SD card ---------- */

static lv_obj_t *s_sd_state, *s_sd_last, *s_sd_msg, *s_sd_auto, *s_sd_export;

static void set_enabled(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
}

static void sd_update(void)
{
    sdlog_status_t st;
    sdlog_get_status(&st);
    char buf[96];
    if (st.state == SDLOG_NO_CARD) {
        snprintf(buf, sizeof(buf), "カードなし");
    } else if (st.state == SDLOG_BUSY) {
        snprintf(buf, sizeof(buf), "書き出し中…（カードを抜かないでください）");
    } else {
        snprintf(buf, sizeof(buf), "使用できます（空き %.1f GB ／ %.1f GB）", st.free_mb / 1024.0, st.total_mb / 1024.0);
    }
    set_if_changed(s_sd_state, buf);
    if (st.last_export) {
        time_t t = (time_t)st.last_export;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(buf, sizeof(buf), "%m/%d %H:%M", &tm);
    } else {
        snprintf(buf, sizeof(buf), "なし");
    }
    set_if_changed(s_sd_last, buf);
    set_if_changed(s_sd_msg, st.msg);
    set_enabled(s_sd_auto, st.state != SDLOG_NO_CARD);
    set_enabled(s_sd_export, st.state == SDLOG_READY);
}

static void sd_auto_cb(lv_event_t *e)
{
    app_settings_t st;
    settings_get(&st);
    st.sd_auto_export = sw_on(e);
    settings_set(&st);
}

static void sd_export_cb(lv_event_t *e)
{
    sdlog_request_export();
}

static void sd_check_cb(lv_event_t *e)
{
    sdlog_request_check();
}

/* ---------- CO2 calibration ---------- */

#define FRC_TARGET_PPM  420
#define FRC_WAIT_S      180

static lv_obj_t *s_frc_msg;
static lv_timer_t *s_frc_timer;
static int s_frc_left;

static void frc_msg_deleted(lv_event_t *e)
{
    s_frc_msg = NULL;
    if (s_frc_timer) {
        lv_timer_delete(s_frc_timer);
        s_frc_timer = NULL;
    }
}

static void frc_tick(lv_timer_t *t)
{
    if (!s_frc_msg) return;
    if (--s_frc_left > 0) {
        lv_label_set_text_fmt(s_frc_msg, "外気に近い空気で測定しています。動かさずにお待ちください。\n残り %d:%02d",
                              s_frc_left / 60, s_frc_left % 60);
        return;
    }
    lv_timer_delete(s_frc_timer);
    s_frc_timer = NULL;
    int16_t corr = 0;
    esp_err_t err = sensor_hub_frc(FRC_TARGET_PPM, &corr);
    if (err == ESP_OK) {
        lv_label_set_text_fmt(s_frc_msg, "校正しました（補正量 %d ppm）。", corr);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        lv_label_set_text(s_frc_msg, "模擬センサーでは実行できません。実センサーの接続後に使えます。");
    } else {
        lv_label_set_text(s_frc_msg, "校正に失敗しました。もう一度お試しください。");
    }
}

static void frc_begin(void)
{
    s_frc_left = FRC_WAIT_S + 1;
    s_frc_msg = ui_message("CO2 の手動校正", "");
    lv_obj_add_event_cb(s_frc_msg, frc_msg_deleted, LV_EVENT_DELETE, NULL);
    s_frc_timer = lv_timer_create(frc_tick, 1000, NULL);
    frc_tick(NULL);
}

static void frc_cb(lv_event_t *e)
{
    ui_confirm("CO2 の手動校正",
               "屋外か、窓を開けて十分に換気した場所に置いてください。3 分間測定してから、その値を 420 ppm として校正します。",
               "開始", frc_begin);
}

static void asc_cb(lv_event_t *e)
{
    app_settings_t st;
    settings_get(&st);
    st.scd41_asc = sw_on(e);
    settings_set(&st);
}

/* ---------- data ---------- */

static void clear_history(void)
{
    datastore_clear();
    ui_message("記録を消去しました", "24 時間分のグラフと、フラッシュの記録を消去しました。");
}

static void clear_cb(lv_event_t *e)
{
    ui_confirm("記録の消去", "グラフに使う記録をすべて消去します。元に戻せません。", "消去する", clear_history);
}

static void reset_settings(void)
{
    settings_reset();
    esp_restart();
}

static void reset_cb(lv_event_t *e)
{
    ui_confirm("設定の初期化", "すべての設定を初期値に戻して再起動します。記録と時計は残ります。", "初期化する",
               reset_settings);
}

void page_settings_create(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(page, UI_PAD, 0);
    lv_obj_set_style_pad_bottom(page, 64, 0);
    lv_obj_set_style_pad_row(page, 16, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    /* 画面 */
    lv_obj_t *disp = section_create(page, "画面");
    lv_obj_t *row = row_create(disp, "明るさ");
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_width(slider, 480);
    lv_slider_set_range(slider, 5, 100);
    app_settings_t st;
    settings_get(&st);
    lv_slider_set_value(slider, st.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_RELEASED, NULL);

    row = row_create(disp, "上下を反転");
    switch_create(row, st.flipped, flip_cb, 0);

    /* アラーム */
    lv_obj_t *al = section_create(page, "アラーム");
    row = row_create(al, "音量");
    slider_create(row, 0, 100, st.volume, volume_cb);
    for (int g = 0; g < AL_COUNT; g++) {
        row = row_create(al, ALARM_LABEL[g]);
        switch_create(row, st.alarm_enabled[g], alarm_sw_cb, g);
    }
    note_create(al, "OFF にしても画面の表示は続きます（音声だけを止めます）。音声の試聴はデモ画面でできます");

    /* 時計 */
    fill_options(s_opt_year, sizeof(s_opt_year), YEAR_FIRST, YEAR_COUNT, 4);
    fill_options(s_opt_mon, sizeof(s_opt_mon), 1, 12, 2);
    fill_options(s_opt_day, sizeof(s_opt_day), 1, 31, 2);
    fill_options(s_opt_hour, sizeof(s_opt_hour), 0, 24, 2);
    fill_options(s_opt_min, sizeof(s_opt_min), 0, 60, 2);
    lv_obj_t *ck = section_create(page, "時計");
    row = row_create(ck, "現在の時刻");
    s_clk_now = ui_label_create(row, &font_jp_20, UI_COLOR_TEXT, "");
    row = row_create(ck, "合わせる時刻");
    lv_obj_t *rs = lv_obj_create(row);
    lv_obj_remove_style_all(rs);
    lv_obj_set_size(rs, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rs, 8, 0);
    s_clk[CLK_Y] = clock_roller(rs, s_opt_year, 100);
    ui_label_create(rs, &font_jp_20, UI_COLOR_TEXT, "年");
    s_clk[CLK_MO] = clock_roller(rs, s_opt_mon, 72);
    ui_label_create(rs, &font_jp_20, UI_COLOR_TEXT, "月");
    s_clk[CLK_D] = clock_roller(rs, s_opt_day, 72);
    ui_label_create(rs, &font_jp_20, UI_COLOR_TEXT, "日");
    s_clk[CLK_H] = clock_roller(rs, s_opt_hour, 72);
    ui_label_create(rs, &font_jp_20, UI_COLOR_TEXT, ":");
    s_clk[CLK_MI] = clock_roller(rs, s_opt_min, 72);
    clock_rollers_load();
    row = row_create(ck, "");
    action_button(row, "この時刻に合わせる（0 秒から）", clock_set_cb);
    note_create(ck, "時刻は時計IC に保存され、再起動しても保持されます。電源を切ったあとに「未設定」になっていたら、ここで合わせ直してください。"
                    "月に 1 分ほどずれることがあります。電源を入れてから時刻を合わせるまでの記録は、実際の時刻に並べ直されます");

    /* SD カード */
    lv_obj_t *sd = section_create(page, "SD カードへの書き出し");
    row = row_create(sd, "カード");
    s_sd_state = ui_label_create(row, &font_jp_20, UI_COLOR_SUBTEXT, "");
    row = row_create(sd, "1 時間ごとに自動で書き出す");
    s_sd_auto = switch_create(row, st.sd_auto_export, sd_auto_cb, 0);
    row = row_create(sd, "最後の書き出し");
    s_sd_last = ui_label_create(row, &font_jp_20, UI_COLOR_SUBTEXT, "");
    s_sd_msg = note_create(sd, "");
    lv_obj_set_style_text_color(s_sd_msg, UI_COLOR_TEXT, 0);
    row = row_create(sd, "");
    lv_obj_t *btns = lv_obj_create(row);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 12, 0);
    action_button(btns, "カードを確認", sd_check_cb);
    s_sd_export = action_button(btns, "今すぐ書き出す", sd_export_cb);
    note_create(sd, "ENVLOG フォルダに 1 日 1 ファイル（20261017.CSV のような名前）で、1 分ごとの値を追記します。"
                    "直近 24 時間のうち、まだ書き出していない分が対象です。時刻を合わせてから使えます。"
                    "カードは書き出し中（数秒）以外ならいつ抜いても大丈夫です。挿したあとは「カードを確認」を押してください");

    /* CO2 の校正 */
    lv_obj_t *cal = section_create(page, "CO2 の校正（SCD41）");
    row = row_create(cal, "自動校正（ASC）");
    switch_create(row, st.scd41_asc, asc_cb, 0);
    note_create(cal, "自動校正は、週に 1 回程度は外気と同じくらいまで換気される部屋が前提です。"
                     "そうでない部屋では OFF にして、手動校正を使ってください");
    row = row_create(cal, "手動校正（外気 = 420 ppm）");
    action_button(row, "手動校正を始める", frc_cb);

    /* 診断 */
    lv_obj_t *diag = section_create(page, "診断");
    for (int i = 0; i < DIAG_COUNT; i++) {
        row = row_create(diag, DIAG_NAMES[i]);
        s_diag_val[i] = ui_label_create(row, &font_jp_20, UI_COLOR_SUBTEXT, "---");
    }

    char buf[96];
    lv_label_set_text(s_diag_val[DIAG_LCD], board_lcd_name());
    lv_label_set_text(s_diag_val[DIAG_EXT5V], board_ext5v_enabled() ? "ON" : "失敗");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(buf, sizeof(buf), "ESP32-P4 v%d.%d / %d MHz / %dコア",
             chip.revision / 100, chip.revision % 100, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, chip.cores);
    lv_label_set_text(s_diag_val[DIAG_CPU], buf);

    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(buf, sizeof(buf), "%s (IDF %s)", app->version, app->idf_ver);
    lv_label_set_text(s_diag_val[DIAG_FW], buf);

    diag_update(NULL);
    lv_timer_create(diag_update, 1000, NULL);

    /* データ */
    lv_obj_t *data = section_create(page, "データ");
    row = row_create(data, "記録（本体の 24 時間分）");
    action_button(row, "記録を消去", clear_cb);
    row = row_create(data, "設定");
    action_button(row, "設定を初期値に戻す", reset_cb);

    /* クレジット */
    lv_obj_t *cr = section_create(page, "クレジット");
    note_create(cr, "音声: VOICEVOX:ずんだもん\n"
                    "フォント: Noto Sans JP（SIL Open Font License 1.1）、Montserrat（SIL OFL）\n"
                    "LVGL（MIT License）、ESP-IDF（Apache License 2.0）");
}
