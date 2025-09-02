#include "mqtt_app.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>

// เพิ่ม extern สำหรับ certificate
extern const uint8_t server1_crt_start[] asm("_binary_dev_crt_start");
extern const uint8_t server1_crt_end[]   asm("_binary_dev_crt_end");

static const char *MQTT_TAG = "mqtt_client";
esp_mqtt_client_handle_t mqtt_client = NULL;
volatile bool g_mqtt_connected = false; // MQTT connection state flag
static volatile bool time_synced = false;

// Device information
#define DEVICE_ID "Camera001"
#define DEVICE_TYPE "Camera1"

// ---------------------- MQTT Configurations ----------------------
#define MQTT_BROKER_URI      "mqtts://dev-connect.datatamer.ai:8884"
#define MQTT_USERNAME        "client"
#define MQTT_PASSWORD        "Apollo1999!"
// send reading live data
char mqtt_topic_live[64];
// request reading product info
char mqtt_topic_info_device_id[64];
// send reading product info
#define MQTT_TOPIC_INFO      "R60AFD1/info"

// OTA update topic
char mqtt_topic_ota_update[64];

// update settings
char mqtt_topic_settings_update[64];
// request reading settings
char mqtt_topic_settings_state_device_id[64];
// send reading settings
#define MQTT_TOPIC_SETTINGS_STATE "camera1/settings_state"

// NTP servers
#define NTP_SERVER1 "pool.ntp.org"
#define NTP_SERVER2 "time.nist.gov"
#define NTP_SERVER3 "time.google.com"

// SNTP callback function
static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(MQTT_TAG, "Time synchronized with NTP server");
    time_synced = true;
    char strftime_buf[64];
    struct tm timeinfo;
    localtime_r(&tv->tv_sec, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(MQTT_TAG, "Current time: %s", strftime_buf);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    // ป้องกัน warning ตัวแปรไม่ได้ใช้
    (void)handler_args;
    (void)base;
    
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "Connected to MQTT broker");
            g_mqtt_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "Disconnected from MQTT broker");
            g_mqtt_connected = false;
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(MQTT_TAG, "MQTT message published, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(MQTT_TAG, "MQTT_EVENT_ERROR");
            g_mqtt_connected = false;
            esp_mqtt_error_codes_t *error_handle = (esp_mqtt_error_codes_t*)event_data;
            if (error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(MQTT_TAG, "Last error code reported from tcp transport: 0x%x", error_handle->connect_return_code);
            } else if (error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGE(MQTT_TAG, "Connection refused error: 0x%x", error_handle->connect_return_code);
            } else {
                ESP_LOGE(MQTT_TAG, "Unknown MQTT error type: %d", error_handle->error_type);
            }
            // Try to reconnect after error
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            esp_mqtt_client_reconnect(event->client);
            break;
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGI(MQTT_TAG, "MQTT connecting...");
            break;
        default:
            ESP_LOGD(MQTT_TAG, "Other MQTT event id: %" PRId32, event_id);
            break;
    }
}

