#ifndef JW_SERVER_H
#define JW_SERVER_H

#include "esp_http_server.h"
#include "cJSON.h"

// Core
void jw_server_core_init(void);
httpd_handle_t jw_server_core_get_server(void);
void jw_server_core_parse_json(const char* data, cJSON** json); // Shared JSON parser

// HTTP
void jw_server_http_start(httpd_handle_t server);
void jw_server_http_stop(void);

// WebSocket
void jw_server_ws_start(httpd_handle_t server);
void jw_server_ws_stop(void);
void jw_server_ws_send_peers_update(void); // Example WS function

#endif