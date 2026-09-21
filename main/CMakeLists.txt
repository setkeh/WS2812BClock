idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event
                       PRIV_REQUIRES nvs_flash fan debug wifi ntp)
