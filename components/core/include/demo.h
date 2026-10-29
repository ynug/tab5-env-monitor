#pragma once

/*
 * Demo scenarios: indoor values as a function of time, deterministic (noise from a hash of
 * the time), so the 24 h before the start can be filled in at once.
 *
 * s:    demo seconds since the scenario start (negative = the history before it)
 * tod0: local time of day [s] at the start; the normal daily rhythm follows it
 * Every scenario is the normal day until the start; its event begins at s = 0.
 */
#include <stdint.h>
#include "series.h"

typedef enum {
    DEMO_NORMAL = 0,    /* daily rhythm only */
    DEMO_CO2,           /* meeting room: CO2 climbs to ~1900 ppm, ventilation after 100 min */
    DEMO_HEAT,          /* heat wave: 34.5 degC / 65 % within an hour (WBGT danger), cools after 3 h */
    DEMO_DRY,           /* winter: 14.5 degC / 25 % within 30 min, recovers after 2.5 h */
    DEMO_PRES,          /* typhoon: -2 hPa/h for 4 h, recovers after 10 h */
    DEMO_COUNT
} demo_scn_t;

typedef struct {
    float co2, t, rh, p;
} demo_vals_t;

void demo_eval(demo_scn_t scn, int32_t s, int32_t tod0, demo_vals_t *out);

/* All recorded series of one minute starting at s (6 samples: avg / min / max) */
void demo_minute(demo_scn_t scn, int32_t s, int32_t tod0, stat_t out[S_COUNT]);

/* 10 s value of the block starting at s */
void demo_sample10(demo_scn_t scn, int32_t s, int32_t tod0, sample10_t *out);
