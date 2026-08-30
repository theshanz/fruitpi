#include "bt_manager.h"
#include "camera_handler.h"
#include "esp_camera.h"
#include "extract_hues.h"
#include "fruit_store.h"
#include "lcd_display.h"
#include "led_indicator.h"
#include "multispectral.h"
#include "piezo_acoustic.h"
#include "sci_32d.h"
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
uint32_t tap_arm_start_ms = 0;         // armed-at timestamp (tap timeout)
uint32_t placement_start_at = 0;
uint32_t scan_start_at = 0;            // millis() when the cam scan should run
ColorFeatures ms_features_pending;     // vision snapshot taken at cam scan
bool     have_ms_features = false;

// Multi-tap consensus: each tap contributes a block-normalized state vector;
// collected taps are fused by median posterior in evaluate_fruit_ntap_32d.
float    tap_states[MAX_CONSENSUS_TAPS][32];
uint8_t  current_tap_idx = 0;          // how many taps collected so far
uint8_t  total_taps = 1;               // target from BLE ScanConfig (clamped)

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
enum class ArmStage : uint8_t { IDLE, GRACE, LISTENING, AWAITING_READY };
static ArmStage    s_arm_stage = ArmStage::IDLE;
static uint32_t    s_grace_end_ms = 0;
static uint8_t     s_grace_shown_sec = 0;
static uint32_t    s_ready_deadline_ms = 0; // BLE data-collection ready window

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
  tap_arm_start_ms = 0;
  current_tap_idx = 0;
  awaiting_placement = false;
  placement_start_at = 0;
  scan_requested = false;
  scan_start_at = 0;
  s_arm_stage = ArmStage::IDLE;
  s_ready_deadline_ms = 0;
  acoustic_sensor.disarm();
  if (was_active) {
    bt.notify_status_change("disarmed");
    led_pet_disarmed();
    lcd_disarmed();
  }
}

// ─── Model selector (hold START to open, CANCEL anywhere leaves) ──
static bool    s_menu_open = false;
static uint8_t s_menu_idx = 0;

// Default/idle screens always carry the active model badge.
static void refresh_default_screen() {
  lcd_set_active_model(store.has_model() ? store.get_loaded_fruit_name()
                                         : nullptr);
  lcd_idle();
}

static void draw_model_menu() {
  uint8_t count = store.model_count();
  char name[MAX_MODEL_NAME_LEN] = "?";
  store.model_name_at(s_menu_idx, name);
  lcd_menu(name, s_menu_idx, count, store.active_index());
}

static void open_model_menu() {
  uint8_t count = store.model_count();
  if (count == 0) {
    lcd_flash("No models!", "Save via app");
    Serial.println("[Menu] Selector empty — save models over BLE first");
    return;
  }
  s_menu_open = true;
  uint8_t act = store.active_index();
  s_menu_idx = (act != 0xFF) ? act : 0;
  draw_model_menu();
  lcd_flash("SELECT MODEL", "SCN nxt CNC exit");
  Serial.printf("[Menu] Model selector open (%u models)\n", count);
}

static void close_model_menu(bool cancelled) {
  s_menu_open = false;
  refresh_default_screen();
  if (cancelled) lcd_flash("Cancelled", nullptr);
  Serial.println("[Menu] Selector closed");
}

static void commit_menu_selection() {
  if (!s_menu_open || s_menu_idx >= store.model_count()) return;
  char name[MAX_MODEL_NAME_LEN] = "?";
  store.model_name_at(s_menu_idx, name);
  if (store.activate_by_index(s_menu_idx)) {
    s_menu_open = false;
    refresh_default_screen();
    lcd_flash("Selected:", name);
    bt.notify_status_change("model_activated");
    Serial.printf("[Menu] Activated '%s'\n", name);
  } else {
    lcd_flash("Select failed", nullptr);
  }
}

