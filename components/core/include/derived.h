#pragma once

/* Saturation vapour pressure [hPa], Magnus (a=17.62, b=243.12) */
float derived_svp(float t_c);
/* Dew point [degC] */
float derived_dew_point(float t_c, float rh);
/* Discomfort index: 0.81T + 0.01H(0.99T - 14.3) + 46.3 */
float derived_discomfort(float t_c, float rh);
/* Indoor WBGT estimate: 0.725T + 0.0368H + 0.00364TH - 3.246 */
float derived_wbgt(float t_c, float rh);
/* Relative humidity measured at t_from converted to t_to (same absolute humidity), clamped 0..100 */
float derived_rh_convert(float rh, float t_from, float t_to);
/* WBGT level (Japanese guideline for daily life): 0 almost safe (<21), 1 caution (<25),
 * 2 warning (<28), 3 severe warning (<31), 4 danger */
int derived_wbgt_level(float wbgt);
