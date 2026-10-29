#pragma once

/*
 * CSV export of the minute history to the microSD card (see core/csvlog.h for the format).
 * Files: /sdcard/ENVLOG/YYYYMMDD.CSV (simulated data: ENVSIM). Only appended to.
 *
 * The slot has no card-detect pin: the card is probed at boot, once an hour and on request.
 * Without a card nothing is written. With the clock set, the export runs every hour (if
 * enabled in the settings) and on request; it covers the minutes of the last 24 h that are
 * not in the files yet.
 */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SDLOG_NO_CARD = 0,
    SDLOG_READY,
    SDLOG_BUSY,
} sdlog_state_t;

typedef struct {
    sdlog_state_t state;
    uint32_t total_mb, free_mb;
    int64_t last_export;        /* unix s of the last successful export, 0 = none */
    uint32_t last_lines;        /* lines written by it */
    char msg[160];              /* result of the last action, for the UI */
} sdlog_status_t;

void sdlog_start(void);
void sdlog_get_status(sdlog_status_t *out);

/* Probe the card (mount if it is new); asynchronous */
void sdlog_request_check(void);
/* Probe the card and export now; asynchronous */
void sdlog_request_export(void);
