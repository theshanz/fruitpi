#include "bt_manager.h"
#include "camera_handler.h"
#include "esp_camera.h"
#include "extract_hues.h"
#include "fruit_store.h"
#include "lcd_display.h"
#include "led_indicator.h"
#include "multispectral.h"
#include "piezo_acoustic.h"
#include "sci_28d.h"
#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include <esp32-hal-gpio.h>

#include "config.h"
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "rgb_led.h"     // raw WS2812 drive for ms flash / capture cue
#endif

FruitStore store;
BTManager bt;
PiezoAcoustic acoustic_sensor(PIEZO_ADC_CHANNEL);

// ─── Shared state ─────────────────────────────────────────────────
float    dynamic_adc_threshold = PIEZO_DEFAULT_THRESHOLD;
bool     is_acoustic_armed = false;
uint32_t arm_start_time = 0;          // millis() when data-collection armed
bool     scan_requested = false;      // physical SCAN button edge

// Inference guide flow: cam scan → fruit on piezo → tap → result.
bool     awaiting_placement = false;   // cam scan done, waiting for SCAN press
bool     tap_capture_running = false;  // piezo armed for the inference tap
uint32_t placement_start_at = 0;
uint32_t scan_start_at = 0;            // millis() when the cam scan should run
ColorFeatures ms_features_pending;     // vision snapshot taken at cam scan
bool     have_ms_features = false;

// ─── Buttons ──────────────────────────────────────────────────────
struct PhysicalButton {
  uint8_t pin;
  bool last_state;
  uint32_t last_ms;
};

static bool button_pressed_edge(PhysicalButton &btn, uint32_t now) {
  bool raw = (digitalRead(btn.pin) == HIGH);
  bool edge = raw && !btn.last_state && (now - btn.last_ms >= BUTTON_DEBOUNCE_MS);
  if (edge) btn.last_ms = now;
  btn.last_state = raw;
  return edge;
}

static PhysicalButton scan_btn = {SCAN_BUTTON_PIN, false, 0};

// Data-collection arming: visible placement countdown first (piezo fully
// disarmed while the fruit is placed), then real listening.
enum class ArmStage : uint8_t { IDLE, GRACE, LISTENING };
static ArmStage    s_arm_stage = ArmStage::IDLE;
static uint32_t    s_grace_end_ms = 0;
static uint8_t     s_grace_shown_sec = 0;

static void begin_placement_grace(float threshold) {
  (void)threshold;
  s_arm_stage = ArmStage::GRACE;
  s_grace_end_ms = millis() + PLACE_GRACE_MS;
  s_grace_shown_sec = (PLACE_GRACE_MS + 999UL) / 1000UL;
  lcd_countdown(s_grace_shown_sec);
  led_pet_place_on_piezo();
  bt.notify_status_change("place_fruit");
}

// CANCEL via GPIO ISR: while armed, the sampler busy-waits at high priority
// on Core 1 and starves loop(), so a polled cancel button would be dead
// exactly when needed. Debounced at the consumer below.
static volatile bool s_isr_cancel = false;
static void IRAM_ATTR cancel_button_isr() { s_isr_cancel = true; }

// One arm path for every caller. ORDER MATTERS: the LCD must finish its bus
// burst BEFORE the sampler leaves DISARMED — an I2C burst right after arming
// couples into the piezo and false-triggers it.
static void start_acoustic_listening(float threshold) {
  lcd_set_quiet(true);
  acoustic_sensor.arm(threshold, ARM_SETTLE_MS * 1000UL);
}

// Idempotent: only announces when something was actually running.
static void cancel_scan() {
  bool was_active = tap_capture_running || awaiting_placement ||
                    acoustic_sensor.get_state() != STATE_DISARMED ||
                    scan_requested || is_acoustic_armed || scan_start_at != 0;
  is_acoustic_armed = false;
  tap_capture_running = false;
  awaiting_placement = false;
  placement_start_at = 0;
  scan_requested = false;
  scan_start_at = 0;
  s_arm_stage = ArmStage::IDLE;
  acoustic_sensor.disarm();
  if (was_active) {
    bt.notify_status_change("disarmed");
    led_pet_disarmed();
    lcd_disarmed();
  }
}

