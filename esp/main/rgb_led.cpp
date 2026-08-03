#include <Arduino.h>
#include "rgb_led.h"

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "esp32-hal-rmt.h"

#define RGB_LED_PIN 48
#define RGB_MASTER_BRIGHTNESS_DEFAULT 20
#define RGB_FLASH_BRIGHTNESS_DEFAULT 255

static rmt_obj_t *rmt_handle = NULL;
static uint8_t g_brightness = RGB_MASTER_BRIGHTNESS_DEFAULT;
static uint8_t g_flash_brightness = RGB_FLASH_BRIGHTNESS_DEFAULT;

void rgb_led_init() {
  rmt_handle = rmtInit(RGB_LED_PIN, RMT_TX_MODE, RMT_MEM_64);
  if (rmt_handle != NULL) {
    rmtSetTick(rmt_handle, 100);
  }
}

static void rgb_write(uint8_t r, uint8_t g, uint8_t b) {
  if (rmt_handle == NULL) {
    return;
  }
  uint8_t buf[3] = {g, r, b};
  rmt_data_t items[24];
  for (int i = 0; i < 3; i++) {
    for (int bit = 7; bit >= 0; bit--) {
      bool one = (buf[i] >> bit) & 1U;
      int idx = i * 8 + (7 - bit);
      items[idx].level0 = 1;
      items[idx].duration0 = one ? 8 : 4;
      items[idx].level1 = 0;
      items[idx].duration1 = one ? 5 : 9;
    }
  }
  rmtWriteBlocking(rmt_handle, items, 24);
}

void rgb_led_set(uint8_t r, uint8_t g, uint8_t b) {
  rgb_write((uint8_t)((uint16_t)r * g_brightness / 255U),
            (uint8_t)((uint16_t)g * g_brightness / 255U),
            (uint8_t)((uint16_t)b * g_brightness / 255U));
}

void rgb_led_flash_white() {
  rgb_write(g_flash_brightness, g_flash_brightness, g_flash_brightness);
}

void rgb_led_set_brightness(uint8_t brightness) {
  g_brightness = brightness;
}

void rgb_led_set_flash_brightness(uint8_t brightness) {
  g_flash_brightness = brightness;
}
#endif
