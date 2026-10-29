#pragma once

/* User settings, persisted in NVS. Readers get a copy; writers replace the whole struct. */
#include <stdbool.h>
#include <stdint.h>
#include "alarm.h"

typedef struct {
    uint8_t brightness;             /* % */
    bool flipped;
    bool alarm_enabled[AL_COUNT];
    uint8_t volume;                 /* % */
    bool scd41_asc;                 /* SCD41 automatic self-calibration */
    bool sd_auto_export;            /* hourly CSV export while a card is inserted */
} app_settings_t;

void settings_init(void);
void settings_get(app_settings_t *out);
/* Store and persist; listeners are called afterwards */
void settings_set(const app_settings_t *s);
/* Back to factory defaults (persisted, listeners called) */
void settings_reset(void);

typedef void (*settings_listener_t)(const app_settings_t *s);
void settings_add_listener(settings_listener_t cb);
