#include "sci_28d.h"
#include <cstring>
#include <cmath>
#include <algorithm>

const float FFT_CENTERS[N_FFT_BINS] = {
    150.0f, 250.0f, 350.0f, 450.0f, 550.0f,
    650.0f, 750.0f, 850.0f, 950.0f, 1100.0f,
    1300.0f, 1500.0f, 1700.0f, 1900.0f, 2100.0f
};

const char* const  CLASS_LABELS[NUM_CLASSES] = {
    "UNRIPE",
    "PERFECTLY_RIPE",
    "OVERRIPE",
    "ROTTEN_OR_HOLLOW",
    "ARTIFICIALLY_RIPENED"
};

// When SCI_28D_HARDCODED is defined, this file compiles to only the shared
// constants above; the rule-based implementation (sci_28d_hardcoded.cpp)
// provides the same functions instead. Callers are unchanged.
#if !defined(SCI_28D_HARDCODED)

#if defined(ESP_PLATFORM) && __has_include("dsps_dotprod.h")
    #include "dsps_dotprod.h"
    #define HAS_ESP_DSP 1
#else
    #define HAS_ESP_DSP 0
#endif

static inline float clamp_f(float v, float lo, float hi){
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float normalize_range(float val,float low,float high){
    if(val < low) return 0.0f;
    if(val > high) return 1.0f;
    return (val - low) / (high - low);
}

// ─── ESP32-S3 Hardware Vector SIMD Execution ──────────────────────
static inline float compute_dot_product(const float* a, const float* b, int len) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    float out = 0.0f;

    // Calls Xtensa LX7 AE32 128-bit SIMD Assembly Kernel directly
    dsps_dotprod_f32_ae32(a, b, &out, len);

    return out;
#elif defined(ESP_PLATFORM)
    float out = 0.0f;
    dsps_dotprod_f32(a, b, &out, len); // Generic ESP32 DSP call
    return out;
#else
    // Portable fallback for non-ESP targets
    float sum = 0.0f;
    for (int i = 0; i < len; i++) sum += a[i] * b[i];
    return sum;
#endif
}

void assemble_state_28d(
    float state[VECTOR_DIMENSIONS],
    const ColorFeatures& vision,
    const AcousticFeatures& acoustic
){

    for (int i = 0; i < 8; i++){
        state[i] = vision.hue_histogram[i];
    }

    state[8] = vision.chromatic_dispersion;

    float vol_23 = powf(vision.volume_cm3, 2.0f / 3.f);
    state[9] = normalize_range(vol_23, MIN_MASS23, MAX_MASS23);

    for (int i = 0; i < 15; i++) {
        state[10 + i] = acoustic.fft_bins[i];
    }

    state[25] = acoustic.spectral_entropy;

    state[26] = acoustic.hertzian_adc;

    state[27] = 0.0f;
}

BiologicalStatus evaluate_fruit_single(
    const float state[VECTOR_DIMENSIONS],
    const ManifoldModel28D& model
){
    BiologicalStatus result;
    float raw_scores[NUM_CLASSES];

    for (int c = 0; c < NUM_CLASSES; c++){
        raw_scores[c] = compute_dot_product(state, model.weights[c], VECTOR_DIMENSIONS) + model.biases[c];
    }

    float green_mass = 0.0f;
    for (int i = GREEN_BINS_START; i < GREEN_BINS_END; i++){
        green_mass += state[i];
    }

    if (green_mass > GREEN_MASS_VETO_THRESHOLD){

        raw_scores[1] = -999.0f; //PREFECTLY_RIPE
        raw_scores[4] = -999.0f; //ARTIFICIALLY_RIPENED
    }
///////////////////////////////////////////// result eval
    float max_score = raw_scores[0];
    for (int i = 1; i< NUM_CLASSES; i++){
        if (raw_scores[i] > max_score) max_score = raw_scores[i];
    }

    float sum_exp = 0.0f;
    for (int i = 0; i<NUM_CLASSES; i++){
        result.probabilities[i] = expf(raw_scores[i] - max_score);
        sum_exp += result.probabilities[i];
    }

    float inv_sum = 1.0f / (sum_exp + 1e-15f);
       for (int i = 0; i < NUM_CLASSES; i++) {
           result.probabilities[i] *= inv_sum;
       }

       // Decision logic
       float p_rotten = result.probabilities[3];
       float p_artif  = result.probabilities[4];

       if (p_rotten > ANOMALY_CONFIDENCE_THRESHOLD && p_rotten > p_artif) {
           result.primary_decision = CLASS_LABELS[3];
           result.is_anomaly = true;
           result.ripeness_index = 0.0f;
       } else if (p_artif > ANOMALY_CONFIDENCE_THRESHOLD) {
           result.primary_decision = CLASS_LABELS[4];
           result.is_anomaly = true;
           result.ripeness_index = 0.0f;
       } else {
           result.is_anomaly = false;
           int best = 0;
           float best_val = result.probabilities[0];

           for (int i = 1; i < 3; i++) {
               if (result.probabilities[i] > best_val) {
                   best_val = result.probabilities[i];
                   best = i;
               }
           }
           result.primary_decision = CLASS_LABELS[best];

           float sum_mat = result.probabilities[0] + result.probabilities[1] + result.probabilities[2];
           result.ripeness_index = (sum_mat > 0.001f) ?
               (1.0f * result.probabilities[0] + 2.0f * result.probabilities[1] + 3.0f * result.probabilities[2]) / sum_mat : 1.0f;
       }

       // Entropy & Confidence
       float H = 0.0f;
       for (int i = 0; i < NUM_CLASSES; i++) {
           float p = result.probabilities[i];
           if (p > 1e-5f) H -= p * logf(p);
       }
       result.transition_entropy = H;

       constexpr float LOG_5 = 1.6094379124f;
       result.confidence = clamp_f((1.0f - (H / LOG_5)) * 100.0f, 0.0f, 100.0f);
//////////////////////////////////////////////
       return result;
}

