set(ota_embed "")
if(CONFIG_OTA_CERT_SOURCE_EMBEDDED)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/certs/server_ca.pem")
        message(FATAL_ERROR
            "OTA: 'Embedded certificate' is selected but "
            "components/ota/certs/server_ca.pem is missing.\n"
            "Copy the update server's root CA there (see certs/README.md), "
            "or choose the certificate-bundle option in menuconfig.")
    endif()
    set(ota_embed "certs/server_ca.pem")
endif()

idf_component_register(SRCS "ota.c"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES ${ota_embed}
                       PRIV_REQUIRES app_update esp_https_ota esp_http_client esp-tls nvs_flash json)
