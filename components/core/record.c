#include "record.h"

#include <math.h>
#include <string.h>

#define DATA_OFS    20
#define CRC_OFS     92

/* Fixed-point scale per series; the stored value is round(x * scale) as int16 */
static const float SCALE[S_COUNT] = {
    [S_CO2] = 1.0f,
    [S_T_SHT] = 100.0f, [S_T_SCD_RAW] = 100.0f, [S_T_SCD] = 100.0f, [S_T_BMP] = 100.0f,
    [S_RH_SHT] = 100.0f, [S_RH_SCD_RAW] = 100.0f, [S_RH_SCD] = 100.0f,
    [S_P_BMP] = 20.0f,
};

uint32_t crc32_le(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void put16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
static void put32(uint8_t *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static uint16_t get16(const uint8_t *b) { return b[0] | (b[1] << 8); }
static uint32_t get32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }

static int16_t to_fixed(float x, float scale)
{
    float v = roundf(x * scale);
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32767.0f) v = -32767.0f;
    return (int16_t)v;
}

void record_encode(const log_rec_t *r, uint8_t out[REC_SIZE])
{
    memset(out, 0, REC_SIZE);
    put16(out + 0, REC_MAGIC);
    out[2] = (uint8_t)r->type;
    out[3] = r->m.flags;
    put32(out + 4, r->lid);
    put32(out + 8, r->m.boot_id);
    put32(out + 12, r->m.uptime_min);
    put32(out + 16, r->m.unix_min);
    if (r->type == REC_TYPE_DATA) {
        uint8_t *d = out + DATA_OFS;
        for (int i = 0; i < S_COUNT; i++, d += 7) {
            const stat_t *s = &r->m.s[i];
            if (s->n) {
                put16(d + 0, (uint16_t)to_fixed(s->avg, SCALE[i]));
                put16(d + 2, (uint16_t)to_fixed(s->min, SCALE[i]));
                put16(d + 4, (uint16_t)to_fixed(s->max, SCALE[i]));
            }
            d[6] = s->n;
        }
    }
    put32(out + CRC_OFS, crc32_le(0, out, CRC_OFS));
}

bool record_decode(const uint8_t in[REC_SIZE], log_rec_t *r)
{
    if (get16(in) != REC_MAGIC || get32(in + CRC_OFS) != crc32_le(0, in, CRC_OFS)) {
        return false;
    }
    if (in[2] != REC_TYPE_DATA && in[2] != REC_TYPE_ANCHOR) {
        return false;
    }
    memset(r, 0, sizeof(*r));
    r->type = (rec_type_t)in[2];
    r->m.flags = in[3];
    r->lid = get32(in + 4);
    r->m.boot_id = get32(in + 8);
    r->m.uptime_min = get32(in + 12);
    r->m.unix_min = get32(in + 16);
    if (r->type == REC_TYPE_DATA) {
        const uint8_t *d = in + DATA_OFS;
        for (int i = 0; i < S_COUNT; i++, d += 7) {
            stat_t *s = &r->m.s[i];
            s->n = d[6];
            if (s->n) {
                s->avg = (int16_t)get16(d + 0) / SCALE[i];
                s->min = (int16_t)get16(d + 2) / SCALE[i];
                s->max = (int16_t)get16(d + 4) / SCALE[i];
            } else {
                s->avg = s->min = s->max = NAN;
            }
        }
    }
    return true;
}

bool record_is_blank(const uint8_t in[REC_SIZE])
{
    for (int i = 0; i < REC_SIZE; i++) {
        if (in[i] != 0xFF) {
            return false;
        }
    }
    return true;
}
