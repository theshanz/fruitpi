// ─────────────────────────────────────────────────────────────────
//  extract_hues.h — ESP32 Camera Vision Feature Extraction
// ─────────────────────────────────────────────────────────────────
#pragma once

#include "esp_camera.h"
#include <cstdint>

struct ColorFeatures {
    float hue_histogram[8];     // Normalized Hue distribution between 20° and 120° (Sum = 1.0)
    float chromatic_dispersion; // 0.0 = mottled/variable, 1.0 = uniform color
    float volume_cm3;           // Estimated volume derived from foreground bounding box
    float raw_rgb_means[3];     // Active multispectral: per-flash mean reflectance (0-255 scale)
    float ambient_rgb_means[3]; // No-flash ambient baseline means (0-255 scale)

    // ── Natural-ellipse (oval) fit + frame-overflow signal ──────────
    // The fruit's intrinsic oval, estimated from the chosen mask's 2nd central
    // image moments (covariance of pixel positions). Unlike the axis-aligned
    // bbox, this survives the fruit clipping a frame edge: a partial oval still
    // yields the ellipse it would have made if wholly in view.
    float ellipse_major_px;     // natural oval major axis (px)
    float ellipse_minor_px;     // natural oval minor axis (px)
    float centroid_px[2];       // mask centroid (x, y) px
    uint8_t overflow_mask;      // bit0=left, bit1=right, bit2=top, bit3=bottom
                                // edge touched by the fruit -> it EXCEEDS the
                                // frame, so volume_cm3 is a LOWER bound / clamped.
};

// Motion-parallax stats for the unified burst (Layer 2), exposed to BLE debug.
struct BurstMotionStats {
    float global_motion;    // mean |displacement| across all macroblocks (px)
    bool  handheld;         // global_motion >= PARALLAX_GLOBAL_HANDHELD -> gate active
    int   moving_blocks;    // blocks whose displacement >= PARALLAX_MIN_MOTION
    int   total_blocks;     // total macroblocks evaluated
};

class HueExtractor {
public:
    /**
     * @brief Converts RGB888 components to HSV color space.
     * @param r Red [0..255]
     * @param g Green [0..255]
     * @param b Blue [0..255]
     * @param h Output Hue angle [0.0..360.0]
     * @param s Output Saturation [0.0..1.0]
     * @param v Output Value / Brightness [0.0..1.0]
     */
    static void rgb888_to_hsv(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& v);

    /**
     * @brief Computes chromatic dispersion from an 8-bin normalized hue histogram.
     * @return Dispersion value in [0.0, 1.0]
     */
    static float calculate_chromatic_dispersion(const float hue[8]);

    /**
     * @brief Processes an ESP32 camera framebuffer and extracts 28-D vision features.
     * @param fb Pointer to camera framebuffer (Expected format: PIXFORMAT_RGB565)
     * @param cm_per_pixel Calibration constant mapping pixels to cm (default: 0.05 cm/px)
     */
    static ColorFeatures process_frame(camera_fb_t* fb, float cm_per_pixel = 0.05f);

    /**
     * @brief Flash/no-flash differential segmentation with edge-refined mask.
     *
     * Foreground is distance-gated by flash falloff. A solid core (ΔY >=
     * EDGE_CORE_DY) is baseline; a definite background (ΔY < EDGE_BG_DY) is
     * cut; the ambiguous rim in between is snapped to the sharpest ΔY gradient
     * (EDGE_GRAD_MIN) when it touches the core, so the silhouette tracks the
     * true physical edge instead of clipping rounded rims or leaking into the
     * table. Far background receives negligible flash light and is excluded
     * from both the hue histogram and the volume bounding box.
     *
     * @param flash   RGB565 frame captured under the white flash
     * @param ambient RGB565 frame captured back-to-back with flash off
     *                (identical locked exposure/gain, so delta-Y is purely
     *                the flash contribution)
     * @param w, h    Frame dimensions in pixels
     * @param cm_per_pixel Calibration constant mapping pixels to cm
     */
    static ColorFeatures process_differential(const uint16_t* flash, const uint16_t* ambient,
                                              uint16_t w, uint16_t h,
                                              float cm_per_pixel = 0.05f,
                                              uint32_t* fg_pixels_out = nullptr,
                                              int* bbox_x_out = nullptr, int* bbox_y_out = nullptr,
                                              int* bbox_w_out = nullptr, int* bbox_h_out = nullptr,
                                              uint32_t* core_pixels_out = nullptr,
                                              uint32_t* snap_pixels_out = nullptr);

    /**
     * @brief Unified 5-layer 3-frame burst segmentation (spec + expert decisions).
     *
     * Captures the full multi-layer defense: Layer 3 shadow healing via
     * per-channel max(Flash1, Flash2), Layer 1 ambient-subtracted ΔY with a
     * 98th-percentile dynamic threshold, Layer 2 SAD macroblock parallax with an
     * adaptive global handle/bench classifier, Layer 4 trimap gradient snapping,
     * and Layer 5 semantic gates (mean-S glossy-table reject + 2D skin gate +
     * glare/noise cuts) with a marginal-density bounding box.
     *
     * @param flashed1, flashed2  Two flash-lit RGB565 frames (F1 then F2).
     * @param ambient             No-flash RGB565 baseline under identical exposure.
     * @param w, h                Frame dimensions in pixels.
     * @param cm_per_pixel        Calibration constant mapping pixels to cm.
     * @param motion              Out: parallax/global-motion diagnostics (optional).
     * @param ...same out-params as process_differential...
     */
    static ColorFeatures process_unified_burst(const uint16_t* flashed1, const uint16_t* flashed2,
                                               const uint16_t* ambient,
                                               uint16_t w, uint16_t h,
                                               float cm_per_pixel = 0.05f,
                                               BurstMotionStats* motion = nullptr,
                                               uint32_t* fg_pixels_out = nullptr,
                                               int* bbox_x_out = nullptr, int* bbox_y_out = nullptr,
                                               int* bbox_w_out = nullptr, int* bbox_h_out = nullptr,
                                               uint32_t* core_pixels_out = nullptr,
                                               uint32_t* snap_pixels_out = nullptr);
};
