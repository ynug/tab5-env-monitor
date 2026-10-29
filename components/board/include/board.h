#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    BOARD_LCD_UNKNOWN = 0,
    BOARD_LCD_ILI9881C,
    BOARD_LCD_ST7123,
    BOARD_LCD_ST7121,
} board_lcd_t;

/* Internal I2C, LCD revision probe, PORT.A 5V output, SoC temperature sensor. Call once before display_init(). */
esp_err_t board_init(void);

/* Free a bus whose slave holds SDA low (SCL pulses + STOP). The pins are reset afterwards. */
void board_i2c_recover(int sda, int scl);

board_lcd_t board_lcd_type(void);
const char *board_lcd_name(void);
bool board_ext5v_enabled(void);
/* ESP32-P4 die temperature in degC, NAN if unavailable */
float board_soc_temp(void);

/*
 * RX8130 RTC, kept in UTC. A read is only trusted if this firmware wrote it (marker in the RTC
 * user RAM), the voltage-loss flag is clear and the date is plausible.
 */
bool board_rtc_read(int64_t *unix_s);
bool board_rtc_write(int64_t unix_s);
