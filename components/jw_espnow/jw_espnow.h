#ifndef JW_ESPNOW_H
#define JW_ESPNOW_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_now.h"
#include "jw_peers.h"

// Peering timeout in milliseconds
#define JW_ESPNOW_PEERING_TIMEOUT_MS 5000

// ESP-NOW message types
typedef enum {
    JW_ESPNOW_MSG_TYPE_PEER_REQUEST = 0,
    JW_ESPNOW_MSG_TYPE_PEER_ACCEPT,
    JW_ESPNOW_MSG_TYPE_PEER_ACCEPT_CONFIRM,
    JW_ESPNOW_MSG_TYPE_PEER_CONFIRMED,
    JW_ESPNOW_MSG_TYPE_CHANNEL_CHANGE,
    JW_ESPNOW_MSG_TYPE_DATA
} jw_espnow_msg_type_t;

// ESP-NOW message structure
typedef struct {
    uint8_t version;                  // Message version (currently 1)
    uint8_t destination_mac[ESP_NOW_ETH_ALEN]; // Destination MAC address
    uint8_t source_mac[ESP_NOW_ETH_ALEN];      // Source MAC address
    jw_espnow_msg_type_t msg_type;    // Message type
    union {
        uint8_t channel;              // For CHANNEL_CHANGE
        jw_peer_data_t data;          // For DATA messages
        struct {
            char peer_name[16];       // Peer name
            jw_peer_type_t peer_type; // Peer type (sensor, relay, switch)
            jw_sensor_subtype_t sensor_subtype; // Sensor subtype (if sensor)
        } peering;                    // For PEER_ACCEPT and PEER_CONFIRMED
    } payload;
} jw_espnow_message_t;

// Opaque context for jw_espnow module
typedef struct jw_espnow_context jw_espnow_context_t;

// Initialize the jw_espnow module
esp_err_t jw_espnow_initialize(void);

// Start the peering process (broadcasts PEER_REQUEST)
esp_err_t jw_espnow_start_peering(void);

// Accept a Peer after user selection (adds to ESP-NOW peer list)
esp_err_t jw_espnow_accept_peer(const uint8_t *mac_address);

// Send CHANNEL_CHANGE message to all Peers
esp_err_t jw_espnow_send_channel_change(uint8_t new_channel);

#endif // JW_ESPNOW_H