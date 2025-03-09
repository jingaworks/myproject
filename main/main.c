#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "jw_common.h"
#include "jw_wifi.h"
#include "jw_server.h"
#include "jw_rtc.h"
#include "jw_sdcard.h"
#include "jw_log.h"

static const char *TAG = "MAIN";

static void data_callback(jw_server_data_type_t type, char *buffer, size_t max_len, size_t *len) {
    ESP_LOGD(TAG, "Data callback called, type: %d, max_len: %u", type, max_len);
    if (buffer && max_len > 0) {
        buffer[0] = '\0';
        *len = 0;
    }
}

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_DEBUG);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    ESP_ERROR_CHECK(jw_wifi_init());
    ESP_ERROR_CHECK(jw_rtc_init());
    ESP_ERROR_CHECK(jw_server_init());
    ESP_ERROR_CHECK(jw_sdcard_init());
    ESP_ERROR_CHECK(jw_log_init());

    QueueHandle_t device_update_queue = xQueueCreate(10, sizeof(jw_server_update_data_t *));
    EventGroupHandle_t event_group = xEventGroupCreate();
    if (!device_update_queue || !event_group) {
        ESP_LOGE(TAG, "Failed to create queue or event group");
        return;
    }

    jw_server_params_impl_t server_params = {
        .device_update_queue = &device_update_queue,
        .data_callback = data_callback,
        .event_group = event_group
    };

    const jw_wifi_server_interface_t server_if = {
        .server_start = jw_server_start,
        .server_stop = jw_server_stop,
        .is_server_running = jw_server_is_running,
        .params = &server_params
    };

    ESP_LOGD(TAG, "Setting server interface: start=%p, stop=%p, is_running=%p, params=%p",
        server_if.server_start, server_if.server_stop, server_if.is_server_running, server_if.params);
    jw_wifi_set_server_interface(&server_if);

    ESP_ERROR_CHECK(jw_wifi_start());
    ESP_ERROR_CHECK(jw_server_start(&server_params));

    ESP_LOGI(TAG, "Server started successfully");
    ESP_LOGI(TAG, "Application started");
}
