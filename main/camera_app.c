#include "esp_camera.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera_app.h"
#include "esp_http_server.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_CAMERA_SENSOR_OV5640
#include "ov5640.h"
#elif defined(CONFIG_CAMERA_SENSOR_OV2640)
#include "ov2640.h"
#endif

static const char *TAG = "CAMERA_APP";
static httpd_handle_t stream_httpd = NULL;

// ตัวแปรเก็บสถานะการพลิกภาพ
static bool vflip_enabled = false;
static bool hmirror_enabled = false;

// Motion Detection Variables - True pixel comparison
static bool motion_detection_enabled = true;
static uint32_t motion_cooldown_ms = 2000;   // 2 วินาที cooldown หลังตรวจพบ motion
static uint32_t last_motion_time = 0;
static bool force_send_next = false;  // สำหรับ manual trigger
static uint32_t motion_threshold = 15000;    // จำนวน pixels ที่เปลี่ยนแปลงถึงจะถือว่ามี motion
static uint32_t motion_pixel_threshold = 25; // ความแตกต่างของแต่ละ pixel (0-255)

// Frame comparison buffers
static uint8_t *previous_frame_buffer = NULL;
static size_t previous_frame_size = 0;
static bool first_frame = true;

// ตั้งค่ากล้องสำหรับ ESP32-S3-EYE
static camera_config_t camera_config = {
    .pin_pwdn = -1,
    .pin_reset = -1,
    .pin_xclk = 15,
    .pin_sscb_sda = 4,
    .pin_sscb_scl = 5,
    .pin_d7 = 16,
    .pin_d6 = 17,
    .pin_d5 = 18,
    .pin_d4 = 12,
    .pin_d3 = 10,
    .pin_d2 = 8,
    .pin_d1 = 9,
    .pin_d0 = 11,
    .pin_vsync = 6,
    .pin_href = 7,
    .pin_pclk = 13,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_SVGA,
    .jpeg_quality = 8,
    .fb_count = 2
};

static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char* _STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ฟังก์ชันสำหรับสตรีมภาพ
esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
        } else {
            if(fb->format != PIXFORMAT_JPEG){
                bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                if(!jpeg_converted){
                    ESP_LOGE(TAG, "JPEG compression failed");
                    esp_camera_fb_return(fb);
                    res = ESP_FAIL;
                }
            } else {
                _jpg_buf_len = fb->len;
                _jpg_buf = fb->buf;
            }
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(fb->format != PIXFORMAT_JPEG && _jpg_buf){
            free(_jpg_buf);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK){
            break;
        }
    }
    return res;
}

// ฟังก์ชันเริ่มต้นเว็บเซิร์ฟเวอร์
void start_camera_stream_server() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 80;
    config.max_open_sockets = 5;

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };

    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        ESP_LOGI(TAG, "Camera stream server started at /stream on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start camera stream server");
    }
}

// ฟังก์ชันเริ่มต้นกล้อง
esp_err_t camera_init(void) {
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        ESP_LOGE(TAG, "Get camera sensor failed");
        return ESP_FAIL;
    }

    // ตั้งค่าเซ็นเซอร์เริ่มต้น
    if(sensor->id.PID == OV5640_PID) {
        sensor->set_vflip(sensor, vflip_enabled);
        sensor->set_hmirror(sensor, hmirror_enabled);
        sensor->set_brightness(sensor, 1);
        sensor->set_saturation(sensor, 1);
    } else if(sensor->id.PID == OV2640_PID) {
        sensor->set_vflip(sensor, vflip_enabled);
        sensor->set_hmirror(sensor, hmirror_enabled);
        sensor->set_contrast(sensor, 1);
    }

    ESP_LOGI(TAG, "Camera initialized successfully");
    return ESP_OK;
}

// --- เพิ่ม handler สำหรับ /capture (snapshot) ---
static esp_err_t capture_handler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

// --- เพิ่ม handler สำหรับ /info (metadata) ---
static esp_err_t info_handler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"size\":%d,\"width\":%d,\"height\":%d,\"timestamp\":%lu}",
        fb->len, fb->width, fb->height, (unsigned long)esp_log_timestamp());
    esp_camera_fb_return(fb);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

