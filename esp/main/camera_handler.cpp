#include "camera_handler.h"

static bool camera_hardware_ok = false;
static framesize_t s_working_size = FRAMESIZE_VGA;

static framesize_t apply_sensor_tuning(sensor_t* s, framesize_t frame_size);
static bool capture_boot_frame(framesize_t start);
static constexpr int CAMERA_WARMUP_DELAY_MS = 2000;

// AI-Thinker on-board white flash LED. High = on.
static constexpr uint8_t FLASH_LED_PIN = 4;
static constexpr int FLASH_SETTLE_DELAY_MS = 150;  // let AEC/AGC converge with flash on

static void flash_led(bool on) {
    digitalWrite(FLASH_LED_PIN, on ? HIGH : LOW);
}

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

    // Correct SCCB fields for the esp32-camera driver
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;

    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST;  // discard stale pending frames, return the newest

    if (psramFound()) {
        // SXGA is a reliable max for the OV2640 on this unit (UXGA/QXGA
        // overrun the DMA/PSRAM margin). Programmed once at init: never change
        // framesize at runtime (desyncs the JPEG/DMA pipeline) and never
        // re-init after de-init (GDMA channel is not rebound).
        config.frame_size   = FRAMESIZE_SXGA;
        config.jpeg_quality = 12;
        config.fb_count     = 1;  // no ring: every fb_get waits for a fresh frame
        config.fb_location  = CAMERA_FB_IN_PSRAM;
    } else {
        config.frame_size   = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count     = 1;
        config.fb_location  = CAMERA_FB_IN_DRAM;
    }

    return config;
}

static framesize_t apply_sensor_tuning(sensor_t* s, framesize_t frame_size) {
    if (!s) return FRAMESIZE_VGA;

    sensor_id_t* id = &s->id;
    camera_sensor_info_t* info = esp_camera_sensor_get_info(id);
    const char* name = info ? info->name : "UNKNOWN";

    // Resolution was already set at init; this is a no-op that keeps the size
    // in sync with s_working_size without touching the live pipeline.
    s->set_framesize(s, frame_size);
    s->set_quality(s, 12);
    s->set_saturation(s, 0);      // saturation >0 clipped channels -> magenta bands
    s->set_contrast(s, 0);
    s->set_brightness(s, 0);
    s->set_whitebal(s, 1);        // auto white balance on
    s->set_awb_gain(s, 1);        // AWB gain correction on
    s->set_gain_ctrl(s, 1);       // auto gain control on
    s->set_exposure_ctrl(s, 1);   // auto exposure control on
    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);

    Serial.printf("[CameraHandler] Sensor: %s (PID 0x%04X), frame %d.\n",
                  name, id ? id->PID : 0, (int)frame_size);
    return frame_size;
}

bool init_camera_subsystem() {
    pinMode(FLASH_LED_PIN, OUTPUT);
    flash_led(false);

    camera_config_t config = build_camera_config();

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CameraHandler] ❌ Error initializing camera: 0x%x\n", err);
        camera_hardware_ok = false;
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        Serial.println("[CameraHandler] ❌ No sensor handle after init.");
        esp_camera_deinit();
        camera_hardware_ok = false;
        return false;
    }

    s_working_size = FRAMESIZE_SXGA;

    // Single init at the working resolution. No set_framesize() at runtime
    // (desyncs the JPEG/DMA pipeline -> striped magenta) and no de-init /
    // re-init (GDMA channel is not rebound -> fb_get() NULL forever).
    if (s) {
        apply_sensor_tuning(s, s_working_size);
    }

    // Let AEC/AGC/AWB converge passively while the sensor streams. The single
    // frame buffer is overwritten continuously, so by the time the first frame
    // is requested the sensor has settled. No ring exists to serve stale frames.
    delay(CAMERA_WARMUP_DELAY_MS);

    // 1 Boot Test Capture with automatic step-down until a frame succeeds.
    camera_hardware_ok = capture_boot_frame(s_working_size);

    return camera_hardware_ok;
}

constexpr int CAPTURE_RETRY_ATTEMPTS = 6;
constexpr int CAPTURE_RETRY_DELAY_MS = 150;

static bool jpeg_has_valid_markers(const camera_fb_t* fb) {
    if (!fb || !fb->buf || fb->len < 4) return false;
    return fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
           fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
}

static bool capture_boot_frame(framesize_t start) {
    // Simple retry loop at the init size. No step-down: changing framesize or
    // re-initializing at runtime breaks this driver's JPEG/DMA pipeline.
    for (int attempt = 1; attempt <= CAPTURE_RETRY_ATTEMPTS; ++attempt) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb && jpeg_has_valid_markers(fb)) {
            Serial.printf("[CameraHandler] ✓ Boot Frame Success (%d bytes JPEG @ frame %d).\n",
                          fb->len, (int)start);
            esp_camera_fb_return(fb);
            s_working_size = start;
            return true;
        }
        if (fb) {
            Serial.printf("[CameraHandler] Boot frame had invalid markers (%d bytes, attempt %d/%d).\n",
                          fb->len, attempt, CAPTURE_RETRY_ATTEMPTS);
            esp_camera_fb_return(fb);
        } else {
            Serial.printf("[CameraHandler] esp_camera_fb_get() NULL at frame %d (attempt %d/%d).\n",
                          (int)start, attempt, CAPTURE_RETRY_ATTEMPTS);
        }
        delay(CAPTURE_RETRY_DELAY_MS);
    }
    Serial.println("[CameraHandler] ❌ Boot Frame Capture Failed!");
    return false;
}

camera_fb_t* capture_camera_frame() {
    if (!camera_hardware_ok) {
        Serial.println("[CameraHandler] Cannot capture: Camera hardware not ready!");
        return nullptr;
    }

    // Flash on before grabbing so AEC/AGC converge to a lit scene.
    flash_led(true);
    delay(FLASH_SETTLE_DELAY_MS);

    // Single-buffer camera: fb_get() waits for a freshly captured frame. Reset
    // the buffer first by consuming whatever stale frame may be sitting in it
    // (one grab only — a tight drain loop wedges the DMA), so the returned
    // frame is guaranteed to be a brand-new capture.
    camera_fb_t* w = esp_camera_fb_get();
    if (w) {
        esp_camera_fb_return(w);
    }

    camera_fb_t* fb = nullptr;
    for (int attempt = 1; attempt <= CAPTURE_RETRY_ATTEMPTS; ++attempt) {
        fb = esp_camera_fb_get();
        if (fb) {
            if (jpeg_has_valid_markers(fb)) {
                if (attempt > 1) {
                    Serial.printf("[CameraHandler] Captured on attempt %d.\n", attempt);
                }
                Serial.printf("[CameraHandler] JPEG markers OK (%d bytes).\n", fb->len);
                flash_led(false);
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

    flash_led(false);

    // Do NOT de-init/re-init here: this driver never rebinds the GDMA channel
    // after esp_camera_deinit() (gdma_disconnect error, fb_get() NULL forever),
    // which would brick the camera for the whole session. Report the error
    // instead; the boot self-test already validated the hardware.
    Serial.println("[CameraHandler] All capture retries failed.");
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
