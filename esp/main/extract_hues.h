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
};
