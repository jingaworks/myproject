#include "jw_server.h"
#include <string.h>
#include <nvs_flash.h>
#include <lwip/sockets.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <dirent.h>
#include <esp_system.h>
#include <esp_mac.h>
#include "cJSON.h"
#include "jw_wifi.h"
#include "jw_rtc.h"
#include "jw_sdcard.h"
#include "jw_log.h"

/** @brief Logging tag for server module */
static const char *TAG = "JW_SERVER";

/** @brief Global server context, initialized to zero */
jw_server_context_impl_t jw_server_context = { 0 };
/** @brief Global server parameters */
jw_server_params_impl_t jw_server_params;

/** @brief Function prototypes for static functions */
static const char *get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize);
static esp_err_t jw_server_start_internal(void);
static void jw_server_web_server_task(void *pvParameters);
static void jw_server_web_status_task(void *pvParameters);
static esp_err_t jw_server_ws_handler(httpd_req_t *req);
static esp_err_t jw_server_nodes_ws_handler(httpd_req_t *req);
static esp_err_t jw_server_peers_ws_handler(httpd_req_t *req);
static esp_err_t jw_server_favicon_get_handler(httpd_req_t *req);
static esp_err_t jw_server_css_get_handler(httpd_req_t *req);
static esp_err_t jw_server_js_get_handler(httpd_req_t *req);
static esp_err_t jw_server_root_get_handler(httpd_req_t *req);
static esp_err_t jw_server_device_get_handler(httpd_req_t *req);
static esp_err_t jw_server_devices_get_handler(httpd_req_t *req);
static esp_err_t jw_server_download_get_handler(httpd_req_t *req);
static esp_err_t jw_server_delete_post_handler(httpd_req_t *req);
static void jw_server_format_data(char *buffer, jw_server_data_type_t type, const char *key, const char *val);
static esp_err_t jw_server_ws_open_fd(httpd_handle_t hd, int sockfd);
static void jw_server_ws_close_fd(httpd_handle_t hd, int sockfd);
static void jw_server_ws_send_async(void *arg);
static bool jw_server_check_client_alive_cb(jw_keep_alive_t h, int fd);
static bool jw_server_client_not_alive_cb(jw_keep_alive_t h, int fd);
static esp_err_t jw_http_resp_dir_html(httpd_req_t *req, const char *dirpath, char *mac_id);

/** @brief WebSocket URI handler configuration for main WS */
static const httpd_uri_t jw_server_ws = {
    .uri = "/ws", .method = HTTP_GET, .handler = jw_server_ws_handler, .user_ctx = NULL, .is_websocket = true, .handle_ws_control_frames = true
};

/** @brief Favicon URI handler configuration */
static const httpd_uri_t jw_server_icon = {
    .uri = "/favicon.ico", .method = HTTP_GET, .handler = jw_server_favicon_get_handler, .user_ctx = NULL
};

/** @brief CSS URI handler configuration */
static const httpd_uri_t jw_server_css = {
    .uri = "/main.css", .method = HTTP_GET, .handler = jw_server_css_get_handler, .user_ctx = NULL
};

/** @brief JavaScript URI handler configuration */
static const httpd_uri_t jw_server_js = {
    .uri = "/jquery.js", .method = HTTP_GET, .handler = jw_server_js_get_handler, .user_ctx = NULL
};

/** @brief Root URI handler configuration */
static const httpd_uri_t jw_server_root = {
    .uri = "/", .method = HTTP_GET, .handler = jw_server_root_get_handler, .user_ctx = NULL
};

/** @brief Device URI handler configuration */
static const httpd_uri_t jw_server_device = {
    .uri = "/device/*", .method = HTTP_GET, .handler = jw_server_device_get_handler, .user_ctx = NULL
};

/** @brief Devices URI handler configuration */
static const httpd_uri_t jw_server_devices = {
    .uri = "/devices", .method = HTTP_GET, .handler = jw_server_devices_get_handler, .user_ctx = NULL
};

/** @brief All files URI handler configuration */
static const httpd_uri_t jw_server_all_files = {
    .uri = "/device_*", .method = HTTP_GET, .handler = jw_server_download_get_handler, .user_ctx = NULL
};

/** @brief Delete URI handler configuration */
static const httpd_uri_t jw_server_delete = {
    .uri = "/delete/*", .method = HTTP_POST, .handler = jw_server_delete_post_handler, .user_ctx = NULL
};

/** @brief Nodes URI handler configuration (for ESP-NOW peering) */
static httpd_uri_t jw_server_nodes = {
    .uri = "/ws/nodes", .method = HTTP_GET, .handler = jw_server_nodes_ws_handler, .user_ctx = NULL, .is_websocket = true
};

/** @brief Peers URI handler configuration (for ESP-NOW peer management) */
static httpd_uri_t jw_server_peers = {
    .uri = "/ws/peers", .method = HTTP_GET, .handler = jw_server_peers_ws_handler, .user_ctx = NULL, .is_websocket = true
};

