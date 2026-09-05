// multispectral.cpp — Active Multispectral Imaging (R/G/B sequential flash)
#include <Arduino.h>
#include <cstring>

#include "multispectral.h"
#include "config.h"
#include "camera_handler.h"
#include "rgb_led.h"
#include "led_indicator.h"
#include "tjpgd.h"
#include "nvs_flash.h"
#include "nvs.h"

// ─── TJpgDec I/O (shared by single-shot and multispectral) ────────
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

static bool decode_jpeg_to_rgb565_shared(const uint8_t* jpeg, size_t jpeg_len,
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

// RAII flash guard: guarantees camera_set_flash(false) runs on EVERY exit
// path (return / NULL frame / error / exception-like early out), so a wedged
// capture can never leave the WS2812 flash lit. Destructor is the safety net.
struct FlashGuard {
    bool active;
    FlashGuard() : active(false) {}
    void on() {
        camera_set_flash(true);
        active = true;
    }
    void off() {
        if (active) {
            camera_set_flash(false);
            active = false;
        }
    }
    ~FlashGuard() { off(); }
};

// Single-shot decode buffer (PSRAM, 320×256 RGB565 = 160KB).
static uint16_t* s_ss_buf = nullptr;

// ─── AWB warm-up (item 1, 2026-09-05) ─────────────────────────────
// Converge white-balance off its cold-start green cast BEFORE the capture locks
// the sensor. AWB is still ACTIVE here (enabled at camera init); the ~10
// discarded frames (15 ms apart) let it settle, then the lock freezes the
// converged gains for the ambient/flash pair.
static void ms_warmup_awb() {
    for (int i = 0; i < MS_WARMUP_FRAMES; i++) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(MS_WARMUP_DELAY_MS));
    }
}

// ─── Fast single-shot white-flash capture (default on all targets) ──
bool multispectral_capture_single_shot(ColorFeatures& out) {
    if (!is_camera_ready()) {
        Serial.println("[MS] single-shot: camera not ready.");
        return false;
    }
    if (!s_ss_buf) {
        s_ss_buf = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_buf) {
            s_ss_buf = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        }
        if (!s_ss_buf) {
            Serial.println("[MS] single-shot: decode buffer alloc failed.");
            return false;
        }
    }

    // Suppress led_indicator so the WS2812 stays white during capture.
    led_indicator_set_scan_active(true);

    ms_warmup_awb();  // converge AWB before the kept frame (item 1)

    FlashGuard guard;
    guard.on();

    // Drain stale frame so the kept frame is exposed under white flash.
    camera_fb_t* w = capture_camera_frame();
    if (w) release_camera_frame(w);

    camera_fb_t* fb = capture_camera_frame();
    if (!fb) {
        led_indicator_set_scan_active(false);
        Serial.println("[MS] single-shot: capture failed.");
        return false;
    }

    // Decode JPEG to RGB565 at 1/4 scale (avoids the fmt2rgb888 crash
    // that occurs at full SXGA resolution).
    uint16_t sw, sh;
    bool decoded = decode_jpeg_to_rgb565_shared(fb->buf, fb->len, s_ss_buf, sw, sh);
    release_camera_frame(fb);

    if (!decoded || sw == 0 || sh == 0) {
        led_indicator_set_scan_active(false);
        Serial.println("[MS] single-shot: JPEG decode failed.");
        return false;
    }

    // Wrap the decoded RGB565 buffer as a fake camera_fb_t for process_frame.
    camera_fb_t fake_fb = {};
    fake_fb.buf    = (uint8_t*)s_ss_buf;
    fake_fb.len    = (size_t)sw * sh * sizeof(uint16_t);
    fake_fb.width  = sw;
    fake_fb.height = sh;
    fake_fb.format = PIXFORMAT_RGB565;

    out = HueExtractor::process_frame(&fake_fb, MS_CM_PER_PIXEL_FULL * (float)(1 << MS_DECODE_SCALE));

    // Fill raw reflectance means from the decoded frame so the report exposes
    // glare (near-white: all channels high) instead of zeros. process_frame
    // leaves raw_rgb_means at 0 — the multispectral path filled it separately
    // in synthesize(). Scale RGB565 to a common 0-255 for comparison.
    {
        uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
        const size_t n = (size_t)sw * sh;
        const uint16_t* p = s_ss_buf;
        for (size_t i = 0; i < n; i++) {
            sum_r += (p[i] >> 11) & 0x1F;
            sum_g += (p[i] >> 5) & 0x3F;
            sum_b += p[i] & 0x1F;
        }
        out.raw_rgb_means[0] = n ? (float)sum_r * 255.0f / 31.0f / (float)n : 0.0f;
        out.raw_rgb_means[1] = n ? (float)sum_g * 255.0f / 63.0f / (float)n : 0.0f;
        out.raw_rgb_means[2] = n ? (float)sum_b * 255.0f / 31.0f / (float)n : 0.0f;
    }

    led_indicator_set_scan_active(false);

    out.ambient_rgb_means[0] = out.ambient_rgb_means[1] = out.ambient_rgb_means[2] = 0.0f;

    Serial.printf("[MS] single-shot OK: %ux%u, hue=[%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f] vol=%.0f rgb=(%.0f,%.0f,%.0f)\n",
                  sw, sh,
                  out.hue_histogram[0], out.hue_histogram[1],
                  out.hue_histogram[2], out.hue_histogram[3],
                  out.hue_histogram[4], out.hue_histogram[5],
                  out.hue_histogram[6], out.hue_histogram[7],
                  out.volume_cm3,
                  out.raw_rgb_means[0], out.raw_rgb_means[1], out.raw_rgb_means[2]);
    return true;
}

