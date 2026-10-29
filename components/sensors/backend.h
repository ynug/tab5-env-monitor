#pragma once

/* Sensor source behind the pipeline: real I2C drivers (step 7) or the simulator. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "series.h"

typedef struct {
    const char *name;
    bool is_sim;
    esp_err_t (*init)(void);
    /* called once per tick before the reads */
    void (*begin_tick)(uint32_t uptime_s);
    bool (*read_sht)(float *t, float *rh);
    bool (*read_bmp)(float *p, float *t);
    /* true only when a new measurement is available */
    bool (*read_scd)(float *co2, float *t, float *rh);
    void (*set_ambient_pressure)(float hpa);
    esp_err_t (*set_asc)(bool enable);
    /* forced recalibration to target_ppm; *correction receives the applied offset */
    esp_err_t (*frc)(uint16_t target_ppm, int16_t *correction);
    /* SENSOR_PRESENT_* of the sensors answering now (NULL: not real sensors) */
    uint8_t (*present)(void);
} sensor_backend_t;

/* init fails with ESP_ERR_NOT_FOUND when no sensor answers on PORT.A */
extern const sensor_backend_t backend_real;
extern const sensor_backend_t backend_sim;

/* Force an ambient value of the simulator (NAN releases it) */
void backend_sim_override(metric_id_t m, float value);
