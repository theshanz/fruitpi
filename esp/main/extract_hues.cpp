// ─────────────────────────────────────────────────────────────────
//  extract_hues.cpp — ESP32 Camera Vision Feature Extraction
// ─────────────────────────────────────────────────────────────────
#include "extract_hues.h"
#include "config.h"
#include "img_converters.h"
#include "esp_heap_caps.h"
#include <cmath>
#include <algorithm>
#include <cstring>

constexpr float HUE_BIN_WIDTH = (HUE_WINDOW_MAX - HUE_WINDOW_MIN) / HUE_BIN_COUNT;
constexpr float UNIFORM_BIN = 1.0f / HUE_BIN_COUNT;
constexpr float EIGHT_MAX_VAR = 0.875f; // max sum-of-squares for an 8-bin histogram summing to 1

// Runtime-tunable gates (BLE "vision_config"); defaults from config.h.
float g_visionValueMin = VISION_VALUE_MIN;
float g_visionSatMin   = VISION_SAT_MIN;

// ─── RGB888 to HSV Conversion ───────────────────────────────────────
void HueExtractor::rgb888_to_hsv(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& v) {
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;

    float max_val = std::max({rf, gf, bf});
    float min_val = std::min({rf, gf, bf});
    float delta = max_val - min_val;

    v = max_val;

    if (max_val < 1e-5f) {
        s = 0.0f;
        h = 0.0f;
        return;
    }

    s = delta / max_val;

    if (delta < 1e-5f) {
        h = 0.0f;
        return;
    }

    if (max_val == rf) {
        h = 60.0f * fmodf(((gf - bf) / delta), 6.0f);
    } else if (max_val == gf) {
        h = 60.0f * (((bf - rf) / delta) + 2.0f);
    } else {
        h = 60.0f * (((rf - gf) / delta) + 4.0f);
    }

    if (h < 0.0f) {
        h += 360.0f;
    }
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

// ─── Pixel Unpack Helpers ─────────────────────────────────────────
static inline void unpack_rgb565(const uint8_t* p, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint16_t pixel = (uint16_t)((p[0] << 8) | p[1]);

    r = (uint8_t)(((pixel >> 11) & 0x1F) << 3); r |= (uint8_t)(r >> 2);
    g = (uint8_t)(((pixel >> 5) & 0x3F) << 2); g |= (uint8_t)(g >> 4);
    b = (uint8_t)((pixel & 0x1F) << 3);        b |= (uint8_t)(b >> 2);
}

// ─── Framebuffer Processing Engine ────────────────────────────────
ColorFeatures HueExtractor::process_frame(camera_fb_t* fb, float cm_per_pixel) {
    ColorFeatures features;
    memset(&features, 0, sizeof(ColorFeatures));

    // Safety Check: Null or invalid framebuffer
    if (!fb || !fb->buf || fb->len == 0) {
        // Fallback uniform distribution
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
        features.chromatic_dispersion = 1.0f;
        features.volume_cm3 = VOLUME_DEFAULT_CM3;
        return features;
    }

    uint32_t raw_counts[8] = {0};
    uint32_t total_valid_hue_pixels = 0;

    // Outlier-filtered volume: projection histograms of hue-window pixels
    // survive glare/shadow edges that a raw min/max bbox would chase. Held in
    // PSRAM (falls back to internal heap) so a full-resolution frame fits.
    uint32_t* proj_x = (uint32_t*)heap_caps_malloc(
        (size_t)fb->width * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!proj_x)
        proj_x = (uint32_t*)heap_caps_malloc(
            (size_t)fb->width * sizeof(uint32_t), MALLOC_CAP_8BIT);
    uint32_t* proj_y = (uint32_t*)heap_caps_malloc(
        (size_t)fb->height * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!proj_y)
        proj_y = (uint32_t*)heap_caps_malloc(
            (size_t)fb->height * sizeof(uint32_t), MALLOC_CAP_8BIT);
    if (proj_x) memset(proj_x, 0, (size_t)fb->width * sizeof(uint32_t));
    if (proj_y) memset(proj_y, 0, (size_t)fb->height * sizeof(uint32_t));

    // The camera subsystem captures JPEG by default. Decode it to RGB888
    // (held in PSRAM) so the rest of the pipeline sees raw pixels regardless
    // of the capture format. RGB565/other sources keep the direct path.
    uint8_t* decoded_rgb = nullptr;
    const uint8_t* buf = fb->buf;
    bool use_rgb888 = false;

    if (fb->format == PIXFORMAT_JPEG) {
        size_t rgb_len = (size_t)fb->width * fb->height * 3;
        decoded_rgb = (uint8_t*)heap_caps_malloc(rgb_len, MALLOC_CAP_SPIRAM);
        if (!decoded_rgb) {
            decoded_rgb = (uint8_t*)heap_caps_malloc(rgb_len, MALLOC_CAP_8BIT);
        }
        if (!decoded_rgb || !fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, decoded_rgb)) {
            if (decoded_rgb) heap_caps_free(decoded_rgb);
            for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
            features.chromatic_dispersion = 1.0f;
            features.volume_cm3 = VOLUME_DEFAULT_CM3;
            return features;
        }
        buf = decoded_rgb;
        use_rgb888 = true;
    }

    size_t total_pixels = fb->width * fb->height;

    for (size_t i = 0; i < total_pixels; i++) {
        uint8_t r, g, b;
        if (use_rgb888) {
            r = buf[0];
            g = buf[1];
            b = buf[2];
            buf += 3;
        } else {
            unpack_rgb565(buf, r, g, b);
            buf += 2;
        }

        float h, s, v;
        rgb888_to_hsv(r, g, b, h, s, v);

        // ─── Background / Noise Filtering ──────────────────────────
        // Filter out dark shadows (V < 0.15), white glare (S < 0.15), or black background
        if (v < g_visionValueMin || s < g_visionSatMin) {
            continue;
        }

        // ─── Bin hues across the configured window ────────────────────
        if (h >= HUE_WINDOW_MIN && h <= HUE_WINDOW_MAX) {
            int x = (int)(i % fb->width);
            int y = (int)(i / fb->width);

            int bin_idx = (int)((h - HUE_WINDOW_MIN) / HUE_BIN_WIDTH);
            if (bin_idx < 0) bin_idx = 0;
            if (bin_idx > 7) bin_idx = 7;

            raw_counts[bin_idx]++;
            total_valid_hue_pixels++;
            if (proj_x) proj_x[x]++;
            if (proj_y) proj_y[y]++;
        }
    }

    // ─── Normalize Histogram ───────────────────────────────────────
    if (total_valid_hue_pixels > 0) {
        float inv_total = 1.0f / (float)total_valid_hue_pixels;
        for (int i = 0; i < 8; i++) {
            features.hue_histogram[i] = raw_counts[i] * inv_total;
        }
    } else {
        // Safe uniform fallback if no fruit pixels matched 20°–120°
        for (int i = 0; i < 8; i++) {
            features.hue_histogram[i] = UNIFORM_BIN;
        }
    }

    // ─── Compute Chromatic Dispersion ─────────────────────────────
    features.chromatic_dispersion = calculate_chromatic_dispersion(features.hue_histogram);

    // ─── Outlier-Filtered Volume via Projection Histograms ──────────
    // The fruit must own at least 3% of the frame before we trust the bbox.
    // Bbox edges are placed where a projection histogram clears a 4-pixel
    // noise floor, so a single stray glare pixel can't inflate the volume.
    features.volume_cm3 = VOLUME_DEFAULT_CM3;
    if (proj_x && proj_y &&
        (float)total_valid_hue_pixels >= 0.03f * (float)total_pixels) {
        const uint32_t NOISE_FLOOR = 4;
        int min_x = -1, max_x = -1, min_y = -1, max_y = -1;
        for (int x = 0; x < fb->width; x++)
            if (proj_x[x] >= NOISE_FLOOR) {
                if (min_x < 0) min_x = x;
                max_x = x;
            }
        for (int y = 0; y < fb->height; y++)
            if (proj_y[y] >= NOISE_FLOOR) {
                if (min_y < 0) min_y = y;
                max_y = y;
            }
        if (min_x >= 0 && max_x > min_x && min_y >= 0 && max_y > min_y) {
            float width_cm  = (float)(max_x - min_x + 1) * cm_per_pixel;
            float height_cm = (float)(max_y - min_y + 1) * cm_per_pixel;

            // Orientation-invariant prolate spheroid: V = (PI/6) * min² * max
            float a = (width_cm < height_cm) ? width_cm : height_cm;
            float b = (width_cm < height_cm) ? height_cm : width_cm;
            constexpr float PI_OVER_6 = 0.5235987756f;
            features.volume_cm3 = PI_OVER_6 * (a * a) * b;

            // Sanity clamping for reasonable fruit sizes [10 cm³..500 cm³]
            if (features.volume_cm3 < VOLUME_CM3_MIN) features.volume_cm3 = VOLUME_CM3_MIN;
            if (features.volume_cm3 > VOLUME_CM3_MAX) features.volume_cm3 = VOLUME_CM3_MAX;
        }
    }

    if (proj_x) heap_caps_free(proj_x);
    if (proj_y) heap_caps_free(proj_y);
    if (decoded_rgb) {
        heap_caps_free(decoded_rgb);
    }

    return features;
}