// Ambient (no-flash) decode buffer for the differential pair, same layout.
static uint16_t* s_ss_amb_buf = nullptr;
// Second flash-frame decode buffer for the unified burst (Layer 2 parallax).
static uint16_t* s_ss_f2_buf = nullptr;

static bool grab_under_flash_state(bool flash_on, uint16_t* out_buf,
                                   uint16_t& out_w, uint16_t& out_h) {
    FlashGuard guard;
    if (flash_on) {
        guard.on();
    } else {
        camera_set_flash(false);
    }
    // Discard frames so the kept frame's exposure provably happens while the
    // LED is in the requested state (identical to the multispectral sync logic).
    for (int d = 0; d < MS_SYNC_DISCARD_FRAMES; d++) {
        camera_fb_t* w = capture_camera_frame();
        if (w) release_camera_frame(w);
    }
    camera_fb_t* fb = capture_camera_frame();
    if (!fb) return false;
    bool ok = decode_jpeg_to_rgb565_shared(fb->buf, fb->len, out_buf, out_w, out_h);
    release_camera_frame(fb);
    return ok;
}

// ─── Flash/No-Flash differential capture (default on all targets) ──
bool multispectral_capture_dual_shot(ColorFeatures& out) {
    if (!is_camera_ready()) {
        Serial.println("[MS] dual-shot: camera not ready.");
        return false;
    }
    if (!s_ss_buf) {
        s_ss_buf = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_buf) s_ss_buf = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_buf) {
            Serial.println("[MS] dual-shot: flash decode buffer alloc failed.");
            return false;
        }
    }
    if (!s_ss_amb_buf) {
        s_ss_amb_buf = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_amb_buf) s_ss_amb_buf = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_amb_buf) {
            Serial.println("[MS] dual-shot: ambient decode buffer alloc failed.");
            return false;
        }
    }

    led_indicator_set_scan_active(true);
    camera_wake();  // session start: restores XCLK from idle
    ms_warmup_awb();  // converge AWB while it is still active (item 1)
    multispectral_lock_sensor();

    // Ambient baseline (flash off), then back-to-back flash frame under the
    // SAME locked exposure/gain so delta-Y isolates the flash contribution.
    uint16_t aw = 0, ah = 0, fw = 0, fh = 0;
    bool a_ok = grab_under_flash_state(false, s_ss_amb_buf, aw, ah);
    bool f_ok = grab_under_flash_state(true, s_ss_buf, fw, fh);

    camera_set_flash(false);
    multispectral_restore_sensor();
    led_indicator_set_scan_active(false);

    if (!a_ok || !f_ok || aw != fw || ah != fh || aw == 0 || fw == 0) {
        camera_sleep_idle();  // back to idle even on failure
        Serial.println("[MS] dual-shot: capture/decode failed.");
        return false;
    }

    uint32_t fg_px = 0;
    int bbox_w = 0, bbox_h = 0;
    out = HueExtractor::process_differential(s_ss_buf, s_ss_amb_buf, fw, fh,
                                             MS_CM_PER_PIXEL_FULL * (float)(1 << MS_DECODE_SCALE),
                                             &fg_px, &bbox_w, &bbox_h);

    Serial.printf("[MS] dual-shot OK: %ux%u, hue=[%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f] vol=%.0f rgb=(%.0f,%.0f,%.0f) amb=(%.0f,%.0f,%.0f) fg=%u bbox=%dx%d\n",
                  fw, fh,
                  out.hue_histogram[0], out.hue_histogram[1],
                  out.hue_histogram[2], out.hue_histogram[3],
                  out.hue_histogram[4], out.hue_histogram[5],
                  out.hue_histogram[6], out.hue_histogram[7],
                  out.volume_cm3,
                  out.raw_rgb_means[0], out.raw_rgb_means[1], out.raw_rgb_means[2],
                  out.ambient_rgb_means[0], out.ambient_rgb_means[1], out.ambient_rgb_means[2],
                  fg_px, bbox_w, bbox_h);
    camera_sleep_idle();  // session end: gate XCLK so the sensor cools at idle
    return true;
}