void init_sntp_sync_time(void) {
    ESP_LOGI(MQTT_TAG, "Initializing and starting SNTP");
    
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER1);
    esp_sntp_setservername(1, NTP_SERVER2);
    esp_sntp_setservername(2, NTP_SERVER3);
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
    
    // Set timezone to GMT+7 (Thailand)
    setenv("TZ", "ICT-7", 1);
    tzset();
    
    // Wait for time sync
    int retry = 0;
    const int retry_count = 15; // รอสูงสุด 30 วินาที
    while (!time_synced && ++retry < retry_count) {
        ESP_LOGI(MQTT_TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    
    if (time_synced) {
        ESP_LOGI(MQTT_TAG, "Time sync completed successfully");
    } else {
        ESP_LOGW(MQTT_TAG, "Time sync timeout, continuing anyway...");
    }
}

bool is_time_synced(void) {
    return time_synced;
}

void mqtt_app_start(void) {
    // Sync time first
    init_sntp_sync_time();
    
    // Initialize MQTT topics
    snprintf(mqtt_topic_live, sizeof(mqtt_topic_live), "camera/live");
    snprintf(mqtt_topic_info_device_id, sizeof(mqtt_topic_info_device_id), "R60AFD1/info_device_id");
    snprintf(mqtt_topic_ota_update, sizeof(mqtt_topic_ota_update), "R60AFD1/ota_update");
    snprintf(mqtt_topic_settings_update, sizeof(mqtt_topic_settings_update), "R60AFD1/settings_update");
    snprintf(mqtt_topic_settings_state_device_id, sizeof(mqtt_topic_settings_state_device_id), "R60AFD1/settings_state_device_id");
    
    ESP_LOGI(MQTT_TAG, "MQTT topics initialized: live=%s", mqtt_topic_live);
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = MQTT_BROKER_URI,
            },
            .verification = {
                .certificate = (const char *)server1_crt_start,
                .skip_cert_common_name_check = false,
                .crt_bundle_attach = NULL,
            },
        },
        .credentials = {
            .username = MQTT_USERNAME,
            .authentication = {
                .password = MQTT_PASSWORD,
            },
        },
        .session = {
            .keepalive = 60,
            .disable_clean_session = false,
        },
        .network = {
            .timeout_ms = 30000,
            .refresh_connection_after_ms = 0,  // ปิด auto refresh
            .reconnect_timeout_ms = 5000,
            .disable_auto_reconnect = false,
        },
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(MQTT_TAG, "Failed to initialize MQTT client");
        return;
    }
    
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(MQTT_TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(MQTT_TAG, "MQTT client started successfully");
    }
}

