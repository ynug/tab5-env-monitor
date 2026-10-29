#include "board.h"

#include <math.h>
#include <string.h>
#include <time.h>
#include "bsp/m5stack_tab5.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "driver/temperature_sensor.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define TOUCH_ST712X_ADDR   0x55
#define TOUCH_GT911_ADDR    0x14
/* IO expander 0x43 P2 = EXT5V_EN (PORT.A / M5-Bus / 2.54-10P 5V output) */
#define EXT5V_EN_PIN        IO_EXPANDER_PIN_NUM_2

static board_lcd_t s_lcd = BOARD_LCD_UNKNOWN;
static bool s_ext5v;
static temperature_sensor_handle_t s_tsens;

static board_lcd_t probe_lcd(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    /* ST712x is a TDDI (touch inside the LCD driver): the LCD must be enabled too.
     * The controller takes a few hundred ms to answer after power-up. */
    bsp_feature_enable(BSP_FEATURE_LCD, true);
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    bool st712x = false, gt911 = false;
    for (int i = 0; i < 30 && !st712x && !gt911; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        st712x = i2c_master_probe(bus, TOUCH_ST712X_ADDR, 50) == ESP_OK;
        gt911 = !st712x && i2c_master_probe(bus, TOUCH_GT911_ADDR, 50) == ESP_OK;
    }

    if (st712x) {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TOUCH_ST712X_ADDR,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
            return BOARD_LCD_UNKNOWN;
        }
        const uint8_t reg[2] = {0x00, 0x00};  /* firmware version register */
        uint8_t fw = 0;
        esp_err_t err = ESP_FAIL;
        /* answers its address before the firmware is up: retry the version read */
        for (int i = 0; i < 5; i++) {
            err = i2c_master_transmit_receive(dev, reg, sizeof(reg), &fw, 1, 100);
            if (err == ESP_OK && (fw == 1 || fw == 3)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        i2c_master_bus_rm_device(dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ST712x firmware version read failed: %s", esp_err_to_name(err));
            return BOARD_LCD_UNKNOWN;
        }
        if (fw == 1) {
            return BOARD_LCD_ST7121;
        }
        if (fw == 3) {
            return BOARD_LCD_ST7123;
        }
        ESP_LOGW(TAG, "unknown ST712x firmware version %u", fw);
        return BOARD_LCD_UNKNOWN;
    }
    return gt911 ? BOARD_LCD_ILI9881C : BOARD_LCD_UNKNOWN;
}

/*
 * A reset in the middle of a transfer can leave a slave holding SDA low, after which the
 * IO expander init (and the BSP) fails. Clock SCL until SDA is released, then send a STOP.
 */
void board_i2c_recover(int sda, int scl)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(sda) | BIT64(scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);

    int pulses = 0;
    while (gpio_get_level(sda) == 0 && pulses < 9) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(10);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(10);
        pulses++;
    }
    /* STOP: SDA low -> high while SCL high */
    gpio_set_level(sda, 0);
    esp_rom_delay_us(10);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(10);
    if (pulses) {
        ESP_LOGW(TAG, "I2C bus recovered after %d clock pulses (SDA %s)", pulses, gpio_get_level(sda) ? "released" : "still low");
    }
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
}

static esp_err_t ext5v_enable(void)
{
    esp_io_expander_handle_t io = bsp_io_expander_init();
    ESP_RETURN_ON_FALSE(io, ESP_FAIL, TAG, "io expander");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(io, EXT5V_EN_PIN, IO_EXPANDER_OUTPUT), TAG, "dir");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(io, EXT5V_EN_PIN, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(io, EXT5V_EN_PIN, 1), TAG, "level");
    return ESP_OK;
}

esp_err_t board_init(void)
{
    board_i2c_recover(BSP_I2C_SDA, BSP_I2C_SCL);
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");

    s_lcd = probe_lcd();
    ESP_LOGI(TAG, "LCD: %s", board_lcd_name());

    s_ext5v = ext5v_enable() == ESP_OK;
    ESP_LOGI(TAG, "EXT 5V: %s", s_ext5v ? "on" : "failed");

    const temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    } else {
        s_tsens = NULL;
    }
    return ESP_OK;
}

board_lcd_t board_lcd_type(void)
{
    return s_lcd;
}

const char *board_lcd_name(void)
{
    switch (s_lcd) {
    case BOARD_LCD_ILI9881C: return "ILI9881C + GT911";
    case BOARD_LCD_ST7123:   return "ST7123";
    case BOARD_LCD_ST7121:   return "ST7121";
    default:                 return "unknown";
    }
}