BiologicalStatus evaluate_fruit_3tap(
    const float state_tap1[VECTOR_DIMENSIONS],
    const float state_tap2[VECTOR_DIMENSIONS],
    const float state_tap3[VECTOR_DIMENSIONS],
    const ManifoldModel28D& model
) {
    BiologicalStatus t1 = evaluate_fruit_single(state_tap1, model);
    BiologicalStatus t2 = evaluate_fruit_single(state_tap2, model);
    BiologicalStatus t3 = evaluate_fruit_single(state_tap3, model);

    float fused[NUM_CLASSES];
    for (int c = 0; c < NUM_CLASSES; c++) {
        float a = t1.probabilities[c];
        float b = t2.probabilities[c];
        float c_ = t3.probabilities[c];

        if (a > b) std::swap(a, b);
        if (b > c_) {
            std::swap(b, c_);
            if (a > b) std::swap(a, b);
        }
        fused[c] = b;
    }

    float sum_fused = 0.0f;
    for (int c = 0; c < NUM_CLASSES; c++) sum_fused += fused[c];

    float inv_sum = 1.0f / (sum_fused + 1e-15f);
    for (int c = 0; c < NUM_CLASSES; c++) fused[c] *= inv_sum;

    BiologicalStatus result;
    for (int c = 0; c < NUM_CLASSES; c++) result.probabilities[c] = fused[c];

    float p_rotten = fused[3];
    float p_artif  = fused[4];

    if (p_rotten > ANOMALY_CONFIDENCE_THRESHOLD && p_rotten > p_artif) {
        result.primary_decision = CLASS_LABELS[3];
        result.is_anomaly = true;
        result.ripeness_index = 0.0f;
    } else if (p_artif > ANOMALY_CONFIDENCE_THRESHOLD) {
        result.primary_decision = CLASS_LABELS[4];
        result.is_anomaly = true;
        result.ripeness_index = 0.0f;
    } else {
        result.is_anomaly = false;
        int best = 0;
        float best_val = fused[0];

        for (int i = 1; i < 3; i++) {
            if (fused[i] > best_val) {
                best_val = fused[i];
                best = i;
            }
        }
        result.primary_decision = CLASS_LABELS[best];

        float sum_mat = fused[0] + fused[1] + fused[2];
        result.ripeness_index = (sum_mat > 0.001f) ?
            (1.0f * fused[0] + 2.0f * fused[1] + 3.0f * fused[2]) / sum_mat : 1.0f;
    }

    float H = 0.0f;
    for (int i = 0; i < NUM_CLASSES; i++) {
        float p = fused[i];
        if (p > 1e-5f) H -= p * logf(p);
    }
    result.transition_entropy = H;

    constexpr float LOG_5 = 1.6094379124f;
    result.confidence = clamp_f((1.0f - (H / LOG_5)) * 100.0f, 0.0f, 100.0f);

    return result;
}

#endif // !defined(SCI_28D_HARDCODED)
