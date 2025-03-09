#include "jw_server.h"
#include "jw_sdcard.h"

static esp_err_t root_handler(httpd_req_t* req) {
    const char* html = "/sdcard/index.html";
    FILE* f = fopen(html, "r");
    if (f) {
        char buf[1024];
        size_t len = fread(buf, 1, sizeof(buf), f);
        httpd_resp_send(req, buf, len);
        fclose(f);
    } else {
        httpd_resp_send_404(req);
    }
    return ESP_OK;
}

static esp_err_t config_handler(httpd_req_t* req) {
    httpd_resp_send(req, "Config OK", 9);
    return ESP_OK;
}

void jw_server_http_start(httpd_handle_t server) {
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t config = { .uri = "/api/config", .method = HTTP_GET, .handler = config_handler };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &config);
    jw_log_msg("HTTP endpoints registered");
}

void jw_server_http_stop(void) {
    // Cleanup if needed
}