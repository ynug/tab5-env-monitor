#pragma once

/*
 * Append-only ring log of fixed-size records on raw flash.
 * Each sector holds floor(sector / REC_SIZE) slots. Entering a sector erases it (it holds the
 * oldest data). The write head is found at open() from the highest valid log id, so no
 * separate metadata is written. A torn write only invalidates that one record (CRC).
 */
#include <stdbool.h>
#include <stdint.h>
#include "record.h"

typedef struct {
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t len);
    int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t len);
    int (*erase)(void *ctx, uint32_t off, uint32_t len);
    void *ctx;
    uint32_t size;          /* bytes, multiple of sector */
    uint32_t sector;        /* erase unit, bytes */
} flash_ops_t;

typedef struct {
    flash_ops_t ops;
    uint32_t sps;           /* slots per sector */
    uint32_t sectors;
    uint32_t head;          /* next slot to write */
    uint32_t next_lid;
    uint32_t count;         /* valid records currently stored */
    uint32_t writes, erases, errors;
} histlog_t;

typedef bool (*histlog_cb_t)(const log_rec_t *r, void *arg);   /* return false to stop */

int histlog_open(histlog_t *log, const flash_ops_t *ops);
int histlog_append(histlog_t *log, log_rec_t *r);              /* assigns r->lid */
int histlog_foreach(histlog_t *log, histlog_cb_t cb, void *arg); /* oldest -> newest */
int histlog_clear(histlog_t *log);
uint32_t histlog_capacity(const histlog_t *log);
