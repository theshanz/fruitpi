#pragma once

#ifdef CONFIG_IDF_TARGET_ESP32S3
void led_indicator_init();
void led_indicator_update();
void led_pet_capturing();
void led_pet_place_fruit();
void led_pet_place_on_piezo();
void led_pet_armed();
void led_pet_tap_ok();
void led_pet_result(const char* decision, bool is_anomaly);
void led_pet_timeout();
void led_pet_camera_error();
void led_pet_disarmed();
void led_pet_connected();
void led_pet_disconnected();
void led_indicator_set_scan_active(bool active);
#else
inline void led_indicator_init() {}
inline void led_indicator_update() {}
inline void led_pet_capturing() {}
inline void led_pet_place_fruit() {}
inline void led_pet_place_on_piezo() {}
inline void led_pet_armed() {}
inline void led_pet_tap_ok() {}
inline void led_pet_result(const char*, bool) {}
inline void led_pet_timeout() {}
inline void led_pet_camera_error() {}
inline void led_pet_disconnected() {}
inline void led_pet_connected() {}
inline void led_pet_disarmed() {}
inline void led_indicator_set_scan_active(bool) {}
#endif
