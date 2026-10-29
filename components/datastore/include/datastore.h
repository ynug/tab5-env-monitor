#pragma once

/*
 * Owns the 24 h history (PSRAM) and its persistence in the "histlog" flash partition.
 * Minute records are written to flash as they arrive. When the clock becomes valid (RTC at
 * boot, or set by hand) an anchor record is written and the history is rebuilt on absolute time.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "history.h"
#include "series.h"

typedef struct {
    uint32_t boot_id;
    bool sim;
    bool abs_time;
    uint32_t ram_minutes;
    uint32_t flash_records, flash_capacity;
    uint32_t flash_writes, flash_erases, flash_errors;
} datastore_stats_t;

/* sim: records are tagged REC_FLAG_SIM and only SIM records are restored */
esp_err_t datastore_init(bool sim);
uint32_t datastore_boot_id(void);

void datastore_push_minute(const minute_rec_t *r);
void datastore_push_10s(const sample10_t *s);

/* Read access for the UI: hold the lock while using the history (the demo history while a demo runs) */
const history_t *datastore_lock(void);
/* Always the real history (e.g. for the SD card export) */
const history_t *datastore_lock_real(void);
void datastore_unlock(void);

/*
 * Demo: a separate RAM-only history (absolute keys) that datastore_lock() returns instead of the
 * real one until datastore_demo_end(). The real history and the flash log keep running.
 */
esp_err_t datastore_demo_begin(uint32_t now_key);
void datastore_demo_put_minute(uint32_t key, const stat_t s[S_COUNT]);
void datastore_demo_push_10s(const sample10_t *s, uint32_t now_key);
void datastore_demo_end(void);

void datastore_get_stats(datastore_stats_t *st);

/* Erase the flash log and the RAM history */
void datastore_clear(void);

/* History key of the current minute (call with the lock held). *abs_time: keys are unix minutes. */
uint32_t datastore_now_key(const history_t *h);

/* Set the system clock (by hand), re-anchor this boot and rebuild the history. Asynchronous. */
void datastore_set_clock(int64_t unix_s);

/* Wall clock considered valid (from the RTC or set by hand) */
bool clock_valid(void);
