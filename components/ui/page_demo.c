#include "pages.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "audio.h"
#include "sensor_hub.h"
#include "settings.h"
#include "ui_theme.h"

/* Demo scenarios (shown instead of the sensors, see sensor_hub_demo_start) and the announcements */

static const char *const SCN_LABEL[DEMO_COUNT] = {
    [DEMO_NORMAL] = "通常の一日", [DEMO_CO2] = "CO2 上昇（会議室）", [DEMO_HEAT] = "猛暑",
    [DEMO_DRY] = "冬の乾燥", [DEMO_PRES] = "気圧の急降下（台風）",
};
static const int SPEEDS[2] = {1, 60};

static lv_obj_t *s_scn_box, *s_speed_box, *s_state, *s_start_label, *s_stop;
static int s_scn = DEMO_CO2, s_speed = 1;

static const struct {
    sound_t s;
    const char *label;
} CLIPS[SND_COUNT] = {
    {SND_CO2_CAUTION, "CO2 注意"},
    {SND_CO2_WARNING, "CO2 警告"},
    {SND_HEAT_CAUTION, "熱中症 警戒"},
    {SND_HEAT_SEVERE, "熱中症 厳重警戒"},
    {SND_HEAT_DANGER, "熱中症 危険"},
    {SND_HUM_LOW, "乾燥"},
    {SND_HUM_HIGH, "多湿"},
    {SND_TEMP_LOW, "低温"},
    {SND_PRES_DROP, "気圧の急低下"},
};

static void play_cb(lv_event_t *e)
{
    audio_play((sound_t)(intptr_t)lv_event_get_user_data(e));
}

static void volume_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    audio_set_volume(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        app_settings_t st;
        settings_get(&st);
        st.volume = v;
        settings_set(&st);
    }
}

static lv_obj_t *seg_button(lv_obj_t *box, const char *text, bool checked, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_button_create(box);
    lv_obj_set_height(b, 56);
    lv_obj_set_style_min_width(b, 120, 0);
    lv_obj_set_style_pad_hor(b, 20, 0);
    lv_obj_set_style_radius(b, 28, 0);
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
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (checked) lv_obj_add_state(b, LV_STATE_CHECKED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

static void check_only(lv_obj_t *box, int idx)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(box); i++) {
        lv_obj_t *c = lv_obj_get_child(box, i);
        if ((int)i == idx) lv_obj_add_state(c, LV_STATE_CHECKED);
        else lv_obj_remove_state(c, LV_STATE_CHECKED);
    }
}

static void scn_cb(lv_event_t *e)
{
    s_scn = (int)(intptr_t)lv_event_get_user_data(e);
    check_only(s_scn_box, s_scn);
}

static void speed_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_speed = SPEEDS[i];
    check_only(s_speed_box, i);
}

static void start_cb(lv_event_t *e)
{
    sensor_hub_demo_start((demo_scn_t)s_scn, s_speed);
}

static void stop_cb(lv_event_t *e)
{
    sensor_hub_demo_stop();
}

static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static void state_update(lv_timer_t *t)
{
    sensor_demo_info_t d;
    sensor_hub_demo_info(&d);
    char buf[128];
    if (d.active) {
        time_t tt = (time_t)d.unix_now;
        struct tm tm;
        localtime_r(&tt, &tm);
        char when[16];
        strftime(when, sizeof(when), "%H:%M", &tm);
        snprintf(buf, sizeof(buf), "再生中: %s ×%d（デモの時刻 %s）", SCN_LABEL[d.scn], d.speed, when);
    } else {
        snprintf(buf, sizeof(buf), "停止中（センサーの値を表示しています）");
    }
    set_text(s_state, buf);
    set_text(s_start_label, d.active ? "最初から再生" : "デモを開始");
    if (d.active) lv_obj_remove_state(s_stop, LV_STATE_DISABLED);
    else lv_obj_add_state(s_stop, LV_STATE_DISABLED);
}

