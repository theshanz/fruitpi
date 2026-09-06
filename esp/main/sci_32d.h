#pragma once

#include <cstdint>
#include "config.h"
#include "extract_hues.h"
#include "piezo_acoustic.h"

// Shared biological ontology + the 852-byte LOADABLE DUAL-LATENT MODEL wire.
// The Dual-Latent engine (dual_latent.h/cpp) is the ONLY classifier in this
// firmware — the legacy 32D Diagonal-Gaussian scorer (and its 32-dim state
// block) has been REMOVED, as has per-tap 32-dim assembly. The BLE debug_state
// payload is now dual-native (per-tap r_spec/r_mom coords, see dual_latent.h).
constexpr int NUM_CLASSES = 5;

extern const char* const CLASS_LABELS[NUM_CLASSES];

struct BiologicalStatus {
    const char* primary_decision;
    float probabilities[NUM_CLASSES];
    float ripeness_index;
    float transition_entropy;
    float confidence;
    bool is_anomaly;
};

// ─── 852-byte Dual-Latent model wire format ────────────────────────
// Packed (no padding) so the struct IS the binary the app uploads/downloads
// over BLE — the existing 852-byte transfer contract is unchanged, only the
// payload is now a Dual-Latent model instead of a 32D profile. Offsets:
//   0..31   fruit_name[32]
//   32..35  magic 'DLAT' (DUAL_MODEL_MAGIC)
//   36      wire_version
//   37      active_class_mask        (category switches: classes in the softmax)
//   38      modality switch          (0 = ACOUSTIC, 1 = OPTICS, 2 = DUAL)
//   39      green_bin_start          (optics skin-color channel range)
//   40      green_bin_end
//   41..47  reserved[7]
//   48..51  r0_spec                  (spectral sigmoid midpoint, ggg-fit)
//   52..55  r0_mom                   (moment sigmoid midpoint, ggg-fit)
//   56..59  alpha                    (sigmoid slope)
//   60..63  beta                     (force de-coating factor)
//   64..67  tau                      (masked-softmax temperature)
//   68..71  w_acoustic               (weights on modalities — DUAL combine)
//   72..75  w_optics
//   76..79  vol_clamp_lo             (volume norm clamps)
//   80..83  vol_clamp_hi
//   84..87  vol23_lo                 (vol^(2/3) norm range)
//   88..91  vol23_hi
//   92..111 anchors[5]               (category centroids on the q latent)
//   112     reserved2_pad            (must be zero)
//   113..114 min_peak_adc            (u16 LE tap-quality amplitude floor; 0 =
//                                    unspecified -> DUAL_DEFAULT_MIN_PEAK_ADC)
//   115     reserved2_tail0          (must be zero; keeps offset 116 for the
//                                    feature-mode byte below)
//   116     acoustic_feature_mode    (0 = Standard r_spec [Default]; backward
//                                    compatible exactly with pre-hook blobs.
//                                    1 = STFT-enhanced AcousticFeatures branch,
//                                    blends the early-decay STFT T1 cell)
//   117..119 reserved2_tail[3]       (must be zero)
//   120..123 stft_t1_prior           (f32 LE decision midpoint on the STFT T1
//                                    early-decay coordinate; only read when
//                                    acoustic_feature_mode == 1)
//   124..851 reserved2[728]          (must be zero)
struct __attribute__((packed)) DualModelWire {
    char    fruit_name[32];
    uint32_t magic;
    uint8_t  wire_version;
    uint8_t  active_class_mask;
    uint8_t  modality;
    uint8_t  green_bin_start;
    uint8_t  green_bin_end;
    uint8_t  reserved[7];
    float    r0_spec;
    float    r0_mom;
    float    alpha;
    float    beta;
    float    tau;
    float    w_acoustic;
    float    w_optics;
    float    vol_clamp_lo;
    float    vol_clamp_hi;
    float    vol23_lo;
    float    vol23_hi;
    float    anchors[NUM_CLASSES];
    uint8_t  reserved2_pad;           // 112
    uint8_t  min_peak_adc_lo;         // 113 (u16 LE)
    uint8_t  min_peak_adc_hi;         // 114
    uint8_t  reserved2_tail0;         // 115 (must be zero)
    uint8_t  acoustic_feature_mode;   // 116 (0 Standard / 1 STFT-enhanced)
    uint8_t  reserved2_tail[3];       // 117..119 (must be zero)
    float    stft_t1_prior;           // 120..123 (f32 LE)
    uint8_t  reserved2[728];          // 124..851
};
static_assert(sizeof(DualModelWire) == 852, "DualModelWire must be 852 bytes");

// Acoustic feature hook modes (see acoustic_feature_mode @116). True backward
// compatibility: mode 0 runs the exact validated r_spec Dual-Latent engine, so
// pre-hook blobs (which zero this byte) produce bit-identical results.
constexpr uint8_t ACOUSTIC_FEATURE_MODE_LEGACY = 0;      // Standard r_spec
constexpr uint8_t ACOUSTIC_FEATURE_MODE_STFT   = 1;      // r_spec + STFT T1
constexpr uint8_t ACOUSTIC_FEATURE_MODE_DEFAULT = ACOUSTIC_FEATURE_MODE_LEGACY;

constexpr size_t MODEL_WIRE_BYTES = 852;

// Wire signature + store format marker. The 852-byte wire keeps its own magic
// at offset 32; the FruitStore marker borrows the same value. Storing the magic
// lets the device/host reject a legacy 32D blob (which has '2FRT' semantics and
// no magic at 32) that survived an older flash image.
constexpr uint32_t DUAL_MODEL_MAGIC = 0x444C4154;   // "DLAT"
constexpr uint8_t  DUAL_MODEL_WIRE_VERSION = 1;
constexpr uint8_t  MODEL_STORE_VERSION = 4;         // v4 = Dual wire (v3 was 32D)

// DISPLAY-ONLY confidence for the LCD: a decisive posterior saturates
// (d'~5.8) to ~0.99999, so a raw bar reads as a flat full 100%. Smooth the
// posterior toward uniform (Laplace, alpha=0.08 — matching the app's readout)
// and return the softened max-probability in percent. The authoritative
// classification/decision is unaffected; this only makes the meter legible.
float display_confidence(const BiologicalStatus& status);