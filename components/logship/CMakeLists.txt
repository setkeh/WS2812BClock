idf_component_register(SRCS "logship.c" "coredump_report.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_timer espcoredump lwip wifi)