// ─── Debug variant: same capture, plus timing + JPEGs + selection area ──
bool multispectral_capture_dual_shot_debug(ColorFeatures& out, DualShotDebugInfo& info,
                                           camera_fb_t*& ambient_fb, camera_fb_t*& flash_fb) {
    memset(&info, 0, sizeof(info));
    ambient_fb = nullptr;
    flash_fb = nullptr;
    uint32_t t_total = millis();

    if (!is_camera_ready()) {
        Serial.println("[MS] dual-shot-debug: camera not ready.");
        return false;
    }
    if (!s_ss_buf) {
        s_ss_buf = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_buf) s_ss_buf = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_buf) {
            Serial.println("[MS] dual-shot-debug: flash decode buffer alloc failed.");
            return false;
        }
    }
    if (!s_ss_amb_buf) {
        s_ss_amb_buf = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_amb_buf) s_ss_amb_buf = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_amb_buf) {
            Serial.println("[MS] dual-shot-debug: ambient decode buffer alloc failed.");
            return false;
        }
    }

    led_indicator_set_scan_active(true);
    FlashGuard guard;
    camera_wake();  // session start: restores XCLK from idle
    ms_warmup_awb();  // converge AWB while it is still active (item 1)
    uint32_t t0 = millis();
    multispectral_lock_sensor();
    info.lock_ms = millis() - t0;

