# myproject

## Overview
myproject is an ESP32-based firmware application developed using the ESP-IDF framework. It provides a robust, modular system for IoT device management, featuring network communication, timekeeping, data storage, and peer-to-peer messaging. The project is structured with reusable components, each handling a specific functionality, integrated via a main application (main.c).

## General Requirements
- Platform: ESP32 (ESP-IDF v5.x)
- Build System: CMake-based, compatible with idf.py
- Tasks: Each component runs as a FreeRTOS task for concurrent operation
- Error Handling: All components must log errors and handle failures gracefully (e.g., retries, fallbacks)
- Logging: Centralized logging via jw_log for debugging and monitoring
- Configuration: Configurable via NVS (Non-Volatile Storage) with defaults in main.c
- Date: Requirements reflect the project state as of March 10, 2025

## Component Requirements and Descriptions

### 1. jw_log
Purpose: Provides a centralized logging system for all components.

Requirements:
- Initialize logging with a default level of ESP_LOG_INFO
- Support task-based operation to periodically log system status
- Offer a buffered jw_log_write function for custom messages
- Integrate with FreeRTOS for task management

Detailed Description:
- Files: jw_log.h, jw_log.c, jw_log_core.h, jw_log_core.c, jw_log_write.h, jw_log_write.c
- Functionality:
  - jw_log_task: Runs a task that initializes logging and logs a status message every 30 seconds
  - jw_log_core_init: Sets up the ESP logging system with a default level
  - jw_log_write: Formats and writes messages to the log with a 256-byte buffer
- Usage: All components use ESP_LOG* macros (or optionally jw_log_write) for consistent logging
- Dependencies: freertos, esp_common
- Example: Logs "Log task running" every 30 seconds after initialization
```c
    void jw_log_task(void *pvParameters) {
        esp_err_t ret = jw_log_core_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Log initialization failed: %d", ret);
            vTaskDelete(NULL);
        }
        jw_log_write(TAG, "Log system started");
        while (1) {
            jw_log_write(TAG, "Log task running");
            vTaskDelay(30000 / portTICK_PERIOD_MS);
        }
    }
```
### 2. jw_rtc
Purpose: Manages system time, synchronized with an external DS3231 RTC module.

Requirements:
- Initialize the RTC system and sync with DS3231 on startup
- Provide functions to set and get system time
- Run a task to log the current time periodically (every 60 seconds)
- Fallback to a default time (2025-03-10 12:00:00) if DS3231 fails

Detailed Description:
- Files: jw_rtc.h, jw_rtc.c, jw_rtc_core.h, jw_rtc_core.c, jw_rtc_time.h, jw_rtc_time.c
- Functionality:
  - jw_rtc_task: Initializes the RTC, sets system time from DS3231, and logs it every minute
  - jw_rtc_core_init: Sets up the RTC by initializing the DS3231 dependency
  - jw_rtc_time_set/jw_rtc_time_get: Interfaces with the system clock using settimeofday and gettimeofday
- Usage: Provides accurate time for logs and other time-sensitive operations
- Dependencies: jw_ds3231, freertos, esp_common
- Example: On boot, sets system time to DS3231’s value (e.g., "2025-03-10 12:00:00")
```c
    void jw_rtc_task(void *pvParameters) {
        esp_err_t ret = jw_rtc_core_init();
        if (ret != ESP_OK) vTaskDelete(NULL);
        struct tm ds3231_time;
        ret = jw_ds3231_time_get(&ds3231_time);
        if (ret == ESP_OK) jw_rtc_time_set(&ds3231_time);
        while (1) {
            struct tm current_time;
            jw_rtc_time_get(&current_time);
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &current_time);
            ESP_LOGI(TAG, "Current system time: %s", time_str);
            vTaskDelay(60000 / portTICK_PERIOD_MS);
        }
    }
```
### 3. jw_sdcard
Purpose: Handles data storage on an SD card via SPI.

Requirements:
- Mount the SD card with retry logic (up to 3 attempts)
- Provide read/write operations for files on the SD card
- Run a task to monitor SD card status and test I/O (every 30 seconds)
- Log all operations and errors

