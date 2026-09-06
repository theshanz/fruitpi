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
#include <limits>
#include <vector>

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
    uint16_t pixel = (uint16_t)((p[1] << 8) | p[0]);

    r = (uint8_t)(((pixel >> 11) & 0x1F) << 3); r |= (uint8_t)(r >> 2);
    g = (uint8_t)(((pixel >> 5) & 0x3F) << 2); g |= (uint8_t)(g >> 4);
    b = (uint8_t)((pixel & 0x1F) << 3);        b |= (uint8_t)(b >> 2);
}

// Max-channel value (0..1) of an RGB565 flash pixel — the V floor for the
// centre-prior seed gate. Same luminance-agnostic V as rgb888_to_hsv.
static inline float flash_v565(const uint16_t* px) {
    uint8_t r, g, b;
    unpack_rgb565((const uint8_t*)px, r, g, b);
    uint8_t m = r > g ? r : g;
    if (b > m) m = b;
    return (float)m / 255.0f;
}

// Percentile of a seed sample using nth_element (k-th order statistic).
template <typename T>
static float seed_percentile(std::vector<T>& vals, float p) {
    if (vals.empty()) return 0.0f;
    size_t k = (size_t)(p * (float)(vals.size() - 1));
    if (k >= vals.size()) k = vals.size() - 1;
    std::nth_element(vals.begin(), vals.begin() + k, vals.end());
    return (float)vals[k];
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

// Sobel gradient magnitude |∇ΔY| at (x,y), edge-clamped (0-pad by clamping, a
// frame-edge fruit is handled by the interior-high-gradient cut; matches the
// scipy.ndimage.sobel kernel, unscaled like the host reference).
static float sobel_gmag(const uint8_t* dy, uint16_t w, uint16_t h, int x, int y) {
    auto at = [&](int xx, int yy) -> int {
        if (xx < 0) xx = 0; else if (xx >= (int)w) xx = (int)w - 1;
        if (yy < 0) yy = 0; else if (yy >= (int)h) yy = (int)h - 1;
        return dy[(size_t)yy * w + (size_t)xx];
    };
    // gx = (c[x+1] - c[x-1]) with [1,2,1] vertical weight (d/dx)
    int gx = (at(x+1,y-1) + 2*at(x+1,y) + at(x+1,y+1))
           - (at(x-1,y-1) + 2*at(x-1,y) + at(x-1,y+1));
    // gy = (c[y+1] - c[y-1]) with [1,2,1] horizontal weight (d/dy)
    int gy = (at(x-1,y+1) + 2*at(x,y+1) + at(x+1,y+1))
           - (at(x-1,y-1) + 2*at(x,y-1) + at(x+1,y-1));
    return sqrtf((float)(gx * gx) + (float)(gy * gy));
}

// Gradient Notch (must run BEFORE select_largest_centre_blob). core_mask holds
// the thresholded foreground; dy_map is the ΔY luma map. Computes the Sobel
// magnitude on ΔY, derives a fovea-adaptive cut threshold from the central seed
// box (the same box that produced the ΔY/V gates), and unconditionally zeroes
// every foreground pixel whose gradient exceeds it. If the cut removes more
// than GRAD_NOTCH_MAX_REMOVAL_FRAC of the fg, or leaves fewer than
// FLASH_MIN_OBJECT_PIXELS, the raw foreground is restored (safety): the notch
// is correct on all validated bundles, but a degenerate capture must never
// collapse to an empty or shredded mask.
// Foveal Center-Box hue extraction — simple, fool-proof.
// ROI: x [110..210], y [78..178] (around cx=160, cy=128).
// Only pixels with dY >= 14 and V >= 0.15 survive. Skin rejection: H [15..35], S <= 0.50.
// 10-bin histogram [20°..155°]. If valid pixels < 250 return None (fg=0).
// fg_pixels_out: surviving box pixel count (gates fruit detection >= 400).
ColorFeatures process_foveal_box(const uint16_t* flash, const uint16_t* ambient,
                                 uint16_t w, uint16_t h, float cm_per_pixel,
                                 uint32_t* fg_pixels_out) {
    ColorFeatures features;
    memset(&features, 0, sizeof(ColorFeatures));

    constexpr int X0 = 110, X1 = 210, Y0 = 78, Y1 = 178;
    constexpr float MIN_DY = 14.0f;
    constexpr float MIN_V = 0.15f;
    constexpr int MIN_PIXELS = 250;

    float hist[10] = {0};
    int valid_pixels = 0;

    for (int y = Y0; y <= Y1; y++) {
        for (int x = X0; x <= X1; x++) {
            size_t idx = (size_t)y * w + (size_t)x;
            uint8_t r1, g1, b1, r2, g2, b2;
            unpack_rgb565((const uint8_t*)(flash + idx), r1, g1, b1);
            unpack_rgb565((const uint8_t*)(ambient + idx), r2, g2, b2);

            float h=0, s=0, v=0;
            HueExtractor::rgb888_to_hsv(r1, g1, b1, h, s, v);
            float v_flash = v;
            float dy = std::abs((float)r1 - (float)r2) + std::abs((float)g1 - (float)g2) + std::abs((float)b1 - (float)b2);
            float dY = dy / 3.0f; // approximate luminance diff

            // Saturation floor + value floor + dY gate
            if (dY < MIN_DY || v_flash < MIN_V || s < 0.25f || v < 0.25f) continue;

            // Skin rejection
            if (h >= 15.0f && h <= 35.0f && s <= 0.50f) continue;

            valid_pixels++;

            // 10-bin histogram [20°..155°]
            int bin = (int)((h - 20.0f) / 13.5f); // (155-20)/10 = 13.5° per bin
            if (bin < 0) bin = 0;
            if (bin > 9) bin = 9;
            hist[bin] += 1.0f;
        }
    }

    if (valid_pixels < MIN_PIXELS) {
        if (fg_pixels_out) *fg_pixels_out = 0; // no fruit in the box
        features.hue_histogram[0] = 0.0f; // fg = 0 signal
        return features;
    }

    if (fg_pixels_out) *fg_pixels_out = (uint32_t)valid_pixels;

    float total = 0.0f;
    for (int i = 0; i < 10; i++) total += hist[i];
    if (total > 0.0f) {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = hist[i] / total; // normalize to 8 bins for compatibility
    } else {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = 0.125f;
    }
    features.chromatic_dispersion = 1.0f; // uniform assumption for box
    features.volume_cm3 = VOLUME_DEFAULT_CM3;
    return features;
}

static void apply_gradient_notch(uint8_t* core_mask, const uint8_t* dy_map,
                                 uint16_t w, uint16_t h,
                                 int seed_x0, int seed_y0, int rw, int rh) {
    const size_t total = (size_t)w * h;

    // Pass 1: foveal |∇ΔY| statistics (μ_fov, σ_fov) + raw foreground count.
    double fov_sum = 0.0, fov_sq = 0.0;
    uint32_t fov_n = 0, raw_fg = 0;
    for (int y = seed_y0; y <= seed_y0 + 2 * rh; y++)
        for (int x = seed_x0; x <= seed_x0 + 2 * rw; x++) {
            float g = sobel_gmag(dy_map, w, h, x, y);
            fov_sum += (double)g; fov_sq += (double)g * (double)g; fov_n++;
        }
    for (size_t i = 0; i < total; i++) raw_fg += core_mask[i];

    float mu_fov = (float)(fov_sum / fov_n);
    float var_fov = (float)(fov_sq / fov_n) - mu_fov * mu_fov;
    if (var_fov < 0.0f) var_fov = 0.0f;
    float t_grad = mu_fov + FOVEAL_GRAD_K * sqrtf(var_fov);

    // Pass 2: count foreground pixels the notch would cut (no mutation yet).
    uint32_t notch_cut = 0;
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) {
            size_t i = y * w + x;
            if (core_mask[i] && sobel_gmag(dy_map, w, h, (int)x, (int)y) > t_grad) notch_cut++;
        }

    uint32_t notch_fg = raw_fg - notch_cut;
    if (raw_fg == 0) return;
    float removal_frac = (float)notch_cut / (float)raw_fg;
    if (notch_fg < (uint32_t)FLASH_MIN_OBJECT_PIXELS || removal_frac > GRAD_NOTCH_MAX_REMOVAL_FRAC)
        return;  // safety: keep the raw foreground

    // Pass 3: apply the 0px cut.
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) {
            size_t i = y * w + x;
            if (core_mask[i] && sobel_gmag(dy_map, w, h, (int)x, (int)y) > t_grad) core_mask[i] = 0;
        }
}