// Ambient baseline — copy out immediately so the single driver buffer is
    // freed BEFORE any further grab. This driver runs fb_count=1 and
    // CAMERA_GRAB_LATEST: holding a camera_fb_t across another fb_get leaves no
    // buffer for the next DMA frame, so fb_get blocks and eventually returns
    // NULL — the exact "sensor wedged" signature seen in every earlier round.
    uint32_t tg = millis();
    camera_fb_t* amb = nullptr;
    size_t amb_len = 0;
    camera_set_flash(false);
    for (int d = 0; d < MS_SYNC_DISCARD_FRAMES; d++) {
        camera_fb_t* wf = capture_camera_frame();
        if (wf) release_camera_frame(wf);
    }
    camera_fb_t* amb_live = capture_camera_frame();
    info.ambient_grab_ms = millis() - tg;
    if (!amb_live) {
        multispectral_restore_sensor();
        led_indicator_set_scan_active(false);
        camera_sleep_idle();
        Serial.println("[MS] dual-shot-debug: ambient grab failed.");
        return false;
    }
    amb = copy_camera_frame(amb_live, amb_len);
    release_camera_frame(amb_live);
    if (!amb) {
        multispectral_restore_sensor();
        led_indicator_set_scan_active(false);
        camera_sleep_idle();
        Serial.println("[MS] dual-shot-debug: ambient copy failed.");
        return false;
    }

    // Flash frame — keep the raw frame for the host (copy out + release).
    tg = millis();
    camera_fb_t* fsh = nullptr;
    size_t fsh_len = 0;
    guard.on();
    for (int d = 0; d < MS_SYNC_DISCARD_FRAMES; d++) {
        camera_fb_t* wf = capture_camera_frame();
        if (wf) release_camera_frame(wf);
    }
    camera_fb_t* fsh_live = capture_camera_frame();
    info.flash_grab_ms = millis() - tg;

    multispectral_restore_sensor();
    led_indicator_set_scan_active(false);

    if (!fsh_live) {
        free_copied_frame(amb);
        camera_sleep_idle();
        Serial.println("[MS] dual-shot-debug: flash grab failed.");
        return false;
    }
    fsh = copy_camera_frame(fsh_live, fsh_len);
    release_camera_frame(fsh_live);
    if (!fsh) {
        free_copied_frame(amb);
        camera_sleep_idle();
        Serial.println("[MS] dual-shot-debug: flash copy failed.");
        return false;
    }

    info.ambient_jpeg_len = amb_len;
    info.flash_jpeg_len = fsh_len;

    uint16_t aw, ah, fw, fh;
    uint32_t td = millis();
    bool a_ok = decode_jpeg_to_rgb565_shared(amb->buf, amb->len, s_ss_amb_buf, aw, ah);
    bool f_ok = decode_jpeg_to_rgb565_shared(fsh->buf, fsh->len, s_ss_buf, fw, fh);
    info.decode_ms = millis() - td;

    if (!a_ok || !f_ok || aw != fw || ah != fh || aw == 0 || fw == 0) {
        release_camera_frame(amb);
        release_camera_frame(fsh);
        camera_sleep_idle();
        Serial.println("[MS] dual-shot-debug: decode failed.");
        return false;
    }
    info.w = fw;
    info.h = fh;

    uint32_t fg_px = 0;
    int bx = 0, by = 0, bw = 0, bh = 0;
    uint32_t core_px = 0, snap_px = 0;
    uint32_t tp = millis();
    out = HueExtractor::process_differential(s_ss_buf, s_ss_amb_buf, fw, fh,
                                             MS_CM_PER_PIXEL_FULL * (float)(1 << MS_DECODE_SCALE),
                                             &fg_px, &bx, &by, &bw, &bh,
                                             &core_px, &snap_px);
    info.process_ms = millis() - tp;
    info.fg_pixels = fg_px;
    info.core_pixels = core_px;
    info.snap_pixels = snap_px;
    info.bbox_x = bx;
    info.bbox_y = by;
    info.bbox_w = bw;
    info.bbox_h = bh;
    info.total_ms = millis() - t_total;

    // Steal ownership to the caller so the raw JPEGs can be streamed.
    ambient_fb = amb;
    flash_fb = fsh;

    Serial.printf("[MS] dual-shot-debug OK: %ux%u fg=%u(core %u snap %u) bbox=(%d,%d,%d,%d) "
                  "t=[lock %u, amb %u, flash %u, dec %u, proc %u, total %u] ms\n",
                  info.w, info.h, fg_px, core_px, snap_px, bx, by, bw, bh,
                  info.lock_ms, info.ambient_grab_ms, info.flash_grab_ms,
                  info.decode_ms, info.process_ms, info.total_ms);
    camera_sleep_idle();  // session end: caller still owns the JPEG buffers, but
                          // the sensor clock can cool down while they stream.
    return true;
}

