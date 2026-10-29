/*
 * SHT35 (0x44) / BMP581 (0x47) / SCD41 (0x62) on PORT.A: G53 SDA / G54 SCL, 100 kHz, pull-ups on
 * the sensor board (docs/SPEC.md 1.4, 3.1, 3.3).
 *
 * Every transaction is retried 3 times (NACK, timeout or CRC). A sensor failing FAIL_LIMIT reads
 * in a row is dropped; dropped or missing sensors are tried again every REPROBE_S seconds, after
 * the bus has been freed (SCL pulses + STOP) and created again.
 * All bus access holds s_lock: the sensor task reads, the UI task may run a forced recalibration.
 */
#include <math.h>
#include "backend.h"
#include "board.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sensor_codec.h"

static const char *TAG = "sensors.i2c";

#define PORTA_SDA       53
#define PORTA_SCL       54
#define I2C_HZ          100000
#define I2C_TIMEOUT_MS  50
#define RETRIES         3
#define FAIL_LIMIT      5
#define REPROBE_S       30

enum { DEV_SHT, DEV_BMP, DEV_SCD, DEV_COUNT };
static const uint8_t ADDR[DEV_COUNT] = {0x44, 0x47, 0x62};
static const char *const NAME[DEV_COUNT] = {"SHT35", "BMP581", "SCD41"};
/* readings dropped after each (re)start: first conversions, BMP581 IIR settling */
static const uint8_t WARMUP[DEV_COUNT] = {2, 3, 2};

typedef struct {
    i2c_master_dev_handle_t h;
    bool ok;
    uint8_t skip;
    uint8_t fails;
} sensor_dev_t;

static i2c_master_bus_handle_t s_bus;
static sensor_dev_t s_dev[DEV_COUNT];
static SemaphoreHandle_t s_lock;
static uint32_t s_next_probe;
static bool s_bus_suspect;
/* SCD41 automatic self-calibration: requested by the settings, applied by the sensor task */
static volatile int8_t s_asc_req = -1;
static int8_t s_asc_cur = -1;

/* at least ms: vTaskDelay(n) may return up to one tick early */
static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

/* ---------- bus ---------- */

static void bus_destroy(void)
{
    for (int d = 0; d < DEV_COUNT; d++) {
        if (s_dev[d].h) {
            i2c_master_bus_rm_device(s_dev[d].h);
            s_dev[d].h = NULL;
        }
        s_dev[d].ok = false;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

static esp_err_t bus_create(void)
{
    board_i2c_recover(PORTA_SDA, PORTA_SCL);
    const i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = PORTA_SDA,
        .scl_io_num = PORTA_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,     /* 4.7 kOhm on the sensor board */
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "bus");
    for (int d = 0; d < DEV_COUNT; d++) {
        const i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ADDR[d],
            .scl_speed_hz = I2C_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dc, &s_dev[d].h), TAG, "add %s", NAME[d]);
    }
    return ESP_OK;
}

/* ---------- Sensirion (SHT35, SCD41) ---------- */

static esp_err_t sen_cmd(int d, uint16_t cmd)
{
    const uint8_t b[2] = {(uint8_t)(cmd >> 8), (uint8_t)cmd};
    return i2c_master_transmit(s_dev[d].h, b, sizeof(b), I2C_TIMEOUT_MS);
}

static esp_err_t sen_cmd_arg(int d, uint16_t cmd, uint16_t arg)
{
    uint8_t b[5] = {(uint8_t)(cmd >> 8), (uint8_t)cmd};
    sensirion_put_word(b + 2, arg);
    return i2c_master_transmit(s_dev[d].h, b, sizeof(b), I2C_TIMEOUT_MS);
}

/* command, wait, read n words (with retries) */
static esp_err_t sen_read(int d, uint16_t cmd, uint32_t wait_ms, uint16_t *w, int n)
{
    uint8_t b[9];
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < RETRIES; i++) {
        err = sen_cmd(d, cmd);
        if (err != ESP_OK) {
            continue;
        }
        delay_ms(wait_ms);
        err = i2c_master_receive(s_dev[d].h, b, n * 3, I2C_TIMEOUT_MS);
        if (err == ESP_OK && !sensirion_words(b, n, w)) {
            err = ESP_ERR_INVALID_CRC;
        }
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
    return err;
}

static esp_err_t sen_retry_cmd(int d, uint16_t cmd)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < RETRIES && err != ESP_OK; i++) {
        err = sen_cmd(d, cmd);
    }
    return err;
}

static esp_err_t sen_retry_cmd_arg(int d, uint16_t cmd, uint16_t arg)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < RETRIES && err != ESP_OK; i++) {
        err = sen_cmd_arg(d, cmd, arg);
    }
    return err;
}

