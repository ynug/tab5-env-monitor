idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES board display ui sensors settings audio controller sdlog nvs_flash)
