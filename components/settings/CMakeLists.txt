idf_component_register(SRCS "settings.c"
                       INCLUDE_DIRS "include"
                       REQUIRES core
                       PRIV_REQUIRES nvs_flash)