#define SHT_SOFT_RESET      0x30A2
#define SHT_READ_STATUS     0xF32D
#define SHT_SINGLE_HIGH     0x2400      /* high repeatability, no clock stretching */

#define SCD_START_PERIODIC  0x21B1
#define SCD_READ_MEAS       0xEC05
#define SCD_STOP_PERIODIC   0x3F86
#define SCD_DATA_READY      0xE4B8
#define SCD_SET_PRESSURE    0xE000
#define SCD_SET_ASC         0x2416
#define SCD_GET_ASC         0x2313
#define SCD_FRC             0x362F
#define SCD_SERIAL          0x3682

static esp_err_t sht_init(void)
{
    ESP_RETURN_ON_ERROR(sen_retry_cmd(DEV_SHT, SHT_SOFT_RESET), TAG, "SHT35 reset");
    delay_ms(2);
    uint16_t st;
    return sen_read(DEV_SHT, SHT_READ_STATUS, 1, &st, 1);
}

/* Stop (it keeps measuring across our resets), apply the ASC request, start */
static esp_err_t scd_restart(void)
{
    sen_retry_cmd(DEV_SCD, SCD_STOP_PERIODIC);     /* also fine when already idle */
    delay_ms(500);
    int8_t req = s_asc_req;
    if (req >= 0 && req != s_asc_cur) {
        /* not persisted: applied again at every start, so the sensor EEPROM is not worn */
        ESP_RETURN_ON_ERROR(sen_retry_cmd_arg(DEV_SCD, SCD_SET_ASC, (uint16_t)req), TAG, "SCD41 set ASC");
        delay_ms(1);
        s_asc_cur = req;
        ESP_LOGI(TAG, "SCD41 ASC %s", req ? "on" : "off");
    }
    ESP_RETURN_ON_ERROR(sen_retry_cmd(DEV_SCD, SCD_START_PERIODIC), TAG, "SCD41 start");
    s_dev[DEV_SCD].skip = WARMUP[DEV_SCD];
    return ESP_OK;
}

static esp_err_t scd_init(void)
{
    sen_retry_cmd(DEV_SCD, SCD_STOP_PERIODIC);
    delay_ms(500);
    uint16_t sn[3], asc;
    ESP_RETURN_ON_ERROR(sen_read(DEV_SCD, SCD_SERIAL, 1, sn, 3), TAG, "SCD41 serial");
    ESP_RETURN_ON_ERROR(sen_read(DEV_SCD, SCD_GET_ASC, 1, &asc, 1), TAG, "SCD41 get ASC");
    s_asc_cur = asc ? 1 : 0;
    ESP_LOGI(TAG, "SCD41 serial %04x%04x%04x, ASC %s", sn[0], sn[1], sn[2], asc ? "on" : "off");
    return scd_restart();
}

/* ---------- BMP581 ---------- */

#define BMP_CHIP_ID     0x01
#define BMP_DATA        0x1D    /* temperature XLSB .. pressure MSB */
#define BMP_INT_STATUS  0x27
#define BMP_STATUS      0x28
#define BMP_DSP_IIR     0x31
#define BMP_OSR_CONFIG  0x36
#define BMP_ODR_CONFIG  0x37
#define BMP_OSR_EFF     0x38
#define BMP_CMD         0x7E

#define BMP_ODR_DEEP_DIS    0x80
#define BMP_ODR_5HZ         (0x18 << 2)
#define BMP_MODE_NORMAL     0x01

static esp_err_t bmp_rd(uint8_t reg, uint8_t *buf, size_t len)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < RETRIES && err != ESP_OK; i++) {
        err = i2c_master_transmit_receive(s_dev[DEV_BMP].h, &reg, 1, buf, len, I2C_TIMEOUT_MS);
    }
    return err;
}

static esp_err_t bmp_wr(uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = {reg, val};
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < RETRIES && err != ESP_OK; i++) {
        err = i2c_master_transmit(s_dev[DEV_BMP].h, b, sizeof(b), I2C_TIMEOUT_MS);
    }
    return err;
}

