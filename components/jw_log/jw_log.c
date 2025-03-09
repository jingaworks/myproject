#include "jw_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "jw_sdcard.h"

#define TAG "JW_LOG"
#define JW_LOG_QUEUE_SIZE 20
#define JW_LOG_NVS_NAMESPACE "jw_log"
#define JW_LOG_NVS_FALLBACK_KEY "fallback_logs"

struct jw_log_context {
    QueueHandle_t log_queue;
    TaskHandle_t log_task;
    jw_log_entry_t fallback_logs[JW_LOG_FALLBACK_SIZE];
    uint8_t fallback_log_index;
    SemaphoreHandle_t mutex;
};

static jw_log_context_t *jw_log_context = NULL;

static void jw_log_run_task(void *params);
static void save_fallback_to_nvs(void);
static void add_fallback_log(const char *message);

esp_err_t jw_log_init(void) {
    if (jw_log_context != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    jw_log_context = heap_caps_malloc(sizeof(jw_log_context_t), MALLOC_CAP_SPIRAM);
    if (!jw_log_context) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return ESP_ERR_NO_MEM;
    }

    jw_log_context->log_queue = xQueueCreate(JW_LOG_QUEUE_SIZE, sizeof(jw_log_entry_t) + 64);
    jw_log_context->mutex = xSemaphoreCreateMutex();
    if (!jw_log_context->log_queue || !jw_log_context->mutex) {
        ESP_LOGE(TAG, "Failed to create queue or mutex");
        heap_caps_free(jw_log_context);
        jw_log_context = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(jw_log_context->fallback_logs, 0, sizeof(jw_log_context->fallback_logs));
    jw_log_context->fallback_log_index = 0;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(JW_LOG_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        goto cleanup;
    }

    size_t size = sizeof(jw_log_context->fallback_logs) + sizeof(uint8_t);
    uint8_t *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for NVS load");
        nvs_close(nvs_handle);
        goto cleanup;
    }

    err = nvs_get_blob(nvs_handle, JW_LOG_NVS_FALLBACK_KEY, buffer, &size);
    if (err == ESP_OK) {
        memcpy(jw_log_context->fallback_logs, buffer, sizeof(jw_log_context->fallback_logs));
        jw_log_context->fallback_log_index = buffer[sizeof(jw_log_context->fallback_logs)];
        ESP_LOGI(TAG, "Loaded fallback logs from NVS, index: %d", jw_log_context->fallback_log_index);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No fallback logs found in NVS, starting fresh");
    }
    else {
        ESP_LOGE(TAG, "NVS load failed: %s", esp_err_to_name(err));
    }
    heap_caps_free(buffer);
    nvs_close(nvs_handle);

    if (xTaskCreate(jw_log_run_task, "jw_log_task", 4096, NULL, 3, &jw_log_context->log_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create log task");
        goto cleanup;
    }

    return ESP_OK;

cleanup:
    if (jw_log_context->log_queue) vQueueDelete(jw_log_context->log_queue);
    if (jw_log_context->mutex) vSemaphoreDelete(jw_log_context->mutex);
    heap_caps_free(jw_log_context);
    jw_log_context = NULL;
    return ESP_FAIL;
}

esp_err_t jw_log_write(jw_log_level_t level, const char *path, const char *message) {
    if (!jw_log_context || !path || !message) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    char full_message[128];
    const char *level_str = (level == JW_LOG_INFO) ? "INFO" : (level == JW_LOG_WARNING) ? "WARN" : "ERROR";
    snprintf(full_message, sizeof(full_message), "[%s] %s", level_str, message);

    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", full_message);
        fclose(f);
        return ESP_OK;
    }
    else {
        ESP_LOGW(TAG, "Failed to write to SD card at %s", path);
        add_fallback_log(full_message);
        return ESP_FAIL;
    }
}

esp_err_t jw_log_get_fallback(jw_log_entry_t **logs, uint8_t *log_count, uint8_t *current_index) {
    if (!jw_log_context || !logs || !log_count || !current_index) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(jw_log_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_ERR_TIMEOUT;
    }

    *logs = jw_log_context->fallback_logs;
    *log_count = JW_LOG_FALLBACK_SIZE;
    *current_index = jw_log_context->fallback_log_index;
    xSemaphoreGive(jw_log_context->mutex);
    return ESP_OK;
}

static void jw_log_run_task(void *params) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // Idle task
    }
}

static void add_fallback_log(const char *message) {
    if (xSemaphoreTake(jw_log_context->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex in add_fallback_log");
        return;
    }

    jw_log_entry_t *log = &jw_log_context->fallback_logs[jw_log_context->fallback_log_index];
    log->timestamp = xTaskGetTickCount() / portTICK_PERIOD_MS / 1000;
    strncpy(log->message, message, sizeof(log->message) - 1);
    log->message[sizeof(log->message) - 1] = '\0';

    jw_log_context->fallback_log_index = (jw_log_context->fallback_log_index + 1) % JW_LOG_FALLBACK_SIZE;
    save_fallback_to_nvs();

    xSemaphoreGive(jw_log_context->mutex);
}

static void save_fallback_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(JW_LOG_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for fallback logs: %s", esp_err_to_name(err));
        return;
    }

    size_t size = sizeof(jw_log_context->fallback_logs) + sizeof(uint8_t);
    uint8_t *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for fallback logs");
        nvs_close(nvs_handle);
        return;
    }

    memcpy(buffer, jw_log_context->fallback_logs, sizeof(jw_log_context->fallback_logs));
    buffer[sizeof(jw_log_context->fallback_logs)] = jw_log_context->fallback_log_index;

    err = nvs_set_blob(nvs_handle, JW_LOG_NVS_FALLBACK_KEY, buffer, size);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save fallback logs to NVS: %s", esp_err_to_name(err));
    }
    else {
        ESP_LOGI(TAG, "Saved fallback logs to NVS, index: %d", jw_log_context->fallback_log_index);
    }

    heap_caps_free(buffer);
    nvs_close(nvs_handle);
}