idf_component_register(SRCS "sensor_hub.c" "backend_sim.c" "backend_real.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES core
                       PRIV_REQUIRES datastore board esp_driver_i2c esp_timer esp_hw_support)
