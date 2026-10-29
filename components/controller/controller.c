#include "controller.h"

#include <math.h>
#include <string.h>
#include "audio.h"
#include "datastore.h"
#include "derived.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sensor_hub.h"
#include "settings.h"

static const char *TAG = "controller";

#define PRES_NOW_MIN    5       /* average of the last minutes */
#define PRES_AGO_MIN    180     /* compared with 3 h ago */
#define PRES_AGO_WIN    5

static SemaphoreHandle_t s_mutex;
static alarm_t s_alarm;
static controller_banner_t s_banner;
static bool s_banner_demo;     /* the banner was computed from demo values */

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000) + 1;
}

void controller_refresh_backlight(void)
{
    app_settings_t st;
    settings_get(&st);
    display_set_brightness(st.brightness);
}

static float pres_avg(const history_t *h, uint32_t from, uint32_t to)
{
    double sum = 0;
    int n = 0;
    for (uint32_t k = from; k <= to; k++) {
        const hist_entry_t *e = history_get(h, k);
        if (e && e->s[S_P_BMP].n) {
            sum += e->s[S_P_BMP].avg;
            n++;
        }
    }
    return n ? (float)(sum / n) : NAN;
}

static float pick(const sample10_t *s, int a, int b, int c)
{
    if (s->s[a].n) return s->s[a].v;
    if (b >= 0 && s->s[b].n) return s->s[b].v;
    if (c >= 0 && s->s[c].n) return s->s[c].v;
    return NAN;
}

static void on_10s(const sample10_t *s)
{
    app_settings_t st;
    settings_get(&st);

    /* a demo runs on its own clock (up to 60x) and must not leave its alarms behind */
    static bool was_demo;
    sensor_demo_info_t demo;
    sensor_hub_demo_info(&demo);
    if (demo.active != was_demo) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        alarm_init(&s_alarm);
        xSemaphoreGive(s_mutex);
        was_demo = demo.active;
    }
    const uint32_t now_alarm = demo.active ? s->t : now_s();

    /* alarm inputs from the 10 s values */
    alarm_input_t in = {0};
    float co2 = s->s[S_CO2].n ? s->s[S_CO2].v : NAN;
    float t = pick(s, S_T_SHT, S_T_BMP, S_T_SCD);
    float rh = pick(s, S_RH_SHT, S_RH_SCD, -1);
    in.value[AL_CO2] = co2;
    in.value[AL_HEAT] = derived_wbgt(t, rh);
    in.value[AL_HUM_LOW] = rh;
    in.value[AL_HUM_HIGH] = rh;
    in.value[AL_TEMP_LOW] = t;

    const history_t *h = datastore_lock();
    uint32_t now = datastore_now_key(h);
    float p_now = pres_avg(h, now - PRES_NOW_MIN, now);
    float p_ago = pres_avg(h, now - PRES_AGO_MIN - PRES_AGO_WIN, now - PRES_AGO_MIN + PRES_AGO_WIN);
    datastore_unlock();
    in.value[AL_PRES_DROP] = p_ago - p_now;

    for (int g = 0; g < AL_COUNT; g++) {
        in.valid[g] = !isnan(in.value[g]);
    }

    alarm_cfg_t cfg;
    alarm_cfg_default(&cfg);
    memcpy(cfg.enabled, st.alarm_enabled, sizeof(cfg.enabled));

    sound_t out[AL_COUNT];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = alarm_eval(&s_alarm, &cfg, now_alarm, &in, out, AL_COUNT);
    s_banner.active = alarm_banner(&s_alarm, &s_banner.group, &s_banner.def);
    s_banner_demo = demo.active;
    xSemaphoreGive(s_mutex);

    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "announce %d", out[i]);
        audio_play(out[i]);
    }
}

static void on_settings(const app_settings_t *st)
{
    static int asc = -1;
    if (asc != st->scd41_asc) {
        asc = st->scd41_asc;
        sensor_hub_set_asc(st->scd41_asc);
    }
    audio_set_volume(st->volume);
    display_set_flipped(st->flipped);
    if (st->brightness != display_get_brightness()) {
        display_set_brightness(st->brightness);
    }
}

esp_err_t controller_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    alarm_init(&s_alarm);

    app_settings_t st;
    settings_get(&st);
    on_settings(&st);
    settings_add_listener(on_settings);

    sensor_hub_set_10s_cb(on_10s);
    return ESP_OK;
}

void controller_get_banner(controller_banner_t *out)
{
    sensor_demo_info_t demo;
    sensor_hub_demo_info(&demo);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_banner;
    /* a demo just started or stopped: drop the other side's banner until the next value */
    if (s_banner_demo != demo.active) {
        out->active = false;
    }
    xSemaphoreGive(s_mutex);
}
