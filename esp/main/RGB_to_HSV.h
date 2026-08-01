// extract_hues.h
#pragma once
#include "esp_camera.h"
#include <cstdint>

struct ColorFeatures {
    float hue_histogram[8];      // fraction of hue-qualifying pixels per 12.5°-wide bucket (20°-120° range)
    float chromatic_dispersion;  // 0 = one hue dominates, 1 = hues spread across many buckets
    float volume_cm3;            // rough size estimate from bounding box
    uint32_t dark_count;         // pixels excluded for being too dark (rot/shadow signal)
    uint32_t total_sampled;      // total pixels considered, for computing dark_count as a fraction
    bool valid;                  // false if the frame was unusable — check before trusting the rest
};

enum class RipenessStatus { NOT_RIPE, RIPE_GOOD, ROTTING };

struct VisionVerdict {
    RipenessStatus status;
    float score;          // 0-100, feeds into the composite formula alongside the acoustic firmness score
    float dark_fraction;  // exposed for debugging/calibration, not just internal use
};

class HueExtractor {
public:
    static ColorFeatures process_frame(camera_fb_t* fb, float cm_per_pixel);
    static VisionVerdict classify_vision(const ColorFeatures& f);

private:
    static void rgb888_to_hsv(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& v);
    static float calculate_chromatic_dispersion(const float hue[8]);
};
