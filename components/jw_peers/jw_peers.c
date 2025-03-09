#include "jw_peers.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "jw_log.h"

#define TAG "JW_PEERS"
#define JW_PEERS_NVS_NAMESPACE "jw_peers"
#define JW_PEERS_NVS_KEY "peers_data"
#define JW_PEERS_NVS_BLACKLIST_KEY "blacklist"
#define JW_PEERS_UPDATE_QUEUE_SIZE 10
#define JW_PEERS_LOG_BUFFER_SIZE 5

struct jw_peers_context {
    jw_peer_entry_t *peers;
    uint8_t peer_count;
    SemaphoreHandle_t mutex;
    QueueHandle_t update_queue;
    TaskHandle_t logging_task;
    uint8_t blacklist[JW_PEERS_BLACKLIST_MAX_SIZE][ESP_NOW_ETH_ALEN];
    uint8_t blacklist_count;
};

static jw_peers_context_t *jw_peers_context = NULL;

static void jw_peers_run_logging_task(void *params);
static void save_to_nvs(void);
static void save_blacklist_to_nvs(void);

esp_err_t jw_peers_initialize(void) {
    if (jw_peers_context != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    jw_peers_context = heap_caps_malloc(sizeof(jw_peers_context_t), MALLOC_CAP_SPIRAM);
    if (!jw_peers_context) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return ESP_ERR_NO_MEM;
    }

    jw_peers_context->peers = NULL;
    jw_peers_context->peer_count = 0;
    jw_peers_context->mutex = xSemaphoreCreateMutex();
    jw_peers_context->update_queue = xQueueCreate(JW_PEERS_UPDATE_QUEUE_SIZE, sizeof(jw_peer_data_t));
    if (!jw_peers_context->mutex || !jw_peers_context->update_queue) {
        ESP_LOGE(TAG, "Failed to create mutex or queue");
        heap_caps_free(jw_peers_context);
        jw_peers_context = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(jw_peers_context->blacklist, 0, sizeof(jw_peers_context->blacklist));
    jw_peers_context->blacklist_count = 0;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(JW_PEERS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        goto cleanup;
    }

    size_t size = JW_PEERS_MAX_CAPACITY * sizeof(jw_peer_entry_t);
    jw_peer_entry_t *temp_peers = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!temp_peers) {
        ESP_LOGE(TAG, "Failed to allocate temp buffer for NVS load");
        nvs_close(nvs_handle);
        goto cleanup;
    }

    err = nvs_get_blob(nvs_handle, JW_PEERS_NVS_KEY, temp_peers, &size);
    if (err == ESP_OK) {
        jw_peers_context->peer_count = size / sizeof(jw_peer_entry_t);
        jw_peers_context->peers = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (!jw_peers_context->peers) {
            ESP_LOGE(TAG, "Failed to allocate peers array from NVS");
            heap_caps_free(temp_peers);
            nvs_close(nvs_handle);
            goto cleanup;
        }
        memcpy(jw_peers_context->peers, temp_peers, size);
        ESP_LOGI(TAG, "Loaded %d peers from NVS", jw_peers_context->peer_count);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No peers found in NVS, starting fresh");
    }
    else {
        ESP_LOGE(TAG, "NVS load failed: %s", esp_err_to_name(err));
    }
    heap_caps_free(temp_peers);

    size = JW_PEERS_BLACKLIST_MAX_SIZE * ESP_NOW_ETH_ALEN;
    uint8_t *blacklist_buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!blacklist_buffer) {
        ESP_LOGE(TAG, "Failed to allocate temp buffer for blacklist");
        nvs_close(nvs_handle);
        goto cleanup;
    }

    err = nvs_get_blob(nvs_handle, JW_PEERS_NVS_BLACKLIST_KEY, blacklist_buffer, &size);
    if (err == ESP_OK) {
        jw_peers_context->blacklist_count = size / ESP_NOW_ETH_ALEN;
        memcpy(jw_peers_context->blacklist, blacklist_buffer, size);
        ESP_LOGI(TAG, "Loaded %d blacklisted peers from NVS", jw_peers_context->blacklist_count);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No blacklist found in NVS, starting fresh");
    }
    else {
        ESP_LOGE(TAG, "NVS blacklist load failed: %s", esp_err_to_name(err));
    }
    heap_caps_free(blacklist_buffer);
    nvs_close(nvs_handle);

    if (xTaskCreate(jw_peers_run_logging_task, "jw_peers_logging", 4096, NULL, 3, &jw_peers_context->logging_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create logging task");
        goto cleanup;
    }

    return ESP_OK;

cleanup:
    if (jw_peers_context->mutex) vSemaphoreDelete(jw_peers_context->mutex);
    if (jw_peers_context->update_queue) vQueueDelete(jw_peers_context->update_queue);
    heap_caps_free(jw_peers_context->peers);
    heap_caps_free(jw_peers_context);
    jw_peers_context = NULL;
    return ESP_FAIL;
}

