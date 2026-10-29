idf_component_register(SRCS "sdlog.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES core datastore settings fatfs sdmmc vfs esp_timer)
