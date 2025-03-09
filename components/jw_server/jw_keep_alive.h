#ifndef KEEP_ALIVE_H
#define KEEP_ALIVE_H

#include <esp_err.h>
#include <stdbool.h>

#define JW_KEEP_ALIVE_CONFIG_DEFAULT()        \
    {                                         \
        .max_clients = 10,                    \
        .task_stack_size = 2048,              \
        .task_prio = tskIDLE_PRIORITY + 1,    \
        .keep_alive_period_ms = 10000,        \
        .not_alive_after_ms = 20000,          \
    }

struct jw_keep_alive_storage;
typedef struct jw_keep_alive_storage *jw_keep_alive_t;

typedef bool (*jw_keep_alive_check_client_alive_cb_t)(jw_keep_alive_t h, int fd);
typedef bool (*jw_keep_alive_client_not_alive_cb_t)(jw_keep_alive_t h, int fd);

/* Configuration struct for keep-alive engine */
typedef struct {
    size_t max_clients;                               // Maximum number of clients
    size_t task_stack_size;                           // Stack size of the created task
    size_t task_prio;                                 // Priority of the created task
    size_t keep_alive_period_ms;                      // Check every client after this time
    size_t not_alive_after_ms;                        // Consider client not alive after this time
    jw_keep_alive_check_client_alive_cb_t check_client_alive_cb; // Callback to check if client is alive
    jw_keep_alive_client_not_alive_cb_t client_not_alive_cb;     // Callback to notify client not alive
    void *user_ctx;                                   // User context available in keep-alive handle
} jw_keep_alive_config_t;

/* Adds a new client to the set of clients to monitor
 * @param[in] h Keep-alive handle
 * @param[in] fd Socket file descriptor for this client
 * @return ESP_OK on success
 */
esp_err_t jw_keep_alive_add_client(jw_keep_alive_t h, int fd);

/* Removes a client from the set
 * @param[in] h Keep-alive handle
 * @param[in] fd Socket file descriptor for this client
 * @return ESP_OK on success
 */
esp_err_t jw_keep_alive_remove_client(jw_keep_alive_t h, int fd);

/* Notifies that a client is alive
 * @param[in] h Keep-alive handle
 * @param[in] fd Socket file descriptor for this client
 * @return ESP_OK on success
 */
esp_err_t jw_keep_alive_client_is_active(jw_keep_alive_t h, int fd);

/* Starts the keep-alive engine
 * @param[in] config Keep-alive configuration
 * @return Keep-alive handle
 */
jw_keep_alive_t jw_keep_alive_start(jw_keep_alive_config_t *config);

/* Stops the keep-alive engine
 * @param[in] h Keep-alive handle
 */
void jw_keep_alive_stop(jw_keep_alive_t h);

/* Sets user-defined context
 * @param[in] h Keep-alive handle
 * @param[in] ctx User context
 */
void jw_keep_alive_set_user_ctx(jw_keep_alive_t h, void *ctx);

/* Gets user-defined context
 * @param[in] h Keep-alive handle
 * @return User context
 */
void *jw_keep_alive_get_user_ctx(jw_keep_alive_t h);

#endif // KEEP_ALIVE_H