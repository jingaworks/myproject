#include "jw_espnow.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "cJSON.h"
#include "jw_server.h"

#define TAG "JW_ESPNOW"
#define JW_ESPNOW_EVENT_QUEUE_SIZE 10
#define JW_ESPNOW_WEB_QUEUE_SIZE 5
#define JW_ESPNOW_PMK "pmk1234567890123"  // 16-byte primary master key
#define JW_ESPNOW_LMK "lmk1234567890123"  // 16-byte local master key
#define BROADCAST_MAC "\xFF\xFF\xFF\xFF\xFF\xFF"

// Structure for jw_espnow context
struct jw_espnow_context {
    QueueHandle_t event_queue;        // Queue for ESP-NOW events
    QueueHandle_t web_settings_queue; // Queue for WebSocket messages
    TaskHandle_t peering_task_handle; // Peering task handle
    EventGroupHandle_t status_events; // Event group for status flags
    uint8_t peer_macs[10][ESP_NOW_ETH_ALEN]; // Cached MACs of Peers
    uint8_t peer_count;               // Number of cached Peers
    SemaphoreHandle_t mutex;          // Mutex for thread-safe access
};

// Static context instance
static jw_espnow_context_t *jw_espnow_context = NULL;

static void jw_espnow_run_peering_task(void *params);
static void jw_espnow_handle_receive_callback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
static void jw_espnow_handle_send_callback(const uint8_t *mac_addr, esp_now_send_status_t status);
static void send_found_peers_notification(cJSON *peers_array);