esp_err_t jw_peers_add_peer(const uint8_t *mac_address, jw_peer_type_t peer_type, const char *peer_name,
    uint8_t sensor_count, const jw_sensor_subtype_t *sensor_types, uint8_t interval_sec) {
    if (!jw_peers_context || !mac_address || !peer_name || sensor_count > 3) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(jw_peers_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    for (uint8_t i = 0; i < jw_peers_context->peer_count; i++) {
        if (memcmp(jw_peers_context->peers[i].mac_address, mac_address, ESP_NOW_ETH_ALEN) == 0) {
            ESP_LOGW(TAG, "Peer " MACSTR " already exists", MAC2STR(mac_address));
            xSemaphoreGive(jw_peers_context->mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (jw_peers_context->peer_count >= JW_PEERS_MAX_CAPACITY) {
        ESP_LOGE(TAG, "Max peer capacity reached");
        xSemaphoreGive(jw_peers_context->mutex);
        return ESP_ERR_NO_MEM;
    }

    size_t new_size = (jw_peers_context->peer_count + 1) * sizeof(jw_peer_entry_t);
    jw_peer_entry_t *new_peers = heap_caps_realloc(jw_peers_context->peers, new_size, MALLOC_CAP_SPIRAM);
    if (!new_peers) {
        ESP_LOGE(TAG, "Failed to resize peers array");
        xSemaphoreGive(jw_peers_context->mutex);
        return ESP_ERR_NO_MEM;
    }
    jw_peers_context->peers = new_peers;

    jw_peer_entry_t *peer = &jw_peers_context->peers[jw_peers_context->peer_count];
    memcpy(peer->mac_address, mac_address, ESP_NOW_ETH_ALEN);
    peer->peer_type = peer_type;
    peer->sensor_count = sensor_count;
    if (sensor_count > 0 && sensor_types) {
        memcpy(peer->sensor_types, sensor_types, sensor_count * sizeof(jw_sensor_subtype_t));
    }
    strncpy(peer->peer_name, peer_name, sizeof(peer->peer_name) - 1);
    peer->peer_name[sizeof(peer->peer_name) - 1] = '\0';
    peer->last_update = 0;
    peer->is_active = true;
    memset(&peer->latest_data, 0, sizeof(jw_peer_data_t));
    peer->data_interval_sec = (interval_sec > 0) ? interval_sec : 60;
    jw_peers_context->peer_count++;

    save_to_nvs();

    ESP_LOGI(TAG, "Added peer " MACSTR " (%s)", MAC2STR(mac_address), peer_name);
    xSemaphoreGive(jw_peers_context->mutex);
    return ESP_OK;
}

esp_err_t jw_peers_get_peer(jw_peer_entry_t **peers, uint8_t *peer_count) {
    if (!jw_peers_context || !peers || !peer_count) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(jw_peers_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    *peers = jw_peers_context->peers;
    *peer_count = jw_peers_context->peer_count;
    xSemaphoreGive(jw_peers_context->mutex);
    return ESP_OK;
}

esp_err_t jw_peers_update_data(const uint8_t *mac_address, const jw_peer_data_t *data) {
    if (!jw_peers_context || !mac_address || !data) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(jw_peers_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    for (uint8_t i = 0; i < jw_peers_context->peer_count; i++) {
        if (memcmp(jw_peers_context->peers[i].mac_address, mac_address, ESP_NOW_ETH_ALEN) == 0) {
            jw_peers_context->peers[i].latest_data = *data;
            jw_peers_context->peers[i].last_update = data->timestamp;
            jw_peers_context->peers[i].is_active = true;
            if (xQueueSend(jw_peers_context->update_queue, data, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "Update queue full for " MACSTR, MAC2STR(mac_address));
            }
            xSemaphoreGive(jw_peers_context->mutex);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Peer " MACSTR " not found for update", MAC2STR(mac_address));
    xSemaphoreGive(jw_peers_context->mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t jw_peers_edit_name(const uint8_t *mac_address, const char *new_name) {
    if (!jw_peers_context || !mac_address || !new_name) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(jw_peers_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    for (uint8_t i = 0; i < jw_peers_context->peer_count; i++) {
        if (memcmp(jw_peers_context->peers[i].mac_address, mac_address, ESP_NOW_ETH_ALEN) == 0) {
            strncpy(jw_peers_context->peers[i].peer_name, new_name, sizeof(jw_peers_context->peers[i].peer_name) - 1);
            jw_peers_context->peers[i].peer_name[sizeof(jw_peers_context->peers[i].peer_name) - 1] = '\0';
            save_to_nvs();
            ESP_LOGI(TAG, "Edited name for " MACSTR " to %s", MAC2STR(mac_address), new_name);
            xSemaphoreGive(jw_peers_context->mutex);
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "Peer " MACSTR " not found for name edit", MAC2STR(mac_address));
    xSemaphoreGive(jw_peers_context->mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t jw_peers_edit_interval(const uint8_t *mac_address, uint8_t interval_sec) {
    if (!jw_peers_context || !mac_address) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(jw_peers_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    for (uint8_t i = 0; i < jw_peers_context->peer_count; i++) {
        if (memcmp(jw_peers_context->peers[i].mac_address, mac_address, ESP_NOW_ETH_ALEN) == 0) {
            jw_peers_context->peers[i].data_interval_sec = interval_sec;
            save_to_nvs();
            ESP_LOGI(TAG, "Edited interval for " MACSTR " to %d sec", MAC2STR(mac_address), interval_sec);
            xSemaphoreGive(jw_peers_context->mutex);
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "Peer " MACSTR " not found for interval edit", MAC2STR(mac_address));
    xSemaphoreGive(jw_peers_context->mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t jw_peers_add_to_blacklist(const uint8_t *mac_address) {
    if (!jw_peers_context || !mac_address) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(jw_peers_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    for (uint8_t i = 0; i < jw_peers_context->blacklist_count; i++) {
        if (memcmp(jw_peers_context->blacklist[i], mac_address, ESP_NOW_ETH_ALEN) == 0) {
            ESP_LOGW(TAG, "Peer " MACSTR " already blacklisted", MAC2STR(mac_address));
            xSemaphoreGive(jw_peers_context->mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (jw_peers_context->blacklist_count >= JW_PEERS_BLACKLIST_MAX_SIZE) {
        ESP_LOGE(TAG, "Blacklist capacity reached");
        xSemaphoreGive(jw_peers_context->mutex);
        return ESP_ERR_NO_MEM;
    }

    memcpy(jw_peers_context->blacklist[jw_peers_context->blacklist_count], mac_address, ESP_NOW_ETH_ALEN);
    jw_peers_context->blacklist_count++;
    save_blacklist_to_nvs();
    ESP_LOGI(TAG, "Added " MACSTR " to blacklist", MAC2STR(mac_address));
    xSemaphoreGive(jw_peers_context->mutex);
    return ESP_OK;
}

esp_err_t jw_peers_remove_from_blacklist(const uint8_t *mac_address) {
    if (!jw_peers_context || !mac_address) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(jw_peers_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    for (uint8_t i = 0; i < jw_peers_context->blacklist_count; i++) {
        if (memcmp(jw_peers_context->blacklist[i], mac_address, ESP_NOW_ETH_ALEN) == 0) {
            for (uint8_t j = i; j < jw_peers_context->blacklist_count - 1; j++) {
                memcpy(jw_peers_context->blacklist[j], jw_peers_context->blacklist[j + 1], ESP_NOW_ETH_ALEN);
            }
            memset(jw_peers_context->blacklist[jw_peers_context->blacklist_count - 1], 0, ESP_NOW_ETH_ALEN);
            jw_peers_context->blacklist_count--;
            save_blacklist_to_nvs();
            ESP_LOGI(TAG, "Removed " MACSTR " from blacklist", MAC2STR(mac_address));
            xSemaphoreGive(jw_peers_context->mutex);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Peer " MACSTR " not found in blacklist", MAC2STR(mac_address));
    xSemaphoreGive(jw_peers_context->mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t jw_peers_get_blacklist(uint8_t(*blacklist)[ESP_NOW_ETH_ALEN], uint8_t *blacklist_count) {
    if (!jw_peers_context || !blacklist || !blacklist_count) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(jw_peers_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    memcpy(blacklist, jw_peers_context->blacklist, jw_peers_context->blacklist_count * ESP_NOW_ETH_ALEN);
    *blacklist_count = jw_peers_context->blacklist_count;
    xSemaphoreGive(jw_peers_context->mutex);
    return ESP_OK;
}

bool jw_peers_is_blacklisted(const uint8_t *mac_address) {
    if (!jw_peers_context || !mac_address) return false;

    if (xSemaphoreTake(jw_peers_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex in is_blacklisted");
        return false;
    }

    for (uint8_t i = 0; i < jw_peers_context->blacklist_count; i++) {
        if (memcmp(jw_peers_context->blacklist[i], mac_address, ESP_NOW_ETH_ALEN) == 0) {
            xSemaphoreGive(jw_peers_context->mutex);
            return true;
        }
    }
    xSemaphoreGive(jw_peers_context->mutex);
    return false;
}

static void jw_peers_run_logging_task(void *params) {
    jw_peer_data_t data_buffer[JW_PEERS_LOG_BUFFER_SIZE];
    uint8_t buffer_count = 0;
    TickType_t last_flush = xTaskGetTickCount();

    while (1) {
        jw_peer_data_t data;
        if (xQueueReceive(jw_peers_context->update_queue, &data, pdMS_TO_TICKS(5000)) == pdTRUE) {
            data_buffer[buffer_count++] = data;
        }

        if (buffer_count >= JW_PEERS_LOG_BUFFER_SIZE ||
            (xTaskGetTickCount() - last_flush >= pdMS_TO_TICKS(5000) && buffer_count > 0)) {
            if (xSemaphoreTake(jw_peers_context->mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                for (uint8_t i = 0; i < buffer_count; i++) {
                    for (uint8_t j = 0; j < jw_peers_context->peer_count; j++) {
                        if (jw_peers_context->peers[j].last_update == data_buffer[i].timestamp) {
                            char log_path[64];
                            time_t now = data_buffer[i].timestamp;
                            struct tm timeinfo;
                            localtime_r(&now, &timeinfo);
                            char date_str[11];
                            strftime(date_str, sizeof(date_str), "%Y_%m_%d", &timeinfo);
                            snprintf(log_path, sizeof(log_path), "/sdcard/peers/" MACSTR "/data/%s.log",
                                MAC2STR(jw_peers_context->peers[j].mac_address), date_str);
                            char log_entry[128];
                            snprintf(log_entry, sizeof(log_entry), "[%lu] %s: sensors=[%.2f,%.2f,%.2f], relay=%d, switch=%d",
                                data_buffer[i].timestamp, jw_peers_context->peers[j].peer_name,
                                data_buffer[i].sensor_values[0], data_buffer[i].sensor_values[1],
                                data_buffer[i].sensor_values[2], data_buffer[i].relay_state,
                                data_buffer[i].switch_state);
                            jw_log_write(JW_LOG_INFO, log_path, log_entry);
                            break;
                        }
                    }
                }
                xSemaphoreGive(jw_peers_context->mutex);
                buffer_count = 0;
                last_flush = xTaskGetTickCount();
            }
        }
    }
}

static void save_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    for (int retries = 0; retries < 3; retries++) {
        err = nvs_open(JW_PEERS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(nvs_handle, JW_PEERS_NVS_KEY, jw_peers_context->peers,
                jw_peers_context->peer_count * sizeof(jw_peer_entry_t));
            if (err == ESP_OK) err = nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            if (err == ESP_OK) return;
        }
        ESP_LOGW(TAG, "NVS save attempt %d failed: %s, retrying...", retries + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(100 * (1 << retries)));
    }
    ESP_LOGE(TAG, "Failed to save to NVS after retries: %s", esp_err_to_name(err));
}

static void save_blacklist_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(JW_PEERS_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for blacklist: %s", esp_err_to_name(err));
        return;
    }

    size_t size = jw_peers_context->blacklist_count * ESP_NOW_ETH_ALEN;
    err = nvs_set_blob(nvs_handle, JW_PEERS_NVS_BLACKLIST_KEY, jw_peers_context->blacklist, size);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save blacklist to NVS: %s", esp_err_to_name(err));
    }
    else {
        ESP_LOGI(TAG, "Saved %d blacklisted peers to NVS", jw_peers_context->blacklist_count);
    }
    nvs_close(nvs_handle);
}