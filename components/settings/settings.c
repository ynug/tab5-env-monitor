#include "settings.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NVS_NS      "settings"
#define NVS_KEY     "v2"
#define MAX_LISTENERS 6

static app_settings_t s_cur;
static SemaphoreHandle_t s_mutex;
static settings_listener_t s_listeners[MAX_LISTENERS];
static int s_nlisteners;

static void defaults(app_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->brightness = 80;
    for (int i = 0; i < AL_COUNT; i++) {
        s->alarm_enabled[i] = true;
    }
    s->volume = 70;
    s->scd41_asc = true;
    s->sd_auto_export = true;
}

void settings_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    defaults(&s_cur);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        app_settings_t tmp;
        size_t len = sizeof(tmp);
        if (nvs_get_blob(h, NVS_KEY, &tmp, &len) == ESP_OK && len == sizeof(tmp)) {
            s_cur = tmp;
        } else {
            ESP_LOGI(TAG, "no stored settings, using defaults");
        }
        nvs_close(h);
    }
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "v1");     /* layout before the sleep mode was removed */
        nvs_commit(h);
        nvs_close(h);
    }
}

void settings_get(app_settings_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_mutex);
}

void settings_set(const app_settings_t *s)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = memcmp(&s_cur, s, sizeof(*s)) != 0;
    s_cur = *s;
    xSemaphoreGive(s_mutex);
    if (!changed) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY, s, sizeof(*s));
        nvs_commit(h);
        nvs_close(h);
    }
    for (int i = 0; i < s_nlisteners; i++) {
        s_listeners[i](s);
    }
}

void settings_add_listener(settings_listener_t cb)
{
    if (s_nlisteners < MAX_LISTENERS) {
        s_listeners[s_nlisteners++] = cb;
    }
}

void settings_reset(void)
{
    app_settings_t d;
    defaults(&d);
    settings_set(&d);
}
