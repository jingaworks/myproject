#include "jw_rtc.h"
#include "esp_sntp.h"
#include "esp_log.h"

static const char *TAG = "JW_RTC";

struct tm jw_rtc_time;
jw_rtc_settings_t jw_rtc_settings = { .timezone = "EST5EDT" };

// Callback for time sync notification
static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Time synchronized");
    time_t now = 0;
    time(&now);
    localtime_r(&now, &jw_rtc_time);
}

esp_err_t jw_rtc_init(void) {
    ESP_LOGI(TAG, "Initializing RTC");

    // Set timezone
    setenv("TZ", jw_rtc_settings.timezone, 1);
    tzset();

    // Initialize SNTP only if not already running
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        sntp_set_time_sync_notification_cb(time_sync_notification_cb);  // Updated as requested
        esp_sntp_init();
    }

    // Initial time sync (will update later with SNTP)
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    jw_rtc_time = timeinfo;

    return ESP_OK;
}


/**
 * @brief Get current time in seconds
 * @return uint32_t Current time in seconds since epoch
 *
 * Retrieves the current Unix timestamp.
 */
uint32_t jw_rtc_get_time_sec(void) {
    time_t now;
    time(&now);
    return (uint32_t)now;
}
