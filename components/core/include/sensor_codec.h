#pragma once

/*
 * Byte-level encoding of the SHT35 / SCD41 / BMP581 data (pure C, no ESP-IDF dependency).
 * Sensirion words are big-endian 16 bit followed by a CRC-8 (poly 0x31, init 0xFF).
 */
#include <stdbool.h>
#include <stdint.h>

uint8_t sensirion_crc8(const uint8_t *data, int len);

/* Check the CRC of n words (3 bytes each) and extract them. False on any CRC mismatch. */
bool sensirion_words(const uint8_t *buf, int n, uint16_t *words);

/* Write a 16-bit argument with its CRC (3 bytes) */
void sensirion_put_word(uint8_t *out, uint16_t word);

/* SHT3x / SCD4x: T = -45 + 175 * raw / 65535, RH = 100 * raw / 65535 */
float sensirion_temp(uint16_t raw);
float sensirion_rh(uint16_t raw);

/* SCD41 perform_forced_recalibration answer: false if it failed (0xFFFF) */
bool scd41_frc_correction(uint16_t word, int16_t *correction_ppm);

/* BMP581 data registers 0x1D..0x22 (temperature XLSB first, then pressure XLSB first) */
float bmp581_temp(const uint8_t *b);        /* degC */
float bmp581_pres_hpa(const uint8_t *b);    /* hPa, from b[3..5] */
