#include "ui_theme.h"

lv_style_t ui_style_card;

void ui_theme_init(lv_display_t *disp)
{
    lv_theme_t *th = lv_theme_default_init(disp, UI_COLOR_ACCENT, UI_COLOR_GOOD, true, &font_jp_20);
    lv_display_set_theme(disp, th);

    lv_style_init(&ui_style_card);
    lv_style_set_bg_color(&ui_style_card, UI_COLOR_CARD);
    lv_style_set_bg_opa(&ui_style_card, LV_OPA_COVER);
    lv_style_set_border_color(&ui_style_card, UI_COLOR_BORDER);
    lv_style_set_border_width(&ui_style_card, 1);
    lv_style_set_radius(&ui_style_card, 16);
    lv_style_set_pad_all(&ui_style_card, 20);
    lv_style_set_shadow_width(&ui_style_card, 0);
}

lv_obj_t *ui_card_create(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &ui_style_card, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *ui_label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

void ui_placeholder(lv_obj_t *page, const char *title, const char *note)
{
    lv_obj_t *t = ui_label_create(page, &font_jp_32, UI_COLOR_TEXT, title);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, UI_PAD, UI_PAD);
    lv_obj_t *n = ui_label_create(page, &font_jp_20, UI_COLOR_SUBTEXT, note);
    lv_obj_align_to(n, t, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
}

/* ---------- modal dialogs ---------- */

static lv_obj_t *s_modal;
static void (*s_modal_ok)(void);

void ui_message_close(void)
{
    if (s_modal) {
        lv_obj_delete(s_modal);
        s_modal = NULL;
    }
}

static void modal_btn_cb(lv_event_t *e)
{
    void (*ok)(void) = (intptr_t)lv_event_get_user_data(e) ? s_modal_ok : NULL;
    ui_message_close();
    if (ok) {
        ok();
    }
}

static lv_obj_t *modal_button(lv_obj_t *parent, const char *text, bool primary)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 200, 56);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, primary ? UI_COLOR_ACCENT : UI_COLOR_BG, 0);
    lv_obj_set_style_border_color(b, UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(b, primary ? 0 : 1, 0);
    lv_obj_t *l = ui_label_create(b, &font_jp_20, primary ? UI_COLOR_BG : UI_COLOR_TEXT, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, modal_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)primary);
    return b;
}

static lv_obj_t *modal_create(const char *title, const char *message, lv_obj_t **msg_out)
{
    ui_message_close();
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, 0);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *box = ui_card_create(s_modal);
    lv_obj_set_size(box, 720, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 28, 0);
    lv_obj_set_style_pad_row(box, 20, 0);
    ui_label_create(box, &font_jp_24, UI_COLOR_TEXT, title);
    lv_obj_t *m = ui_label_create(box, &font_jp_20, UI_COLOR_SUBTEXT, message);
    lv_obj_set_width(m, LV_PCT(100));
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
    if (msg_out) {
        *msg_out = m;
    }

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    return row;
}

void ui_confirm(const char *title, const char *message, const char *ok_label, void (*on_ok)(void))
{
    lv_obj_t *row = modal_create(title, message, NULL);
    s_modal_ok = on_ok;
    modal_button(row, "キャンセル", false);
    modal_button(row, ok_label, true);
}

lv_obj_t *ui_message(const char *title, const char *message)
{
    lv_obj_t *msg;
    lv_obj_t *row = modal_create(title, message, &msg);
    s_modal_ok = NULL;
    modal_button(row, "閉じる", false);
    return msg;
}
