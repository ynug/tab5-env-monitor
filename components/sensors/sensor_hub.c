#include "sensor_hub.h"

#include <string.h>
#include <time.h>
#include "backend.h"
#include "datastore.h"
#include "demo.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sensors";

#define TASK_PRIO   10
#define TASK_STACK  (6 * 1024)
#define TASK_CORE   1

static const sensor_backend_t *s_be;
static pipeline_t s_pipe;
static SemaphoreHandle_t s_mutex;
static sensor_live_t s_live;
static sensor_10s_cb_t s_10s_cb;

#define DEMO_START_NO_CLOCK 1792195200LL    /* 2026-10-17 09:00 JST, when the clock is not set */
#define TZ_S                (9 * 3600)

/* Demo: requests come from the UI, the sensor task runs it */
static struct {
    bool req_start, req_stop;
    demo_scn_t req_scn;
    int req_speed;

    bool active;
    demo_scn_t scn;
    int speed;
    int64_t t0;         /* unix s at the start, minute aligned */
    int32_t tod0;       /* local time of day at the start */
    int32_t s;          /* demo seconds since the start */
    int32_t next10;     /* start of the next 10 s block */
} s_demo;

static void demo_begin(demo_scn_t scn, int speed)
{
    int64_t t0 = clock_valid() ? (int64_t)time(NULL) : DEMO_START_NO_CLOCK;
    t0 -= t0 % 60;
    if (datastore_demo_begin((uint32_t)(t0 / 60)) != ESP_OK) {
        ESP_LOGE(TAG, "demo: no memory");
        return;
    }
    s_demo.scn = scn;
    s_demo.speed = speed;
    s_demo.t0 = t0;
    s_demo.tod0 = (int32_t)((t0 + TZ_S) % 86400);
    s_demo.s = 0;
    s_demo.next10 = 0;
    /* the 24 h before the start, so every graph has data at once */
    for (int32_t m = -HIST_MINUTES; m < 0; m++) {
        stat_t st[S_COUNT];
        demo_minute(scn, m * 60, s_demo.tod0, st);
        datastore_demo_put_minute((uint32_t)((t0 + m * 60) / 60), st);
    }
    s_demo.active = true;
    ESP_LOGI(TAG, "demo %d x%d started", scn, speed);
}

/* One real second: advance the demo clock, emit its 10 s / minute values, update the display */
static void demo_tick(void)
{
    s_demo.s += s_demo.speed;
    while (s_demo.next10 + 10 <= s_demo.s) {
        const int32_t b = s_demo.next10;
        sample10_t v;
        demo_sample10(s_demo.scn, b, s_demo.tod0, &v);
        v.t = (uint32_t)(s_demo.t0 + b);
        datastore_demo_push_10s(&v, (uint32_t)((s_demo.t0 + b + 10) / 60));
        if (s_10s_cb) {
            s_10s_cb(&v);
        }
        if ((b + 10) % 60 == 0) {
            stat_t st[S_COUNT];
            demo_minute(s_demo.scn, b - 50, s_demo.tod0, st);
            datastore_demo_put_minute((uint32_t)((s_demo.t0 + b - 50) / 60), st);
        }
        s_demo.next10 += 10;
    }

    demo_vals_t dv;
    demo_eval(s_demo.scn, s_demo.s, s_demo.tod0, &dv);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    display_vals_t *d = &s_live.disp;
    d->value[M_CO2] = dv.co2;
    d->value[M_TEMP] = dv.t;
    d->value[M_HUM] = dv.rh;
    d->value[M_PRES] = dv.p;
    for (int m = 0; m < M_COUNT; m++) {
        d->valid[m] = true;
    }
    d->t_src = SRC_SHT;
    d->rh_src = SRC_SHT;
    xSemaphoreGive(s_mutex);
}

