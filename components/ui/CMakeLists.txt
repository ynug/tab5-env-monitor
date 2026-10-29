file(GLOB UI_FONTS "fonts/*.c")
idf_component_register(SRCS "ui.c" "ui_theme.c" "page_sensor.c" "page_settings.c" "page_demo.c" "page_graph.c" "ui_debug.c"
                            ${UI_FONTS}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "." "fonts"
                       REQUIRES lvgl__lvgl
                       PRIV_REQUIRES display board sensors datastore core controller settings audio sdlog esp_timer esp_system esp_app_format mbedtls vfs esp_driver_usb_serial_jtag)
