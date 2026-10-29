#pragma once

#include <stdbool.h>
#include "demo.h"
#include "esp_err.h"
#include "pipeline.h"
#include "series.h"

typedef struct {
    display_vals_t disp;
    sample10_t last10;
    bool have10;
    pipeline_stats_t stats;
    float scd_t_offset;
    bool offset_known;
    bool sim;
    bool demo;          /* disp shows the demo, not the sensors */
    const char *backend;
    uint8_t present;    /* SENSOR_PRESENT_* answering on PORT.A (real sensors only) */
} sensor_live_t;

#define SENSOR_PRESENT_SHT35    0x01
#define SENSOR_PRESENT_BMP581   0x02
#define SENSOR_PRESENT_SCD41    0x04

/* Pick a backend (real sensors if found, else the simulator) and start sampling. */
esp_err_t sensor_hub_start(void);
bool sensor_hub_is_sim(void);
void sensor_hub_get_live(sensor_live_t *out);

/* Called from the sensor task for every new 10 s value */
typedef void (*sensor_10s_cb_t)(const sample10_t *s);
void sensor_hub_set_10s_cb(sensor_10s_cb_t cb);

/* SCD41 calibration (ESP_ERR_NOT_SUPPORTED on the simulator) */
esp_err_t sensor_hub_set_asc(bool enable);
esp_err_t sensor_hub_frc(uint16_t target_ppm, int16_t *correction);

/* Simulator only: force an ambient value (NAN to release) for testing */
void sensor_hub_sim_override(metric_id_t m, float value);

/*
 * Demo (applied by the sensor task within a second): the display, the alarms and
 * datastore_lock() follow the scenario; the real measurement keeps being recorded.
 * speed: demo seconds per second (1 or 60). Starting again restarts with the new scenario.
 */
void sensor_hub_demo_start(demo_scn_t scn, int speed);
void sensor_hub_demo_stop(void);

typedef struct {
    bool active;
    bool pending;       /* a start/stop request is not applied yet */
    demo_scn_t scn;
    int speed;
    int64_t unix_now;   /* demo clock */
} sensor_demo_info_t;
void sensor_hub_demo_info(sensor_demo_info_t *out);
