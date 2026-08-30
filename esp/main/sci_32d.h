#pragma once

#include <cstdint>
#include "config.h"
#include "extract_hues.h"
#include "piezo_acoustic.h"

// 32D Fruit Profile Engine.
// Block layout (raw, NON-normalized state — amplitude is a real cue):
//   dims 0..9    optics & mass  (8 hue bins + dispersion + volume^(2/3))
//   dims 10..25  acoustic image  (4x4 time-frequency STFT grid, contrast-norm)
//   dims 26..31  bio-moments     (temporal centroid, tail energy, harmonic
//                                 absorption, abbott stiffness, spectral
//                                 entropy, dynamic damping)
//
// SCORING MODEL (v3): Diagonal-Gaussian (Mahalanobis). The former cosine
// runner threw the block-L2 norm away and left near-parallel prototypes, so
// distinct ripe/unripe mangoes tied at ~51% confidence. v3 keeps the raw state
// (so absolute acoustic amplitude survives) and scores
//     score[c] = bias[c] - 0.5 * sum_d  inv_var[d] * (state[d]-mean[c][d])^2
// followed by softmax over the masked classes. The 852-byte wire layout is
// unchanged; only the meaning of the float payload changed:
//     feature_weights[32] = per-feature pooled inverse-variance (precision)
//     prototypes[5][32]   = per-class means (raw, block-normalization removed)
//     biases[5]           = per-class offset (priors); -100 masks inactive
constexpr int VECTOR_DIMENSIONS = 32;
constexpr int NUM_CLASSES = 5;

constexpr int BIO_CENTROID = 26;
constexpr int BIO_TAIL     = 27;
constexpr int BIO_HARMONIC = 28;
constexpr int BIO_ABBOTT   = 29;

// Optics block bounds (hue bins 82.5°–120° = green).
constexpr int GREEN_BINS_START = 5;
constexpr int GREEN_BINS_END   = 8;

extern const char* const CLASS_LABELS[NUM_CLASSES];

struct BiologicalStatus {
    const char* primary_decision;
    float probabilities[NUM_CLASSES];
    float ripeness_index;
    float transition_entropy;
    float confidence;
    bool is_anomaly;
};

// ─── 852-byte wire format (matches the validated FruitProfile32D) ───
// Offsets:
//   0..31   fruit_name[32]
//   32      active_class_mask
//   33      green_bin_start
//   34      green_bin_end
//   35      flags
//   36..55  green_veto_threshold, min_volume_cm3, max_volume_cm3,
//           expected_f0_hz, damping_scale, reserved[8]
//   64..191 feature_weights[32]      (v3: pooled inverse-variance precision)
//   192..831 prototypes[5][32]       (v3: per-class MEANS, raw not normalized)
//   832..851 biases[5]               (per-class priors; -100 masks inactive)
struct ManifoldModel32D {
    char fruit_name[32];
    uint8_t active_class_mask;
    uint8_t green_bin_start;
    uint8_t green_bin_end;
    uint8_t flags;
    float green_veto_threshold;
    float min_volume_cm3;
    float max_volume_cm3;
    float expected_f0_hz;
    float damping_scale;
    uint8_t reserved[8];
    float feature_weights[32];
    float prototypes[NUM_CLASSES][VECTOR_DIMENSIONS];
    float biases[NUM_CLASSES];
};

using Fruit32D = ManifoldModel32D;

constexpr size_t MODEL_WIRE_BYTES = 852;

// Store signature + format version persisted by FruitStore. The 852-byte wire
// has no spare header bytes (the app packs flags=0 and reserved[8]=0), so
// "2FRT" + version live only in NVS to reject legacy blobs at boot.
//   v1/v2 = 852-byte cosine runner (unit block-normalized prototypes).
//   v3    = 852-byte Diagonal-Gaussian scorer (feature_weights=inv variance,
//           prototypes=class means). The float payload meaning changed, so v3
//           bumps the number to force a wipe of any pre-v3 model blobs.
constexpr uint32_t FRUIT32D_MAGIC = 0x32465254;  // "2FRT"
constexpr uint8_t MODEL_STORE_VERSION = 3;

// Structural validation of a decoded 852-byte ManifoldModel32D. Mirrors what
// the app's pack_binary() emits; a legacy or corrupt read-back fails so the
// store never lists or activates a blob the classifier cannot trust.
bool model_blob_is_valid(const ManifoldModel32D& m);

void assemble_state_32d(
    float state[VECTOR_DIMENSIONS],
    const ColorFeatures& vision,
    const AcousticFeatures& acoustic
);

BiologicalStatus evaluate_fruit_single_32d(
    const float state[VECTOR_DIMENSIONS],
    const ManifoldModel32D& model
);

// Generalized N-tap consensus: sums each tap's raw logits (a product of
// likelihoods) and applies a single softmax + decision. n_taps==1
// short-circuits to the single-tap scorer.
constexpr uint8_t MAX_CONSENSUS_TAPS = 7;
BiologicalStatus evaluate_fruit_ntap_32d(
    const float states[][VECTOR_DIMENSIONS],
    uint8_t n_taps,
    const ManifoldModel32D& model
);
