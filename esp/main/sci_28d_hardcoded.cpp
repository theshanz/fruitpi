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
    float s9 = (vol_23 - MIN_MASS23) / (MAX_MASS23 - MIN_MASS23 + 1e-5f);
    if (s9 < 0.0f) s9 = 0.0f;
    if (s9 > 1.0f) s9 = 1.0f;
    state[9] = s9;

    if (ACOUSTIC_FORCE_INVARIANT) {
        // Must stay identical to sci_28d.cpp / extract_28d.py.
        const float amp = acoustic.impact_amplitude > IMPACT_AMP_FLOOR
                              ? acoustic.impact_amplitude
                              : IMPACT_AMP_FLOOR;
        const float corr = 2.0f * logf(amp);
        for (int i = 0; i < 15; i++) {
            const float fi = FFT_CENTERS[i];
            state[10 + i] -= corr * (fi * fi) / F2_NORM;
        }
        state[26] = 0.0f;
    } else {
        for (int i = 0; i < 15; i++) state[10 + i] = acoustic.fft_bins[i];
        state[26] = acoustic.impact_amplitude;
    }
    state[25] = acoustic.spectral_entropy;
    state[27] = 0.0f;
}

// =========================================================================
// TWO-CLASS NEAREST-PROTOTYPE CLASSIFIER (UNRIPE vs PERFECTLY_RIPE)
//
// Prototypes are hard-coded from real data-collector sessions (see
// datacollection/extract_28d.py for the firmware-faithful feature math):
//   UNRIPE -> dataset/Mango/Mango_1785891275 (6 taps, 3 hue captures)
//   PERFECTLY_RIPE -> dataset/Mango/Mango_1785891354 (7 taps, 3 hue captures)
//
// REGENERATED after the acoustic DSP rework (per-window DC removal, Hann
// mainlobe k-1..k+1 integration, state[26] = normalized impact_amplitude
// instead of a raw ADC peak). If you change piezo_acoustic.cpp feature math,
// re-run extract_28d.py and paste all three tables again.
//
// A state vector is assigned to whichever prototype it is closer to, using a
// per-dimension range-normalized squared Euclidean distance so each feature
// contributes equally regardless of its raw magnitude. Range = |ripe - unripe|
// per dimension (zeros replaced by 1.0). Other classes are ignored for now.
//
// CAVEAT: these sessions were tapped harder than typical fruit taps
// (~800/~593 count deviations vs ~100-275 in the field). Weak real taps sit
// below BOTH prototypes on dim 26; re-record sessions at realistic strength
// if scans skew PERFECTLY_RIPE.
// =========================================================================

// Prototype for class "UNRIPE"
static const float UNRIPE_PROTOTYPE[VECTOR_DIMENSIONS] = {
    0.419193f, 0.015123f, 0.514591f, 0.000000f, 0.001116f, 0.003258f,
    0.001470f, 0.045249f, 0.606800f, 0.182745f, 0.694901f, 1.767812f,
    3.535927f, 6.669797f, 9.672530f, 13.377932f, 16.435476f, 20.189005f,
    25.009979f, 31.823705f, 44.042512f, 58.908904f, 77.978604f, 92.837161f,
    117.900724f, 0.734942f, 0.195379f, 0.000000f
};

// Prototype for class "PERFECTLY_RIPE"
static const float RIPE_PROTOTYPE[VECTOR_DIMENSIONS] = {
    0.488644f, 0.103018f, 0.365849f, 0.004372f, 0.003930f, 0.011918f,
    0.008621f, 0.013649f, 0.691945f, 0.182745f, 0.711259f, 1.673503f,
    3.795823f, 6.419134f, 9.046142f, 11.883506f, 15.682791f, 18.354138f,
    22.423520f, 28.227213f, 41.511093f, 51.095840f, 65.173268f, 79.806818f,
    101.463050f, 0.702702f, 0.144904f, 0.000000f
};

// Per-dimension normalization range (|ripe - unripe|, zeros -> 1.0)
static const float RANGE_SCALE[VECTOR_DIMENSIONS] = {
    0.069451f, 0.087895f, 0.148742f, 0.004372f, 0.002814f, 0.008660f,
    0.007150f, 0.031600f, 0.085146f, 1.000000f, 0.016359f, 0.094308f,
    0.259896f, 0.250663f, 0.626388f, 1.494426f, 0.752685f, 1.834868f,
    2.586459f, 3.596492f, 2.531419f, 7.813064f, 12.805336f, 13.030343f,
    16.437674f, 0.032241f, 0.050475f, 1.000000f
};

// Range-normalized squared Euclidean distance between a state vector and a
// prototype (replicates the classifier used to validate the prototypes).
static float dist_sq(const float state[VECTOR_DIMENSIONS], const float proto[VECTOR_DIMENSIONS]) {
    float d = 0.0f;
    for (int i = 0; i < VECTOR_DIMENSIONS; i++) {
        float diff = (state[i] - proto[i]) / RANGE_SCALE[i];
        d += diff * diff;
    }
    return d;
}

// Resolve a decision from the accumulated squared distances to the two
// prototypes. Soft probabilities are derived from the relative distances.
static BiologicalStatus decide_binary(float d_unripe, float d_ripe) {
    BiologicalStatus result;
    memset(&result, 0, sizeof(result));

    float sum = d_unripe + d_ripe + 1e-9f;
    float p_unripe = d_ripe / sum;
    float p_ripe = d_unripe / sum;

    if (d_unripe <= d_ripe) {
        result.primary_decision = CLASS_LABELS[0]; // UNRIPE
        result.ripeness_index = 1.0f;
        result.confidence = p_unripe * 100.0f;
    } else {
        result.primary_decision = CLASS_LABELS[1]; // PERFECTLY_RIPE
        result.ripeness_index = 2.0f;
        result.confidence = p_ripe * 100.0f;
    }

    result.probabilities[0] = p_unripe;
    result.probabilities[1] = p_ripe;
    result.is_anomaly = false;
    result.transition_entropy = 0.0f;
    return result;
}

BiologicalStatus evaluate_fruit_single(
    const float state[VECTOR_DIMENSIONS],
    const ManifoldModel28D& model // Ignored: decision is prototype-based
) {
    return decide_binary(dist_sq(state, UNRIPE_PROTOTYPE), dist_sq(state, RIPE_PROTOTYPE));
}

// 3-tap wrapper: average the accumulated squared distances across the three
// taps, then classify once (reduces per-tap jitter).
BiologicalStatus evaluate_fruit_3tap(
    const float state1[VECTOR_DIMENSIONS],
    const float state2[VECTOR_DIMENSIONS],
    const float state3[VECTOR_DIMENSIONS],
    const ManifoldModel28D& model
) {
    float du = dist_sq(state1, UNRIPE_PROTOTYPE) + dist_sq(state2, UNRIPE_PROTOTYPE) + dist_sq(state3, UNRIPE_PROTOTYPE);
    float dr = dist_sq(state1, RIPE_PROTOTYPE) + dist_sq(state2, RIPE_PROTOTYPE) + dist_sq(state3, RIPE_PROTOTYPE);
    return decide_binary(du, dr);
}

#endif // defined(SCI_28D_HARDCODED)
