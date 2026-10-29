idf_component_register(SRCS "controller.c"
                       INCLUDE_DIRS "include"
                       REQUIRES core
                       PRIV_REQUIRES settings sensors datastore audio display esp_timer)