/** @brief Flag to track if /ws/nodes is registered */
static bool nodes_uri_registered = false;

/**
 * @brief Extract path from URI relative to base path
 */
static const char *get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize) {
    if (!dest || !base_path || !uri) return NULL;
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);
    const char *quest = strchr(uri, '?');
    if (quest) pathlen = MIN(pathlen, quest - uri);
    const char *hash = strchr(uri, '#');
    if (hash) pathlen = MIN(pathlen, hash - uri);
    if (base_pathlen + pathlen + 1 > destsize) return NULL;
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);
    return dest + base_pathlen;
}

/**
 * @brief Initialize the server module
 */
esp_err_t jw_server_init(void) {
    ESP_LOGI(TAG, "Initializing server module");
    if (jw_server_context.updates_queue != NULL) {
        ESP_LOGW(TAG, "Server context already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    jw_server_context.updates_queue = xQueueCreate(10, sizeof(jw_server_update_data_t *));
    if (!jw_server_context.updates_queue) {
        ESP_LOGE(TAG, "Failed to create updates queue");
        return ESP_FAIL;
    }
    nodes_uri_registered = false;
    return ESP_OK;
}

/**
 * @brief Start the HTTP server with specified parameters
 */
esp_err_t jw_server_start(const jw_server_params_t *params) {
    if (!params || !((jw_server_params_impl_t *)params)->device_update_queue || !((jw_server_params_impl_t *)params)->data_callback || !((jw_server_params_impl_t *)params)->event_group) {
        ESP_LOGE(TAG, "Invalid server parameters");
        return ESP_ERR_INVALID_ARG;
    }
    if (jw_server_context.server_handle) {
        ESP_LOGE(TAG, "Server already started");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting HTTP server");
    jw_server_params = *(jw_server_params_impl_t *)params;
    esp_err_t err = jw_server_start_internal();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server internally: %s", esp_err_to_name(err));
        return err;
    }

    if (jw_server_context.is_running) {
        if (eTaskGetState(jw_server_context.web_status_task_handle) == eSuspended) {
            vTaskResume(jw_server_context.web_status_task_handle);
            ESP_LOGI(TAG, "Resumed web_status_task_handle");
        }
        if (eTaskGetState(jw_server_context.web_server_task_handle) == eSuspended) {
            vTaskResume(jw_server_context.web_server_task_handle);
            ESP_LOGI(TAG, "Resumed web_server_task_handle");
        }
    }
    else {
        ESP_LOGD(TAG, "Creating web server task");
        if (xTaskCreate(jw_server_web_server_task, "jw_server_task", 8192, NULL, 2, &jw_server_context.web_server_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create web server task");
            jw_server_stop();
            return ESP_FAIL;
        }
        ESP_LOGD(TAG, "Creating web status task");
        if (xTaskCreate(jw_server_web_status_task, "jw_status_task", 8192, NULL, 2, &jw_server_context.web_status_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create web status task");
            jw_server_stop();
            return ESP_FAIL;
        }
        jw_server_context.is_running = true;
    }
    xEventGroupSetBits(jw_server_params.event_group, SERVER_STARTED_BIT);
    return ESP_OK;
}

/**
 * @brief Stop the HTTP server
 */
esp_err_t jw_server_stop(void) {
    if (!jw_server_context.server_handle) {
        ESP_LOGW(TAG, "Server not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Stopping HTTP server");
    if (jw_server_context.keep_alive) {
        jw_keep_alive_stop(jw_server_context.keep_alive);
        jw_server_context.keep_alive = NULL;
    }

    esp_err_t err = httpd_stop(jw_server_context.server_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop server: %s", esp_err_to_name(err));
        return err;
    }

    jw_server_context.server_handle = NULL;
    if (jw_server_context.web_status_task_handle && (eTaskGetState(jw_server_context.web_status_task_handle) == eReady || eTaskGetState(jw_server_context.web_status_task_handle) == eBlocked)) {
        vTaskSuspend(jw_server_context.web_status_task_handle);
        ESP_LOGI(TAG, "Suspended web_status_task_handle");
    }
    if (jw_server_context.web_server_task_handle && (eTaskGetState(jw_server_context.web_server_task_handle) == eReady || eTaskGetState(jw_server_context.web_server_task_handle) == eBlocked)) {
        vTaskSuspend(jw_server_context.web_server_task_handle);
        ESP_LOGI(TAG, "Suspended web_server_task_handle");
    }
    if (jw_server_context.file_data) {
        free(jw_server_context.file_data);
        jw_server_context.file_data = NULL;
    }
    jw_server_context.is_running = false;
    nodes_uri_registered = false;
    xEventGroupSetBits(jw_wifi_get_event_group(), SERVER_STOPPED_BIT);
    ESP_LOGI(TAG, "Server stopped");
    return ESP_OK;
}

/**
 * @brief Check if the server is running
 */
bool jw_server_is_running(void) {
    return jw_server_context.is_running;
}

/**
 * @brief Internal server start function
 */
static esp_err_t jw_server_start_internal(void) {
    if (jw_server_context.server_handle) {
        ESP_LOGW(TAG, "Server already running internally");
        return ESP_ERR_INVALID_STATE;
    }

    jw_server_context.file_data = calloc(1, sizeof(jw_server_file_data_t));
    if (!jw_server_context.file_data) {
        ESP_LOGE(TAG, "Failed to allocate file data");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(jw_server_context.file_data->base_path, JW_SDCARD_MOUNT_POINT, sizeof(jw_server_context.file_data->base_path));

    jw_keep_alive_config_t keep_alive_config = JW_KEEP_ALIVE_CONFIG_DEFAULT();
    keep_alive_config.max_clients = JW_SERVER_MAX_CLIENTS;
    keep_alive_config.client_not_alive_cb = jw_server_client_not_alive_cb;
    keep_alive_config.check_client_alive_cb = jw_server_check_client_alive_cb;
    jw_server_context.keep_alive = jw_keep_alive_start(&keep_alive_config);
    if (!jw_server_context.keep_alive) {
        ESP_LOGE(TAG, "Failed to start keep-alive");
        free(jw_server_context.file_data);
        jw_server_context.file_data = NULL;
        return ESP_FAIL;
    }

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.max_open_sockets = JW_SERVER_MAX_WS_OPEN_SOCKETS;
    conf.global_user_ctx = jw_server_context.keep_alive;
    conf.open_fn = jw_server_ws_open_fd;
    conf.close_fn = jw_server_ws_close_fd;
    conf.stack_size = 8192;
    conf.uri_match_fn = httpd_uri_match_wildcard;
    conf.max_uri_handlers = 11;

    esp_err_t err = httpd_start(&jw_server_context.server_handle, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        jw_keep_alive_stop(jw_server_context.keep_alive);
        jw_server_context.keep_alive = NULL;
        free(jw_server_context.file_data);
        jw_server_context.file_data = NULL;
        return err;
    }

    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_ws);
    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_icon);
    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_css);
    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_js);
    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_root);
    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_device);
    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_devices);
    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_all_files);
    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_delete);
    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_nodes);
    httpd_register_uri_handler(jw_server_context.server_handle, &jw_server_peers);

    jw_keep_alive_set_user_ctx(jw_server_context.keep_alive, jw_server_context.server_handle);
    ESP_LOGI(TAG, "HTTP server started internally");
    nodes_uri_registered = true;
    return ESP_OK;
}
/**
 * @brief Format data into JSON string for WebSocket transmission
 */