// Largest-blob centre-selection (replaces the single-pixel centre seed).
// core_mask is a uint8 foreground map (1 == candidate). Labels every 4-connected
// component, then selects the one to keep:
//   largest_component sets the size bar; among all components >= CENTRE_SIZE_RATIO
//   (0.50) of the largest, pick the one whose centroid is closest to the frame
//   centre. This keeps a central fruit while surviving off-centre fruit and
//   pruning decorative glare. The chosen component is written into out_mask
//   (0 / 1) and its pixel count returned.
static uint32_t select_largest_centre_blob(const uint8_t* core_mask, uint8_t* out_mask,
                                           uint16_t w, uint16_t h,
                                           float centre_size_ratio = 0.50f,
                                           float centre_sigma = MS_CENTRE_BIAS_SIGMA) {
    const size_t total = (size_t)w * h;
    memset(out_mask, 0, total);

    uint16_t* label = (uint16_t*)heap_caps_malloc(total * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!label) label = (uint16_t*)heap_caps_malloc(total * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!label) return 0;
    memset(label, 0, total * sizeof(uint16_t));

    // Worst case a fully foreground frame is one component; sizes index [1..ncomp].
    uint32_t* sizes = (uint32_t*)heap_caps_malloc((total + 1) * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!sizes) sizes = (uint32_t*)heap_caps_malloc((total + 1) * sizeof(uint32_t), MALLOC_CAP_8BIT);
    if (!sizes) {
        heap_caps_free(label);
        return 0;
    }

    uint32_t* stack = (uint32_t*)heap_caps_malloc(total * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!stack) stack = (uint32_t*)heap_caps_malloc(total * sizeof(uint32_t), MALLOC_CAP_8BIT);

    uint16_t ncomp = 0;
    for (size_t i = 0; i < total; i++) {
        if (!core_mask[i] || label[i]) continue;
        uint16_t cur = ++ncomp;
        label[i] = cur;
        uint32_t comp_count = 1;
        if (stack) {
            size_t sp = 0;
            stack[sp++] = (uint32_t)i;
            while (sp > 0) {
                size_t p = (size_t)stack[--sp];
                int py = (int)(p / w), px = (int)(p % w);
                if (px > 0 && core_mask[p - 1] && !label[p - 1]) { label[p - 1] = cur; stack[sp++] = (uint32_t)(p - 1); comp_count++; }
                if ((size_t)(px + 1) < (size_t)w && core_mask[p + 1] && !label[p + 1]) { label[p + 1] = cur; stack[sp++] = (uint32_t)(p + 1); comp_count++; }
                if (py > 0 && core_mask[p - w] && !label[p - w]) { label[p - w] = cur; stack[sp++] = (uint32_t)(p - w); comp_count++; }
                if ((size_t)(py + 1) < (size_t)h && core_mask[p + w] && !label[p + w]) { label[p + w] = cur; stack[sp++] = (uint32_t)(p + w); comp_count++; }
            }
        }
        sizes[cur] = comp_count;
    }

    if (ncomp == 0) {
        if (stack) heap_caps_free(stack);
        heap_caps_free(label);
        heap_caps_free(sizes);
        return 0;
    }

    uint32_t largest = 0;
    for (uint16_t c = 1; c <= ncomp; c++) if (sizes[c] > largest) largest = sizes[c];
    (void)largest;  // sizes retained for reporting; selection below is score-based
    const double inv2sig2 = 0.5 / ((double)centre_sigma * (double)centre_sigma);

    // Score every component as  size * exp(-d² / (2σ²)),  d = bbox-midpoint
    // distance to the frame centre. A large background blob anchored to a frame
    // edge (e.g. a warm-coloured chair) carries a tiny gaussian weight and is
    // pruned; a fruit within ~one σ of centre wins even if not the largest. The
    // oversized size term keeps a large off-centre fruit ahead of a small
    // exactly-centred blob when both sit well away from the middle.
    int best_score_is = -1;   // fixed-point score (kept as scaled int to avoid division)
    double best_score = -1.0;
    uint16_t chosen = 0;
    uint32_t chosen_size = 0;
    for (uint16_t c = 1; c <= ncomp; c++) {
        int min_x = (int)w - 1, max_x = 0, min_y = (int)h - 1, max_y = 0;
        for (size_t i = 0; i < total; i++) {
            if (label[i] == c) {
                int ix = (int)(i % w), iy = (int)(i / w);
                if (ix < min_x) min_x = ix;
                if (ix > max_x) max_x = ix;
                if (iy < min_y) min_y = iy;
                if (iy > max_y) max_y = iy;
            }
        }
        int cxm = (min_x + max_x) / 2, cym = (min_y + max_y) / 2;
        int ddx = cxm - (int)(w / 2), ddy = cym - (int)(h / 2);
        double d2 = (double)ddx * (double)ddx + (double)ddy * (double)ddy;
        double score = (double)sizes[c] * exp(-d2 * inv2sig2);
        if (chosen == 0 || score > best_score) {
            best_score = score;
            chosen = c;
            chosen_size = sizes[c];
            best_score_is = (int)(score * 1024.0);
        }
    }
    (void)best_score_is;

    if (stack) heap_caps_free(stack);
    heap_caps_free(label);
    if (chosen == 0) {
        heap_caps_free(sizes);
        return 0;
    }

    uint32_t out = 0;
    for (size_t i = 0; i < total; i++) {
        if (label[i] == chosen) { out_mask[i] = 1; out++; }
    }
    heap_caps_free(sizes);
    return out ? chosen_size : 0;
}