void mqtt_send_image(const uint8_t *image_data, size_t image_len) {
    if (!mqtt_client || !g_mqtt_connected) {
        ESP_LOGW(MQTT_TAG, "MQTT not connected, attempting reconnect...");
        esp_mqtt_client_reconnect(mqtt_client);
        vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait 2 seconds for reconnection
        
        if (!g_mqtt_connected) {
            ESP_LOGW(MQTT_TAG, "Still not connected, skipping image publish");
            return;
        }
    }
    
    if (!image_data || image_len == 0) {
        ESP_LOGE(MQTT_TAG, "Invalid image data");
        return;
    }
    
    if (strlen(mqtt_topic_live) == 0) {
        ESP_LOGE(MQTT_TAG, "MQTT topic not set");
        return;
    }
    
    // Limit image size for MQTT (max 512KB for base64)
    if (image_len > 512 * 1024) {
        ESP_LOGE(MQTT_TAG, "Image too large: %zu bytes", image_len);
        return;
    }
    
    // Get current timestamp
    time_t now;
    time(&now);
    
    // Calculate base64 encoded length
    size_t base64_len = 0;
    int ret = mbedtls_base64_encode(NULL, 0, &base64_len, image_data, image_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        ESP_LOGE(MQTT_TAG, "Failed to calculate base64 length");
        return;
    }
    
    // Allocate buffer for base64 encoded data
    unsigned char *base64_buffer = malloc(base64_len + 1);
    if (!base64_buffer) {
        ESP_LOGE(MQTT_TAG, "Failed to allocate memory for base64 encoding");
        return;
    }
    
    // Encode image to base64
    size_t actual_len = 0;
    ret = mbedtls_base64_encode(base64_buffer, base64_len, &actual_len, image_data, image_len);
    if (ret != 0) {
        ESP_LOGE(MQTT_TAG, "Failed to encode image to base64");
        free(base64_buffer);
        return;
    }
    base64_buffer[actual_len] = '\0';
    
    // Create JSON payload
    cJSON *json_payload = cJSON_CreateObject();
    if (!json_payload) {
        ESP_LOGE(MQTT_TAG, "Failed to create JSON object");
        free(base64_buffer);
        return;
    }
    
    // Add device information
    cJSON_AddStringToObject(json_payload, "device_id", DEVICE_ID);
    cJSON_AddStringToObject(json_payload, "device_type", DEVICE_TYPE);
    cJSON_AddNumberToObject(json_payload, "timestamp", (double)now);
    cJSON_AddNumberToObject(json_payload, "image_size", (double)image_len);
    cJSON_AddStringToObject(json_payload, "format", "jpeg");
    cJSON_AddBoolToObject(json_payload, "has_image_data", true);
    
    // Add base64 encoded image as content
    cJSON_AddStringToObject(json_payload, "content", (const char *)base64_buffer);
    
    char *json_string = cJSON_Print(json_payload);
    cJSON_Delete(json_payload);
    
    if (!json_string) {
        ESP_LOGE(MQTT_TAG, "Failed to create JSON string");
        free(base64_buffer);
        return;
    }
    
    ESP_LOGI(MQTT_TAG, "Publishing image with base64 content: %zu bytes to topic: %s", image_len, mqtt_topic_live);
    ESP_LOGI(MQTT_TAG, "JSON payload size: %d bytes", strlen(json_string));
    ESP_LOGI(MQTT_TAG, "Base64 encoded size: %zu bytes", actual_len);
    ESP_LOGI(MQTT_TAG, "Base64 preview (first 100 chars): %.100s", (const char *)base64_buffer);
    ESP_LOGI(MQTT_TAG, "JSON content: %s", json_string);
    
    // Check if JSON is too large for MQTT
    size_t json_size = strlen(json_string);
    if (json_size > 256 * 1024) {  // 256KB limit
        ESP_LOGW(MQTT_TAG, "JSON payload too large: %zu bytes, splitting...", json_size);
        
        // Send metadata first
        cJSON *meta_payload = cJSON_CreateObject();
        cJSON_AddStringToObject(meta_payload, "device_id", DEVICE_ID);
        cJSON_AddStringToObject(meta_payload, "device_type", DEVICE_TYPE);
        cJSON_AddNumberToObject(meta_payload, "timestamp", (double)now);
        cJSON_AddNumberToObject(meta_payload, "image_size", (double)image_len);
        cJSON_AddStringToObject(meta_payload, "format", "jpeg");
        cJSON_AddBoolToObject(meta_payload, "has_image_data", true);
        cJSON_AddStringToObject(meta_payload, "status", "metadata_only");
        
        char *meta_string = cJSON_Print(meta_payload);
        cJSON_Delete(meta_payload);
        
        int meta_msg_id = esp_mqtt_client_publish(mqtt_client, mqtt_topic_live, meta_string, strlen(meta_string), 0, 0);
        free(meta_string);
        
        if (meta_msg_id >= 0) {
            ESP_LOGI(MQTT_TAG, "Metadata published successfully with msg_id=%d", meta_msg_id);
            
            // Send base64 data to separate topic
            char data_topic[128];
            snprintf(data_topic, sizeof(data_topic), "camera/live/data");
            int data_msg_id = esp_mqtt_client_publish(mqtt_client, data_topic, (const char *)base64_buffer, actual_len, 0, 0);
            
            if (data_msg_id >= 0) {
                ESP_LOGI(MQTT_TAG, "Base64 data published successfully with msg_id=%d", data_msg_id);
            } else {
                ESP_LOGE(MQTT_TAG, "Failed to publish base64 data, error code: %d", data_msg_id);
            }
        }
    } else {
        // Send complete JSON payload
        int msg_id = esp_mqtt_client_publish(mqtt_client, mqtt_topic_live, json_string, strlen(json_string), 0, 0);
        
        if (msg_id >= 0) {
            ESP_LOGI(MQTT_TAG, "Image with base64 content published successfully with msg_id=%d", msg_id);
        } else {
            ESP_LOGE(MQTT_TAG, "Failed to publish image with base64 content, error code: %d", msg_id);
        }
    }
    
    free(json_string);
    free(base64_buffer);
}

// Forward declaration for the get_live_json_payload_str function
char* get_live_json_payload_str(void) {
    // TODO: Implement this function based on your live data requirements
    return NULL;
}
