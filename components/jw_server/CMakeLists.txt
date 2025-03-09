
idf_component_register(SRCS "jw_keep_alive.c" "jw_server.c"
                       INCLUDE_DIRS "." "html"
                       REQUIRES cJSON esp_http_server jw_common
                       PRIV_REQUIRES cJSON fatfs esp_wifi esp_http_server esp_timer jw_wifi jw_rtc jw_sdcard jw_log jw_espnow jw_peers 
                       EMBED_FILES 
                            "html/favicon.ico"
                            "html/jquery.js"
                            "html/main.css"
                            "html/header_script.html"
                            "html/footer_script.html"
                            "html/device.html"
                            "html/root.html"
                            "html/devices.html")