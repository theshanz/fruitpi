#include "bt_manager.h"
#include "camera_handler.h"
#include "esp_camera.h"
#include "extract_hues.h"
#include "fruit_store.h"
#include "led_indicator.h"
#include "multispectral.h"
#include "piezo_acoustic.h"
#include "sci_28d.h"
#include <Arduino.h>
#include <cstdint>
#include <esp32-hal-gpio.h>
#include <sys/stat.h>
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "rgb_led.h"
#endif

constexpr uint8_t BOOT_BUTTON_PIN = 0;
constexpr uint8_t SCAN_BUTTON_PIN = 14;     // start scan (active-high, wired to 5V)
constexpr uint8_t CANCEL_BUTTON_PIN = 21;   // cancel scan (active-high, wired to 5V)
                                            // WARNING: do NOT use GPIO12 for this button —
                                            // it is CAM_PIN_Y6 (camera data line) and reads
                                            // as a permanently-pressed button.
constexpr uint32_t ARM_TIMEOUT_MS = ARM_TIMEOUT_US / 1000;
constexpr uint32_t PLACE_FRUIT_HOLD_MS = 800;

FruitStore store;
BTManager bt;
PiezoAcoustic acoustic_sensor(ADC1_CHANNEL_1);

// Dynamic Config
// Real fruit taps measure ~0.025-0.067 (5x weaker than direct board taps), so
// the default must be low; BLE set_threshold still overrides at runtime.
float dynamic_adc_threshold = 0.02f;
bool is_acoustic_armed = false;
uint32_t arm_start_time = 0;
bool capture_button_pressed = false;
bool are_we_in_middle_of_capture = false;

// Physical SCAN button request (same pipeline as the BLE inference_request).
bool scan_requested = false;

// Timestamp when the multispectral scan should start (after the place-fruit hold).
uint32_t scan_start_at = 0;

// ─── Physical button debounce ───
// NOTE: the user wired these buttons to 5V. ESP32-S3 GPIOs are not 5V
// tolerant (abs max ~3.6V) — reading works, but prefer rewiring to 3.3V or a
// divider. Active-high (pin goes HIGH while pressed) with internal pull-down.
struct PhysicalButton {
  uint8_t pin;
  bool last_state;
  uint32_t last_ms;
};

static bool button_pressed_edge(PhysicalButton &btn, uint32_t now) {
  constexpr uint32_t BOUNCE_MS = 50;
  bool raw = (digitalRead(btn.pin) == HIGH);
  bool edge = raw && !btn.last_state && (now - btn.last_ms >= BOUNCE_MS);
  if (edge) {
    btn.last_ms = now;
  }
  btn.last_state = raw;
  return edge;
}

PhysicalButton scan_btn = {SCAN_BUTTON_PIN, false, 0};
PhysicalButton cancel_btn = {CANCEL_BUTTON_PIN, false, 0};

// Shared abort for BLE cancel requests AND the physical cancel button.
// Idempotent: only flashes the LED / notifies BLE when a scan was actually
// in progress (prevents LED churn from stray/repeat cancel presses).
static void cancel_scan() {
  bool was_active = is_acoustic_armed || are_we_in_middle_of_capture ||
                    capture_button_pressed || scan_requested ||
                    (scan_start_at != 0);
  is_acoustic_armed = false;
  acoustic_sensor.disarm();
  are_we_in_middle_of_capture = false;
  capture_button_pressed = false;
  scan_requested = false;
  scan_start_at = 0;
  if (was_active) {
    bt.notify_status_change("disarmed");
    led_pet_disarmed();
  }
}

// Runtime multispectral calibration (BLE ms_config)

