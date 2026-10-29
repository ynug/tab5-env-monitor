#pragma once

/*
 * Fixed-size (96 byte) flash record, little endian:
 *   0 u16 magic   2 u8 type   3 u8 flags   4 u32 lid (log sequence)
 *   8 u32 boot_id 12 u32 uptime_min 16 u32 unix_min (0 = unknown)
 *  20 9 x {i16 avg, i16 min, i16 max, u8 n}   (63 bytes, data records only)
 *  83..91 reserved (0)   92 u32 crc32 of bytes 0..91
 * An ANCHOR record ties (boot_id, uptime_min) to unix_min once the clock becomes valid.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "series.h"

#define REC_SIZE    96
#define REC_MAGIC   0x4D48

typedef enum {
    REC_TYPE_DATA = 1,
    REC_TYPE_ANCHOR = 2,
} rec_type_t;

typedef struct {
    rec_type_t type;
    uint32_t lid;
    minute_rec_t m;     /* anchor: only boot_id / uptime_min / unix_min are used */
} log_rec_t;

uint32_t crc32_le(uint32_t crc, const uint8_t *p, size_t n);
void record_encode(const log_rec_t *r, uint8_t out[REC_SIZE]);
bool record_decode(const uint8_t in[REC_SIZE], log_rec_t *r);
bool record_is_blank(const uint8_t in[REC_SIZE]);