// ─── Natural-ellipse fit + frame-overflow signal ──────────────────
// Fits the intrinsic oval of a chosen binary mask via 2nd central image moments
// (covariance of foreground pixel positions). Returns the major/minor axis
// lengths in px, the centroid, and a per-edge overflow flag so a fruit cropped
// by the frame (volume is then a lower bound) is detectable. Filling more than
// OVERFLOW_EDGE_FRAC of a dimension is treated as "hugging that edge".
static void fit_fruit_ellipse(const uint8_t* mask, uint16_t w, uint16_t h,
                              float* major_px, float* minor_px,
                              float* cx, float* cy, uint8_t* overflow) {
    const size_t total = (size_t)w * h;
    uint64_t n = 0, sx = 0, sy = 0;
    for (size_t i = 0; i < total; i++) {
        if (!mask[i]) continue;
        n++;
        sx += (uint64_t)(i % w);
        sy += (uint64_t)(i / w);
    }
    *major_px = 0.0f; *minor_px = 0.0f; *cx = 0.0f; *cy = 0.0f; *overflow = 0;
    if (n < 4) return;
    const double mx = (double)sx / (double)n;
    const double my = (double)sy / (double)n;
    double u20 = 0.0, u02 = 0.0, u11 = 0.0;
    for (size_t i = 0; i < total; i++) {
        if (!mask[i]) continue;
        double dx = (double)(i % w) - mx;
        double dy = (double)(i / w) - my;
        u20 += dx * dx;
        u02 += dy * dy;
        u11 += dx * dy;
    }
    u20 /= (double)n; u02 /= (double)n; u11 /= (double)n;
    // Eigenvalues of the 2x2 central-moment matrix give the squared semi-axes.
    const double trace = u20 + u02;
    const double disc = sqrt((u20 - u02) * (u20 - u02) + 4.0 * u11 * u11);
    const double lam1 = (trace + disc) * 0.5;   // largest variance direction
    const double lam2 = (trace - disc) * 0.5;   // smallest
    // Convert std-deviations to ellipse semi-axes of an equivalent 2D gaussian
    // enclosing ~99.7% of the mass: radius ~3*sigma per axis.
    const double s1 = sqrt(fmax(lam1, 1e-9));
    const double s2 = sqrt(fmax(lam2, 1e-9));
    *major_px = (float)(3.0 * s1);
    *minor_px = (float)(3.0 * s2);
    *cx = (float)mx;
    *cy = (float)my;

    // Overflow: did the fruit touch / nearly fill any frame edge?
    int min_x = w, max_x = -1, min_y = h, max_y = -1;
    for (size_t i = 0; i < total; i++) {
        if (!mask[i]) continue;
        int ix = (int)(i % w), iy = (int)(i / w);
        if (ix < min_x) min_x = ix;
        if (ix > max_x) max_x = ix;
        if (iy < min_y) min_y = iy;
        if (iy > max_y) max_y = iy;
    }
    constexpr float EDGE_FRAC = 0.02f;  // within 2% of an edge => hugging it
    if (min_x <= (int)(EDGE_FRAC * w)) *overflow |= OVERFLOW_LEFT;
    if (max_x >= (int)(w - 1) - (int)(EDGE_FRAC * w)) *overflow |= OVERFLOW_RIGHT;
    if (min_y <= (int)(EDGE_FRAC * h)) *overflow |= OVERFLOW_TOP;
    if (max_y >= (int)(h - 1) - (int)(EDGE_FRAC * h)) *overflow |= OVERFLOW_BOTTOM;
}

// Opposite-border glare rejection: overflow bits set for BOTH a row pair
// (top+bottom) or column pair (left+right) mean the segmented blob spans the
// full sensor — a dark-room flash flood / glossy-glare band that no real fruit
// can span. Mirrors the canonical final_pipeline._segment rule (single source).
static bool spans_opposite_borders(uint8_t overflow) {
    return ((overflow & (OVERFLOW_TOP | OVERFLOW_BOTTOM)) == (OVERFLOW_TOP | OVERFLOW_BOTTOM))
        || ((overflow & (OVERFLOW_LEFT | OVERFLOW_RIGHT)) == (OVERFLOW_LEFT | OVERFLOW_RIGHT));
}

// Bounds an already-selected binary mask to the natural oval estimated from its
// 2nd central moments (3-sigma covariance ellipse, rotated to the major axis).
// This prunes same-colour background arms that radiate to the frame edges (a
// green mango held against a green wall/table merges into one component at the
// differential gate; the dense central fruit dominates the covariance, so the
// 3-sigma ellipse trims the diffuse peripheral background and leaves the fruit).
static uint32_t crop_mask_to_ellipse(uint8_t* mask, uint16_t w, uint16_t h) {
    const size_t total = (size_t)w * h;
    uint64_t n = 0, sx = 0, sy = 0;
    for (size_t i = 0; i < total; i++) { if (!mask[i]) continue; n++; sx += (uint64_t)(i % w); sy += (uint64_t)(i / w); }
    if (n < 4) return (uint32_t)n;
    const double mx = (double)sx / (double)n;
    const double my = (double)sy / (double)n;
    double u20 = 0.0, u02 = 0.0, u11 = 0.0;
    for (size_t i = 0; i < total; i++) {
        if (!mask[i]) continue;
        double dx = (double)(i % w) - mx, dy = (double)(i / w) - my;
        u20 += dx * dx; u02 += dy * dy; u11 += dx * dy;
    }
    u20 /= (double)n; u02 /= (double)n; u11 /= (double)n;
    // Semi-axes: 3-sigma (computed x2 for the full axis length denominator).
    const double trace = u20 + u02;
    const double disc = sqrt((u20 - u02) * (u20 - u02) + 4.0 * u11 * u11);
    const double L1 = (trace + disc) * 0.5, L2 = (trace - disc) * 0.5;
    double a = 1.5 * sqrt(fmax(L1, 1e-9));   // (3σ)→full-axis uses 3, so semi = 1.5
    double b = 1.5 * sqrt(fmax(L2, 1e-9));
    if (a < 1e-6 || b < 1e-6) return (uint32_t)n;
    const double ang = 0.5 * atan2(2.0 * u11, (u20 - u02));
    const double co = cos(ang), si = sin(ang);
    const double ia = 1.0 / a / a, ib = 1.0 / b / b;
    uint32_t out = 0;
    for (size_t i = 0; i < total; i++) {
        if (!mask[i]) continue;
        const double dx = (double)(i % w) - mx, dy = (double)(i / w) - my;
        const double u =  co * dx + si * dy;   // aligned-frame coords
        const double v = -si * dx + co * dy;
        if (u * u * ia + v * v * ib <= 1.0) { out++; }
        else { mask[i] = 0; }
    }
    return out;
}

