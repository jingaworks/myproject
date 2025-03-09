#ifndef JW_LOG_H
#define JW_LOG_H

#include <stdint.h>
#include "esp_err.h"

#define JW_LOG_FALLBACK_SIZE 10  // Circular log size for fallback

typedef enum {
    JW_LOG_INFO = 0,
    JW_LOG_WARNING,
    JW_LOG_ERROR
} jw_log_level_t;

typedef struct {
    uint32_t timestamp;  // Timestamp in seconds
    char message[64];    // Log message (larger to accommodate more detail)
} jw_log_entry_t;

typedef struct jw_log_context jw_log_context_t;

esp_err_t jw_log_init(void);
esp_err_t jw_log_write(jw_log_level_t level, const char *path, const char *message);
esp_err_t jw_log_get_fallback(jw_log_entry_t **logs, uint8_t *log_count, uint8_t *current_index);

// /**
//  * @brief Create a device-specific log directory
//  * @param mac_addr MAC address of the device (hex string)
//  * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
//  * 
//  * Creates a directory structure for device logs based on MAC address.
//  */
// esp_err_t jw_log_create_device_directory(const char *mac_addr);

#endif // JW_LOG_H