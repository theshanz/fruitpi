// extract_hues.cpp — ESP32 Camera Vision Feature Extraction
#include "extract_hues.h"
#include <cmath>
#include <algorithm>
#include <cstring>

constexpr float EIGHT_MAX_VAR = 0.875f; // max sum-of-squared-deviations for an 8-bin histogram summing to 1

// ─── RGB888 to HSV Conversion ───────────────────────────────────────
void HueExtractor::rgb888_to_hsv(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& v) {
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;

    float max_val = std::max({rf, gf, bf});
    float min_val = std::min({rf, gf, bf});
    float delta = max_val - min_val;

    v = max_val;

    if (max_val < 1e-5f) { s = 0.0f; h = 0.0f; return; }

    s = delta / max_val;

    if (delta < 1e-5f) { h = 0.0f; return; }

    if (max_val == rf)      h = 60.0f * fmodf(((gf - bf) / delta), 6.0f);
    else if (max_val == gf) h = 60.0f * (((bf - rf) / delta) + 2.0f);
    else                    h = 60.0f * (((rf - gf) / delta) + 4.0f);

    if (h < 0.0f) h += 360.0f;
}

// ─── Chromatic Dispersion Math ─────────────────────────────────────
float HueExtractor::calculate_chromatic_dispersion(const float hue[8]) {
    float mean = 0.0f;
    for (int i = 0; i < 8; i++) mean += hue[i];
    mean /= 8.0f;

    float sum_sq = 0.0f;
    for (int i = 0; i < 8; i++) {
        float d = hue[i] - mean;
        sum_sq += d * d;
    }

    float dispersion = 1.0f - (sum_sq / EIGHT_MAX_VAR);
    if (dispersion < 0.0f) return 0.0f;
    if (dispersion > 1.0f) return 1.0f;
    return dispersion;
}

// ─── Framebuffer Processing Engine ────────────────────────────────
ColorFeatures HueExtractor::process_frame(camera_fb_t* fb, float cm_per_pixel) {
    ColorFeatures features;
    memset(&features, 0, sizeof(ColorFeatures));

    if (!fb || !fb->buf || fb->len == 0) {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = 0.125f;
        features.chromatic_dispersion = 1.0f;
        features.volume_cm3 = 50.0f;
        features.dark_count = 0;
        features.total_sampled = 0;
        features.valid = false;       // caller must check this before trusting anything above
        return features;
    }

    uint32_t raw_counts[8] = {0};
    uint32_t total_valid_hue_pixels = 0;
    uint32_t dark_count = 0;
    uint32_t total_sampled = 0;

    int min_x = fb->width, max_x = 0;
    int min_y = fb->height, max_y = 0;
    bool foreground_detected = false;

    size_t total_pixels = fb->width * fb->height;
    const uint8_t* buf = fb->buf;

    for (size_t i = 0; i < total_pixels; i++) {
        uint8_t byte0 = buf[i * 2];
        uint8_t byte1 = buf[i * 2 + 1];
        uint16_t pixel = (byte0 << 8) | byte1;

        uint8_t r = (pixel >> 11) & 0x1F;
        uint8_t g = (pixel >> 5) & 0x3F;
        uint8_t b = pixel & 0x1F;

        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);

        float h, s, v;
        rgb888_to_hsv(r, g, b, h, s, v);

        total_sampled++;

        // Dark pixels are a rot/shadow signal — count them, don't just discard them
        if (v < 0.15f) {
            dark_count++;
            continue;
        }
        // Glare (washed-out/gray) is a separate phenomenon from rot — exclude, don't count as dark
        if (s < 0.15f) {
            continue;
        }

        int x = i % fb->width;
        int y = i / fb->width;

        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
        foreground_detected = true;

        if (h >= 20.0f && h <= 120.0f) {
            int bin_idx = (int)((h - 20.0f) / 12.5f);
            if (bin_idx < 0) bin_idx = 0;
            if (bin_idx > 7) bin_idx = 7;
            raw_counts[bin_idx]++;
            total_valid_hue_pixels++;
        }
    }

    if (total_valid_hue_pixels > 0) {
        float inv_total = 1.0f / (float)total_valid_hue_pixels;
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = raw_counts[i] * inv_total;
    } else {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = 0.125f;
    }

    features.chromatic_dispersion = calculate_chromatic_dispersion(features.hue_histogram);
    features.dark_count = dark_count;
    features.total_sampled = total_sampled;

    if (foreground_detected && max_x > min_x && max_y > min_y) {
        float width_px  = (float)(max_x - min_x + 1);
        float height_px = (float)(max_y - min_y + 1);
        float width_cm  = width_px * cm_per_pixel;
        float height_cm = height_px * cm_per_pixel;

        constexpr float PI_OVER_6 = 0.5235987756f;
        features.volume_cm3 = PI_OVER_6 * (width_cm * width_cm) * height_cm;

        if (features.volume_cm3 < 10.0f) features.volume_cm3 = 10.0f;
        if (features.volume_cm3 > 500.0f) features.volume_cm3 = 500.0f;
    } else {
        features.volume_cm3 = 50.0f;
    }

    features.valid = true;
    return features;
}

// ─── Ripeness / Health Classification ─────────────────────────────
// Transparent decision logic, not a trained model — every branch is explainable.
// All thresholds below are placeholders: tune them in Week 5 against your
// 15-20 known-ripeness mangoes, the same way the acoustic thresholds get tuned.
VisionVerdict HueExtractor::classify_vision(const ColorFeatures& f) {
    VisionVerdict v{};

    if (!f.valid) {
        v.status = RipenessStatus::ROTTING;   // fail safe, not fail silent
        v.score = 0.0f;
        v.dark_fraction = 0.0f;
        return v;
    }

    v.dark_fraction = f.total_sampled > 0 ? (float)f.dark_count / (float)f.total_sampled : 0.0f;

    float green_weight  = f.hue_histogram[0] + f.hue_histogram[1];                         // ~20-45°
    float yellow_weight = f.hue_histogram[5] + f.hue_histogram[6] + f.hue_histogram[7];    // ~82.5-120°

    if (v.dark_fraction > 0.15f) {
        // Dark/rotten coverage dominates — overrides hue entirely, regardless of how
        // good the remaining skin looks.
        v.status = RipenessStatus::ROTTING;
        v.score = 20.0f * (1.0f - v.dark_fraction);
    } else if (green_weight > yellow_weight) {
        v.status = RipenessStatus::NOT_RIPE;
        v.score = 55.0f;
    } else {
        v.status = RipenessStatus::RIPE_GOOD;
        v.score = 90.0f + 10.0f * (yellow_weight - green_weight);
    }

    if (v.score < 0.0f)   v.score = 0.0f;
    if (v.score > 100.0f) v.score = 100.0f;
    return v;
}