// ─── Flash/No-Flash Differential Segmentation ─────────────────────
ColorFeatures HueExtractor::process_differential(const uint16_t* flash, const uint16_t* ambient,
                                                  uint16_t w, uint16_t h, float cm_per_pixel,
                                                  uint32_t* fg_pixels_out,
                                                  int* bbox_x_out, int* bbox_y_out,
                                                  int* bbox_w_out, int* bbox_h_out,
                                                  uint32_t* core_pixels_out,
                                                  uint32_t* snap_pixels_out) {
    ColorFeatures features;
    memset(&features, 0, sizeof(ColorFeatures));
    if (fg_pixels_out) *fg_pixels_out = 0;
    if (bbox_x_out) *bbox_x_out = 0;
    if (bbox_y_out) *bbox_y_out = 0;
    if (bbox_w_out) *bbox_w_out = 0;
    if (bbox_h_out) *bbox_h_out = 0;
    if (core_pixels_out) *core_pixels_out = 0;
    if (snap_pixels_out) *snap_pixels_out = 0;

    if (!flash || !ambient || w == 0 || h == 0) {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
        features.chromatic_dispersion = 1.0f;
        features.volume_cm3 = VOLUME_DEFAULT_CM3;
        return features;
    }

    size_t total_pixels = (size_t)w * h;

    // Per-pixel flash-vs-ambient ΔY map, immutable core mask and final
    // edge-refined mask (1 byte/px each, PSRAM, fallback internal heap).
    uint8_t* dy_map = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_SPIRAM);
    if (!dy_map) dy_map = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_8BIT);
    if (!dy_map) {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
        features.chromatic_dispersion = 1.0f;
        features.volume_cm3 = VOLUME_DEFAULT_CM3;
        return features;
    }
    uint8_t* core_mask = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_SPIRAM);
    if (!core_mask) core_mask = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_8BIT);
    if (!core_mask) {
        heap_caps_free(dy_map);
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
        features.chromatic_dispersion = 1.0f;
        features.volume_cm3 = VOLUME_DEFAULT_CM3;
        return features;
    }
    uint8_t* out_mask = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_SPIRAM);
    if (!out_mask) out_mask = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_8BIT);
    if (!out_mask) {
        heap_caps_free(dy_map);
        heap_caps_free(core_mask);
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
        features.chromatic_dispersion = 1.0f;
        features.volume_cm3 = VOLUME_DEFAULT_CM3;
        return features;
    }

    // ΔY map from the RGB565 channels (Rec. 601 luma bump per pixel, clamped).
    for (size_t i = 0; i < total_pixels; i++) {
        uint8_t fr, fg, fb, ar, ag, ab;
        unpack_rgb565((const uint8_t*)&flash[i], fr, fg, fb);
        unpack_rgb565((const uint8_t*)&ambient[i], ar, ag, ab);
        int fy = ((int)fr * 299 + (int)fg * 587 + (int)fb * 114) / 1000;
        int ay = ((int)ar * 299 + (int)ag * 587 + (int)ab * 114) / 1000;
        int d = fy - ay;
        if (d < 0) d = 0;
        if (d > 255) d = 255;
        dy_map[i] = (uint8_t)d;
    }

    // Segmentation (latest 2026-09-04): the foreground mask is thresholded with
    // ADAPTIVE ΔY/V derived from a central foveal seed box (so a dark mango under
    // flash is no longer discarded by a fixed EDGE_CORE_DY gate), then the
    // largest connected component is selected with a centre-proximity tiebreak
    // among comparable blobs. This recovers the full fruit even when it is
    // off-centre / overflows the frame, unlike the prior single-pixel centre
    // seed that collapsed to a minor seed box on glossy mango.
    uint32_t core_count = 0;
    uint32_t snap_count = 0;
    float seed_v_min = CENTRE_SEED_V_MIN;   // hoisted: reused by the hue V gate below

    int se = w / 2, cty = h / 2;
    int rw = (int)((float)w * CENTRE_SEED_FRACTION);
    int rh = (int)((float)h * CENTRE_SEED_FRACTION);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    int seed_x0 = se - rw, seed_y0 = cty - rh;
    if (seed_x0 < 0) seed_x0 = 0;
    if (seed_x0 + rw + 1 > (int)w) seed_x0 = (int)w - rw - 1;
    if (seed_y0 < 0) seed_y0 = 0;
    if (seed_y0 + rh + 1 > (int)h) seed_y0 = (int)h - rh - 1;

    {
        std::vector<uint8_t> seed_dy;
        seed_dy.reserve((size_t)(2 * rw + 1) * (2 * rh + 1));
        std::vector<float> seed_v;
        seed_v.reserve((size_t)(2 * rw + 1) * (2 * rh + 1));
        for (int y = seed_y0; y <= seed_y0 + 2 * rh; y++) {
            for (int x = seed_x0; x <= seed_x0 + 2 * rw; x++) {
                size_t i = (size_t)y * w + (size_t)x;
                seed_dy.push_back(dy_map[i]);
                seed_v.push_back(flash_v565(&flash[i]));
            }
        }
        int seed_dy_thresh = (int)seed_percentile(seed_dy, 0.25f);
        if (seed_dy_thresh < CENTRE_SEED_DY_MIN) seed_dy_thresh = CENTRE_SEED_DY_MIN;
        seed_v_min = seed_percentile(seed_v, 0.10f);
        if (seed_v_min < CENTRE_SEED_V_MIN) seed_v_min = CENTRE_SEED_V_MIN;

        // Candidate foreground = adaptive ΔY && adaptive flash-V, into core_mask.
        uint8_t dy_t = (uint8_t)seed_dy_thresh;
        for (size_t i = 0; i < total_pixels; i++) {
            uint8_t cand = (dy_map[i] >= dy_t && flash_v565(&flash[i]) >= seed_v_min) ? 1 : 0;
            core_mask[i] = cand;
            out_mask[i] = 0;   // visited / output, initially empty
        }

        // Strict foveal fail-fast (item 2, mirrors final_pipeline._FOVEAL_FG_MIN):
        // the central foveal seed box must itself yield a confident fruit-like
        // foreground. Edge/border washes read 0-140 foveal px and are reported
        // empty instead of being guessed at by the notch/CCL machinery.
        uint32_t foveal_fg = 0;
        for (int y = seed_y0; y <= seed_y0 + 2 * rh; y++)
            for (int x = seed_x0; x <= seed_x0 + 2 * rw; x++)
                foveal_fg += core_mask[(size_t)y * w + (size_t)x];
        if (foveal_fg < (uint32_t)MS_FOVEAL_FG_MIN) {
            for (int i = 0; i < 8; i++) features.hue_histogram[i] = 0.0f;
            features.chromatic_dispersion = 1.0f;
            features.volume_cm3 = 0.0f;
            heap_caps_free(dy_map);
            heap_caps_free(core_mask);
            heap_caps_free(out_mask);
            return features;
        }

        // Select the foreground component: the largest blob, with a centre
        // proximity tiebreak among comparable-sized blobs (>=50% of the largest).
        // This replaces the single-pixel centre seed, which collapsed to a tiny
        // seed box whenever the fruit's glossy near-edge landed on frame centre
        // (the 2026-09-04 live green-mango bug: 12,310px / 40,914px fruit lost to
        // a 4-col seed box because centre dy was glare-low).
        apply_gradient_notch(core_mask, dy_map, w, h, seed_x0, seed_y0, rw, rh);
        select_largest_centre_blob(core_mask, out_mask, w, h);

        // Bound the selected blob to its natural oval: prunes same-colour
        // background that the differential gate merges in (a green mango held
        // against a flash-lit green wall/table), so a fruit that truly fits the
        // frame is not falsely flagged as overrunning it.
        crop_mask_to_ellipse(out_mask, w, h);
    }

    for (size_t i = 0; i < total_pixels; i++) core_count += out_mask[i];
    if (core_pixels_out) *core_pixels_out = core_count;
    if (snap_pixels_out) *snap_pixels_out = snap_count;

    // ─── Foreground stats for volume (from the refined mask bbox) ──
    uint32_t fg_count = 0;
    int min_x = -1, max_x = -1, min_y = -1, max_y = -1;
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            if (out_mask[y * w + x]) {
                fg_count++;
                if (min_x < 0 || (int)x < min_x) min_x = (int)x;
                if ((int)x > max_x) max_x = (int)x;
                if (min_y < 0 || (int)y < min_y) min_y = (int)y;
                if ((int)y > max_y) max_y = (int)y;
            }
        }
    }

    // Empty scene (no credible fruit in flash range): report zero bbox, zero
    // volume and an all-zero hue histogram instead of the 350 cm³ / uniform
    // fallback that previously produced a meaningless max-clamped volume.
    // fg_pixels/core/snap/bbox out-params keep their zero defaults.
    if (fg_count < (uint32_t)FLASH_MIN_OBJECT_PIXELS) {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = 0.0f;
        features.chromatic_dispersion = 1.0f;  // "no data", matching zero bins
        features.volume_cm3 = 0.0f;
        heap_caps_free(dy_map);
        heap_caps_free(core_mask);
        heap_caps_free(out_mask);
        return features;
    }

    features.volume_cm3 = VOLUME_DEFAULT_CM3;
    if (min_x >= 0 && max_x > min_x && min_y >= 0 && max_y > min_y) {
        float width_cm  = (float)(max_x - min_x + 1) * cm_per_pixel;
        float height_cm = (float)(max_y - min_y + 1) * cm_per_pixel;
        float a = (width_cm < height_cm) ? width_cm : height_cm;
        float b = (width_cm < height_cm) ? height_cm : width_cm;
        constexpr float PI_OVER_6 = 0.5235987756f;
        features.volume_cm3 = PI_OVER_6 * (a * a) * b;
        if (features.volume_cm3 < VOLUME_CM3_MIN) features.volume_cm3 = VOLUME_CM3_MIN;
        if (features.volume_cm3 > VOLUME_CM3_MAX) features.volume_cm3 = VOLUME_CM3_MAX;
    }

    // ─── Natural-ellipse fit + overflow signal (drives the "fits frame?" flag) ─
    // The bbox volume above is an axis-aligned estimate. The intrinsic oval from
    // image moments is closer to the true fruit silhouette and survives frame
    // cropping, so when the fruit is wholly in view we refine the volume with
    // the natural ellipse (prolate spheroid of its major/minor axes). When the
    // fruit overruns an edge we keep the bbox bound but flag which edges, so the
    // caller (BLE/UI) can warn "fruit exceeds frame" and treat the volume as a
    // lower bound.
    float emaj = 0.0f, emin = 0.0f, ecx = 0.0f, ecy = 0.0f;
    uint8_t overflow = 0;
    if (fg_count >= (uint32_t)FLASH_MIN_OBJECT_PIXELS) {
        fit_fruit_ellipse(out_mask, w, h, &emaj, &emin, &ecx, &ecy, &overflow);
    }
    features.ellipse_major_px = emaj;
    features.ellipse_minor_px = emin;
    features.centroid_px[0] = ecx;
    features.centroid_px[1] = ecy;
    features.overflow_mask = overflow;
    // Opposite-border glare rejection: wall/table glare spans the full sensor.
    if (spans_opposite_borders(overflow)) {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = 0.0f;
        features.chromatic_dispersion = 1.0f;
        features.volume_cm3 = 0.0f;
        heap_caps_free(dy_map);
        heap_caps_free(core_mask);
        heap_caps_free(out_mask);
        return features;
    }
    if (overflow == 0 && emaj > 1.0f && emin > 1.0f) {
        // Whole fruit in view: refine volume from the natural oval (3-sigma
        // ellipse axes -> semi-axis lengths), a closer fit than the bbox.
        const float a = emin * cm_per_pixel;      // minor -> elliptical section
        const float b = emaj * cm_per_pixel;      // major -> long diameter
        constexpr float PI_OVER_6 = 0.5235987756f;
        features.volume_cm3 = PI_OVER_6 * (a * a) * b;
        if (features.volume_cm3 < VOLUME_CM3_MIN) features.volume_cm3 = VOLUME_CM3_MIN;
        if (features.volume_cm3 > VOLUME_CM3_MAX) features.volume_cm3 = VOLUME_CM3_MAX;
    }

    // ─── Masked hue histogram + reflectance means (flash frame) ──
    uint32_t raw_counts[8] = {0};
    uint32_t total_valid_hue_pixels = 0;
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0, sum_ar = 0, sum_ag = 0, sum_ab = 0;
    uint32_t fg_color_pixels = 0;

    for (size_t i = 0; i < total_pixels; i++) {
        if (!out_mask[i]) continue;

        uint8_t fr, fg, fb, ar, ag, ab;
        unpack_rgb565((const uint8_t*)&flash[i], fr, fg, fb);
        unpack_rgb565((const uint8_t*)&ambient[i], ar, ag, ab);

        // Raw (pre-WB) saturation gate: washed-out neutral cast regions (~S 0.24)
        // must not count as green in the hue report, while real fruit (~S 0.5)
        // stays in. Evaluated on the raw flash before the B WB-scale below.
        {
            float rh, rs, rv;
            rgb888_to_hsv(fr, fg, fb, rh, rs, rv);
            if (rs < HUE_RAW_S_MIN) continue;
        }

        // Flash white-balance: MS_WB_B_SCALE (1.0 = none) — the sensor's on-chip
        // AWB already balances the flash frame naturally (converged by the AWB
        // warm-up before the lock). No B-reduction is applied, so yellow fruit is
        // not pushed toward the green bins. V = max(R,G,B) is unchanged for
        // green-dominant fruit, so seed_v_min stays valid.
        int wb_fb = (int)((float)fb * MS_WB_B_SCALE);
        if (wb_fb > 255) wb_fb = 255;
        fb = (uint8_t)wb_fb;

        float h, s, v;
        rgb888_to_hsv(fr, fg, fb, h, s, v);

        // Specular glare: bright, near-achromatic pixels flatten the histogram.
        if (s < GLARE_MIN_S_CUT && v > GLARE_MAX_V_CUT) continue;

        // Luminance floor uses the centre-prior adaptive seed value (so dark
        // mango pixels that passed the segmentation survive), plus the
        // desaturated-background S floor. Fixed FRUIT_MIN_V_CUT would re-discard
        // the dark fruit that the seed segmentation is designed to recover.
        if (v < seed_v_min || s < FRUIT_MIN_S_CUT) continue;

        uint32_t x = (uint32_t)(i % w);
        uint32_t y = (uint32_t)(i / w);
        (void)x; (void)y;

        sum_r += fr; sum_g += fg; sum_b += fb;
        sum_ar += ar; sum_ag += ag; sum_ab += ab;
        fg_color_pixels++;

        if (h >= HUE_WINDOW_MIN && h <= HUE_WINDOW_MAX) {
            int bin_idx = (int)((h - HUE_WINDOW_MIN) / HUE_BIN_WIDTH);
            if (bin_idx < 0) bin_idx = 0;
            if (bin_idx > 7) bin_idx = 7;
            raw_counts[bin_idx]++;
            total_valid_hue_pixels++;
        }
    }

    if (total_valid_hue_pixels > 0) {
        float inv_total = 1.0f / (float)total_valid_hue_pixels;
        for (int i = 0; i < 8; i++)
            features.hue_histogram[i] = raw_counts[i] * inv_total;
    } else {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
    }

    features.chromatic_dispersion = calculate_chromatic_dispersion(features.hue_histogram);

    if (fg_color_pixels) {
        features.raw_rgb_means[0] = (float)sum_r / (float)fg_color_pixels;
        features.raw_rgb_means[1] = (float)sum_g / (float)fg_color_pixels;
        features.raw_rgb_means[2] = (float)sum_b / (float)fg_color_pixels;
        features.ambient_rgb_means[0] = (float)sum_ar / (float)fg_color_pixels;
        features.ambient_rgb_means[1] = (float)sum_ag / (float)fg_color_pixels;
        features.ambient_rgb_means[2] = (float)sum_ab / (float)fg_color_pixels;
    }

    heap_caps_free(dy_map);
    heap_caps_free(core_mask);
    heap_caps_free(out_mask);

    if (fg_pixels_out) *fg_pixels_out = fg_count;
    if (bbox_x_out) *bbox_x_out = min_x;
    if (bbox_y_out) *bbox_y_out = min_y;
    if (bbox_w_out) *bbox_w_out = (max_x > min_x) ? (max_x - min_x + 1) : 0;
    if (bbox_h_out) *bbox_h_out = (max_y > min_y) ? (max_y - min_y + 1) : 0;

    return features;
}

