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

// ─── v2 rule-engine helpers ─────────────────────────────────────────
// Where each class sits on the ripeness axis. UNRIPE 0, PERFECTLY_RIPE 1,
// OVERRIPE past the peak, ROTTEN decayed, ARTIFICIALLY_RIPENED chemically
// ripened. The rule posterior is projected onto this axis to produce the
// ripeness index.
constexpr float RULE_CLASS_RANK[NUM_CLASSES] = {0.00f, 1.00f, 0.70f, 0.20f, 0.50f};

// Per-feature values for one scan. Acoustic ids are medians over the taps,
// optical ids come from the hue histogram and the black-spot statistics.
struct RuleFeatureSet {
    float v[DISC_COUNT];
    bool  ok[DISC_COUNT];
};

void rule_feature_values(const DualLatentParams& p,
                         const AcousticFeatures taps[MAX_DUAL_TAPS],
                         uint8_t n_taps,
                         const ColorFeatures& vision,
                         RuleFeatureSet& out)
{
    memset(&out, 0, sizeof(out));

    const float vol_norm = dual_volume_norm(p, vision.volume_cm3);

    float f_shear[MAX_DUAL_TAPS], f_rspec[MAX_DUAL_TAPS], f_rmom[MAX_DUAL_TAPS];
    float f_ent[MAX_DUAL_TAPS], f_centroid[MAX_DUAL_TAPS], f_tail[MAX_DUAL_TAPS];
    float f_harm[MAX_DUAL_TAPS], f_abbott[MAX_DUAL_TAPS], f_bioent[MAX_DUAL_TAPS];
    float f_damp[MAX_DUAL_TAPS], f_t1[MAX_DUAL_TAPS], f_peak[MAX_DUAL_TAPS];
    float f_modeband[MAX_DUAL_TAPS];

    for (uint8_t i = 0; i < n_taps; i++) {
        const AcousticFeatures& t = taps[i];
        f_shear[i] = t.fbm2d_r_high_low;
        f_rspec[i] = t.fbm2d_r_high_low -
                     p.beta * logf(fmaxf(t.peak_abs * PIEZO_ADC_FULL_SCALE,
                                         LOG_PEAK_FLOOR));
        const float ab = clamp_f(t.bio_moments[3] * vol_norm, 0.0f, 1.0f);
        f_rmom[i]  = (t.bio_moments[4] + t.bio_moments[1]) - (ab + t.stft_grid[0]);
        f_ent[i]     = t.spectral_entropy;
        f_centroid[i]= t.bio_moments[0];
        f_tail[i]    = t.bio_moments[1];
        f_harm[i]    = t.bio_moments[2];
        f_abbott[i]  = t.bio_moments[3];
        f_bioent[i]  = t.bio_moments[4];
        f_damp[i]    = t.bio_moments[5];
        f_t1[i]      = t.stft_grid[1 * 4 + 2];
        f_peak[i]    = t.peak_abs;
        f_modeband[i]= t.mode_band_energy;
    }

    out.v[DISC_SHEAR]       = median_n(f_shear, n_taps);    out.ok[DISC_SHEAR] = true;
    out.v[DISC_R_SPEC]      = median_n(f_rspec, n_taps);    out.ok[DISC_R_SPEC] = true;
    out.v[DISC_R_MOM]       = median_n(f_rmom, n_taps);     out.ok[DISC_R_MOM] = true;
    out.v[DISC_ENTROPY]     = median_n(f_ent, n_taps);      out.ok[DISC_ENTROPY] = true;
    out.v[DISC_CENTROID]    = median_n(f_centroid, n_taps); out.ok[DISC_CENTROID] = true;
    out.v[DISC_TAIL]        = median_n(f_tail, n_taps);     out.ok[DISC_TAIL] = true;
    out.v[DISC_HARMONIC]    = median_n(f_harm, n_taps);     out.ok[DISC_HARMONIC] = true;
    out.v[DISC_ABBOTT]      = median_n(f_abbott, n_taps);   out.ok[DISC_ABBOTT] = true;
    out.v[DISC_BIO_ENTROPY] = median_n(f_bioent, n_taps);   out.ok[DISC_BIO_ENTROPY] = true;
    out.v[DISC_DAMPING]     = median_n(f_damp, n_taps);     out.ok[DISC_DAMPING] = true;
    out.v[DISC_STFT_T1]     = median_n(f_t1, n_taps);       out.ok[DISC_STFT_T1] = true;
    out.v[DISC_PEAK]        = median_n(f_peak, n_taps);     out.ok[DISC_PEAK] = true;
    // The mean, not the median: the mode band is a narrow cell, so its
    // per-tap values scatter more than the wide-band features, and the
    // median throws away the resolution the class boundary needs.  Measured
    // on the Mango-capped corpus with the device's own quantity: the mean
    // separates the two classes 9/9 on the usable strike sites, the median
    // only 8/9.
    {
        float sum = 0.0f;
        for (uint8_t i = 0; i < n_taps; i++) sum += f_modeband[i];
        out.v[DISC_MODE_BAND] = n_taps ? (sum / (float)n_taps) : 0.0f;
    }
    out.ok[DISC_MODE_BAND] = true;

    // ── Optical channel (skin colour) ──
    float hue_sum = 0.0f;
    for (int i = 0; i < 8; i++) hue_sum += vision.hue_histogram[i];
    if (hue_sum > 0.5f) {
        float green = 0.0f, warm = 0.0f, wsum = 0.0f, hsum = 0.0f, ent = 0.0f;
        for (int i = 0; i < 8; i++) {
            const float w = vision.hue_histogram[i];
            const float centre = HUE_WINDOW_MIN +
                (HUE_WINDOW_MAX - HUE_WINDOW_MIN) * ((float)i + 0.5f) /
                (float)HUE_BIN_COUNT;
            hsum += w * centre;
            wsum += w;
            if (i >= p.green_bin_start && i < p.green_bin_end) green += w;
            if (i < p.green_bin_start) warm += w;
            const float pi = w / hue_sum;
            if (pi > 1e-5f) ent -= pi * logf(pi);
        }
        out.v[DISC_GREEN] = green;   out.ok[DISC_GREEN] = true;
        out.v[DISC_WARM]  = warm;    out.ok[DISC_WARM] = true;
        if (wsum > 1e-6f) {
            out.v[DISC_HUE_MEAN] = hsum / wsum;  out.ok[DISC_HUE_MEAN] = true;
        }
        out.v[DISC_HUE_ENT] = ent;  out.ok[DISC_HUE_ENT] = true;
        out.v[DISC_DISPERSION] = vision.chromatic_dispersion;
        out.ok[DISC_DISPERSION] = true;
    }

    // ── Black-spot channel (foveal-box vision path only) ──
    if (vision.dark_spot_valid) {
        out.v[DISC_DARK_SPOT] = vision.dark_spot_fraction;
        out.ok[DISC_DARK_SPOT] = true;
        out.v[DISC_DARK_MASS] = vision.dark_mass;
        out.ok[DISC_DARK_MASS] = true;
    }
}

