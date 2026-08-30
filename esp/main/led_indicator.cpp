// ─────────────────────────────────────────────────────────────────
//  led_indicator.cpp — WS2812 status cue player.
//
//  One persistent show runs underneath (idle = blue breath, or a held
//  guide state like ARMED). Transient cues play as overlays on top;
//  queued overlays chain after the current one (ANALYZING -> result).
// ─────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include "led_indicator.h"
#include "config.h"

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include <string.h>
#include "rgb_led.h"
#include "sci_32d.h"   // CLASS_LABELS — result mapped by index, not literals

enum class Pattern { SOLID, BREATH, BLINK, STROBE, DOUBLE_BLINK, FADE, RAINBOW };

struct LedShow {
  uint8_t r, g, b;
  Pattern pattern;
  float rate;            // pattern speed (Hz; meaning varies per pattern)
  uint32_t duration_ms;  // overlay length; 0 = runs until replaced
};

enum class ShowId {
  IDLE, PLACE_FRUIT, PLACE_ON_PIEZO, ARMED,
  CAPTURING, TAP_OK,
  UNRIPE, RIPE, OVERRIPE, ROTTEN, ARTIFICIAL, ANOMALY,
  ANALYZING, TIMEOUT, CAMERA_ERROR, DISARMED,
  CONNECTED, DISCONNECTED, BOOT
};

static const LedShow SHOWS[] = {
  /* IDLE           */ { 40, 120, 255, Pattern::BREATH, 0.3f, 0 },
  /* PLACE_FRUIT    */ { 255, 255, 255, Pattern::BLINK, 1.2f, 900 },
  /* PLACE_ON_PIEZO */ { 0, 255, 255, Pattern::BLINK, 1.0f, 0 },   // held persistent
  /* ARMED          */ { 160, 82, 45, Pattern::BREATH, 1.0f, 0 },   // held persistent
  /* CAPTURING      */ { 255, 255, 255, Pattern::SOLID, 0.0f, 400 },
  /* TAP_OK         */ { 160, 82, 45, Pattern::SOLID, 0.0f, 700 },
  /* UNRIPE         */ { 0, 255, 60, Pattern::SOLID, 0.0f, RESULT_HOLD_MS },
  /* RIPE           */ { 200, 255, 0, Pattern::SOLID, 0.0f, RESULT_HOLD_MS },
  /* OVERRIPE       */ { 255, 140, 0, Pattern::SOLID, 0.0f, RESULT_HOLD_MS },
  /* ROTTEN         */ { 255, 30, 30, Pattern::BLINK, 1.0f, RESULT_HOLD_MS },
  /* ARTIFICIAL     */ { 255, 0, 255, Pattern::BLINK, 2.0f, RESULT_HOLD_MS },
  /* ANOMALY        */ { 255, 40, 40, Pattern::STROBE, 8.0f, RESULT_HOLD_MS },
  /* ANALYZING      */ { 255, 190, 0, Pattern::STROBE, 8.0f, 500 },
  /* TIMEOUT        */ { 255, 110, 20, Pattern::DOUBLE_BLINK, 2.0f, 1500 },
  /* CAMERA_ERROR   */ { 255, 40, 40, Pattern::BLINK, 3.0f, 1500 },
  /* DISARMED       */ { 80, 140, 255, Pattern::FADE, 0.0f, 800 },
  /* CONNECTED      */ { 180, 80, 255, Pattern::BLINK, 4.0f, 1500 },
  /* DISCONNECTED   */ { 255, 60, 60, Pattern::DOUBLE_BLINK, 2.0f, 1200 },
  /* BOOT           */ { 0, 0, 0, Pattern::RAINBOW, 0.0f, 1200 },
};
static_assert(sizeof(SHOWS) / sizeof(SHOWS[0]) == (int)ShowId::BOOT + 1,
              "SHOWS table out of sync with ShowId");

static LedShow persistent_show;              // runs when no overlay is active
static LedShow overlay_show;
static bool overlay_active = false;
static uint32_t overlay_start = 0, overlay_end = 0;
static ShowId pending[2];                    // chained overlays (max 2)
static uint8_t pending_count = 0;
static bool scan_active = false;

void led_indicator_set_scan_active(bool active) { scan_active = active; }

static void play_now(ShowId id) {
  overlay_show = SHOWS[(int)id];
  overlay_active = true;
  overlay_start = millis();
  overlay_end = overlay_start + overlay_show.duration_ms;
}