static void jw_server_format_data(char *buffer, jw_server_data_type_t type, const char *key, const char *val) {
    if (!buffer || !key || !val) return;
    char *buf_ptr = buffer;
    size_t remaining = JW_SERVER_JSON_RESPONSE_SIZE;
    *buf_ptr++ = '{';
    remaining--;
    if (type == JW_SERVER_WS_REG_DEVICE) {
        int len = snprintf(buf_ptr, remaining, "\"b_channel\":%u,", jw_wifi_get_settings()->channel);
        if (len >= remaining) { ESP_LOGE(TAG, "Buffer overflow in reg_device"); return; }
        buf_ptr += len; remaining -= len;
    }
    else if (type == JW_SERVER_WS_MAIN || type == JW_SERVER_WS_DEVICES) {
        jw_wifi_settings_t *wifi_settings = jw_wifi_get_settings();
        int len = snprintf(buf_ptr, remaining, "\"b_channel\":%u,\"rtc-timezone\":\"%s\",\"wifi-mode\":%u,\"wifi-cc\":%u,\"wifi-ap_ssid\":\"%s\",\"wifi-ap_pass\":\"%s\",\"wifi-ap_channel\":%u,\"wifi-sta_ssid\":\"%s\",\"wifi-sta_pass\":\"%s\",\"wifi-sta_channel\":%u,\"wifi-sta_internet\":%u",
            wifi_settings->channel, jw_rtc_settings.timezone, wifi_settings->mode, wifi_settings->country_code,
            wifi_settings->ap.ssid, wifi_settings->ap.pass, wifi_settings->channel, wifi_settings->sta.ssid,
            "********", wifi_settings->channel, wifi_settings->mode == JW_WIFI_MODE_STA || wifi_settings->mode == JW_WIFI_MODE_APSTA);
        if (len >= remaining) { ESP_LOGE(TAG, "Buffer overflow in main/devices"); return; }
        buf_ptr += len; remaining -= len;
    }
    else {
        char pre[80];
        switch (type) {
            case JW_SERVER_WS_WIFI: snprintf(pre, sizeof(pre), "wifi-%s", key); break;
            case JW_SERVER_WS_RTC: snprintf(pre, sizeof(pre), "rtc-%s", key); break;
            case JW_SERVER_WS_LOG: snprintf(pre, sizeof(pre), "log-%s", key); break;
            case JW_SERVER_WS_BOARD: snprintf(pre, sizeof(pre), "device-%s", key); break;
            default: snprintf(pre, sizeof(pre), "%s", key); break;
        }
        int len = snprintf(buf_ptr, remaining, "\"%s\":\"%s\"", pre, val);
        if (len >= remaining) { ESP_LOGE(TAG, "Buffer overflow in other"); return; }
        buf_ptr += len; remaining -= len;
    }
    if (remaining < 2) { ESP_LOGE(TAG, "No space for closing"); return; }
    *buf_ptr++ = '}';
    *buf_ptr++ = 0;
}

