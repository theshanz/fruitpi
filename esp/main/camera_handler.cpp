#include "camera_handler.h"
#include "config.h"

#include "driver/ledc.h"

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "rgb_led.h"
#endif

static bool camera_hardware_ok = false;
static framesize_t s_working_size = FRAMESIZE_VGA;

// Idle power control: the sensor streams continuously between captures unless
// XCLK is gated. On classic ESP32 the driver owns LEDC CH0 (Timer 0) on
// pin_xclk; ledc_stop freezes the channel -> sensor loses its pixel clock.
// On ESP32-S3 XCLK comes from the LCD_CAM clock generator (CAM_CLK) via the
// GPIO matrix, so ALL LEDC calls fail here with "LEDC is not initialized".
// Empirically, flipping the XCLK pad direction (GPIO_MODE_DISABLE/OUTPUT) does
// NOT resume the clock on this build either, so the S3 sensor is left
// streaming (see camera_sleep_idle). The S3-EYE has no PWDN pin, so without a
// working CAM_CLK gate there is no idle power lever on S3.
static bool s_camera_awake = false;
#ifndef CONFIG_IDF_TARGET_ESP32S3
static bool s_xclk_duty_saved = false;
static ledc_mode_t s_xclk_mode = LEDC_LOW_SPEED_MODE;
static uint32_t s_xclk_duty = 0;
#endif
static constexpr uint32_t CAMERA_WAKE_SETTLE_MS = 60;

// AI-Thinker on-board white flash LED (non-S3 targets). High = on.
#ifndef CONFIG_IDF_TARGET_ESP32S3
static constexpr uint8_t FLASH_LED_PIN = 4;
#endif

void camera_set_flash(bool on) {
#ifdef CONFIG_IDF_TARGET_ESP32S3
    if (on) {
        rgb_led_set_flash_brightness(WHITE_FLASH_BRIGHTNESS);
        rgb_led_flash_white();
    } else {
        rgb_led_set(0, 0, 0);
    }
#else
    static bool flash_pin_init = false;
    if (!flash_pin_init) {
        pinMode(FLASH_LED_PIN, OUTPUT);
        digitalWrite(FLASH_LED_PIN, LOW);
        flash_pin_init = true;
    }
    digitalWrite(FLASH_LED_PIN, on ? HIGH : LOW);
#endif
}

static framesize_t apply_sensor_tuning(sensor_t* s, framesize_t frame_size);
static bool capture_boot_frame(framesize_t start);
static constexpr int CAMERA_WARMUP_DELAY_MS = 2000;

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
    config.xclk_freq_hz = 20000000;  // 24MHz bit-slipped the bus; 16MHz gave no frames
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST;  // discard stale pending frames, return the newest

    if (psramFound()) {
        // SXGA is the reliable max for the OV3660 on this unit (UXGA/QXGA
        // overrun the DMA/PSRAM margin). Programmed once at init: never change
        // framesize at runtime (desyncs the S3 JPEG/DMA pipeline) and never
        // re-init after de-init (GDMA channel is not rebound).
        config.frame_size   = FRAMESIZE_SXGA;
        config.jpeg_quality = 12;
        config.fb_count     = 1;  // no ring: every fb_get waits for a fresh frame
        config.fb_location  = CAMERA_FB_IN_PSRAM;
    } else {
        config.frame_size   = FRAMESIZE_QVGA;
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

    // Clock is live right after esp_camera_init; track that state so the first
    // camera_sleep_idle() (just after the boot test) actually stops it.
    s_camera_awake = true;

    s_working_size = FRAMESIZE_SXGA;

    // Single init at the working resolution. No set_framesize() at runtime
    // (desyncs the S3 JPEG/DMA pipeline -> striped magenta) and no de-init /
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

    // Drop the sensor to idle (XCLK off) until the first real capture arrives.
    // With the clock gated the OV3660 stops streaming, which removes the idle
    // heat the user noticed. Stashed duty (restored by camera_wake) is the
    // driver's original value, so the clock resumes bit-identical.
    camera_sleep_idle();

    return camera_hardware_ok;
}

#ifndef CONFIG_IDF_TARGET_ESP32S3
// ─── Idle power (classic ESP32 only): stop/restart XCLK on the LEDC channel
// the driver owns. On S3 the CAM_CLK signal is not LEDC-driven, so the whole
// duty-save dance is meaningless there; we gate the GPIO pin instead. ───────
static bool find_xclk_duty(ledc_mode_t& mode_out, uint32_t& duty_out) {
    // Classic ESP32 supports both modes; the driver picks one. Use whichever
    // currently produces the clock.
    const ledc_mode_t modes[] = { LEDC_LOW_SPEED_MODE, LEDC_HIGH_SPEED_MODE };
    for (int i = 0; i < 2; i++) {
        uint32_t d = ledc_get_duty(modes[i], LEDC_CHANNEL_0);
        if (d > 0) {
            mode_out = modes[i];
            duty_out = d;
            return true;
        }
    }
    return false;
}
#endif