bool board_ext5v_enabled(void)
{
    return s_ext5v;
}

float board_soc_temp(void)
{
    float t;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK) {
        return t;
    }
    return NAN;
}

/* ---------- RX8130 ---------- */

#define RTC_ADDR        0x32
#define RTC_REG_TIME    0x10    /* sec, min, hour, weekday (bit), day, month, year (BCD) */
#define RTC_REG_RAM     0x20    /* 4 bytes of user RAM */
#define RTC_REG_FLAG    0x1D
#define RTC_FLAG_VLF    0x02    /* voltage loss: time is not reliable */
static const uint8_t RTC_MARK[4] = {'T', '5', 'U', 'C'};

static i2c_master_dev_handle_t rtc_dev(void)
{
    static i2c_master_dev_handle_t dev;
    if (!dev) {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = RTC_ADDR,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &dev) != ESP_OK) {
            dev = NULL;
        }
    }
    return dev;
}

static bool rtc_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_master_dev_handle_t dev = rtc_dev();
    return dev && i2c_master_transmit_receive(dev, &reg, 1, buf, len, 100) == ESP_OK;
}

static bool rtc_write_regs(uint8_t reg, const uint8_t *buf, size_t len)
{
    i2c_master_dev_handle_t dev = rtc_dev();
    uint8_t tmp[9];
    if (!dev || len > sizeof(tmp) - 1) {
        return false;
    }
    tmp[0] = reg;
    memcpy(tmp + 1, buf, len);
    return i2c_master_transmit(dev, tmp, len + 1, 100) == ESP_OK;
}

static bool bcd_ok(uint8_t v)
{
    return (v & 0x0F) <= 9 && (v >> 4) <= 9;
}

static int from_bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0x0F);
}

static uint8_t to_bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

bool board_rtc_read(int64_t *unix_s)
{
    uint8_t flag, mark[4], t[7];
    if (!rtc_read_regs(RTC_REG_FLAG, &flag, 1) || (flag & RTC_FLAG_VLF)) {
        return false;
    }
    if (!rtc_read_regs(RTC_REG_RAM, mark, 4) || memcmp(mark, RTC_MARK, 4) != 0) {
        return false;
    }
    if (!rtc_read_regs(RTC_REG_TIME, t, 7)) {
        return false;
    }
    uint8_t sec = t[0] & 0x7F, min = t[1] & 0x7F, hour = t[2] & 0x3F, day = t[4] & 0x3F, mon = t[5] & 0x1F, yr = t[6];
    if (!bcd_ok(sec) || !bcd_ok(min) || !bcd_ok(hour) || !bcd_ok(day) || !bcd_ok(mon) || !bcd_ok(yr)) {
        return false;
    }
    struct tm tm = {
        .tm_sec = from_bcd(sec), .tm_min = from_bcd(min), .tm_hour = from_bcd(hour),
        .tm_mday = from_bcd(day), .tm_mon = from_bcd(mon) - 1, .tm_year = from_bcd(yr) + 100,
    };
    if (tm.tm_year < 125 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_hour > 23 ||
        tm.tm_min > 59 || tm.tm_sec > 59) {
        return false;
    }
    /* timegm is not available: compute UTC from the broken-down time */
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int y = tm.tm_year + 1900;
    int64_t days = (int64_t)(y - 1970) * 365 + ((y - 1969) / 4) - ((y - 1901) / 100) + ((y - 1601) / 400);
    days += cum[tm.tm_mon] + tm.tm_mday - 1;
    if (tm.tm_mon > 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
        days++;
    }
    *unix_s = days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    return true;
}

bool board_rtc_write(int64_t unix_s)
{
    time_t tt = (time_t)unix_s;
    struct tm tm;
    gmtime_r(&tt, &tm);
    const uint8_t t[7] = {
        to_bcd(tm.tm_sec), to_bcd(tm.tm_min), to_bcd(tm.tm_hour), (uint8_t)(1u << tm.tm_wday),
        to_bcd(tm.tm_mday), to_bcd(tm.tm_mon + 1), to_bcd(tm.tm_year % 100),
    };
    const uint8_t clear_vlf = (uint8_t)~RTC_FLAG_VLF;   /* write-0-to-clear */
    return rtc_write_regs(RTC_REG_TIME, t, 7) && rtc_write_regs(RTC_REG_RAM, RTC_MARK, 4) &&
           rtc_write_regs(RTC_REG_FLAG, &clear_vlf, 1);
}