// ─── Unified 5-Layer 3-Frame Burst Segmentation ────────────────────
// Layer 2: SAD macroblock motion-parallax on two flash frames. Each 16×16 block
// is displaced ±3 px within its ±3 neighbors and the magnitude of the best
// match records its motion. Returns the mean global motion (px) so the caller
// can adaptively gate handheld vs stationary scenes.
static float compute_parallax_grid(const uint16_t* f1, const uint16_t* f2,
                                   int w, int h, std::vector<uint8_t>& block_fg) {
    const int bx_count = w / PARALLAX_BLOCK_SIZE;
    const int by_count = h / PARALLAX_BLOCK_SIZE;
    std::vector<uint8_t> disp(bx_count * by_count, 0);
    double global_sum = 0.0;
    int moving = 0;

    for (int by = 1; by < by_count - 1; by++) {
        for (int bx = 1; bx < bx_count - 1; bx++) {
            const int origin_x = bx * PARALLAX_BLOCK_SIZE;
            const int origin_y = by * PARALLAX_BLOCK_SIZE;
            long best_sad = 0x7FFFFFFF;
            int best_dx = 0, best_dy = 0;

            for (int dy = -PARALLAX_SEARCH_WIN; dy <= PARALLAX_SEARCH_WIN; dy++) {
                for (int dx = -PARALLAX_SEARCH_WIN; dx <= PARALLAX_SEARCH_WIN; dx++) {
                    long sad = 0;
                    bool valid = true;
                    // Subsample 2 px for a ~4× speed-up on the SAD accumulators.
                    for (int y = 0; y < PARALLAX_BLOCK_SIZE && valid; y += 2) {
                        const int sy = origin_y + y;
                        const int ty = origin_y + y + dy;
                        if (ty < 0 || ty >= h) { valid = false; break; }
                        const uint16_t* srow = f1 + (size_t)sy * w + origin_x;
                        const uint16_t* trow = f2 + (size_t)ty * w + origin_x + dx;
                        for (int x = 0; x < PARALLAX_BLOCK_SIZE; x += 2) {
                            int xo = x + dx;
                            if (xo < 0 || xo >= w) { valid = false; break; }
                            uint16_t a = srow[x];
                            uint16_t b = trow[xo];
                            long ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
                            long br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
                            long dr = ar - br; if (dr < 0) dr = -dr;
                            long dg = ag - bg; if (dg < 0) dg = -dg;
                            long db = ab - bb; if (db < 0) db = -db;
                            sad += dr + dg + db;
                        }
                    }
                    if (!valid) { sad = 0x7FFFFFFF; }
                    if (sad < best_sad) {
                        best_sad = sad;
                        best_dx = dx;
                        best_dy = dy;
                    }
                }
            }

            float mag = std::sqrt((float)(best_dx * best_dx + best_dy * best_dy));
            disp[by * bx_count + bx] = (uint8_t)(mag + 0.5f);
            global_sum += mag;
            if (mag >= PARALLAX_MIN_MOTION) moving++;
        }
    }

    int eval_blocks = std::max(1, (by_count - 2) * (bx_count - 2));
    block_fg = disp;
    // Return mean motion across ALL blocks (edge blocks excluded from search
    // read as static), plus store the moving-block count in the reserved byte.
    // The global mean here intentionally counts only searched blocks so a wide
    // static border cannot pull a genuinely handheld scene under the gate.
    float global = (float)(global_sum / (double)eval_blocks);
    return std::min(global, 255.0f);
}

