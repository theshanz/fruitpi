// multispectral.cpp — Active Multispectral Imaging (R/G/B sequential flash)
#include <Arduino.h>
#include <cstring>

#include "multispectral.h"
#include "camera_handler.h"
#include "rgb_led.h"
#include "led_indicator.h"
#include "tjpgd.h"
#include "nvs_flash.h"
#include "nvs.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)

// ─── Tuning Constants ─────────────────────────────────────────────
static constexpr int    MS_DECODE_SCALE        = 2;     // 1/4: SXGA 1280x1024 -> 320x256
static constexpr int    MS_FIXED_GAIN          = 5;     // locked AGC gain
static constexpr int    MS_FIXED_AEC           = 30;    // locked AEC value (calibrated on white card)
static constexpr int    MS_SYNC_DISCARD_FRAMES = 2;     // frames discarded after LED color change so the kept
                                                        // frame's exposure provably happens under that LED
static constexpr float  MS_CM_PER_PIXEL_FULL   = 0.05f; // cm/px calibration at full resolution
static constexpr uint16_t MS_MAX_W             = 320;   // max decoded width (scale 2)
static constexpr uint16_t MS_MAX_H             = 256;   // max decoded height
static constexpr float  MS_CH_GAIN_R           = 1.16f; // white-card calibrated per-channel balance
static constexpr float  MS_CH_GAIN_G           = 0.90f;
static constexpr float  MS_CH_GAIN_B           = 0.58f;
static constexpr bool   MS_AMBIENT_DEFAULT     = true;  // subtract no-flash baseline before gains

static constexpr const char* MS_NVS_NAMESPACE  = "ms_calib";
static constexpr const char* MS_NVS_KEY_GR     = "gain_r";
static constexpr const char* MS_NVS_KEY_GG     = "gain_g";
static constexpr const char* MS_NVS_KEY_GB     = "gain_b";
static constexpr const char* MS_NVS_KEY_AEC    = "aec";
static constexpr const char* MS_NVS_KEY_AMB    = "amb_en";

// ─── Runtime calibration state (NVS-backed, override via BLE ms_config) ──
static float s_gain_r = MS_CH_GAIN_R;
static float s_gain_g = MS_CH_GAIN_G;
static float s_gain_b = MS_CH_GAIN_B;
static int   s_aec    = MS_FIXED_AEC;
static bool  s_ambient_enabled = MS_AMBIENT_DEFAULT;

