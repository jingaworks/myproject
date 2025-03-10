
# Project Requirements for myproject

## Project Base (repository) - Mar 08, 2025

1. Public GitHub Repository
   - Host: GitHub (public, e.g., https://github.com/jingaworks/myproject).
   - Purpose: Central storage for all project files (e.g., .c, .h, configs).
   - Access: User provides full repo link in each request (e.g., "Analyze [https://github.com/jingaworks/myproject]").

2. Link Usage
   - Format: HTTPS URL to public repo (e.g., https://github.com/jingaworks/myproject).
   - Specificity: Optionally point to files (e.g., "Focus on /main/main.c") or let Grok explore the repo.
   - Updates: User pushes changes to repo and shares updated link (or same link if versioned) for next iteration.

3. Development Basics
   - Platform: User works on Windows 10, browser-based chat with Grok.
   - Expectation: Grok interprets linked code from the public repo, suggests updates without needing local setup.

## Overview
myproject is an ESP32-based firmware application developed using the ESP-IDF framework. It aims to provide a robust, modular system for IoT device management, including network communication, timekeeping, data storage, and peer-to-peer messaging. The project is structured with reusable components, each handling a specific functionality, integrated via a main application (main.c).

## General Requirements
- Platform: ESP32 (ESP-IDF v5.x).
- Build System: CMake-based, compatible with idf.py.
- Tasks: Each component runs as a FreeRTOS task for concurrent operation.
- Error Handling: All components must log errors and handle failures gracefully (e.g., retries, fallbacks).
- Logging: Centralized logging via jw_log for debugging and monitoring.
- Configuration: Configurable via NVS (Non-Volatile Storage) with defaults in main.c.
- Date: Requirements reflect the project state as of March 10, 2025.

## Component Requirements and Descriptions

1. jw_log
    Purpose: Provides a centralized logging system for all components.

    Requirements:
    - Initialize logging with a default level of ESP_LOG_INFO.
    - Support task-based operation to periodically log system status.
    - Offer a buffered jw_log_write function for custom messages.
    - Integrate with FreeRTOS for task management.

    Detailed Description:
    - Files: jw_log.h, jw_log.c, jw_log_core.h, jw_log_core.c, jw_log_write.h, jw_log_write.c.
    - Functionality:
      - jw_log_task: Runs a task that initializes logging and logs a status message every 30 seconds.
      - jw_log_core_init: Sets up the ESP logging system with a default level.
      - jw_log_write: Formats and writes messages to the log with a 256-byte buffer.
    - Usage: All components use ESP_LOG* macros (or optionally jw_log_write) for consistent logging.
    - Dependencies: freertos, esp_common.
    - Example: Logs "Log task running" every 30 seconds after initialization.

2. jw_rtc
    Purpose: Manages system time, synchronized with an external DS3231 RTC module.

    Requirements:
    - Initialize the RTC system and sync with DS3231 on startup.
    - Provide functions to set and get system time.
    - Run a task to log the current time periodically (every 60 seconds).
    - Fallback to a default time (2025-03-10 12:00:00) if DS3231 fails.

    Detailed Description:
    - Files: jw_rtc.h, jw_rtc.c, jw_rtc_core.h, jw_rtc_core.c, jw_rtc_time.h, jw_rtc_time.c.
    - Functionality:
      - jw_rtc_task: Initializes the RTC, sets system time from DS3231, and logs it every minute.
      - jw_rtc_core_init: Sets up the RTC by initializing the DS3231 dependency.
      - jw_rtc_time_set/jw_rtc_time_get: Interfaces with the system clock using settimeofday and gettimeofday.
    - Usage: Provides accurate time for logs and other time-sensitive operations.
    - Dependencies: jw_ds3231, freertos, esp_common.
    - Example: On boot, sets system time to DS3231’s value (e.g., "2025-03-10 12:00:00").

3. jw_sdcard
    Purpose: Handles data storage on an SD card via SPI.

    Requirements:
    - Mount the SD card with retry logic (up to 3 attempts).
    - Provide read/write operations for files on the SD card.
    - Run a task to monitor SD card status and test I/O (every 30 seconds).
    - Log all operations and errors.

    Detailed Description:
    - Files: jw_sdcard.h, jw_sdcard.c, jw_sdcard_core.h, jw_sdcard_core.c, jw_sdcard_io.h, jw_sdcard_io.c.
    - Functionality:
      - jw_sdcard_task: Mounts the SD card, performs a test write/read, and logs status periodically.
      - jw_sdcard_core_init: Mounts the SD card using esp_vfs_fat_sdmmc_mount with retries.
      - jw_sdcard_io_write/jw_sdcard_io_read: Writes and reads files using standard C file operations.
    - Usage: Stores logs, configuration, or sensor data persistently.
    - Dependencies: esp_vfs_fat, sdmmc, freertos.
    - Example: Writes "SD Card Test Data" to /sdcard/test.txt and reads it back on startup.

4. jw_ds3231
    Purpose: Interfaces with the DS3231 RTC module over I2C for accurate timekeeping.

    Requirements:
    - Initialize I2C communication with the DS3231 (address 0x68, GPIO 21 SDA, GPIO 22 SCL).
    - Provide functions to set and get time from the DS3231 in struct tm format.
    - Run an optional task to log DS3231 time (every 60 seconds).
    - Support BCD conversion for DS3231 registers.

    Detailed Description:
    - Files: jw_ds3231.h, jw_ds3231.c, jw_ds3231_core.h, jw_ds3231_core.c, jw_ds3231_time.h, jw_ds3231_time.c.
    - Functionality:
      - jw_ds3231_task: Initializes the DS3231 and logs its time periodically (optional).
      - jw_ds3231_core_init: Configures I2C master mode for communication.
      - jw_ds3231_time_set/jw_ds3231_time_get: Reads/writes DS3231 registers, converting between BCD and decimal.
    - Usage: Primary time source for jw_rtc.
    - Dependencies: driver (I2C), freertos.
    - Example: Sets DS3231 time to "2025-03-10 12:00:00" and retrieves it.

5. jw_server
    Purpose: Runs an HTTP and WebSocket server for remote monitoring and control.

    Requirements:
    - Start an HTTP server with a /status endpoint and a /ws WebSocket endpoint.
    - Provide WiFi and peer status via /status.
    - Support task-based operation with a 30-second status check.
    - Allow URI registration for extensibility.

    Detailed Description:
    - Files: jw_server.h, jw_server.c, jw_server_core.h, jw_server_core.c, jw_server_http.h, jw_server_http.c, jw_server_ws.h, jw_server_ws.c, jw_server_utils.h, jw_server_utils.c.
    - Functionality:
      - jw_server_task: Starts the server and runs indefinitely.
      - jw_server_core_start: Initializes HTTP and WebSocket servers.
      - jw_server_http_init: Sets up HTTP with a status handler.
      - jw_server_ws_init: Configures WebSocket echo functionality.
      - jw_server_utils_format_status: Formats JSON status with WiFi and peer data.
    - Usage: Remote access to device status and real-time updates.
    - Dependencies: esp_http_server, jw_wifi, jw_peers, freertos.
    - Example: Returns {"wifi": 2, "peers": 1} on /status.

6. jw_wifi
    Purpose: Manages WiFi connectivity in STA mode.

    Requirements:
    - Connect to a configured SSID/password with reconnection logic.
    - Provide a state machine (DISCONNECTED, CONNECTING, CONNECTED).
    - Run a task to monitor and reconnect WiFi (every 10 seconds).
    - Log connection status.

    Detailed Description:
    - Files: jw_wifi.h, jw_wifi.c, jw_wifi_core.h, jw_wifi_core.c, jw_wifi_connect.h, jw_wifi_connect.c.
    - Functionality:
      - jw_wifi_task: Initializes WiFi and attempts connection, retrying if disconnected.
      - jw_wifi_core_init: Sets up WiFi STA mode and event handlers.
      - jw_wifi_connect: Configures and connects to the AP.
      - jw_wifi_get_state: Returns current WiFi state.
    - Usage: Enables network features for jw_server and jw_espnow.
    - Dependencies: esp_wifi, esp_event, esp_netif, freertos.
    - Example: Connects to "default_ssid" and logs "WiFi connected".

7. jw_peers
    Purpose: Manages a list of peer devices.

    Requirements:
    - Maintain a linked list of peers (MAC and name) with a configurable maximum.
    - Provide add, remove, and count operations with mutex protection.
    - Run a task to log peer count (every 60 seconds).
    - Support peer data structure from jw_common.

    Detailed Description:
    - Files: jw_peers.h, jw_peers.c, jw_peers_list.h, jw_peers_list.c, jw_peers_utils.h, jw_peers_utils.c.
    - Functionality:
      - jw_peers_task: Initializes the list and adds a test peer, logging count periodically.
      - jw_peers_list_init/add/remove/count: Manages the peer list.
      - jw_peers_utils_print_peer: Logs peer details.
    - Usage: Tracks devices for jw_server and potential jw_espnow integration.
    - Dependencies: jw_common, freertos.
    - Example: Adds a peer "TestPeer" with MAC AA:BB:CC:DD:EE:FF.

8. jw_espnow
    Purpose: Enables peer-to-peer communication via ESP-NOW.

    Requirements:
    - Initialize ESP-NOW and register a receive callback.
    - Support sending data to specific or broadcast MAC addresses.
    - Run a task to send periodic test messages (every 10 seconds).
    - Depend on WiFi STA mode being active.

    Detailed Description:
    - Files: jw_espnow.h, jw_espnow.c, jw_espnow_core.h, jw_espnow_core.c, jw_espnow_send.h, jw_espnow_send.c, jw_espnow_recv.h, jw_espnow_recv.c.
    - Functionality:
      - jw_espnow_task: Initializes ESP-NOW, sends "Hello ESP-NOW" to broadcast, and logs received data.
      - jw_espnow_core_init: Sets up ESP-NOW with WiFi STA.
      - jw_espnow_send_data: Sends data to a MAC address.
      - jw_espnow_recv_register_cb: Registers a callback for incoming data.
    - Usage: Wireless communication between ESP32 devices.
    - Dependencies: esp_now, jw_wifi, freertos.
    - Example: Broadcasts "Hello ESP-NOW" every 10 seconds.

9. jw_common
    Purpose: Provides shared data structures and constants.

    Requirements:
    - Define jw_device_info_t for peer data (MAC and name).
    - Set constants like JW_MAC_ADDR_LEN (6) and JW_MAX_NAME_LEN (32).
    - Be dependency-free for broad use.

    Detailed Description:
    - Files: jw_common.h.
    - Functionality:
      - Defines jw_device_info_t with a 6-byte MAC and 32-char name.
      - Supplies constants for consistent use across components.
    - Usage: Used by jw_peers for peer management.
    - Dependencies: None.
    - Example: jw_device_info_t peer = {.mac = {0xAA, ...}, .name = "TestPeer"}.

10. main
  Purpose: Orchestrates all components and monitors system state.

  Requirements:
  - Initialize NVS and load configuration (SSID, password, max peers, SD card enable).
  - Create tasks for all components with appropriate stack sizes and priorities.
  - Monitor WiFi and peer status, restarting if both fail.
  - Log system state every 5 seconds.

  Detailed Description:
  - Files: main.c.
  - Functionality:
    - app_main: Sets up NVS, loads config, creates tasks, and runs a monitoring loop.
    - Config struct: jw_config_t with WiFi and peer settings.
    - State machine: INIT, RUNNING, FAILED.
  - Usage: Entry point for the firmware.
  - Dependencies: All components, nvs_flash, freertos.
  - Example: Logs "System state: RUNNING, WiFi: 2, Peers: 1".

## Additional Requirements
- Hardware:
  - ESP32 module (e.g., ESP32-WROOM-32).
  - DS3231 RTC module (I2C, 3.3V).
  - SD card module (SPI, 3.3V).
- Power: 3.3V supply, with battery backup for DS3231.
- Testing: Unit tests for each component’s core functions.
- Documentation: Maintain docs/project_workflow.txt and this file.   



Notes:
- Expand this file later with project-specific needs.
- Minimal setup: Git installed, public repo created, link shared with Grok.