static void set_persistent(ShowId id) {
  persistent_show = SHOWS[(int)id];
  overlay_active = false;
  pending_count = 0;
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

static void render(const LedShow &s, uint32_t t, uint8_t &r, uint8_t &g, uint8_t &b) {
  switch (s.pattern) {
    case Pattern::SOLID:
      r = s.r; g = s.g; b = s.b;
      break;
    case Pattern::BREATH: {
      float v = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * s.rate * (float)t / 1000.0f);
      float amp = 0.2f + 0.8f * v;
      r = (uint8_t)((float)s.r * amp);
      g = (uint8_t)((float)s.g * amp);
      b = (uint8_t)((float)s.b * amp);
      break;
    }
    case Pattern::BLINK: {
      uint32_t half = (uint32_t)(500.0f / s.rate);
      if ((t / half) & 1U) { r = 0; g = 0; b = 0; }
      else                 { r = s.r; g = s.g; b = s.b; }
      break;
    }
    case Pattern::STROBE: {
      uint32_t half = (uint32_t)(500.0f / s.rate);
      if ((t / half) & 1U) { r = 255; g = 255; b = 255; }
      else                 { r = s.r; g = s.g; b = s.b; }
      break;
    }
    case Pattern::DOUBLE_BLINK: {
      uint32_t a = (uint32_t)(500.0f / s.rate);
      uint32_t tt = t % (6U * a);
      bool on = (tt < a) || (tt >= 2U * a && tt < 3U * a);
      if (on) { r = s.r; g = s.g; b = s.b; }
      else    { r = s.r / 4; g = s.g / 4; b = s.b / 4; }
      break;
    }
    case Pattern::FADE: {
      uint32_t dur = s.duration_ms ? s.duration_ms : 1U;
      float v = sinf((float)M_PI * (float)(t % dur) / (float)dur);
      r = (uint8_t)((float)s.r * v);
      g = (uint8_t)((float)s.g * v);
      b = (uint8_t)((float)s.b * v);
      break;
    }
    case Pattern::RAINBOW: {
      hsv_to_rgb((uint8_t)((t / 8) & 0xFFU), 200, r, g, b);
      break;
    }
  }
}

void led_indicator_init() {
  rgb_led_init();
  set_persistent(ShowId::IDLE);
  play_now(ShowId::BOOT);
}

void led_indicator_update() {
  static uint32_t last_frame = 0;
  uint32_t now = millis();
  if (now - last_frame < LCD_FRAME_MS) {
    return;
  }
  last_frame = now;

  // A multispectral scan owns the WS2812 (driven raw for the R/G/B flashes).
  if (scan_active) {
    return;
  }

  if (overlay_active && now >= overlay_end) {
    if (pending_count > 0) {
      ShowId next = pending[0];
      pending[0] = pending[1];
      pending_count--;
      overlay_show = SHOWS[(int)next];
      overlay_start = now;
      overlay_end = now + overlay_show.duration_ms;
    } else {
      overlay_active = false;
    }
  }

  uint8_t r, g, b;
  if (overlay_active) {
    render(overlay_show, now - overlay_start, r, g, b);
  } else {
    render(persistent_show, now, r, g, b);
  }
  rgb_led_set(r, g, b);
}

void led_pet_capturing()        { play_now(ShowId::CAPTURING); }
void led_pet_place_fruit()      { play_now(ShowId::PLACE_FRUIT); }
void led_pet_place_on_piezo()   { set_persistent(ShowId::PLACE_ON_PIEZO); }
void led_pet_armed()            { set_persistent(ShowId::ARMED); }
void led_pet_timeout()          { set_persistent(ShowId::IDLE); play_now(ShowId::TIMEOUT); }
void led_pet_camera_error()     { play_now(ShowId::CAMERA_ERROR); }
void led_pet_disarmed()         { set_persistent(ShowId::IDLE); play_now(ShowId::DISARMED); }
void led_pet_connected()        { play_now(ShowId::CONNECTED); }
void led_pet_disconnected()     { play_now(ShowId::DISCONNECTED); }

void led_pet_tap_ok() {
  // Data-collection path has no result to follow — return toward idle after
  // the "captured" cue. Inference path re-chains ANALYZING via led_pet_result.
  set_persistent(ShowId::IDLE);
  play_now(ShowId::TAP_OK);
}

void led_pet_result(const char* decision, bool is_anomaly) {
  set_persistent(ShowId::IDLE);

  ShowId res = ShowId::RIPE;
  if (is_anomaly) {
    res = ShowId::ANOMALY;
  } else if (decision != nullptr) {
    for (int i = 0; i < NUM_CLASSES; i++) {
      if (decision == CLASS_LABELS[i]) { res = (ShowId)((int)ShowId::UNRIPE + i); break; }
    }
  }

  if (overlay_active && pending_count > 0) {
    // A chain is already playing — append so the result shows after it.
    if (pending_count < 2) pending[pending_count++] = res;
  } else {
    play_now(ShowId::ANALYZING);
    pending[0] = res;
    pending_count = 1;
  }
}
#endif
