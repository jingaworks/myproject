idf_component_register(SRCS "jw_log.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi jw_sdcard jw_rtc)