static esp_err_t bmp_init(void)
{
    ESP_RETURN_ON_ERROR(bmp_wr(BMP_CMD, 0xB6), TAG, "BMP581 reset");
    delay_ms(3);
    uint8_t id, st;
    ESP_RETURN_ON_ERROR(bmp_rd(BMP_CHIP_ID, &id, 1), TAG, "BMP581 id");
    ESP_RETURN_ON_FALSE(id == 0x50 || id == 0x51, ESP_ERR_NOT_FOUND, TAG, "BMP581 chip id 0x%02x", id);
    ESP_RETURN_ON_ERROR(bmp_rd(BMP_INT_STATUS, &st, 1), TAG, "BMP581 int status");   /* clears POR */
    ESP_RETURN_ON_ERROR(bmp_rd(BMP_STATUS, &st, 1), TAG, "BMP581 status");
    ESP_RETURN_ON_FALSE((st & 0x06) == 0x02, ESP_ERR_INVALID_STATE, TAG, "BMP581 NVM status 0x%02x", st);

    /* configure in standby: IIR pressure coefficient 1 (weak), temperature unfiltered,
     * oversampling pressure x16 / temperature x2, then normal mode at 5 Hz */
    ESP_RETURN_ON_ERROR(bmp_wr(BMP_ODR_CONFIG, BMP_ODR_DEEP_DIS), TAG, "BMP581 standby");
    delay_ms(3);
    ESP_RETURN_ON_ERROR(bmp_wr(BMP_DSP_IIR, 1 << 3), TAG, "BMP581 IIR");
    ESP_RETURN_ON_ERROR(bmp_wr(BMP_OSR_CONFIG, 0x40 | (4 << 3) | 1), TAG, "BMP581 OSR");
    ESP_RETURN_ON_ERROR(bmp_wr(BMP_ODR_CONFIG, BMP_ODR_DEEP_DIS | BMP_ODR_5HZ | BMP_MODE_NORMAL), TAG, "BMP581 ODR");
    uint8_t eff;
    if (bmp_rd(BMP_OSR_EFF, &eff, 1) == ESP_OK && !(eff & 0x80)) {
        ESP_LOGW(TAG, "BMP581: ODR not valid for the OSR (0x%02x)", eff);
    }
    s_dev[DEV_BMP].skip = WARMUP[DEV_BMP];
    return ESP_OK;
}

/* ---------- device state ---------- */

static void dev_init(int d)
{
    sensor_dev_t *s = &s_dev[d];
    s->ok = false;
    s->fails = 0;
    if (i2c_master_probe(s_bus, ADDR[d], I2C_TIMEOUT_MS) != ESP_OK) {
        return;
    }
    esp_err_t err = d == DEV_SHT ? sht_init() : d == DEV_BMP ? bmp_init() : scd_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s (0x%02x) answers but init failed: %s", NAME[d], ADDR[d], esp_err_to_name(err));
        return;
    }
    if (d == DEV_SHT) {
        s->skip = WARMUP[DEV_SHT];
    }
    s->ok = true;
    ESP_LOGI(TAG, "%s (0x%02x) ready", NAME[d], ADDR[d]);
}

/* A failed read: after FAIL_LIMIT in a row the sensor is dropped until the next reprobe */
static void dev_failed(int d, esp_err_t err)
{
    sensor_dev_t *s = &s_dev[d];
    if (++s->fails >= FAIL_LIMIT) {
        ESP_LOGW(TAG, "%s lost (%s)", NAME[d], esp_err_to_name(err));
        s->ok = false;
        s_bus_suspect = true;
    }
}

/* A good read: false while it is still a warm-up reading */
static bool dev_good(int d)
{
    sensor_dev_t *s = &s_dev[d];
    s->fails = 0;
    if (s->skip) {
        s->skip--;
        return false;
    }
    return true;
}

static uint8_t present_mask(void)
{
    uint8_t m = 0;
    for (int d = 0; d < DEV_COUNT; d++) {
        if (s_dev[d].ok) {
            m |= 1u << d;
        }
    }
    return m;
}

/* ---------- backend ---------- */

static esp_err_t real_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    }
    esp_err_t err = bus_create();
    if (err == ESP_OK) {
        for (int d = 0; d < DEV_COUNT; d++) {
            dev_init(d);
        }
    }
    for (int d = 0; d < DEV_COUNT; d++) {
        ESP_LOGI(TAG, "PORT.A 0x%02x %s: %s", ADDR[d], NAME[d], s_dev[d].ok ? "found" : "not found");
    }
    if (err != ESP_OK || !present_mask()) {
        bus_destroy();
        return ESP_ERR_NOT_FOUND;
    }
    s_next_probe = REPROBE_S;
    return ESP_OK;
}

static void real_begin_tick(uint32_t uptime_s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dev[DEV_SCD].ok && s_asc_req >= 0 && s_asc_req != s_asc_cur && scd_restart() != ESP_OK) {
        dev_failed(DEV_SCD, ESP_FAIL);
    }
    if (uptime_s >= s_next_probe && present_mask() != (1u << DEV_COUNT) - 1) {
        s_next_probe = uptime_s + REPROBE_S;
        if (s_bus_suspect || !present_mask()) {
            /* a sensor dropped out: free the bus before talking to it again */
            uint8_t keep = present_mask();
            bus_destroy();
            s_bus_suspect = false;
            if (bus_create() == ESP_OK) {
                for (int d = 0; d < DEV_COUNT; d++) {
                    dev_init(d);
                }
            }
            if (present_mask() != keep) {
                ESP_LOGI(TAG, "sensors after bus reset: 0x%02x (was 0x%02x)", present_mask(), keep);
            }
        } else {
            for (int d = 0; d < DEV_COUNT; d++) {
                if (!s_dev[d].ok) {
                    dev_init(d);
                }
            }
        }
    }
    xSemaphoreGive(s_lock);
}