void multispectral_set_config(float gain_r, float gain_g, float gain_b, int aec, bool ambient) {
    s_gain_r = gain_r;
    s_gain_g = gain_g;
    s_gain_b = gain_b;
    if (aec > 0) s_aec = aec;
    s_ambient_enabled = ambient;

    nvs_handle_t h;
    if (nvs_open(MS_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, MS_NVS_KEY_GR, (int32_t)(s_gain_r * 1000.0f + 0.5f));
        nvs_set_i32(h, MS_NVS_KEY_GG, (int32_t)(s_gain_g * 1000.0f + 0.5f));
        nvs_set_i32(h, MS_NVS_KEY_GB, (int32_t)(s_gain_b * 1000.0f + 0.5f));
        nvs_set_i32(h, MS_NVS_KEY_AEC, s_aec);
        nvs_set_u8(h, MS_NVS_KEY_AMB, s_ambient_enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    } else {
        Serial.println("[MS] config: NVS open failed (running RAM-only).");
    }
    Serial.printf("[MS] config: gain=(%.2f,%.2f,%.2f) aec=%d ambient=%d\n",
                  s_gain_r, s_gain_g, s_gain_b, s_aec, s_ambient_enabled ? 1 : 0);
}

void multispectral_load_calibration() {
    nvs_handle_t h;
    if (nvs_open(MS_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        Serial.println("[MS] calib: no NVS overrides, using baked defaults.");
        return;
    }
    int32_t aec = s_aec;
    uint8_t amb = s_ambient_enabled ? 1 : 0;
    bool any = false;
    int32_t gr_i = 0, gg_i = 0, gb_i = 0;
    if (nvs_get_i32(h, MS_NVS_KEY_GR, &gr_i) == ESP_OK && gr_i > 0) { s_gain_r = gr_i / 1000.0f; any = true; }
    if (nvs_get_i32(h, MS_NVS_KEY_GG, &gg_i) == ESP_OK && gg_i > 0) { s_gain_g = gg_i / 1000.0f; any = true; }
    if (nvs_get_i32(h, MS_NVS_KEY_GB, &gb_i) == ESP_OK && gb_i > 0) { s_gain_b = gb_i / 1000.0f; any = true; }
    if (nvs_get_i32(h, MS_NVS_KEY_AEC, &aec) == ESP_OK && aec > 0) { s_aec = aec; any = true; }
    if (nvs_get_u8(h, MS_NVS_KEY_AMB, &amb) == ESP_OK) { s_ambient_enabled = amb != 0; any = true; }
    nvs_close(h);
    Serial.printf("[MS] calib: gain=(%.2f,%.2f,%.2f) aec=%d ambient=%d\n",
                  s_gain_r, s_gain_g, s_gain_b, s_aec, s_ambient_enabled ? 1 : 0);
    if (!any) Serial.println("[MS] calib: keys absent, using baked defaults.");
}

// ─── TJpgDec I/O ──────────────────────────────────────────────────
struct MsStream {
    const uint8_t* data;
    size_t         size;
    size_t         pos;
};

static size_t ms_in_func(JDEC* jd, uint8_t* buff, size_t ndata) {
    MsStream* s = (MsStream*)jd->device;
    size_t avail = s->size - s->pos;
    size_t n = ndata < avail ? ndata : avail;
    if (buff) {
        memcpy(buff, s->data + s->pos, n);
    }
    s->pos += n;
    return n;
}

static uint16_t* g_out = nullptr;
static uint16_t  g_out_w = 0;

static int ms_out_func(JDEC* jd, void* bitmap, JRECT* rect) {
    (void)jd;
    const uint16_t* src = (const uint16_t*)bitmap;
    uint16_t* dst = g_out + (size_t)rect->top * g_out_w + rect->left;
    size_t nx = (size_t)rect->right - rect->left + 1;
    size_t ny = (size_t)rect->bottom - rect->top + 1;
    for (size_t y = 0; y < ny; y++) {
        memcpy(dst, src, nx * sizeof(uint16_t));
        dst += g_out_w;
        src += nx;
    }
    return 1;
}

// ─── Buffers (PSRAM) ──────────────────────────────────────────────
static uint16_t* s_buf_r = nullptr;  // RGB565 under red flash
static uint16_t* s_buf_g = nullptr;  // RGB565 under green flash
static uint16_t* s_buf_b = nullptr;  // RGB565 under blue flash (reused as synth output)
static uint16_t* s_buf_a = nullptr;  // RGB565 no-flash ambient baseline

static bool ensure_buffers() {
    if (s_buf_r) return true;
    const size_t px = (size_t)MS_MAX_W * MS_MAX_H;
    s_buf_r = (uint16_t*)ps_malloc(px * sizeof(uint16_t));
    s_buf_g = (uint16_t*)ps_malloc(px * sizeof(uint16_t));
    s_buf_b = (uint16_t*)ps_malloc(px * sizeof(uint16_t));
    s_buf_a = (uint16_t*)ps_malloc(px * sizeof(uint16_t));
    if (!s_buf_r || !s_buf_g || !s_buf_b || !s_buf_a) {
        Serial.println("[MS] PSRAM buffer allocation failed!");
        if (s_buf_r) { free(s_buf_r); s_buf_r = nullptr; }
        if (s_buf_g) { free(s_buf_g); s_buf_g = nullptr; }
        if (s_buf_b) { free(s_buf_b); s_buf_b = nullptr; }
        if (s_buf_a) { free(s_buf_a); s_buf_a = nullptr; }
        return false;
    }
    return true;
}

static bool decode_jpeg_to_rgb565(const uint8_t* jpeg, size_t jpeg_len,
                                  uint16_t* out, uint16_t& out_w, uint16_t& out_h) {
    static alignas(16) uint8_t work[4096];
    JDEC jd;
    MsStream stream = { jpeg, jpeg_len, 0 };

    JRESULT rc = jd_prepare(&jd, ms_in_func, work, sizeof(work), &stream);
    if (rc != JDR_OK) {
        Serial.printf("[MS] jd_prepare failed: %d\n", (int)rc);
        return false;
    }

    out_w = jd.width >> MS_DECODE_SCALE;
    out_h = jd.height >> MS_DECODE_SCALE;
    if (out_w > MS_MAX_W || out_h > MS_MAX_H) {
        Serial.printf("[MS] Decoded size %ux%u exceeds buffer.\n", out_w, out_h);
        return false;
    }

    g_out = out;
    g_out_w = out_w;
    rc = jd_decomp(&jd, ms_out_func, MS_DECODE_SCALE);
    if (rc != JDR_OK) {
        Serial.printf("[MS] jd_decomp failed: %d\n", (int)rc);
        return false;
    }
    return true;
}

// ─── Sensor control lock/restore ──────────────────────────────────
void multispectral_lock_sensor() {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        Serial.println("[MS] lock: no sensor.");
        return;
    }
    led_indicator_set_scan_active(true);
    int rc = s->set_whitebal(s, 0);
    if (rc != 0) Serial.printf("[MS] lock: set_whitebal rc=%d\n", rc);
    rc = s->set_awb_gain(s, 0);
    if (rc != 0) Serial.printf("[MS] lock: set_awb_gain rc=%d\n", rc);
    rc = s->set_gain_ctrl(s, 0);
    if (rc != 0) Serial.printf("[MS] lock: set_gain_ctrl rc=%d\n", rc);
    rc = s->set_exposure_ctrl(s, 0);
    if (rc != 0) Serial.printf("[MS] lock: set_exposure_ctrl rc=%d\n", rc);
    rc = s->set_agc_gain(s, MS_FIXED_GAIN);
    if (rc != 0) Serial.printf("[MS] lock: set_agc_gain rc=%d\n", rc);
    rc = s->set_aec_value(s, s_aec);
    if (rc != 0) Serial.printf("[MS] lock: set_aec_value rc=%d\n", rc);
    delay(20);
}

void multispectral_restore_sensor() {
    led_indicator_set_scan_active(false);
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        Serial.println("[MS] restore: no sensor.");
        return;
    }
    int rc = s->set_whitebal(s, 1);
    if (rc != 0) Serial.printf("[MS] restore: set_whitebal rc=%d\n", rc);
    rc = s->set_awb_gain(s, 1);
    if (rc != 0) Serial.printf("[MS] restore: set_awb_gain rc=%d\n", rc);
    rc = s->set_gain_ctrl(s, 1);
    if (rc != 0) Serial.printf("[MS] restore: set_gain_ctrl rc=%d\n", rc);
    rc = s->set_exposure_ctrl(s, 1);
    if (rc != 0) Serial.printf("[MS] restore: set_exposure_ctrl rc=%d\n", rc);
}

