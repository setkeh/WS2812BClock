idf_component_register(SRCS "debug.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_timer)