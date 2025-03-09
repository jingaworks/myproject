#include "jw_server.h"
#include "jw_log.h"

static httpd_handle_t server = NULL;

void jw_server_init(void) {
    jw_server_core_init(&server); // Pass server handle to core
    jw_log_msg("jw_server initialized");
}

void jw_server_start(void) {
    if (server) {
        jw_server_http_start(server); // Start HTTP
        jw_server_ws_start(server);   // Start WebSocket
        jw_log_msg("jw_server started");
    }
}

void jw_server_stop(void) {
    jw_server_ws_stop();   // Stop WebSocket first (cleans up clients)
    jw_server_http_stop(); // Stop HTTP
    if (server) {
        httpd_stop(server); // Stop the server instance
        server = NULL;
        jw_log_msg("jw_server stopped");
    }
}

void jw_server_send_peers_update(void) {
    jw_server_ws_send_peers_update(); // Delegate to WS module
}