#ifndef JW_COMMON_H
#define JW_COMMON_H

#include <esp_err.h>

/** @brief Opaque server context type (avoids jw_server.h dependency) */
typedef struct jw_server_context jw_server_context_t;

/** @brief Opaque server params type (avoids jw_server.h dependency) */
typedef struct jw_server_params jw_server_params_t;

/** @brief Interface structure for server operations used by jw_wifi */
typedef struct {
    esp_err_t(*server_start)(const jw_server_params_t *params);  // Function pointer to start the server
    esp_err_t(*server_stop)(void);                              // Function pointer to stop the server
    bool (*is_server_running)(void);                             // Function pointer to check server running state
    jw_server_params_t *params;                                  // Pointer to server parameters
    jw_server_context_t *context;                                // Pointer to server context (still included but not dereferenced)
} jw_wifi_server_interface_t;

#endif // JW_COMMON_H