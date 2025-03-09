idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES cJSON nvs_flash esp_timer esp_wifi esp_http_server jw_wifi jw_rtc jw_server jw_sdcard jw_log jw_common jw_peers jw_espnow) # Added jw_log