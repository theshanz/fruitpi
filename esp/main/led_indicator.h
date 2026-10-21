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
// Freeze the animation while the piezo is listening.  The WS2812 is driven by
// the RMT peripheral at ~800 kHz, so an animating pattern is a continuous
// digital emitter on the board and couples into the piezo: freezing it keeps
// the LED's last colour and stops the waveform entirely.
void led_indicator_set_held(bool held);
bool led_indicator_is_held();
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
inline void led_indicator_set_held(bool) {}
inline bool led_indicator_is_held() { return false; }
#endif
