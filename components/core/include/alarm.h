#pragma once

/*
 * Alarm state machines (pure C, see docs/SPEC.md 8).
 *
 * Each group has hysteresis levels. A level is announced once it has held for hold_s, then
 * repeated every repeat_s while the condition lasts. Rising to a higher level is announced
 * after that level has held for hold_s; falling back is not announced. While sleeping,
 * announcements are suppressed (except danger levels if allowed) and the ones still active
 * are announced once when the sleep period ends.
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AL_CO2 = 0,
    AL_HEAT,
    AL_HUM_LOW,
    AL_HUM_HIGH,
    AL_TEMP_LOW,
    AL_PRES_DROP,
    AL_COUNT
} alarm_group_t;

typedef enum {
    SND_CO2_CAUTION = 0,
    SND_CO2_WARNING,
    SND_HEAT_CAUTION,
    SND_HEAT_SEVERE,
    SND_HEAT_DANGER,
    SND_HUM_LOW,
    SND_HUM_HIGH,
    SND_TEMP_LOW,
    SND_PRES_DROP,
    SND_COUNT
} sound_t;

#define ALARM_MAX_LEVELS 3

typedef struct {
    float enter;
    float release;
    sound_t sound;
    const char *banner;     /* standard Japanese text for the screen */
    uint8_t rank;           /* banner priority, higher wins */
    bool danger;            /* may sound during sleep if allowed */
} alarm_level_def_t;

typedef struct {
    const char *name;
    bool below;             /* condition is value <= enter (else >=) */
    int nlevels;
    alarm_level_def_t lv[ALARM_MAX_LEVELS];
} alarm_group_def_t;

extern const alarm_group_def_t ALARM_DEFS[AL_COUNT];

typedef struct {
    float value[AL_COUNT];  /* CO2 ppm, WBGT, RH, RH, temperature, 3 h pressure drop [hPa] */
    bool valid[AL_COUNT];
} alarm_input_t;

typedef struct {
    bool enabled[AL_COUNT];
    bool sleeping;
    bool sleep_allow_danger;
    uint32_t hold_s;        /* 300 */
    uint32_t repeat_s;      /* 1800 */
} alarm_cfg_t;

typedef struct {
    uint8_t level;                      /* current hysteresis level, 0 = clear */
    uint8_t announced;                  /* level last announced */
    uint32_t since[ALARM_MAX_LEVELS];   /* time level >= k+1 started, 0 = not reached */
    uint32_t last_announce;
    bool suppressed;                    /* an announcement was swallowed by sleep */
} alarm_group_state_t;

typedef struct {
    alarm_group_state_t g[AL_COUNT];
    bool was_sleeping;
} alarm_t;

void alarm_init(alarm_t *a);
void alarm_cfg_default(alarm_cfg_t *cfg);

/* Evaluate (every 10 s). now_s must be monotonic and > 0. Returns the number of sounds to play. */
int alarm_eval(alarm_t *a, const alarm_cfg_t *cfg, uint32_t now_s, const alarm_input_t *in, sound_t *out, int max_out);

/* Most important active condition; false if none */
bool alarm_banner(const alarm_t *a, alarm_group_t *group, const alarm_level_def_t **def);
