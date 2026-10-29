#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Recorded 1-minute series (see docs/SPEC.md 3.5). Order is part of the flash record format. */
typedef enum {
    S_CO2 = 0,      /* SCD41, pressure compensated [ppm] */
    S_T_SHT,        /* SHT35 [degC] */
    S_T_SCD_RAW,    /* SCD41 as reported [degC] */
    S_T_SCD,        /* SCD41 offset corrected [degC] */
    S_T_BMP,        /* BMP581 [degC] */
    S_RH_SHT,       /* SHT35 [%RH] */
    S_RH_SCD_RAW,   /* SCD41 as reported [%RH] */
    S_RH_SCD,       /* SCD41 converted to the reference temperature [%RH] */
    S_P_BMP,        /* BMP581 station pressure [hPa] */
    S_COUNT
} series_id_t;

/* One aggregated value. n == 0 means missing. */
typedef struct {
    float avg;
    float min;
    float max;
    uint8_t n;
} stat_t;

/* 10-second value */
typedef struct {
    float v;
    uint8_t n;          /* number of 1 s (or 5 s) samples, 0 = missing */
} val_t;

typedef struct {
    uint32_t t;         /* block start [s] (unix if time valid, else uptime) */
    val_t s[S_COUNT];
} sample10_t;

#define REC_FLAG_SIM    0x01    /* produced by the simulated sensor backend */

typedef struct {
    uint32_t boot_id;
    uint32_t uptime_min;    /* minute since boot at block start */
    uint32_t unix_min;      /* unix time / 60, 0 if unknown when recorded */
    uint8_t flags;
    stat_t s[S_COUNT];
} minute_rec_t;

/* Display metrics */
typedef enum { M_CO2 = 0, M_TEMP, M_HUM, M_PRES, M_COUNT } metric_id_t;

/* Which sensor provides the primary temperature / humidity */
typedef enum { SRC_NONE = 0, SRC_SHT, SRC_BMP, SRC_SCD } source_t;
