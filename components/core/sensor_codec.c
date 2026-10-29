#include "sensor_codec.h"

uint8_t sensirion_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

bool sensirion_words(const uint8_t *buf, int n, uint16_t *words)
{
    for (int i = 0; i < n; i++) {
        const uint8_t *w = buf + i * 3;
        if (sensirion_crc8(w, 2) != w[2]) {
            return false;
        }
        words[i] = (uint16_t)((w[0] << 8) | w[1]);
    }
    return true;
}

void sensirion_put_word(uint8_t *out, uint16_t word)
{
    out[0] = (uint8_t)(word >> 8);
    out[1] = (uint8_t)word;
    out[2] = sensirion_crc8(out, 2);
}

float sensirion_temp(uint16_t raw)
{
    return -45.0f + 175.0f * (float)raw / 65535.0f;
}

float sensirion_rh(uint16_t raw)
{
    return 100.0f * (float)raw / 65535.0f;
}

bool scd41_frc_correction(uint16_t word, int16_t *correction_ppm)
{
    if (word == 0xFFFF) {
        return false;
    }
    *correction_ppm = (int16_t)((int32_t)word - 0x8000);
    return true;
}

float bmp581_temp(const uint8_t *b)
{
    int32_t raw = (int32_t)((uint32_t)b[2] << 16 | (uint32_t)b[1] << 8 | b[0]);
    if (raw & 0x800000) {
        raw -= 0x1000000;
    }
    return (float)raw / 65536.0f;
}

float bmp581_pres_hpa(const uint8_t *b)
{
    uint32_t raw = (uint32_t)b[5] << 16 | (uint32_t)b[4] << 8 | b[3];
    return (float)raw / 64.0f / 100.0f;     /* Pa / 64 per LSB */
}