/**
 * @brief Serve HTML directory listing for file browsing
 */
static esp_err_t jw_http_resp_dir_html(httpd_req_t *req, const char *dirpath, char *mac_id) {
    if (!req || !dirpath || !mac_id) {
        ESP_LOGE(TAG, "Invalid parameters for directory listing");
        return ESP_ERR_INVALID_ARG;
    }

    char entrypath[255];
    DIR *dir = opendir(dirpath);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", dirpath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory does not exist");
        return ESP_FAIL;
    }

    strlcpy(entrypath, dirpath, sizeof(entrypath));
    extern const unsigned char header_script_start[] asm("_binary_header_script_html_start");
    extern const unsigned char header_script_end[] asm("_binary_header_script_html_end");
    const size_t header_script_size = (header_script_end - header_script_start);
    httpd_resp_send_chunk(req, (const char *)header_script_start, header_script_size);

    httpd_resp_sendstr_chunk(req, "<section class=\"hidden\"><div id=\"mac_id\">");
    httpd_resp_sendstr_chunk(req, mac_id);
    httpd_resp_sendstr_chunk(req, "</div></section>");
    httpd_resp_sendstr_chunk(req,
        "<section id=\"table\"><div id=\"content\">"
        "<table width=\"80%\" style=\"margin-left:auto; margin-right:auto;\" border=\"1\">"
        "<col width=\"80%\" /><col width=\"20%\" />"
        "<thead><tr><th>Name</th><th>Delete</th></tr></thead>"
        "<tbody>");

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        httpd_resp_sendstr_chunk(req, "<tr><td><a href=\"");
        httpd_resp_sendstr_chunk(req, req->uri);
        httpd_resp_sendstr_chunk(req, entry->d_name);
        httpd_resp_sendstr_chunk(req, "\" target=\"_blank\" style=\"color: white; font-size: large;\">");
        httpd_resp_sendstr_chunk(req, entry->d_name);
        httpd_resp_sendstr_chunk(req, "</a></td><td style=\"text-align:center;\">");
        httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/delete");
        httpd_resp_sendstr_chunk(req, req->uri);
        httpd_resp_sendstr_chunk(req, entry->d_name);
        httpd_resp_sendstr_chunk(req, "\"><button type=\"submit\">Delete</button></form>");
        httpd_resp_sendstr_chunk(req, "</td></tr>\n");
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "</tbody></table><br><br></div></section>");
    extern const unsigned char footer_script_start[] asm("_binary_footer_script_html_start");
    extern const unsigned char footer_script_end[] asm("_binary_footer_script_html_end");
    const size_t footer_script_size = (footer_script_end - footer_script_start);
    httpd_resp_send_chunk(req, (const char *)footer_script_start, footer_script_size);
    httpd_resp_sendstr_chunk(req, "</section></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/**
 * @brief Task to handle WebSocket updates and client communication
 */
static void jw_server_web_server_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting web server task");
    QueueHandle_t q_update_settings = *jw_server_params.device_update_queue;
    jw_server_update_data_t *server_updates;
    int client_fds[JW_SERVER_MAX_CLIENTS];

    for (;;) {
        if (xQueueReceive(jw_server_context.updates_queue, &server_updates, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "Received update, type: %d", server_updates->type);
            jw_server_data_type_t update_type = server_updates->type;
            int socket_id = server_updates->socket_id;
            char key[64] = { 0 }, val[64] = { 0 };

            switch (update_type) {
                case JW_SERVER_WS_BOARD:
                case JW_SERVER_WS_MAIN:
                case JW_SERVER_WS_WIFI:
                case JW_SERVER_WS_RTC:
                case JW_SERVER_WS_LOG:
                    strlcpy(key, server_updates->data.key, sizeof(key));
                    strlcpy(val, server_updates->data.val, sizeof(val));
                    break;
                case JW_SERVER_WS_REBOOT:
                    esp_restart();
                    break;
                case JW_SERVER_WS_DEVICES:
                    free(server_updates);
                    break;
                case JW_SERVER_WS_REG_DEVICE:
                    strlcpy(val, key, sizeof(val));
                    strlcpy(key, "reg_device", sizeof(key));
                    free(server_updates);
                    break;
                case JW_SERVER_WS_NEW_PEER:
                case JW_SERVER_WS_CHANGE_CHAN:
                    free(server_updates);
                    continue;
                case JW_SERVER_WS_PEER_REMOVE:
                case JW_SERVER_WS_CLEAR_PEERS:
                    if (xQueueSend(q_update_settings, &server_updates, pdMS_TO_TICKS(512)) != pdTRUE) {
                        ESP_LOGW(TAG, "Failed to send update to device queue");
                        free(server_updates);
                    }
                    continue;
                default:
                    ESP_LOGW(TAG, "Unknown update type: %d, discarding", update_type);
                    free(server_updates);
                    continue;
            }

            size_t clients = JW_SERVER_MAX_CLIENTS;
            if (httpd_get_client_list(jw_server_context.server_handle, &clients, client_fds) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get client list");
                free(server_updates);
                continue;
            }

            char *response = heap_caps_malloc(JW_SERVER_JSON_RESPONSE_SIZE, MALLOC_CAP_SPIRAM);
            if (!response) {
                ESP_LOGE(TAG, "Failed to allocate response buffer");
                free(server_updates);
                continue;
            }
            memset(response, 0, JW_SERVER_JSON_RESPONSE_SIZE);
            jw_server_format_data(response, update_type, key, val);

            if (update_type == JW_SERVER_WS_MAIN || update_type == JW_SERVER_WS_DEVICES || update_type == JW_SERVER_WS_REG_DEVICE) {
                if (httpd_ws_get_fd_info(jw_server_context.server_handle, socket_id) == HTTPD_WS_CLIENT_WEBSOCKET) {
                    jw_server_send_ws_data(socket_id, response, strlen(response));
                }
            }
            else {
                for (size_t i = 0; i < clients; i++) {
                    if (httpd_ws_get_fd_info(jw_server_context.server_handle, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                        jw_server_send_ws_data(client_fds[i], response, strlen(response));
                    }
                }
            }
            free(response);
        }
    }
}

/**
 * @brief Task to periodically send status updates to WebSocket clients
 */
static void jw_server_web_status_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting web status task");
    char json_response[JW_SERVER_JSON_RESPONSE_SIZE];
    int client_fds[JW_SERVER_MAX_CLIENTS];
    TickType_t xLastWakeTime = xTaskGetTickCount();

    for (;;) {
        size_t clients = JW_SERVER_MAX_CLIENTS;
        if (httpd_get_client_list(jw_server_context.server_handle, &clients, client_fds) == ESP_OK && clients) {
            char time_buffer[80];
            snprintf(time_buffer, sizeof(time_buffer), "%02d:%02d:%02d",
                jw_rtc_time.tm_hour, jw_rtc_time.tm_min, jw_rtc_time.tm_sec);
            memset(json_response, 0, JW_SERVER_JSON_RESPONSE_SIZE);
            char *buf_ptr = json_response;
            *buf_ptr++ = '{';
            buf_ptr += snprintf(buf_ptr, JW_SERVER_JSON_RESPONSE_SIZE - (buf_ptr - json_response), "\"b_time\":\"%s\"", time_buffer);
            *buf_ptr++ = '}';
            *buf_ptr++ = 0;

            for (size_t i = 0; i < clients; i++) {
                if (httpd_ws_get_fd_info(jw_server_context.server_handle, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                    jw_server_send_ws_data(client_fds[i], json_response, strlen(json_response));
                }
            }
        }
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief Handle WebSocket requests and messages for /ws
 */
static esp_err_t jw_server_ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket /ws connection opened");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get /ws frame length: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t *buf = NULL;
    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate /ws buffer");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to receive /ws frame: %s", esp_err_to_name(err));
            free(buf);
            return err;
        }
        buf[ws_pkt.len] = '\0';
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        ESP_LOGD(TAG, "/ws received PONG from fd:%d", sockfd);
        free(buf);
        return jw_keep_alive_client_is_active(jw_server_context.keep_alive, sockfd);
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGD(TAG, "/ws received TEXT from fd:%d, len:%d, payload:%s", sockfd, ws_pkt.len, (char *)ws_pkt.payload);
        cJSON *json = cJSON_Parse((const char *)ws_pkt.payload);
        if (!json) {
            ESP_LOGE(TAG, "/ws failed to parse JSON: %s", cJSON_GetErrorPtr());
            free(buf);
            return ESP_FAIL;
        }

        jw_server_update_data_t *server_updates = calloc(1, sizeof(jw_server_update_data_t));
        if (!server_updates) {
            ESP_LOGE(TAG, "Failed to allocate /ws update data");
            cJSON_Delete(json);
            free(buf);
            return ESP_ERR_NO_MEM;
        }
        server_updates->socket_id = sockfd;

        cJSON *type = cJSON_GetObjectItem(json, "type");
        cJSON *key = cJSON_GetObjectItem(json, "key");
        cJSON *val = cJSON_GetObjectItem(json, "val");
        if (type && cJSON_IsNumber(type) && type->valueint >= JW_SERVER_WS_MAIN && type->valueint <= JW_SERVER_WS_CLEAR_PEERS) {
            server_updates->type = (jw_server_data_type_t)type->valueint;
        }
        else {
            ESP_LOGW(TAG, "/ws invalid or missing type: %d", type ? type->valueint : -1);
            free(server_updates);
            cJSON_Delete(json);
            free(buf);
            return ESP_FAIL;
        }
        if (key && cJSON_IsString(key)) {
            strlcpy(server_updates->data.key, key->valuestring, sizeof(server_updates->data.key));
        }
        if (val && cJSON_IsString(val)) {
            strlcpy(server_updates->data.val, val->valuestring, sizeof(server_updates->data.val));
        }

        if (xQueueSend(jw_server_context.updates_queue, &server_updates, pdMS_TO_TICKS(512)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to send /ws update to queue");
            free(server_updates);
        }
        cJSON_Delete(json);
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        ESP_LOGI(TAG, "/ws received PING from fd:%d, sending PONG", sockfd);
        ws_pkt.type = HTTPD_WS_TYPE_PONG;
        ws_pkt.len = 0;
        ws_pkt.payload = NULL;
        err = httpd_ws_send_frame(req, &ws_pkt);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send /ws PONG: %s", esp_err_to_name(err));
        }
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "/ws received CLOSE from fd:%d", sockfd);
    }

    free(buf);
    return ESP_OK;
}

