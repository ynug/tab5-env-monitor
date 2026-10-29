#pragma once

/*
 * Sensor processing pipeline (pure C, no ESP-IDF dependency).
 *
 * Fed once per second with raw readings. Produces:
 *  - validated + outlier-filtered samples
 *  - 10 s values (mean of accepted samples) incl. SCD41 corrections
 *  - 1 min records (mean / min / max of the 10 s values, missing if < 3 valid)
 *  - smoothed display values (adaptive One Euro filter), independent from the records
 */
#include <stdbool.h>
#include <stdint.h>
#include "series.h"

typedef struct {
    bool sht_ok;
    float sht_t, sht_rh;
    bool bmp_ok;
    float bmp_p, bmp_t;
    bool scd_new;       /* a fresh SCD41 measurement arrived this tick */
    float scd_co2, scd_t, scd_rh;
} raw_tick_t;

typedef enum { RAW_SHT_T = 0, RAW_SHT_RH, RAW_BMP_P, RAW_BMP_T, RAW_SCD_CO2, RAW_SCD_T, RAW_SCD_RH, RAW_COUNT } raw_ch_t;

#define HAMPEL_WIN 5

typedef struct {
    float buf[HAMPEL_WIN];
    uint8_t n;
    uint8_t pos;
    float floor;        /* minimum rejection threshold (absolute units) */
} hampel_t;

typedef struct {
    float min_cutoff;   /* Hz */
    float beta;
    float d_cutoff;     /* Hz */
    float x, dx;
    bool init;
} one_euro_t;

typedef struct {
    float value[M_COUNT];
    bool valid[M_COUNT];
    source_t t_src;
    source_t rh_src;
} display_vals_t;

typedef struct {
    uint32_t rejected_range[RAW_COUNT];
    uint32_t rejected_outlier[RAW_COUNT];
    uint32_t blocks;
    uint32_t minutes;
} pipeline_stats_t;

#define PIPE_EV_10S 0x01
#define PIPE_EV_MIN 0x02

typedef struct {
    hampel_t hampel[RAW_COUNT];

    /* current 10 s block */
    bool blk_active;
    uint32_t blk_index;         /* t / 10 */
    uint32_t blk_uptime;        /* uptime [s] at block start */
    bool blk_time_valid;
    double blk_sum[RAW_COUNT];
    uint8_t blk_n[RAW_COUNT];

    /* current minute */
    bool min_active;
    uint32_t min_index;         /* t / 60 */
    uint32_t min_uptime;        /* uptime [s] at minute start */
    bool min_time_valid;
    double min_sum[S_COUNT];
    float min_lo[S_COUNT], min_hi[S_COUNT];
    uint8_t min_n[S_COUNT];

    /* SCD41 self-heating offset learned against SHT35 */
    bool offset_known;
    float scd_t_offset;

    /* display */
    one_euro_t filt[M_COUNT];
    uint32_t last_update[M_COUNT];  /* uptime [s] of last accepted input */
    display_vals_t disp;

    uint32_t boot_id;
    uint8_t rec_flags;

    sample10_t last10;
    minute_rec_t last_min;
    pipeline_stats_t stats;
} pipeline_t;

void pipeline_init(pipeline_t *p, uint32_t boot_id, uint8_t rec_flags);

/* Seed the SCD41 temperature offset (e.g. from NVS) */
void pipeline_set_scd_offset(pipeline_t *p, float offset);

/*
 * One tick per second.
 *  t           current time [s]: unix time if time_valid, else uptime
 *  uptime_s    seconds since boot
 * Returns PIPE_EV_* flags; results in p->last10 / p->last_min.
 */
uint32_t pipeline_tick(pipeline_t *p, uint32_t t, bool time_valid, uint32_t uptime_s, const raw_tick_t *raw);

/* Close the running minute (e.g. before a clock jump). Returns PIPE_EV_MIN if a record was produced. */
uint32_t pipeline_flush_minute(pipeline_t *p);

/* Exposed for tests */
void hampel_init(hampel_t *h, float floor);
bool hampel_accept(hampel_t *h, float x);
void one_euro_init(one_euro_t *f, float min_cutoff, float beta, float d_cutoff);
float one_euro_update(one_euro_t *f, float x, float dt);
