#include "history.h"

#include <math.h>
#include <string.h>

void history_init(history_t *h, hist_entry_t *min_buf, sample10_t *s10_buf, uint32_t boot_id, uint8_t flags_mode)
{
    memset(h, 0, sizeof(*h));
    h->min = min_buf;
    h->s10 = s10_buf;
    h->boot_id = boot_id;
    h->flags_mode = flags_mode;
    memset(h->min, 0, sizeof(hist_entry_t) * HIST_MINUTES);
}

static void put(history_t *h, uint32_t key, const stat_t s[S_COUNT])
{
    if (h->have && (int32_t)(h->latest_key - key) >= HIST_MINUTES) {
        return;     /* older than the window */
    }
    hist_entry_t *e = &h->min[key % HIST_MINUTES];
    e->key = key;
    e->used = true;
    memcpy(e->s, s, sizeof(e->s));
    if (!h->have || (int32_t)(key - h->latest_key) > 0) {
        h->latest_key = key;
        h->have = true;
    }
}

void history_add_minute(history_t *h, const minute_rec_t *r)
{
    uint32_t key;
    if (h->abs_time) {
        if (!r->unix_min) {
            return;     /* cannot place; the rebuild after the anchor will pick it up */
        }
        key = r->unix_min;
    } else {
        key = h->rel_base + r->uptime_min;
    }
    put(h, key, r->s);
}

void history_add_10s(history_t *h, const sample10_t *s)
{
    h->s10[h->s10_head] = *s;
    h->s10_head = (h->s10_head + 1) % HIST_10S;
    if (h->s10_count < HIST_10S) {
        h->s10_count++;
    }
}

float history_primary_temp(const hist_entry_t *e)
{
    if (e->s[S_T_SHT].n) return e->s[S_T_SHT].avg;
    if (e->s[S_T_BMP].n) return e->s[S_T_BMP].avg;
    if (e->s[S_T_SCD].n) return e->s[S_T_SCD].avg;
    return NAN;
}

float history_primary_rh(const hist_entry_t *e)
{
    if (e->s[S_RH_SHT].n) return e->s[S_RH_SHT].avg;
    if (e->s[S_RH_SCD].n) return e->s[S_RH_SCD].avg;
    return NAN;
}

const hist_entry_t *history_get(const history_t *h, uint32_t key)
{
    if (!h->have || (int32_t)(h->latest_key - key) >= HIST_MINUTES || (int32_t)(key - h->latest_key) > 0) {
        return NULL;
    }
    const hist_entry_t *e = &h->min[key % HIST_MINUTES];
    return (e->used && e->key == key) ? e : NULL;
}

uint32_t history_count(const history_t *h)
{
    uint32_t n = 0;
    if (!h->have) {
        return 0;
    }
    for (uint32_t i = 0; i < HIST_MINUTES; i++) {
        if (history_get(h, h->latest_key - i)) {
            n++;
        }
    }
    return n;
}

/* ---------- rebuild ---------- */

static hist_boot_t *boot_find(hist_rebuild_t *rb, uint32_t boot_id, bool create)
{
    for (int i = 0; i < rb->nboots; i++) {
        if (rb->boots[i].boot_id == boot_id) {
            return &rb->boots[i];
        }
    }
    if (!create) {
        return NULL;
    }
    if (rb->nboots == HIST_MAX_BOOTS) {
        /* drop the oldest boot */
        memmove(&rb->boots[0], &rb->boots[1], sizeof(hist_boot_t) * (HIST_MAX_BOOTS - 1));
        rb->nboots--;
    }
    hist_boot_t *b = &rb->boots[rb->nboots++];
    memset(b, 0, sizeof(*b));
    b->boot_id = boot_id;
    return b;
}

void history_rebuild_begin(hist_rebuild_t *rb, history_t *h, uint32_t now_unix_min, uint32_t cur_uptime_min)
{
    memset(rb, 0, sizeof(*rb));
    rb->h = h;
    rb->now_unix_min = now_unix_min;
    rb->cur_uptime_min = cur_uptime_min;
}

bool history_rebuild_feed(const log_rec_t *r, void *arg)
{
    hist_rebuild_t *rb = arg;
    history_t *h = rb->h;

    if (rb->pass == 0) {
        if (r->type == REC_TYPE_ANCHOR) {
            hist_boot_t *b = boot_find(rb, r->m.boot_id, true);
            b->has_anchor = true;
            b->anchor_uptime = r->m.uptime_min;
            b->anchor_unix = r->m.unix_min;
        } else if ((r->m.flags & REC_FLAG_SIM) == h->flags_mode) {
            hist_boot_t *b = boot_find(rb, r->m.boot_id, true);
            if (!b->has_data) {
                b->has_data = true;
                b->first_uptime = r->m.uptime_min;
            }
            b->last_uptime = r->m.uptime_min;
        }
        return true;
    }

    if (r->type != REC_TYPE_DATA || (r->m.flags & REC_FLAG_SIM) != h->flags_mode) {
        return true;
    }
    hist_boot_t *b = boot_find(rb, r->m.boot_id, false);
    uint32_t key;
    if (h->abs_time) {
        if (r->m.unix_min) {
            key = r->m.unix_min;
        } else if (b && b->has_anchor) {
            key = b->anchor_unix - (b->anchor_uptime - r->m.uptime_min);
        } else {
            return true;    /* boot never had a valid clock: cannot be placed */
        }
        if ((int32_t)(rb->now_unix_min - key) >= HIST_MINUTES || (int32_t)(key - rb->now_unix_min) > 0) {
            return true;
        }
    } else {
        if (!b) {
            return true;
        }
        key = b->rel_base + r->m.uptime_min;
    }
    put(h, key, r->m.s);
    return true;
}

void history_rebuild_next_pass(hist_rebuild_t *rb)
{
    history_t *h = rb->h;
    rb->pass = 1;
    h->abs_time = rb->now_unix_min != 0;
    h->have = false;
    memset(h->min, 0, sizeof(hist_entry_t) * HIST_MINUTES);

    if (h->abs_time) {
        h->rel_base = 0;
        return;
    }
    /* relative: lay boots end to end in log order, current boot last */
    uint32_t next = HIST_MINUTES;   /* keep keys well above 0 */
    bool cur_seen = false;
    for (int i = 0; i < rb->nboots; i++) {
        hist_boot_t *b = &rb->boots[i];
        if (!b->has_data) {
            continue;
        }
        b->rel_base = next - b->first_uptime;
        next += (b->last_uptime - b->first_uptime) + 2;
        if (b->boot_id == h->boot_id) {
            cur_seen = true;
            h->rel_base = b->rel_base;
        }
    }
    if (!cur_seen) {
        h->rel_base = next - rb->cur_uptime_min;
    }
}

void history_rebuild_end(hist_rebuild_t *rb)
{
    history_t *h = rb->h;
    if (!h->have) {
        /* nothing restored: start the window at the present */
        h->latest_key = h->abs_time ? rb->now_unix_min : h->rel_base + rb->cur_uptime_min;
    }
}
