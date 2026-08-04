#include <Arduino.h>
#include "led_indicator.h"

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include <string.h>
#include "rgb_led.h"

enum class Pattern {
  SOLID,
  BREATH,
  BLINK,
  STROBE,
  DOUBLE_BLINK,
  FADE,
  RAINBOW,
  FLASH_WHITE
};

struct LedShow {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  Pattern pattern;
  float rate;
  uint32_t duration_ms;
};

enum class ShowId {
  IDLE,
  PLACE_FRUIT,
  ARMED,
  CAPTURING,
  TAP_OK,
  UNRIPE,
  RIPE,
  OVERRIPE,
  ROTTEN,
  ARTIFICIAL,
  ANOMALY,
  ANALYZING,
  TIMEOUT,
  CAMERA_ERROR,
  DISARMED,
  CONNECTED,
  BOOT
};

static const LedShow SHOWS[] = {
  { 40, 120, 255, Pattern::BREATH, 0.3f, 0 },           // IDLE
  { 255, 255, 255, Pattern::BLINK, 1.2f, 900 },         // PLACE_FRUIT (slow white blink)
  { 160, 82, 45, Pattern::BREATH, 1.0f, 0 },             // ARMED — brown breath "tap now"
  { 255, 255, 255, Pattern::FLASH_WHITE, 0.0f, 600 },   // CAPTURING
  { 160, 82, 45, Pattern::SOLID, 0.0f, 700 },            // TAP_OK — brown "tap captured"
  { 0, 255, 60, Pattern::SOLID, 0.0f, 5000 },           // UNRIPE
  { 200, 255, 0, Pattern::SOLID, 0.0f, 5000 },          // RIPE
  { 255, 140, 0, Pattern::SOLID, 0.0f, 5000 },          // OVERRIPE
  { 255, 30, 30, Pattern::BLINK, 1.0f, 5000 },          // ROTTEN
  { 255, 0, 255, Pattern::BLINK, 2.0f, 5000 },          // ARTIFICIAL
  { 255, 40, 40, Pattern::STROBE, 8.0f, 5000 },         // ANOMALY
  { 255, 190, 0, Pattern::STROBE, 8.0f, 500 },          // ANALYZING
  { 255, 110, 20, Pattern::DOUBLE_BLINK, 2.0f, 1500 },  // TIMEOUT
  { 255, 40, 40, Pattern::BLINK, 3.0f, 1500 },          // CAMERA_ERROR
  { 80, 140, 255, Pattern::FADE, 0.0f, 800 },           // DISARMED
  { 180, 80, 255, Pattern::BLINK, 4.0f, 1500 },         // CONNECTED
  { 0, 0, 0, Pattern::RAINBOW, 0.0f, 1200 },            // BOOT
};

static LedShow persistent_show;
static bool overlay_active = false;
static LedShow overlay_show;
static ShowId pending_shows[3];
static uint8_t pending_count = 0;
static uint32_t overlay_start = 0;
static uint32_t overlay_end = 0;
static bool scan_active = false;

void led_indicator_set_scan_active(bool active) {
  scan_active = active;
}

static void hsv_to_rgb(uint8_t hue, uint8_t val, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint16_t v = val;
  uint16_t f = (uint16_t)((hue * 6U) & 0xFFU);
  uint8_t hi = (uint8_t)((hue * 6U) >> 8);
  uint8_t p = 0;
  uint8_t q = (uint8_t)(v * (255U - f) / 255U);
  uint8_t t = (uint8_t)(v * f / 255U);
  switch (hi) {
    case 0: r = (uint8_t)v; g = t; b = p; break;
    case 1: r = q; g = (uint8_t)v; b = p; break;
    case 2: r = p; g = (uint8_t)v; b = t; break;
    case 3: r = p; g = q; b = (uint8_t)v; break;
    case 4: r = t; g = p; b = (uint8_t)v; break;
    default: r = (uint8_t)v; g = p; b = q; break;
  }
}

static bool render(const LedShow &s, uint32_t t, uint8_t &r, uint8_t &g, uint8_t &b) {
  switch (s.pattern) {
    case Pattern::SOLID:
      r = s.r;
      g = s.g;
      b = s.b;
      break;
    case Pattern::BREATH: {
      float v = 0.5f + 0.5f * sinf(2.0f * 3.14159f * s.rate * (float)t / 1000.0f);
      float amp = 0.2f + 0.8f * v;
      r = (uint8_t)((float)s.r * amp);
      g = (uint8_t)((float)s.g * amp);
      b = (uint8_t)((float)s.b * amp);
      break;
    }
    case Pattern::BLINK: {
      uint32_t half = (uint32_t)(500.0f / s.rate);
      if (((t / half) & 1U) == 0) {
        r = s.r; g = s.g; b = s.b;
      } else {
        r = 0; g = 0; b = 0;
      }
      break;
    }
    case Pattern::STROBE: {
      uint32_t half = (uint32_t)(500.0f / s.rate);
      if (((t / half) & 1U) == 0) {
        r = 255; g = 255; b = 255;
      } else {
        r = s.r; g = s.g; b = s.b;
      }
      break;
    }
    case Pattern::DOUBLE_BLINK: {
      uint32_t a = (uint32_t)(500.0f / s.rate);
      uint32_t cyc = 6U * a;
      uint32_t tt = t % cyc;
      bool on = (tt < a) || (tt >= 2U * a && tt < 3U * a);
      if (on) {
        r = s.r; g = s.g; b = s.b;
      } else {
        r = (uint8_t)(s.r / 4); g = (uint8_t)(s.g / 4); b = (uint8_t)(s.b / 4);
      }
      break;
    }
    case Pattern::FADE: {
      uint32_t dur = s.duration_ms ? s.duration_ms : 1U;
      float tt = (float)(t % dur) / (float)dur;
      float v = sinf(3.14159f * tt);
      r = (uint8_t)((float)s.r * v);
      g = (uint8_t)((float)s.g * v);
      b = (uint8_t)((float)s.b * v);
      break;
    }
    case Pattern::RAINBOW: {
      uint8_t hue = (uint8_t)((t / 8) & 0xFFU);
      hsv_to_rgb(hue, 200, r, g, b);
      break;
    }
    case Pattern::FLASH_WHITE: {
      rgb_led_flash_white();
      return true;
    }
  }
  return false;
}

