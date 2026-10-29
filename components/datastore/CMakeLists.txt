idf_component_register(SRCS "datastore.c"
                       INCLUDE_DIRS "include"
                       REQUIRES core
                       PRIV_REQUIRES esp_partition nvs_flash esp_timer)