// --- เพิ่มฟังก์ชันเริ่มต้น server สำหรับ /capture และ /info ---
void start_camera_snapshot_server() {
    if (!stream_httpd) return; // ต้องเริ่ม stream server ก่อน
    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &capture_uri);
}

void start_camera_info_server() {
    if (!stream_httpd) return;
    httpd_uri_t info_uri = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = info_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &info_uri);
}

// --- เพิ่ม handler สำหรับ /base64 ---
static esp_err_t base64_handler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // คำนวณขนาด base64 (ประมาณ 4/3 ของขนาดต้นฉบับ + padding)
    size_t base64_len = ((fb->len + 2) / 3) * 4 + 1;
    char *base64_buf = malloc(base64_len);
    if (!base64_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for base64");
        esp_camera_fb_return(fb);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // แปลงเป็น base64
    size_t actual_len = 0;
    int ret = mbedtls_base64_encode((unsigned char*)base64_buf, base64_len, &actual_len, fb->buf, fb->len);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 encoding failed: %d", ret);
        free(base64_buf);
        esp_camera_fb_return(fb);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "text/plain");
    esp_err_t res = httpd_resp_send(req, base64_buf, actual_len);
    
    free(base64_buf);
    esp_camera_fb_return(fb);
    return res;
}

void start_camera_base64_server() {
    if (!stream_httpd) return;
    httpd_uri_t base64_uri = {
        .uri = "/base64",
        .method = HTTP_GET,
        .handler = base64_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &base64_uri);
}

// True Motion Detection - Pixel comparison algorithm
static uint32_t calculate_frame_difference(const uint8_t *current, const uint8_t *previous, size_t len) {
    uint32_t diff_count = 0;
    
    // เปรียบเทียบทีละ pixel โดยข้ามบาง pixel เพื่อเพิ่มความเร็ว
    for (size_t i = 0; i < len && i < previous_frame_size; i += 4) {
        int32_t diff = abs((int32_t)current[i] - (int32_t)previous[i]);
        if (diff > motion_pixel_threshold) {
            diff_count++;
        }
    }
    
    return diff_count;
}

static void update_reference_frame(camera_fb_t *fb) {
    if (previous_frame_buffer) {
        free(previous_frame_buffer);
    }
    
    previous_frame_buffer = malloc(fb->len);
    if (previous_frame_buffer) {
        memcpy(previous_frame_buffer, fb->buf, fb->len);
        previous_frame_size = fb->len;
    }
}

bool detect_motion(camera_fb_t *fb) {
    if (!motion_detection_enabled || !fb || !fb->buf) {
        return false; // ไม่ส่งภาพถ้าปิด motion detection
    }
    
    uint32_t current_time = esp_log_timestamp();
    
    // ตรวจสอบ manual trigger
    if (force_send_next) {
        force_send_next = false;
        last_motion_time = current_time;
        update_reference_frame(fb);
        ESP_LOGI(TAG, "Manual motion trigger - sending image");
        return true;
    }
    
    // ตรวจสอบ cooldown period (ป้องกันการส่งภาพติดต่อกัน)
    if (current_time - last_motion_time < motion_cooldown_ms) {
        return false;
    }
    
    // ภาพแรก - เก็บเป็น reference frame
    if (first_frame || !previous_frame_buffer) {
        update_reference_frame(fb);
        first_frame = false;
        ESP_LOGI(TAG, "First frame captured for motion detection");
        return false; // ไม่ส่งภาพแรก
    }
    
    // คำนวณความแตกต่างระหว่างเฟรม
    uint32_t diff_pixels = calculate_frame_difference(fb->buf, previous_frame_buffer, fb->len);
    
    ESP_LOGI(TAG, "Motion analysis: %lu changed pixels, threshold: %lu", diff_pixels, motion_threshold);
    
    if (diff_pixels > motion_threshold) {
        last_motion_time = current_time;
        update_reference_frame(fb);
        ESP_LOGI(TAG, "Motion detected! Changed pixels: %lu", diff_pixels);
        return true;
    }
    
    ESP_LOGD(TAG, "No significant motion detected");
    return false;
}

