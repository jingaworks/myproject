idf_component_register(SRCS "jw_espnow.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi jw_peers
                       PRIV_REQUIRES esp_wifi jw_server)