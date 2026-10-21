#include "bt_manager.h"
#include "buzzer.h"
#include "camera_handler.h"
#include "dual_latent.h"
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

// ─── Never-empty tap guard (collection path) ───────────────────────────
// The tap-quality gate can reject every strike of a session (a mis-placed
// fruit, a soft tap, a model floor that is too high for this rig).  With no
// fallback the host waits for a window the device will never accept and the
// run hangs.  So the best rejected window is kept and streamed once the
// rejection budget is spent: a session always ends with at least one window.
constexpr uint8_t TAP_REJECT_BUDGET = 3;   // rejects before the best is used
static uint16_t s_best_rejected[512];
static uint16_t s_best_rejected_peak = 0;
static bool     s_best_rejected_valid = false;
static uint8_t  s_reject_count = 0;

// Inference guide flow: cam scan → fruit on piezo → tap → result.
bool     awaiting_placement = false;   // cam scan done, waiting for SCAN press
bool     tap_capture_running = false;  // piezo armed for the inference tap
uint32_t tap_arm_start_ms = 0;         // armed-at timestamp (tap timeout)
uint32_t placement_start_at = 0;
uint32_t scan_start_at = 0;            // millis() when the cam scan should run
ColorFeatures ms_features_pending;     // vision snapshot taken at cam scan
bool     have_ms_features = false;

// Multi-tap consensus: each tap contributes the raw per-tap acoustic features
// that feed the Dual-Latent modality engine. The Dual median-aggregates the
// manifolds first then makes a single decision (evaluate_fruit_dual), and a
// per-tap debug notify streams the exact dual-latent coords over BLE.
// (Repurposed: the legacy 32-dim state block is gone.)
AcousticFeatures tap_acoustics[MAX_DUAL_TAPS];
uint8_t  current_tap_idx = 0;          // how many taps collected so far
uint8_t  total_taps = 1;               // target from BLE ScanConfig (clamped)
uint32_t tap_debounce_until_ms = 0;    // suppress ringing residue after a tap

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

// ─── Level-stable debounce ────────────────────────────────────────────────
// The button contacts chatter: a press is a burst of edges over a few
// milliseconds, and a worn contact can keep bouncing for tens of
// milliseconds.  The old edge-only debounce only required the RELEASE to be
// stable, so a bounce whose low gap was longer than the debounce window
// produced TWO taps: the first armed the piezo, the second was queued as a
// fresh scan (the "it resets to a new scan" symptom).
//
// Here the level is accepted only after it has been steady for the whole
// window, so a chatter burst is ONE event, and a press that is released
// early is simply ignored.
struct DebouncedLevel {
  uint8_t  pin;
  bool     accepted;      // the debounced level
  bool     candidate;     // the raw level waiting to be confirmed
  uint32_t candidate_ms;  // when the candidate first appeared
  bool     primed;

  explicit DebouncedLevel(uint8_t p)
      : pin(p), accepted(false), candidate(false), candidate_ms(0), primed(false) {}

  bool read(uint32_t now) {
    const bool raw = (digitalRead(pin) == HIGH);
    if (!primed) {
      primed = true;
      candidate = raw;
      accepted = raw;
      candidate_ms = now;
      return accepted;
    }
    if (raw != candidate) {
      candidate = raw;
      candidate_ms = now;
    }
    if (raw != accepted && (now - candidate_ms) >= BUTTON_DEBOUNCE_MS) {
      accepted = raw;
    }
    return accepted;
  }
};

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
  // The armed window starves loop() (the sampler busy-waits on the other
  // core), so the emitters have to be silenced HERE as well — the loop's
  // discipline only runs between taps.
  led_indicator_set_held(true);
  camera_set_suspended(true);
  buzzer_set_muted(true);           // the buzzer is an emitter too
  acoustic_sensor.arm(threshold, ARM_SETTLE_MS * 1000UL);
  Serial.printf("[Guide] LISTENING (taps %u/%u) — waiting for a fresh tap "
                "(flush + settle %lu ms)\n",
                (unsigned)current_tap_idx, (unsigned)total_taps,
                (unsigned long)ARM_SETTLE_MS);
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
  camera_wake();
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
  camera_sleep_idle();
}

