#include <esp_system.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "jw_keep_alive.h"

typedef enum {
    JW_NO_CLIENT = 0,
    JW_CLIENT_FD_ADD,
    JW_CLIENT_FD_REMOVE,
    JW_CLIENT_UPDATE,
    JW_CLIENT_ACTIVE,
    JW_STOP_TASK,
} jw_client_fd_action_type_t;

typedef struct {
    jw_client_fd_action_type_t type;
    int fd;
    uint64_t last_seen;
} jw_client_fd_action_t;

struct jw_keep_alive_storage {
    size_t max_clients;
    jw_keep_alive_check_client_alive_cb_t check_client_alive_cb;
    jw_keep_alive_client_not_alive_cb_t client_not_alive_cb;
    size_t keep_alive_period_ms;
    size_t not_alive_after_ms;
    void *user_ctx;
    QueueHandle_t q;
    jw_client_fd_action_t clients[];
};

static const char *JW_KEEP_ALIVE_TAG = "jw_keep_alive";

static uint64_t jw_keep_alive_tick_get_ms(void) {
    return esp_timer_get_time() / 1000;
}

static uint64_t jw_keep_alive_get_max_delay(jw_keep_alive_t h) {
    int64_t check_after_ms = 30000;
    uint64_t now = jw_keep_alive_tick_get_ms();
    for (int i = 0; i < h->max_clients; ++i) {
        if (h->clients[i].type == JW_CLIENT_ACTIVE) {
            uint64_t check_this_client_at = h->clients[i].last_seen + h->keep_alive_period_ms;
            if (check_this_client_at < check_after_ms + now) {
                check_after_ms = check_this_client_at - now;
                if (check_after_ms < 0) check_after_ms = 1000;
            }
        }
    }
    return check_after_ms;
}

static bool jw_keep_alive_update_client(jw_keep_alive_t h, int sockfd, uint64_t timestamp) {
    for (int i = 0; i < h->max_clients; ++i) {
        if (h->clients[i].type == JW_CLIENT_ACTIVE && h->clients[i].fd == sockfd) {
            h->clients[i].last_seen = timestamp;
            return true;
        }
    }
    return false;
}

static bool jw_keep_alive_remove_client_internal(jw_keep_alive_t h, int sockfd) {
    for (int i = 0; i < h->max_clients; ++i) {
        if (h->clients[i].type == JW_CLIENT_ACTIVE && h->clients[i].fd == sockfd) {
            h->clients[i].type = JW_NO_CLIENT;
            h->clients[i].fd = -1;
            return true;
        }
    }
    return false;
}

static bool jw_keep_alive_add_client_internal(jw_keep_alive_t h, int sockfd) {
    for (int i = 0; i < h->max_clients; ++i) {
        if (h->clients[i].type == JW_NO_CLIENT) {
            h->clients[i].type = JW_CLIENT_ACTIVE;
            h->clients[i].fd = sockfd;
            h->clients[i].last_seen = jw_keep_alive_tick_get_ms();
            return true;
        }
    }
    return false;
}

