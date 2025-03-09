#include "jw_server.h"
#include "jw_peers.h"
#include "jw_espnow.h"
#include "jw_keep_alive.h"

static void ws_handler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        jw_keep_alive_add(req);
        return;
    }
    uint8_t buf[256] = {0};
    int ret = httpd_ws_recv_frame(req, buf, sizeof(buf));
    if (ret >= 0) {
        cJSON* json = NULL;
        jw_server_core_parse_json((char*)buf, &json);
        if (json) {
            int type = cJSON_GetObjectItem(json, "type")->valueint;
            const char* key = cJSON_GetObjectItem(json, "key")->valuestring;
            const char* val = cJSON_GetObjectItem(json, "val")->valuestring;
            if (type == 9 && strcmp(key, "confirm_peer") == 0) {
                jw_peers_add(val);
                jw_espnow_peer(val);
            }
            cJSON_Delete(json);
        }
    }
}

void jw_server_ws_start(httpd_handle_t server) {
    httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
    httpd_uri_t ws_nodes = { .uri = "/ws/nodes", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
    httpd_uri_t ws_peers = { .uri = "/ws/peers", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
    httpd_register_uri_handler(server, &ws);
    httpd_register_uri_handler(server, &ws_nodes);
    httpd_register_uri_handler(server, &ws_peers);
    jw_log_msg("WebSocket endpoints registered");
}

void jw_server_ws_stop(void) {
    jw_keep_alive_clear();
}

void jw_server_ws_send_peers_update(void) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "type", 1);
    cJSON_AddStringToObject(json, "key", "peers");
    char* peers = jw_peers_get_all();
    cJSON_AddStringToObject(json, "val", peers);
    char* rendered = cJSON_PrintUnformatted(json);
    httpd_ws_send_frame_to_clients("/ws/peers", rendered, strlen(rendered));
    free(rendered);
    cJSON_Delete(json);
}