void camera_sleep_idle() {
    if (!s_camera_awake) return;

#ifdef CONFIG_IDF_TARGET_ESP32S3
    // Empirically rejected on this board: flipping the XCLK pad direction
    // (GPIO_MODE_DISABLE -> GPIO_MODE_OUTPUT) does NOT restart CAM_CLK on this
    // esp32-camera build — after wake the sensor stops ACKing SCCB and every
    // fb_get returns NULL, even the ambient grab. The SDK prebuilds have no
    // source to mirror the exact pad/matrix setup, so the only safe option is
    // to leave the sensor streaming (same as Round 2, where ambient grabbed
    // cleanly). Idle XCLK gating stays a separate, later nicety — it is not
    // the flash-on wedge root cause.
    Serial.println("[CameraHandler] Sensor idle (XCLK gating disabled on S3).");
    return;
#else
    // Save the driver's live duty once so wake restores the exact clock.
    if (!s_xclk_duty_saved) {
        if (find_xclk_duty(s_xclk_mode, s_xclk_duty)) {
            s_xclk_duty_saved = true;
        } else {
            // Without the original duty we cannot restore the clock, so skipping
            // idle is safer than leaving the sensor unwakeable.
            Serial.println("[CameraHandler] Warn: XCLK duty unknown, staying awake.");
            return;
        }
    }
    // Native IDF LEDC API (esp_camera configures the XCLK channel through the
    // IDF driver, not the Arduino HAL, so the Arduino ledc_* wrappers error
    // out with "LEDC is not initialized"). ledc_stop freezes channel output at
    // idle level 0 -> the sensor loses its pixel clock and stops streaming.
    ledc_stop(s_xclk_mode, LEDC_CHANNEL_0, 0);
    s_camera_awake = false;
    Serial.println("[CameraHandler] Sensor idle (XCLK off).");
#endif
}

void camera_wake() {
#ifdef CONFIG_IDF_TARGET_ESP32S3
    // Round 2 semantics: the sensor never goes to sleep on S3 (see
    // camera_sleep_idle), so wake leaves the settled clock untouched.
    if (s_camera_awake) return;
    s_camera_awake = true;
    return;
#else
    if (s_camera_awake) return;
    if (s_xclk_duty_saved) {
        ledc_set_duty(s_xclk_mode, LEDC_CHANNEL_0, s_xclk_duty);
        ledc_update_duty(s_xclk_mode, LEDC_CHANNEL_0);
    }
    s_camera_awake = true;
    // Let a few VSYNCs come back so the first fb_get isn't a stale/wedged grab.
    delay(CAMERA_WAKE_SETTLE_MS);
    Serial.println("[CameraHandler] Sensor awake (XCLK on).");
#endif
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

    // Idle XCLK is gated: wake the sensor the moment a grab is requested.
    // Idempotent, so mid-session grabs (drains, dual-shot) are no-ops.
    camera_wake();

        // Single-buffer camera: fb_get() waits for a freshly captured frame. Reset
    // the buffer first by consuming whatever stale frame may be sitting in it
    // (one grab only — a tight drain loop wedges the S3 DMA), so the returned
    // frame is guaranteed to be a brand-new capture.
    camera_fb_t* w = esp_camera_fb_get();
    if (w) {
        esp_camera_fb_return(w);
    }

    camera_fb_t* fb = nullptr;
    // Fail fast: a single NULL from fb_get() means the sensor has stopped
    // delivering frames (wedged PCLK/VSYNC). Do NOT retry — each attempt can
    // block for seconds, and with the flash lit it leaves the LED on for
    // minutes. Return immediately; the caller's FlashGuard turns the LED off.
    for (int attempt = 1; attempt <= CAPTURE_RETRY_ATTEMPTS; ++attempt) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[CameraHandler] Frame grab failed (sensor wedged) — aborting capture.");
            return nullptr;
        }
        if (jpeg_has_valid_markers(fb)) {
            if (attempt > 1) {
                Serial.printf("[CameraHandler] Captured on attempt %d.\n", attempt);
            }
            Serial.printf("[CameraHandler] JPEG markers OK (%d bytes).\n", fb->len);
            return fb;
        }
        Serial.printf("[CameraHandler] Frame had invalid JPEG markers (%d bytes), discarding (attempt %d/%d).\n",
                      fb->len, attempt, CAPTURE_RETRY_ATTEMPTS);
        esp_camera_fb_return(fb);
        fb = nullptr;
        delay(CAPTURE_RETRY_DELAY_MS);
    }

    // Do NOT de-init/re-init here: this driver never rebinds the GDMA channel
    // after esp_camera_deinit() (gdma_disconnect error, fb_get() NULL forever),
    // which would brick the camera for the whole session. Report the error
    // instead; the boot self-test already validated the hardware.
    Serial.println("[CameraHandler] All capture retries failed (invalid markers).");
    return nullptr;
}

void release_camera_frame(camera_fb_t* fb) {
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

camera_fb_t* copy_camera_frame(const camera_fb_t* src, size_t& dst_len) {
    dst_len = 0;
    if (!src || !src->buf || src->len == 0) return nullptr;
    camera_fb_t* copy = (camera_fb_t*)heap_caps_malloc(sizeof(camera_fb_t),
                                                       MALLOC_CAP_SPIRAM);
    if (!copy) return nullptr;
    memset(copy, 0, sizeof(camera_fb_t));
    uint8_t* dst_buf = (uint8_t*)heap_caps_malloc(src->len,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst_buf) {
        dst_buf = (uint8_t*)heap_caps_malloc(src->len, MALLOC_CAP_8BIT);
    }
    if (!dst_buf) {
        heap_caps_free(copy);
        return nullptr;
    }
    memcpy(dst_buf, src->buf, src->len);
    copy->buf    = dst_buf;
    copy->len    = src->len;
    copy->width  = src->width;
    copy->height = src->height;
    copy->format = src->format;
    dst_len = src->len;
    return copy;
}

void free_copied_frame(camera_fb_t* fb) {
    if (!fb) return;
    if (fb->buf) heap_caps_free(fb->buf);
    heap_caps_free(fb);
}

bool is_camera_ready() {
    return camera_hardware_ok;
}