// ─── Unified 3-frame burst (Ambient → Flash₁ → Flash₂) ─────────────
// Buffers for the small (non-debug) burst path.
static uint16_t* s_burst_amb_buf = nullptr;
static uint16_t* s_burst_f1_buf  = nullptr;
static uint16_t* s_burst_f2_buf  = nullptr;

// Grab one frame under a fixed flash state, decode to RGB565 (1/4 scale) into
// out_buf. Returns true if decoded; dimensions land in out_w/out_h. Copies
// each raw JPEG out and immediately returns the driver frame (fb_count=1:
// a driver buffer must never be held across another fb_get).
static bool burst_grab_decode(bool flash_on, uint16_t* out_buf,
                              uint16_t& out_w, uint16_t& out_h) {
    FlashGuard guard;
    if (flash_on) {
        guard.on();
    } else {
        camera_set_flash(false);
    }
    for (int d = 0; d < MS_SYNC_DISCARD_FRAMES; d++) {
        camera_fb_t* wf = capture_camera_frame();
        if (wf) release_camera_frame(wf);
    }
    camera_fb_t* live = capture_camera_frame();
    if (!live) return false;
    // We only hold this live frame long enough to decode; the copy_out + release
    // pattern below mirrors the debug path so a JPEG can also be streamed if the
    // caller wants it.
    bool ok = decode_jpeg_to_rgb565_shared(live->buf, live->len, out_buf, out_w, out_h);
    release_camera_frame(live);
    return ok;
}

bool multispectral_capture_burst(ColorFeatures& out) {
    if (!is_camera_ready()) {
        Serial.println("[MS] burst: camera not ready.");
        return false;
    }
    if (!s_burst_amb_buf) s_burst_amb_buf = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
    if (!s_burst_f1_buf)  s_burst_f1_buf  = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
    if (!s_burst_f2_buf)  s_burst_f2_buf  = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
    if (!s_burst_amb_buf) s_burst_amb_buf = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
    if (!s_burst_f1_buf)  s_burst_f1_buf  = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
    if (!s_burst_f2_buf)  s_burst_f2_buf  = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
    if (!s_burst_amb_buf || !s_burst_f1_buf || !s_burst_f2_buf) {
        Serial.println("[MS] burst: decode buffer alloc failed.");
        return false;
    }

    led_indicator_set_scan_active(true);
    camera_wake();
    ms_warmup_awb();  // converge AWB while it is still active (item 1)
    multispectral_lock_sensor();

    uint16_t aw = 0, ah = 0, f1w = 0, f1h = 0, f2w = 0, f2h = 0;
    bool a_ok   = burst_grab_decode(false, s_burst_amb_buf, aw, ah);
    bool f1_ok  = burst_grab_decode(true,  s_burst_f1_buf,  f1w, f1h);
    // ~25 ms later: second flash frame (handheld micro-tilt gives parallax).
    vTaskDelay(pdMS_TO_TICKS(25));
    bool f2_ok  = burst_grab_decode(true,  s_burst_f2_buf,  f2w, f2h);

    camera_set_flash(false);
    multispectral_restore_sensor();
    led_indicator_set_scan_active(false);

    if (!a_ok || !f1_ok || !f2_ok || aw != f1w || f1w != f2w || ah != f1h || f1h != f2h || aw == 0) {
        camera_sleep_idle();
        Serial.println("[MS] burst: capture/decode failed.");
        return false;
    }

    BurstMotionStats motion;
    uint32_t fg_px = 0;
    int bw = 0, bh = 0;
    out = HueExtractor::process_unified_burst(s_burst_f1_buf, s_burst_f2_buf, s_burst_amb_buf,
                                              f1w, f1h,
                                              MS_CM_PER_PIXEL_FULL * (float)(1 << MS_DECODE_SCALE),
                                              &motion, &fg_px,
                                              &bw, &bh);

    Serial.printf("[MS] burst OK: %ux%u fg=%u bbox=%dx%d motion=%.2fpx(%s) "
                  "hue=[%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f] vol=%.0f rgb=(%.0f,%.0f,%.0f)\n",
                  f1w, f1h, fg_px, bw, bh,
                  motion.global_motion, motion.handheld ? "handheld" : "bench",
                  out.hue_histogram[0], out.hue_histogram[1], out.hue_histogram[2], out.hue_histogram[3],
                  out.hue_histogram[4], out.hue_histogram[5], out.hue_histogram[6], out.hue_histogram[7],
                  out.volume_cm3, out.raw_rgb_means[0], out.raw_rgb_means[1], out.raw_rgb_means[2]);
    camera_sleep_idle();
    return true;
}