static void start_overlay(ShowId id) {
  overlay_show = SHOWS[(int)id];
  overlay_active = true;
  pending_count = 0;
  overlay_start = millis();
  overlay_end = overlay_start + overlay_show.duration_ms;
}

void led_indicator_init() {
  rgb_led_init();
  persistent_show = SHOWS[(int)ShowId::IDLE];
  overlay_active = false;
  pending_count = 0;
  start_overlay(ShowId::BOOT);
}

void led_indicator_update() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 4) {
    return;
  }
  last = now;

  // A multispectral scan owns the WS2812 (LED is driven at full brightness for
  // the R/G/B flashes). Do not let the pet indicator touch the LED meanwhile.
  if (scan_active) {
    return;
  }

  if (overlay_active && now >= overlay_end) {
    if (pending_count > 0) {
      ShowId nxt = pending_shows[0];
      for (uint8_t i = 0; i + 1 < pending_count; i++) {
        pending_shows[i] = pending_shows[i + 1];
      }
      pending_count--;
      overlay_show = SHOWS[(int)nxt];
      overlay_active = true;
      overlay_start = now;
      overlay_end = overlay_start + overlay_show.duration_ms;
    } else {
      overlay_active = false;
    }
  }

  uint8_t r, g, b;
  bool direct = false;
  if (overlay_active) {
    direct = render(overlay_show, now - overlay_start, r, g, b);
  } else {
    render(persistent_show, now, r, g, b);
  }
  if (!direct) {
    rgb_led_set(r, g, b);
  }
}

void led_pet_capturing() {
  rgb_led_flash_white();
  start_overlay(ShowId::CAPTURING);
}

void led_pet_place_fruit() {
  start_overlay(ShowId::PLACE_FRUIT);
}

void led_pet_armed() {
  persistent_show = SHOWS[(int)ShowId::ARMED];
  overlay_active = false;
  pending_count = 0;
}

void led_pet_tap_ok() {
  overlay_show = SHOWS[(int)ShowId::TAP_OK];
  overlay_active = true;
  overlay_start = millis();
  overlay_end = overlay_start + overlay_show.duration_ms;
  // chain: TAP_OK -> ANALYZING -> (result appended by led_pet_result)
  pending_count = 1;
  pending_shows[0] = ShowId::ANALYZING;
}

void led_pet_result(const char* decision, bool is_anomaly) {
  persistent_show = SHOWS[(int)ShowId::IDLE];
  ShowId res = ShowId::RIPE;
  if (is_anomaly) {
    res = ShowId::ANOMALY;
  } else if (decision != nullptr) {
    if (strcmp(decision, "UNRIPE") == 0) {
      res = ShowId::UNRIPE;
    } else if (strcmp(decision, "PERFECTLY_RIPE") == 0) {
      res = ShowId::RIPE;
    } else if (strcmp(decision, "OVERRIPE") == 0) {
      res = ShowId::OVERRIPE;
    } else if (strcmp(decision, "ROTTEN_OR_HOLLOW") == 0) {
      res = ShowId::ROTTEN;
    } else if (strcmp(decision, "ARTIFICIALLY_RIPENED") == 0) {
      res = ShowId::ARTIFICIAL;
    }
  }
  uint32_t now = millis();
  if (overlay_active && pending_count > 0) {
    // A guide chain is playing (e.g. after led_pet_tap_ok): append the result
    // so it shows after ANALYZING instead of clobbering the current cue.
    if (pending_count < 3) {
      pending_shows[pending_count++] = res;
    }
  } else {
    overlay_show = SHOWS[(int)ShowId::ANALYZING];
    overlay_active = true;
    pending_count = 1;
    pending_shows[0] = res;
    overlay_start = now;
    overlay_end = overlay_start + overlay_show.duration_ms;
  }
}

void led_pet_timeout() {
  persistent_show = SHOWS[(int)ShowId::IDLE];
  start_overlay(ShowId::TIMEOUT);
}

void led_pet_camera_error() {
  start_overlay(ShowId::CAMERA_ERROR);
}

void led_pet_disarmed() {
  persistent_show = SHOWS[(int)ShowId::IDLE];
  start_overlay(ShowId::DISARMED);
}

void led_pet_connected() {
  start_overlay(ShowId::CONNECTED);
}

void led_pet_disconnected() {
  start_overlay(ShowId::DISARMED);
}
#endif