void handle_laptop_capture_image_only() {
  Serial.println("[Laptop] Direct Camera Image Request...");
  if (!is_camera_ready()) {
    Serial.println("[Laptop] Camera not ready!");
    bt.notify_status_change("camera_error");
    return;
  }
  camera_fb_t *fb = capture_camera_frame();
  Serial.printf("[Laptop] capture_camera_frame() -> %s\n", fb ? "OK" : "NULL");
  if (fb) {
    bt.send_raw_jpeg_stream(fb->buf, fb->len);
    release_camera_frame(fb);
    bt.notify_status_change("image_captured");
    led_pet_capturing();
  } else {
    bt.notify_status_change("camera_error");
    led_pet_camera_error();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(SCAN_BUTTON_PIN, INPUT_PULLDOWN);
  pinMode(CANCEL_BUTTON_PIN, INPUT_PULLDOWN);
  led_indicator_init();

  if (init_camera_subsystem()) {
    Serial.println("[Main] Camera ready for captures.");
  } else {
    Serial.println("[Main] Camera init FAILED!");
  }
  if (!acoustic_sensor.init()) {
    Serial.println("Failed to initialize Piezo system!");
    while (1)
      ;
  }
  store.init();
  multispectral_load_calibration();
  bt.init("Fruitipi", &store);
}

void loop() {
  uint32_t current_time = millis();
  SystemMode mode = bt.get_mode();

  static bool last_ble_connected = false;
  bool now_connected = bt.is_connected();
  if (now_connected && !last_ble_connected) {
    led_pet_connected();
  } else if (!now_connected && last_ble_connected) {
    led_pet_disconnected();
  }
  last_ble_connected = now_connected;

  if (bt.has_threshold_update()) {
    dynamic_adc_threshold = bt.get_updated_threshold();
    Serial.printf("[Laptop] Acoustic Threshold updated to: %.2f\n", dynamic_adc_threshold);
  }
/// 1.Trigger for inference
  constexpr uint32_t BOUNCE_MS = 50;
  static uint32_t last_btn_ms = 0;
  static bool last_btn_state = HIGH;

  bool btn_now = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  if (btn_now && !last_btn_state && (current_time - last_btn_ms >= BOUNCE_MS)) {
      capture_button_pressed = true;      // one press = one inference run
      last_btn_ms = current_time;
  }
  last_btn_state = btn_now;
/// 2.
    if(bt.check_inference_request() && mode == MODE_INFERENCE){
        capture_button_pressed = true;
    }

  /// Physical buttons — active-high, debounced edge detection.
  if (button_pressed_edge(scan_btn, current_time)) {
    scan_requested = true;
    Serial.println("[Button] SCAN pressed");
  }
  if (button_pressed_edge(cancel_btn, current_time)) {
    Serial.println("[Button] CANCEL pressed");
    cancel_scan();
  }

  bt.service_transfer();

//disarming from bluetooth
  if (bt.check_and_clear_cancel_request()) {
    cancel_scan();
  }


  // Physical SCAN button — mode-aware start.
  if (scan_requested && mode == MODE_DATA_COLLECTION) {
    // Data collection: arm the acoustic sampler only; a tap streams the
    // 512-sample waveform over BLE (is_acoustic_armed block below).
    if (acoustic_sensor.get_state() == STATE_DISARMED) {
      is_acoustic_armed = true;
      arm_start_time = current_time;
      acoustic_sensor.arm(dynamic_adc_threshold);
      Serial.printf("[Guide] TAP NOW (thresh=%.2f)\n", dynamic_adc_threshold);
      Serial.printf("[Button] ARM (thresh=%.2f) — tap now\n", dynamic_adc_threshold);
      bt.notify_status_change("acoustic_armed");
      led_pet_armed();
    }
    scan_requested = false;
  }

  if ((capture_button_pressed || scan_requested) && mode == MODE_INFERENCE) {

#if defined(SCI_28D_HARDCODED)
    // Hardcoded rule engine ignores the stored model — no model upload needed.
    const Fruit28D *model = store.get_active_model_ptr();  // may be null
    bool have_model = true;
#else
    const Fruit28D *model = store.get_active_model_ptr();
    bool have_model = (model != nullptr);
#endif
    if (have_model) {
      ColorFeatures c_f;

      // Guide phase 1 — "place fruit" hold before the scan.
      if (!are_we_in_middle_of_capture && scan_start_at == 0) {
        if (is_camera_ready()) {
          led_pet_place_fruit();
          Serial.println("[Guide] PLACE FRUIT — hold fruit in front of camera...");
          scan_start_at = current_time + PLACE_FRUIT_HOLD_MS;
        }
      }

      // Guide phase 2 — multispectral scan (R/G/B flashes), then arm ("tap now").
      if (scan_start_at != 0 && current_time >= scan_start_at) {
        scan_start_at = 0;
        if (is_camera_ready()) {
          if (multispectral_capture(c_f)) {
            acoustic_sensor.arm(dynamic_adc_threshold);
            are_we_in_middle_of_capture = true;
            led_pet_armed();
            Serial.printf("[Guide] TAP NOW (thresh=%.2f)\n", dynamic_adc_threshold);
            Serial.printf("[Acoustic] INFERENCE ARM (thresh=%.2f) — tap now\n",
                          dynamic_adc_threshold);
            bt.notify_status_change("acoustic_armed");
          } else {
            bt.notify_status_change("camera_error");
            are_we_in_middle_of_capture = false;
            capture_button_pressed = false;
            scan_requested = false;
          }
        } else {
          bt.notify_status_change("camera_error");
          capture_button_pressed = false;
          scan_requested = false;
        }
      }

      // Guide phase 3/4 — tap captured, then result.
      if (acoustic_sensor.is_data_ready() && are_we_in_middle_of_capture) {
        Serial.println("[Guide] TAP DETECTED — analyzing...");
        led_pet_tap_ok();
        AcousticFeatures features = acoustic_sensor.get_latest_features();
        float state[28];
        assemble_state_28d(state, c_f, features);
#if defined(SCI_28D_HARDCODED)
        static Fruit28D dummy_model = {};
        BiologicalStatus status = evaluate_fruit_single(state, dummy_model);
#else
        BiologicalStatus status = evaluate_fruit_single(state, *model);
#endif
        bt.notify_scan_result(status);
        led_pet_result(status.primary_decision, status.is_anomaly);

        //reset state
        are_we_in_middle_of_capture = false;
        capture_button_pressed = false;
        scan_requested = false;
      }
    }
  }

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
          // Pump until this JPEG finishes streaming before starting the next,
          // otherwise begin_transfer() wipes the buffer mid-send (same class
          // of bug we fixed in raw_capture).
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

  float ms_gain_r, ms_gain_g, ms_gain_b;
  int ms_aec;
  bool ms_ambient;
  if (bt.check_ms_config_request(&ms_gain_r, &ms_gain_g, &ms_gain_b, &ms_aec, &ms_ambient)) {
    multispectral_set_config(ms_gain_r, ms_gain_g, ms_gain_b, ms_aec, ms_ambient);
  }

  if (bt.check_arm_acoustic_request()) {
    // Debounce spam re-arms: only arm when the sampler is actually idle.
    // Re-arming mid-capture wipes the ring buffer and swallows the tap.
    if (acoustic_sensor.get_state() == STATE_DISARMED) {
      is_acoustic_armed = true;
      arm_start_time = current_time;
      acoustic_sensor.arm(dynamic_adc_threshold);   // wake the 8.8 kHz sampler
      Serial.printf("[Guide] TAP NOW (thresh=%.2f)\n", dynamic_adc_threshold);
      Serial.printf("[Acoustic] ARM (thresh=%.2f)\n", dynamic_adc_threshold);
      bt.notify_status_change("acoustic_armed");
      led_pet_armed();
    } else {
      bt.notify_status_change("acoustic_already_armed");
    }
  }

  if (bt.check_raw_capture_request()) {
    const uint8_t RAW_CAPTURE_WINDOWS = bt.get_raw_capture_windows();
    constexpr uint32_t RAW_CAPTURE_GAP_MS = 120;     // pump budget per window
    rgb_led_set_raw(255, 60, 0);                     // orange = capture in progress
    uint16_t raw_adc[512];
    uint16_t peak_adc;

    if (bt.is_raw_capture_trigger_mode()) {
      // Trigger-based capture: arm, wait for tap, return sampler's window
      // (64 pre-trigger + 448 post-trigger = consistent tap position)
      Serial.println("[RawCapture] Trigger mode: arm → tap → capture");
      for (int w = 0; w < RAW_CAPTURE_WINDOWS; w++) {
        // Arm piezo with threshold 0.02
        acoustic_sensor.arm(0.02f);

        // Wait for trigger (up to 5s)
        uint32_t arm_start = millis();
        while (acoustic_sensor.get_state() != STATE_DISARMED &&
               millis() - arm_start < 5000) {
          if (acoustic_sensor.is_data_ready()) {
            break;
          }
          delay(1);
        }

        // Check if we got a valid capture
        if (acoustic_sensor.is_data_ready()) {
          acoustic_sensor.disarm();
          // Return sampler's captured window (tap at sample 64)
          acoustic_sensor.capture_sampler_raw_window(raw_adc, &peak_adc);
          bt.send_raw_acoustic_waveform(raw_adc, peak_adc);
        } else {
          acoustic_sensor.disarm();
          // Timeout — send empty window
          memset(raw_adc, 0, sizeof(raw_adc));
          peak_adc = 0;
          bt.send_raw_acoustic_waveform(raw_adc, peak_adc);
        }

        // Pump TX engine
        uint32_t pump_end = millis() + RAW_CAPTURE_GAP_MS;
        while (bt.is_transfer_active() && millis() < pump_end) {
          bt.service_transfer();
          delay(5);
        }
      }
    } else {
      // Continuous capture (original mode)
      Serial.println("[RawCapture] Continuous mode: capture without trigger");
      for (int w = 0; w < RAW_CAPTURE_WINDOWS; w++) {
        acoustic_sensor.capture_raw_waveform(raw_adc, &peak_adc);
        bt.send_raw_acoustic_waveform(raw_adc, peak_adc);
        uint32_t pump_end = millis() + RAW_CAPTURE_GAP_MS;
        while (bt.is_transfer_active() && millis() < pump_end) {
          bt.service_transfer();
          delay(5);
        }
      }
    }

    rgb_led_set_raw(0, 0, 0);
    bt.notify_status_change("raw_capture_done");
  }

  if (is_acoustic_armed) {
    // The 8.8 kHz background sampler detects the tap (reliable for the
    // short, weak pulses produced by tapping a fruit) and captures the full
    // 512-sample window (64 pre-trigger + 448 post-trigger).
    if (acoustic_sensor.is_data_ready()) {

      Serial.println("[Guide] TAP DETECTED — captured");
      Serial.println("[Acoustic] Physical Tap Detected!");

      // Stream the sampler's captured window as raw ADC waveform
      uint16_t raw_adc[512];
      uint16_t peak_adc;
      acoustic_sensor.capture_sampler_raw_window(raw_adc, &peak_adc);
      bt.send_raw_acoustic_waveform(raw_adc, peak_adc);

      // Auto-disarm after capturing tap
      is_acoustic_armed = false;
      bt.notify_status_change("acoustic_captured");
      led_pet_tap_ok();

    }
    // 3-Second Timeout
    else if (current_time - arm_start_time >= ARM_TIMEOUT_MS) {
      is_acoustic_armed = false;
      acoustic_sensor.disarm();
      bt.notify_status_change("timeout_disarmed");
      led_pet_timeout();
    }
  }

  led_indicator_update();
  delay(10);
}
