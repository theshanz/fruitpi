#pragma once

#include <Arduino.h>

#ifdef CONFIG_IDF_TARGET_ESP32S3
void lcd_display_init();
void lcd_display_update();          // call every loop(): revert timers
void lcd_set_quiet(bool quiet);     // true = freeze bus (piezo listening)

void lcd_connected();
void lcd_disconnected();
void lcd_disarmed();
void lcd_armed();                  // "TAP fruit!" — listening starts
void lcd_countdown(uint8_t sec);   // "TAP in Ns" — placement grace
void lcd_tap_ok();
void lcd_result(const char* decision, bool is_anomaly, float confidence_0_100);
void lcd_timeout();
void lcd_placement_timeout();
void lcd_camera_error();
void lcd_place_fruit();
void lcd_place_on_piezo();
#else
// No display wired on this board — every call compiles away.
inline void lcd_display_init() {}
inline void lcd_display_update() {}
inline void lcd_set_quiet(bool) {}
inline void lcd_connected() {}
inline void lcd_disconnected() {}
inline void lcd_disarmed() {}
inline void lcd_armed() {}
inline void lcd_countdown(uint8_t) {}
inline void lcd_tap_ok() {}
inline void lcd_result(const char*, bool, float) {}
inline void lcd_timeout() {}
inline void lcd_placement_timeout() {}
inline void lcd_camera_error() {}
inline void lcd_place_fruit() {}
inline void lcd_place_on_piezo() {}
#endif
