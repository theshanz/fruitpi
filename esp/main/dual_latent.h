#pragma once

#include <cstdint>
#include "config.h"
#include "extract_hues.h"
#include "piezo_acoustic.h"
#include "sci_32d.h"

// ─── Dual-Latent modality engine ───────────────────────────────────
// The decision latent is a single ripeness axis q ∈ [0,1+) fed by the model's
// MODALITY SWITCH and per-modality weights:
//
//   ACOUSTIC : q = q_int = 0.5*q_spec + 0.5*q_mom
//       q_spec <- sigmoid(ALPHA*(r_spec - R0_SPEC)),  r_spec = median over
//                 taps of (FBM-2D shear ratio) - BETA*ln(peak_abs)
//       q_mom  <- sigmoid(ALPHA*(r_mom - R0_MOM)),    r_mom  = median over
//                 taps of (entropy + tail) - (abbott + STFT t0 band0)
//   OPTICS     : q = q_surf = 1 - green-bin mass (skin ripeness)
//   DUAL       : q = W_ACOUSTIC*q_int + W_OPTICS*q_surf
//
// The per-tap inputs (fbm2d_r_high_low, peak_abs, stft_grid, bio_moments) are
// computed on-device by PiezoAcoustic. Parameters come from EITHER the
// compiled-in defaults (config.h) OR a loaded 852-byte DualModelWire blob
// stored in NVS (uploaded/listed/activated like the old "model").
constexpr uint8_t MAX_DUAL_TAPS = 7;

// Runnable parameters decoded from either the wire or the compiled defaults.
struct DualLatentParams {
    uint8_t modality;              // Modality enum (0 = ACOUSTIC...)
    uint8_t active_class_mask;     // category switches
    uint8_t green_bin_start;
    uint8_t green_bin_end;
    float   r0_spec;
    float   r0_mom;
    float   alpha;
    float   beta;
    float   tau;
    float   w_acoustic;
    float   w_optics;
    float   vol_clamp_lo;
    float   vol_clamp_hi;
    float   vol23_lo;
    float   vol23_hi;
    float   anchors[NUM_CLASSES];
    uint16_t min_peak_adc;           // tap-quality amplitude floor (counts)
                                     // wire @113; 0 on wire -> default
    uint8_t acoustic_feature_mode;   // 0 = Standard r_spec (legacy, default),
                                     // 1 = STFT-enhanced with stft_t1 term.
                                     // wire @116; default = LEGACY so pre-hook
                                     // blobs run the exact validated engine.
    float   stft_t1_prior;           // decision midpoint on the STFT T1
                                     // early-decay coordinate (wire @120), read
                                     // only when acoustic_feature_mode == 1.
};

// Compiled-in defaults (config.h) — used when no model blob is active.
DualLatentParams dual_params_default();

// Structural validation of an 852-byte DualModelWire: magic, version, name,
// coherent ranges, finite constants, canonical (zeroed) reserved region.
// Mirrors what the builder emits and what the store trusts.
bool dual_wire_valid(const DualModelWire& m);

// Decodes a validated 852-byte wire into runnable params.
DualLatentParams dual_params_decode(const DualModelWire& m);

// Parametrized N-tap consensus driven by an uploaded model blob.
BiologicalStatus evaluate_fruit_dual(
    const DualLatentParams& p,
    const AcousticFeatures taps[MAX_DUAL_TAPS],
    uint8_t n_taps,
    const ColorFeatures& vision);

// N-tap consensus with the compiled-in defaults: median-aggregate the per-tap
// spectral/moment manifolds FIRST, then a SINGLE decision (matches the
// validated Python engine — a median of posterior votes is NOT used).
BiologicalStatus evaluate_fruit_dual(
    const AcousticFeatures taps[MAX_DUAL_TAPS],
    uint8_t n_taps,
    const ColorFeatures& vision);

// ─── Dual-native BLE debug payload (repurposed) ────────────────────
// The legacy 32-dim state block is gone. Each tap now emits its exact
// contribution to the run as real-manifold + latent coordinates, computed
// with the SAME math the engine uses (parity with evaluate_fruit_dual).
struct DualTapCoords {
    float r_spec;   // real-manifold spectral coord (beta de-coated)
    float r_mom;    // real-manifold moment coord
    float q_spec;   // sigmoid spectral latent
    float q_mom;    // sigmoid moment latent
    float stft_t1;  // STFT T1 early-decay cell (contrast-normalized 0..1),
                    // tap.stft_grid[1*4+2]; additive debug field (mode 1 hook)
};

// Volume^(2/3) -> [0,1] normalizer (shared with the engine + debug payload).
float dual_volume_norm(const DualLatentParams& p, float vol_cm3);

// One tap's manifold coords for the debug payload (same math as the engine).
DualTapCoords dual_tap_coords(
    const DualLatentParams& p,
    const AcousticFeatures& tap,
    float vol_norm);