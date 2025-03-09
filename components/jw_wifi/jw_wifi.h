#ifndef JW_WIFI_H
#define JW_WIFI_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include "jw_common.h"

/** @brief NVS key for station SSID */
#define JW_WIFI_NVS_KEY_STA_SSID "sta_ssid"
/** @brief NVS key for station password */
#define JW_WIFI_NVS_KEY_STA_PASS "sta_pass"
/** @brief NVS key for access point SSID */
#define JW_WIFI_NVS_KEY_AP_SSID "ap_ssid"
/** @brief NVS key for access point password */
#define JW_WIFI_NVS_KEY_AP_PASS "ap_pass"
/** @brief NVS key for Wi-Fi channel */
#define JW_WIFI_NVS_KEY_CHANNEL "channel"
/** @brief Event bit for Wi-Fi got IP */
#define JW_WIFI_GOT_IP_BIT BIT0
/** @brief Country code for United States (ISO 3166-1 numeric) */
#define WIFI_COUNTRY_CODE_US 39

/** @brief Enum for Wi-Fi user modes */
typedef enum {
    JW_WIFI_MODE_NO_WIFI,  // No Wi-Fi enabled
    JW_WIFI_MODE_STA,      // Station mode only
    JW_WIFI_MODE_AP,       // Access point mode only
    JW_WIFI_MODE_APSTA    // Both AP and STA modes
} jw_wifi_user_mode_t;

/** @brief Structure for access point settings */
typedef struct {
    char ssid[32];         // SSID of the access point
    char pass[64];         // Password of the access point
} jw_wifi_ap_settings_t;

/** @brief Structure for station settings */
typedef struct {
    char ssid[32];         // SSID of the station
    char pass[64];         // Password of the station
    esp_ip4_addr_t ip;     // IP address assigned to station
} jw_wifi_sta_settings_t;

/** @brief Structure for Wi-Fi settings */
typedef struct {
    jw_wifi_user_mode_t mode;      // Current Wi-Fi mode
    uint8_t channel;               // Wi-Fi channel (1-13)
    uint8_t country_code;          // Country code for Wi-Fi regulations
    jw_wifi_ap_settings_t ap;      // Access point settings
    jw_wifi_sta_settings_t sta;    // Station settings
} jw_wifi_settings_t;

/**
 * @brief Initialize the Wi-Fi module
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t jw_wifi_init(void);

/**
 * @brief Start the Wi-Fi module
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t jw_wifi_start(void);

/**
 * @brief Apply current Wi-Fi settings
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t jw_wifi_apply(void);

/**
 * @brief Save Wi-Fi configuration to NVS
 * @param key NVS key for the setting
 * @param value Value to save
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t jw_wifi_save_config(const char *key, const char *value);

/**
 * @brief Get current Wi-Fi settings
 * @return jw_wifi_settings_t* Pointer to current settings
 */
jw_wifi_settings_t *jw_wifi_get_settings(void);

/**
 * @brief Get Wi-Fi event group
 * @return EventGroupHandle_t Handle to Wi-Fi event group
 */
EventGroupHandle_t jw_wifi_get_event_group(void);

/**
 * @brief Set Wi-Fi mode
 * @param mode Desired Wi-Fi mode
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_ARG or ESP_FAIL on failure
 */
esp_err_t jw_wifi_set_mode(jw_wifi_user_mode_t mode);

/**
 * @brief Scan available Wi-Fi networks
 * @param ap_records Array to store AP records
 * @param max_count Maximum number of APs to scan
 * @param ap_count Pointer to number of APs found
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t jw_wifi_scan_networks(wifi_ap_record_t *ap_records, uint16_t max_count, uint16_t *ap_count);

/**
 * @brief Wi-Fi manager task
 * @param pvParameters Task parameters (unused)
 */
void jw_wifi_manager_task(void *pvParameters);

/**
 * @brief Set the server interface for Wi-Fi module
 * @param interface Pointer to the server interface structure
 */
void jw_wifi_set_server_interface(const jw_wifi_server_interface_t *interface);

#endif // JW_WIFI_H