/**
 * @brief Handle WebSocket requests and messages for /ws/nodes
 */
static esp_err_t jw_server_nodes_ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket /ws/nodes connection opened");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get /ws/nodes frame length: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t *buf = NULL;
    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate /ws/nodes buffer");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to receive /ws/nodes frame: %s", esp_err_to_name(err));
            free(buf);
            return err;
        }
        buf[ws_pkt.len] = '\0';
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        ESP_LOGD(TAG, "/ws/nodes received PONG from fd:%d", sockfd);
        free(buf);
        return jw_keep_alive_client_is_active(jw_server_context.keep_alive, sockfd);
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGD(TAG, "/ws/nodes received TEXT from fd:%d, len:%d", sockfd, ws_pkt.len);
        cJSON *json = cJSON_Parse((const char *)ws_pkt.payload);
        if (!json) {
            ESP_LOGE(TAG, "/ws/nodes failed to parse JSON: %s", cJSON_GetErrorPtr());
            free(buf);
            return ESP_FAIL;
        }

        cJSON *action = cJSON_GetObjectItem(json, "action");
        if (action && cJSON_IsString(action)) {
            ESP_LOGI(TAG, "/ws/nodes action: %s", action->valuestring);
            // Example: Handle node-specific actions (e.g., peer discovery)
            if (strcmp(action->valuestring, "discover") == 0) {
                ESP_LOGI(TAG, "/ws/nodes triggering peer discovery");
                // Add logic here if needed
            }
        }

        cJSON_Delete(json);
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        ESP_LOGI(TAG, "/ws/nodes received PING from fd:%d, sending PONG", sockfd);
        ws_pkt.type = HTTPD_WS_TYPE_PONG;
        ws_pkt.len = 0;
        ws_pkt.payload = NULL;
        err = httpd_ws_send_frame(req, &ws_pkt);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send /ws/nodes PONG: %s", esp_err_to_name(err));
        }
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "/ws/nodes received CLOSE from fd:%d", sockfd);
    }

    free(buf);
    return ESP_OK;
}

