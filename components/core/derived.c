#include "derived.h"

#include <math.h>

#define MAGNUS_A 17.62f
#define MAGNUS_B 243.12f

float derived_svp(float t_c)
{
    return 6.112f * expf(MAGNUS_A * t_c / (MAGNUS_B + t_c));
}

float derived_dew_point(float t_c, float rh)
{
    if (rh <= 0.0f) {
        return NAN;
    }
    float g = logf(rh / 100.0f) + MAGNUS_A * t_c / (MAGNUS_B + t_c);
    return MAGNUS_B * g / (MAGNUS_A - g);
}

float derived_discomfort(float t_c, float rh)
{
    return 0.81f * t_c + 0.01f * rh * (0.99f * t_c - 14.3f) + 46.3f;
}

float derived_wbgt(float t_c, float rh)
{
    return 0.725f * t_c + 0.0368f * rh + 0.00364f * t_c * rh - 3.246f;
}

float derived_rh_convert(float rh, float t_from, float t_to)
{
    float v = rh * derived_svp(t_from) / derived_svp(t_to);
    return v < 0.0f ? 0.0f : v > 100.0f ? 100.0f : v;
}

int derived_wbgt_level(float wbgt)
{
    if (wbgt < 21) return 0;
    if (wbgt < 25) return 1;
    if (wbgt < 28) return 2;
    if (wbgt < 31) return 3;
    return 4;
}
