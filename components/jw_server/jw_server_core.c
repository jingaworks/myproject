#include "jw_server.h"
#include "jw_keep_alive.h"
#include "jw_log.h"

static httpd_handle_t server = NULL;

void jw_server_core_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16; // For multiple endpoints
    config.stack_size = 8192;     // PSRAM-friendly
    if (httpd_start(&server, &config) == ESP_OK) {
        jw_log_msg("Server started");
        xTaskCreate(jw_server_web_server_task, "server", 4096, NULL, 5, NULL);
        xTaskCreate(jw_server_web_status_task, "status", 4096, NULL, 5, NULL);
    }
}

httpd_handle_t jw_server_core_get_server(void) {
    return server;
}

void jw_server_core_parse_json(const char* data, cJSON** json) {
    *json = cJSON_Parse(data);
    if (!*json) jw_log_msg("JSON parse error");
}