/**
 * @brief Handle WebSocket requests and messages for /ws/peers
 */
static esp_err_t jw_server_peers_ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket /ws/peers connection opened");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    esp_err_t err = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get /ws/peers frame length: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t *buf = NULL;
    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate /ws/peers buffer");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        err = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to receive /ws/peers frame: %s", esp_err_to_name(err));
            free(buf);
            return err;
        }
        buf[ws_pkt.len] = '\0';
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        ESP_LOGD(TAG, "/ws/peers received PONG from fd:%d", sockfd);
        free(buf);
        return jw_keep_alive_client_is_active(jw_server_context.keep_alive, sockfd);
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGD(TAG, "/ws/peers received TEXT from fd:%d, len:%d", sockfd, ws_pkt.len);
        cJSON *json = cJSON_Parse((const char *)ws_pkt.payload);
        if (!json) {
            ESP_LOGE(TAG, "/ws/peers failed to parse JSON: %s", cJSON_GetErrorPtr());
            free(buf);
            return ESP_FAIL;
        }

        cJSON *action = cJSON_GetObjectItem(json, "action");
        if (action && cJSON_IsString(action)) {
            ESP_LOGI(TAG, "/ws/peers action: %s", action->valuestring);
            // Example: Handle peer-specific actions (e.g., test message)
            if (strcmp(action->valuestring, "test_message") == 0) {
                ESP_LOGI(TAG, "/ws/peers sending test message response");
                cJSON *response = cJSON_CreateObject();
                cJSON_AddStringToObject(response, "status", "Test message received");
                char *response_str = cJSON_PrintUnformatted(response);
                jw_server_send_ws_data(sockfd, response_str, strlen(response_str));
                cJSON_free(response_str);
                cJSON_Delete(response);
            }
        }

        cJSON_Delete(json);
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        ESP_LOGI(TAG, "/ws/peers received PING from fd:%d, sending PONG", sockfd);
        ws_pkt.type = HTTPD_WS_TYPE_PONG;
        ws_pkt.len = 0;
        ws_pkt.payload = NULL;
        err = httpd_ws_send_frame(req, &ws_pkt);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send /ws/peers PONG: %s", esp_err_to_name(err));
        }
    }
    else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "/ws/peers received CLOSE from fd:%d", sockfd);
    }

    free(buf);
    return ESP_OK;
}

