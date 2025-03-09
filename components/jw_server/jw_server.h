#ifndef JW_SERVER_H
#define JW_SERVER_H

#include "esp_http_server.h"
#include "cJSON.h"

// Public Interface (jw_server.c)
void jw_server_init(void);           // Initialize the server component
void jw_server_start(void);          // Start HTTP and WebSocket services
void jw_server_stop(void);           // Stop all services
void jw_server_send_peers_update(void); // Trigger Peer info update via WS

// Internal (for jw_server_* modules, not called directly by main.c)
void jw_server_core_init(httpd_handle_t* server);
void jw_server_core_parse_json(const char* data, cJSON** json);
void jw_server_http_start(httpd_handle_t server);
void jw_server_http_stop(void);
void jw_server_ws_start(httpd_handle_t server);
void jw_server_ws_stop(void);

#endif