#ifndef JW_RTC_H
#define JW_RTC_H

#include <time.h>
#include "esp_err.h"

/** @brief Global RTC time structure */
extern struct tm jw_rtc_time;

/** @brief Structure for RTC settings */
typedef struct {
    char timezone[32];  // Timezone string (e.g., "EST5EDT")
} jw_rtc_settings_t;

/** @brief Global RTC settings (timezone string) */
extern jw_rtc_settings_t jw_rtc_settings;

/**
 * @brief Initialize the RTC module
 * 
 * This function sets up the RTC with the configured timezone and initializes SNTP
 * for time synchronization if not already running.
 * 
 * @return ESP_OK on success, or an error code on failure
 */
esp_err_t jw_rtc_init(void);

/**
 * @brief Get current time in seconds
 * @return uint32_t Current time in seconds since epoch
 * 
 * Returns the current time as a Unix timestamp.
 */
uint32_t jw_rtc_get_time_sec(void);

#endif // JW_RTC_H
