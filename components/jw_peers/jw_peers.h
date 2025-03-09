#ifndef JW_PEERS_H
#define JW_PEERS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_now.h"

#define JW_PEERS_MAX_CAPACITY 10
#define JW_PEERS_BLACKLIST_MAX_SIZE 10

typedef enum {
    JW_PEER_TYPE_SENSOR = 0,
    JW_PEER_TYPE_RELAY,
    JW_PEER_TYPE_SWITCH,
    JW_PEER_TYPE_UNKNOWN
} jw_peer_type_t;

typedef enum {
    JW_SENSOR_SUBTYPE_TEMPERATURE = 0,
    JW_SENSOR_SUBTYPE_HUMIDITY,
    JW_SENSOR_SUBTYPE_LIGHT,
    JW_SENSOR_SUBTYPE_UNKNOWN
} jw_sensor_subtype_t;

typedef struct {
    uint32_t timestamp;
    float sensor_values[3];
    bool relay_state;
    bool switch_state;
} jw_peer_data_t;

typedef struct {
    uint8_t mac_address[ESP_NOW_ETH_ALEN];
    jw_peer_type_t peer_type;
    uint8_t sensor_count;
    jw_sensor_subtype_t sensor_types[3];
    char peer_name[16];
    uint32_t last_update;
    bool is_active;
    jw_peer_data_t latest_data;
    uint8_t data_interval_sec;
} jw_peer_entry_t;

typedef struct jw_peers_context jw_peers_context_t;

esp_err_t jw_peers_initialize(void);
esp_err_t jw_peers_add_peer(const uint8_t *mac_address, jw_peer_type_t peer_type, const char *peer_name,
    uint8_t sensor_count, const jw_sensor_subtype_t *sensor_types, uint8_t interval_sec);
esp_err_t jw_peers_get_peer(jw_peer_entry_t **peers, uint8_t *peer_count);
esp_err_t jw_peers_update_data(const uint8_t *mac_address, const jw_peer_data_t *data);
esp_err_t jw_peers_edit_name(const uint8_t *mac_address, const char *new_name);
esp_err_t jw_peers_edit_interval(const uint8_t *mac_address, uint8_t interval_sec);

// Blacklist management
esp_err_t jw_peers_add_to_blacklist(const uint8_t *mac_address);
esp_err_t jw_peers_remove_from_blacklist(const uint8_t *mac_address);
esp_err_t jw_peers_get_blacklist(uint8_t(*blacklist)[ESP_NOW_ETH_ALEN], uint8_t *blacklist_count);
bool jw_peers_is_blacklisted(const uint8_t *mac_address);

#endif // JW_PEERS_H