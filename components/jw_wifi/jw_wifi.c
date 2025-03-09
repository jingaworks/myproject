#include "jw_wifi.h"
#include <esp_log.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <string.h>
#include <stdbool.h>

/** @brief Logging tag for Wi-Fi module */
static const char *TAG = "JW_WIFI";

/** @brief Mutex for Wi-Fi settings access */
static SemaphoreHandle_t jw_wifi_settings_mutex;
/** @brief Current Wi-Fi settings */
static jw_wifi_settings_t jw_wifi_settings = { 0 };
/** @brief Flag indicating settings need applying */
static bool jw_wifi_settings_dirty = false;
/** @brief Event group for Wi-Fi events */
static EventGroupHandle_t jw_wifi_event_group;
/** @brief Server interface for Wi-Fi module */
static const jw_wifi_server_interface_t *wifi_server_if = NULL;

/** @brief Prototype for static event handler */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

/**
 * @brief Wi-Fi event handler
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "STA disconnected, reconnecting...");
                esp_wifi_connect();
                xEventGroupClearBits(jw_wifi_event_group, JW_WIFI_GOT_IP_BIT);
                break;
            default:
                break;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        jw_wifi_settings.sta.ip = event->ip_info.ip;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(jw_wifi_event_group, JW_WIFI_GOT_IP_BIT);
    }
}

/**
 * @brief Initialize the Wi-Fi module
 */
esp_err_t jw_wifi_init(void) {
    ESP_LOGI(TAG, "Initializing Wi-Fi");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    jw_wifi_settings_mutex = xSemaphoreCreateMutex();
    if (!jw_wifi_settings_mutex) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi settings mutex");
        return ESP_FAIL;
    }

    jw_wifi_event_group = xEventGroupCreate();
    if (!jw_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        vSemaphoreDelete(jw_wifi_settings_mutex);
        return ESP_FAIL;
    }

    // Assume esp_netif_init() and esp_event_loop_create_default() are called in main.c
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Wi-Fi: %s", esp_err_to_name(err));
        vEventGroupDelete(jw_wifi_event_group);
        vSemaphoreDelete(jw_wifi_settings_mutex);
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        vEventGroupDelete(jw_wifi_event_group);
        vSemaphoreDelete(jw_wifi_settings_mutex);
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
        esp_wifi_deinit();
        vEventGroupDelete(jw_wifi_event_group);
        vSemaphoreDelete(jw_wifi_settings_mutex);
        return err;
    }

    jw_wifi_settings.mode = JW_WIFI_MODE_APSTA;
    strcpy(jw_wifi_settings.ap.ssid, "ESP32_AP");
    strcpy(jw_wifi_settings.ap.pass, "password123");
    jw_wifi_settings.channel = 1;
    jw_wifi_settings.country_code = WIFI_COUNTRY_CODE_US;

    return ESP_OK;
}

/**
 * @brief Start the Wi-Fi module
 */
esp_err_t jw_wifi_start(void) {
    ESP_LOGI(TAG, "Starting Wi-Fi");
    esp_err_t err = jw_wifi_apply();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply Wi-Fi settings: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGD(TAG, "Creating Wi-Fi manager task");
    if (xTaskCreate(jw_wifi_manager_task, "wifi_manager", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi manager task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
// esp_err_t jw_wifi_start(void) {
//     ESP_LOGI(TAG, "Starting Wi-Fi");
//     return jw_wifi_apply();
// }

/**
 * @brief Apply current Wi-Fi settings
 */
esp_err_t jw_wifi_apply(void) {
    xSemaphoreTake(jw_wifi_settings_mutex, portMAX_DELAY);

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "Failed to stop Wi-Fi: %s", esp_err_to_name(err));
        xSemaphoreGive(jw_wifi_settings_mutex);
        return err;
    }

    wifi_config_t wifi_config = { 0 };
    ESP_LOGW(TAG, "mode %d", jw_wifi_settings.mode);
    switch (jw_wifi_settings.mode) {
        case JW_WIFI_MODE_STA:
            err = esp_wifi_set_mode(WIFI_MODE_STA);
            strlcpy((char *)wifi_config.sta.ssid, jw_wifi_settings.sta.ssid, sizeof(wifi_config.sta.ssid));
            strlcpy((char *)wifi_config.sta.password, jw_wifi_settings.sta.pass, sizeof(wifi_config.sta.password));
            wifi_config.sta.channel = jw_wifi_settings.channel;
            err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            break;
        case JW_WIFI_MODE_AP:
            err = esp_wifi_set_mode(WIFI_MODE_AP);
            strlcpy((char *)wifi_config.ap.ssid, jw_wifi_settings.ap.ssid, sizeof(wifi_config.ap.ssid));
            strlcpy((char *)wifi_config.ap.password, jw_wifi_settings.ap.pass, sizeof(wifi_config.ap.password));
            wifi_config.ap.channel = jw_wifi_settings.channel;
            wifi_config.ap.max_connection = 4;
            wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
            err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
            break;
        case JW_WIFI_MODE_APSTA:
            err = esp_wifi_set_mode(WIFI_MODE_APSTA);
            strlcpy((char *)wifi_config.sta.ssid, "TPLink_G", sizeof(wifi_config.sta.ssid));
            strlcpy((char *)wifi_config.sta.password, "L30nt3_123", sizeof(wifi_config.sta.password));
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            // strlcpy((char *)wifi_config.sta.ssid, jw_wifi_settings.sta.ssid, sizeof(wifi_config.sta.ssid));
            // strlcpy((char *)wifi_config.sta.password, jw_wifi_settings.sta.pass, sizeof(wifi_config.sta.password));
            wifi_config.sta.channel = jw_wifi_settings.channel;
            strlcpy((char *)wifi_config.ap.ssid, jw_wifi_settings.ap.ssid, sizeof(wifi_config.ap.ssid));
            strlcpy((char *)wifi_config.ap.password, jw_wifi_settings.ap.pass, sizeof(wifi_config.ap.password));
            wifi_config.ap.channel = jw_wifi_settings.channel;
            wifi_config.ap.max_connection = 4;
            wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
            err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
            break;
        case JW_WIFI_MODE_NO_WIFI:
            err = ESP_OK;
            break;
        default:
            ESP_LOGE(TAG, "Invalid Wi-Fi mode: %d", jw_wifi_settings.mode);
            xSemaphoreGive(jw_wifi_settings_mutex);
            return ESP_ERR_INVALID_ARG;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Wi-Fi config: %s", esp_err_to_name(err));
        xSemaphoreGive(jw_wifi_settings_mutex);
        return err;
    }

    if (jw_wifi_settings.mode != JW_WIFI_MODE_NO_WIFI) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Wi-Fi: %s", esp_err_to_name(err));
            xSemaphoreGive(jw_wifi_settings_mutex);
            return err;
        }
    }

    jw_wifi_settings_dirty = false;
    xSemaphoreGive(jw_wifi_settings_mutex);
    return ESP_OK;
}