// HOLD CANCEL anywhere: forget the persisted choice + unload from RAM.
static void reset_model_selection() {
  cancel_scan();
  store.clear_active();
  refresh_default_screen();
  if (s_menu_open) { draw_model_menu(); lcd_flash("Reset — none", "active"); }
  else             { lcd_flash("Model cleared", nullptr); }
  bt.notify_status_change("model_cleared");
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
  // START and SCAN are merged onto one GPIO14 button (active-high).
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLDOWN);   // == SCAN_BUTTON_PIN (same pin)
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
  refresh_default_screen();              // badge the idle screen if a model
                                         // survived in NVS
  multispectral_load_calibration();
  bt.init("Fruitipi", &store);
}

void loop() {
  uint32_t current_time = millis();
  SystemMode mode = bt.get_mode();

  // Mode-change cue: announce DEBUG loudly so nobody scans blind.
  static SystemMode last_announced_mode = MODE_INFERENCE;
  if (mode != last_announced_mode) {
    if (mode == MODE_DEBUG) lcd_flash("DEBUG MODE", nullptr);
    last_announced_mode = mode;
  }

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

  /// ── 3. START/SCAN button (merged on GPIO14): tap = inference/scan, ──
  ///    hold = model selector. Active-high (INPUT_PULLDOWN).
  static bool     btn_prev = false;
  static uint32_t btn_press_ms = 0;
  static bool     btn_hold_done = false;
  bool boot_trigger = false;

  bool btn_now = (digitalRead(BOOT_BUTTON_PIN) == HIGH);  // == SCAN_BUTTON_PIN
  if (btn_now && !btn_prev) {
    btn_press_ms = current_time;
    btn_hold_done = false;
  }
  if (btn_now && !btn_hold_done &&
      current_time - btn_press_ms >= MENU_HOLD_MS) {
    btn_hold_done = true;              // long press consumed, never a tap
    if (s_menu_open) commit_menu_selection();   // hold again = use browsed
    else             open_model_menu();          // hold = open the selector
  }
  if (!btn_now && btn_prev && !btn_hold_done &&
      current_time - btn_press_ms >= BUTTON_DEBOUNCE_MS) {
    // Clean short tap.
    if (s_menu_open) {
      // In the selector: a tap advances through stored models.
      uint8_t count = store.model_count();
      if (count > 0) {
        s_menu_idx = (uint8_t)((s_menu_idx + 1) % count);
        draw_model_menu();
        Serial.printf("[Button] Button cycles -> model %u\n", s_menu_idx);
      }
    } else {
      boot_trigger = true;              // run inference
      scan_requested = true;            // merged SCAN role (data-collection arm / guide)
      Serial.println("[Button] START/SCAN pressed");
    }
  }
  btn_prev = btn_now;

  // DEBUG mode runs the same guide flow — it just records instead of
  // classifying (and skips the no-model gate below).
  if (bt.check_inference_request() &&
      (mode == MODE_INFERENCE || mode == MODE_DEBUG) && !s_menu_open) {
    boot_trigger = true;
  }

  // CANCEL works everywhere. The ISR only stamps the press; the verdict
  // waits here so a press can grow into a hold (= selection reset). A
  // short tap cancels whatever is running / closes the selector.
  static bool     cancel_pending = false;
  static uint32_t cancel_press_ms = 0;
  static bool     cancel_hold_done = false;
  static uint32_t last_cancel_ms = 0;
  if (s_isr_cancel) {
    s_isr_cancel = false;
    if (!cancel_pending && current_time - last_cancel_ms >= BUTTON_DEBOUNCE_MS) {
      cancel_pending = true;
      cancel_press_ms = current_time;
      cancel_hold_done = false;
      last_cancel_ms = current_time;
    }
  }
  if (cancel_pending) {
    const bool cancel_down = digitalRead(CANCEL_BUTTON_PIN) == HIGH;
    if (cancel_down && !cancel_hold_done &&
        current_time - cancel_press_ms >= MENU_HOLD_MS) {
      cancel_hold_done = true;
      reset_model_selection();           // held long enough: wipe selection
    }
    if (!cancel_down && !cancel_hold_done) {
      Serial.println("[Button] CANCEL pressed");
      cancel_scan();                     // stop any run first...
      if (s_menu_open) close_model_menu(true);  // ...then leave the selector
    }
    if (!cancel_down) cancel_pending = false;   // released — verdict served
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
      (boot_trigger || scan_requested) &&
      (mode == MODE_INFERENCE || mode == MODE_DEBUG) &&
      !s_menu_open;                      // selector owns the buttons meanwhile

  // Nothing to classify without an active model — point at the selector.
  // MODE_DEBUG skips this gate: it records vectors, classification optional.
  if (guide_trigger && !store.has_model() &&
      bt.get_mode() != MODE_DEBUG) {
    guide_trigger = false;
    scan_requested = false;
    Serial.println("[Guide] No active model — hold START to select one");
    lcd_flash("No model!", "HOLD START");
    bt.notify_status_change("no_model");
  }

  // Step A — placement confirmed: second press arms the piezo.
  if (guide_trigger && awaiting_placement) {
    guide_trigger = false;
    scan_requested = false;
    awaiting_placement = false;
    placement_start_at = 0;
    lcd_armed();                         // burst BEFORE arming
    start_acoustic_listening(dynamic_adc_threshold);
    tap_capture_running = true;
    tap_arm_start_ms = current_time;
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
      if (bt.get_mode() == MODE_DEBUG) {
        // Debug mode: skip second-press wait — auto-arm piezo immediately
        // so the BLE connection doesn't drop before we can arm.
        start_acoustic_listening(dynamic_adc_threshold);
        tap_capture_running = true;
        tap_arm_start_ms = current_time;
        Serial.println("[Guide] DEBUG: auto-armed piezo — TAP NOW");
        bt.notify_status_change("acoustic_armed");
        led_pet_armed();
      } else {
        // Multi-tap consensus: read the target tap count from BLE ScanConfig
        // (clamped 1..7). A run-wide volume override replaces the
        // camera-derived estimate for every tap of this run.
        total_taps = bt.get_target_tap_count();
        current_tap_idx = 0;
        awaiting_placement = true;
        placement_start_at = current_time;
        led_pet_place_on_piezo();
        lcd_place_on_piezo();
        Serial.printf("[Guide] PLACE ON PIEZO — %u tap%s, press SCAN when ready\n",
                      total_taps, total_taps == 1 ? "" : "s");
        bt.notify_status_change("place_on_piezo");
      }
    } else {
      bt.notify_status_change("camera_error");
      lcd_camera_error();
    }
  }

  // Phase 3/4 — tap captured → classify → report.
  // No tap within the arm window: abandon the run instead of hanging armed.
  if (tap_capture_running && !acoustic_sensor.is_data_ready() &&
      current_time - tap_arm_start_ms >= ARM_TIMEOUT_MS) {
    tap_capture_running = false;
    placement_start_at = 0;
    acoustic_sensor.disarm();
    current_tap_idx = 0;               // discard partial consensus run
    Serial.println("[Guide] TAP TIMEOUT — run discarded");
    bt.notify_status_change("timeout_disarmed");
    led_pet_timeout();
    lcd_timeout();
  }

  if (tap_capture_running && acoustic_sensor.is_data_ready()) {
    AcousticFeatures features = acoustic_sensor.get_latest_features();
    if (features.impact_amplitude < MIN_TAP_PEAK) {
      // Electrical noise, not a tap — discard and re-arm silently.
      Serial.printf("[Guide] Noise ignored (amp=%.3f) — re-arming\n",
                    features.impact_amplitude);
      start_acoustic_listening(dynamic_adc_threshold);
    } else {
      Serial.printf("[Guide] TAP %u/%u DETECTED\n", current_tap_idx + 1,
                    total_taps);
      led_pet_tap_ok();

      // Apply a run-wide volume override (BLE ScanConfig) if present.
      if (bt.has_volume_override()) {
        ms_features_pending.volume_cm3 = bt.get_override_volume_cm3();
      }
      assemble_state_32d(tap_states[current_tap_idx], ms_features_pending,
                         features);                    // block-norm 32-D
      bt.notify_debug_state(tap_states[current_tap_idx],
                            features.impact_amplitude);
      current_tap_idx++;

      const Fruit32D *model = store.get_active_model_ptr();

      if (bt.get_mode() == MODE_DEBUG) {
        // Debug mode: always record raw waveform; classify only
        // if a model happens to be loaded.
        uint16_t raw_adc[512];
        uint16_t peak_adc;
        acoustic_sensor.capture_sampler_raw_window(raw_adc, &peak_adc);
        bt.send_raw_acoustic_waveform(raw_adc, peak_adc);
        if (model) {
          BiologicalStatus status =
              evaluate_fruit_ntap_32d(tap_states, current_tap_idx, *model);
          bt.notify_scan_result(status);
        }
        bt.notify_status_change("debug_captured");
        lcd_flash("Recorded", nullptr);
        tap_capture_running = false;
        current_tap_idx = 0;
        have_ms_features = false;
      } else if (!model) {
        Serial.println("[Guide] No active model — run aborted");
        bt.notify_status_change("no_model");
        led_pet_timeout();
        tap_capture_running = false;
        current_tap_idx = 0;
        have_ms_features = false;
      } else if (current_tap_idx < total_taps) {
        // Consensus run — collect more taps. Re-arm the piezo and show
        // progress; the vision snapshot stays unchanged from the cam scan.
        Serial.printf("[Guide] %u/%u — TAP AGAIN\n", current_tap_idx,
                      total_taps);
        char line1[16];
        snprintf(line1, sizeof(line1), "TAP %u/%u OK", current_tap_idx,
                 total_taps);
        lcd_flash(line1, "TAP AGAIN");
        start_acoustic_listening(dynamic_adc_threshold);
        tap_capture_running = true;
        tap_arm_start_ms = current_time;
      } else {
        // Final tap: fuse all collected states by median posterior.
        BiologicalStatus status =
            evaluate_fruit_ntap_32d(tap_states, total_taps, *model);
        bt.notify_scan_result(status);
        led_pet_result(status.primary_decision, status.is_anomaly);
        lcd_result(status.primary_decision, status.is_anomaly,
                   status.confidence);
        tap_capture_running = false;
        current_tap_idx = 0;
        have_ms_features = false;
      }
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
  // Two-stage ready confirmation, mirroring the inference flow: the first
  // `arm_acoustic` disarms and asks the user to place the fruit
  // (`place_on_piezo`); a second `arm_ready` confirms it sits on the piezo
  // before we actually listen. This avoids the timed-grace freeze the app
  // saw and guarantees the placement can't false-trigger the tap.

  // ── arm_ready: user confirms the fruit is placed → arm for real.
  if (bt.check_arm_ready_request()) {
    if (s_arm_stage == ArmStage::AWAITING_READY) {
      s_arm_stage = ArmStage::LISTENING;
      s_ready_deadline_ms = 0;
      is_acoustic_armed = true;
      arm_start_time = current_time;
      start_acoustic_listening(dynamic_adc_threshold);
      led_pet_armed();
      Serial.printf("[Acoustic] READY — TAP NOW (thresh=%.2f)\n",
                    dynamic_adc_threshold);
      bt.notify_status_change("acoustic_armed");
    } else {
      bt.notify_status_change("acoustic_already_armed");
    }
  }

  // ── arm_acoustic: asked to arm, but the fruit may not be placed yet.
  if (bt.check_arm_acoustic_request()) {
    if (acoustic_sensor.get_state() == STATE_DISARMED &&
        s_arm_stage != ArmStage::GRACE &&
        s_arm_stage != ArmStage::AWAITING_READY) {
      s_arm_stage = ArmStage::AWAITING_READY;
      s_ready_deadline_ms = current_time + READY_WAIT_MS;
      lcd_place_on_piezo();
      led_pet_place_on_piezo();
      Serial.printf("[Acoustic] ARM — place fruit, send arm_ready to TAP\n");
      bt.notify_status_change("place_on_piezo");
    } else {
      bt.notify_status_change("acoustic_already_armed");
    }
  }

  // No READY within the window: abandon instead of hanging waiting forever.
  if (s_arm_stage == ArmStage::AWAITING_READY && s_ready_deadline_ms != 0 &&
      current_time >= s_ready_deadline_ms) {
    s_arm_stage = ArmStage::IDLE;
    s_ready_deadline_ms = 0;
    bt.notify_status_change("timeout_disarmed");
    led_pet_timeout();
    lcd_timeout();
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