// ─── Flash grab with LED/exposure sync ────────────────────────────
camera_fb_t* multispectral_grab_flash_frame(uint8_t r, uint8_t g, uint8_t b) {
    rgb_led_set_raw(r, g, b);

    // Discard frames until the kept frame's exposure provably happens while
    // the LED is on. At SXGA the frame period (~100-200 ms) exceeds any settle
    // delay, so without this the returned frame is exposed before LED-on.
    camera_fb_t* fb = nullptr;
    for (int d = 0; d < MS_SYNC_DISCARD_FRAMES; d++) {
        fb = capture_camera_frame();
        if (!fb) {
            Serial.println("[MS] grab: discard frame failed.");
            return nullptr;
        }
        release_camera_frame(fb);
    }
    return capture_camera_frame();
}

// ─── Synthesis ────────────────────────────────────────────────────
static void synthesize(ColorFeatures& out, uint16_t w, uint16_t h) {
    const uint16_t* pr = s_buf_r;
    const uint16_t* pg = s_buf_g;
    uint16_t* pb = s_buf_b;
    const uint16_t* pa = s_ambient_enabled ? s_buf_a : nullptr;
    const size_t n = (size_t)w * h;

    uint64_t sum_r = 0, sum_g = 0, sum_b = 0;

    for (size_t i = 0; i < n; i++) {
        uint8_t r5 = (pr[i] >> 11) & 0x1F;
        uint8_t g6 = (pg[i] >> 5) & 0x3F;
        uint8_t b5 = pb[i] & 0x1F;
        if (pa) {
            uint8_t ar = (pa[i] >> 11) & 0x1F;
            uint8_t ag = (pa[i] >> 5) & 0x3F;
            uint8_t ab = pa[i] & 0x1F;
            r5 = (r5 > ar) ? (uint8_t)(r5 - ar) : 0;
            g6 = (g6 > ag) ? (uint8_t)(g6 - ag) : 0;
            b5 = (b5 > ab) ? (uint8_t)(b5 - ab) : 0;
        }

        r5 = (uint8_t)(r5 * s_gain_r);
        g6 = (uint8_t)(g6 * s_gain_g);
        b5 = (uint8_t)(b5 * s_gain_b);
        if (r5 > 31) r5 = 31;
        if (g6 > 63) g6 = 63;
        if (b5 > 31) b5 = 31;

        sum_r += r5;
        sum_g += g6;
        sum_b += b5;

        pb[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }

    camera_fb_t fb = {};
    fb.buf = (uint8_t*)pb;
    fb.len = n * sizeof(uint16_t);
    fb.width = w;
    fb.height = h;
    fb.format = PIXFORMAT_RGB565;

    float cm_per_pixel = MS_CM_PER_PIXEL_FULL * (float)(1 << MS_DECODE_SCALE);
    out = HueExtractor::process_frame(&fb, cm_per_pixel);

    // Corrected per-flash mean reflectance on a common 0-255 scale (R/B are
    // 5-bit, G is 6-bit in RGB565, so scale each to 255 for comparison).
    out.raw_rgb_means[0] = n ? (float)sum_r * 255.0f / 31.0f / (float)n : 0.0f;
    out.raw_rgb_means[1] = n ? (float)sum_g * 255.0f / 63.0f / (float)n : 0.0f;
    out.raw_rgb_means[2] = n ? (float)sum_b * 255.0f / 31.0f / (float)n : 0.0f;

    // Raw ambient baseline means (pre-gain) so the JSON shows light leakage.
    uint64_t sum_ar = 0, sum_ag = 0, sum_ab = 0;
    if (pa) {
        for (size_t i = 0; i < n; i++) {
            sum_ar += (pa[i] >> 11) & 0x1F;
            sum_ag += (pa[i] >> 5) & 0x3F;
            sum_ab += pa[i] & 0x1F;
        }
        out.ambient_rgb_means[0] = n ? (float)sum_ar * 255.0f / 31.0f / (float)n : 0.0f;
        out.ambient_rgb_means[1] = n ? (float)sum_ag * 255.0f / 63.0f / (float)n : 0.0f;
        out.ambient_rgb_means[2] = n ? (float)sum_ab * 255.0f / 31.0f / (float)n : 0.0f;
    } else {
        out.ambient_rgb_means[0] = out.ambient_rgb_means[1] = out.ambient_rgb_means[2] = 0.0f;
    }
}

bool multispectral_capture(ColorFeatures& out) {
    sensor_t* s = esp_camera_sensor_get();
    if (!s || !is_camera_ready()) {
        Serial.println("[MS] Camera not ready.");
        return false;
    }
    if (!ensure_buffers()) {
        return false;
    }

    uint16_t* bufs[3] = { s_buf_r, s_buf_g, s_buf_b };

    multispectral_lock_sensor();
    rgb_led_set_raw(0, 0, 0);

    // No-flash ambient baseline first. LED off + frame-discard sync so the
    // kept frame is provably exposed with the WS2812 fully dark (the pet LED
    // is suppressed during the scan, and this avoids any stale pre-scan frame).
    bool ambient_ok = false;
    if (s_ambient_enabled) {
        camera_fb_t* fb = multispectral_grab_flash_frame(0, 0, 0);
        if (fb) {
            uint16_t aw, ah;
            if (decode_jpeg_to_rgb565(fb->buf, fb->len, s_buf_a, aw, ah)) {
                ambient_ok = true;
                Serial.printf("[MS] Ambient baseline: %ux%u\n", aw, ah);
            } else {
                Serial.println("[MS] Ambient decode failed; running without subtraction.");
            }
            release_camera_frame(fb);
        }
    }

    uint16_t w = 0, h = 0;
    bool ok = true;
    for (int ch = 0; ch < 3 && ok; ch++) {
        uint32_t t0 = millis();
        camera_fb_t* fb = multispectral_grab_flash_frame(
            MS_FLASH_COLORS[ch].r, MS_FLASH_COLORS[ch].g, MS_FLASH_COLORS[ch].b);
        if (!fb) {
            Serial.printf("[MS] Capture failed under flash channel %d.\n", ch);
            ok = false;
            break;
        }
        uint16_t cw, chh;
        if (!decode_jpeg_to_rgb565(fb->buf, fb->len, bufs[ch], cw, chh)) {
            release_camera_frame(fb);
            ok = false;
            break;
        }
        release_camera_frame(fb);
        Serial.printf("[MS] Flash ch%d (%u,%u,%u): %u ms, frame %ux%u\n",
                      ch, MS_FLASH_COLORS[ch].r, MS_FLASH_COLORS[ch].g, MS_FLASH_COLORS[ch].b,
                      (unsigned)(millis() - t0), cw, chh);

        if (ch == 0) {
            w = cw;
            h = chh;
        } else if (cw != w || chh != h) {
            Serial.println("[MS] Frame dimensions differ across flashes.");
            ok = false;
        }
    }

    rgb_led_set_raw(0, 0, 0);
    multispectral_restore_sensor();

    if (!ok) {
        return false;
    }

    if (s_ambient_enabled && !ambient_ok) {
        Serial.println("[MS] WARNING: ambient baseline unavailable; scan ran without subtraction.");
    }

    synthesize(out, w, h);
    Serial.println("[MS] Multispectral capture OK.");
    return true;
}

#else  // !CONFIG_IDF_TARGET_ESP32S3

bool multispectral_capture(ColorFeatures& out) {
    (void)out;
    return false;
}

void multispectral_lock_sensor() {}

void multispectral_restore_sensor() {}

void multispectral_set_config(float gain_r, float gain_g, float gain_b, int aec, bool ambient) {
    (void)gain_r;
    (void)gain_g;
    (void)gain_b;
    (void)aec;
    (void)ambient;
}

void multispectral_load_calibration() {}

camera_fb_t* multispectral_grab_flash_frame(uint8_t r, uint8_t g, uint8_t b) {
    (void)r;
    (void)g;
    (void)b;
    return nullptr;
}

#endif
