#pragma once

/*
 * Glue between sensors, alarms, audio and the screen brightness.
 */
#include <stdbool.h>
#include <stddef.h>
#include "alarm.h"
#include "esp_err.h"

esp_err_t controller_start(void);

typedef struct {
    bool active;
    alarm_group_t group;
    const alarm_level_def_t *def;
} controller_banner_t;

void controller_get_banner(controller_banner_t *out);

/* Re-apply brightness after a settings change or preview */
void controller_refresh_backlight(void);