// Weighted band membership for one class. A slot whose feature could NOT be
// measured contributes 0 (no evidence) but still counts in the denominator —
// so a class that needs a channel we do not have is penalised instead of
// winning by default. That is what makes the black-spot discriminator safe:
// without a camera the ROTTEN class cannot win.
float rule_class_score(const FruitRuleParams& rp, const RuleFeatureSet& fs,
                       uint8_t cls) {
    float num = 0.0f, den = 0.0f;
    for (uint8_t i = 0; i < FRM_MAX_SLOTS; i++) {
        if (!((rp.class_slot_mask[cls] >> i) & 1U)) continue;
        const float w = rp.slot_weight[i];
        den += w;
        if (w <= 0.0f) continue;
        const uint8_t fid = rp.slot_feature[i];
        if (fid >= DISC_COUNT || !fs.ok[fid]) continue;   // no evidence -> 0
        float lo = rp.class_lo[cls][i];
        float hi = rp.class_hi[cls][i];
        float soft = rp.class_soft[cls][i];
        if (hi < lo) { const float t = lo; lo = hi; hi = t; }
        if (soft <= 0.0f) soft = 1e-3f;
        const float x = fs.v[fid];
        float d = 0.0f;
        if (x < lo) d = lo - x; else if (x > hi) d = x - hi;
        const float m = (d <= 0.0f) ? 1.0f : expf(-0.5f * (d / soft) * (d / soft));
        num += w * m;
    }
    return (den > 0.0f) ? (num / den) : 0.0f;
}