/* Apply UI requests (in the sensor task, so the demo state has one owner) */
static void demo_requests(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool start = s_demo.req_start, stop = s_demo.req_stop;
    demo_scn_t scn = s_demo.req_scn;
    int speed = s_demo.req_speed;
    s_demo.req_start = s_demo.req_stop = false;
    xSemaphoreGive(s_mutex);

    if (stop || start) {
        if (s_demo.active) {
            s_demo.active = false;
            datastore_demo_end();
            ESP_LOGI(TAG, "demo stopped");
        }
    }
    if (start) {
        demo_begin(scn, speed);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_live.demo = s_demo.active;
    if (!s_demo.active) {
        s_live.disp = s_pipe.disp;      /* back to the real values at once */
    }
    xSemaphoreGive(s_mutex);
}

static const sensor_backend_t *pick_backend(void)
{
    if (backend_real.init() == ESP_OK) {
        return &backend_real;
    }
    ESP_LOGW(TAG, "no sensor on PORT.A: using the simulator");
    ESP_ERROR_CHECK(backend_sim.init());
    return &backend_sim;
}

static void task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));

        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
        bool tv = clock_valid();
        uint32_t t = tv ? (uint32_t)time(NULL) : up;

        raw_tick_t raw = {0};
        s_be->begin_tick(up);
        raw.sht_ok = s_be->read_sht(&raw.sht_t, &raw.sht_rh);
        raw.bmp_ok = s_be->read_bmp(&raw.bmp_p, &raw.bmp_t);
        raw.scd_new = s_be->read_scd(&raw.scd_co2, &raw.scd_t, &raw.scd_rh);

        demo_requests();

        /* the real measurement and its records continue during a demo; only what is shown
         * and the alarms come from the demo */
        uint32_t ev = pipeline_tick(&s_pipe, t, tv, up, &raw);
        if (ev & PIPE_EV_10S) {
            datastore_push_10s(&s_pipe.last10);
            if (s_10s_cb && !s_demo.active) {
                s_10s_cb(&s_pipe.last10);
            }
        }
        if (ev & PIPE_EV_MIN) {
            datastore_push_minute(&s_pipe.last_min);
            /* SCD41 CO2 pressure compensation with the last minute's mean pressure */
            if (s_pipe.last_min.s[S_P_BMP].n) {
                s_be->set_ambient_pressure(s_pipe.last_min.s[S_P_BMP].avg);
            }
        }

        if (s_demo.active) {
            demo_tick();
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (!s_demo.active) {
            s_live.disp = s_pipe.disp;
        }
        if (ev & PIPE_EV_10S) {
            s_live.last10 = s_pipe.last10;
            s_live.have10 = true;
        }
        s_live.stats = s_pipe.stats;
        s_live.scd_t_offset = s_pipe.scd_t_offset;
        s_live.offset_known = s_pipe.offset_known;
        s_live.present = s_be->present ? s_be->present() : 0;
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t sensor_hub_start(void)
{
    s_be = pick_backend();
    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex, ESP_ERR_NO_MEM, TAG, "mutex");

    ESP_RETURN_ON_ERROR(datastore_init(s_be->is_sim), TAG, "datastore");
    pipeline_init(&s_pipe, datastore_boot_id(), s_be->is_sim ? REC_FLAG_SIM : 0);
    s_live.sim = s_be->is_sim;
    s_live.backend = s_be->name;

    BaseType_t ok = xTaskCreatePinnedToCore(task, "sensors", TASK_STACK, NULL, TASK_PRIO, NULL, TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "backend: %s", s_be->name);
    return ESP_OK;
}

bool sensor_hub_is_sim(void)
{
    return s_be && s_be->is_sim;
}

void sensor_hub_get_live(sensor_live_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_live;
    xSemaphoreGive(s_mutex);
}

void sensor_hub_set_10s_cb(sensor_10s_cb_t cb)
{
    s_10s_cb = cb;
}

void sensor_hub_sim_override(metric_id_t m, float value)
{
    backend_sim_override(m, value);
}

esp_err_t sensor_hub_set_asc(bool enable)
{
    return s_be && s_be->set_asc ? s_be->set_asc(enable) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sensor_hub_frc(uint16_t target_ppm, int16_t *correction)
{
    return s_be && s_be->frc ? s_be->frc(target_ppm, correction) : ESP_ERR_NOT_SUPPORTED;
}

void sensor_hub_demo_start(demo_scn_t scn, int speed)
{
    if ((unsigned)scn >= DEMO_COUNT) {
        return;     /* the debug console passes any number */
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_demo.req_scn = scn;
    s_demo.req_speed = speed < 1 ? 1 : speed;
    s_demo.req_start = true;
    xSemaphoreGive(s_mutex);
}

void sensor_hub_demo_stop(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_demo.req_stop = true;
    xSemaphoreGive(s_mutex);
}

void sensor_hub_demo_info(sensor_demo_info_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->active = s_demo.active;
    out->pending = s_demo.req_start || s_demo.req_stop;
    out->scn = s_demo.scn;
    out->speed = s_demo.speed;
    out->unix_now = s_demo.t0 + s_demo.s;
    xSemaphoreGive(s_mutex);
}
