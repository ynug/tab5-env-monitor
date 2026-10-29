/* Host unit tests for components/core. Run: tools/run_host_tests.sh */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm.h"
#include "csvlog.h"
#include "demo.h"
#include "derived.h"
#include "histlog.h"
#include "history.h"
#include "pipeline.h"
#include "record.h"
#include "sensor_codec.h"

static int g_fail, g_checks;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_NEAR(a, b, tol) do { g_checks++; double _a = (a), _b = (b); if (!(fabs(_a - _b) <= (tol))) { \
    g_fail++; printf("  FAIL %s:%d: %s = %.4f, expected %.4f\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* ---------- derived ---------- */

static void test_derived(void)
{
    CHECK_NEAR(derived_dew_point(25.0f, 50.0f), 13.85, 0.05);
    CHECK_NEAR(derived_wbgt(25.0f, 50.0f), 21.27, 0.01);
    CHECK_NEAR(derived_discomfort(25.0f, 50.0f), 71.78, 0.01);
    /* same absolute humidity: warmer -> lower RH */
    float rh = derived_rh_convert(50.0f, 25.0f, 29.0f);
    CHECK(rh < 50.0f && rh > 35.0f);
    CHECK_NEAR(derived_rh_convert(rh, 29.0f, 25.0f), 50.0, 0.01);
    CHECK(derived_wbgt_level(20.9f) == 0 && derived_wbgt_level(21.0f) == 1 && derived_wbgt_level(27.9f) == 2);
    CHECK(derived_wbgt_level(28.0f) == 3 && derived_wbgt_level(31.0f) == 4);
}

/* ---------- filters ---------- */

static void test_hampel(void)
{
    hampel_t h;
    hampel_init(&h, 0.3f);
    for (int i = 0; i < 10; i++) {
        CHECK(hampel_accept(&h, 24.0f + 0.01f * (i % 3)));
    }
    CHECK(!hampel_accept(&h, 30.0f));          /* spike */
    CHECK(hampel_accept(&h, 24.01f));
    /* a real step is accepted once it dominates the window */
    int accepted_at = -1;
    for (int i = 0; i < 6; i++) {
        if (hampel_accept(&h, 26.0f) && accepted_at < 0) {
            accepted_at = i;
        }
    }
    CHECK(accepted_at >= 1 && accepted_at <= 3);
}

static void test_one_euro(void)
{
    one_euro_t f;
    one_euro_init(&f, 0.03f, 3.0f, 0.1f);
    float y = 0;
    srand(1);
    for (int i = 0; i < 300; i++) {
        float noise = ((rand() % 1000) / 1000.0f - 0.5f) * 0.1f;
        y = one_euro_update(&f, 24.0f + noise, 1.0f);
    }
    CHECK_NEAR(y, 24.0, 0.03);                 /* noise suppressed */
    for (int i = 0; i < 120; i++) {
        y = one_euro_update(&f, 26.0f, 1.0f);
    }
    CHECK_NEAR(y, 26.0, 0.1);                  /* follows a step within 2 min */
}

/* ---------- pipeline ---------- */

static raw_tick_t steady(uint32_t t)
{
    raw_tick_t r = {
        .sht_ok = true, .sht_t = 24.0f, .sht_rh = 50.0f,
        .bmp_ok = true, .bmp_p = 1008.0f, .bmp_t = 24.4f,
        .scd_new = (t % 5) == 0, .scd_co2 = 600.0f, .scd_t = 28.0f, .scd_rh = 40.0f,
    };
    return r;
}

static void test_pipeline_aggregation(void)
{
    static pipeline_t p;
    pipeline_init(&p, 7, 0);
    int n10 = 0, nmin = 0;
    minute_rec_t first = {0};
    for (uint32_t t = 0; t <= 120; t++) {
        raw_tick_t r = steady(t);
        uint32_t ev = pipeline_tick(&p, t, false, t, &r);
        if (ev & PIPE_EV_10S) {
            n10++;
            CHECK(p.last10.s[S_T_SHT].n == 10);
            CHECK(p.last10.s[S_CO2].n == 2);
        }
        if (ev & PIPE_EV_MIN) {
            if (nmin == 0) first = p.last_min;
            nmin++;
        }
    }
    CHECK(n10 == 12);
    CHECK(nmin == 2);
    CHECK(first.boot_id == 7 && first.uptime_min == 0 && first.unix_min == 0);
    CHECK(first.s[S_T_SHT].n == 6);
    CHECK_NEAR(first.s[S_T_SHT].avg, 24.0, 1e-4);
    CHECK_NEAR(first.s[S_P_BMP].avg, 1008.0, 1e-3);
    CHECK_NEAR(first.s[S_CO2].avg, 600.0, 1e-3);
    /* SCD41 self-heating removed, humidity referred to the SHT35 temperature */
    CHECK_NEAR(first.s[S_T_SCD].avg, 24.0, 0.01);
    CHECK_NEAR(first.s[S_RH_SCD].avg, derived_rh_convert(40.0f, 28.0f, 24.0f), 0.01);
    CHECK(p.disp.valid[M_TEMP] && p.disp.t_src == SRC_SHT);
    CHECK_NEAR(p.disp.value[M_TEMP], 24.0, 0.01);
}

static void test_pipeline_missing_and_fallback(void)
{
    static pipeline_t p;
    pipeline_init(&p, 1, 0);
    minute_rec_t m = {0};
    bool got = false;
    for (uint32_t t = 0; t <= 60; t++) {
        raw_tick_t r = steady(t);
        r.sht_ok = false;                    /* SHT35 gone */
        if (t < 25) r.bmp_ok = (t % 10) < 2; /* BMP too sparse in the first blocks */
        if (pipeline_tick(&p, t, false, t, &r) & PIPE_EV_MIN) {
            m = p.last_min;
            got = true;
        }
    }
    CHECK(got);
    CHECK(m.s[S_T_SHT].n == 0 && isnan(m.s[S_T_SHT].avg));
    CHECK(m.s[S_T_BMP].n == 4);               /* 2 of 6 blocks had < 3 samples */
    CHECK(p.disp.t_src == SRC_BMP);
    CHECK(p.disp.rh_src == SRC_SCD);
}

static void test_pipeline_spike_rejected(void)
{
    static pipeline_t p;
    pipeline_init(&p, 1, 0);
    for (uint32_t t = 0; t < 30; t++) {
        raw_tick_t r = steady(t);
        if (t == 15) r.sht_t = 45.0f;        /* glitch */
        if (t == 16) r.bmp_p = 2000.0f;      /* out of range */
        pipeline_tick(&p, t, false, t, &r);
    }
    CHECK(p.stats.rejected_outlier[RAW_SHT_T] == 1);
    CHECK(p.stats.rejected_range[RAW_BMP_P] == 1);
    CHECK_NEAR(p.disp.value[M_TEMP], 24.0, 0.01);
}

static void test_pipeline_clock_jump(void)
{
    static pipeline_t p;
    pipeline_init(&p, 1, 0);
    int mins = 0;
    minute_rec_t last = {0};
    for (uint32_t up = 0; up < 200; up++) {
        bool valid = up >= 95;
        uint32_t t = valid ? 1800000000u + up : up;  /* NTP sync at uptime 95 s */
        raw_tick_t r = steady(up);
        if (pipeline_tick(&p, t, valid, up, &r) & PIPE_EV_MIN) {
            mins++;
            last = p.last_min;
        }
    }
    CHECK(mins >= 3);
    CHECK(last.unix_min != 0 && last.unix_min == (1800000000u + 180) / 60 - 1);
}

/* ---------- record ---------- */

static void test_record(void)
{
    log_rec_t r = {.type = REC_TYPE_DATA, .lid = 42};
    r.m.boot_id = 3; r.m.uptime_min = 77; r.m.unix_min = 29999999; r.m.flags = REC_FLAG_SIM;
    for (int i = 0; i < S_COUNT; i++) {
        r.m.s[i] = (stat_t){.avg = 23.456f, .min = 23.1f, .max = 23.9f, .n = 6};
    }
    r.m.s[S_CO2] = (stat_t){.avg = 612.4f, .min = 600, .max = 630, .n = 6};
    r.m.s[S_P_BMP] = (stat_t){.avg = 1008.43f, .min = 1008.3f, .max = 1008.5f, .n = 6};
    r.m.s[S_RH_SCD] = (stat_t){.avg = NAN, .min = NAN, .max = NAN, .n = 0};

    uint8_t buf[REC_SIZE];
    record_encode(&r, buf);
    log_rec_t d;
    CHECK(record_decode(buf, &d));
    CHECK(d.lid == 42 && d.m.boot_id == 3 && d.m.uptime_min == 77 && d.m.unix_min == 29999999);
    CHECK(d.m.flags == REC_FLAG_SIM);
    CHECK_NEAR(d.m.s[S_T_SHT].avg, 23.46, 0.006);
    CHECK_NEAR(d.m.s[S_CO2].avg, 612.0, 0.6);
    CHECK_NEAR(d.m.s[S_P_BMP].avg, 1008.43, 0.026);
    CHECK(d.m.s[S_RH_SCD].n == 0 && isnan(d.m.s[S_RH_SCD].avg));
    buf[30] ^= 0x01;
    CHECK(!record_decode(buf, &d));
    memset(buf, 0xFF, sizeof(buf));
    CHECK(record_is_blank(buf) && !record_decode(buf, &d));
}

/* ---------- histlog over emulated NOR flash ---------- */

#define FL_SECTOR 4096
#define FL_SECTORS 8
static uint8_t g_flash[FL_SECTOR * FL_SECTORS];

static int fl_read(void *c, uint32_t off, void *buf, uint32_t len) { memcpy(buf, g_flash + off, len); return 0; }
static int fl_write(void *c, uint32_t off, const void *buf, uint32_t len)
{
    const uint8_t *b = buf;
    for (uint32_t i = 0; i < len; i++) g_flash[off + i] &= b[i];   /* NOR: bits only go 1 -> 0 */
    return 0;
}
static int fl_erase(void *c, uint32_t off, uint32_t len) { memset(g_flash + off, 0xFF, len); return 0; }
static const flash_ops_t FL = {fl_read, fl_write, fl_erase, NULL, sizeof(g_flash), FL_SECTOR};

typedef struct { uint32_t n, last_lid, first_lid; bool ordered; } walk_t;
static bool walk_cb(const log_rec_t *r, void *a)
{
    walk_t *w = a;
    if (w->n == 0) w->first_lid = r->lid;
    else if (r->lid != w->last_lid + 1) w->ordered = false;
    w->last_lid = r->lid;
    w->n++;
    return true;
}

static log_rec_t data_rec(uint32_t boot, uint32_t up, uint32_t unix_min)
{
    log_rec_t r = {.type = REC_TYPE_DATA};
    r.m.boot_id = boot; r.m.uptime_min = up; r.m.unix_min = unix_min;
    for (int i = 0; i < S_COUNT; i++) r.m.s[i] = (stat_t){.avg = (float)up, .min = (float)up, .max = (float)up, .n = 6};
    return r;
}

static void test_histlog(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    histlog_t log;
    CHECK(histlog_open(&log, &FL) == 0);
    const uint32_t cap = histlog_capacity(&log);
    CHECK(cap == FL_SECTORS * (FL_SECTOR / REC_SIZE));

    for (uint32_t i = 0; i < 1000; i++) {
        log_rec_t r = data_rec(1, i, 0);
        CHECK(histlog_append(&log, &r) == 0);
    }
    CHECK(log.next_lid == 1001);
    CHECK(log.count <= cap && log.count > cap - FL_SECTOR / REC_SIZE);

    walk_t w = {.ordered = true};
    histlog_foreach(&log, walk_cb, &w);
    CHECK(w.ordered && w.last_lid == 1000 && w.n == log.count);

    /* reopen finds the same head */
    histlog_t log2;
    CHECK(histlog_open(&log2, &FL) == 0);
    CHECK(log2.next_lid == 1001 && log2.head == log.head && log2.count == log.count);

    /* torn write at the head: garbage in the next slot is skipped */
    uint32_t off = (log2.head / log2.sps) * FL_SECTOR + (log2.head % log2.sps) * REC_SIZE;
    if (log2.head % log2.sps != 0) {
        g_flash[off] = 0x48;
        g_flash[off + 1] = 0x00;
        histlog_t log3;
        CHECK(histlog_open(&log3, &FL) == 0);
        CHECK(log3.head == log2.head + 1 || log3.head % log3.sps == 0);
        log_rec_t r = data_rec(2, 0, 0);
        CHECK(histlog_append(&log3, &r) == 0 && r.lid == 1001);
        walk_t w3 = {.ordered = true};
        histlog_foreach(&log3, walk_cb, &w3);
        CHECK(w3.ordered && w3.last_lid == 1001);
    }
}

/* ---------- history rebuild ---------- */

static hist_entry_t g_min[HIST_MINUTES];
static sample10_t g_s10[HIST_10S];

static void rebuild(history_t *h, histlog_t *log, uint32_t now_unix, uint32_t cur_up)
{
    static hist_rebuild_t rb;
    history_rebuild_begin(&rb, h, now_unix, cur_up);
    histlog_foreach(log, history_rebuild_feed, &rb);
    history_rebuild_next_pass(&rb);
    histlog_foreach(log, history_rebuild_feed, &rb);
    history_rebuild_end(&rb);
}

static void test_history_rebuild(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    histlog_t log;
    histlog_open(&log, &FL);

    const uint32_t T0 = 29000000;   /* unix minute */
    /* boot 1: 30 min without clock, then an anchor at uptime 30 = T0+30 */
    for (uint32_t m = 0; m < 30; m++) { log_rec_t r = data_rec(1, m, 0); histlog_append(&log, &r); }
    log_rec_t a = {.type = REC_TYPE_ANCHOR};
    a.m.boot_id = 1; a.m.uptime_min = 30; a.m.unix_min = T0 + 30;
    histlog_append(&log, &a);
    for (uint32_t m = 30; m < 40; m++) { log_rec_t r = data_rec(1, m, T0 + m); histlog_append(&log, &r); }
    /* boot 2: 20 min, never got a clock */
    for (uint32_t m = 0; m < 20; m++) { log_rec_t r = data_rec(2, m, 0); histlog_append(&log, &r); }
    /* a SIM record that must be ignored */
    { log_rec_t r = data_rec(2, 25, 0); r.m.flags = REC_FLAG_SIM; histlog_append(&log, &r); }

    history_t h;

    /* boot 3, clock valid at T0+100 */
    history_init(&h, g_min, g_s10, 3, 0);
    rebuild(&h, &log, T0 + 100, 0);
    CHECK(h.abs_time);
    CHECK(history_count(&h) == 40);             /* boot 2 cannot be placed */
    const hist_entry_t *e = history_get(&h, T0 + 5);
    CHECK(e && fabsf(e->s[S_CO2].avg - 5.0f) < 1e-3);
    CHECK(history_get(&h, T0 + 39) != NULL && history_get(&h, T0 + 40) == NULL);
    minute_rec_t live = data_rec(3, 0, T0 + 101).m;
    history_add_minute(&h, &live);
    CHECK(h.latest_key == T0 + 101 && history_get(&h, T0 + 101));

    /* boot 3 without clock: everything laid end to end */
    history_init(&h, g_min, g_s10, 3, 0);
    rebuild(&h, &log, 0, 0);
    CHECK(!h.abs_time);
    CHECK(history_count(&h) == 60);
    live = data_rec(3, 0, 0).m;
    history_add_minute(&h, &live);
    CHECK(history_count(&h) == 61 && h.latest_key == h.rel_base);
    /* gap between boots */
    CHECK(history_get(&h, h.rel_base - 1) == NULL);

    /* SIM mode sees only the SIM record */
    history_init(&h, g_min, g_s10, 3, REC_FLAG_SIM);
    rebuild(&h, &log, 0, 0);
    CHECK(history_count(&h) == 1);
}


/* ---------- alarm ---------- */

static alarm_input_t only(alarm_group_t g, float v)
{
    alarm_input_t in = {0};
    in.value[g] = v;
    in.valid[g] = true;
    return in;
}

/* run from t0 to t1 (exclusive) every 10 s with value v; returns sounds in order */
static int run(alarm_t *a, const alarm_cfg_t *cfg, uint32_t t0, uint32_t t1, alarm_group_t g, float v, sound_t *log, int *nlog)
{
    int total = 0;
    for (uint32_t t = t0; t < t1; t += 10) {
        sound_t out[4];
        alarm_input_t in = only(g, v);
        int n = alarm_eval(a, cfg, t, &in, out, 4);
        for (int i = 0; i < n; i++) log[(*nlog)++] = out[i];
        total += n;
    }
    return total;
}

static void test_alarm_timing(void)
{
    alarm_t a;
    alarm_cfg_t cfg;
    alarm_init(&a);
    alarm_cfg_default(&cfg);
    sound_t log[32];
    int n = 0;

    /* 1100 ppm: caution announced after 5 min, not before */
    CHECK(run(&a, &cfg, 10, 300, AL_CO2, 1100, log, &n) == 0);
    CHECK(run(&a, &cfg, 300, 320, AL_CO2, 1100, log, &n) == 1 && log[0] == SND_CO2_CAUTION);
    /* repeated every 30 min */
    CHECK(run(&a, &cfg, 320, 2110, AL_CO2, 1100, log, &n) == 0);
    CHECK(run(&a, &cfg, 2110, 2130, AL_CO2, 1100, log, &n) == 1);
    /* hysteresis: 950 keeps the caution, banner still shown */
    run(&a, &cfg, 2130, 2200, AL_CO2, 950, log, &n);
    CHECK(a.g[AL_CO2].level == 1);
    /* escalation: warning after 5 min at >= 1500 */
    n = 0;
    CHECK(run(&a, &cfg, 2200, 2490, AL_CO2, 1600, log, &n) == 0);
    CHECK(run(&a, &cfg, 2490, 2510, AL_CO2, 1600, log, &n) == 1 && log[0] == SND_CO2_WARNING);
    /* falling back to caution: no announcement */
    CHECK(run(&a, &cfg, 2510, 2600, AL_CO2, 1200, log, &n) == 0);
    CHECK(a.g[AL_CO2].level == 1 && a.g[AL_CO2].announced == 1);
    /* cleared below 900, then a new episode needs 5 min again */
    run(&a, &cfg, 2600, 2700, AL_CO2, 850, log, &n);
    CHECK(a.g[AL_CO2].level == 0);
    n = 0;
    CHECK(run(&a, &cfg, 2700, 2990, AL_CO2, 1100, log, &n) == 0);
    CHECK(run(&a, &cfg, 2990, 3010, AL_CO2, 1100, log, &n) == 1);
}

static void test_alarm_levels_and_banner(void)
{
    alarm_t a;
    alarm_cfg_t cfg;
    alarm_init(&a);
    alarm_cfg_default(&cfg);
    sound_t log[32];
    int n = 0;

    /* WBGT straight to 31: the danger level is announced (not caution first) */
    run(&a, &cfg, 10, 320, AL_HEAT, 31.5f, log, &n);
    CHECK(n == 1 && log[0] == SND_HEAT_DANGER);
    alarm_group_t g;
    const alarm_level_def_t *d;
    CHECK(alarm_banner(&a, &g, &d) && g == AL_HEAT && d->sound == SND_HEAT_DANGER);

    /* "below" groups: humidity 34 % */
    alarm_init(&a);
    n = 0;
    run(&a, &cfg, 10, 320, AL_HUM_LOW, 34.0f, log, &n);
    CHECK(n == 1 && log[0] == SND_HUM_LOW);
    run(&a, &cfg, 320, 400, AL_HUM_LOW, 37.0f, log, &n);
    CHECK(a.g[AL_HUM_LOW].level == 1);        /* release only above 38 */
    run(&a, &cfg, 400, 420, AL_HUM_LOW, 39.0f, log, &n);
    CHECK(a.g[AL_HUM_LOW].level == 0 && !alarm_banner(&a, &g, &d));

    /* invalid input clears */
    alarm_init(&a);
    alarm_input_t in = only(AL_CO2, 1200);
    sound_t out[4];
    alarm_eval(&a, &cfg, 10, &in, out, 4);
    in.valid[AL_CO2] = false;
    alarm_eval(&a, &cfg, 20, &in, out, 4);
    CHECK(a.g[AL_CO2].level == 0);
}

static void test_alarm_disabled_and_sleep(void)
{
    alarm_t a;
    alarm_cfg_t cfg;
    sound_t log[32];
    int n = 0;

    /* disabled: no sound, state still tracked */
    alarm_init(&a);
    alarm_cfg_default(&cfg);
    cfg.enabled[AL_CO2] = false;
    run(&a, &cfg, 10, 400, AL_CO2, 1200, log, &n);
    CHECK(n == 0 && a.g[AL_CO2].level == 1);

    /* sleep: suppressed, announced once on wake */
    alarm_init(&a);
    alarm_cfg_default(&cfg);
    cfg.sleeping = true;
    n = 0;
    run(&a, &cfg, 10, 4000, AL_CO2, 1200, log, &n);
    CHECK(n == 0 && a.g[AL_CO2].suppressed);
    cfg.sleeping = false;
    CHECK(run(&a, &cfg, 4000, 4010, AL_CO2, 1200, log, &n) == 1 && log[0] == SND_CO2_CAUTION);
    CHECK(run(&a, &cfg, 4010, 4100, AL_CO2, 1200, log, &n) == 0);

    /* sleep with the danger exception */
    alarm_init(&a);
    cfg.sleeping = true;
    cfg.sleep_allow_danger = true;
    n = 0;
    run(&a, &cfg, 10, 320, AL_CO2, 1600, log, &n);
    CHECK(n == 1 && log[0] == SND_CO2_WARNING);
    n = 0;
    alarm_init(&a);
    run(&a, &cfg, 10, 320, AL_CO2, 1100, log, &n);
    CHECK(n == 0);                            /* caution is not a danger level */
}


/* ---------- csvlog ---------- */

static void test_csvlog(void)
{
    const int tz = 9 * 60;
    /* 2026-10-17 14:05 JST = 05:05 UTC */
    const uint32_t t = (uint32_t)(1792213500LL / 60);
    char name[16];
    csvlog_day_name(name, sizeof(name), t, tz);
    CHECK(strcmp(name, "20261017.CSV") == 0);
    /* 23:59 JST is still the same day, 00:00 JST the next */
    uint32_t day0 = csvlog_day_start(t, tz);
    CHECK(t - day0 == 14 * 60 + 5);
    csvlog_day_name(name, sizeof(name), day0 + 1439, tz);
    CHECK(strcmp(name, "20261017.CSV") == 0);
    csvlog_day_name(name, sizeof(name), day0 + 1440, tz);
    CHECK(strcmp(name, "20261018.CSV") == 0);
    csvlog_day_name(name, sizeof(name), (uint32_t)(1709164800LL / 60) - tz, tz);    /* leap day */
    CHECK(strcmp(name, "20240229.CSV") == 0);

    hist_entry_t e;
    memset(&e, 0, sizeof(e));
    e.s[S_CO2] = (stat_t){.avg = 612.4f, .n = 6};
    e.s[S_T_SHT] = (stat_t){.avg = 25.0f, .n = 6};
    e.s[S_RH_SHT] = (stat_t){.avg = 50.0f, .n = 6};
    char line[CSV_LINE_MAX];
    int n = csvlog_format(line, sizeof(line), t, tz, &e);
    CHECK(n > 0 && n < CSV_LINE_MAX && line[n - 1] == '\n');
    CHECK(strncmp(line, "2026-10-17 14:05,612,25.00,50.0,,25.00,,,,50.0,,,13.9,71.8,21.3,", 64) == 0);
    /* same number of fields as the header */
    int commas = 0, hcommas = 0;
    for (int i = 0; i < n; i++) commas += line[i] == ',';
    for (const char *c = CSV_HEADER; *c; c++) hcommas += *c == ',';
    CHECK(commas == hcommas);

    uint32_t back = 0;
    CHECK(csvlog_parse_line(line, (size_t)n - 1, tz, &back) && back == t);
    char bad[CSV_LINE_MAX];
    memcpy(bad, line, (size_t)n);
    bad[20] = bad[20] == '1' ? '2' : '1';
    CHECK(!csvlog_parse_line(bad, (size_t)n - 1, tz, &back));

    /* tail: header, two lines, then a line cut by a power loss */
    char tail[1024];
    size_t o = 0;
    o += (size_t)snprintf(tail + o, sizeof(tail) - o, "%s", CSV_HEADER);
    o += (size_t)csvlog_format(tail + o, sizeof(tail) - o, t, tz, &e);
    o += (size_t)csvlog_format(tail + o, sizeof(tail) - o, t + 1, tz, &e);
    bool nl = false;
    CHECK(csvlog_last_minute(tail, o, tz, &back, &nl) && back == t + 1 && nl);
    int cut = csvlog_format(tail + o, sizeof(tail) - o, t + 2, tz, &e);
    CHECK(csvlog_last_minute(tail, o + (size_t)cut - 10, tz, &back, &nl) && back == t + 1 && !nl);
    /* a complete-looking but damaged last line is skipped too */
    tail[o + 3] = 'x';
    CHECK(csvlog_last_minute(tail, o + (size_t)cut, tz, &back, &nl) && back == t + 1 && nl);
    /* tail that starts mid-line, and a file with only the header */
    CHECK(csvlog_last_minute(tail + strlen(CSV_HEADER) + 5, o - strlen(CSV_HEADER) - 5, tz, &back, &nl) && back == t + 1);
    CHECK(!csvlog_last_minute(CSV_HEADER, strlen(CSV_HEADER), tz, &back, &nl) && nl);
    CHECK(!csvlog_last_minute("", 0, tz, &back, &nl) && nl);
}

/* ---------- demo scenarios ---------- */

/* Run a scenario through the alarm logic for `hours` (10 s steps, 3 h pressure history) */
static void demo_run(demo_scn_t scn, int hours, bool heard[SND_COUNT])
{
    alarm_t a;
    alarm_cfg_t cfg;
    alarm_init(&a);
    alarm_cfg_default(&cfg);
    memset(heard, 0, sizeof(bool) * SND_COUNT);
    const int32_t tod0 = 9 * 3600;
    for (int32_t s = 0; s < hours * 3600; s += 10) {
        sample10_t v;
        demo_sample10(scn, s, tod0, &v);
        stat_t now[S_COUNT], ago[S_COUNT];
        demo_minute(scn, s - 60, tod0, now);
        demo_minute(scn, s - 3 * 3600, tod0, ago);
        alarm_input_t in = {0};
        float t = v.s[S_T_SHT].v, rh = v.s[S_RH_SHT].v;
        in.value[AL_CO2] = v.s[S_CO2].v;
        in.value[AL_HEAT] = derived_wbgt(t, rh);
        in.value[AL_HUM_LOW] = in.value[AL_HUM_HIGH] = rh;
        in.value[AL_TEMP_LOW] = t;
        in.value[AL_PRES_DROP] = ago[S_P_BMP].avg - now[S_P_BMP].avg;
        for (int g = 0; g < AL_COUNT; g++) in.valid[g] = true;
        sound_t out[AL_COUNT];
        int n = alarm_eval(&a, &cfg, (uint32_t)s + 1000, &in, out, AL_COUNT);
        for (int i = 0; i < n; i++) heard[out[i]] = true;
    }
}

static void test_demo(void)
{
    bool heard[SND_COUNT];
    demo_run(DEMO_NORMAL, 24, heard);
    int any = 0;
    for (int i = 0; i < SND_COUNT; i++) any += heard[i];
    CHECK(any == 0);                            /* a normal day is quiet */

    demo_run(DEMO_CO2, 3, heard);
    CHECK(heard[SND_CO2_CAUTION] && heard[SND_CO2_WARNING]);
    demo_run(DEMO_HEAT, 4, heard);
    CHECK(heard[SND_HEAT_CAUTION] || heard[SND_HEAT_SEVERE]);
    CHECK(heard[SND_HEAT_DANGER]);
    demo_run(DEMO_DRY, 3, heard);
    CHECK(heard[SND_HUM_LOW] && heard[SND_TEMP_LOW]);
    demo_run(DEMO_PRES, 6, heard);
    CHECK(heard[SND_PRES_DROP]);

    /* the history before the start is the same normal day for every scenario */
    stat_t a[S_COUNT], b[S_COUNT];
    demo_minute(DEMO_NORMAL, -7200, 0, a);
    demo_minute(DEMO_HEAT, -7200, 0, b);
    CHECK(memcmp(a, b, sizeof(a)) == 0 && a[S_T_SHT].n == 6 && a[S_T_SHT].min <= a[S_T_SHT].max);
}

/* ---------- sensor codec ---------- */

static void test_sensor_codec(void)
{
    /* Sensirion datasheet examples */
    const uint8_t beef[2] = {0xBE, 0xEF};
    CHECK(sensirion_crc8(beef, 2) == 0x92);
    /* SCD41 read_measurement: 500 ppm, 25 degC, 37 %RH */
    const uint8_t meas[9] = {0x01, 0xF4, 0x33, 0x66, 0x67, 0xA2, 0x5E, 0xB9, 0x3C};
    uint16_t w[3];
    CHECK(sensirion_words(meas, 3, w));
    CHECK(w[0] == 500);
    CHECK_NEAR(sensirion_temp(w[1]), 25.0, 0.01);
    CHECK_NEAR(sensirion_rh(w[2]), 37.0, 0.01);
    uint8_t bad[9];
    memcpy(bad, meas, sizeof(bad));
    bad[4] ^= 0x01;
    CHECK(!sensirion_words(bad, 3, w));
    /* set_ambient_pressure 987 hPa */
    uint8_t arg[3];
    sensirion_put_word(arg, 987);
    CHECK(arg[0] == 0x03 && arg[1] == 0xDB && arg[2] == 0x42);
    /* perform_forced_recalibration: 0x7FCE = -50 ppm */
    int16_t corr = 0;
    CHECK(scd41_frc_correction(0x7FCE, &corr) && corr == -50);
    CHECK(!scd41_frc_correction(0xFFFF, &corr));

    /* BMP581: 25.5 degC, 1013.25 hPa = 101325 Pa * 64 = 6484800 = 0x62F340 */
    const uint8_t b[6] = {0x00, 0x80, 0x19, 0x40, 0xF3, 0x62};
    CHECK_NEAR(bmp581_temp(b), 25.5, 1e-4);
    CHECK_NEAR(bmp581_pres_hpa(b), 1013.25, 1e-3);
    const uint8_t neg[6] = {0x00, 0x00, 0xFB, 0, 0, 0};   /* -5 degC */
    CHECK_NEAR(bmp581_temp(neg), -5.0, 1e-4);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        {"derived", test_derived},
        {"hampel", test_hampel},
        {"one_euro", test_one_euro},
        {"pipeline_aggregation", test_pipeline_aggregation},
        {"pipeline_missing_and_fallback", test_pipeline_missing_and_fallback},
        {"pipeline_spike_rejected", test_pipeline_spike_rejected},
        {"pipeline_clock_jump", test_pipeline_clock_jump},
        {"record", test_record},
        {"histlog", test_histlog},
        {"history_rebuild", test_history_rebuild},
        {"alarm_timing", test_alarm_timing},
        {"alarm_levels_and_banner", test_alarm_levels_and_banner},
        {"alarm_disabled_and_sleep", test_alarm_disabled_and_sleep},
        {"csvlog", test_csvlog},
        {"demo", test_demo},
        {"sensor_codec", test_sensor_codec},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fail;
        tests[i].fn();
        printf("%-32s %s\n", tests[i].name, g_fail == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
