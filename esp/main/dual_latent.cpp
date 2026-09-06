#include "dual_latent.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <Arduino.h>

namespace {

constexpr float SIG_CLAMP = 50.0f;   // exp() guard for the sigmoid
constexpr float LOG_PEAK_FLOOR = 1e-6f;

float clamp_f(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float sigmoid(float z) {
    if (z >  SIG_CLAMP) z =  SIG_CLAMP;
    if (z < -SIG_CLAMP) z = -SIG_CLAMP;
    return 1.0f / (1.0f + expf(-z));
}

// Median of a small array (mutates a scratch copy; n <= MAX_DUAL_TAPS).
float median_n(const float* xs, uint8_t n) {
    float tmp[MAX_DUAL_TAPS];
    for (uint8_t i = 0; i < n; i++) tmp[i] = xs[i];
    for (uint8_t i = 1; i < n; i++) {
        float key = tmp[i];
        int j = (int)i - 1;
        while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = key;
    }
    float m = (n % 2 == 1) ? tmp[n / 2]
                           : 0.5f * (tmp[n / 2 - 1] + tmp[n / 2]);
    return m;
}

// Vol^(2/3) -> [0,1] normalizer: the engine's abbott-scaled momentum term.
// Helper (below) shared between the consensus engine and the debug payload.

// q_surf: skin-color ripeness from the 8-bin hue histogram. Green bins
// oppose ripeness (82.5-120 deg), so q_surf = 1 - green mass — 0 for unripe
// green skin, ~1 for yellow/orange ripe skin.
float surface_q(const DualLatentParams& p, const ColorFeatures& vision) {
    float green_mass = 0.0f;

    for (int i = p.green_bin_start; i < p.green_bin_end; i++)
        green_mass += vision.hue_histogram[i];
    return clamp_f(1.0f - green_mass, 0.0f, 1.0f);
}

// Canonical masked 1D metric decision on the ripeness latent q. For mask
// 0x03 the flip point falls at the UNRIPE/PERFECTLY_RIPE anchor midpoint
// (0.10 + 0.85)/2 = 0.475 — the boundary the fingerr test reproduced.
BiologicalStatus decide_latent(const DualLatentParams& p, float q) {
    BiologicalStatus result;
    memset(&result, 0, sizeof(result));

    const uint8_t mask = p.active_class_mask;
    float logits[NUM_CLASSES];
    float running_max = -1e30f;
    for (int c = 0; c < NUM_CLASSES; c++) {
        if (!((mask >> c) & 1U)) { logits[c] = -100000.0f; continue; }
        float diff = q - p.anchors[c];
        logits[c] = -(diff * diff) / p.tau;
        if (logits[c] > running_max) running_max = logits[c];
    }

    float sum_exp = 0.0f;
    for (int c = 0; c < NUM_CLASSES; c++) {
        result.probabilities[c] =
            (running_max > -1e29f) ? expf(logits[c] - running_max) : 0.0f;
        sum_exp += result.probabilities[c];
    }
    float inv = 1.0f / (sum_exp + 1e-15f);
    for (int c = 0; c < NUM_CLASSES; c++)
        result.probabilities[c] *= inv;

    int best = 0;
    float pmax = result.probabilities[0];
    for (int i = 1; i < NUM_CLASSES; i++)
        if (result.probabilities[i] > pmax) { pmax = result.probabilities[i]; best = i; }

    result.primary_decision = CLASS_LABELS[best];
    result.ripeness_index = q;
    result.confidence = clamp_f(pmax * 100.0f, 0.0f, 100.0f);

    float H = 0.0f;
    for (int i = 0; i < NUM_CLASSES; i++) {
        float p_i = result.probabilities[i];
        if (p_i > 1e-5f) H -= p_i * logf(p_i);
    }
    result.transition_entropy = H;

    // Rotten/hollow and artificially-ripened are biological anomalies.
    result.is_anomaly = ((best == 3 || best == 4) &&
                         result.probabilities[best] > ANOMALY_CONFIDENCE_THRESHOLD);
    return result;
}

const char* modality_name(uint8_t m) {
    switch (m) {
        case MODALITY_ACOUSTIC: return "ACOUSTIC(optics w=0.0)";
        case MODALITY_OPTICS:   return "OPTICS";
        default:                return "DUAL";
    }
}

}  // namespace

// ─── Dual-native BLE debug payload helpers (repurposed) ─────────────
// Exposed so the per-tap coords STREAMED over BLE are byte-for-byte the same
// math the consensus engine aggregates. The 32-dim state block is GONE.
float dual_volume_norm(const DualLatentParams& p, float vol_cm3) {
    if (vol_cm3 < p.vol_clamp_lo) vol_cm3 = p.vol_clamp_lo;
    if (vol_cm3 > p.vol_clamp_hi) vol_cm3 = p.vol_clamp_hi;
    float vol23 = powf(vol_cm3, 2.0f / 3.0f);
    return clamp_f((vol23 - p.vol23_lo) / (p.vol23_hi - p.vol23_lo),
                   0.0f, 1.0f);
}

DualTapCoords dual_tap_coords(const DualLatentParams& p,
                              const AcousticFeatures& tap,
                              float vol_norm) {
    DualTapCoords c{};
    // q_spec coordinate: real force de-coating of the FBM-2D shear ratio.
    // The de-coating factor BETA was fit on RAW ADC-count peaks
    // (peak_of() = max|w|, counts scale; e.g. the fingerr dataset peaks are
    // ~2000 counts). tap.peak_abs is stored NORMALIZED to 0..1, so rescale it
    // back to counts before taking the log — otherwise ln(peak) is negative
    // and the -beta*ln() term flips sign, biasing every r_spec ~ +1.66 toward
    // the ripe anchor (the "everything reads PERFECTLY_RIPE" bug).
    float log_p = logf(fmaxf(tap.peak_abs * PIEZO_ADC_FULL_SCALE,
                             LOG_PEAK_FLOOR));
    c.r_spec = tap.fbm2d_r_high_low - p.beta * log_p;
    // q_mom coordinate: signed bio-moment contrast
    //   (spectral_entropy + tail_energy) - (abbott + STFT t0 band0)
    // abbott = fund_e/4 x volume^(2/3) normalizer, EXACTLY like the ggg/
    // fingerr reference state[29] = clip(fund_e/4 * state[9]) (volume term is
    // part of the canonical formula; offline parity replay confirms it).
    float abbott = clamp_f(tap.bio_moments[3] * vol_norm, 0.0f, 1.0f);
    c.r_mom = (tap.bio_moments[4] + tap.bio_moments[1]) -
              (abbott + tap.stft_grid[0]);
    c.q_spec = sigmoid(p.alpha * (c.r_spec - p.r0_spec));
    c.q_mom  = sigmoid(p.alpha * (c.r_mom  - p.r0_mom));
    return c;
}

// ─── Params: compiled defaults & wire decode ───────────────────────
DualLatentParams dual_params_default() {
    DualLatentParams p{};
    p.modality            = (uint8_t)ACTIVE_MODALITY;
    p.active_class_mask   = MODAL_ACTIVE_CLASS_MASK;
    p.green_bin_start     = (uint8_t)MS_SURF_GREEN_BINS_START;
    p.green_bin_end       = (uint8_t)MS_SURF_GREEN_BINS_END;
    p.r0_spec             = DUAL_R0_SPEC;
    p.r0_mom              = DUAL_R0_MOM;
    p.alpha               = DUAL_ALPHA;
    p.beta                = DUAL_BETA;
    p.tau                 = DUAL_TAU;
    p.w_acoustic          = MODAL_W_ACOUSTIC;
    p.w_optics            = MODAL_W_OPTICS;
    p.vol_clamp_lo        = DUAL_VOL_CLAMP_LO;
    p.vol_clamp_hi        = DUAL_VOL_CLAMP_HI;
    p.vol23_lo            = DUAL_VOL23_LO;
    p.vol23_hi            = DUAL_VOL23_HI;
    for (int c = 0; c < NUM_CLASSES; c++) p.anchors[c] = DUAL_ANCHORS[c];
    p.min_peak_adc = DUAL_DEFAULT_MIN_PEAK_ADC;
    return p;
}

static uint16_t wire_min_peak_adc(const DualModelWire& m) {
    return (uint16_t)m.min_peak_adc_lo | ((uint16_t)m.min_peak_adc_hi << 8);
}

bool dual_wire_valid(const DualModelWire& m) {
    // Magic + wire version: anything else is a legacy 32D/corrupt blob.
    if (m.magic != DUAL_MODEL_MAGIC) return false;
    if (m.wire_version != DUAL_MODEL_WIRE_VERSION) return false;

    // Name: printable ASCII, NUL-terminated inside the 32-byte field.
    const uint8_t* name = reinterpret_cast<const uint8_t*>(m.fruit_name);
    if (name[0] < 0x20 || name[0] == 0x7F) return false;
    bool terminated = false;
    for (int i = 0; i < 32; i++) {
        if (name[i] == 0) { terminated = true; break; }
        if (name[i] < 0x20 || name[i] == 0x7F) return false;
    }
    if (!terminated) return false;

    // Category switches: at least one enabled class, none outside the set.
    const uint8_t mask = m.active_class_mask;
    if (mask == 0 || (mask & ~(uint8_t)((1u << NUM_CLASSES) - 1)) != 0)
        return false;
    if (m.modality > MODALITY_DUAL) return false;
    if (m.green_bin_start >= m.green_bin_end || m.green_bin_end > 8)
        return false;

    // Reserved regions must be canonical (all zero).
    for (size_t i = 0; i < sizeof(m.reserved); i++)
        if (m.reserved[i] != 0) return false;
    if (m.reserved2_pad != 0) return false;
    for (size_t i = 0; i < sizeof(m.reserved2); i++)
        if (m.reserved2[i] != 0) return false;

    // min_peak_adc: 0 = unspecified (falls back to default), otherwise a valid
    // 12-bit ADC amplitude floor.
    const uint16_t mp = wire_min_peak_adc(m);
    if (mp != 0 && mp > (uint16_t)PIEZO_ADC_FULL_SCALE) return false;

    // Floating parameters finite and within the ranges the builder emits.
    if (!std::isfinite(m.r0_spec) || !std::isfinite(m.r0_mom)) return false;
    if (!std::isfinite(m.alpha) || m.alpha <= 0.0f) return false;
    if (!std::isfinite(m.beta) || m.beta < 0.0f) return false;
    if (!std::isfinite(m.tau) || m.tau <= 0.0f) return false;
    if (!std::isfinite(m.w_acoustic) || !std::isfinite(m.w_optics)) return false;
    if (m.w_acoustic < 0.0f || m.w_optics < 0.0f) return false;
    if (m.vol_clamp_lo <= 0.0f || m.vol_clamp_hi <= m.vol_clamp_lo) return false;
    if (m.vol23_lo < 0.0f || m.vol23_hi <= m.vol23_lo) return false;
    for (int c = 0; c < NUM_CLASSES; c++)
        if (!std::isfinite(m.anchors[c])) return false;
    return true;
}

DualLatentParams dual_params_decode(const DualModelWire& m) {
    DualLatentParams p{};
    p.modality          = m.modality;
    p.active_class_mask = m.active_class_mask;
    p.green_bin_start   = m.green_bin_start;
    p.green_bin_end     = m.green_bin_end;
    p.r0_spec           = m.r0_spec;
    p.r0_mom            = m.r0_mom;
    p.alpha             = m.alpha;
    p.beta              = m.beta;
    p.tau               = m.tau;
    p.w_acoustic        = m.w_acoustic;
    p.w_optics          = m.w_optics;
    p.vol_clamp_lo      = m.vol_clamp_lo;
    p.vol_clamp_hi      = m.vol_clamp_hi;
    p.vol23_lo          = m.vol23_lo;
    p.vol23_hi          = m.vol23_hi;
    for (int c = 0; c < NUM_CLASSES; c++) p.anchors[c] = m.anchors[c];
    const uint16_t mp = wire_min_peak_adc(m);
    p.min_peak_adc = mp != 0 ? mp : (uint16_t)DUAL_DEFAULT_MIN_PEAK_ADC;
    return p;
}

// ─── N-tap consensus engine (parametrized) ─────────────────────────
// `src` labels the serial log source (e.g. "blob" vs "default").
static BiologicalStatus evaluate_fruit_dual_impl(
    const DualLatentParams& p,
    const AcousticFeatures taps[MAX_DUAL_TAPS],
    uint8_t n_taps,
    const ColorFeatures& vision,
    const char* src)
{
    if (n_taps == 0) n_taps = 1;
    if (n_taps > MAX_DUAL_TAPS) n_taps = MAX_DUAL_TAPS;

    float vol_norm = dual_volume_norm(p, vision.volume_cm3);

    // ── Stage 1/2: per-tap real-manifold coordinates ──
    float r_spec[MAX_DUAL_TAPS];
    float r_mom[MAX_DUAL_TAPS];
    for (uint8_t i = 0; i < n_taps; i++) {
        DualTapCoords c = dual_tap_coords(p, taps[i], vol_norm);
        r_spec[i] = c.r_spec;
        r_mom[i] = c.r_mom;
    }

    // ── Stage 3: order-invariant median aggregation ──
    float r_spec_fruit = median_n(r_spec, n_taps);
    float r_mom_fruit  = median_n(r_mom, n_taps);

    // ── Stage 4: continuous physical manifold (sigmoid q) ──
    float q_spec = sigmoid(p.alpha * (r_spec_fruit - p.r0_spec));
    float q_mom  = sigmoid(p.alpha * (r_mom_fruit  - p.r0_mom));
    float q_int  = 0.5f * (q_spec + q_mom);

    // ── Stage 4b: optics channel (skin-color ripeness) ──
    float q_surf = surface_q(p, vision);

    // ── Stage 4c: modality switch combine ──
    float q;
    switch (p.modality) {
        case MODALITY_ACOUSTIC: q = q_int; break;
        case MODALITY_OPTICS:   q = q_surf; break;
        default: q = p.w_acoustic * q_int + p.w_optics * q_surf; break;
    }

    // ── Stage 5: canonical masked 1D metric decision ──
    BiologicalStatus result = decide_latent(p, q);

    Serial.printf("[Dual] modality=%s N=%u r_spec=%.4f r_mom=%.4f "
                  "q_spec=%.3f q_mom=%.3f q_int=%.3f q_surf=%.3f q=%.3f "
                  "-> %s (%.1f%%) src=%s mask=0x%02X\n",
                  modality_name(p.modality), n_taps,
                  r_spec_fruit, r_mom_fruit, q_spec, q_mom, q_int, q_surf, q,
                  result.primary_decision, (double)result.confidence,
                  src, p.active_class_mask);

    return result;
}

BiologicalStatus evaluate_fruit_dual(
    const DualLatentParams& p,
    const AcousticFeatures taps[MAX_DUAL_TAPS],
    uint8_t n_taps,
    const ColorFeatures& vision)
{
    return evaluate_fruit_dual_impl(p, taps, n_taps, vision, "blob");
}

BiologicalStatus evaluate_fruit_dual(
    const AcousticFeatures taps[MAX_DUAL_TAPS],
    uint8_t n_taps,
    const ColorFeatures& vision)
{
    return evaluate_fruit_dual_impl(dual_params_default(), taps, n_taps,
                                    vision, "default");
}