Detailed Description:
- Files: jw_sdcard.h, jw_sdcard.c, jw_sdcard_core.h, jw_sdcard_core.c, jw_sdcard_io.h, jw_sdcard_io.c
- Functionality:
  - jw_sdcard_task: Mounts the SD card, performs a test write/read, and logs status periodically
  - jw_sdcard_core_init: Mounts the SD card using esp_vfs_fat_sdmmc_mount with retries
  - jw_sdcard_io_write/jw_sdcard_io_read: Writes and reads files using standard C file operations
- Usage: Stores logs, configuration, or sensor data persistently
- Dependencies: esp_vfs_fat, sdmmc, freertos
- Example: Writes "SD Card Test Data" to /sdcard/test.txt and reads it back on startup
```c
    void jw_sdcard_task(void *pvParameters) {
        esp_err_t ret = jw_sdcard_core_init();
        if (ret != ESP_OK) vTaskDelete(NULL);
        const char *test_data = "SD Card Test Data";
        ret = jw_sdcard_io_write("/sdcard/test.txt", (const uint8_t *)test_data, strlen(test_data));
        if (ret == ESP_OK) {
            uint8_t read_buffer[32] = {0};
            jw_sdcard_io_read("/sdcard/test.txt", read_buffer, strlen(test_data));
            ESP_LOGI(TAG, "Read from SD: %s", read_buffer);
        }
        while (1) {
            ESP_LOGI(TAG, "SD card task running");
            vTaskDelay(30000 / portTICK_PERIOD_MS);
        }
    }
```
### 4. jw_ds3231
Purpose: Interfaces with the DS3231 RTC module over I2C for accurate timekeeping.

Requirements:
- Initialize I2C communication with the DS3231 (address 0x68, GPIO 21 SDA, GPIO 22 SCL)
- Provide functions to set and get time from the DS3231 in struct tm format
- Run an optional task to log DS3231 time (every 60 seconds)
- Support BCD conversion for DS3231 registers

Detailed Description:
- Files: jw_ds3231.h, jw_ds3231.c, jw_ds3231_core.h, jw_ds3231_core.c, jw_ds3231_time.h, jw_ds3231_time.c
- Functionality:
  - jw_ds3231_task: Initializes the DS3231 and logs its time periodically (optional)
  - jw_ds3231_core_init: Configures I2C master mode for communication
  - jw_ds3231_time_set/jw_ds3231_time_get: Reads/writes DS3231 registers, converting between BCD and decimal
- Usage: Primary time source for jw_rtc
- Dependencies: driver (I2C), freertos
- Example: Sets DS3231 time to "2025-03-10 12:00:00" and retrieves it
```c
    void jw_ds3231_task(void *pvParameters) {
        esp_err_t ret = jw_ds3231_core_init();
        if (ret != ESP_OK) vTaskDelete(NULL);
        struct tm test_time = {.tm_year = 125, .tm_mon = 2, .tm_mday = 10, .tm_hour = 12, .tm_min = 0, .tm_sec = 0};
        jw_ds3231_time_set(&test_time);
        while (1) {
            struct tm current_time;
            jw_ds3231_time_get(&current_time);
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &current_time);
            ESP_LOGI(TAG, "DS3231 time: %s", time_str);
            vTaskDelay(60000 / portTICK_PERIOD_MS);
        }
    }
```
### 5. jw_server
Purpose: Runs an HTTP and WebSocket server for remote monitoring and control.

Requirements:
- Start an HTTP server with a /status endpoint and a /ws WebSocket endpoint
- Provide WiFi and peer status via /status
- Support task-based operation with a 30-second status check
- Allow URI registration for extensibility

Detailed Description:
- Files: jw_server.h, jw_server.c, jw_server_core.h, jw_server_core.c, jw_server_http.h, jw_server_http.c, jw_server_ws.h, jw_server_ws.c, jw_server_utils.h, jw_server_utils.c
- Functionality:
  - jw_server_task: Starts the server and runs indefinitely
  - jw_server_core_start: Initializes HTTP and WebSocket servers
  - jw_server_http_init: Sets up HTTP with a status handler
  - jw_server_ws_init: Configures WebSocket echo functionality
  - jw_server_utils_format_status: Formats JSON status with WiFi and peer data
