#include "pipeline.h"

#include <math.h>
#include <string.h>
#include "derived.h"

#define HAMPEL_K            3.0f
#define MAD_SCALE           1.4826f
#define MIN_VALID_10S       3       /* 10 s values per minute */
#define OFFSET_TAU_S        1800.0f /* SCD41 offset learning time constant */
#define DISPLAY_TIMEOUT_S   30

typedef struct {
    float lo, hi;
    float hampel_floor;
    uint8_t min_per_block;  /* accepted samples needed for a valid 10 s value */
} raw_spec_t;

static const raw_spec_t RAW_SPEC[RAW_COUNT] = {
    [RAW_SHT_T]   = {-10.0f, 60.0f, 0.3f, 3},
    [RAW_SHT_RH]  = {0.0f, 100.0f, 2.0f, 3},
    [RAW_BMP_P]   = {300.0f, 1250.0f, 0.5f, 3},
    [RAW_BMP_T]   = {-10.0f, 60.0f, 0.3f, 3},
    [RAW_SCD_CO2] = {250.0f, 40000.0f, 50.0f, 1},
    [RAW_SCD_T]   = {-10.0f, 60.0f, 0.5f, 1},
    [RAW_SCD_RH]  = {0.0f, 100.0f, 3.0f, 1},
};

/* min_cutoff [Hz], beta, d_cutoff [Hz] */
static const float FILT_PARAM[M_COUNT][3] = {
    [M_CO2]  = {0.03f, 0.05f, 0.1f},
    [M_TEMP] = {0.03f, 3.0f, 0.1f},
    [M_HUM]  = {0.03f, 1.0f, 0.1f},
    [M_PRES] = {0.02f, 2.0f, 0.1f},
};

/* ---------- Hampel ---------- */

void hampel_init(hampel_t *h, float floor)
{
    memset(h, 0, sizeof(*h));
    h->floor = floor;
}