/**
 * @brief Save Wi-Fi configuration to NVS
 */
esp_err_t jw_wifi_save_config(const char *key, const char *value) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi_config", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set NVS value for %s: %s", key, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return err;
}

/**
 * @brief Get current Wi-Fi settings
 */
jw_wifi_settings_t *jw_wifi_get_settings(void) {
    return &jw_wifi_settings;
}

/**
 * @brief Get Wi-Fi event group
 */
EventGroupHandle_t jw_wifi_get_event_group(void) {
    return jw_wifi_event_group;
}

/**
 * @brief Set Wi-Fi mode
 */
esp_err_t jw_wifi_set_mode(jw_wifi_user_mode_t mode) {
    xSemaphoreTake(jw_wifi_settings_mutex, portMAX_DELAY);

    if (mode > JW_WIFI_MODE_APSTA) {
        ESP_LOGE(TAG, "Invalid mode: %d", mode);
        xSemaphoreGive(jw_wifi_settings_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    if (jw_wifi_settings.mode == JW_WIFI_MODE_APSTA && mode == JW_WIFI_MODE_STA) {
        EventBits_t bits = xEventGroupGetBits(jw_wifi_event_group);
        if (!(bits & JW_WIFI_GOT_IP_BIT)) {
            ESP_LOGW(TAG, "Cannot switch to STA mode: no active router connection");
            xSemaphoreGive(jw_wifi_settings_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    jw_wifi_settings.mode = mode;
    jw_wifi_settings_dirty = true;

    if (wifi_server_if) {
        if (mode == JW_WIFI_MODE_NO_WIFI) {
            wifi_server_if->server_stop();
        }
        else if (jw_wifi_settings.mode == JW_WIFI_MODE_NO_WIFI) {
            wifi_server_if->server_start(wifi_server_if->params);
        }
    }
    else {
        ESP_LOGW(TAG, "Server interface not set; skipping server state change");
    }

    esp_err_t err = jw_wifi_apply();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply new mode: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(jw_wifi_settings_mutex);
    return err;
}

/**
 * @brief Scan available Wi-Fi networks
 */
esp_err_t jw_wifi_scan_networks(wifi_ap_record_t *ap_records, uint16_t max_count, uint16_t *ap_count) {
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi scan: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_scan_get_ap_records(&max_count, ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_scan_get_ap_num(ap_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP count: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Found %d Wi-Fi networks", *ap_count);
    return ESP_OK;
}

/**
 * @brief Wi-Fi manager task
 */
void jw_wifi_manager_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting Wi-Fi manager task");
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(jw_wifi_event_group, JW_WIFI_GOT_IP_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & JW_WIFI_GOT_IP_BIT) {
            ESP_LOGD(TAG, "Checking server state");
            if (!wifi_server_if) {
                ESP_LOGE(TAG, "Server interface is NULL");
            }
            else if (!wifi_server_if->is_server_running) {
                ESP_LOGE(TAG, "is_server_running function pointer is NULL");
            }
            else {
                bool running = wifi_server_if->is_server_running();
                ESP_LOGD(TAG, "Server running: %d", running);
                if (!running) {
                    ESP_LOGD(TAG, "Starting server");
                    if (wifi_server_if->server_start) {
                        wifi_server_if->server_start(wifi_server_if->params);
                    }
                    else {
                        ESP_LOGE(TAG, "server_start function pointer is NULL");
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Set the server interface for Wi-Fi module
 */
void jw_wifi_set_server_interface(const jw_wifi_server_interface_t *interface) {
    wifi_server_if = interface;
}