ColorFeatures HueExtractor::process_unified_burst(const uint16_t* flashed1, const uint16_t* flashed2,
                                                  const uint16_t* ambient,
                                                  uint16_t w, uint16_t h, float cm_per_pixel,
                                                  BurstMotionStats* motion,
                                                  uint32_t* fg_pixels_out,
                                                  int* bbox_x_out, int* bbox_y_out,
                                                  int* bbox_w_out, int* bbox_h_out,
                                                  uint32_t* core_pixels_out,
                                                  uint32_t* snap_pixels_out) {
    ColorFeatures features;
    memset(&features, 0, sizeof(ColorFeatures));
    if (fg_pixels_out) *fg_pixels_out = 0;
    if (bbox_x_out) *bbox_x_out = 0;
    if (bbox_y_out) *bbox_y_out = 0;
    if (bbox_w_out) *bbox_w_out = 0;
    if (bbox_h_out) *bbox_h_out = 0;
    if (core_pixels_out) *core_pixels_out = 0;
    if (snap_pixels_out) *snap_pixels_out = 0;
    if (motion) { motion->global_motion = 0.0f; motion->handheld = false;
                  motion->moving_blocks = 0; motion->total_blocks = 0; }

    if (!flashed1 || !flashed2 || !ambient || w == 0 || h == 0) {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
        features.chromatic_dispersion = 1.0f;
        features.volume_cm3 = VOLUME_DEFAULT_CM3;
        return features;
    }

    const int bx_count = w / PARALLAX_BLOCK_SIZE;
    const int by_count = h / PARALLAX_BLOCK_SIZE;

    // Layer 2: parallax grid + adaptive global classifier.
    std::vector<uint8_t> parallax_grid;
    float global_motion = compute_parallax_grid(flashed1, flashed2, w, h, parallax_grid);
    bool handheld = (global_motion >= PARALLAX_GLOBAL_HANDHELD);
    int moving_blocks = 0;
    for (size_t i = 0; i < parallax_grid.size(); i++)
        if (parallax_grid[i] >= (uint8_t)(PARALLAX_MIN_MOTION + 0.5f)) moving_blocks++;
    if (motion) {
        motion->global_motion = global_motion;
        motion->handheld = handheld;
        motion->moving_blocks = moving_blocks;
        motion->total_blocks = bx_count * by_count;
    }

    size_t total_pixels = (size_t)w * h;

    // Layer 1+3: per-channel max(F1,F2) fusion then ambient-subtracted ΔY.
    uint8_t* dy_map = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_SPIRAM);
    if (!dy_map) dy_map = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_8BIT);
    if (!dy_map) {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
        features.chromatic_dispersion = 1.0f;
        features.volume_cm3 = VOLUME_DEFAULT_CM3;
        return features;
    }
    uint8_t* core_mask = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_SPIRAM);
    if (!core_mask) core_mask = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_8BIT);
    if (!core_mask) { heap_caps_free(dy_map);
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
        features.chromatic_dispersion = 1.0f; features.volume_cm3 = VOLUME_DEFAULT_CM3; return features; }
    uint8_t* out_mask = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_SPIRAM);
    if (!out_mask) out_mask = (uint8_t*)heap_caps_malloc(total_pixels, MALLOC_CAP_8BIT);
    if (!out_mask) { heap_caps_free(dy_map); heap_caps_free(core_mask);
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
        features.chromatic_dispersion = 1.0f; features.volume_cm3 = VOLUME_DEFAULT_CM3; return features; }

    float seed_v_min = CENTRE_SEED_V_MIN;   // hoisted: reused by the hue V gate below
    uint32_t snap_count = 0;

    // ΔY map from the shadow-healed flash-vs-ambient subtraction.
    for (size_t i = 0; i < total_pixels; i++) {
        uint8_t r1, g1, b1, r2, g2, b2, ar, ag, ab;
        unpack_rgb565((const uint8_t*)&flashed1[i], r1, g1, b1);
        unpack_rgb565((const uint8_t*)&flashed2[i], r2, g2, b2);
        unpack_rgb565((const uint8_t*)&ambient[i], ar, ag, ab);
        uint8_t fr = r1 > r2 ? r1 : r2;   // Layer 3: shadow-heal via max
        uint8_t fg_ = g1 > g2 ? g1 : g2;
        uint8_t fb = b1 > b2 ? b1 : b2;
        int dr = (int)fr - ar; if (dr < 0) dr = 0;
        int dg = (int)fg_ - ag; if (dg < 0) dg = 0;
        int db = (int)fb - ab; if (db < 0) db = 0;
        int dy = (0.299f * dr + 0.587f * dg + 0.114f * db);
        if (dy > 255) dy = 255;
        if (dy < 0) dy = 0;
        dy_map[i] = (uint8_t)dy;
    }

    // Segmentation (latest 2026-09-04, matches process_differential): adaptive
    // ΔY/V thresholds from a central foveal seed box, then largest-blob
    // selection with a centre-proximity tiebreak among comparable blobs.
    uint32_t core_count = 0;
    {
        int se = (int)w / 2, cty = (int)h / 2;
        int rw = (int)((float)w * CENTRE_SEED_FRACTION);
        int rh = (int)((float)h * CENTRE_SEED_FRACTION);
        if (rw < 1) rw = 1;
        if (rh < 1) rh = 1;
        int seed_x0 = se - rw, seed_y0 = cty - rh;
        if (seed_x0 < 0) seed_x0 = 0;
        if (seed_x0 + rw + 1 > (int)w) seed_x0 = (int)w - rw - 1;
        if (seed_y0 < 0) seed_y0 = 0;
        if (seed_y0 + rh + 1 > (int)h) seed_y0 = (int)h - rh - 1;

        std::vector<uint8_t> seed_dy;
        std::vector<float> seed_v;
        seed_dy.reserve((size_t)(2 * rw + 1) * (2 * rh + 1));
        seed_v.reserve((size_t)(2 * rw + 1) * (2 * rh + 1));
        for (int y = seed_y0; y <= seed_y0 + 2 * rh; y++) {
            for (int x = seed_x0; x <= seed_x0 + 2 * rw; x++) {
                size_t i = (size_t)y * w + (size_t)x;
                uint8_t r1, g1, b1, r2, g2, b2;
                unpack_rgb565((const uint8_t*)&flashed1[i], r1, g1, b1);
                unpack_rgb565((const uint8_t*)&flashed2[i], r2, g2, b2);
                uint8_t m = (r1 > r2 ? r1 : r2);
                uint8_t mg = (g1 > g2 ? g1 : g2);
                uint8_t mb = (b1 > b2 ? b1 : b2);
                if (mg > m) m = mg;
                if (mb > m) m = mb;
                seed_dy.push_back(dy_map[i]);
                seed_v.push_back((float)m / 255.0f);
            }
        }
        int seed_dy_thresh = (int)seed_percentile(seed_dy, 0.25f);
        if (seed_dy_thresh < CENTRE_SEED_DY_MIN) seed_dy_thresh = CENTRE_SEED_DY_MIN;
        seed_v_min = seed_percentile(seed_v, 0.10f);
        if (seed_v_min < CENTRE_SEED_V_MIN) seed_v_min = CENTRE_SEED_V_MIN;
        uint8_t dy_t = (uint8_t)seed_dy_thresh;

        // Candidate foreground = adaptive ΔY && adaptive fused-V, into core_mask.
        for (size_t i = 0; i < total_pixels; i++) {
            uint8_t r1, g1, b1, r2, g2, b2;
            unpack_rgb565((const uint8_t*)&flashed1[i], r1, g1, b1);
            unpack_rgb565((const uint8_t*)&flashed2[i], r2, g2, b2);
            uint8_t m = (r1 > r2 ? r1 : r2);
            uint8_t mg = (g1 > g2 ? g1 : g2);
            uint8_t mb = (b1 > b2 ? b1 : b2);
            if (mg > m) m = mg;
            if (mb > m) m = mb;
            uint8_t cand = (dy_map[i] >= dy_t && (float)m / 255.0f >= seed_v_min) ? 1 : 0;
            core_mask[i] = cand;
            out_mask[i] = 0;   // visited / output, initially empty
        }

        // Strict foveal fail-fast (item 2, mirrors final_pipeline._FOVEAL_FG_MIN):
        // same as process_differential — the foveal seed box must yield a confident
        // fruit-like foreground on its own; edge/border washes report empty.
        uint32_t foveal_fg = 0;
        for (int y = seed_y0; y <= seed_y0 + 2 * rh; y++)
            for (int x = seed_x0; x <= seed_x0 + 2 * rw; x++)
                foveal_fg += core_mask[(size_t)y * w + (size_t)x];
        if (foveal_fg < (uint32_t)MS_FOVEAL_FG_MIN) {
            for (int i = 0; i < 8; i++) features.hue_histogram[i] = 0.0f;
            features.chromatic_dispersion = 1.0f;
            features.volume_cm3 = 0.0f;
            heap_caps_free(dy_map);
            heap_caps_free(core_mask);
            heap_caps_free(out_mask);
            if (core_pixels_out) *core_pixels_out = 0;
            if (snap_pixels_out) *snap_pixels_out = 0;
            return features;
        }

        // Select the foreground component: largest blob with a centre-proximity
        // tiebreak among comparable blobs (>=50% of the largest). Mirrors
        // process_differential and the validated host port; replaces the
        // single-pixel centre seed that collapsed to a 20% box on off-centre /
        // full-frame fruit (2026-09-04 captures).
        apply_gradient_notch(core_mask, dy_map, w, h, seed_x0, seed_y0, rw, rh);
        select_largest_centre_blob(core_mask, out_mask, w, h);
    }

    // Count the seeded component and its mean saturation (for the low-S table
    // reject below).
    core_count = 0;
    float sum_saturation = 0.0f;
    for (size_t i = 0; i < total_pixels; i++) {
        if (out_mask[i]) {
            core_count++;
            uint8_t r1, g1, b1, r2, g2, b2;
            unpack_rgb565((const uint8_t*)&flashed1[i], r1, g1, b1);
            unpack_rgb565((const uint8_t*)&flashed2[i], r2, g2, b2);
            float hh, ss, vv;
            rgb888_to_hsv(r1 > r2 ? r1 : r2, g1 > g2 ? g1 : g2, b1 > b2 ? b1 : b2, hh, ss, vv);
            sum_saturation += ss;
        }
    }

    // Layer 5 glossy-table / white-wall reject: a neutral (low-S) foreground
    // region is flash glare off a reflective surface, not fruit -> empty.
    if (core_count >= (uint32_t)FLASH_MIN_OBJECT_PIXELS) {
        float mean_s = sum_saturation / (float)core_count;
        if (mean_s < TABLE_MIN_MEAN_S) core_count = 0;
    }

    if (core_count < (uint32_t)FLASH_MIN_OBJECT_PIXELS) {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = 0.0f;
        features.chromatic_dispersion = 1.0f;
        features.volume_cm3 = 0.0f;
        heap_caps_free(dy_map);
        heap_caps_free(core_mask);
        heap_caps_free(out_mask);
        if (core_pixels_out) *core_pixels_out = 0;
        if (snap_pixels_out) *snap_pixels_out = 0;
        return features;
    }

    if (core_pixels_out) *core_pixels_out = core_count;
    if (snap_pixels_out) *snap_pixels_out = snap_count;

    // Layer 6: marginal-density projection bbox with an 8% row/column floor.
    uint32_t* proj_x = (uint32_t*)heap_caps_malloc((size_t)w * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!proj_x) proj_x = (uint32_t*)heap_caps_malloc((size_t)w * sizeof(uint32_t), MALLOC_CAP_8BIT);
    uint32_t* proj_y = (uint32_t*)heap_caps_malloc((size_t)h * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!proj_y) proj_y = (uint32_t*)heap_caps_malloc((size_t)h * sizeof(uint32_t), MALLOC_CAP_8BIT);
    if (proj_x) memset(proj_x, 0, (size_t)w * sizeof(uint32_t));
    if (proj_y) memset(proj_y, 0, (size_t)h * sizeof(uint32_t));

    uint32_t fg_count = 0;
    if (proj_x && proj_y) {
        for (size_t i = 0; i < total_pixels; i++) {
            if (out_mask[i]) {
                fg_count++;
                proj_x[i % w]++;
                proj_y[i / w]++;
            }
        }
    } else {
        for (size_t i = 0; i < total_pixels; i++) if (out_mask[i]) fg_count++;
    }

    int min_x = 0, max_x = w - 1, min_y = 0, max_y = h - 1;
    int col_thresh = std::max(4, (int)(0.08f * h));
    int row_thresh = std::max(4, (int)(0.08f * w));
    bool found_x = false, found_y = false;
    if (proj_x && proj_y && fg_count) {
        for (int x = 0; x < (int)w; x++) if (proj_x[x] >= (uint32_t)col_thresh) { min_x = x; found_x = true; break; }
        for (int x = (int)w - 1; x >= 0; x--) if (proj_x[x] >= (uint32_t)col_thresh) { max_x = x; break; }
        for (int y = 0; y < (int)h; y++) if (proj_y[y] >= (uint32_t)row_thresh) { min_y = y; found_y = true; break; }
        for (int y = (int)h - 1; y >= 0; y--) if (proj_y[y] >= (uint32_t)row_thresh) { max_y = y; break; }
        if (!found_x) { min_x = 0; max_x = w - 1; }
        if (!found_y) { min_y = 0; max_y = h - 1; }
    }

    if (fg_pixels_out) *fg_pixels_out = fg_count;
    if (bbox_x_out) *bbox_x_out = min_x;
    if (bbox_y_out) *bbox_y_out = min_y;
    if (bbox_w_out) *bbox_w_out = (max_x >= min_x) ? (max_x - min_x + 1) : 0;
    if (bbox_h_out) *bbox_h_out = (max_y >= min_y) ? (max_y - min_y + 1) : 0;

    // Ellipsoid volume from the density bbox (prolate spheroid, same as spec).
    features.volume_cm3 = VOLUME_DEFAULT_CM3;
    if (found_x && found_y && max_x > min_x && max_y > min_y) {
        float wc = (float)(max_x - min_x + 1) * cm_per_pixel;
        float hc = (float)(max_y - min_y + 1) * cm_per_pixel;
        float a = (wc < hc) ? wc : hc;
        float b = (wc < hc) ? hc : wc;
        constexpr float PI_OVER_6 = 0.5235987756f;
        features.volume_cm3 = PI_OVER_6 * (a * a) * b;
        if (features.volume_cm3 < VOLUME_CM3_MIN) features.volume_cm3 = VOLUME_CM3_MIN;
        if (features.volume_cm3 > VOLUME_CM3_MAX) features.volume_cm3 = VOLUME_CM3_MAX;
    } else {
        features.volume_cm3 = 0.0f;
    }

    // Natural-ellipse fit + overflow signal (mirror of process_differential).
    float emaj = 0.0f, emin = 0.0f, ecx = 0.0f, ecy = 0.0f;
    uint8_t overflow = 0;
    if (fg_count >= (uint32_t)FLASH_MIN_OBJECT_PIXELS) {
        fit_fruit_ellipse(out_mask, w, h, &emaj, &emin, &ecx, &ecy, &overflow);
    }
    features.ellipse_major_px = emaj;
    features.ellipse_minor_px = emin;
    features.centroid_px[0] = ecx;
    features.centroid_px[1] = ecy;
    features.overflow_mask = overflow;

    // Opposite-border glare rejection: wall/table glare spans the full sensor.
    if (spans_opposite_borders(overflow)) {
        features.volume_cm3 = 0.0f;
        features.ellipse_major_px = 0.0f;
        features.ellipse_minor_px = 0.0f;
        features.centroid_px[0] = 0.0f;
        features.centroid_px[1] = 0.0f;
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = 0.0f;
        features.chromatic_dispersion = 1.0f;
        if (fg_pixels_out) *fg_pixels_out = 0;
        if (core_pixels_out) *core_pixels_out = 0;
        if (snap_pixels_out) *snap_pixels_out = 0;
        if (bbox_x_out) *bbox_x_out = 0;
        if (bbox_y_out) *bbox_y_out = 0;
        if (bbox_w_out) *bbox_w_out = 0;
        if (bbox_h_out) *bbox_h_out = 0;
        heap_caps_free(dy_map);
        heap_caps_free(core_mask);
        heap_caps_free(out_mask);
        if (proj_x) heap_caps_free(proj_x);
        if (proj_y) heap_caps_free(proj_y);
        return features;
    }

    if (overflow == 0 && emaj > 1.0f && emin > 1.0f) {
        // Whole fruit in view -> refine volume with the natural oval.
        const float a = emin * cm_per_pixel;
        const float b = emaj * cm_per_pixel;
        constexpr float PI_OVER_6 = 0.5235987756f;
        features.volume_cm3 = PI_OVER_6 * (a * a) * b;
        if (features.volume_cm3 < VOLUME_CM3_MIN) features.volume_cm3 = VOLUME_CM3_MIN;
        if (features.volume_cm3 > VOLUME_CM3_MAX) features.volume_cm3 = VOLUME_CM3_MAX;
    }

    // Layer 5 hue extraction on the fused flash frame inside the mask: skip
    // glare, low-light noise, and the 2D skin band; leave fruit hue intact.
    uint32_t raw_counts[8] = {0};
    uint32_t total_valid_hue_pixels = 0;
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0, sum_ar = 0, sum_ag = 0, sum_ab = 0;
    uint32_t fg_color_pixels = 0;

    for (size_t i = 0; i < total_pixels; i++) {
        if (!out_mask[i]) continue;
        uint8_t r1, g1, b1, r2, g2, b2, ar, ag, ab;
        unpack_rgb565((const uint8_t*)&flashed1[i], r1, g1, b1);
        unpack_rgb565((const uint8_t*)&flashed2[i], r2, g2, b2);
        unpack_rgb565((const uint8_t*)&ambient[i], ar, ag, ab);
        uint8_t fr = r1 > r2 ? r1 : r2;
        uint8_t fg_ = g1 > g2 ? g1 : g2;
        uint8_t fb = b1 > b2 ? b1 : b2;

        // Raw (pre-WB) saturation gate: washed-out neutral cast regions (~S 0.24)
        // must not count as green in the hue report, while real fruit (~S 0.5)
        // stays in. Evaluated on the raw fused flash before the B WB-scale below.
        {
            float rh, rs, rv;
            rgb888_to_hsv(fr, fg_, fb, rh, rs, rv);
            if (rs < HUE_RAW_S_MIN) continue;
        }

        // Flash white-balance: MS_WB_B_SCALE (1.0 = none, see process_differential).
        // AWB converges before the lock so no B-reduction is needed.
        int wb_fb = (int)((float)fb * MS_WB_B_SCALE);
        if (wb_fb > 255) wb_fb = 255;
        fb = (uint8_t)wb_fb;

        float h, s, v;
        rgb888_to_hsv(fr, fg_, fb, h, s, v);

        if (s < GLARE_MIN_S_CUT && v > GLARE_MAX_V_CUT) continue;   // glare
        if (v < seed_v_min || s < FRUIT_MIN_S_CUT) continue;   // adaptive floor (centre-seed V)
        if (h >= SKIN_HUE_MIN_DEG && h <= SKIN_HUE_MAX_DEG && s < SKIN_SAT_MAX) continue; // fingers

        sum_r += fr; sum_g += fg_; sum_b += fb;
        sum_ar += ar; sum_ag += ag; sum_ab += ab;
        fg_color_pixels++;

        if (h >= HUE_WINDOW_MIN && h <= HUE_WINDOW_MAX) {
            int bin_idx = (int)((h - HUE_WINDOW_MIN) / HUE_BIN_WIDTH);
            if (bin_idx < 0) bin_idx = 0;
            if (bin_idx > 7) bin_idx = 7;
            raw_counts[bin_idx]++;
            total_valid_hue_pixels++;
        }
    }

    if (total_valid_hue_pixels > 0) {
        float inv_total = 1.0f / (float)total_valid_hue_pixels;
        for (int i = 0; i < 8; i++)
            features.hue_histogram[i] = raw_counts[i] * inv_total;
    } else {
        for (int i = 0; i < 8; i++) features.hue_histogram[i] = UNIFORM_BIN;
    }

    features.chromatic_dispersion = calculate_chromatic_dispersion(features.hue_histogram);

    if (fg_color_pixels) {
        features.raw_rgb_means[0] = (float)sum_r / (float)fg_color_pixels;
        features.raw_rgb_means[1] = (float)sum_g / (float)fg_color_pixels;
        features.raw_rgb_means[2] = (float)sum_b / (float)fg_color_pixels;
        features.ambient_rgb_means[0] = (float)sum_ar / (float)fg_color_pixels;
        features.ambient_rgb_means[1] = (float)sum_ag / (float)fg_color_pixels;
        features.ambient_rgb_means[2] = (float)sum_ab / (float)fg_color_pixels;
    }

    heap_caps_free(dy_map);
    heap_caps_free(core_mask);
    heap_caps_free(out_mask);
    if (proj_x) heap_caps_free(proj_x);
    if (proj_y) heap_caps_free(proj_y);

    return features;
}