// Debug variant: identical burst, plus timing + parallax + 3 raw JPEGs.
bool multispectral_capture_burst_debug(ColorFeatures& out, DualShotDebugInfo& info,
                                       camera_fb_t*& ambient_fb, camera_fb_t*& flash_fb,
                                       camera_fb_t*& flash2_fb, bool raw_only) {
    memset(&info, 0, sizeof(info));
    ambient_fb = nullptr;
    flash_fb = nullptr;
    flash2_fb = nullptr;
    uint32_t t_total = millis();

    if (!is_camera_ready()) {
        Serial.println("[MS] burst-debug: camera not ready.");
        return false;
    }
    if (!s_ss_buf) {
        s_ss_buf = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_buf) s_ss_buf = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_buf) { Serial.println("[MS] burst-debug: flash decode buffer alloc failed."); return false; }
    }
    if (!s_ss_amb_buf) {
        s_ss_amb_buf = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_amb_buf) s_ss_amb_buf = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_amb_buf) { Serial.println("[MS] burst-debug: ambient decode buffer alloc failed."); return false; }
    }
    if (!s_ss_f2_buf) {
        s_ss_f2_buf = (uint16_t*)ps_malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_f2_buf) s_ss_f2_buf = (uint16_t*)malloc((size_t)MS_MAX_W * MS_MAX_H * sizeof(uint16_t));
        if (!s_ss_f2_buf) { Serial.println("[MS] burst-debug: flash2 decode buffer alloc failed."); return false; }
    }

    led_indicator_set_scan_active(true);
    FlashGuard guard;
    camera_wake();
    ms_warmup_awb();  // converge AWB while it is still active (item 1)
    uint32_t t0 = millis();
    multispectral_lock_sensor();
    info.lock_ms = millis() - t0;

    // Ambient baseline — copy out + release so the single driver buffer is free
    // before every later fb_get (fb_count=1 / CAMERA_GRAB_LATEST wedge fix).
    camera_fb_t* amb = nullptr;
    size_t amb_len = 0;
    camera_fb_t* fsh = nullptr;
    size_t fsh_len = 0;
    camera_fb_t* f2  = nullptr;
    size_t f2_len  = 0;

    uint32_t tg = millis();
    camera_set_flash(false);
    for (int d = 0; d < MS_SYNC_DISCARD_FRAMES; d++) {
        camera_fb_t* wf = capture_camera_frame();
        if (wf) release_camera_frame(wf);
    }
    camera_fb_t* amb_live = capture_camera_frame();
    info.ambient_grab_ms = millis() - tg;
    if (amb_live) {
        amb = copy_camera_frame(amb_live, amb_len);
        release_camera_frame(amb_live);
    }

    // Flash₁ — copy out + release.
    tg = millis();
    guard.on();
    for (int d = 0; d < MS_SYNC_DISCARD_FRAMES; d++) {
        camera_fb_t* wf = capture_camera_frame();
        if (wf) release_camera_frame(wf);
    }
    camera_fb_t* fsh_live = capture_camera_frame();
    info.flash_grab_ms = millis() - tg;
    if (fsh_live) {
        fsh = copy_camera_frame(fsh_live, fsh_len);
        release_camera_frame(fsh_live);
    }

    // Flash₂ — same, ~25 ms after F1 so a handheld micro-tilt gives parallax.
    vTaskDelay(pdMS_TO_TICKS(25));
    tg = millis();
    // Flash stays on (guard.active) across F2 — the WS2812 LED is continuous.
    for (int d = 0; d < MS_SYNC_DISCARD_FRAMES; d++) {
        camera_fb_t* wf = capture_camera_frame();
        if (wf) release_camera_frame(wf);
    }
    camera_fb_t* f2_live = capture_camera_frame();
    info.flash2_grab_ms = millis() - tg;
    if (f2_live) {
        f2 = copy_camera_frame(f2_live, f2_len);
        release_camera_frame(f2_live);
    }

    multispectral_restore_sensor();
    led_indicator_set_scan_active(false);
    if (!amb || !fsh || !f2) {
        free_copied_frame(amb);
        free_copied_frame(fsh);
        free_copied_frame(f2);
        camera_sleep_idle();
        Serial.println("[MS] burst-debug: capture/copy failed.");
        return false;
    }

    info.ambient_jpeg_len = amb_len;
    info.flash_jpeg_len   = fsh_len;
    info.flash2_jpeg_len  = f2_len;

    uint16_t aw, ah, f1w, f1h, f2w, f2h;
    uint32_t td = millis();
    bool a_ok  = decode_jpeg_to_rgb565_shared(amb->buf, amb->len, s_ss_amb_buf, aw, ah);
    bool f1_ok = decode_jpeg_to_rgb565_shared(fsh->buf, fsh->len, s_ss_buf, f1w, f1h);
    bool f2_ok = decode_jpeg_to_rgb565_shared(f2->buf, f2->len, s_ss_f2_buf, f2w, f2h);
    info.decode_ms = millis() - td;

    if (!a_ok || !f1_ok || !f2_ok || aw != f1w || f1w != f2w || ah != f1h || f1h != f2h || aw == 0) {
        free_copied_frame(amb);
        free_copied_frame(fsh);
        free_copied_frame(f2);
        camera_sleep_idle();
        Serial.println("[MS] burst-debug: decode failed.");
        return false;
    }
    info.w = f1w;
    info.h = f1h;

    // In raw-only (host-compute) mode the on-device segmentation is skipped;
    // the host rebuilds every feature from the three JPEGs. The decode buffers
    // are still used to verify dimension parity, then discarded.
    if (raw_only) {
        info.process_ms = 0;
        info.fg_pixels = 0;
        info.core_pixels = 0;
        info.snap_pixels = 0;
        info.global_motion = 0.0f;
        info.handheld = false;
        info.moving_blocks = 0;
        info.total_blocks = 0;
        info.bbox_x = 0;
        info.bbox_y = 0;
        info.bbox_w = 0;
        info.bbox_h = 0;
        info.total_ms = millis() - t_total;
        memset(&out, 0, sizeof(out));
        ambient_fb = amb;
        flash_fb = fsh;
        flash2_fb = f2;
        Serial.printf("[MS] burst-debug OK (raw-only): %ux%u "
                      "t=[lock %u, amb %u, f1 %u, f2 %u, dec %u, proc 0, total %u] ms "
                      "[amb %u, f1 %u, f2 %u] B\n",
                      info.w, info.h, info.lock_ms, info.ambient_grab_ms,
                      info.flash_grab_ms, info.flash2_grab_ms, info.decode_ms,
                      info.total_ms, (unsigned)info.ambient_jpeg_len,
                      (unsigned)info.flash_jpeg_len, (unsigned)info.flash2_jpeg_len);
        camera_sleep_idle();
        return true;
    }

    BurstMotionStats motion;
    uint32_t fg_px = 0;
    int bx = 0, by = 0, bw = 0, bh = 0;
    uint32_t core_px = 0, snap_px = 0;
    uint32_t tp = millis();
    out = HueExtractor::process_unified_burst(s_ss_buf, s_ss_f2_buf, s_ss_amb_buf, f1w, f1h,
                                              MS_CM_PER_PIXEL_FULL * (float)(1 << MS_DECODE_SCALE),
                                              &motion, &fg_px, &bx, &by, &bw, &bh,
                                              &core_px, &snap_px);
    info.process_ms = millis() - tp;
    info.fg_pixels = fg_px;
    info.core_pixels = core_px;
    info.snap_pixels = snap_px;
    info.global_motion = motion.global_motion;
    info.handheld = motion.handheld;
    info.moving_blocks = motion.moving_blocks;
    info.total_blocks = motion.total_blocks;
    info.bbox_x = bx;
    info.bbox_y = by;
    info.bbox_w = bw;
    info.bbox_h = bh;
    info.total_ms = millis() - t_total;

    ambient_fb = amb;
    flash_fb = fsh;
    flash2_fb = f2;

    Serial.printf("[MS] burst-debug OK: %ux%u fg=%u(core %u snap %u) motion=%.2fpx(%s,%d/%d blk) "
                  "bbox=(%d,%d,%d,%d) t=[lock %u, amb %u, f1 %u, f2 %u, dec %u, proc %u, total %u] ms\n",
                  info.w, info.h, fg_px, core_px, snap_px,
                  motion.global_motion, motion.handheld ? "handheld" : "bench",
                  motion.moving_blocks, motion.total_blocks,
                  bx, by, bw, bh,
                  info.lock_ms, info.ambient_grab_ms, info.flash_grab_ms,
                  info.flash2_grab_ms, info.decode_ms, info.process_ms, info.total_ms);
    camera_sleep_idle();
    return true;
}

