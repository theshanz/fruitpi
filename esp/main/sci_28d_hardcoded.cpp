#include "sci_28d.h"

#if defined(SCI_28D_HARDCODED)

#include <cmath>
#include <cstring>
#include <algorithm>

// Standalone function to assemble the state array if needed
void assemble_state_28d(
    float state[VECTOR_DIMENSIONS],
    const ColorFeatures& vision,
    const AcousticFeatures& acoustic
) {
    for (int i = 0; i < 8; i++) state[i] = vision.hue_histogram[i];
    state[8] = vision.chromatic_dispersion;

    float vol_23 = powf(vision.volume_cm3, 2.0f / 3.0f);
    state[9] = (vol_23 - MIN_MASS23) / (MAX_MASS23 - MIN_MASS23 + 1e-5f);

    for (int i = 0; i < 15; i++) state[10 + i] = acoustic.fft_bins[i];
    state[25] = acoustic.spectral_entropy;
    state[26] = acoustic.hertzian_adc;
    state[27] = 0.0f;
}

// =========================================================================
// SIMPLE IF / ELSE RULE ENGINE WITH FIRMNESS EQUATION
// =========================================================================
BiologicalStatus evaluate_fruit_single(
    const float state[VECTOR_DIMENSIONS],
    const ManifoldModel28D& model // Ignored completely
) {
    BiologicalStatus result;
    memset(&result, 0, sizeof(result));

    // ---------------------------------------------------------------------
    // 1. UNPACK HUMAN-READABLE COLOR VARIABLES (From state[0..7])
    // ---------------------------------------------------------------------
    float green_ratio  = state[0] + state[1]; // Green hue bins
    float yellow_ratio = state[2] + state[3]; // Yellow/Orange hue bins
    float red_ratio    = state[4] + state[5]; // Red/Brown hue bins

    // ---------------------------------------------------------------------
    // 2. UNPACK ACOUSTIC FEATURES & CALCULATE FIRMNESS EQUATION
    // ---------------------------------------------------------------------
    float spectral_entropy = state[25]; // Higher = hollow/dull sound
    float impact_peak_adc  = state[26]; // Higher = harder/more elastic tap

    // Find the dominant resonant frequency bin (150 Hz to 2100 Hz)
    int dominant_bin = 0;
    float max_bin_power = 0.0f;
    for (int b = 0; b < 15; b++) {
        if (state[10 + b] > max_bin_power) {
            max_bin_power = state[10 + b];
            dominant_bin = b;
        }
    }
    // Convert bin index to approximate center frequency (Hz)
    float dominant_frequency_hz = FFT_CENTERS[dominant_bin];

    // --- FIRMNESS EQUATION ---
    // High frequency + High impact amplitude + Low entropy = HARD / FIRM
    // Low frequency + Low impact amplitude + High entropy = SOFT / MUSHY
    float firmness_index = (dominant_frequency_hz * impact_peak_adc) / (spectral_entropy + 0.05f);

    // ---------------------------------------------------------------------
    // 3. SIMPLE IF / ELSE DECISION TREE
    // ---------------------------------------------------------------------

    // RULE 1: ROTTEN OR HOLLOW
    // High noise (entropy) or near-zero tap response
    if (spectral_entropy > 0.65f || impact_peak_adc < 0.05f) {
        result.primary_decision = CLASS_LABELS[3]; // "ROTTEN_OR_HOLLOW"
        result.is_anomaly = true;
        result.ripeness_index = 0.0f;

        result.probabilities[0] = 0.01f;
        result.probabilities[1] = 0.01f;
        result.probabilities[2] = 0.03f;
        result.probabilities[3] = 0.90f; // Rotten
        result.probabilities[4] = 0.05f;
    }

    // RULE 2: ARTIFICIALLY RIPENED
    // Green/unripe skin visually, but very soft/mushy acoustic firmness inside
    else if (green_ratio > 0.35f && firmness_index < 200.0f) {
        result.primary_decision = CLASS_LABELS[4]; // "ARTIFICIALLY_RIPENED"
        result.is_anomaly = true;
        result.ripeness_index = 0.0f;

        result.probabilities[0] = 0.10f;
        result.probabilities[1] = 0.05f;
        result.probabilities[2] = 0.05f;
        result.probabilities[3] = 0.00f;
        result.probabilities[4] = 0.80f; // Artificially Ripened
    }

    // RULE 3: UNRIPE
    // Green skin and high firmness score (> 500 Hz pitch equivalent)
    else if (green_ratio > 0.35f || firmness_index > 500.0f) {
        result.primary_decision = CLASS_LABELS[0]; // "UNRIPE"
        result.is_anomaly = false;
        result.ripeness_index = 1.0f;

        result.probabilities[0] = 0.85f; // Unripe
        result.probabilities[1] = 0.10f;
        result.probabilities[2] = 0.05f;
        result.probabilities[3] = 0.00f;
        result.probabilities[4] = 0.00f;
    }

    // RULE 4: OVERRIPE
    // Yellow/Brown skin and low firmness score (< 250 Hz pitch equivalent)
    else if (firmness_index < 250.0f || red_ratio > 0.30f) {
        result.primary_decision = CLASS_LABELS[2]; // "OVERRIPE"
        result.is_anomaly = false;
        result.ripeness_index = 3.0f;

        result.probabilities[0] = 0.05f;
        result.probabilities[1] = 0.15f;
        result.probabilities[2] = 0.80f; // Overripe
        result.probabilities[3] = 0.00f;
        result.probabilities[4] = 0.00f;
    }

    // RULE 5: PERFECTLY RIPE (Default / Ideal Range)
    // Moderate firmness (250 - 500 Hz) and yellow/orange skin
    else {
        result.primary_decision = CLASS_LABELS[1]; // "PERFECTLY_RIPE"
        result.is_anomaly = false;
        result.ripeness_index = 2.0f;

        result.probabilities[0] = 0.10f;
        result.probabilities[1] = 0.80f; // Perfectly Ripe
        result.probabilities[2] = 0.10f;
        result.probabilities[3] = 0.00f;
        result.probabilities[4] = 0.00f;
    }

    // Overall Confidence & Transition Entropy
    result.confidence = 90.0f;
    result.transition_entropy = spectral_entropy;

    return result;
}

// 3-Tap wrapper (simply calls single evaluation)
BiologicalStatus evaluate_fruit_3tap(
    const float state1[VECTOR_DIMENSIONS],
    const float state2[VECTOR_DIMENSIONS],
    const float state3[VECTOR_DIMENSIONS],
    const ManifoldModel28D& model
) {
    return evaluate_fruit_single(state1, model);
}

#endif // defined(SCI_28D_HARDCODED)
