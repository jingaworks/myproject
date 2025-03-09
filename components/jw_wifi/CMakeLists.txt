idf_component_register(SRCS "jw_wifi.c"
                       INCLUDE_DIRS "."
                       REQUIRES 
                            esp_wifi 
                            jw_common
                       PRIV_REQUIRES nvs_flash)