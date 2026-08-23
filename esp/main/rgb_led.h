#pragma once

#include <stdint.h>

#ifdef CONFIG_IDF_TARGET_ESP32S3
void rgb_led_init();
void rgb_led_set(uint8_t r, uint8_t g, uint8_t b);
void rgb_led_set_raw(uint8_t r, uint8_t g, uint8_t b);
void rgb_led_set_brightness(uint8_t brightness);
void rgb_led_set_flash_brightness(uint8_t brightness);
void rgb_led_flash_white();
#endif
