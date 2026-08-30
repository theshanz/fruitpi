#include "sci_32d.h"
#include <cstring>
#include <cstddef>
#include <cmath>
#include <algorithm>

const char* const  CLASS_LABELS[NUM_CLASSES] = {
    "UNRIPE",
    "PERFECTLY_RIPE",
    "OVERRIPE",
    "ROTTEN_OR_HOLLOW",
    "ARTIFICIALLY_RIPENED"
};

// The wire size must match the validated FruitProfile32D (852 bytes).
static_assert(MODEL_WIRE_BYTES == 852, "32D wire size drifted");
static_assert(VECTOR_DIMENSIONS == 32, "32D engine expects 32 dims");

// SERIAL-WIRE LAYOUT GUARDS: the .bin/BLE blob is laid out by the app to the
// exact validated FruitProfile32D pack_binary() layout. Guard the offsets so a
// struct reorder can never silently break the wire format.
//   name 0..31 | mask@32 | gStart@33 | gEnd@34 | flags@35 |
//   veto@36 | minVol@40 | maxVol@44 | f0@48 | damp@52 | reserved 56..63 |
//   feature_weights 64..191 (v3: pooled inverse-variance)
//   | prototypes 192..831 (v3: per-class means) | biases 832..851
static_assert(offsetof(ManifoldModel32D, active_class_mask) == 32,
              "mask offset must be 32");
static_assert(offsetof(ManifoldModel32D, feature_weights) == 64,
              "feature_weights offset must be 64");
static_assert(offsetof(ManifoldModel32D, prototypes) == 192,
              "prototypes offset must be 192");
static_assert(offsetof(ManifoldModel32D, biases) == 832,
              "biases offset must be 832");

static inline bool floats_finite(const float* xs, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!std::isfinite(xs[i])) return false;
    return true;
}

// Structural validation of a decoded 852-byte ManifoldModel32D — every rule
// mirrors what the app's pack_binary() emits for a compiled profile.
bool model_blob_is_valid(const ManifoldModel32D& m) {
    // Name: printable ASCII, NUL-terminated inside the 32-byte field.
    const uint8_t* name = reinterpret_cast<const uint8_t*>(m.fruit_name);
    if (name[0] < 0x20 || name[0] == 0x7F) return false;
    bool terminated = false;
    for (int i = 0; i < 32; i++) {
        if (name[i] == 0) { terminated = true; break; }
        if (name[i] < 0x20 || name[i] == 0x7F) return false;
    }
    if (!terminated) return false;

    // Mask: at least one enabled class, no bits outside the 5-class set.
    const uint8_t mask = m.active_class_mask;
    if (mask == 0 || (mask & ~(uint8_t)((1u << NUM_CLASSES) - 1)) != 0)
        return false;

    // Green-bin range coherent inside the 8-slot hue window; header flags
    // must stay clear (reserved for future wire versions).
    if (m.green_bin_start >= m.green_bin_end || m.green_bin_end > 8)
        return false;
    if (m.flags != 0) return false;

    // Header scalars finite and within the range the compiler emits.
    if (!std::isfinite(m.green_veto_threshold)
        || m.green_veto_threshold < 0.0f
        || m.green_veto_threshold > 1.0f)
        return false;
    if (!std::isfinite(m.min_volume_cm3) || m.min_volume_cm3 <= 0.0f)
        return false;
    if (!std::isfinite(m.max_volume_cm3)
        || m.max_volume_cm3 <= m.min_volume_cm3)
        return false;
    if (!std::isfinite(m.expected_f0_hz) || m.expected_f0_hz <= 0.0f)
        return false;
    if (!std::isfinite(m.damping_scale) || m.damping_scale <= 0.0f)
        return false;

    // v3: pooled inverse-variance (feature_weights), raw class MEANS
    // (prototypes), and biases: all finite.
    if (!floats_finite(m.feature_weights, VECTOR_DIMENSIONS))
        return false;
    for (int c = 0; c < NUM_CLASSES; c++)
        if (!floats_finite(m.prototypes[c], VECTOR_DIMENSIONS))
            return false;
    if (!floats_finite(m.biases, NUM_CLASSES)) return false;

    return true;
}

