#include "sci_32d.h"
#include <cstring>
#include <cstddef>

// The Dual-Latent engine (dual_latent.cpp) is the ONLY classifier in this
// firmware. The legacy 32D Diagonal-Gaussian scorer AND its 32-dim state
// block are removed; this file keeps only the shared ontology and the
// display-only confidence helper.

const char* const  CLASS_LABELS[NUM_CLASSES] = {
    "UNRIPE",
    "PERFECTLY_RIPE",
    "OVERRIPE",
    "ROTTEN_OR_HOLLOW",
    "ARTIFICIALLY_RIPENED"
};

// The wire size must match the validated 852-byte transfer contract.
static_assert(MODEL_WIRE_BYTES == 852, "wire size must be 852");

// SERIAL-WIRE LAYOUT GUARDS for the DualModelWire, connecting the struct to
// the offset table the Python builder (build_dual_model_bin.py) emits.
static_assert(offsetof(DualModelWire, magic) == 32,
              "magic offset must be 32");
static_assert(offsetof(DualModelWire, active_class_mask) == 37,
              "mask offset must be 37");
static_assert(offsetof(DualModelWire, modality) == 38,
              "modality offset must be 38");
static_assert(offsetof(DualModelWire, r0_spec) == 48,
              "r0_spec offset must be 48");
static_assert(offsetof(DualModelWire, anchors) == 92,
              "anchors offset must be 92");
static_assert(offsetof(DualModelWire, min_peak_adc_lo) == 113,
              "min_peak_adc must be at offset 113");
static_assert(offsetof(DualModelWire, acoustic_feature_mode) == 116,
              "acoustic_feature_mode must be at offset 116");
static_assert(offsetof(DualModelWire, stft_t1_prior) == 120,
              "stft_t1_prior must be at offset 120");

// DISPLAY-ONLY confidence for the LCD. Smooths the posterior toward uniform
// (Laplace, alpha=0.08) so a decisive result reads ~94% instead of a flat full
// bar, then returns the softened winner probability as 0-100%. The decision
// itself is untouched.
float display_confidence(const BiologicalStatus& status) {
    const float alpha = 0.08f;
    float p_max = 0.0f;
    for (int c = 0; c < NUM_CLASSES; c++) {
        float p = status.probabilities[c];
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        float softened = (1.0f - alpha) * p + alpha / (float)NUM_CLASSES;
        if (softened > p_max) p_max = softened;
    }
    float conf = p_max * 100.0f;
    if (conf < 0.0f) conf = 0.0f;
    if (conf > 100.0f) conf = 100.0f;
    return conf;
}