/*
 * Simulated SCD41 / SHT35 / BMP581 so that the whole pipeline runs without hardware.
 * Ambient: daily temperature cycle plus a fast 200 s triangle swing (display changes about every
 * 10 s, for verification), occupancy-driven CO2 ramps with ventilation, pressure swings. Sensor models add noise, the SCD41 self-heating and occasional glitches.
 */
#include <math.h>
#include "backend.h"
#include "derived.h"
#include "esp_random.h"

#define TWO_PI      6.2831853f
#define P_GLITCH    0.002f      /* spike probability per reading */
#define P_DROPOUT   0.003f      /* failed read (CRC / NACK) probability */

static uint32_t s_t;
static float s_amb_t, s_amb_rh, s_amb_co2, s_amb_p;
static float s_override[M_COUNT] = {NAN, NAN, NAN, NAN};

static float urand(void)
{
    return (esp_random() >> 8) * (1.0f / 16777216.0f);
}

static float gauss(float sigma)
{
    float u1 = urand() + 1e-7f, u2 = urand();
    return sigma * sqrtf(-2.0f * logf(u1)) * cosf(TWO_PI * u2);
}

static bool chance(float p)
{
    return urand() < p;
}

/* Triangle wave in [-1, 1]: constant slope, unlike a sine that stalls at its peaks */
static float tri(uint32_t t, uint32_t period)
{
    float ph = (float)(t % period) / period;
    return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
}

static void ambient(uint32_t t)
{
    float day = TWO_PI * (t % 86400) / 86400.0f;
    /* +-0.6 triangle over 200 s = 0.12 per 10 s, so the 0.1 digit moves every ~10 s (verification) */
    s_amb_t = 24.0f + 1.5f * sinf(day) + 0.6f * tri(t, 200);
    s_amb_rh = 50.0f - 2.5f * (s_amb_t - 24.0f) + 3.0f * sinf(TWO_PI * t / 7200.0f);

    /* 90 min cycle: 50 min occupied (CO2 rises), then ventilation */
    float c = (t % 5400) / 60.0f;
    s_amb_co2 = c < 50.0f ? 520.0f + 900.0f * (1.0f - expf(-c / 30.0f))
                          : 520.0f + 900.0f * (1.0f - expf(-50.0f / 30.0f)) * expf(-(c - 50.0f) / 6.0f);

    s_amb_p = 1008.0f + 2.0f * sinf(TWO_PI * t / 43200.0f) + 0.6f * tri(t + 50, 200);
}

void backend_sim_override(metric_id_t m, float value)
{
    if (m < M_COUNT) {
        s_override[m] = value;
    }
}

static esp_err_t sim_init(void)
{
    ambient(0);
    return ESP_OK;
}

static void sim_begin_tick(uint32_t uptime_s)
{
    s_t = uptime_s;
    ambient(uptime_s);
    if (!isnan(s_override[M_CO2])) s_amb_co2 = s_override[M_CO2];
    if (!isnan(s_override[M_TEMP])) s_amb_t = s_override[M_TEMP];
    if (!isnan(s_override[M_HUM])) s_amb_rh = s_override[M_HUM];
    if (!isnan(s_override[M_PRES])) s_amb_p = s_override[M_PRES];
}

static bool sim_read_sht(float *t, float *rh)
{
    if (chance(P_DROPOUT)) {
        return false;
    }
    *t = s_amb_t + gauss(0.02f) + (chance(P_GLITCH) ? 8.0f : 0.0f);
    *rh = s_amb_rh + gauss(0.2f);
    return true;
}

static bool sim_read_bmp(float *p, float *t)
{
    if (chance(P_DROPOUT)) {
        return false;
    }
    *p = s_amb_p + gauss(0.03f) + (chance(P_GLITCH) ? -15.0f : 0.0f);
    *t = s_amb_t + 0.4f + gauss(0.03f);
    return true;
}

static bool sim_read_scd(float *co2, float *t, float *rh)
{
    if (s_t % 5 != 0 || chance(P_DROPOUT)) {
        return false;
    }
    float t_scd = s_amb_t + 3.8f;   /* self-heating */
    *co2 = s_amb_co2 + gauss(8.0f) + (chance(P_GLITCH) ? 600.0f : 0.0f);
    *t = t_scd + gauss(0.08f);
    *rh = derived_rh_convert(s_amb_rh, s_amb_t, t_scd) + gauss(0.4f);
    return true;
}

static void sim_set_pressure(float hpa)
{
}

static esp_err_t sim_set_asc(bool enable)
{
    return ESP_OK;
}

static esp_err_t sim_frc(uint16_t target_ppm, int16_t *correction)
{
    return ESP_ERR_NOT_SUPPORTED;
}

const sensor_backend_t backend_sim = {
    .name = "simulator",
    .is_sim = true,
    .init = sim_init,
    .begin_tick = sim_begin_tick,
    .read_sht = sim_read_sht,
    .read_bmp = sim_read_bmp,
    .read_scd = sim_read_scd,
    .set_ambient_pressure = sim_set_pressure,
    .set_asc = sim_set_asc,
    .frc = sim_frc,
};