static void sort_small(float *a, int n)
{
    for (int i = 1; i < n; i++) {
        float v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

static float median_small(const float *src, int n)
{
    float a[HAMPEL_WIN];
    memcpy(a, src, n * sizeof(float));
    sort_small(a, n);
    return (n & 1) ? a[n / 2] : 0.5f * (a[n / 2 - 1] + a[n / 2]);
}

/*
 * Causal Hampel filter: the window holds the most recent raw values (accepted or not), so a
 * genuine step is accepted once the majority of the window has moved.
 */
bool hampel_accept(hampel_t *h, float x)
{
    h->buf[h->pos] = x;
    h->pos = (h->pos + 1) % HAMPEL_WIN;
    if (h->n < HAMPEL_WIN) {
        h->n++;
    }
    if (h->n < 3) {
        return true;
    }
    float med = median_small(h->buf, h->n);
    float dev[HAMPEL_WIN];
    for (int i = 0; i < h->n; i++) {
        dev[i] = fabsf(h->buf[i] - med);
    }
    float mad = median_small(dev, h->n);
    float thr = HAMPEL_K * MAD_SCALE * mad;
    if (thr < h->floor) {
        thr = h->floor;
    }
    return fabsf(x - med) <= thr;
}

/* ---------- One Euro filter ---------- */

void one_euro_init(one_euro_t *f, float min_cutoff, float beta, float d_cutoff)
{
    memset(f, 0, sizeof(*f));
    f->min_cutoff = min_cutoff;
    f->beta = beta;
    f->d_cutoff = d_cutoff;
}

static float lp_alpha(float cutoff, float dt)
{
    float tau = 1.0f / (2.0f * (float)M_PI * cutoff);
    return 1.0f / (1.0f + tau / dt);
}

float one_euro_update(one_euro_t *f, float x, float dt)
{
    if (!f->init || dt <= 0.0f) {
        f->x = x;
        f->dx = 0.0f;
        f->init = true;
        return x;
    }
    float dx = (x - f->x) / dt;
    f->dx += lp_alpha(f->d_cutoff, dt) * (dx - f->dx);
    float cutoff = f->min_cutoff + f->beta * fabsf(f->dx);
    f->x += lp_alpha(cutoff, dt) * (x - f->x);
    return f->x;
}

/* ---------- pipeline ---------- */

void pipeline_init(pipeline_t *p, uint32_t boot_id, uint8_t rec_flags)
{
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < RAW_COUNT; i++) {
        hampel_init(&p->hampel[i], RAW_SPEC[i].hampel_floor);
    }
    for (int m = 0; m < M_COUNT; m++) {
        one_euro_init(&p->filt[m], FILT_PARAM[m][0], FILT_PARAM[m][1], FILT_PARAM[m][2]);
    }
    p->boot_id = boot_id;
    p->rec_flags = rec_flags;
}

void pipeline_set_scd_offset(pipeline_t *p, float offset)
{
    p->scd_t_offset = offset;
    p->offset_known = true;
}

static bool check(pipeline_t *p, raw_ch_t ch, float x)
{
    if (isnan(x) || x < RAW_SPEC[ch].lo || x > RAW_SPEC[ch].hi) {
        p->stats.rejected_range[ch]++;
        return false;
    }
    if (!hampel_accept(&p->hampel[ch], x)) {
        p->stats.rejected_outlier[ch]++;
        return false;
    }
    return true;
}

static void set_val(val_t *v, double sum, uint8_t n, uint8_t min_n)
{
    if (n >= min_n) {
        v->v = (float)(sum / n);
        v->n = n;
    } else {
        v->v = NAN;
        v->n = 0;
    }
}

static void finalize_block(pipeline_t *p)
{
    sample10_t *s = &p->last10;
    memset(s, 0, sizeof(*s));
    s->t = p->blk_index * 10;

    val_t raw[RAW_COUNT];
    for (int ch = 0; ch < RAW_COUNT; ch++) {
        set_val(&raw[ch], p->blk_sum[ch], p->blk_n[ch], RAW_SPEC[ch].min_per_block);
    }

    s->s[S_CO2] = raw[RAW_SCD_CO2];
    s->s[S_T_SHT] = raw[RAW_SHT_T];
    s->s[S_T_SCD_RAW] = raw[RAW_SCD_T];
    s->s[S_T_BMP] = raw[RAW_BMP_T];
    s->s[S_RH_SHT] = raw[RAW_SHT_RH];
    s->s[S_RH_SCD_RAW] = raw[RAW_SCD_RH];
    s->s[S_P_BMP] = raw[RAW_BMP_P];

    /* learn SCD41 self-heating offset against the SHT35 */
    if (raw[RAW_SCD_T].n && raw[RAW_SHT_T].n) {
        float d = raw[RAW_SCD_T].v - raw[RAW_SHT_T].v;
        if (!p->offset_known) {
            p->scd_t_offset = d;
            p->offset_known = true;
        } else {
            p->scd_t_offset += (10.0f / OFFSET_TAU_S) * (d - p->scd_t_offset);
        }
    }

    if (raw[RAW_SCD_T].n && p->offset_known) {
        s->s[S_T_SCD].v = raw[RAW_SCD_T].v - p->scd_t_offset;
        s->s[S_T_SCD].n = raw[RAW_SCD_T].n;
    } else {
        s->s[S_T_SCD].v = NAN;
    }

    /* SCD41 humidity referred to the best ambient temperature */
    if (raw[RAW_SCD_RH].n && raw[RAW_SCD_T].n) {
        float t_ref = NAN;
        if (raw[RAW_SHT_T].n) {
            t_ref = raw[RAW_SHT_T].v;
        } else if (s->s[S_T_SCD].n) {
            t_ref = s->s[S_T_SCD].v;
        }
        if (!isnan(t_ref)) {
            s->s[S_RH_SCD].v = derived_rh_convert(raw[RAW_SCD_RH].v, raw[RAW_SCD_T].v, t_ref);
            s->s[S_RH_SCD].n = raw[RAW_SCD_RH].n;
        } else {
            s->s[S_RH_SCD].v = NAN;
        }
    } else {
        s->s[S_RH_SCD].v = NAN;
    }

    p->stats.blocks++;
    p->blk_active = false;
}

static bool finalize_minute(pipeline_t *p)
{
    if (!p->min_active) {
        return false;
    }
    p->min_active = false;

    minute_rec_t *r = &p->last_min;
    memset(r, 0, sizeof(*r));
    r->boot_id = p->boot_id;
    r->uptime_min = p->min_uptime / 60;
    r->unix_min = p->min_time_valid ? p->min_index : 0;
    r->flags = p->rec_flags;

    bool any = false;
    for (int i = 0; i < S_COUNT; i++) {
        stat_t *st = &r->s[i];
        if (p->min_n[i] >= MIN_VALID_10S) {
            st->avg = (float)(p->min_sum[i] / p->min_n[i]);
            st->min = p->min_lo[i];
            st->max = p->min_hi[i];
            st->n = p->min_n[i];
            any = true;
        } else {
            st->avg = st->min = st->max = NAN;
            st->n = 0;
        }
    }
    if (any) {
        p->stats.minutes++;
    }
    return any;
}

static uint32_t add_block_to_minute(pipeline_t *p, bool time_valid)
{
    uint32_t ev = 0;
    uint32_t minute = (p->last10.t) / 60;

    if (p->min_active && (p->min_index != minute || p->min_time_valid != time_valid)) {
        if (finalize_minute(p)) {
            ev |= PIPE_EV_MIN;
        }
    }
    if (!p->min_active) {
        p->min_active = true;
        p->min_index = minute;
        p->min_uptime = p->blk_uptime;
        p->min_time_valid = time_valid;
        memset(p->min_sum, 0, sizeof(p->min_sum));
        memset(p->min_n, 0, sizeof(p->min_n));
    }
    for (int i = 0; i < S_COUNT; i++) {
        const val_t *v = &p->last10.s[i];
        if (!v->n) {
            continue;
        }
        if (p->min_n[i] == 0) {
            p->min_lo[i] = p->min_hi[i] = v->v;
        } else {
            if (v->v < p->min_lo[i]) p->min_lo[i] = v->v;
            if (v->v > p->min_hi[i]) p->min_hi[i] = v->v;
        }
        p->min_sum[i] += v->v;
        p->min_n[i]++;
    }
    /* last block of the minute: close it right away */
    if ((p->last10.t / 10) % 6 == 5) {
        if (finalize_minute(p)) {
            ev |= PIPE_EV_MIN;
        }
    }
    return ev;
}

static void display_input(pipeline_t *p, metric_id_t m, float x, uint32_t uptime_s)
{
    float dt = p->filt[m].init ? (float)(uptime_s - p->last_update[m]) : 0.0f;
    if (dt > DISPLAY_TIMEOUT_S) {
        p->filt[m].init = false;    /* restart after a long gap instead of easing in */
        dt = 0.0f;
    }
    p->disp.value[m] = one_euro_update(&p->filt[m], x, dt);
    p->disp.valid[m] = true;
    p->last_update[m] = uptime_s;
}

static void display_update(pipeline_t *p, const bool acc[RAW_COUNT], const raw_tick_t *raw, uint32_t uptime_s)
{
    float scd_t_corr = NAN;
    if (acc[RAW_SCD_T] && p->offset_known) {
        scd_t_corr = raw->scd_t - p->scd_t_offset;
    }

    if (acc[RAW_SCD_CO2]) {
        display_input(p, M_CO2, raw->scd_co2, uptime_s);
    }

    if (acc[RAW_SHT_T]) {
        display_input(p, M_TEMP, raw->sht_t, uptime_s);
        p->disp.t_src = SRC_SHT;
    } else if (acc[RAW_BMP_T] && (uptime_s - p->last_update[M_TEMP] > 3 || p->disp.t_src != SRC_SHT)) {
        display_input(p, M_TEMP, raw->bmp_t, uptime_s);
        p->disp.t_src = SRC_BMP;
    } else if (!isnan(scd_t_corr) && p->disp.t_src != SRC_SHT && p->disp.t_src != SRC_BMP) {
        display_input(p, M_TEMP, scd_t_corr, uptime_s);
        p->disp.t_src = SRC_SCD;
    }

    if (acc[RAW_SHT_RH]) {
        display_input(p, M_HUM, raw->sht_rh, uptime_s);
        p->disp.rh_src = SRC_SHT;
    } else if (acc[RAW_SCD_RH] && acc[RAW_SCD_T] && (uptime_s - p->last_update[M_HUM] > 3 || p->disp.rh_src != SRC_SHT)) {
        float t_ref = p->disp.valid[M_TEMP] ? p->disp.value[M_TEMP] : scd_t_corr;
        if (!isnan(t_ref)) {
            display_input(p, M_HUM, derived_rh_convert(raw->scd_rh, raw->scd_t, t_ref), uptime_s);
            p->disp.rh_src = SRC_SCD;
        }
    }

    if (acc[RAW_BMP_P]) {
        display_input(p, M_PRES, raw->bmp_p, uptime_s);
    }

    for (int m = 0; m < M_COUNT; m++) {
        if (p->disp.valid[m] && uptime_s - p->last_update[m] > DISPLAY_TIMEOUT_S) {
            p->disp.valid[m] = false;
            if (m == M_TEMP) p->disp.t_src = SRC_NONE;
            if (m == M_HUM) p->disp.rh_src = SRC_NONE;
        }
    }
}

uint32_t pipeline_tick(pipeline_t *p, uint32_t t, bool time_valid, uint32_t uptime_s, const raw_tick_t *raw)
{
    uint32_t ev = 0;
    uint32_t blk = t / 10;

    if (p->blk_active && blk != p->blk_index) {
        finalize_block(p);
        ev |= PIPE_EV_10S;
        ev |= add_block_to_minute(p, p->blk_time_valid);
    }
    if (!p->blk_active) {
        p->blk_active = true;
        p->blk_index = blk;
        p->blk_uptime = uptime_s;
        p->blk_time_valid = time_valid;
        memset(p->blk_sum, 0, sizeof(p->blk_sum));
        memset(p->blk_n, 0, sizeof(p->blk_n));
    }

    bool acc[RAW_COUNT] = {0};
    float x[RAW_COUNT] = {
        raw->sht_t, raw->sht_rh, raw->bmp_p, raw->bmp_t, raw->scd_co2, raw->scd_t, raw->scd_rh,
    };
    bool present[RAW_COUNT] = {
        raw->sht_ok, raw->sht_ok, raw->bmp_ok, raw->bmp_ok, raw->scd_new, raw->scd_new, raw->scd_new,
    };
    for (int ch = 0; ch < RAW_COUNT; ch++) {
        if (present[ch] && check(p, ch, x[ch])) {
            acc[ch] = true;
            p->blk_sum[ch] += x[ch];
            p->blk_n[ch]++;
        }
    }

    display_update(p, acc, raw, uptime_s);
    return ev;
}

uint32_t pipeline_flush_minute(pipeline_t *p)
{
    return finalize_minute(p) ? PIPE_EV_MIN : 0;
}