// Function to manually trigger motion
void trigger_motion_manually(void) {
    force_send_next = true;
    ESP_LOGI(TAG, "Motion trigger manually activated");
}

void set_motion_detection(bool enable) {
    motion_detection_enabled = enable;
    ESP_LOGI(TAG, "Motion detection %s", enable ? "enabled" : "disabled");
}

void set_motion_threshold(uint32_t threshold) {
    motion_threshold = threshold;
    ESP_LOGI(TAG, "Motion threshold set to %lu", threshold);
}

void set_motion_pixel_threshold(uint32_t pixel_threshold) {
    motion_pixel_threshold = pixel_threshold;
    ESP_LOGI(TAG, "Motion pixel threshold set to %lu", pixel_threshold);
}

void cleanup_motion_detection(void) {
    if (previous_frame_buffer) {
        free(previous_frame_buffer);
        previous_frame_buffer = NULL;
        previous_frame_size = 0;
        ESP_LOGI(TAG, "Motion detection buffers cleaned up");
    }
}

void set_motion_cooldown(uint32_t cooldown_ms) {
    motion_cooldown_ms = cooldown_ms;
    ESP_LOGI(TAG, "Motion cooldown set to %lu ms", cooldown_ms);
}

// HTTP handlers for motion detection settings
static esp_err_t motion_settings_handler(httpd_req_t *req) {
    char buf[100];
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf) - 1) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // Parse simple URL parameters (enable=1&threshold=5000&pixel_threshold=30)
    char *enable_str = strstr(buf, "enable=");
    char *threshold_str = strstr(buf, "threshold=");
    char *pixel_threshold_str = strstr(buf, "pixel_threshold=");

    if (enable_str) {
        bool enable = (enable_str[7] == '1');
        set_motion_detection(enable);
    }

    if (threshold_str) {
        uint32_t threshold = atoi(threshold_str + 10);
        if (threshold > 0 && threshold < 100000) {
            set_motion_threshold(threshold);
        }
    }

    if (pixel_threshold_str) {
        uint32_t pixel_threshold = atoi(pixel_threshold_str + 16);
        if (pixel_threshold > 0 && pixel_threshold < 255) {
            set_motion_pixel_threshold(pixel_threshold);
        }
    }

    // Return current settings
    char response[200];
    snprintf(response, sizeof(response), 
        "{\"motion_detection\":%s,\"threshold\":%lu,\"pixel_threshold\":%lu}", 
        motion_detection_enabled ? "true" : "false",
        motion_threshold,
        motion_pixel_threshold);
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

static esp_err_t motion_status_handler(httpd_req_t *req) {
    uint32_t current_time = esp_log_timestamp();
    uint32_t time_since_last = current_time - last_motion_time;
    
    char response[200];
    snprintf(response, sizeof(response), 
        "{\"motion_detection_enabled\":%s,\"cooldown_ms\":%lu,\"time_since_last_ms\":%lu,\"can_trigger\":%s}", 
        motion_detection_enabled ? "true" : "false",
        motion_cooldown_ms,
        time_since_last,
        time_since_last >= motion_cooldown_ms ? "true" : "false");
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

static esp_err_t motion_trigger_handler(httpd_req_t *req) {
    trigger_motion_manually();
    
    char response[] = "{\"status\":\"triggered\",\"message\":\"Motion trigger activated\"}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

void start_camera_motion_control_server() {
    if (!stream_httpd) return;
    
    httpd_uri_t motion_settings_uri = {
        .uri = "/motion/settings",
        .method = HTTP_POST,
        .handler = motion_settings_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &motion_settings_uri);

    httpd_uri_t motion_status_uri = {
        .uri = "/motion/status",
        .method = HTTP_GET,
        .handler = motion_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &motion_status_uri);

    httpd_uri_t motion_trigger_uri = {
        .uri = "/motion/trigger",
        .method = HTTP_POST,
        .handler = motion_trigger_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(stream_httpd, &motion_trigger_uri);
    
    ESP_LOGI(TAG, "Motion control endpoints: /motion/status (GET), /motion/settings (POST), /motion/trigger (POST)");
}