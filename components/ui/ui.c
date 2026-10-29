#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "controller.h"
#include "datastore.h"
#include "pages.h"
#include "sensor_hub.h"
#include "ui_debug.h"
#include "ui_theme.h"

#define PAGE_COUNT  3
#define PAGE_HOME   0
#define DOT_SIZE    10

typedef struct {
    const char *title;
    void (*create)(lv_obj_t *page);
} page_def_t;

static const page_def_t PAGES[PAGE_COUNT] = {
    {"センサー", page_sensor_create},
    {"設定", page_settings_create},
    {"デモ", page_demo_create},
};

static lv_obj_t *s_title;
static lv_obj_t *s_banner, *s_banner_dot, *s_banner_text;
static lv_obj_t *s_demo, *s_demo_text;
static lv_obj_t *s_clocks[2];
static int s_nclocks;
static lv_obj_t *s_dots[PAGE_COUNT];

static void set_active_page(int idx)
{
    lv_label_set_text(s_title, PAGES[idx].title);
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_set_style_bg_color(s_dots[i], i == idx ? UI_COLOR_ACCENT : UI_COLOR_BORDER, 0);
    }
}

static void tile_changed_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target_obj(e);
    lv_obj_t *tile = lv_tileview_get_tile_active(tv);
    set_active_page((int)(intptr_t)lv_obj_get_user_data(tile));
}

static void clock_update(lv_timer_t *t)
{
    char buf[8] = "--:--";
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (clock_valid()) {
        strftime(buf, sizeof(buf), "%H:%M", &tm);
    }
    for (int i = 0; i < s_nclocks; i++) {
        if (strcmp(lv_label_get_text(s_clocks[i]), buf) != 0) {
            lv_label_set_text(s_clocks[i], buf);
        }
    }
}

void ui_clock_attach(lv_obj_t *label)
{
    if (s_nclocks < (int)(sizeof(s_clocks) / sizeof(s_clocks[0]))) {
        s_clocks[s_nclocks++] = label;
        clock_update(NULL);
    }
}

static void banner_update(lv_timer_t *t)
{
    controller_banner_t b;
    controller_get_banner(&b);
    if (b.active) {
        lv_color_t c = b.def->rank >= 5 ? UI_COLOR_BAD : b.def->rank >= 3 ? UI_COLOR_WARN : UI_COLOR_ACCENT;
        lv_obj_set_style_bg_color(s_banner_dot, c, 0);
        if (strcmp(lv_label_get_text(s_banner_text), b.def->banner) != 0) {
            lv_label_set_text(s_banner_text, b.def->banner);
        }
        lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    }

    sensor_demo_info_t d;
    sensor_hub_demo_info(&d);
    if (d.active) {
        time_t tt = (time_t)d.unix_now;
        struct tm tm;
        localtime_r(&tt, &tm);
        char buf[32];
        snprintf(buf, sizeof(buf), "DEMO ×%d  %02d:%02d", d.speed, tm.tm_hour, tm.tm_min);
        if (strcmp(lv_label_get_text(s_demo_text), buf) != 0) {
            lv_label_set_text(s_demo_text, buf);
        }
        lv_obj_remove_flag(s_demo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_demo, LV_OBJ_FLAG_HIDDEN);
    }
}

static void status_bar_create(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), UI_STATUS_H);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_hor(bar, UI_PAD, 0);

    s_title = ui_label_create(bar, &font_jp_24, UI_COLOR_TEXT, "");
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 0, 0);

    /* alarm banner: status dot + standard Japanese text (colour never carries the meaning alone) */
    s_banner = lv_obj_create(bar);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_size(s_banner, 820, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_banner, 10, 0);
    lv_obj_align(s_banner, LV_ALIGN_LEFT_MID, 170, 0);
    s_banner_dot = lv_obj_create(s_banner);
    lv_obj_remove_style_all(s_banner_dot);
    lv_obj_set_size(s_banner_dot, 14, 14);
    lv_obj_set_style_radius(s_banner_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_banner_dot, LV_OPA_COVER, 0);
    s_banner_text = ui_label_create(s_banner, &font_jp_20, UI_COLOR_TEXT, "");
    lv_obj_set_width(s_banner_text, 790);
    lv_label_set_long_mode(s_banner_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    /* demo badge: the demo clock runs up to 60x, the status clock stays real */
    s_demo = lv_obj_create(bar);
    lv_obj_remove_style_all(s_demo);
    lv_obj_set_size(s_demo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_demo, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_demo, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_demo, 6, 0);
    lv_obj_set_style_pad_hor(s_demo, 10, 0);
    lv_obj_set_style_pad_ver(s_demo, 4, 0);
    lv_obj_align(s_demo, LV_ALIGN_RIGHT_MID, -100, 0);
    s_demo_text = ui_label_create(s_demo, &font_jp_20, UI_COLOR_BG, "");
    lv_obj_add_flag(s_demo, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *clock = ui_label_create(bar, &font_jp_24, UI_COLOR_TEXT, "--:--");
    lv_obj_align(clock, LV_ALIGN_RIGHT_MID, 0, 0);
    ui_clock_attach(clock);
    lv_timer_create(clock_update, 1000, NULL);
    banner_update(NULL);
    lv_timer_create(banner_update, 1000, NULL);
}

static void page_dots_create(lv_obj_t *scr)
{
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(box, 12, 0);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(box);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        s_dots[i] = d;
    }
}

void ui_init(void)
{
    lv_lock();

    lv_display_t *disp = lv_display_get_default();
    ui_theme_init(disp);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    status_bar_create(scr);

    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_size(tv, LV_PCT(100), lv_display_get_vertical_resolution(disp) - UI_STATUS_H);
    lv_obj_set_pos(tv, 0, UI_STATUS_H);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_dir_t dir = (i > 0 ? LV_DIR_LEFT : 0) | (i < PAGE_COUNT - 1 ? LV_DIR_RIGHT : 0);
        lv_obj_t *tile = lv_tileview_add_tile(tv, i, 0, dir);
        lv_obj_set_user_data(tile, (void *)(intptr_t)i);
        PAGES[i].create(tile);
    }
    lv_obj_add_event_cb(tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    page_dots_create(scr);
    lv_tileview_set_tile_by_index(tv, PAGE_HOME, 0, LV_ANIM_OFF);
    set_active_page(PAGE_HOME);
    ui_debug_init();

    lv_unlock();
}