static lv_obj_t *row_box(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_set_style_pad_row(r, 10, 0);
    return r;
}

static lv_obj_t *action_button(lv_obj_t *parent, const char *text, bool primary, lv_event_cb_t cb, lv_obj_t **label)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 260, 60);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, primary ? UI_COLOR_ACCENT : UI_COLOR_BG, 0);
    lv_obj_set_style_bg_color(b, UI_COLOR_BORDER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(b, primary ? 0 : 1, 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = ui_label_create(b, &font_jp_20, primary ? UI_COLOR_BG : UI_COLOR_TEXT, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (label) *label = l;
    return b;
}

static void demo_card_create(lv_obj_t *page)
{
    lv_obj_t *card = ui_card_create(page);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);
    ui_label_create(card, &font_jp_24, UI_COLOR_ACCENT, "デモ");

    ui_label_create(card, &font_jp_20, UI_COLOR_TEXT, "シナリオ");
    s_scn_box = row_box(card);
    for (int i = 0; i < DEMO_COUNT; i++) {
        seg_button(s_scn_box, SCN_LABEL[i], i == s_scn, scn_cb, i);
    }
    ui_label_create(card, &font_jp_20, UI_COLOR_TEXT, "速さ");
    s_speed_box = row_box(card);
    seg_button(s_speed_box, "×1（実時間）", true, speed_cb, 0);
    seg_button(s_speed_box, "×60（1 分 = 1 時間）", false, speed_cb, 1);

    lv_obj_t *btns = row_box(card);
    lv_obj_set_style_pad_column(btns, 16, 0);
    action_button(btns, "デモを開始", true, start_cb, &s_start_label);
    s_stop = action_button(btns, "デモを終了", false, stop_cb, NULL);
    s_state = ui_label_create(btns, &font_jp_20, UI_COLOR_SUBTEXT, "");

    lv_obj_t *note = ui_label_create(card, &font_jp_16, UI_COLOR_SUBTEXT,
                                     "開始すると、センサー画面・グラフ・アラーム（音声）がシナリオの値で動きます。直前 24 時間分の記録も入ります。"
                                     "×60 では「5 分続いたら知らせる」が 5 秒、「30 分ごと」が 30 秒になります。"
                                     "デモの値は本体の記録や SD カードには残らず、その間もセンサーの計測と記録は続きます");
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);

    state_update(NULL);
    lv_timer_create(state_update, 1000, NULL);
}

void page_demo_create(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, UI_PAD, 0);
    lv_obj_set_style_pad_bottom(page, 64, 0);
    lv_obj_set_style_pad_row(page, 16, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    demo_card_create(page);

    lv_obj_t *card = ui_card_create(page);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);
    ui_label_create(card, &font_jp_24, UI_COLOR_ACCENT, "アナウンス試聴");
    ui_label_create(card, &font_jp_16, UI_COLOR_SUBTEXT, "音声: VOICEVOX:ずんだもん");

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_style_pad_column(grid, 12, 0);
    for (int i = 0; i < SND_COUNT; i++) {
        lv_obj_t *b = lv_button_create(grid);
        lv_obj_set_size(b, 380, 64);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, UI_COLOR_BG, 0);
        lv_obj_set_style_bg_color(b, UI_COLOR_BORDER, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b, UI_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_t *l = ui_label_create(b, &font_jp_20, UI_COLOR_TEXT, "");
        lv_label_set_text_fmt(l, LV_SYMBOL_PLAY "  %s", CLIPS[i].label);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, play_cb, LV_EVENT_CLICKED, (void *)(intptr_t)CLIPS[i].s);
    }

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 24, 0);
    ui_label_create(row, &font_jp_20, UI_COLOR_TEXT, "音量");
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_width(slider, 480);
    lv_slider_set_range(slider, 0, 100);
    app_settings_t st;
    settings_get(&st);
    lv_slider_set_value(slider, st.volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, volume_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, volume_cb, LV_EVENT_RELEASED, NULL);
}
