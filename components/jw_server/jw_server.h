#ifndef JW_SERVER_H
#define JW_SERVER_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_err.h>
#include "cJSON.h"
#include "jw_keep_alive.h"
#include "jw_common.h"

/** @brief Maximum number of HTTP clients */
#define JW_SERVER_MAX_CLIENTS 11
/** @brief Maximum number of WebSocket connections */
#define JW_SERVER_MAX_WS_OPEN_SOCKETS 11
/** @brief Scratch buffer size for file operations */
#define JW_SERVER_SCRATCH_BUFSIZE 1024
/** @brief JSON response buffer size */
#define JW_SERVER_JSON_RESPONSE_SIZE (1024 * 4)
/** @brief Server started event bit */
#define SERVER_STARTED_BIT BIT0
/** @brief Server stopped event bit */
#define SERVER_STOPPED_BIT BIT1
/** @brief Maximum number of Wi-Fi networks to scan */
#define MAX_NETWORKS 20

/** @brief Enum for WebSocket data types */
typedef enum {
    JW_SERVER_WS_REBOOT,       // Device reboot request
    JW_SERVER_WS_MAIN,         // System status update
    JW_SERVER_WS_DEVICE,       // Device-specific update
    JW_SERVER_WS_BOARD,        // Board configuration update
    JW_SERVER_WS_WIFI,         // Wi-Fi settings update
    JW_SERVER_WS_RTC,          // RTC settings update
    JW_SERVER_WS_LOG,          // Logging settings update
    JW_SERVER_WS_DEVICES,      // Peered devices update
    JW_SERVER_WS_REG_DEVICE,   // Device registration
    JW_SERVER_WS_NEW_PEER,     // Add ESP-NOW peer
    JW_SERVER_WS_CLEAR_PEERS,  // Clear all ESP-NOW peers
    JW_SERVER_WS_CHANGE_CHAN,  // Change Wi-Fi channel
    JW_SERVER_WS_PEER_REMOVE   // Remove ESP-NOW peer
} jw_server_data_type_t;

/** @brief Structure for file server data */
typedef struct {
    char base_path[128];           // Base path for file storage
    char scratch[JW_SERVER_SCRATCH_BUFSIZE]; // Buffer for file ops
} jw_server_file_data_t;

/** @brief Structure for WebSocket update data */
typedef struct {
    jw_server_data_type_t type;    // Update type
    int socket_id;                 // Client socket ID
    struct {
        char key[64];             // Update key
        char val[64];             // Update value
        char data[128];           // Additional data
    } data;                        // Update data fields
} jw_server_update_data_t;

/** @brief Structure for async WebSocket response */
typedef struct {
    httpd_handle_t hd;             // HTTP server handle
    int fd;                        // Client file descriptor
    uint8_t *data;                 // Data to send
    size_t len;                    // Data length
} ws_async_resp_arg_t;

/** @brief Structure for server initialization parameters (internal implementation) */
typedef struct {
    QueueHandle_t *device_update_queue; // Queue for device updates
    void (*data_callback)(jw_server_data_type_t type, char *buffer, size_t max_len, size_t *len); // Callback for response data
    EventGroupHandle_t event_group;     // Event group for synchronization
} jw_server_params_impl_t;

/** @brief Structure for server context (internal implementation) */
typedef struct {
    httpd_handle_t server_handle;       // HTTP server handle
    QueueHandle_t updates_queue;        // Queue for WebSocket updates
    TaskHandle_t web_server_task_handle;// Web server task handle
    TaskHandle_t web_status_task_handle;// Status task handle
    jw_server_file_data_t *file_data;   // File server data pointer
    jw_keep_alive_t keep_alive;         // Keep-alive instance
    bool is_running;                    // Server running flag
} jw_server_context_impl_t;

/** @brief Global server context (internal type) */
extern jw_server_context_impl_t jw_server_context;
/** @brief Global server parameters (internal type) */
extern jw_server_params_impl_t jw_server_params;

esp_err_t jw_server_init(void);
esp_err_t jw_server_start(const jw_server_params_t *params);
esp_err_t jw_server_stop(void);
bool jw_server_is_running(void);
esp_err_t jw_server_send_ws_data(int sockfd, const char *data, size_t len);
void jw_server_notify_found_peers(cJSON *msg);  // Added
esp_err_t jw_server_unregister_nodes_uri(void); // Added

#endif // JW_SERVER_H