// The class score with a per-site anchor: the site's edge replaces the
// global band edge for the two classes that read the mode band.  The site is
// not declared by the app: the device evaluates every row and keeps the one
// that matches, because "if the taps are identical it does not matter which
// site it is, and if they differ, that is a different site".
float rule_class_score_site(const FruitRuleParams& rp, const RuleFeatureSet& fs,
                            uint8_t cls, float anchor, float softness) {
    if (rp.side_count == 0) return rule_class_score(rp, fs, cls);
    // Only the classes that read the mode band carry a per-site edge.
    for (uint8_t i = 0; i < FRM_MAX_SLOTS; i++) {
        if (!((rp.class_slot_mask[cls] >> i) & 1U)) continue;
        if (rp.slot_feature[i] != DISC_MODE_BAND) continue;
        float num = 0.0f, den = 0.0f;
        for (uint8_t j = 0; j < FRM_MAX_SLOTS; j++) {
            if (!((rp.class_slot_mask[cls] >> j) & 1U)) continue;
            const float w = rp.slot_weight[j];
            den += w;
            if (w <= 0.0f) continue;
            const uint8_t fid = rp.slot_feature[j];
            if (fid >= DISC_COUNT || !fs.ok[fid]) continue;
            float lo = rp.class_lo[cls][j];
            float hi = rp.class_hi[cls][j];
            float soft = rp.class_soft[cls][j];
            if (j == i) {
                // this slot's edge comes from the site
                lo = (cls == 0) ? 0.0f : anchor;
                hi = (cls == 0) ? anchor : 1.0f;
                soft = softness;
            }
            if (hi < lo) { const float t = lo; lo = hi; hi = t; }
            if (soft <= 0.0f) soft = 1e-3f;
            const float x = fs.v[fid];
            float d = 0.0f;
            if (x < lo) d = lo - x; else if (x > hi) d = x - hi;
            const float m = (d <= 0.0f) ? 1.0f : expf(-0.5f * (d / soft) * (d / soft));
            num += w * m;
        }
        return (den > 0.0f) ? (num / den) : 0.0f;
    }
    return rule_class_score(rp, fs, cls);
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
    // STFT T1 early-decay cell (contrast-normalized 0..1): the hook coordinate
    // for acoustic_feature_mode == 1. Higher T1 band power at 500..850 Hz in the
    // 14..29 ms slice -> lower ripeness latent (see evaluate_fruit_dual_impl).
    c.stft_t1 = tap.stft_grid[1 * 4 + 2];
    return c;
}

// ─── Params: compiled defaults & wire decode ───────────────────────
DualLatentParams dual_params_default() {
    DualLatentParams p{};
    p.site_id             = FRM_NO_CLASS;   // AUTO
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
    p.acoustic_feature_mode = ACOUSTIC_FEATURE_MODE_DEFAULT;  // 0 = legacy
    p.stft_t1_prior = 0.5f;   // unused in mode 0; neutral midpoint if overridden
    return p;
}

static uint16_t wire_min_peak_adc(const DualModelWire& m) {
    return (uint16_t)m.min_peak_adc_lo | ((uint16_t)m.min_peak_adc_hi << 8);
}

