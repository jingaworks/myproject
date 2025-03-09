#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "jw_common.h"
#include "jw_wifi.h"
#include "jw_rtc.h"
#include "jw_sdcard.h"
#include "jw_log.h"
#include "jw_server.h"
#include "jw_espnow.h"
#include "jw_peers.h"

void app_main() {
    jw_wifi_init();
    jw_rtc_init();
    jw_sdcard_init();
    jw_log_init();
    jw_server_core_init();             // Core server setup
    jw_server_http_start(jw_server_core_get_server()); // HTTP endpoints
    jw_server_ws_start(jw_server_core_get_server());   // WebSocket endpoints
    jw_espnow_init();
    jw_peers_init();
    while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
}