idf_component_register(SRCS "display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board esp_lcd esp_driver_ppa esp_timer lvgl__lvgl
                       PRIV_REQUIRES esp_mm)