static bool real_read_sht(float *t, float *rh)
{
    if (!s_dev[DEV_SHT].ok) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t w[2];
    esp_err_t err = sen_read(DEV_SHT, SHT_SINGLE_HIGH, 16, w, 2);  /* max 15.5 ms */
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        dev_failed(DEV_SHT, err);
        return false;
    }
    if (!dev_good(DEV_SHT)) {
        return false;
    }
    *t = sensirion_temp(w[0]);
    *rh = sensirion_rh(w[1]);
    return true;
}

static bool real_read_bmp(float *p, float *t)
{
    if (!s_dev[DEV_BMP].ok) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t b[6];
    esp_err_t err = bmp_rd(BMP_DATA, b, sizeof(b));
    xSemaphoreGive(s_lock);
    if (err == ESP_OK && b[3] == 0 && b[4] == 0 && b[5] == 0) {
        err = ESP_ERR_INVALID_RESPONSE;     /* no conversion yet (or reset) */
    }
    if (err != ESP_OK) {
        dev_failed(DEV_BMP, err);
        return false;
    }
    if (!dev_good(DEV_BMP)) {
        return false;
    }
    *t = bmp581_temp(b);
    *p = bmp581_pres_hpa(b);
    return true;
}

static bool real_read_scd(float *co2, float *t, float *rh)
{
    if (!s_dev[DEV_SCD].ok) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t rdy, w[3];
    esp_err_t err = sen_read(DEV_SCD, SCD_DATA_READY, 1, &rdy, 1);
    bool ready = err == ESP_OK && (rdy & 0x07FF);
    if (ready) {
        err = sen_read(DEV_SCD, SCD_READ_MEAS, 1, w, 3);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        dev_failed(DEV_SCD, err);
        return false;
    }
    if (!ready) {
        s_dev[DEV_SCD].fails = 0;
        return false;
    }
    if (!dev_good(DEV_SCD) || w[0] == 0) {
        return false;
    }
    *co2 = w[0];
    *t = sensirion_temp(w[1]);
    *rh = sensirion_rh(w[2]);
    return true;
}

static void real_set_pressure(float hpa)
{
    if (!s_dev[DEV_SCD].ok || isnan(hpa)) {
        return;
    }
    uint16_t v = (uint16_t)lroundf(fminf(fmaxf(hpa, 700.0f), 1200.0f));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = sen_retry_cmd_arg(DEV_SCD, SCD_SET_PRESSURE, v);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SCD41 set pressure: %s", esp_err_to_name(err));
    }
}

/* Applied by the sensor task at its next tick (the SCD41 must be stopped for it) */
static esp_err_t real_set_asc(bool enable)
{
    s_asc_req = enable ? 1 : 0;
    return ESP_OK;
}

static esp_err_t real_frc(uint16_t target_ppm, int16_t *correction)
{
    if (!s_dev[DEV_SCD].ok) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = sen_retry_cmd(DEV_SCD, SCD_STOP_PERIODIC);
    uint16_t w = 0xFFFF;
    if (err == ESP_OK) {
        delay_ms(500);
        err = sen_retry_cmd_arg(DEV_SCD, SCD_FRC, target_ppm);
    }
    if (err == ESP_OK) {
        delay_ms(400);
        uint8_t b[3];
        err = i2c_master_receive(s_dev[DEV_SCD].h, b, sizeof(b), I2C_TIMEOUT_MS);
        if (err == ESP_OK && !sensirion_words(b, 1, &w)) {
            err = ESP_ERR_INVALID_CRC;
        }
    }
    esp_err_t start = sen_retry_cmd(DEV_SCD, SCD_START_PERIODIC);
    s_dev[DEV_SCD].skip = WARMUP[DEV_SCD];
    xSemaphoreGive(s_lock);

    if (err == ESP_OK && !scd41_frc_correction(w, correction)) {
        err = ESP_FAIL;
    }
    ESP_LOGI(TAG, "SCD41 FRC %u ppm: %s (correction %d)", target_ppm, esp_err_to_name(err), err == ESP_OK ? *correction : 0);
    if (start != ESP_OK) {
        dev_failed(DEV_SCD, start);
    }
    return err;
}

const sensor_backend_t backend_real = {
    .name = "PORT.A",
    .is_sim = false,
    .init = real_init,
    .begin_tick = real_begin_tick,
    .read_sht = real_read_sht,
    .read_bmp = real_read_bmp,
    .read_scd = real_read_scd,
    .set_ambient_pressure = real_set_pressure,
    .set_asc = real_set_asc,
    .frc = real_frc,
    .present = present_mask,
};
