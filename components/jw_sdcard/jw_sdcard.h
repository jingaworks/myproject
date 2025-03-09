#ifndef JW_SDCARD_H
#define JW_SDCARD_H

#include <esp_err.h>
#include <stdbool.h>  // Added to define bool type

/** @brief Mount point for SD card filesystem */
#define JW_SDCARD_MOUNT_POINT "/sdcard"

/**
 * @brief Initialize the SD card module
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 *
 * Mounts the SD card filesystem for use.
 */
esp_err_t jw_sdcard_init(void);

/**
 * @brief Check if SD card is mounted
 * @return bool True if mounted, false otherwise
 *
 * Verifies if the SD card is currently accessible.
 */
bool jw_sdcard_is_mounted(void);

/**
 * @brief Write data to a file on the SD card
 * @param path Full path to the file
 * @param data Data to write
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 *
 * Writes the specified data to a file, creating directories if needed.
 */
esp_err_t jw_sdcard_write_file(const char *path, const char *data);

#endif // JW_SDCARD_H