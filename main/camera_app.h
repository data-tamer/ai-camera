#ifndef CAMERA_APP_H
#define CAMERA_APP_H

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ฟังก์ชันเริ่มต้นระบบกล้อง */
esp_err_t camera_init(void);

/* ฟังก์ชันจับภาพจากกล้อง */
camera_fb_t* camera_capture(void);

/* ฟังก์ชันคืนเฟรมบัฟเฟอร์ */
void camera_return_fb(camera_fb_t *fb);

/* ฟังก์ชันปิดการทำงานกล้อง */
void camera_deinit(void);

/* ฟังก์ชันเข้าถึงเซ็นเซอร์ */
sensor_t* camera_get_sensor(void);

/* ฟังก์ชันควบคุมการพลิกภาพ */
esp_err_t camera_set_vflip(bool enable);
esp_err_t camera_set_hmirror(bool enable);
esp_err_t camera_set_flip(bool vflip, bool hmirror);

/* ฟังก์ชันตรวจสอบสถานะการพลิกภาพ */
bool camera_get_vflip_status(void);
bool camera_get_hmirror_status(void);

/* ฟังก์ชันเริ่มต้นเว็บเซิร์ฟเวอร์สำหรับสตรีมมิ่ง */
void start_camera_stream_server(void);
void start_camera_snapshot_server(void);
void start_camera_info_server(void);
void start_camera_base64_server(void);

/* ฟังก์ชัน Motion Detection */
bool detect_motion(camera_fb_t *fb);
void set_motion_detection(bool enable);
void set_motion_threshold(uint32_t threshold);
void set_motion_pixel_threshold(uint32_t pixel_threshold);
void set_motion_cooldown(uint32_t cooldown_ms);
void trigger_motion_manually(void);
void cleanup_motion_detection(void);
void start_camera_motion_control_server(void);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_APP_H */