#if defined(CONFIG_IDF_TARGET_ESP32S3)

// Tuning constants live in config.h (MS_*).
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

    // OV3660-safety: do NOT call set_exposure_ctrl(s, 0) mid-stream. Disabling
    // auto-exposure while the DVP pipeline is live freezes the internal exposure
    // integrator; the sensor then halts PCLK output, the ES32-S3 LCD_CAM/GDMA
    // waits forever, and esp_camera_fb_get() blocks then returns NULL. Lock
    // gain + white balance instead (still fixed for the ambient/flash pair);
    // AEC stays enabled so the timing generator keeps running.
    int rc = s->set_whitebal(s, 0);
    if (rc != 0) Serial.printf("[MS] lock: set_whitebal rc=%d\n", rc);
    rc = s->set_awb_gain(s, 0);
    if (rc != 0) Serial.printf("[MS] lock: set_awb_gain rc=%d\n", rc);
    rc = s->set_gain_ctrl(s, 0);
    if (rc != 0) Serial.printf("[MS] lock: set_gain_ctrl rc=%d\n", rc);
    rc = s->set_agc_gain(s, MS_FIXED_GAIN);
    if (rc != 0) Serial.printf("[MS] lock: set_agc_gain rc=%d\n", rc);
    rc = s->set_wb_mode(s, 0);
    if (rc != 0) Serial.printf("[MS] lock: set_wb_mode rc=%d\n", rc);
    // Give the sensor one frame to absorb the register writes.
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

    ms_warmup_awb();  // converge AWB while it is still active (item 1)
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

void multispectral_lock_sensor() {
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        Serial.println("[MS] lock: no sensor.");
        return;
    }
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
    rc = s->set_aec_value(s, MS_FIXED_AEC);
    if (rc != 0) Serial.printf("[MS] lock: set_aec_value rc=%d\n", rc);
    delay(20);
}

void multispectral_restore_sensor() {
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
