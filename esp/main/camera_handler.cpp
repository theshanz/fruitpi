#include "camera_handler.h"

static bool camera_hardware_ok = false;

static camera_config_t build_camera_config() {
    camera_config_t config;
    memset(&config, 0, sizeof(config));

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_PIN_Y2;
    config.pin_d1       = CAM_PIN_Y3;
    config.pin_d2       = CAM_PIN_Y4;
    config.pin_d3       = CAM_PIN_Y5;
    config.pin_d4       = CAM_PIN_Y6;
    config.pin_d5       = CAM_PIN_Y7;
    config.pin_d6       = CAM_PIN_Y8;
    config.pin_d7       = CAM_PIN_Y9;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;

    // Correct SCCB fields for ESP32-S3 driver
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;

    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    if (psramFound()) {
        // VGA (640x480) is optimal for fast Bluetooth transmission
        config.frame_size   = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count     = 2;
        config.fb_location  = CAMERA_FB_IN_PSRAM;
    } else {
        config.frame_size   = FRAMESIZE_QVGA;
        config.jpeg_quality = 15;
        config.fb_count     = 1;
        config.fb_location  = CAMERA_FB_IN_DRAM;
    }

    return config;
}

bool init_camera_subsystem() {
    camera_config_t config = build_camera_config();

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CameraHandler] ❌ Error initializing camera: 0x%x\n", err);
        camera_hardware_ok = false;
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 0);
        s->set_hmirror(s, 0);
    }

    // Perform 1 Boot Test Capture to verify hardware bus stability
    camera_fb_t* test_fb = esp_camera_fb_get();
    if (test_fb) {
        Serial.printf("[CameraHandler] ✓ Boot Frame Success (%d bytes JPEG).\n", test_fb->len);
        esp_camera_fb_return(test_fb);
        camera_hardware_ok = true;
    } else {
        Serial.println("[CameraHandler] ❌ Boot Frame Capture Failed!");
        camera_hardware_ok = false;
    }

    return camera_hardware_ok;
}

constexpr int CAPTURE_RETRY_ATTEMPTS = 6;
constexpr int CAPTURE_RETRY_DELAY_MS = 150;

static bool jpeg_has_valid_markers(const camera_fb_t* fb) {
    if (!fb || !fb->buf || fb->len < 4) return false;
    return fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
           fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
}

static uint32_t fast_hash(const uint8_t* buf, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= buf[i];
        h *= 16777619u;
    }
    return h;
}

camera_fb_t* capture_camera_frame() {
    if (!camera_hardware_ok) {
        Serial.println("[CameraHandler] Cannot capture: Camera hardware not ready!");
        return nullptr;
    }

    // Detect the S3 DMA stall that returns the SAME buffer/content repeatedly
    // instead of NULL: identical buffer pointer + length + content hash == stale.
    static camera_fb_t* last_fb = nullptr;
    static uint32_t last_len = 0;
    static uint32_t last_hash = 0;

    camera_fb_t* fb = nullptr;
    for (int attempt = 1; attempt <= CAPTURE_RETRY_ATTEMPTS; ++attempt) {
        fb = esp_camera_fb_get();
        if (fb) {
            if (fb == last_fb && fb->len == last_len && fast_hash(fb->buf, fb->len) == last_hash) {
                Serial.printf("[CameraHandler] Stale frame detected (%d bytes, identical to previous). Restarting camera...\n",
                              fb->len);
                esp_camera_fb_return(fb);
                fb = nullptr;
                break;  // pipeline is stuck returning old data; retries won't help
            }
            if (jpeg_has_valid_markers(fb)) {
                if (attempt > 1) {
                    Serial.printf("[CameraHandler] Captured on attempt %d.\n", attempt);
                }
                last_fb = fb;
                last_len = fb->len;
                last_hash = fast_hash(fb->buf, fb->len);
                Serial.printf("[CameraHandler] JPEG markers OK (%d bytes).\n", fb->len);
                return fb;
            }
            Serial.printf("[CameraHandler] Frame had invalid JPEG markers (%d bytes), discarding (attempt %d/%d).\n",
                          fb->len, attempt, CAPTURE_RETRY_ATTEMPTS);
            esp_camera_fb_return(fb);
            fb = nullptr;
        } else {
            Serial.printf("[CameraHandler] esp_camera_fb_get() returned NULL (attempt %d/%d).\n",
                          attempt, CAPTURE_RETRY_ATTEMPTS);
        }
        delay(CAPTURE_RETRY_DELAY_MS);
    }

    // Hard-wedged/stale DMA pipeline: full re-init to restart the sensor + DMA chain.
    Serial.println("[CameraHandler] All retries failed, re-initializing camera...");
    esp_camera_deinit();
    camera_config_t config = build_camera_config();
    esp_err_t err = esp_camera_init(&config);
    if (err == ESP_OK) {
        sensor_t* s = esp_camera_sensor_get();
        if (s) {
            s->set_vflip(s, 0);
            s->set_hmirror(s, 0);
        }
        fb = esp_camera_fb_get();
        if (fb && jpeg_has_valid_markers(fb)) {
            last_fb = fb;
            last_len = fb->len;
            last_hash = fast_hash(fb->buf, fb->len);
            Serial.printf("[CameraHandler] Recovered after re-init (%d bytes JPEG).\n", fb->len);
            return fb;
        }
        if (fb) {
            Serial.println("[CameraHandler] Recovered frame had invalid JPEG markers, discarding.");
            esp_camera_fb_return(fb);
        }
    }
    Serial.printf("[CameraHandler] Camera re-init failed: 0x%x\n", err);
    camera_hardware_ok = false;
    return nullptr;
}

void release_camera_frame(camera_fb_t* fb) {
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

bool is_camera_ready() {
    return camera_hardware_ok;
}
