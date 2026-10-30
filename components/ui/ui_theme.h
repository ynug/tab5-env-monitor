#pragma once

#include "lvgl.h"
#include "ui_fonts.h"

#define UI_COLOR_BG         lv_color_hex(0x0E1116)
#define UI_COLOR_CARD       lv_color_hex(0x1A1F27)
#define UI_COLOR_BORDER     lv_color_hex(0x262D38)
#define UI_COLOR_TEXT       lv_color_hex(0xE8ECF1)
#define UI_COLOR_SUBTEXT    lv_color_hex(0x8B95A5)
#define UI_COLOR_ACCENT     lv_color_hex(0x4FC3F7)
#define UI_COLOR_GOOD       lv_color_hex(0x4CAF50)
#define UI_COLOR_WARN       lv_color_hex(0xFFC107)
#define UI_COLOR_BAD        lv_color_hex(0xFF6B5E)
#define UI_COLOR_COOL       lv_color_hex(0x4FC3F7)   /* low temperature / humid */

#define UI_STATUS_H         56
#define UI_PAD              24

extern lv_style_t ui_style_card;

void ui_theme_init(lv_display_t *disp);

/* Card container with the shared card style */
lv_obj_t *ui_card_create(lv_obj_t *parent);
/* Label with font and color in one call */
lv_obj_t *ui_label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
/* Modal confirmation: on_ok runs when the primary button is pressed */
void ui_confirm(const char *title, const char *message, const char *ok_label, void (*on_ok)(void));
/* Modal message with a close button; returns the label so the caller can update it */
lv_obj_t *ui_message(const char *title, const char *message);
void ui_message_close(void);

/* Placeholder text for pages implemented in later steps */
void ui_placeholder(lv_obj_t *page, const char *title, const char *note);