- Usage: Remote access to device status and real-time updates
- Dependencies: esp_http_server, jw_wifi, jw_peers, freertos
- Example: Returns {"wifi": 2, "peers": 1} on /status
```c
    void jw_server_task(void *pvParameters) {
        jw_config_t *config = (jw_config_t *)pvParameters;
        esp_err_t ret = jw_server_core_start(config);
        if (ret != ESP_OK) vTaskDelete(NULL);
        while (1) {
            ESP_LOGI(TAG, "Server running");
            vTaskDelay(30000 / portTICK_PERIOD_MS);
        }
    }
```
### 6. jw_wifi
Purpose: Manages WiFi connectivity in STA mode.

Requirements:
- Connect to a configured SSID/password with reconnection logic
- Provide a state machine (DISCONNECTED, CONNECTING, CONNECTED)
- Run a task to monitor and reconnect WiFi (every 10 seconds)
- Log connection status

Detailed Description:
- Files: jw_wifi.h, jw_wifi.c, jw_wifi_core.h, jw_wifi_core.c, jw_wifi_connect.h, jw_wifi_connect.c
- Functionality:
  - jw_wifi_task: Initializes WiFi and attempts connection, retrying if disconnected
  - jw_wifi_core_init: Sets up WiFi STA mode and event handlers
  - jw_wifi_connect: Configures and connects to the AP
  - jw_wifi_get_state: Returns current WiFi state
- Usage: Enables network features for jw_server and jw_espnow
- Dependencies: esp_wifi, esp_event, esp_netif, freertos
- Example: Connects to "default_ssid" and logs "WiFi connected"
```c
    void jw_wifi_task(void *pvParameters) {
        jw_config_t *config = (jw_config_t *)pvParameters;
        esp_err_t ret = jw_wifi_core_init();
        if (ret != ESP_OK) vTaskDelete(NULL);
        while (1) {
            if (jw_wifi_get_state() != WIFI_CONNECTED) {
                jw_wifi_connect(config->wifi_ssid, config->wifi_password);
            }
            vTaskDelay(10000 / portTICK_PERIOD_MS);
        }
    }
```
### 7. jw_peers
Purpose: Manages a list of peer devices.

Requirements:
- Maintain a linked list of peers (MAC and name) with a configurable maximum
- Provide add, remove, and count operations with mutex protection
- Run a task to log peer count (every 60 seconds)
- Support peer data structure from jw_common

Detailed Description:
- Files: jw_peers.h, jw_peers.c, jw_peers_list.h, jw_peers_list.c, jw_peers_utils.h, jw_peers_utils.c
- Functionality:
  - jw_peers_task: Initializes the list and adds a test peer, logging count periodically
  - jw_peers_list_init/add/remove/count: Manages the peer list
  - jw_peers_utils_print_peer: Logs peer details
- Usage: Tracks devices for jw_server and potential jw_espnow integration
- Dependencies: jw_common, freertos
- Example: Adds a peer "TestPeer" with MAC AA:BB:CC:DD:EE:FF
```c
    void jw_peers_task(void *pvParameters) {
        jw_config_t *config = (jw_config_t *)pvParameters;
        esp_err_t ret = jw_peers_list_init(config->max_peers);
        if (ret != ESP_OK) vTaskDelete(NULL);
        uint8_t test_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        jw_peers_list_add(test_mac, "TestPeer");
        while (1) {
            ESP_LOGI(TAG, "Peer count: %d", jw_peers_list_count());
            vTaskDelay(60000 / portTICK_PERIOD_MS);
        }
    }
```
### 8. jw_espnow
Purpose: Enables peer-to-peer communication via ESP-NOW.

Requirements:
- Initialize ESP-NOW and register a receive callback
- Support sending data to specific or broadcast MAC addresses
- Run a task to send periodic test messages (every 10 seconds)
- Depend on WiFi STA mode being active