// Raw waveform debug capture: N windows streamed back-to-back over BLE.
// Per-JPEG BLE pump deadline for the ms_debug / ms_debug_diff streams: the loop
// exits early when the transfer completes, so this is only the safety ceiling
// for a slow/large JPEG (a 60KB frame at typical BLE throughput needs >3s, and
// cutting it short lets the next begin_transfer() wipe it mid-send).
constexpr uint32_t MS_JPEG_PUMP_MS = 15000;
static void run_raw_capture() {
  const uint8_t WINDOWS = bt.get_raw_capture_windows();
  constexpr uint32_t PUMP_BUDGET_MS = 120;   // BLE pump time per window
#ifdef CONFIG_IDF_TARGET_ESP32S3
  rgb_led_set_raw(255, 60, 0);               // orange = capture in progress
#endif
  static uint16_t raw_adc[512];   // 1 KB — keeps it off loopTask's stack
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

// The camera is NOT brought up at boot.  The OV3660 streams continuously once
// esp_camera_init runs, and the S3 has no working XCLK gate, so an eager boot
// probe leaves the sensor radiating for the whole session — a permanent noise
// source for the piezo.  Instead the camera is initialised lazily by
// camera_ensure_initialized() on the first capture that actually needs a
// picture, and suspended for every acoustic read.

// Tiny boot-stage tracer: flushes so a wedged boot shows exactly how far it got.
static void boot_stage(const char* s) {
  Serial.printf("[Boot] stage: %s\n", s);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  boot_stage("serial");
  // START and SCAN are merged onto one GPIO14 button (active-high).
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLDOWN);   // == SCAN_BUTTON_PIN (same pin)
  pinMode(CANCEL_BUTTON_PIN, INPUT_PULLDOWN);
  Serial.printf("[Button] idle levels: START=%d CANCEL=%d\n",
                digitalRead(BOOT_BUTTON_PIN), digitalRead(CANCEL_BUTTON_PIN));
  attachInterrupt(digitalPinToInterrupt(CANCEL_BUTTON_PIN), cancel_button_isr,
                  RISING);
  led_indicator_init();
  lcd_display_init();
  buzzer_init();
  boot_stage("led/lcd/buzzer");

  if (!acoustic_sensor.init()) {
    Serial.println("Failed to initialize Piezo system!");
    while (1) ;
  }
  boot_stage("acoustic");
  store.init();
  refresh_default_screen();              // badge the idle screen if a model
                                         // survived in NVS
  multispectral_load_calibration();
  boot_stage("cal");
  bt.init("Fruitipi", &store);
  boot_stage("ble");
  if (bt.get_mode() == MODE_DEBUG) {
    // DEBUG boot: acoustics-only, camera not needed for the capture path.
    // Skipping the probe entirely keeps the BLE + raw-waveform pipeline free
    // of the camera's SCCB/current-spike wedge risk on marginal power.
    Serial.println("[Main] DEBUG mode: camera probe skipped (acoustics-only).");
  } else {
    // The camera comes up lazily, on the first capture that needs a picture.
    // An acoustic-only session therefore never brings the sensor up at all,
    // and the board stays quiet for the piezo.
    Serial.println("[Main] BLE advertising live; camera is lazy (first use).");
  }
}