static inline float clamp_f(float v, float lo, float hi){
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
// ─── 32D state assembly ───────────────────────────────────────────
// v3 keeps the RAW block values (NO per-block L2 normalization). The old
// cosine runner normalized each block to discard force/amplitude, which made
// distinct ripe/unripe mangoes near-parallel (~51% confidence even though the
// winner was always right). Audio amplitude is a dominant physical cue (unripe
// ~1.3–1.7k p2p vs ripe ~0.4–0.8k in our mango set), and the Diagonal-Gaussian
// scorer below needs it preserved.
void assemble_state_32d(
    float state[VECTOR_DIMENSIONS],
    const ColorFeatures& vision,
    const AcousticFeatures& acoustic
){
    // ── Optics & mass (0..9) ──
    for (int i = 0; i < 8; i++) state[i] = vision.hue_histogram[i];

    // Chromatic dispersion (validated: 1 - sum((h-1/8)^2)/0.875)
    float disp = 0.0f;
    for (int i = 0; i < 8; i++) {
        float d = vision.hue_histogram[i] - 0.125f;
        disp += d * d;
    }
    state[8] = 1.0f - disp / 0.875f;
    if (state[8] < 0.0f) state[8] = 0.0f;
    if (state[8] > 1.0f) state[8] = 1.0f;

    // Volume^(2/3) normalized to [0,1] (validated range: vol 10..600).
    float vol = vision.volume_cm3;
    if (vol < 10.0f) vol = 10.0f;
    if (vol > 600.0f) vol = 600.0f;
    float vol_23 = powf(vol, 2.0f / 3.0f);
    state[9] = clamp_f((vol_23 - 4.64f) / (71.13f - 4.64f), 0.0f, 1.0f);

    // ── Acoustic image (10..25): contrast-normed 4x4 STFT grid ──
    for (int i = 0; i < 16; i++) state[10 + i] = acoustic.stft_grid[i];

    // ── Bio-moments (26..31) ──
    state[BIO_CENTROID] = acoustic.bio_moments[0];            // temporal centroid
    state[BIO_TAIL]     = acoustic.bio_moments[1];            // tail energy
    state[BIO_HARMONIC] = acoustic.bio_moments[2];            // harmonic absorption
    // Abbotti stiffness = fundamental grid energy x volume^(2/3).
    float abbott = acoustic.bio_moments[3] * state[9];
    state[BIO_ABBOTT] = (abbott > 1.0f) ? 1.0f : abbott;      // dim 29
    state[30] = acoustic.bio_moments[4];                      // spectral entropy
    state[31] = acoustic.bio_moments[5];                      // dynamic damping
}

// ── v3 Diagonal-Gaussian (Mahalanobis) raw-logit scoring ─────────
// score[c] = bias[c] - 0.5 * sum_d inv_var[d]*(state[d]-mean[c][d])^2
// feature_weights[] holds the pooled per-feature inverse-variance (a small
// positive floor is enforced by the compiler), prototypes[] hold each class
// mean. Raw per-class logits are returned so the n-tap path can compound them
// as a sum of logits (a product of likelihoods) before a single softmax.
static void score_logits_32d(
    const float state[VECTOR_DIMENSIONS],
    const ManifoldModel32D& model,
    float raw_scores[NUM_CLASSES]
){
    const uint8_t class_mask = model.active_class_mask
                                   ? model.active_class_mask
                                   : (uint8_t)ACTIVE_CLASS_MASK;

    for (int c = 0; c < NUM_CLASSES; c++){
        if (!((class_mask >> c) & 1U)) {
            raw_scores[c] = -100000.0f;
            continue;
        }
        float d2 = 0.0f;
        for (int d = 0; d < VECTOR_DIMENSIONS; d++){
            float diff = state[d] - model.prototypes[c][d];
            d2 += model.feature_weights[d] * diff * diff;
        }
        raw_scores[c] = model.biases[c] - 0.5f * d2;
    }

    // Biological chlorophyll veto — green bins are read dynamically from the
    // profile header. A 0.0 threshold means the veto is disabled (the compiler
    // leaves it off for e.g. an UNRIPE-only model because unripe IS the green
    // class); we never fall back to a hardcoded macro.
    if (model.green_veto_threshold > 0.0f &&
        model.green_bin_start < model.green_bin_end &&
        model.green_bin_end <= 8) {
        float green_mass = 0.0f;
        for (int i = model.green_bin_start; i < model.green_bin_end; i++)
            green_mass += state[i];
        if (green_mass > model.green_veto_threshold){
            raw_scores[1] = -999.0f;   // PERFECTLY_RIPE
            raw_scores[4] = -999.0f;   // ARTIFICIALLY_RIPENED
        }
    }
}

// Converts a vector of summed per-class logits into a decided BiologicalStatus:
// softmax, anomaly checks, ripeness index, entropy and confidence.
static BiologicalStatus decide_32d(
    const float logits[NUM_CLASSES]
){
    BiologicalStatus result;
    float probs[NUM_CLASSES];

    float max_score = logits[0];
    for (int i = 1; i < NUM_CLASSES; i++)
        if (logits[i] > max_score) max_score = logits[i];

    float sum_exp = 0.0f;
    for (int i = 0; i < NUM_CLASSES; i++){
        probs[i] = expf(logits[i] - max_score);
        sum_exp += probs[i];
    }
    float inv_sum = 1.0f / (sum_exp + 1e-15f);
    for (int i = 0; i < NUM_CLASSES; i++)
        result.probabilities[i] = probs[i] * inv_sum;

    float p_rotten = result.probabilities[3];
    float p_artif  = result.probabilities[4];

    if (p_rotten > ANOMALY_CONFIDENCE_THRESHOLD && p_rotten > p_artif){
        result.primary_decision = CLASS_LABELS[3];
        result.is_anomaly = true;
        result.ripeness_index = 0.0f;
    } else if (p_artif > ANOMALY_CONFIDENCE_THRESHOLD){
        result.primary_decision = CLASS_LABELS[4];
        result.is_anomaly = true;
        result.ripeness_index = 0.0f;
    } else {
        result.is_anomaly = false;
        int best = 0;
        float best_val = result.probabilities[0];
        for (int i = 1; i < 3; i++){
            if (result.probabilities[i] > best_val){ best_val = result.probabilities[i]; best = i; }
        }
        result.primary_decision = CLASS_LABELS[best];
        float sum_mat = result.probabilities[0] + result.probabilities[1] + result.probabilities[2];
        result.ripeness_index = (sum_mat > 0.001f) ?
            (1.0f * result.probabilities[0] + 2.0f * result.probabilities[1] + 3.0f * result.probabilities[2]) / sum_mat : 1.0f;
    }

    float H = 0.0f;
    for (int i = 0; i < NUM_CLASSES; i++){
        float p = result.probabilities[i];
        if (p > 1e-5f) H -= p * logf(p);
    }
    result.transition_entropy = H;

    float pmax = 0.0f;
    for (int i = 0; i < NUM_CLASSES; i++)
        if (result.probabilities[i] > pmax) pmax = result.probabilities[i];
    result.confidence = clamp_f(pmax * 100.0f, 0.0f, 100.0f);

    return result;
}

static BiologicalStatus evaluate_scores(
    const float state[VECTOR_DIMENSIONS],
    const ManifoldModel32D& model
){
    float raw_scores[NUM_CLASSES];
    score_logits_32d(state, model, raw_scores);
    return decide_32d(raw_scores);
}

BiologicalStatus evaluate_fruit_single_32d(
    const float state[VECTOR_DIMENSIONS],
    const ManifoldModel32D& model
){
    return evaluate_scores(state, model);
}

// Generalized N-tap consensus: each tap's raw per-tap logits are summed across
// taps (a product-of-likelihoods / sum-of-logits fusion), then a single softmax
// and decision are applied. This compounds confidence monotonically with the
// number of agreeing taps (statistically correct for i.i.d. taps), unlike a
// median-of-posteriors vote which saturates around a single tap's confidence.
BiologicalStatus evaluate_fruit_ntap_32d(
    const float states[][VECTOR_DIMENSIONS],
    uint8_t n_taps,
    const ManifoldModel32D& model
){
    if (n_taps <= 1) {
        return evaluate_fruit_single_32d(states[0], model);
    }
    if (n_taps > MAX_CONSENSUS_TAPS) n_taps = MAX_CONSENSUS_TAPS;

    float summed_logits[NUM_CLASSES] = {0.0f};
    float tap_logits[NUM_CLASSES];

    for (uint8_t i = 0; i < n_taps; i++) {
        score_logits_32d(states[i], model, tap_logits);
        for (int c = 0; c < NUM_CLASSES; c++) summed_logits[c] += tap_logits[c];
    }

    return decide_32d(summed_logits);
}