Detailed Description:
- Files: jw_espnow.h, jw_espnow.c, jw_espnow_core.h, jw_espnow_core.c, jw_espnow_send.h, jw_espnow_send.c, jw_espnow_recv.h, jw_espnow_recv.c
- Functionality:
  - jw_espnow_task: Initializes ESP-NOW, sends "Hello ESP-NOW" to broadcast, and logs received data
  - jw_espnow_core_init: Sets up ESP-NOW with WiFi STA
  - jw_espnow_send_data: Sends data to a MAC address
  - jw_espnow_recv_register_cb: Registers a callback for incoming data
- Usage: Wireless communication between ESP32 devices
- Dependencies: esp_now, jw_wifi, freertos
- Example: Broadcasts "Hello ESP-NOW" every 10 seconds
```c
    void jw_espnow_task(void *pvParameters) {
        esp_err_t ret = jw_espnow_core_init();
        if (ret != ESP_OK) vTaskDelete(NULL);
        const char *msg = "Hello ESP-NOW";
        uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        while (1) {
            jw_espnow_send_data(broadcast_mac, (const uint8_t *)msg, strlen(msg));
            vTaskDelay(10000 / portTICK_PERIOD_MS);
        }
    }
```
### 9. jw_common
Purpose: Provides shared data structures and constants.

Requirements:
- Define jw_device_info_t for peer data (MAC and name)
- Set constants like JW_MAC_ADDR_LEN (6) and JW_MAX_NAME_LEN (32)
- Be dependency-free for broad use

Detailed Description:
- Files: jw_common.h
- Functionality:
  - Defines jw_device_info_t with a 6-byte MAC and 32-char name
  - Supplies constants for consistent use across components
- Usage: Used by jw_peers for peer management
- Dependencies: None
- Example: jw_device_info_t peer = {.mac = {0xAA, ...}, .name = "TestPeer"}
```c
    typedef struct {
        uint8_t mac[JW_MAC_ADDR_LEN];
        char name[JW_MAX_NAME_LEN];
    } jw_device_info_t;

    #define JW_MAC_ADDR_LEN 6
    #define JW_MAX_NAME_LEN 32
```
### 10. main
Purpose: Orchestrates all components and monitors system state.

Requirements:
- Initialize NVS and load configuration (SSID, password, max peers, SD card enable)
- Create tasks for all components with appropriate stack sizes and priorities
- Monitor WiFi and peer status, restarting if both fail
- Log system state every 5 seconds

Detailed Description:
- Files: main.c
- Functionality:
  - app_main: Sets up NVS, loads config, creates tasks, and runs a monitoring loop
  - Config struct: jw_config_t with WiFi and peer settings
  - State machine: INIT, RUNNING, FAILED
- Usage: Entry point for the firmware
- Dependencies: All components, nvs_flash, freertos
- Example: Logs "System state: RUNNING, WiFi: 2, Peers: 1"
```c
    void app_main(void) {
        nvs_flash_init();
        load_config(&config);
        xTaskCreate(jw_log_task, "log_task", 4096, NULL, 6, NULL);
        xTaskCreate(jw_rtc_task, "rtc_task", 2048, NULL, 4, NULL);
        xTaskCreate(jw_sdcard_task, "sdcard_task", 4096, NULL, 3, NULL);
        while (1) {
            if (jw_wifi_get_state() == WIFI_DISCONNECTED && jw_peers_list_count() == 0) {
                esp_restart();
            }
            vTaskDelay(5000 / portTICK_PERIOD_MS);
        }
    }
```
## Additional Requirements
- Hardware:
  - ESP32 module (e.g., ESP32-WROOM-32)
  - DS3231 RTC module (I2C, 3.3V)
  - SD card module (SPI, 3.3V)
- Power: 3.3V supply, with battery backup for DS3231
- Testing: Unit tests for each component’s core functions
- Documentation: Maintain docs/project_workflow.txt and this README

## Getting Started
1. Clone the repository:
    git clone https://github.com/jingaworks/myproject.git
    cd myproject
2. Set up ESP-IDF:
    idf.py set-target esp32
3. Build and flash:
    idf.py build
    idf.py -p /dev/ttyUSB0 flash monitor
