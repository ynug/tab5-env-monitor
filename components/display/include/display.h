#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

/* Logical (landscape) resolution seen by LVGL */
#define DISPLAY_W 1280
#define DISPLAY_H 720

/*
 * Panel, LVGL and touch bring-up. Starts the LVGL task.
 * Other tasks must wrap LVGL calls with lv_lock()/lv_unlock().
 */
esp_err_t display_init(void);

void display_set_brightness(int percent);
int display_get_brightness(void);

/* Rotate the UI by 180 degrees (landscape, either side up) */
void display_set_flipped(bool flipped);
bool display_is_flipped(void);

/* Called on every touch press (LVGL task). Return true to swallow the touch, e.g. to only wake the screen. */
typedef bool (*display_touch_hook_t)(void);
void display_set_touch_hook(display_touch_hook_t hook);
/* For other input devices: call when a press starts; true = swallow this press */
bool display_touch_began(void);
