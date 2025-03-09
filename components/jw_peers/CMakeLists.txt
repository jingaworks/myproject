idf_component_register(SRCS "jw_peers.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi
                       PRIV_REQUIRES nvs_flash esp_wifi jw_log)