/**
 * @brief Serve favicon.ico file
 */
static esp_err_t jw_server_favicon_get_handler(httpd_req_t *req) {
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[] asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
}

/**
 * @brief Serve main.css file
 */
static esp_err_t jw_server_css_get_handler(httpd_req_t *req) {
    extern const char css_start[] asm("_binary_main_css_start");
    extern const char css_end[] asm("_binary_main_css_end");
    const uint32_t css_len = css_end - css_start;
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, css_start, css_len);
}

/**
 * @brief Serve jquery.js file
 */
static esp_err_t jw_server_js_get_handler(httpd_req_t *req) {
    extern const char js_start[] asm("_binary_jquery_js_start");
    extern const char js_end[] asm("_binary_jquery_js_end");
    const uint32_t js_len = js_end - js_start;
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req, js_start, js_len);
}

/**
 * @brief Serve root HTML page
 */
static esp_err_t jw_server_root_get_handler(httpd_req_t *req) {
    extern const char root_start[] asm("_binary_root_html_start");
    extern const char root_end[] asm("_binary_root_html_end");
    const uint32_t root_len = root_end - root_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, root_start, root_len);
}

/**
 * @brief Serve device HTML page
 */
static esp_err_t jw_server_device_get_handler(httpd_req_t *req) {
    extern const char device_start[] asm("_binary_device_html_start");
    extern const char device_end[] asm("_binary_device_html_end");
    const uint32_t device_len = device_end - device_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, device_start, device_len);
}

/**
 * @brief Serve devices HTML page
 */
static esp_err_t jw_server_devices_get_handler(httpd_req_t *req) {
    extern const char devices_start[] asm("_binary_devices_html_start");
    extern const char devices_end[] asm("_binary_devices_html_end");
    const uint32_t devices_len = devices_end - devices_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, devices_start, devices_len);
}

/**
 * @brief Serve file downloads from SD card
 */
