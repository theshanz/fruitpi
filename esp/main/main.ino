#include "bt_manager.h"
#include "camera_handler.h"
#include "esp_camera.h"
#include "extract_hues.h"
#include "fruit_store.h"
#include "piezo_acoustic.h"
#include "sci_28d.h"
#include <Arduino.h>
#include <cstdint>
#include <random>
#include <sys/stat.h>
#include "soc/rtc_cntl_reg.h"

constexpr uint32_t ARM_TIMEOUT_MS = 3000;

FruitStore store;
BTManager bt;
PiezoAcoustic acoustic_sensor(ADC1_CHANNEL_5);

// Dynamic Config
float dynamic_adc_threshold = 0.15f;
bool is_acoustic_armed = false;
uint32_t arm_start_time = 0;
bool capture_button_pressed = false;
bool are_we_in_middle_of_capture = false;

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
  } else {
    bt.notify_status_change("camera_error");
  }
}

void setup() {
  // TEST band-aid: disable the brownout detector (power is marginal via CH340).
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);

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
  bt.init("Fruitipi", &store);
}

void loop() {
  uint32_t current_time = millis();
  SystemMode mode = bt.get_mode();

  float trigger_threshold = 0.15f;
  if (bt.has_threshold_update()) {
    trigger_threshold = bt.get_updated_threshold();
  }
/// 1.Trigger for inference
  if (bt.check_inference_request() && mode == MODE_INFERENCE) {
    capture_button_pressed = true;
  }

  bt.service_transfer();

//disarming from bluetooth
  if (bt.check_and_clear_cancel_request()) {
    is_acoustic_armed = false;
    acoustic_sensor.disarm();
    are_we_in_middle_of_capture = false;    // clear in-flight inference state
    capture_button_pressed = false;
    bt.notify_status_change("disarmed");
  }


  if (capture_button_pressed && mode == MODE_INFERENCE) {

    const Fruit28D *model = store.get_active_model_ptr();
    if (model) {
      static ColorFeatures c_f;
      if (is_camera_ready() && !are_we_in_middle_of_capture) {
        // Taking the visual data
        camera_fb_t *fb = capture_camera_frame();
        c_f = HueExtractor::process_frame(fb);
        release_camera_frame(fb);
        acoustic_sensor.arm(trigger_threshold);
        are_we_in_middle_of_capture = true;
      }
      if (acoustic_sensor.is_data_ready() && are_we_in_middle_of_capture) {
        AcousticFeatures features = acoustic_sensor.get_latest_features();
        float state[28];
        assemble_state_28d(state, c_f, features);
        BiologicalStatus status = evaluate_fruit_single(state, *model);
        bt.notify_scan_result(status);

        //reset state
        are_we_in_middle_of_capture = false;
        capture_button_pressed = false;
      }
    }
  }

  if (bt.check_capture_image_request()) {
    handle_laptop_capture_image_only();
  }

  if (bt.check_arm_acoustic_request()) {
    is_acoustic_armed = true;
    arm_start_time = current_time;
    bt.notify_status_change("acoustic_armed");
  }

  if (bt.has_threshold_update()) {
    dynamic_adc_threshold = bt.get_updated_threshold();
    Serial.printf("[Laptop] Acoustic Threshold updated to: %.2f\n",
                  dynamic_adc_threshold);
  }

  if (is_acoustic_armed) {

    // Poll piezo using the dynamic threshold
    if (acoustic_sensor.check_impact_detected(dynamic_adc_threshold)) {

      Serial.println("[Acoustic] Physical Tap Detected!");

      // Capture and stream raw 512 ADC time-domain waveform
      uint16_t raw_adc[512];
      uint16_t peak_adc;
      acoustic_sensor.capture_raw_waveform(raw_adc, &peak_adc);
      bt.send_raw_acoustic_waveform(raw_adc, peak_adc);

      // Auto-disarm after capturing tap
      is_acoustic_armed = false;
      bt.notify_status_change("acoustic_captured");

    }
    // 3-Second Timeout
    else if (current_time - arm_start_time >= ARM_TIMEOUT_MS) {
      is_acoustic_armed = false;
      bt.notify_status_change("timeout_disarmed");
    }
  }

  delay(10);
}