static void handle_laptop_capture_image_only() {
  Serial.println("[Laptop] Direct Camera Image Request...");
  if (!is_camera_ready()) {
    Serial.println("[Laptop] Camera not ready!");
    bt.notify_status_change("camera_error");
    return;
  }
  camera_fb_t *fb = capture_camera_frame();
  if (fb) {
    bt.send_raw_jpeg_stream(fb->buf, fb->len);
    release_camera_frame(fb);
    bt.notify_status_change("image_captured");
    led_pet_capturing();
  } else {
    bt.notify_status_change("camera_error");
    led_pet_camera_error();
    lcd_camera_error();
  }
}

// Raw waveform debug capture: N windows streamed back-to-back over BLE.
static void run_raw_capture() {
  const uint8_t WINDOWS = bt.get_raw_capture_windows();
  constexpr uint32_t PUMP_BUDGET_MS = 120;   // BLE pump time per window
#ifdef CONFIG_IDF_TARGET_ESP32S3
  rgb_led_set_raw(255, 60, 0);               // orange = capture in progress
#endif
  uint16_t raw_adc[512];
  uint16_t peak_adc;

  if (bt.is_raw_capture_trigger_mode()) {
    Serial.println("[RawCapture] Trigger mode: arm, tap, capture");
    for (int w = 0; w < WINDOWS; w++) {
      start_acoustic_listening(PIEZO_DEFAULT_THRESHOLD);
      uint32_t deadline = millis() + ARM_TIMEOUT_MS;
      while (!acoustic_sensor.is_data_ready() && millis() < deadline) {
        delay(1);
      }
      if (acoustic_sensor.is_data_ready()) {
        acoustic_sensor.capture_sampler_raw_window(raw_adc, &peak_adc);
      } else {
        acoustic_sensor.disarm();            // timeout — send an empty window
        memset(raw_adc, 0, sizeof(raw_adc));
        peak_adc = 0;
      }
      bt.send_raw_acoustic_waveform(raw_adc, peak_adc);
      uint32_t pump_end = millis() + PUMP_BUDGET_MS;
      while (bt.is_transfer_active() && millis() < pump_end) {
        bt.service_transfer();
        delay(5);
      }
    }
  } else {
    // Legacy continuous mode: raw samples without trigger. NOTE: reads zeros
    // on this hardware (main-task ADC quirk); trigger mode is the useful one.
    Serial.println("[RawCapture] Continuous mode (legacy)");
    for (int w = 0; w < WINDOWS; w++) {
      acoustic_sensor.capture_raw_waveform(raw_adc, &peak_adc);
      bt.send_raw_acoustic_waveform(raw_adc, peak_adc);
      uint32_t pump_end = millis() + PUMP_BUDGET_MS;
      while (bt.is_transfer_active() && millis() < pump_end) {
        bt.service_transfer();
        delay(5);
      }
    }
  }
#ifdef CONFIG_IDF_TARGET_ESP32S3
  rgb_led_set_raw(0, 0, 0);
#endif
  bt.notify_status_change("raw_capture_done");
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(SCAN_BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(CANCEL_BUTTON_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(CANCEL_BUTTON_PIN), cancel_button_isr,
                  RISING);
  led_indicator_init();
  lcd_display_init();

  if (init_camera_subsystem()) {
    Serial.println("[Main] Camera ready for captures.");
  } else {
    Serial.println("[Main] Camera init FAILED!");
  }
  if (!acoustic_sensor.init()) {
    Serial.println("Failed to initialize Piezo system!");
    while (1) ;
  }
  store.init();
  multispectral_load_calibration();
  bt.init("Fruitipi", &store);
}

void loop() {
  uint32_t current_time = millis();
  SystemMode mode = bt.get_mode();

  /// ── LCD bus discipline ────────────────────────────────────────────
  // While the piezo samples, the LCD stays silent (ADC coupling); any
  // screen requested meanwhile replays automatically when sampling ends.
  lcd_set_quiet(acoustic_sensor.get_state() != STATE_DISARMED);

  /// ── 1. BLE connection cues ────────────────────────────────────────
  static bool last_ble_connected = false;
  bool now_connected = bt.is_connected();
  if (now_connected != last_ble_connected) {
    if (now_connected) { led_pet_connected(); lcd_connected(); }
    else               { led_pet_disconnected(); lcd_disconnected(); }
    last_ble_connected = now_connected;
  }

  /// ── 2. Threshold updates from the laptop (single consumer) ───────
  if (bt.has_threshold_update()) {
    dynamic_adc_threshold = bt.get_updated_threshold();
    Serial.printf("[Laptop] Acoustic Threshold updated to: %.2f\n",
                  dynamic_adc_threshold);
  }

  /// ── 3. Inference triggers: BOOT button + BLE request ─────────────
  static uint32_t last_btn_ms = 0;
  static bool last_btn_state = HIGH;
  bool boot_trigger = false;

  bool btn_now = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  if (btn_now && !last_btn_state &&
      (current_time - last_btn_ms >= BUTTON_DEBOUNCE_MS)) {
    boot_trigger = true;                 // one press = one inference run
    last_btn_ms = current_time;
  }
  last_btn_state = btn_now;

  if (bt.check_inference_request() && mode == MODE_INFERENCE) {
    boot_trigger = true;
  }

  /// ── 4. Physical SCAN / CANCEL buttons ────────────────────────────
  if (button_pressed_edge(scan_btn, current_time)) {
    scan_requested = true;
    Serial.println("[Button] SCAN pressed");
  }
  if (s_isr_cancel) {
    s_isr_cancel = false;
    static uint32_t last_isr_cancel_ms = 0;
    if (current_time - last_isr_cancel_ms >= BUTTON_DEBOUNCE_MS) {
      last_isr_cancel_ms = current_time;
      Serial.println("[Button] CANCEL pressed");
      cancel_scan();
    }
  }

  bt.service_transfer();

  // Disarming from bluetooth
  if (bt.check_and_clear_cancel_request()) {
    cancel_scan();
  }

  /// ── 5. Data collection: SCAN arms the piezo for a streamed tap ───
  if (scan_requested && mode == MODE_DATA_COLLECTION) {
    scan_requested = false;
    if (acoustic_sensor.get_state() == STATE_DISARMED &&
        s_arm_stage != ArmStage::GRACE) {
      begin_placement_grace(dynamic_adc_threshold);
      Serial.printf("[Button] ARM — place fruit, TAP in %us\n",
                    (unsigned)(PLACE_GRACE_MS / 1000));
    }
  }

  // Grace expiry: placement window over — arm for real and say so.
  if (s_arm_stage == ArmStage::GRACE) {
    uint32_t left_ms = s_grace_end_ms - current_time;
    uint8_t sec_left = (uint8_t)((left_ms + 999UL) / 1000UL);
    if (sec_left != s_grace_shown_sec && sec_left > 0) {
      s_grace_shown_sec = sec_left;
      lcd_countdown(sec_left);           // bus is free: piezo still disarmed
    }
    if (current_time >= s_grace_end_ms) {
      s_arm_stage = ArmStage::LISTENING;
      is_acoustic_armed = true;
      arm_start_time = current_time;
      lcd_armed();                       // burst BEFORE arming (see helper)
      start_acoustic_listening(dynamic_adc_threshold);
      led_pet_armed();
      Serial.printf("[Acoustic] ARM (thresh=%.2f) — TAP NOW\n",
                    dynamic_adc_threshold);
      bt.notify_status_change("acoustic_armed");
    }
  }

  /// ── 6. Inference guide flow ───────────────────────────────────────
  bool guide_trigger =
      (boot_trigger || scan_requested) && mode == MODE_INFERENCE;

  // Step A — placement confirmed: second press arms the piezo.
  if (guide_trigger && awaiting_placement) {
    guide_trigger = false;
    scan_requested = false;
    awaiting_placement = false;
    placement_start_at = 0;
    lcd_armed();                         // burst BEFORE arming
    start_acoustic_listening(dynamic_adc_threshold);
    tap_capture_running = true;
    Serial.printf("[Guide] TAP NOW (thresh=%.2f)\n", dynamic_adc_threshold);
    bt.notify_status_change("acoustic_armed");
  }

  // Phase 1 — hold the fruit in front of the camera, then scan.
  if (guide_trigger && !tap_capture_running &&
      !awaiting_placement && scan_start_at == 0 && is_camera_ready()) {
    led_pet_place_fruit();
    lcd_place_fruit();
    Serial.println("[Guide] PLACE FRUIT — hold fruit in front of camera...");
    scan_start_at = current_time + PLACE_FRUIT_HOLD_MS;
  }

  // Phase 2 — multispectral scan (ambient + R/G/B flashes).
  if (scan_start_at != 0 && current_time >= scan_start_at) {
    scan_start_at = 0;
    scan_requested = false;              // trigger consumed by the scan
    ColorFeatures c_f;
    if (multispectral_capture(c_f)) {
      ms_features_pending = c_f;         // kept for the tap classifier below
      have_ms_features = true;
      awaiting_placement = true;
      placement_start_at = current_time;
      led_pet_place_on_piezo();
      lcd_place_on_piezo();
      Serial.println("[Guide] PLACE ON PIEZO — press SCAN once it sits there");
      bt.notify_status_change("place_on_piezo");
    } else {
      bt.notify_status_change("camera_error");
      lcd_camera_error();
    }
  }

  // Phase 3/4 — tap captured → classify → report.
  if (tap_capture_running && acoustic_sensor.is_data_ready()) {
    AcousticFeatures features = acoustic_sensor.get_latest_features();
    if (features.impact_amplitude < MIN_TAP_PEAK) {
      // Electrical noise, not a tap — discard and re-arm silently.
      Serial.printf("[Guide] Noise ignored (amp=%.3f) — re-arming\n",
                    features.impact_amplitude);
      start_acoustic_listening(dynamic_adc_threshold);
    } else {
      Serial.println("[Guide] TAP DETECTED — analyzing...");
      led_pet_tap_ok();

      float state[28];
      assemble_state_28d(state, ms_features_pending, features);
#if defined(SCI_28D_HARDCODED)
      static Fruit28D dummy_model = {};  // rule engine ignores stored models
      BiologicalStatus status = evaluate_fruit_single(state, dummy_model);
#else
      const Fruit28D *model = store.get_active_model_ptr();
      BiologicalStatus status = evaluate_fruit_single(state, *model);
#endif
      bt.notify_scan_result(status);
      led_pet_result(status.primary_decision, status.is_anomaly);
      lcd_result(status.primary_decision, status.is_anomaly, status.confidence);
      tap_capture_running = false;
      have_ms_features = false;
    }
  }

  // Placement timeout — user never pressed SCAN after the cam scan.
  if (awaiting_placement &&
      current_time - placement_start_at >= PLACEMENT_TIMEOUT_MS) {
    awaiting_placement = false;
    placement_start_at = 0;
    Serial.println("[Guide] PLACEMENT TIMEOUT — session expired");
    bt.notify_status_change("place_on_piezo_timeout");
    led_pet_timeout();
    lcd_placement_timeout();
  }

  /// ── 7. Laptop-triggered captures & calibration ───────────────────
  if (bt.check_capture_image_request()) {
    handle_laptop_capture_image_only();
  }

  if (bt.check_ms_scan_request()) {
    ColorFeatures ms_f;
    if (multispectral_capture(ms_f)) {
      bt.notify_ms_features(ms_f);
    } else {
      bt.notify_status_change("camera_error");
    }
  }

  if (bt.check_ms_debug_request()) {
#ifdef CONFIG_IDF_TARGET_ESP32S3
    if (!is_camera_ready()) {
      bt.notify_status_change("camera_error");
    } else {
      multispectral_lock_sensor();
      rgb_led_set_raw(0, 0, 0);
      // ch0 = no-flash ambient baseline, then R/G/B flashes.
      for (int ch = 0; ch < 4; ch++) {
        uint8_t r = 0, g = 0, b = 0;
        if (ch >= 1) {
          r = MS_FLASH_COLORS[ch - 1].r;
          g = MS_FLASH_COLORS[ch - 1].g;
          b = MS_FLASH_COLORS[ch - 1].b;
        }
        camera_fb_t *fb = multispectral_grab_flash_frame(r, g, b);
        if (fb) {
          bt.send_ms_debug_jpeg(fb->buf, fb->len);
          release_camera_frame(fb);
          // Pump until this JPEG finishes streaming before starting the
          // next, otherwise begin_transfer() wipes the buffer mid-send.
          uint32_t pump_end = millis() + 3000;
          while (bt.is_transfer_active() && millis() < pump_end) {
            bt.service_transfer();
            delay(5);
          }
        }
      }
      rgb_led_set_raw(0, 0, 0);
      multispectral_restore_sensor();
      bt.notify_status_change("ms_debug_done");
    }
#else
    bt.notify_status_change("camera_error");
#endif
  }

  {
    float gain_r, gain_g, gain_b;
    int aec;
    bool ambient;
    if (bt.check_ms_config_request(&gain_r, &gain_g, &gain_b, &aec, &ambient)) {
      multispectral_set_config(gain_r, gain_g, gain_b, aec, ambient);
    }
  }

  /// ── 8. BLE arm request (data collection) ─────────────────────────
  if (bt.check_arm_acoustic_request()) {
    if (acoustic_sensor.get_state() == STATE_DISARMED &&
        s_arm_stage != ArmStage::GRACE) {
      begin_placement_grace(dynamic_adc_threshold);
      Serial.printf("[Acoustic] ARM — grace %us, then listen\n",
                    (unsigned)(PLACE_GRACE_MS / 1000));
    } else {
      bt.notify_status_change("acoustic_already_armed");
    }
  }

  /// ── 9. Raw waveform debug capture ────────────────────────────────
  if (bt.check_raw_capture_request()) {
    run_raw_capture();
  }

  /// ── 10. Armed window: stream captured taps, enforce timeout ──────
  if (is_acoustic_armed) {
    if (acoustic_sensor.is_data_ready()) {
      AcousticFeatures feats = acoustic_sensor.get_latest_features();
      uint16_t raw_adc[512];
      uint16_t peak_adc;
      acoustic_sensor.capture_sampler_raw_window(raw_adc, &peak_adc);

      if (feats.impact_amplitude < MIN_TAP_PEAK) {
        // Electrical noise, not a tap — discard and re-arm silently.
        Serial.printf("[Acoustic] Noise ignored (amp=%.3f) — re-arming\n",
                      feats.impact_amplitude);
        arm_start_time = current_time;
        start_acoustic_listening(dynamic_adc_threshold);
      } else {
        bt.send_raw_acoustic_waveform(raw_adc, peak_adc);
        is_acoustic_armed = false;
        bt.notify_status_change("acoustic_captured");
        led_pet_tap_ok();
        lcd_tap_ok();
      }
    } else if (current_time - arm_start_time >= ARM_TIMEOUT_MS) {
      is_acoustic_armed = false;
      acoustic_sensor.disarm();
      bt.notify_status_change("timeout_disarmed");
      led_pet_timeout();
      lcd_timeout();
    }
  }

  led_indicator_update();
  lcd_display_update();
  delay(10);
}
