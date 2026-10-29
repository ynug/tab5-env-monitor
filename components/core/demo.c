#include "demo.h"

#include <math.h>
#include <string.h>

#define PI_F    3.14159265f
#define MIN_S   60
#define HOUR_S  3600

/* 0..1, eased, from start over dur seconds */
static float ramp(int32_t s, int32_t start, int32_t dur)
{
    float x = (float)(s - start) / (float)dur;
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    return x * x * (3 - 2 * x);
}

/* -1..1 from the time and a channel number */
static float noise(int32_t s, uint32_t ch)
{
    uint32_t h = (uint32_t)s * 2654435761u ^ (ch + 1) * 0x9E3779B9u;
    h ^= h >> 15;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return (float)(h & 0xFFFF) / 32767.5f - 1.0f;
}

static float bump(float h, float center, float width)
{
    float d = fabsf(h - center);
    if (d > 12) d = 24 - d;     /* wrap around midnight */
    return expf(-(d / width) * (d / width));
}

void demo_eval(demo_scn_t scn, int32_t s, int32_t tod0, demo_vals_t *out)
{
    int32_t tod = (tod0 + s) % 86400;
    if (tod < 0) tod += 86400;
    const float h = (float)tod / HOUR_S;
    const float day = sinf(2 * PI_F * (h - 10) / 24);      /* peak at 16:00 */

    float t = 23.0f + 1.5f * day;
    float rh = 50.0f - 6.0f * day;
    float co2 = 480.0f + 380.0f * (bump(h, 8.0f, 1.5f) + bump(h, 21.0f, 2.5f));
    float p = 1012.0f + 1.0f * sinf(2 * PI_F * h / 12) + 0.8f * sinf(2 * PI_F * (float)s / (3 * 86400));

    switch (scn) {
    case DEMO_CO2: {
        float e = ramp(s, 0, 40 * MIN_S) * (1 - ramp(s, 100 * MIN_S, 15 * MIN_S));
        co2 += (1900.0f - co2) * e;
        break;
    }
    case DEMO_HEAT: {
        float e = ramp(s, 0, HOUR_S) * (1 - ramp(s, 3 * HOUR_S, HOUR_S));
        t += (34.5f - t) * e;
        rh += (65.0f - rh) * e;
        break;
    }
    case DEMO_DRY: {
        float e = ramp(s, 0, 30 * MIN_S) * (1 - ramp(s, 150 * MIN_S, HOUR_S));
        t += (14.5f - t) * e;
        rh += (25.0f - rh) * e;
        break;
    }
    case DEMO_PRES:
        p -= 8.0f * ramp(s, 0, 4 * HOUR_S) * (1 - ramp(s, 10 * HOUR_S, 6 * HOUR_S));
        break;
    default:
        break;
    }

    out->t = t + 0.05f * noise(s, 0);
    out->rh = rh + 0.3f * noise(s, 1);
    out->co2 = co2 + 5.0f * noise(s, 2);
    out->p = p + 0.03f * noise(s, 3);
}

/* The other sensors read close to the primary ones (SCD41 raw: self-heating) */
static void spread(const demo_vals_t *v, float x[S_COUNT])
{
    x[S_CO2] = v->co2;
    x[S_T_SHT] = v->t;
    x[S_T_SCD_RAW] = v->t + 2.8f;
    x[S_T_SCD] = v->t + 0.15f;
    x[S_T_BMP] = v->t + 0.4f;
    x[S_RH_SHT] = v->rh;
    x[S_RH_SCD_RAW] = v->rh - 8.0f;
    x[S_RH_SCD] = v->rh - 0.8f;
    x[S_P_BMP] = v->p;
}

void demo_minute(demo_scn_t scn, int32_t s, int32_t tod0, stat_t out[S_COUNT])
{
    float sum[S_COUNT] = {0};
    memset(out, 0, sizeof(stat_t) * S_COUNT);
    for (int k = 0; k < 6; k++) {
        demo_vals_t v;
        float x[S_COUNT];
        demo_eval(scn, s + 5 + 10 * k, tod0, &v);
        spread(&v, x);
        for (int i = 0; i < S_COUNT; i++) {
            sum[i] += x[i];
            if (k == 0 || x[i] < out[i].min) out[i].min = x[i];
            if (k == 0 || x[i] > out[i].max) out[i].max = x[i];
        }
    }
    for (int i = 0; i < S_COUNT; i++) {
        out[i].avg = sum[i] / 6;
        out[i].n = 6;
    }
}

void demo_sample10(demo_scn_t scn, int32_t s, int32_t tod0, sample10_t *out)
{
    demo_vals_t v;
    float x[S_COUNT];
    demo_eval(scn, s + 5, tod0, &v);
    spread(&v, x);
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < S_COUNT; i++) {
        out->s[i].v = x[i];
        out->s[i].n = 10;
    }
}
