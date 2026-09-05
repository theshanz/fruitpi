#pragma once

#include "extract_hues.h"

struct MsFlashColor {
    uint8_t r, g, b;
};

// Detailed diagnostics for a dual-shot capture, for the BLE `ms_debug_diff`
// command. All times are milliseconds.
struct DualShotDebugInfo {
    uint32_t lock_ms;          // sensor AWB/AGC/AEC lock
    uint32_t ambient_grab_ms;  // flash-off ambient frame grab (+discards)
    uint32_t flash_grab_ms;    // flash-on frame grab (+discards)
    uint32_t flash2_grab_ms;   // second flash frame grab (micro-tilt parallax)
    uint32_t decode_ms;        // JPEGs -> RGB565
    uint32_t process_ms;       // unified burst segmentation + features
    uint32_t total_ms;         // entire capture incl. transfers excluded
    uint16_t w, h;             // decoded frame size
    uint32_t fg_pixels;        // edge-refined foreground mask pixels (core+snap)
    uint32_t core_pixels;      // solid-core pixels (ΔY >= dynamic τ_core)
    uint32_t snap_pixels;      // rim pixels admitted by gradient snapping
    float    global_motion;    // Layer 2 mean macroblock displacement (px)
    bool     handheld;         // true if the global-motion gate was enforced
    int      moving_blocks;    // blocks with displacement >= PARALLAX_MIN_MOTION
    int      total_blocks;     // total macroblocks in the population
    int bbox_x, bbox_y;        // selection area (top-left) in pixels
    int bbox_w, bbox_h;        // selection area size in pixels
    size_t ambient_jpeg_len;
    size_t flash_jpeg_len;
    size_t flash2_jpeg_len;
};

constexpr MsFlashColor MS_FLASH_COLORS[3] = {
    {255, 0, 0},
    {0, 255, 0},
    {0, 0, 255},
};

/**
 * @brief Active Multispectral Imaging capture (R -> G -> B sequential flash).
 *
 * Locks AWB/AGC/AEC, flashes the WS2812 pure red/green/blue, captures one
 * JPEG frame per flash, decodes each to RGB565 (TJpgDec, downscaled), then
 * synthesizes a single white-balanced RGB565 image (red channel from the red
 * flash, green from the green flash, blue from the blue flash) and runs the
 * standard HueExtractor pipeline on it.
 *
 * @param out Receives the synthesized ColorFeatures on success.
 * @return true on success, false if capture/decode/synthesis failed.
 */
bool multispectral_capture(ColorFeatures& out);

/**
 * @brief Overrides the runtime per-channel gains, locked AEC value and
 *        ambient-subtraction enable, then persists them to NVS.
 *
 * Used by the BLE ms_config command for white-card calibration without
 * reflashing. Missing NVS keys fall back to the compile-time constants.
 */
void multispectral_set_config(float gain_r, float gain_g, float gain_b, int aec, bool ambient);

/**
 * @brief Loads calibration overrides (gains/AEC/ambient) from NVS, falling
 *        back to the compile-time constants when keys are absent.
 */
void multispectral_load_calibration();

/**
 * @brief Locks AWB/AGC/AEC to the fixed values used for multispectral captures.
 */
void multispectral_lock_sensor();

/**
 * @brief Restores AWB/AGC/AEC to automatic after a multispectral capture.
 */
void multispectral_restore_sensor();

/**
 * @brief Grabs one camera frame exposed under the given flash color.
 *
 * Turns on the WS2812 to the requested color and discards enough frames for
 * the sensor exposure to synchronize with the LED, guaranteeing the returned
 * frame was exposed while the LED was active. The LED is left ON for the
 * caller to turn off / change color.
 *
 * @return camera_fb_t* to release with release_camera_frame(), or nullptr.
 */
camera_fb_t* multispectral_grab_flash_frame(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Fast single-shot optical capture with the white flash.
 *
 * The default capture path on all targets. Turns the white flash on, grabs one
 * freshly-exposed frame, runs the standard HueExtractor pipeline directly on
 * the JPEG frame, then turns the flash off.
 *
 * Faster than the R/G/B multispectral scan (one flash grab instead of four)
 * and uses the camera's native color accuracy, which is what matters on the
 * higher-quality OV2640. The multispectral R/G/B path stays available for
 * calibration/debug via multispectral_capture().
 *
 * @param out Receives the extracted ColorFeatures on success.
 * @return true on success, false if capture or feature extraction failed.
 */
bool multispectral_capture_single_shot(ColorFeatures& out);

/**
 * @brief Flash/no-flash differential capture (default on all targets).
 *
 * Captures two back-to-back frames under the SAME locked exposure/gain:
 * one with the flash off (ambient baseline) and one with the white flash on.
 * process_differential() then distance-gates the foreground via flash falloff
 * (a pixel is foreground only if the flash lit it clearly above the ambient
 * baseline), so far-away background is excluded from both the hue histogram
 * and the volume bounding box.
 *
 * @param out Receives the extracted ColorFeatures on success.
 * @return true on success, false if capture or feature extraction failed.
 */
bool multispectral_capture_dual_shot(ColorFeatures& out);

/**
 * @brief Unified 3-frame burst capture (Ambiant → Flash₁ → Flash₂).
 *
 * The production path for the 5-layer pipeline on all targets. Locks
 * AWB/AGC/AEC, captures one no-flash baseline and TWO back-to-back white-flash
 * frames ~25 ms apart (so a handheld micro-tilt gives Layer 2 a parallax
 * signal), decodes each to RGB565, then runs process_unified_burst() (fusion +
 * dynamic ΔY + parallax + trimap + density bbox + semantic gates).
 *
 * @param out Receives the extracted ColorFeatures (ColFeatures with volume).
 * @return true on success, false if capture or feature extraction failed.
 */
bool multispectral_capture_burst(ColorFeatures& out);

/**
 * @brief Debug variant of the unified burst capture.
 *
 * Runs the identical ambient + Flash₁ + Flash₂ grab / fusion / segmentation,
 * but also exposes the timing breakdown, parallax diagnostics, the foreground
 * selection area (bbox) and the three original JPEG frames so a host can
 * inspect exactly what the firmware saw.
 *
 * The returned camera_fb_t* buffers are PSRAM copies made by
 * copy_camera_frame() (fb_count=1 cannot hold a driver frame across a grab).
 * The caller MUST free them with free_copied_frame().
 *
 * When raw_only is true the on-device segmentation is skipped entirely and the
 * feature/timing/debug out-params are left empty; the host recomputes all
 * features from the three returned JPEGs. This is the fast local-report path.
 *
 * @param out     Receives the extracted ColorFeatures.
 * @param info    Receives timing / parallax / selection-area diagnostics.
 * @param ambient_fb Out: no-flash JPEG frame, or nullptr.
 * @param flash_fb   Out: first flash JPEG frame, or nullptr.
 * @param flash2_fb  Out: second flash JPEG frame, or nullptr.
 * @param raw_only   Skip on-device segmentation (host recomputes everything).
 * @return true on success; on failure the frame out-params are nullptr.
 */
bool multispectral_capture_burst_debug(ColorFeatures& out, DualShotDebugInfo& info,
                                       camera_fb_t*& ambient_fb, camera_fb_t*& flash_fb,
                                       camera_fb_t*& flash2_fb, bool raw_only = false);
