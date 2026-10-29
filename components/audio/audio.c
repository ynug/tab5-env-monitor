#include "audio.h"

#include <string.h>
#include "bsp/m5stack_tab5.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define SAMPLE_RATE 16000
#define QUEUE_LEN   8
#define GAP_MS      1000        /* pause between queued announcements */
#define CHUNK       1024
#define TASK_PRIO   6
#define TASK_CORE   1

#define WAV(name)                                                         \
    extern const uint8_t name##_wav_start[] asm("_binary_" #name "_wav_start"); \
    extern const uint8_t name##_wav_end[] asm("_binary_" #name "_wav_end");
WAV(co2_caution)
WAV(co2_warning)
WAV(heat_caution)
WAV(heat_severe)
WAV(heat_danger)
WAV(humidity_low)
WAV(humidity_high)
WAV(temp_low)
WAV(pressure_drop)

static const struct {
    const uint8_t *start, *end;
} CLIPS[SND_COUNT] = {
    [SND_CO2_CAUTION] = {co2_caution_wav_start, co2_caution_wav_end},
    [SND_CO2_WARNING] = {co2_warning_wav_start, co2_warning_wav_end},
    [SND_HEAT_CAUTION] = {heat_caution_wav_start, heat_caution_wav_end},
    [SND_HEAT_SEVERE] = {heat_severe_wav_start, heat_severe_wav_end},
    [SND_HEAT_DANGER] = {heat_danger_wav_start, heat_danger_wav_end},
    [SND_HUM_LOW] = {humidity_low_wav_start, humidity_low_wav_end},
    [SND_HUM_HIGH] = {humidity_high_wav_start, humidity_high_wav_end},
    [SND_TEMP_LOW] = {temp_low_wav_start, temp_low_wav_end},
    [SND_PRES_DROP] = {pressure_drop_wav_start, pressure_drop_wav_end},
};

static esp_codec_dev_handle_t s_spk;
static QueueHandle_t s_queue;
static volatile bool s_busy;
static int s_volume = 70;

/* Locate the PCM payload of a RIFF/WAVE file */
static bool wav_data(const uint8_t *p, const uint8_t *end, const uint8_t **data, size_t *len)
{
    if (end - p < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) {
        return false;
    }
    p += 12;
    while (end - p >= 8) {
        uint32_t sz = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
        if (!memcmp(p, "data", 4)) {
            *data = p + 8;
            *len = sz <= (size_t)(end - p - 8) ? sz : (size_t)(end - p - 8);
            return true;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}

static void play_clip(sound_t s)
{
    const uint8_t *pcm;
    size_t len;
    if (!wav_data(CLIPS[s].start, CLIPS[s].end, &pcm, &len)) {
        ESP_LOGE(TAG, "bad clip %d", s);
        return;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    bsp_feature_enable(BSP_FEATURE_SPEAKER, true);
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "open failed");
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = len - off < CHUNK ? len - off : CHUNK;
        esp_codec_dev_write(s_spk, (void *)(pcm + off), n);
    }
    /* let the DMA drain before closing */
    static const uint8_t silence[CHUNK];
    esp_codec_dev_write(s_spk, (void *)silence, sizeof(silence));
    esp_codec_dev_close(s_spk);
    bsp_feature_enable(BSP_FEATURE_SPEAKER, false);
}

static void task(void *arg)
{
    sound_t s;
    for (;;) {
        xQueueReceive(s_queue, &s, portMAX_DELAY);
        s_busy = true;
        ESP_LOGI(TAG, "play %d", s);
        play_clip(s);
        if (uxQueueMessagesWaiting(s_queue)) {
            vTaskDelay(pdMS_TO_TICKS(GAP_MS));
        }
        s_busy = uxQueueMessagesWaiting(s_queue) > 0;
    }
}

esp_err_t audio_init(void)
{
    s_spk = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(s_spk, ESP_FAIL, TAG, "speaker codec");
    bsp_feature_enable(BSP_FEATURE_SPEAKER, false);
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(sound_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    BaseType_t ok = xTaskCreatePinnedToCore(task, "audio", 4096, NULL, TASK_PRIO, NULL, TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void audio_set_volume(int percent)
{
    s_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

bool audio_play(sound_t s)
{
    if (!s_queue || s >= SND_COUNT) {
        return false;
    }
    return xQueueSend(s_queue, &s, 0) == pdTRUE;
}

bool audio_busy(void)
{
    return s_busy;
}
