#pragma once

#include "lvgl.h"

void page_sensor_create(lv_obj_t *page);
void page_settings_create(lv_obj_t *page);
void page_demo_create(lv_obj_t *page);

typedef enum {
    UI_GRAPH_CO2 = 0,
    UI_GRAPH_TEMP,
    UI_GRAPH_HUM,
    UI_GRAPH_PRES,
    UI_GRAPH_COUNT
} ui_graph_t;

/* Open the graph screen; "back" returns to the current screen */
void graph_open(ui_graph_t id);

/* Keep a label showing HH:MM (or --:-- while the clock is not synced) */
void ui_clock_attach(lv_obj_t *label);