static void jw_keep_alive_task(void *arg) {
    jw_keep_alive_t keep_alive_storage = arg;
    bool run_task = true;
    jw_client_fd_action_t client_action;
    while (run_task) {
        if (xQueueReceive(keep_alive_storage->q, (void *)&client_action, jw_keep_alive_get_max_delay(keep_alive_storage) / portTICK_PERIOD_MS) == pdTRUE) {
            switch (client_action.type) {
                case JW_CLIENT_FD_ADD:
                    if (!jw_keep_alive_add_client_internal(keep_alive_storage, client_action.fd)) {
                        ESP_LOGE(JW_KEEP_ALIVE_TAG, "Cannot add new client");
                    }
                    break;
                case JW_CLIENT_FD_REMOVE:
                    if (!jw_keep_alive_remove_client_internal(keep_alive_storage, client_action.fd)) {
                        ESP_LOGE(JW_KEEP_ALIVE_TAG, "Cannot remove client fd:%d", client_action.fd);
                    }
                    break;
                case JW_CLIENT_UPDATE:
                    if (!jw_keep_alive_update_client(keep_alive_storage, client_action.fd, client_action.last_seen)) {
                        ESP_LOGE(JW_KEEP_ALIVE_TAG, "Cannot find client fd:%d", client_action.fd);
                    }
                    break;
                case JW_STOP_TASK:
                    run_task = false;
                    break;
                default:
                    ESP_LOGE(JW_KEEP_ALIVE_TAG, "Unexpected client action");
                    break;
            }
        } else {
            uint64_t now = jw_keep_alive_tick_get_ms();
            for (int i = 0; i < keep_alive_storage->max_clients; ++i) {
                if (keep_alive_storage->clients[i].type == JW_CLIENT_ACTIVE) {
                    if (keep_alive_storage->clients[i].last_seen + keep_alive_storage->keep_alive_period_ms <= now) {
                        ESP_LOGD(JW_KEEP_ALIVE_TAG, "Haven't seen the client (fd=%d) for a while", keep_alive_storage->clients[i].fd);
                        if (keep_alive_storage->clients[i].last_seen + keep_alive_storage->not_alive_after_ms <= now) {
                            keep_alive_storage->client_not_alive_cb(keep_alive_storage, keep_alive_storage->clients[i].fd);
                        } else {
                            keep_alive_storage->check_client_alive_cb(keep_alive_storage, keep_alive_storage->clients[i].fd);
                        }
                    }
                }
            }
        }
    }
    vQueueDelete(keep_alive_storage->q);
    free(keep_alive_storage);
    vTaskDelete(NULL);
}

jw_keep_alive_t jw_keep_alive_start(jw_keep_alive_config_t *config) {
    size_t queue_size = config->max_clients / 2;
    size_t client_list_size = config->max_clients + queue_size;
    jw_keep_alive_t keep_alive_storage = calloc(1, sizeof(struct jw_keep_alive_storage) + client_list_size * sizeof(jw_client_fd_action_t));
    if (keep_alive_storage == NULL) return NULL;
    keep_alive_storage->check_client_alive_cb = config->check_client_alive_cb;
    keep_alive_storage->client_not_alive_cb = config->client_not_alive_cb;
    keep_alive_storage->max_clients = config->max_clients;
    keep_alive_storage->not_alive_after_ms = config->not_alive_after_ms;
    keep_alive_storage->keep_alive_period_ms = config->keep_alive_period_ms;
    keep_alive_storage->user_ctx = config->user_ctx;
    keep_alive_storage->q = xQueueCreate(queue_size, sizeof(jw_client_fd_action_t));
    if (xTaskCreate(jw_keep_alive_task, "keep_alive_task", config->task_stack_size, keep_alive_storage, config->task_prio, NULL) != pdTRUE) {
        jw_keep_alive_stop(keep_alive_storage);
        return NULL;
    }
    return keep_alive_storage;
}

void jw_keep_alive_stop(jw_keep_alive_t h) {
    jw_client_fd_action_t stop = { .type = JW_STOP_TASK };
    xQueueSendToFront(h->q, &stop, 0);
}

esp_err_t jw_keep_alive_add_client(jw_keep_alive_t h, int fd) {
    jw_client_fd_action_t client_fd_action = { .fd = fd, .type = JW_CLIENT_FD_ADD };
    if (xQueueSendToBack(h->q, &client_fd_action, 0) == pdTRUE) return ESP_OK;
    return ESP_FAIL;
}

esp_err_t jw_keep_alive_remove_client(jw_keep_alive_t h, int fd) {
    jw_client_fd_action_t client_fd_action = { .fd = fd, .type = JW_CLIENT_FD_REMOVE };
    if (xQueueSendToBack(h->q, &client_fd_action, 0) == pdTRUE) return ESP_OK;
    return ESP_FAIL;
}

esp_err_t jw_keep_alive_client_is_active(jw_keep_alive_t h, int fd) {
    jw_client_fd_action_t client_fd_action = { .fd = fd, .type = JW_CLIENT_UPDATE, .last_seen = jw_keep_alive_tick_get_ms() };
    if (xQueueSendToBack(h->q, &client_fd_action, 0) == pdTRUE) return ESP_OK;
    return ESP_FAIL;
}

void jw_keep_alive_set_user_ctx(jw_keep_alive_t h, void *ctx) {
    h->user_ctx = ctx;
}

void *jw_keep_alive_get_user_ctx(jw_keep_alive_t h) {
    return h->user_ctx;
}