static esp_err_t jw_server_download_get_handler(httpd_req_t *req) {
    char filepath[255];
    const char *filename = get_path_from_uri(filepath, JW_SDCARD_MOUNT_POINT, req->uri, sizeof(filepath));
    if (!filename || strlen(filename) + strlen(jw_server_context.file_data->base_path) + 1 > sizeof(filepath)) {
        ESP_LOGE(TAG, "Filename too long or invalid");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    strcpy(filepath, jw_server_context.file_data->base_path);
    strcat(filepath, filename);
    if (filename[strlen(filename) - 1] == '/') {
        char buf[128];
        memcpy(buf, filename + 1, strlen(filename) - 2);
        buf[strlen(filename) - 2] = 0;
        return jw_http_resp_dir_html(req, filepath, buf);
    }

    FILE *fd = fopen(filepath, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Sending file: %s", filename);
    httpd_resp_set_type(req, "text/plain");
    char *chunk = jw_server_context.file_data->scratch;
    size_t chunksize;
    do {
        chunksize = fread(chunk, 1, JW_SERVER_SCRATCH_BUFSIZE, fd);
        if (chunksize > 0 && httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
            fclose(fd);
            ESP_LOGE(TAG, "File sending failed: %s", filename);
            httpd_resp_sendstr_chunk(req, NULL);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
            return ESP_FAIL;
        }
    } while (chunksize != 0);

    fclose(fd);
    ESP_LOGD(TAG, "File sent: %s", filename);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/**
 * @brief Handle file deletion via POST request
 */
static esp_err_t jw_server_delete_post_handler(httpd_req_t *req) {
    char filepath[255];
    const char *filename = get_path_from_uri(filepath, JW_SDCARD_MOUNT_POINT, req->uri + sizeof("/delete") - 1, sizeof(filepath));
    if (!filename || strlen(filename) + strlen(jw_server_context.file_data->base_path) + 1 > sizeof(filepath)) {
        ESP_LOGE(TAG, "Filename too long or invalid");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    strcpy(filepath, jw_server_context.file_data->base_path);
    strcat(filepath, filename);
    if (filename[strlen(filename) - 1] == '/') {
        ESP_LOGE(TAG, "Invalid filename for delete: %s", filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Deleting file: %s", filename);
    if (unlink(filepath) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
        return ESP_FAIL;
    }

    char redirect_location[32] = { 0 };
    char buf[128] = { 0 };
    uint8_t remove_front_len = strlen("/delete");
    memcpy(buf, req->uri + remove_front_len, strlen(req->uri) - remove_front_len);
    httpd_resp_set_status(req, "303 See Other");
    char *location = strtok(buf, "/");
    if (location) {
        snprintf(redirect_location, sizeof(redirect_location), "/%s/", location);
        httpd_resp_set_hdr(req, "Location", redirect_location);
    }
    else {
        httpd_resp_set_hdr(req, "Location", "/");
    }
    httpd_resp_sendstr(req, "File deleted successfully");
    return ESP_OK;
}

/**
 * @brief Callback for WebSocket client connection opening
 */
static esp_err_t jw_server_ws_open_fd(httpd_handle_t hd, int sockfd) {
    ESP_LOGI(TAG, "New client connected: fd:%d", sockfd);
    return jw_keep_alive_add_client(jw_server_context.keep_alive, sockfd);
}

/**
 * @brief Callback for WebSocket client connection closing
 */
static void jw_server_ws_close_fd(httpd_handle_t hd, int sockfd) {
    ESP_LOGI(TAG, "Client disconnected: fd:%d", sockfd);
    jw_keep_alive_remove_client(jw_server_context.keep_alive, sockfd);
    close(sockfd);
}

/**
 * @brief Send data over WebSocket to a specific client
 */
esp_err_t jw_server_send_ws_data(int sockfd, const char *data, size_t len) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    httpd_ws_frame_t ws_pkt = {
        .payload = (uint8_t *)data,
        .len = len,
        .type = HTTPD_WS_TYPE_TEXT
    };
    esp_err_t err = httpd_ws_send_frame_async(jw_server_context.server_handle, sockfd, &ws_pkt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send WebSocket data to fd:%d: %s", sockfd, esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief Asynchronous WebSocket send function
 */
static void jw_server_ws_send_async(void *arg) {
    ws_async_resp_arg_t *resp_arg = arg;
    if (!resp_arg || !resp_arg->hd || !resp_arg->data) {
        free(resp_arg ? resp_arg->data : NULL);
        free(resp_arg);
        return;
    }
    httpd_ws_frame_t ws_pkt = {
        .payload = resp_arg->data,
        .len = resp_arg->len,
        .type = HTTPD_WS_TYPE_TEXT
    };
    httpd_ws_send_frame_async(resp_arg->hd, resp_arg->fd, &ws_pkt);
    free(resp_arg->data);
    free(resp_arg);
}

/**
 * @brief Keep-alive callback to check client status
 */
static bool jw_server_check_client_alive_cb(jw_keep_alive_t h, int fd) {
    ESP_LOGD(TAG, "Checking if client is alive: fd:%d", fd);
    ws_async_resp_arg_t *resp_arg = malloc(sizeof(ws_async_resp_arg_t));
    if (!resp_arg) {
        ESP_LOGE(TAG, "Failed to allocate async response arg for fd:%d", fd);
        return false;
    }
    resp_arg->hd = jw_keep_alive_get_user_ctx(h);
    resp_arg->fd = fd;
    resp_arg->data = NULL;
    resp_arg->len = 0;
    if (httpd_queue_work(resp_arg->hd, jw_server_ws_send_async, resp_arg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to queue PING for fd:%d", fd);
        free(resp_arg);
        return false;
    }
    return true;
}

/**
 * @brief Keep-alive callback for inactive clients
 */
static bool jw_server_client_not_alive_cb(jw_keep_alive_t h, int fd) {
    ESP_LOGE(TAG, "Client not alive, closing fd:%d", fd);
    httpd_sess_trigger_close(jw_keep_alive_get_user_ctx(h), fd);
    return true;
}

/**
 * @brief Notify WebSocket clients about found ESP-NOW peers
 */
void jw_server_notify_found_peers(cJSON *msg) {
    if (jw_server_context.server_handle && nodes_uri_registered &&
        xQueueSend(jw_server_context.updates_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to queue found_peers notification");
        cJSON_Delete(msg);
    }
}

/**
 * @brief Unregister the /ws/nodes WebSocket URI
 */
esp_err_t jw_server_unregister_nodes_uri(void) {
    if (!jw_server_context.server_handle || !nodes_uri_registered) {
        ESP_LOGW(TAG, "Server not started or /ws/nodes not registered");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_uri_t ws_nodes_uri = {
        .uri = jw_server_nodes.uri,
        .method = HTTP_GET,
        .handler = NULL,
        .user_ctx = NULL,
        .is_websocket = true
    };
    esp_err_t ret = httpd_unregister_uri_handler(jw_server_context.server_handle, ws_nodes_uri.uri, ws_nodes_uri.method);
    if (ret == ESP_OK) {
        nodes_uri_registered = false;
        ESP_LOGI(TAG, "Unregistered /ws/nodes URI");
    }
    else {
        ESP_LOGE(TAG, "Failed to unregister /ws/nodes URI: %s", esp_err_to_name(ret));
    }
    return ret;
}

