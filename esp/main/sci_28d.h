#pragma once

#include <cstdint>
#include "config.h"
#include "extract_hues.h"
#include "piezo_acoustic.h"

constexpr int VECTOR_DIMENSIONS = 28;
constexpr int NUM_CLASSES = 5;

constexpr int GREEN_BINS_START = 5;  // hue bins 82.5°–120° = green
constexpr int GREEN_BINS_END   = 8;

// Classifier knobs live in config.h (GREEN_MASS_VETO_THRESHOLD,
// ANOMALY_CONFIDENCE_THRESHOLD, MIN_MASS23, MAX_MASS23).

extern const char* const CLASS_LABELS[NUM_CLASSES];

// Center frequencies (Hz) for the 15 FFT bins used to build state[10..24].
extern const float FFT_CENTERS[N_FFT_BINS];

struct BiologicalStatus {
    const char* primary_decision;
    float probabilities[NUM_CLASSES];
    float ripeness_index;
    float transition_entropy;
    float confidence;
    bool is_anomaly;
};

#if defined(_MSC_VER)
struct __declspec(align(16)) ManifoldModel28D {
#else
struct alignas(16) ManifoldModel28D {
#endif
    char fruit_name[32];
    float weights[NUM_CLASSES][VECTOR_DIMENSIONS];
    float biases[NUM_CLASSES];
};

using Fruit28D = ManifoldModel28D;

void assemble_state_28d(
    float state[VECTOR_DIMENSIONS],
    const ColorFeatures& vision,
    const AcousticFeatures& acoustic
);

BiologicalStatus evaluate_fruit_single(
    const float state[VECTOR_DIMENSIONS],
    const ManifoldModel28D& model
);

BiologicalStatus evaluate_fruit_3tap(
    const float state_tap1[VECTOR_DIMENSIONS],
    const float state_tap2[VECTOR_DIMENSIONS],
    const float state_tap3[VECTOR_DIMENSIONS],
    const ManifoldModel28D& model
);
