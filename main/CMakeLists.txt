idf_component_register(SRCS "fan.c" "debug.c" "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_uart esp_timer)
