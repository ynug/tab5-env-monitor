#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "audio.h"
#include "board.h"
#include "controller.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdlog.h"
#include "sensor_hub.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    setenv("TZ", "JST-9", 1);
    tzset();

    settings_init();
    ESP_ERROR_CHECK(board_init());

    /* System time from the RTC (set by hand on the settings page) */
    int64_t rtc;
    if (board_rtc_read(&rtc)) {
        struct timeval tv = {.tv_sec = rtc};
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "time from RTC: %lld", (long long)rtc);
    } else {
        ESP_LOGI(TAG, "RTC time not valid");
    }
    ESP_ERROR_CHECK(sensor_hub_start());
    ESP_ERROR_CHECK(display_init());
    if (audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio unavailable");
    }
    ESP_ERROR_CHECK(controller_start());
    ui_init();
    sdlog_start();
    ESP_LOGI(TAG, "internal RAM free %u KB (largest block %u KB)", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

    ESP_LOGI(TAG, "started");
}
