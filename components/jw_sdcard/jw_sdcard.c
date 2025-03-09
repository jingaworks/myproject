#include "jw_sdcard.h"
#include <string.h>
#include <errno.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <esp_log.h>

/** @brief Logging tag for SD card module */
static const char *TAG = "JW_SDCARD";

/** @brief SD card slot configuration */
static sdmmc_host_t host = SDMMC_HOST_DEFAULT();
sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

/** @brief SD card handle */
static sdmmc_card_t *card = NULL;

/** @brief Flag indicating SD card mount status */
static bool is_mounted = false;

/**
 * @brief Initialize the SD card module
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 *
 * Mounts the SD card filesystem using SPI interface.
 */
esp_err_t jw_sdcard_init(void) {
    esp_err_t err;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    ESP_LOGI(TAG, "Initializing SD card");

    slot_config.width = 4;

    ESP_LOGI(TAG, "Mounting filesystem");
    err = esp_vfs_fat_sdmmc_mount(JW_SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        }
        else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                "Make sure SD card lines have pull-up resistors in place.",
                esp_err_to_name(err));
        }

        return ESP_FAIL;
    }

    is_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", JW_SDCARD_MOUNT_POINT);
    return ESP_OK;
}

/**
 * @brief Check if SD card is mounted
 * @return bool True if mounted, false otherwise
 *
 * Returns the current mount status of the SD card.
 */
bool jw_sdcard_is_mounted(void) {
    return is_mounted;
}

/**
 * @brief Write data to a file on the SD card
 * @param path Full path to the file
 * @param data Data to write
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 *
 * Writes data to the specified file, creating directories as needed.
 */
esp_err_t jw_sdcard_write_file(const char *path, const char *data) {
    if (!is_mounted) {
        ESP_LOGW(TAG, "SD card not mounted, cannot write");
        return ESP_FAIL;
    }

    char *dir_end = strrchr(path, '/');
    if (dir_end != path) {
        char dir_path[128];
        strncpy(dir_path, path, dir_end - path);
        dir_path[dir_end - path] = '\0';

        struct stat st;
        if (stat(dir_path, &st) != 0) {
            char *slash = dir_path;
            while ((slash = strchr(slash + 1, '/'))) {
                *slash = '\0';
                if (mkdir(dir_path, 0775) != 0 && errno != EEXIST) {
                    ESP_LOGE(TAG, "Failed to create directory %s: %s", dir_path, strerror(errno));
                    return ESP_FAIL;
                }
                *slash = '/';
            }
            if (mkdir(dir_path, 0775) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "Failed to create directory %s: %s", dir_path, strerror(errno));
                return ESP_FAIL;
            }
        }
    }

    FILE *fd = fopen(path, "a");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to open file %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    size_t len = strlen(data);
    size_t written = fwrite(data, 1, len, fd);
    if (written != len) {
        ESP_LOGE(TAG, "Failed to write to file %s", path);
        fclose(fd);
        return ESP_FAIL;
    }

    fclose(fd);
    return ESP_OK;
}