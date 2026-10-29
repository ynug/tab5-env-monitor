#include "histlog.h"

#include <string.h>

static uint32_t slot_off(const histlog_t *log, uint32_t slot)
{
    return (slot / log->sps) * log->ops.sector + (slot % log->sps) * REC_SIZE;
}

static int read_slot(histlog_t *log, uint32_t slot, uint8_t buf[REC_SIZE])
{
    return log->ops.read(log->ops.ctx, slot_off(log, slot), buf, REC_SIZE);
}

uint32_t histlog_capacity(const histlog_t *log)
{
    return log->sps * log->sectors;
}

int histlog_open(histlog_t *log, const flash_ops_t *ops)
{
    memset(log, 0, sizeof(*log));
    log->ops = *ops;
    log->sps = ops->sector / REC_SIZE;
    log->sectors = ops->size / ops->sector;
    if (log->sps == 0 || log->sectors < 2) {
        return -1;
    }

    const uint32_t cap = histlog_capacity(log);
    uint8_t buf[REC_SIZE];
    log_rec_t r;
    bool found = false;
    uint32_t max_lid = 0, max_slot = 0;

    for (uint32_t s = 0; s < cap; s++) {
        if (read_slot(log, s, buf) != 0) {
            return -1;
        }
        if (record_decode(buf, &r)) {
            log->count++;
            if (!found || (int32_t)(r.lid - max_lid) > 0) {
                found = true;
                max_lid = r.lid;
                max_slot = s;
            }
        }
    }

    if (!found) {
        log->head = 0;
        log->next_lid = 1;
        return 0;
    }
    log->next_lid = max_lid + 1;
    log->head = (max_slot + 1) % cap;
    /* Skip slots dirtied by a torn write; a sector start gets erased on use anyway */
    while (log->head % log->sps != 0) {
        if (read_slot(log, log->head, buf) != 0) {
            return -1;
        }
        if (record_is_blank(buf)) {
            break;
        }
        log->head = (log->head + 1) % cap;
    }
    return 0;
}

static int erase_sector(histlog_t *log, uint32_t sector)
{
    uint8_t buf[REC_SIZE];
    log_rec_t r;
    for (uint32_t i = 0; i < log->sps; i++) {
        if (read_slot(log, sector * log->sps + i, buf) == 0 && record_decode(buf, &r) && log->count) {
            log->count--;
        }
    }
    log->erases++;
    return log->ops.erase(log->ops.ctx, sector * log->ops.sector, log->ops.sector);
}

int histlog_append(histlog_t *log, log_rec_t *r)
{
    const uint32_t cap = histlog_capacity(log);
    uint8_t buf[REC_SIZE], chk[REC_SIZE];

    for (int attempt = 0; attempt < 2; attempt++) {
        if (log->head % log->sps == 0 && erase_sector(log, log->head / log->sps) != 0) {
            log->errors++;
            return -1;
        }
        r->lid = log->next_lid;
        record_encode(r, buf);
        uint32_t slot = log->head;
        log->head = (log->head + 1) % cap;
        if (log->ops.write(log->ops.ctx, slot_off(log, slot), buf, REC_SIZE) == 0 &&
            read_slot(log, slot, chk) == 0 && memcmp(buf, chk, REC_SIZE) == 0) {
            log->next_lid++;
            log->count++;
            log->writes++;
            return 0;
        }
        log->errors++;  /* slot is unusable until its sector is erased again; try the next one */
    }
    return -1;
}

int histlog_foreach(histlog_t *log, histlog_cb_t cb, void *arg)
{
    uint8_t buf[REC_SIZE];
    log_rec_t r;
    const uint32_t head_sector = log->head / log->sps;
    /* Oldest data: the head sector itself if it has not been entered yet, else the next one */
    const uint32_t first = (log->head % log->sps == 0) ? head_sector : (head_sector + 1) % log->sectors;

    for (uint32_t k = 0; k < log->sectors; k++) {
        uint32_t sector = (first + k) % log->sectors;
        for (uint32_t i = 0; i < log->sps; i++) {
            if (read_slot(log, sector * log->sps + i, buf) != 0) {
                return -1;
            }
            if (record_decode(buf, &r) && !cb(&r, arg)) {
                return 0;
            }
        }
    }
    return 0;
}

int histlog_clear(histlog_t *log)
{
    for (uint32_t s = 0; s < log->sectors; s++) {
        if (log->ops.erase(log->ops.ctx, s * log->ops.sector, log->ops.sector) != 0) {
            return -1;
        }
    }
    log->head = 0;
    log->count = 0;
    log->erases += log->sectors;
    return 0;
}