bool dual_wire_valid(const DualModelWire& m) {
    // Magic + wire version: anything else is a legacy 32D/corrupt blob.
    if (m.magic != DUAL_MODEL_MAGIC) return false;
    // v1 = classic single-latent blob, v2 = fruit-specific rule block.
    if (m.wire_version != DUAL_MODEL_WIRE_VERSION &&
        m.wire_version != DUAL_MODEL_WIRE_VERSION_RULES)
        return false;

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

    // Reserved regions must be canonical (all zero). The STFT-acoustic hook
    // bytes (acoustic_feature_mode @116 and stft_t1_prior @120..123) are now
    // ACTIVE fields, so they are excluded from the zero-check below and
    // validated explicitly; 115 / 117..119 stay reserved.
    for (size_t i = 0; i < sizeof(m.reserved); i++)
        if (m.reserved[i] != 0) return false;
    if (m.reserved2_pad != 0) return false;
    if (m.reserved2_tail0 != 0) return false;
    for (size_t i = 0; i < sizeof(m.reserved2_tail); i++)
        if (m.reserved2_tail[i] != 0) return false;

    // The 728 bytes at offset 124 are the v2 RULE BLOCK. A v1 blob must
    // carry it zeroed (canonical); a v2 blob must carry a structurally
    // valid block — that is where the fruit-specific discriminators live.
    if (m.wire_version == DUAL_MODEL_WIRE_VERSION_RULES) {
        if (!fruit_rule_block_valid(m.rules)) return false;
    } else {
        if (!fruit_rule_block_empty(m.rules)) return false;
    }

    // Acoustic feature hook: mode must be known; prior finite.
    if (m.acoustic_feature_mode != ACOUSTIC_FEATURE_MODE_LEGACY &&
        m.acoustic_feature_mode != ACOUSTIC_FEATURE_MODE_STFT)
        return false;
    if (!std::isfinite(m.stft_t1_prior)) return false;

    // min_peak_adc: 0 = unspecified (falls back to default), otherwise a valid
    // 12-bit ADC amplitude floor.
    const uint16_t mp = wire_min_peak_adc(m);
    if (mp != 0 && mp > (uint16_t)PIEZO_ADC_FULL_SCALE) return false;

    // Floating parameters finite and within the ranges the builder emits.
    if (!std::isfinite(m.r0_spec) || !std::isfinite(m.r0_mom)) return false;
    if (!std::isfinite(m.alpha) || m.alpha == 0.0f) return false;  // sign flips q direction
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
    // AUTO by default: the device picks the site whose anchor the feature
    // lands nearest to.  The app overrides it with the site the operator
    // says they tapped.
    p.site_id = FRM_NO_CLASS;
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
    p.acoustic_feature_mode = m.acoustic_feature_mode;
    p.stft_t1_prior = m.stft_t1_prior;
    // v2 blobs decide with the fruit-specific rule block; v1 keeps the
    // classic single-latent path (rules stay disabled).
    p.use_rules = (m.wire_version >= DUAL_MODEL_WIRE_VERSION_RULES);
    if (p.use_rules) p.rules = fruit_rule_decode(m.rules);
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

    // ── v2 blobs decide with the fruit-specific rule engine ──
    if (p.use_rules) {
        return evaluate_fruit_rules(p.rules, p, taps, n_taps, vision);
    }

    float vol_norm = dual_volume_norm(p, vision.volume_cm3);

    // ── Stage 1/2: per-tap real-manifold coordinates ──
    float r_spec[MAX_DUAL_TAPS];
    float r_mom[MAX_DUAL_TAPS];
    float stft_t1[MAX_DUAL_TAPS];
    for (uint8_t i = 0; i < n_taps; i++) {
        DualTapCoords c = dual_tap_coords(p, taps[i], vol_norm);
        r_spec[i] = c.r_spec;
        r_mom[i] = c.r_mom;
        stft_t1[i] = c.stft_t1;
    }

    // ── Stage 3: order-invariant median aggregation ──
    float r_spec_fruit = median_n(r_spec, n_taps);
    float r_mom_fruit  = median_n(r_mom, n_taps);

    // ── Stage 4: continuous physical manifold (sigmoid q) ──
    float q_spec = sigmoid(p.alpha * (r_spec_fruit - p.r0_spec));
    float q_mom  = sigmoid(p.alpha * (r_mom_fruit  - p.r0_mom));
    float q_int  = 0.5f * (q_spec + q_mom);

    // ── Stage 4a: optional STFT-enhanced acoustic branch (opt-in only) ──
    // acoustic_feature_mode == 1 blends the prominent early-decay STFT T1 cell
    // (tap.stft_grid[1*4+2], contrast-normalized 0..1) into the acoustic latent.
    // Higher T1 band power at 500..850 Hz in the 14..29 ms slice correlates with
    // LESS ripeness (the "ripe t=1 band collapse" tell), so q_t1 rises as T1
    // power falls below the prior. q_spec/q_mom are unchanged; mode 0 (default)
    // leaves q_int byte-for-byte identical to the pre-hook engine.
    float t1_fruit = 0.0f;
    if (p.acoustic_feature_mode == ACOUSTIC_FEATURE_MODE_STFT) {
        t1_fruit = median_n(stft_t1, n_taps);
        float q_t1 = sigmoid(p.alpha * (p.stft_t1_prior - t1_fruit));
        q_int = 0.5f * q_int + 0.5f * q_t1;
    }

    // ── Stage 4b: optics channel (skin-color ripeness) ──
    float q_surf = surface_q(p, vision);

    // Optics are only meaningful when a real capture produced the hue
    // histogram (normalized, sum ~1). With the camera skipped (acoustics-only
    // debug), an all-zero histogram makes q_surf read 1.0, so the engine falls
    // back to the acoustic latent and never lets phantom optics drive the
    // decision or the contradiction guard.
    float hue_sum = 0.0f;
    for (int i = 0; i < 8; i++) hue_sum += vision.hue_histogram[i];
    const bool optic_ok = hue_sum > 0.5f;

    // ── Stage 4c: modality switch combine ──
    float q;
    switch (p.modality) {
        case MODALITY_ACOUSTIC: q = q_int; break;
        case MODALITY_OPTICS:   q = optic_ok ? q_surf : q_int; break;
        default: q = optic_ok ? (p.w_acoustic * q_int + p.w_optics * q_surf)
                              : q_int;
                  break;
    }

    // ── Stage 4d: 2D multimodal contradiction guard (mask-gated) ──
    // A fruit that reads ripe on the SKIN (q_surf) yet unripe on the INTERIOR
    // (q_int) contradicts genuine radial ripening (skin leads, flesh follows)
    // and is the signature of ethylene/chemical artificial ripening. The guard
    // is gated behind mask bit 4 so only models that OPT IN to recognizing
    // ARTIFICIALLY_RIPENED see it — shipped blobs use mask 0x03 (bit 4 unset)
    // and follow the exact validated decision path.
    if (p.modality == MODALITY_DUAL &&
        optic_ok &&
        p.active_class_mask & (1u << 4) &&
        q_surf >= 0.65f && q_int <= 0.40f && (q_surf - q_int) >= 0.35f) {
        BiologicalStatus status;
        memset(&status, 0, sizeof(status));
        float contradiction = clamp_f((q_surf - q_int) * 100.0f, 80.0f, 99.0f);
        status.probabilities[4] = contradiction / 100.0f;
        status.primary_decision = CLASS_LABELS[4];
        status.ripeness_index   = p.w_optics * q_surf + p.w_acoustic * q_int;
        status.transition_entropy = 0.10f;
        status.confidence       = contradiction;
        status.is_anomaly       = true;
        Serial.printf("[Dual] contradiction: q_surf=%.3f vs q_int=%.3f "
                      "(delta=%.3f) -> %s (%.1f%%) src=%s mask=0x%02X "
                      "modality=%s\n",
                      q_surf, q_int, q_surf - q_int, CLASS_LABELS[4],
                      (double)contradiction, src, p.active_class_mask,
                      modality_name(p.modality));
        return status;
    }

    // ── Stage 5: canonical masked 1D metric decision ──
    BiologicalStatus result = decide_latent(p, q);

    if (p.acoustic_feature_mode == ACOUSTIC_FEATURE_MODE_STFT)
        Serial.printf("[Dual] modality=%s N=%u r_spec=%.4f r_mom=%.4f "
                      "stft_t1=%.3f(prior=%.3f) q_spec=%.3f q_mom=%.3f "
                      "q_int=%.3f q_surf=%.3f q=%.3f -> %s (%.1f%%) src=%s "
                      "mask=0x%02X feat=STFT\n",
                      modality_name(p.modality), n_taps,
                      r_spec_fruit, r_mom_fruit, t1_fruit, p.stft_t1_prior,
                      q_spec, q_mom, q_int, q_surf, q,
                      result.primary_decision, (double)result.confidence,
                      src, p.active_class_mask);
    else
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

// ─── v2 fruit-specific discriminator rules (the decision path for
//     wire_version == 2 blobs) ───────────────────────────────────────
BiologicalStatus evaluate_fruit_rules(
    const FruitRuleParams& rp,
    const DualLatentParams& p,
    const AcousticFeatures taps[MAX_DUAL_TAPS],
    uint8_t n_taps,
    const ColorFeatures& vision)
{
    BiologicalStatus result;
    memset(&result, 0, sizeof(result));

    if (n_taps == 0) n_taps = 1;
    if (n_taps > MAX_DUAL_TAPS) n_taps = MAX_DUAL_TAPS;

    RuleFeatureSet fs;
    rule_feature_values(p, taps, n_taps, vision, fs);

    const uint8_t mask = p.active_class_mask;

    // ── Class scores from the handcrafted bands ──
    // With a per-site table the device tries every site's anchor and keeps
    // the one whose bands match best; without one it uses the global bands.
    float score[NUM_CLASSES];
    int chosen_side = -1;
    if (rp.side_count > 0) {
        // The site whose anchor the feature sits CLOSEST to.  The sites'
        // score ranges overlap, so the feature cannot IDENTIFY the site -
        // the anchors are within ~15 % of each other and picking the site
        // with the biggest separation would always pick the same one.  The
        // nearest anchor is the best available guess, and the app can do
        // better by uploading the blob for the site it knows.
        float best_dist = -1.0f;
        for (uint8_t side = 0; side < rp.side_count; side++) {
            // A declared site wins outright: the app told us which surface
            // was tapped, so its anchor is the one that applies.
            if (p.site_id != FRM_NO_CLASS && rp.side_id[side] == p.site_id) {
                chosen_side = (int)side;
                for (int c = 0; c < NUM_CLASSES; c++) {
                    score[c] = ((mask >> c) & 1U)
                        ? rule_class_score_site(rp, fs, (uint8_t)c, rp.side_anchor[side],
                                                rp.side_soft[side])
                        : 0.0f;
                }
                break;
            }
            float s[NUM_CLASSES];
            for (int c = 0; c < NUM_CLASSES; c++) {
                s[c] = ((mask >> c) & 1U)
                    ? rule_class_score_site(rp, fs, (uint8_t)c, rp.side_anchor[side],
                                            rp.side_soft[side])
                    : 0.0f;
            }
            float dist = fabsf(fs.v[DISC_MODE_BAND] - rp.side_anchor[side]);
            if (best_dist < 0.0f || dist < best_dist) {
                best_dist = dist;
                chosen_side = (int)side;
                for (int c = 0; c < NUM_CLASSES; c++) score[c] = s[c];
            }
        }
    } else {
        for (int c = 0; c < NUM_CLASSES; c++) {
            score[c] = ((mask >> c) & 1U) ? rule_class_score(rp, fs, (uint8_t)c) : 0.0f;
        }
    }

    // ── Winner / runner-up, then the "too close to call" fallback ──
    int best = -1, second = -1;
    for (int c = 0; c < NUM_CLASSES; c++) {
        if (!((mask >> c) & 1U)) continue;
        if (best < 0 || score[c] > score[best]) {
            second = best; best = c;
        } else if (second < 0 || score[c] > score[second]) {
            second = c;
        }
    }
    if (best < 0) { best = 0; second = -1; }

    bool defaulted = false;
    if (rp.default_class < NUM_CLASSES && ((mask >> rp.default_class) & 1U)) {
        const float gap = (second >= 0) ? (score[best] - score[second]) : score[best];
        if (gap < rp.margin) {
            score[rp.default_class] = score[best] + 1e-3f;
            best = rp.default_class;
            defaulted = true;
        }
    }

    // ── Posterior over the enabled classes ──
    float logits[NUM_CLASSES];
    float running_max = -1e30f;
    for (int c = 0; c < NUM_CLASSES; c++) {
        if (!((mask >> c) & 1U)) { logits[c] = -100000.0f; continue; }
        logits[c] = score[c] / rp.temperature;
        if (logits[c] > running_max) running_max = logits[c];
    }
    float sum_exp = 0.0f;
    for (int c = 0; c < NUM_CLASSES; c++) {
        result.probabilities[c] =
            (running_max > -1e29f) ? expf(logits[c] - running_max) : 0.0f;
        sum_exp += result.probabilities[c];
    }
    const float inv = 1.0f / (sum_exp + 1e-15f);
    for (int c = 0; c < NUM_CLASSES; c++) result.probabilities[c] *= inv;

    // ── Hard override: two handcrafted conditions that win outright ──
    bool overridden = false;
    if (rp.override_class < NUM_CLASSES) {
        const uint8_t f1 = rp.override_slot1, f2 = rp.override_slot2;
        bool c1 = false, c2 = false;
        if (f1 < DISC_COUNT && fs.ok[f1])
            c1 = (rp.override_op1 == DISC_OP_GT) ? (fs.v[f1] > rp.override_val1)
                                                 : (fs.v[f1] < rp.override_val1);
        if (f2 < DISC_COUNT && fs.ok[f2])
            c2 = (rp.override_op2 == DISC_OP_GT) ? (fs.v[f2] > rp.override_val2)
                                                 : (fs.v[f2] < rp.override_val2);
        if (c1 && c2) {
            overridden = true;
            best = rp.override_class;
            float others = 0.0f;
            for (int c = 0; c < NUM_CLASSES; c++)
                if (c != best && ((mask >> c) & 1U)) others += 1.0f;
            for (int c = 0; c < NUM_CLASSES; c++) result.probabilities[c] = 0.0f;
            result.probabilities[best] = 0.90f;
            if (others > 0.0f) {
                for (int c = 0; c < NUM_CLASSES; c++)
                    if (c != best && ((mask >> c) & 1U))
                        result.probabilities[c] = 0.10f / others;
            }
        }
    }

    // ── Ripeness index: the posterior projected onto the ripeness axis ──
    const float gain = rp.ripeness_gain > 0.0f ? rp.ripeness_gain : 1.0f;
    float ripeness = 0.0f;
    for (int c = 0; c < NUM_CLASSES; c++) {
        const float rank = clamp_f(0.5f + gain * (RULE_CLASS_RANK[c] - 0.5f),
                                   0.0f, 1.0f);
        ripeness += result.probabilities[c] * rank;
    }
    result.ripeness_index = clamp_f(ripeness, 0.0f, 1.0f);

    result.primary_decision = CLASS_LABELS[best];
    result.confidence = clamp_f(result.probabilities[best] * 100.0f, 0.0f, 100.0f);

    float H = 0.0f;
    for (int c = 0; c < NUM_CLASSES; c++) {
        const float p_i = result.probabilities[c];
        if (p_i > 1e-5f) H -= p_i * logf(p_i);
    }
    result.transition_entropy = H;

    // Rotten/hollow and artificially-ripened are biological anomalies. The
    // black-spot discriminator lands here: a model that gives the ROTTEN
    // class a band on DISC_DARK_SPOT flips the decision into the anomaly
    // class as soon as spots are seen.
    result.is_anomaly = ((best == 3 || best == 4) &&
                          (overridden || score[best] >= rp.anomaly_threshold));

    Serial.printf("[Rule] N=%u score=[%.3f %.3f %.3f %.3f %.3f] "
                  "spot=%.3f dark=%.3f -> %s (%.1f%%) ri=%.3f%s%s mask=0x%02X\n",
                  n_taps, score[0], score[1], score[2], score[3], score[4],
                  fs.ok[DISC_DARK_SPOT] ? fs.v[DISC_DARK_SPOT] : -1.0f,
                  fs.ok[DISC_DARK_MASS] ? fs.v[DISC_DARK_MASS] : -1.0f,
                  result.primary_decision, (double)result.confidence,
                  result.ripeness_index,
                  overridden ? " [override]" : "",
                  defaulted ? " [default]" : "",
                  mask);

    return result;
}
