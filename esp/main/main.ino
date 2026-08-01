#include <Arduino.h>
#include "sci_28d.h"
#include "fruit_store.h"
#include "bt_manager.h"
#include "extract_hues.h"
#include "piezo_acoustic.h"
#include "camera_handler.h"

constexpr uint8_t BOOT_BUTTON_PIN = 0;
constexpr uint32_t ARM_TIMEOUT_MS = 3000;

FruitStore store;
BTManager bt;
PiezoAcoustic acoustic_sensor(ADC1_CHANNEL_0);

// Dynamic Config
float dynamic_adc_threshold = 0.15f;
bool is_acoustic_armed = false;
uint32_t arm_start_time = 0;

void handle_laptop_capture_image_only() {
    Serial.println("[Laptop] Direct Camera Image Request...");
    if (!is_camera_ready()) {
        Serial.println("[Laptop] Camera not ready!");
        bt.notify_status_change("camera_error");
        return;
    }
    camera_fb_t* fb = capture_camera_frame();
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
    Serial.begin(115200);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    if (init_camera_subsystem()) {
        Serial.println("[Main] Camera ready for captures.");
    } else {
        Serial.println("[Main] Camera init FAILED!");
    }

    store.init();
    acoustic_sensor.init();
    bt.init("Fruitipi", &store);
}

void loop() {
    uint32_t current_time = millis();

    // ─── 1. HANDLE COMMANDS OVER BLE ─────────────────────────────
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
        Serial.printf("[Laptop] Acoustic Threshold updated to: %.2f\n", dynamic_adc_threshold);
    }

    if (bt.check_and_clear_cancel_request()) {
        is_acoustic_armed = false;
        bt.notify_status_change("disarmed");
    }

    // ─── 2. MONITOR ACOUSTIC SENSOR WHEN ARMED ────────────────────────
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