// The decoded model's parameters plus the strike site the operator tapped
// (0xFF = auto: the device picks the site whose anchor the feature lands
// nearest to).
static DualLatentParams dual_params_with_site(DualLatentParams p) {
  p.site_id = bt.get_site_id();
  return p;
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

  /// ── LED + camera discipline ───────────────────────────────────────
  // The WS2812 is driven by the RMT peripheral at ~800 kHz, so an animating
  // pattern is a CONTINUOUS digital emitter on the board and couples into
  // the piezo.  While the sampler is busy the animation is frozen: the LED
  // keeps its last colour and the RMT waveform stops entirely.
  //
  // The camera is the same story and worse: the OV3660 streams continuously
  // from esp_camera_init and the S3 has no working XCLK gate, so once the
  // sensor is up it radiates for the rest of the session.  It is therefore
  // brought up lazily (first capture that needs it) and suspended for every
  // acoustic read.
  const bool sampler_busy = acoustic_sensor.get_state() != STATE_DISARMED;
  lcd_set_quiet(sampler_busy);
  led_indicator_set_held(sampler_busy);
  camera_set_suspended(sampler_busy);
  buzzer_set_muted(sampler_busy);
  static uint32_t last_state_log_ms = 0;
  if (sampler_busy && current_time - last_state_log_ms > 2000) {
    last_state_log_ms = current_time;
    Serial.printf("[Quiet] armed: state=%d held=%d cam_susp=%d mute=%d\n",
                  (int)acoustic_sensor.get_state(),
                  (int)led_indicator_is_held(), (int)camera_is_suspended(),
                  (int)buzzer_is_muted());
  }

  /// ── 1. BLE connection cues ────────────────────────────────────────
  static bool last_ble_connected = false;
  bool now_connected = bt.is_connected();
  if (now_connected != last_ble_connected) {
    if (now_connected) {
      led_pet_connected();
      lcd_connected();
      buzzer_double(30);
    } else {
      led_pet_disconnected();
      lcd_disconnected();
      buzzer_beep(60);
    }
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

  // Level-stable debounce: the contacts chatter, and a bounce must never
  // read as two taps.
  static DebouncedLevel start_btn(BOOT_BUTTON_PIN);   // == SCAN_BUTTON_PIN
  const bool btn_now = start_btn.read(current_time);
  if (btn_now && !btn_prev) {
    btn_press_ms = current_time;
    btn_hold_done = false;
    buzzer_click();
  }
  if (btn_now && !btn_hold_done &&
      current_time - btn_press_ms >= MENU_HOLD_MS) {
    btn_hold_done = true;              // long press consumed, never a tap
    if (s_menu_open) {
      // The selector is already open, so the buttons belong to it: a hold
      // commits the browsed model.  This cannot steal a scan, because a scan
      // only starts when the selector is CLOSED.
      commit_menu_selection();
    } else if (awaiting_placement) {
      // A press while the fruit is being placed means CONFIRM, however long
      // it is held.  Without this a slightly long press opened the model
      // selector instead, and the selector owns the buttons — so the flow
      // could never reach the acoustic stage.
      boot_trigger = true;
      Serial.println("[Button] hold used as placement confirm");
    } else {
      // The START button is TAP-ONLY.  The hold used to open the model
      // selector, and that STOLE presses: a slightly long press opened the
      // selector, the selector then owns the buttons (a tap cycled a model
      // instead of scanning, and an app scan request was ignored), so the
      // button looked blocked.  The selector now lives on the CANCEL
      // button's hold, on a button that can never steal a scan.
      Serial.println("[Button] long press ignored (tap = scan)");
    }
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
  if (btn_now != btn_prev) {
    // Every real level transition on GPIO14, with the hold latch — this is
    // what tells a stuck-high idle (never edges, hold latches, taps die
    // silently) from a normal press/release pair.
    Serial.printf("[Button] START raw %d -> %d (hold_done=%d elapsed=%lu ms)\n",
                  (int)btn_prev, (int)btn_now, (int)btn_hold_done,
                  (unsigned long)(current_time - btn_press_ms));
  }
  btn_prev = btn_now;

  // DEBUG mode runs the same guide flow — it just records instead of
  // classifying (and skips the no-model gate below).
  if (bt.check_inference_request() &&
      (mode == MODE_INFERENCE || mode == MODE_DEBUG)) {
    // An explicit scan request must never be swallowed by an open selector:
    // close it and run the scan.
    if (s_menu_open) {
      close_model_menu(false);
      Serial.println("[Menu] closed by a scan request");
    }
    boot_trigger = true;
  }

  // CANCEL works everywhere. The ISR only stamps the press; the verdict
  // waits here so a press can grow into a hold (= selection reset). A
  // short tap cancels whatever is running / closes the selector.
  static bool     cancel_pending = false;
  static uint32_t cancel_press_ms = 0;
  static bool     cancel_hold_done = false;
  static uint32_t last_cancel_ms = 0;
  static bool     cancel_press_confirmed = false;
  if (s_isr_cancel) {
    s_isr_cancel = false;
    if (!cancel_pending && current_time - last_cancel_ms >= CANCEL_RATE_LIMIT_MS) {
      cancel_pending = true;
      cancel_press_ms = current_time;
      cancel_hold_done = false;
      cancel_press_confirmed = false;    // confirmed once the level holds
      last_cancel_ms = current_time;
    }
  }
  if (cancel_pending) {
    // Debounced level: a chattering release must not fire the verdict early.
    static DebouncedLevel cancel_btn(CANCEL_BUTTON_PIN);
    const bool cancel_down = cancel_btn.read(current_time);
    if (cancel_down) {
      // The level has been HIGH for the whole debounce window: a real press.
      if (!cancel_press_confirmed) {
        cancel_press_confirmed = true;
        cancel_press_ms = current_time;   // the press starts NOW
        buzzer_click();
      }
      if (!cancel_hold_done &&
          current_time - cancel_press_ms >= MENU_HOLD_MS) {
        cancel_hold_done = true;
        // The model selector moved here from the START button's hold, so a
        // scan press can never be stolen by the menu gesture.  In the
        // selector the same hold wipes the selection.
        if (s_menu_open) reset_model_selection();
        else             open_model_menu();
      }
    } else if (!cancel_press_confirmed) {
      // The pin never held HIGH long enough to be a press: a spike (this
      // pin is wired to 5 V and is prone to noise).  Drop it — a spike must
      // never abort a run.
      cancel_pending = false;
    } else if (!cancel_hold_done) {
      // Released after a CONFIRMED press: that is the verdict.
      Serial.println("[Button] CANCEL pressed");
      cancel_scan();                     // stop any run first...
      if (s_menu_open) close_model_menu(true);  // ...then leave the selector
      cancel_pending = false;            // released — verdict served
    } else {
      cancel_pending = false;            // released after a hold
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

  // A trigger that arrives while a run is already under way must be
  // DROPPED, not queued.  The guide flow is edge-driven: a leftover trigger
  // starts a FRESH scan the moment the current run finishes, which is what
  // made a double tap look like "it went back to a new scan" — the first tap
  // confirmed the placement and armed the piezo, the second was queued and
  // fired a new scan afterwards.
  //
  // `awaiting_placement` is deliberately NOT in this list: the second press
  // IS the placement confirmation and must still get through.
  if (tap_capture_running || scan_start_at != 0 || is_acoustic_armed) {
    if (boot_trigger || scan_requested) {
      Serial.println("[Button] tap ignored — a run is already in progress");
    }
    boot_trigger = false;
    scan_requested = false;
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

  // NOTE: the old "no active 32D model -> abort" gate is GONE. The inference
  // path always scores with the Dual-Latent modality engine
  // (evaluate_fruit_dual). With no uploaded blob the compiled-in defaults run;
  // a loaded DualModelWire blob (LCD/app "model") switches the params in.
  // A model is optional — it never blocks a scan.

  // Active-modality gate for the optical phase: MODALITY_OPTICS/DUAL enable
  // the camera; the compiled-in MODALITY_ACOUSTIC (optics w=0) skips it.
  DualLatentParams run_params = dual_params_default();
  if (const DualModelWire* m = store.get_active_model_ptr()) {
    run_params = dual_params_decode(*m);
  }
  // The strike site the operator tapped (the app's choice; 0xFF = auto, so
  // the device picks the site whose anchor the feature lands nearest to).
  run_params.site_id = bt.get_site_id();
  const bool optics_enabled = run_params.modality != MODALITY_ACOUSTIC;

  // Shared arm step, reached by BOTH the optics path (after a successful cam
  // scan) and the acoustics-only path (no camera beat at all): DEBUG auto-arms
  // the piezo; inference waits for the second press with the fruit on it.
  auto arm_run = [&]() {
    if (bt.get_mode() == MODE_DEBUG) {
      // Debug auto-arms with NO camera beat, so it must never reuse optics
      // captured by a previous inference scan (stale hue from an earlier dual
      // run would re-enable the contradiction guard / optics blend here).
      for (int i = 0; i < 8; i++) ms_features_pending.hue_histogram[i] = 0.0f;
      ms_features_pending.raw_rgb_means[0] = ms_features_pending.raw_rgb_means[1] =
          ms_features_pending.raw_rgb_means[2] = 0.0f;
      ms_features_pending.ambient_rgb_means[0] = ms_features_pending.ambient_rgb_means[1] =
          ms_features_pending.ambient_rgb_means[2] = 0.0f;
      total_taps = bt.get_target_tap_count();
      current_tap_idx = 0;
      start_acoustic_listening(dynamic_adc_threshold);
      tap_capture_running = true;
      tap_arm_start_ms = current_time;
      Serial.printf("[Guide] DEBUG: auto-armed piezo — TAP NOW (%u taps)\n",
                    total_taps);
      bt.notify_status_change("acoustic_armed");
      led_pet_armed();
    } else {
      total_taps = bt.get_target_tap_count();
      current_tap_idx = 0;
      awaiting_placement = true;
      placement_start_at = current_time;
      led_pet_place_on_piezo();
      lcd_place_on_piezo();
      Serial.printf("[Guide] PLACE ON PIEZO — %u taps, press SCAN when ready\n",
                    total_taps);
      bt.notify_status_change("place_on_piezo");
    }
  };

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

  // Phase 1 — optics-enabled runs hold the fruit in front of the camera, then
  // scan (Phase 2). Acoustics-only skips the camera beat and arms directly —
  // the fruit goes straight on the piezo (or DEBUG auto-arms), so the optical
  // phase is NEVER run for an acoustics-only model.
  if (guide_trigger && !tap_capture_running && !awaiting_placement &&
      scan_start_at == 0) {
    if (optics_enabled && is_camera_ready()) {
      led_pet_place_fruit();
      lcd_place_fruit();
      Serial.println("[Guide] PLACE FRUIT — hold fruit in front of camera...");
      scan_start_at = current_time + PLACE_FRUIT_HOLD_MS;
    } else if (!optics_enabled) {
      guide_trigger = false;
      scan_requested = false;
      // Neutral vision stub keeps the engine's volume/abbott term at the
      // bench-calibrated default for the acoustic manifold.
      memset(&ms_features_pending, 0, sizeof(ms_features_pending));
      ms_features_pending.volume_cm3 = VOLUME_DEFAULT_CM3;
      have_ms_features = true;
      arm_run();
    }
    // optics_enabled without a ready camera: leave the trigger pending; the
    // camera becoming ready (or CANCEL) resolves it.
  }

  // Phase 2 — the optical phase runs ONLY when the active modality enables
  // optics (MODALITY_OPTICS / MODALITY_DUAL); acoustics-only never reaches
  // here (Phase 1 armed the run directly). When optics runs, a confident
  // foveal foreground (fg_px >= MS_FOVEAL_FG_MIN) is required — fruit
  // detection FALSE cancels the whole run: no piezo arm, no result.
  if (scan_start_at != 0 && current_time >= scan_start_at) {
    scan_start_at = 0;
    scan_requested = false;              // trigger consumed by the scan

    uint32_t fg_px = 0;
    bool proceed = false;
    if (multispectral_capture_dual_shot(ms_features_pending, &fg_px)) {
      if (fg_px >= MS_FOVEAL_FG_MIN) {
        have_ms_features = true;
        proceed = true;
      } else {
        // Fruit-detection false -> cancel: no piezo arm, no result.
        Serial.printf("[Guide] FRUIT NOT DETECTED (fg=%u px < %u) — run cancelled\n",
                      fg_px, MS_FOVEAL_FG_MIN);
        bt.notify_status_change("fruit_not_detected");
        led_pet_camera_error();
        lcd_no_fruit();
      }
    } else {
      bt.notify_status_change("camera_error");
      lcd_camera_error();
    }

    if (proceed) {
      arm_run();
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

  if (tap_capture_running && acoustic_sensor.is_data_ready() &&
      current_time < tap_debounce_until_ms) {
    // Ringing residue from the previous tap inside the cooldown window: it is
    // NOT a new physical impact. Flush + re-arm clean so the NEXT data-ready
    // can only come from a fresh threshold crossing after settle. Logged so
    // the cooldown is observable, not silent.
    Serial.printf("[Guide] residue within cooldown %u ms — discarded, "
                  "re-armed clean for a fresh crossing\n",
                  (unsigned)TAP_MIN_INTERVAL_MS);
    start_acoustic_listening(dynamic_adc_threshold);
    tap_arm_start_ms = current_time;
  }

  if (tap_capture_running && acoustic_sensor.is_data_ready()) {
    AcousticFeatures features = acoustic_sensor.get_latest_features();
    if (features.impact_amplitude < MIN_TAP_PEAK) {
      // Electrical noise, not a tap — discard and re-arm silently.
      Serial.printf("[Guide] Noise ignored (amp=%.3f) — re-arming\n",
                    features.impact_amplitude);
      start_acoustic_listening(dynamic_adc_threshold);
    } else {
      // Pull the raw 512-sample window ONCE, up front. It feeds BOTH the
      // tap-quality gate (reject duds / grazes / truncated captures BEFORE
      // they pollute the consensus) AND, in DEBUG mode, the raw-wave stream.
      static uint16_t raw_adc[512];   // 1 KB — stays off loopTask's 8 KB stack
      uint16_t peak_adc = 0;
      acoustic_sensor.capture_sampler_raw_window(raw_adc, &peak_adc);

      // Quality thresholds: timing + ringdown are fixed universal gates; the
      // amplitude floor is per-fruit from the active model wire (Banana 160,
      // Mango 200; 0 -> device default 160) so it rides with the model blob.
      DualLatentParams tap_params = dual_params_default();
      if (const DualModelWire* m = store.get_active_model_ptr()) {
        tap_params = dual_params_decode(*m);
      }
      PiezoAcoustic::TapQualityResult tq =
          PiezoAcoustic::evaluate_tap_quality(raw_adc, tap_params.min_peak_adc);
      if (!tq.idx_ok || !tq.amp_ok || !tq.decay_ok) {
        // Bad tap — do NOT count. Flush + re-arm clean and wait for a
        // replacement tap, exactly like a first attempt. Visible retry cue.
        Serial.printf("[Guide] TAP REJECTED (idx=%u adc=%u tail=%.2f%s%s%s) "
                      "- TAP CLEANLY\n",
                      tq.peak_idx, tq.peak_adc, tq.tail_ratio,
                      tq.idx_ok ? "" : " [timing]", tq.amp_ok ? "" : " [amp]",
                      tq.decay_ok ? "" : " [ringdown]");
        lcd_tap_retry();
        start_acoustic_listening(dynamic_adc_threshold);
        tap_arm_start_ms = current_time;
      } else {
        Serial.printf("[Guide] TAP %u/%u DETECTED (since-arm %lu ms)\n",
                      current_tap_idx + 1, total_taps,
                      (unsigned long)(current_time - tap_arm_start_ms));
        led_pet_tap_ok();

        // Apply a run-wide volume override (BLE ScanConfig) if present.
        if (bt.has_volume_override()) {
          ms_features_pending.volume_cm3 = bt.get_override_volume_cm3();
        }
        tap_acoustics[current_tap_idx] = features;      // Dual-Latent per-tap input

        // Dual-native per-tap debug payload: the exact real-manifold coords +
        // sigmoid latents this tap contributes, computed with the same math.
        const float vol_norm =
            dual_volume_norm(tap_params, ms_features_pending.volume_cm3);
        DualTapCoords coords = dual_tap_coords(tap_params, features, vol_norm);
        bt.notify_debug_state(coords, features.impact_amplitude);
        current_tap_idx++;
        tap_debounce_until_ms = current_time + TAP_MIN_INTERVAL_MS;

        if (bt.get_mode() == MODE_DEBUG) {
          // Repurposed DEBUG mode: the tap STREAMS its raw waveform for capture
          // (data collection), then the SAME dual consensus runs below — DEBUG
          // is the dual system's data-collection/inspection mode, not a 32D one.
          bt.send_raw_acoustic_waveform(raw_adc, peak_adc);
        }

        if (current_tap_idx < total_taps) {
        // Consensus run — collect more taps. Re-arm the piezo; the vision
        // snapshot stays unchanged from the cam scan. Persistent progress:
        // the "TAP k/N OK" line is held as the base (survives the bus-quiet
        // re-arm), and a short "TAP AGAIN" cue flashes on top of it.
        Serial.printf("[Guide] %u/%u — TAP AGAIN\n", current_tap_idx,
                      total_taps);
        lcd_tap_progress(current_tap_idx, total_taps);
        lcd_tap_again(current_tap_idx, total_taps);
        start_acoustic_listening(dynamic_adc_threshold);
        tap_capture_running = true;
        tap_arm_start_ms = current_time;
      } else {
        // Final tap: acknowledge it, then classify with the Dual-Latent
        // modality engine (median-aggregated per-tap manifolds -> single
        // decision). A loaded DualModelWire blob controls the params (logs
        // src=blob); with no model the compiled-in defaults run (logs
        // src=default).
        lcd_tap_progress(current_tap_idx, total_taps);
        BiologicalStatus status;
        if (const DualModelWire* m = store.get_active_model_ptr()) {
          status = evaluate_fruit_dual(dual_params_with_site(dual_params_decode(*m)), tap_acoustics,
                                       total_taps, ms_features_pending);
        } else {
          status = evaluate_fruit_dual(tap_acoustics, total_taps,
                                       ms_features_pending);
        }
        bt.notify_scan_result(status);
        led_pet_result(status.primary_decision, status.is_anomaly);
        // The sampler auto-disarmed above, but the quiet flag is only
        // refreshed at the top of the next loop(): the cue is offered while
        // still muted and the buzzer flushes it there — same deferral the
        // LED overlay uses. It can never land inside an armed window.
        if (status.is_anomaly) buzzer_error(90);
        else                   buzzer_double(45);
        // LCD meter uses the DISPLAY-ONLY degaussed confidence so a decisive
        // result doesn't render as a flat full bar (raw ~100%). The decision
        // and BLE-published posterior are untouched.
        lcd_result(status.primary_decision, status.is_anomaly,
                   display_confidence(status));
        if (bt.get_mode() == MODE_DEBUG) {
          bt.notify_status_change("debug_captured");
          lcd_flash("Recorded", nullptr);
        }
        tap_capture_running = false;
        current_tap_idx = 0;
        have_ms_features = false;
      }
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
    if (multispectral_capture_dual_shot(ms_f)) {
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
      camera_wake();
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
          uint32_t pump_end = millis() + MS_JPEG_PUMP_MS;
          while (bt.is_transfer_active() && millis() < pump_end) {
            bt.service_transfer();
            delay(5);
          }
        }
      }
      rgb_led_set_raw(0, 0, 0);
      multispectral_restore_sensor();
      camera_sleep_idle();
      bt.notify_status_change("ms_debug_done");
    }
#else
    bt.notify_status_change("camera_error");
#endif
  }

  // Dual-shot debug: same capture as ms_capture, but also streams the two
  // original JPEGs (ambient then flash) plus a full timing/selection report.
  // Works on every target (no WS2812 dependency).
  if (bt.check_ms_debug_diff_request()) {
    if (!is_camera_ready()) {
      bt.notify_status_change("camera_error");
    } else {
      ColorFeatures d_f;
      DualShotDebugInfo d_info;
      camera_fb_t *d_amb = nullptr, *d_fsh = nullptr, *d_f2 = nullptr;
      if (!multispectral_capture_burst_debug(d_f, d_info, d_amb, d_fsh, d_f2, true)) {
        bt.notify_status_change("camera_error");
        if (d_amb) free_copied_frame(d_amb);
        if (d_fsh) free_copied_frame(d_fsh);
        if (d_f2) free_copied_frame(d_f2);
      } else {
        // Local-report path: the feature report is built on the HOST from the
        // three raw JPEGs, so only the tiny timing/meta notify is sent here.
        bt.notify_ms_debug_diff_meta(d_info);
        // Stream ambient, pump to completion, then F1, then F2.
        if (d_amb) {
          bt.send_ms_debug_jpeg(d_amb->buf, d_amb->len);
          uint32_t pump_end = millis() + MS_JPEG_PUMP_MS;
          while (bt.is_transfer_active() && millis() < pump_end) {
            bt.service_transfer();
            delay(5);
          }
          free_copied_frame(d_amb);
        }
        if (d_fsh) {
          bt.send_ms_debug_jpeg(d_fsh->buf, d_fsh->len);
          uint32_t pump_end = millis() + MS_JPEG_PUMP_MS;
          while (bt.is_transfer_active() && millis() < pump_end) {
            bt.service_transfer();
            delay(5);
          }
          free_copied_frame(d_fsh);
        }
        if (d_f2) {
          bt.send_ms_debug_jpeg(d_f2->buf, d_f2->len);
          uint32_t pump_end = millis() + MS_JPEG_PUMP_MS;
          while (bt.is_transfer_active() && millis() < pump_end) {
            bt.service_transfer();
            delay(5);
          }
          free_copied_frame(d_f2);
        }
        bt.notify_status_change("ms_debug_diff_done");
      }
    }
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
      static uint16_t raw_adc[512];   // 1 KB — keeps it off loopTask's stack
      uint16_t peak_adc;
      acoustic_sensor.capture_sampler_raw_window(raw_adc, &peak_adc);

      if (feats.impact_amplitude < MIN_TAP_PEAK) {
        // Electrical noise, not a tap — discard and re-arm silently.
        Serial.printf("[Acoustic] Noise ignored (amp=%.3f) — re-arming\n",
                      feats.impact_amplitude);
        arm_start_time = current_time;
        start_acoustic_listening(dynamic_adc_threshold);
      } else {
        // The same quality gate the inference path applies, so a collection
        // run stores the same windows the device would accept: a window whose
        // impact is not centred in the pre-trigger region, or that carries no
        // ring-down, is not a strike — it is the piee's static output
        // shifting (a DC step) or a scrape.  min_peak_adc = 0 because the
        // amplitude floor here is MIN_TAP_PEAK (the device's own noise
        // floor); the model's per-fruit floor belongs to the model, not to
        // the corpus being collected for it.
        PiezoAcoustic::TapQualityResult tq =
            PiezoAcoustic::evaluate_tap_quality(raw_adc, 0);
        if (!tq.idx_ok || !tq.decay_ok) {
          // Keep the BEST rejected window.  Without this a session whose
          // strikes all fail the gate can never complete: the host waits for
          // a tap that the device will never accept, and the run hangs until
          // the operator gives up.  The guard streams the best-so-far once
          // the rejection budget is spent, so a session always ends with at
          // least one window and the corpus records the *worst* case rather
          // than nothing.
          const bool better = !s_best_rejected_valid ||
                              tq.peak_adc > s_best_rejected_peak;
          if (better) {
            memcpy(s_best_rejected, raw_adc, sizeof(s_best_rejected));
            s_best_rejected_peak = tq.peak_adc;
            s_best_rejected_valid = true;
          }
          s_reject_count++;
          Serial.printf("[Acoustic] TAP REJECTED (idx=%u adc=%u tail=%.2f%s%s) "
                        "- TAP CLEANLY (reject %u/%u)\n",
                        tq.peak_idx, tq.peak_adc, tq.tail_ratio,
                        tq.idx_ok ? "" : " [timing]",
                        tq.decay_ok ? "" : " [ringdown]",
                        (unsigned)s_reject_count, (unsigned)TAP_REJECT_BUDGET);
          if (s_reject_count >= TAP_REJECT_BUDGET && s_best_rejected_valid) {
            // Budget spent: stream the best window we saw so the session
            // completes rather than hanging.
            Serial.printf("[Acoustic] reject budget spent - streaming the best "
                          "of %u rejected taps (adc=%u)\n",
                          (unsigned)s_reject_count, (unsigned)s_best_rejected_peak);
            bt.send_raw_acoustic_waveform(s_best_rejected, s_best_rejected_peak);
            s_reject_count = 0;
            s_best_rejected_valid = false;
            is_acoustic_armed = false;
            bt.notify_status_change("acoustic_captured");
            led_pet_tap_ok();
            lcd_tap_ok();
          } else {
            arm_start_time = current_time;
            start_acoustic_listening(dynamic_adc_threshold);
          }
        } else {
          s_reject_count = 0;
          s_best_rejected_valid = false;
          bt.send_raw_acoustic_waveform(raw_adc, peak_adc);
          is_acoustic_armed = false;
          bt.notify_status_change("acoustic_captured");
          led_pet_tap_ok();
          lcd_tap_ok();
        }
      }
    } else if (current_time - arm_start_time >= ARM_TIMEOUT_MS) {
      is_acoustic_armed = false;
      acoustic_sensor.disarm();
      bt.notify_status_change("timeout_disarmed");
      led_pet_timeout();
      lcd_timeout();
    }
  }

  buzzer_update();
  led_indicator_update();
  lcd_display_update();
  delay(10);
}