esp_err_t jw_espnow_initialize(void) {
    if (jw_espnow_context != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    jw_espnow_context = heap_caps_malloc(sizeof(jw_espnow_context_t), MALLOC_CAP_SPIRAM);
    if (!jw_espnow_context) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return ESP_ERR_NO_MEM;
    }

    jw_espnow_context->event_queue = xQueueCreate(JW_ESPNOW_EVENT_QUEUE_SIZE, sizeof(jw_espnow_message_t));
    jw_espnow_context->web_settings_queue = xQueueCreate(JW_ESPNOW_WEB_QUEUE_SIZE, sizeof(jw_espnow_message_t));
    jw_espnow_context->status_events = xEventGroupCreate();
    jw_espnow_context->mutex = xSemaphoreCreateMutex();
    if (!jw_espnow_context->event_queue || !jw_espnow_context->web_settings_queue ||
        !jw_espnow_context->status_events || !jw_espnow_context->mutex) {
        ESP_LOGE(TAG, "Failed to create queues, event group, or mutex");
        if (jw_espnow_context->event_queue) vQueueDelete(jw_espnow_context->event_queue);
        if (jw_espnow_context->web_settings_queue) vQueueDelete(jw_espnow_context->web_settings_queue);
        if (jw_espnow_context->status_events) vEventGroupDelete(jw_espnow_context->status_events);
        if (jw_espnow_context->mutex) vSemaphoreDelete(jw_espnow_context->mutex);
        heap_caps_free(jw_espnow_context);
        jw_espnow_context = NULL;
        return ESP_FAIL;
    }

    jw_espnow_context->peer_count = 0;
    memset(jw_espnow_context->peer_macs, 0, sizeof(jw_espnow_context->peer_macs));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(jw_espnow_handle_receive_callback));
    ESP_ERROR_CHECK(esp_now_register_send_cb(jw_espnow_handle_send_callback));
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)JW_ESPNOW_PMK));

    esp_now_peer_info_t broadcast_peer = {
        .channel = 0,
        .ifidx = ESP_IF_WIFI_STA,
        .encrypt = false
    };
    memcpy(broadcast_peer.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&broadcast_peer));

    if (xTaskCreate(jw_espnow_run_peering_task, "jw_espnow_peering", 4096, NULL, 4, &jw_espnow_context->peering_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create peering task");
        vQueueDelete(jw_espnow_context->event_queue);
        vQueueDelete(jw_espnow_context->web_settings_queue);
        vEventGroupDelete(jw_espnow_context->status_events);
        vSemaphoreDelete(jw_espnow_context->mutex);
        heap_caps_free(jw_espnow_context);
        jw_espnow_context = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

esp_err_t jw_espnow_start_peering(void) {
    if (!jw_espnow_context) return ESP_ERR_INVALID_STATE;
    jw_espnow_message_t msg = {
        .version = 1,
        .msg_type = JW_ESPNOW_MSG_TYPE_PEER_REQUEST
    };
    esp_read_mac(msg.source_mac, ESP_MAC_WIFI_STA);
    strncpy(msg.payload.peering.peer_name, "Controller", sizeof(msg.payload.peering.peer_name) - 1);
    memcpy(msg.destination_mac, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_send(msg.destination_mac, (uint8_t *)&msg, sizeof(jw_espnow_message_t)));
    ESP_LOGI(TAG, "Started peering broadcast");
    return ESP_OK;
}

esp_err_t jw_espnow_accept_peer(const uint8_t *mac_address) {
    if (!jw_espnow_context || !mac_address) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(jw_espnow_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    if (jw_espnow_context->peer_count >= JW_PEERS_MAX_CAPACITY) {
        ESP_LOGE(TAG, "Max peer capacity reached");
        xSemaphoreGive(jw_espnow_context->mutex);
        return ESP_ERR_NO_MEM;
    }

    esp_now_peer_info_t peer_info = {
        .channel = 0,
        .ifidx = ESP_IF_WIFI_STA,
        .encrypt = true
    };
    memcpy(peer_info.peer_addr, mac_address, ESP_NOW_ETH_ALEN);
    memcpy(peer_info.lmk, JW_ESPNOW_LMK, ESP_NOW_KEY_LEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));

    memcpy(jw_espnow_context->peer_macs[jw_espnow_context->peer_count], mac_address, ESP_NOW_ETH_ALEN);
    jw_espnow_context->peer_count++;

    jw_espnow_message_t msg = {
        .version = 1,
        .msg_type = JW_ESPNOW_MSG_TYPE_PEER_ACCEPT_CONFIRM
    };
    memcpy(msg.destination_mac, mac_address, ESP_NOW_ETH_ALEN);
    esp_read_mac(msg.source_mac, ESP_MAC_WIFI_STA);
    ESP_ERROR_CHECK(esp_now_send(msg.destination_mac, (uint8_t *)&msg, sizeof(jw_espnow_message_t)));

    ESP_LOGI(TAG, "Accepted peer " MACSTR, MAC2STR(mac_address));
    xSemaphoreGive(jw_espnow_context->mutex);
    return ESP_OK;
}

esp_err_t jw_espnow_send_channel_change(uint8_t new_channel) {
    if (!jw_espnow_context) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(jw_espnow_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    if (jw_espnow_context->peer_count == 0) {
        ESP_LOGI(TAG, "No peers to send CHANNEL_CHANGE");
        xSemaphoreGive(jw_espnow_context->mutex);
        return ESP_OK;
    }

    jw_espnow_message_t msg = {
        .version = 1,
        .msg_type = JW_ESPNOW_MSG_TYPE_CHANNEL_CHANGE,
        .payload.channel = new_channel
    };
    esp_read_mac(msg.source_mac, ESP_MAC_WIFI_STA);

    for (uint8_t i = 0; i < jw_espnow_context->peer_count; i++) {
        memcpy(msg.destination_mac, jw_espnow_context->peer_macs[i], ESP_NOW_ETH_ALEN);
        esp_err_t err = esp_now_send(msg.destination_mac, (uint8_t *)&msg, sizeof(jw_espnow_message_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send CHANNEL_CHANGE to " MACSTR ": %s",
                MAC2STR(msg.destination_mac), esp_err_to_name(err));
        }
        else {
            ESP_LOGI(TAG, "Sent CHANNEL_CHANGE (%d) to " MACSTR, new_channel, MAC2STR(msg.destination_mac));
        }
    }

    xSemaphoreGive(jw_espnow_context->mutex);
    return ESP_OK;
}

static void send_found_peers_notification(cJSON *peers_array) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "event", "found_peers");
    cJSON_AddItemToObject(msg, "peers", peers_array);
    jw_server_notify_found_peers(msg);
}

static void jw_espnow_run_peering_task(void *params) {
    jw_espnow_message_t msg;
    uint8_t controller_mac[ESP_NOW_ETH_ALEN];
    esp_read_mac(controller_mac, ESP_MAC_WIFI_STA);
    cJSON *found_peers = cJSON_CreateArray();
    TickType_t peering_start = 0;

    while (1) {
        if (xQueueReceive(jw_espnow_context->web_settings_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (msg.msg_type == JW_ESPNOW_MSG_TYPE_PEER_REQUEST) {
                ESP_LOGI(TAG, "Sending PEER_REQUEST to " MACSTR, MAC2STR(msg.destination_mac));
                ESP_ERROR_CHECK(esp_now_send(msg.destination_mac, (uint8_t *)&msg, sizeof(jw_espnow_message_t)));
                peering_start = xTaskGetTickCount();
            }
        }

        if (xQueueReceive(jw_espnow_context->event_queue, &msg, 0) == pdTRUE) {
            switch (msg.msg_type) {
                case JW_ESPNOW_MSG_TYPE_PEER_REQUEST:
                    // Already handled above via web_settings_queue, log and skip
                    ESP_LOGI(TAG, "Received redundant PEER_REQUEST from " MACSTR, MAC2STR(msg.source_mac));
                    break;
                case JW_ESPNOW_MSG_TYPE_PEER_ACCEPT:
                    if (memcmp(msg.destination_mac, controller_mac, ESP_NOW_ETH_ALEN) == 0) {
                        ESP_LOGI(TAG, "Received PEER_ACCEPT from " MACSTR, MAC2STR(msg.source_mac));
                        if (jw_peers_is_blacklisted(msg.source_mac)) {
                            ESP_LOGI(TAG, "Peer " MACSTR " is blacklisted, skipping", MAC2STR(msg.source_mac));
                            break;
                        }
                        bool exists = false;
                        for (int i = 0; i < cJSON_GetArraySize(found_peers); i++) {
                            cJSON *peer = cJSON_GetArrayItem(found_peers, i);
                            char mac_str[18];
                            snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(msg.source_mac));
                            if (strcmp(cJSON_GetObjectItem(peer, "mac")->valuestring, mac_str) == 0) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            cJSON *peer = cJSON_CreateObject();
                            char mac_str[18];
                            snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(msg.source_mac));
                            cJSON_AddStringToObject(peer, "mac", mac_str);
                            cJSON_AddStringToObject(peer, "name", msg.payload.peering.peer_name);
                            cJSON_AddNumberToObject(peer, "type", msg.payload.peering.peer_type);
                            if (msg.payload.peering.peer_type == JW_PEER_TYPE_SENSOR) {
                                cJSON_AddNumberToObject(peer, "subtype", msg.payload.peering.sensor_subtype);
                            }
                            cJSON_AddItemToArray(found_peers, peer);
                        }
                        if (xTaskGetTickCount() - peering_start >= pdMS_TO_TICKS(JW_ESPNOW_PEERING_TIMEOUT_MS)) {
                            send_found_peers_notification(found_peers);
                            found_peers = cJSON_CreateArray();
                            peering_start = 0;
                            jw_server_unregister_nodes_uri();
                        }
                    }
                    break;
                case JW_ESPNOW_MSG_TYPE_PEER_ACCEPT_CONFIRM:
                    ESP_LOGI(TAG, "Received PEER_ACCEPT_CONFIRM from " MACSTR, MAC2STR(msg.source_mac));
                    break;
                case JW_ESPNOW_MSG_TYPE_PEER_CONFIRMED:
                    if (memcmp(msg.destination_mac, controller_mac, ESP_NOW_ETH_ALEN) == 0) {
                        ESP_LOGI(TAG, "Peer " MACSTR " fully confirmed", MAC2STR(msg.source_mac));
                        esp_err_t err = jw_peers_add_peer(msg.source_mac, msg.payload.peering.peer_type,
                            msg.payload.peering.peer_name,
                            (msg.payload.peering.peer_type == JW_PEER_TYPE_SENSOR) ? 1 : 0,
                            (msg.payload.peering.peer_type == JW_PEER_TYPE_SENSOR) ? &msg.payload.peering.sensor_subtype : NULL,
                            60);
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to add peer " MACSTR " to jw_peers: %s",
                                MAC2STR(msg.source_mac), esp_err_to_name(err));
                        }
                        else {
                            ESP_LOGI(TAG, "Added peer " MACSTR " to jw_peers", MAC2STR(msg.source_mac));
                            jw_server_unregister_nodes_uri();
                        }
                    }
                    break;
                case JW_ESPNOW_MSG_TYPE_CHANNEL_CHANGE:
                    ESP_LOGI(TAG, "Received CHANNEL_CHANGE (%d) from " MACSTR, msg.payload.channel, MAC2STR(msg.source_mac));
                    break;
                case JW_ESPNOW_MSG_TYPE_DATA:
                    ESP_LOGI(TAG, "Received DATA from " MACSTR, MAC2STR(msg.source_mac));
                    jw_peers_update_data(msg.source_mac, &msg.payload.data);
                    break;
                default:
                    ESP_LOGW(TAG, "Unhandled message type %d from " MACSTR, msg.msg_type, MAC2STR(msg.source_mac));
                    break;
            }
        }
        if (peering_start && (xTaskGetTickCount() - peering_start >= pdMS_TO_TICKS(JW_ESPNOW_PEERING_TIMEOUT_MS))) {
            if (cJSON_GetArraySize(found_peers) > 0) {
                send_found_peers_notification(found_peers);
            }
            else {
                cJSON *msg = cJSON_CreateObject();
                cJSON_AddStringToObject(msg, "event", "peer_failed");
                cJSON_AddStringToObject(msg, "message", "No peers responded");
                jw_server_notify_found_peers(msg);
            }
            found_peers = cJSON_CreateArray();
            peering_start = 0;
            jw_server_unregister_nodes_uri();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void jw_espnow_handle_receive_callback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (!jw_espnow_context || len < sizeof(jw_espnow_message_t)) {
        ESP_LOGE(TAG, "Invalid receive data");
        return;
    }

    jw_espnow_message_t msg;
    memcpy(&msg, data, sizeof(jw_espnow_message_t));
    memcpy(msg.source_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    if (msg.version != 1) {
        ESP_LOGW(TAG, "Ignoring message with version %d", msg.version);
        return;
    }
    if (xQueueSend(jw_espnow_context->event_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full");
    }
}

static void jw_espnow_handle_send_callback(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "Send failed to " MACSTR, MAC2STR(mac_addr));
    }
    else {
        ESP_LOGI(TAG, "Send succeeded to " MACSTR, MAC2STR(mac_addr));
    }
}