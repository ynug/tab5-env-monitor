#include "alarm.h"

#include <string.h>

const alarm_group_def_t ALARM_DEFS[AL_COUNT] = {
    [AL_CO2] = {"CO2", false, 2, {
        {1000.0f, 900.0f, SND_CO2_CAUTION, "二酸化炭素濃度が高めです。換気してください", 3, false},
        {1500.0f, 1400.0f, SND_CO2_WARNING, "二酸化炭素濃度がとても高いです。すぐに換気してください", 5, true},
    }},
    [AL_HEAT] = {"熱中症", false, 3, {
        {25.0f, 24.0f, SND_HEAT_CAUTION, "熱中症に注意してください", 3, false},
        {28.0f, 27.0f, SND_HEAT_SEVERE, "熱中症の危険が高まっています", 4, false},
        {31.0f, 30.0f, SND_HEAT_DANGER, "熱中症の危険があります。すぐに室温を下げてください", 6, true},
    }},
    [AL_HUM_LOW] = {"乾燥", true, 1, {
        {35.0f, 38.0f, SND_HUM_LOW, "空気が乾燥しています", 2, false},
    }},
    [AL_HUM_HIGH] = {"多湿", false, 1, {
        {70.0f, 67.0f, SND_HUM_HIGH, "湿度が高くなっています", 2, false},
    }},
    [AL_TEMP_LOW] = {"低温", true, 1, {
        {16.0f, 17.0f, SND_TEMP_LOW, "室温が低くなっています", 2, false},
    }},
    [AL_PRES_DROP] = {"気圧", false, 1, {
        {3.0f, 2.0f, SND_PRES_DROP, "気圧が急に下がっています", 1, false},
    }},
};

void alarm_init(alarm_t *a)
{
    memset(a, 0, sizeof(*a));
}

void alarm_cfg_default(alarm_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    for (int i = 0; i < AL_COUNT; i++) {
        cfg->enabled[i] = true;
    }
    cfg->hold_s = 300;
    cfg->repeat_s = 1800;
}

static bool reached(const alarm_group_def_t *d, int k, float v)
{
    return d->below ? v <= d->lv[k].enter : v >= d->lv[k].enter;
}

static bool released(const alarm_group_def_t *d, int k, float v)
{
    return d->below ? v > d->lv[k].release : v < d->lv[k].release;
}

static uint8_t next_level(const alarm_group_def_t *d, uint8_t level, float v)
{
    uint8_t raw = 0;
    for (int k = 0; k < d->nlevels; k++) {
        if (reached(d, k, v)) {
            raw = k + 1;
        }
    }
    if (raw > level) {
        return raw;
    }
    while (level > 0 && released(d, level - 1, v)) {
        level--;
    }
    return level;
}

static void fire(const alarm_cfg_t *cfg, alarm_group_t g, alarm_group_state_t *s, int level, uint32_t now,
                 sound_t *out, int max_out, int *n)
{
    const alarm_level_def_t *def = &ALARM_DEFS[g].lv[level - 1];
    s->last_announce = now;
    if (!cfg->enabled[g]) {
        return;
    }
    if (cfg->sleeping && !(cfg->sleep_allow_danger && def->danger)) {
        s->suppressed = true;
        return;
    }
    if (*n < max_out) {
        out[(*n)++] = def->sound;
    }
}

int alarm_eval(alarm_t *a, const alarm_cfg_t *cfg, uint32_t now, const alarm_input_t *in, sound_t *out, int max_out)
{
    int n = 0;
    const bool woke = a->was_sleeping && !cfg->sleeping;

    for (int g = 0; g < AL_COUNT; g++) {
        const alarm_group_def_t *d = &ALARM_DEFS[g];
        alarm_group_state_t *s = &a->g[g];

        s->level = in->valid[g] ? next_level(d, s->level, in->value[g]) : 0;
        for (int k = 0; k < d->nlevels; k++) {
            if (s->level > k) {
                if (!s->since[k]) s->since[k] = now;
            } else {
                s->since[k] = 0;
            }
        }

        if (s->level == 0) {
            s->announced = 0;
            s->suppressed = false;
            continue;
        }
        if (s->level < s->announced) {
            s->announced = s->level;    /* fell back: no announcement */
        }

        /* highest level above the announced one that has held long enough */
        int due = 0;
        for (int k = s->announced; k < s->level; k++) {
            if (now - s->since[k] >= cfg->hold_s) {
                due = k + 1;
            }
        }
        if (due) {
            s->announced = due;
            fire(cfg, g, s, due, now, out, max_out, &n);
        } else if (s->announced && now - s->last_announce >= cfg->repeat_s) {
            fire(cfg, g, s, s->announced, now, out, max_out, &n);
        } else if (woke && s->announced && s->suppressed) {
            s->suppressed = false;
            fire(cfg, g, s, s->announced, now, out, max_out, &n);
        }
    }
    a->was_sleeping = cfg->sleeping;
    return n;
}

bool alarm_banner(const alarm_t *a, alarm_group_t *group, const alarm_level_def_t **def)
{
    int best = -1;
    for (int g = 0; g < AL_COUNT; g++) {
        uint8_t lvl = a->g[g].level;
        if (!lvl) continue;
        const alarm_level_def_t *d = &ALARM_DEFS[g].lv[lvl - 1];
        if (best < 0 || d->rank > (*def)->rank) {
            best = g;
            *def = d;
        }
    }
    if (best < 0) {
        return false;
    }
    *group = (alarm_group_t)best;
    return true;
}
