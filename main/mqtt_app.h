#ifndef MQTT_APP_H
#define MQTT_APP_H

#include <stdint.h>  // สำหรับ uint8_t
#include <stddef.h>  // สำหรับ size_t
#include <stdbool.h> // สำหรับ bool
#include "mqtt_client.h"

// External variables
extern esp_mqtt_client_handle_t mqtt_client;
extern volatile bool g_mqtt_connected;

// MQTT topic variables
extern char mqtt_topic_live[64];
extern char mqtt_topic_info_device_id[64];
extern char mqtt_topic_ota_update[64];
extern char mqtt_topic_settings_update[64];
extern char mqtt_topic_settings_state_device_id[64];

// Functions
void mqtt_app_start(void);
void mqtt_send_image(const uint8_t *image_data, size_t image_len);
char* get_live_json_payload_str(void);
void init_sntp_sync_time(void);
bool is_time_synced(void);

#endif