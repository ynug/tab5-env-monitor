#pragma once

/*
 * In-RAM history: 24 h of 1-minute entries and 1 h of 10-second values.
 *
 * Minute entries are indexed by a key in a circular array (slot = key % HIST_MINUTES):
 *  - absolute mode: key = unix minute (clock valid)
 *  - relative mode: key = rel_base + uptime minute; boots are laid end to end with a
 *    1-minute gap because the real downtime is unknown
 * A key that is not present is a gap in the graph.
 */
#include <stdbool.h>
#include <stdint.h>
#include "record.h"
#include "series.h"

#define HIST_MINUTES    1440
#define HIST_10S        360
#define HIST_MAX_BOOTS  256

typedef struct {
    uint32_t key;
    bool used;
    stat_t s[S_COUNT];
} hist_entry_t;

typedef struct {
    hist_entry_t *min;          /* [HIST_MINUTES] */
    sample10_t *s10;            /* [HIST_10S] */
    uint32_t s10_head, s10_count;

    bool have;
    uint32_t latest_key;
    bool abs_time;
    uint32_t rel_base;          /* relative mode: key = rel_base + uptime_min */
    uint32_t boot_id;
    uint8_t flags_mode;         /* records with other REC_FLAG_SIM are ignored */
} history_t;

void history_init(history_t *h, hist_entry_t *min_buf, sample10_t *s10_buf, uint32_t boot_id, uint8_t flags_mode);

/* Live data */
void history_add_minute(history_t *h, const minute_rec_t *r);
void history_add_10s(history_t *h, const sample10_t *s);

/* Primary temperature / humidity of an entry (SHT35, else the fallbacks); NAN if none */
float history_primary_temp(const hist_entry_t *e);
float history_primary_rh(const hist_entry_t *e);

/* Lookup; NULL for a gap */
const hist_entry_t *history_get(const history_t *h, uint32_t key);
uint32_t history_count(const history_t *h);

/*
 * Rebuild from the flash log (oldest -> newest), e.g. at boot and when the clock becomes valid.
 * now_unix_min: current unix minute or 0 if unknown. cur_uptime_min: this boot's uptime minute.
 * The caller provides foreach(), which must call history_rebuild_feed() for each record, twice:
 * pass 0 collects anchors / boot ranges, pass 1 places data.
 */
typedef struct {
    uint32_t boot_id;
    bool has_anchor;
    uint32_t anchor_uptime, anchor_unix;
    bool has_data;
    uint32_t first_uptime, last_uptime;
    uint32_t rel_base;
} hist_boot_t;

typedef struct {
    history_t *h;
    int pass;
    uint32_t now_unix_min;
    uint32_t cur_uptime_min;
    hist_boot_t boots[HIST_MAX_BOOTS];
    int nboots;
} hist_rebuild_t;

void history_rebuild_begin(hist_rebuild_t *rb, history_t *h, uint32_t now_unix_min, uint32_t cur_uptime_min);
bool history_rebuild_feed(const log_rec_t *r, void *rb);   /* histlog_cb_t compatible */
void history_rebuild_next_pass(hist_rebuild_t *rb);
void history_rebuild_end(hist_rebuild_t *rb);
