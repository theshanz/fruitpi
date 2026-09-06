// ─────────────────────────────────────────────────────────────────
//  config.h — every adjustment variable in one place.
//  Pins, thresholds, timings, gains. If you tune the device, do it here.
// ─────────────────────────────────────────────────────────────────
#pragma once

#include <Arduino.h>
#include <driver/adc.h>

// ─── Buttons ──────────────────────────────────────────────────────
// The physical START (inference) and SCAN buttons are merged onto ONE pin
// (GPIO14, active-high, INPUT_PULLDOWN): tap = inference/scan, hold = open
// the model selector. There is no button on GPIO0 in the current wiring.
constexpr uint8_t BOOT_BUTTON_PIN   = 14;  // START, active-high (INPUT_PULLDOWN)
constexpr uint8_t SCAN_BUTTON_PIN   = 14;  // SCAN — same physical button as START
constexpr uint8_t CANCEL_BUTTON_PIN = 21;  // active-high, wired to 5V (prefer 3.3V!)
                                     // WARNING: GPIO12 = CAM_PIN_Y6 — reads permanently
                                     // pressed if wired. Never use it for a button.
constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;
constexpr uint32_t MENU_HOLD_MS       = 800;   // hold-to-commit / hold-to-reset

// ─── LCD (PCF8574 I2C backpack) ───────────────────────────────────
constexpr uint8_t  LCD_SDA_PIN = 41;  // NOT GPIO1: adjacent to piezo ADC (coupling)
constexpr uint8_t  LCD_SCL_PIN = 38;
constexpr uint32_t LCD_I2C_HZ  = 50000;

// ─── Piezo acoustic sampler ───────────────────────────────────────
constexpr adc1_channel_t PIEZO_ADC_CHANNEL = ADC1_CHANNEL_1; // GPIO2 on ESP32-S3.
                                     // Keep away from GPIO1/LCD-SDA (ADC coupling).

#define FFT_SIZE            512      // must be 2^N
#define SAMPLING_FREQ_HZ    8820     // Nyquist 4410 Hz
#define N_FFT_BINS          15       // legacy bins (deprecated; see STFT grid)
#define F2_NORM             441000.0f// frequency² conditioning normalizer
#define EPS_LOG             1e-10f   // log() guard
#define FFT_CLAMP_MIN       -10.0f   // conditioned-bin floor
#define PRE_TRIGGER_SAMPLES 64       // pre-tap samples kept in each window
#define RING_BUFFER_SIZE    1024     // circular buffer (2× FFT_SIZE)
#define PIEZO_FLUSH_SAMPLES 256      // ~29 ms discarded after arm(): stale ring bleed-off

// ─── Dual-Latent acoustic image (STFT) ────────────────────────────
// Peak-aligned 384-sample window, n_fft=128, hop=32 -> 9 frames, pooled
// into a 4x4 time-frequency grid. Bands match the validated engine.
#define STFT_FFT_N        128
#define STFT_HOP          32
#define STFT_ALIGN_PRE    16          // samples before peak in window
#define STFT_WINDOW       384         // aligned window length
#define STFT_FRAMES       9           // (384-128)/32 + 1
#define STFT_FREQ_BANDS   4           // 4 frequency bands per time slice
#define STFT_TIME_BINS    4           // pooled time slices
constexpr float STFT_BAND_HZ[STFT_FREQ_BANDS][2] = {
    {100.0f, 250.0f}, {250.0f, 500.0f},
    {500.0f, 850.0f}, {850.0f, 1400.0f}
};
constexpr float STFT_CONTRAST_DB = 1.5f;  // dynamic-range floor (dB)

// Bio-moment extraction (dual moment channel r_mom inputs)
constexpr float TAIL_WINDOW_S   = 0.020f; // late-tail region after this much time
constexpr int   RMS_ENVELOPE_N  = 16;     // RMS envelope window
constexpr float DECAY_NOISE_GATE = 0.05f; // decay fit above this level
constexpr float DAMPING_SCALE    = 10.0f; // log-slope -> dim 31 scale
constexpr float HARMONIC_CLIP    = 3.0f;  // harmonic/fortitude clip then /3

constexpr uint32_t ARM_SETTLE_MS  = 800;    // ignore triggers this long after arming
                                     // (button/placement/LCD bursts decay)
constexpr uint32_t ARM_TIMEOUT_MS = 15000;  // auto-disarm when no tap arrives;
                                     // 15 s per tap window so a guided human
                                     // taper can react (5 s was too tight)
constexpr uint32_t ARM_TIMEOUT_US = ARM_TIMEOUT_MS * 1000UL;

// BLE data-collection arming uses a two-stage ready confirmation that mirrors
// the inference flow: `arm_acoustic` asks the user to PLACE the fruit (piezo
// stays DISARMED), then `arm_ready` confirms it sits there before listening.
// READY_WAIT_MS bounds how long we wait for that confirmation before giving
// up (timeout_disarmed) so the device never hangs armed.
constexpr uint32_t READY_WAIT_MS = 30000;

// Data-collection arming runs a visible placement countdown first: the piezo
// is kept DISARMED while the user puts the fruit on it, so the placement
// itself can never register as a tap. When the countdown ends the sampler
// arms and the screen flips to "TAP NOW".
constexpr uint32_t PLACE_GRACE_MS = 3000;

constexpr float PIEZO_DEFAULT_THRESHOLD = 0.02f; // min |x−baseline| deviation;
                                     // real fruit taps measure ~0.02–0.07.
                                     // BLE set_threshold overrides at runtime.
constexpr float MIN_TAP_PEAK = 0.015f;           // windows below this impact amplitude
                                 // are electrical noise, silently re-armed.

// peak_abs is stored normalized to 0..1 after `raw_adc / 4095.0f`; the dual
// β·ln(peak) force-de-coater was calibrated on RAW 12-bit counts (peaks ~2000
// in the dataset), so rescale before the log or the ln(peak) sign flips.
constexpr float PIEZO_ADC_FULL_SCALE = 4095.0f;

// Minimum gap between accepted taps: re-arm suppresses the ringing residue of
// the previous impact so one strike can't register as several taps. 500 ms
// guarantees each physical impact is a distinct tap for the N-tap consensus.
constexpr uint32_t TAP_MIN_INTERVAL_MS = 500;

// Trigger gates: require sharp slew AND energy above baseline. Both adapt to
// measured ambient noise (gate = mult × noise-EMA, floored, capped).
constexpr float TRIGGER_DELTA_MIN    = 0.015f;
constexpr float SLEW_GATE_MULT       = 4.0f;
constexpr float SLEW_NOISE_EMA_INIT  = 0.004f;
constexpr float SLEW_NOISE_ALPHA     = 0.002f;
constexpr float SLEW_GATE_MAX        = 0.08f;
constexpr float DEV_GATE_MULT        = 4.0f;
constexpr float DEV_NOISE_EMA_INIT   = 0.005f;
constexpr float DEV_NOISE_ALPHA      = 0.002f;
constexpr float DEV_GATE_MAX         = 0.08f;
constexpr float BASELINE_ALPHA_LISTEN = 0.005f; // τ≈23 ms while listening
constexpr float BASELINE_ALPHA_SETTLE = 0.05f;  // fast re-track while settling

// ─── Vision (HueExtractor) ────────────────────────────────────────
constexpr float VISION_VALUE_MIN   = 0.15f;  // skip dark shadows below this V
constexpr float VISION_SAT_MIN     = 0.15f;  // skip washed-out glare below this S
constexpr float HUE_WINDOW_MIN     = 20.0f;  // hue histogram window [deg]
constexpr float HUE_WINDOW_MAX     = 120.0f;
constexpr int   HUE_BIN_COUNT      = 8;      // bins across the window
constexpr float CM_PER_PIXEL_FULL  = 0.05f;  // calibration at native resolution
constexpr float VOLUME_CM3_MIN     = 10.0f;  // sanity clamp for estimated volume
constexpr float VOLUME_CM3_MAX     = 500.0f;
constexpr float VOLUME_DEFAULT_CM3 = 350.0f; // fallback when bbox criteria fail

// Frame-overflow edge bits (the "fits frame?" signal from fit_fruit_ellipse):
// a fruit hugging an edge sets that bit. A blob touching BOTH Top+Bottom (or
// BOTH Left+Right) edges is wall/table glare sweeping the whole sensor, not a
// fruit — matches final_pipeline._OVFL_* / ms_tool (single source of truth).
constexpr uint8_t OVERFLOW_LEFT   = 0x01;
constexpr uint8_t OVERFLOW_RIGHT  = 0x02;
constexpr uint8_t OVERFLOW_TOP    = 0x04;
constexpr uint8_t OVERFLOW_BOTTOM = 0x08;

// ─── Multispectral scan ───────────────────────────────────────────
constexpr int    MS_DECODE_SCALE        = 2;    // TJpgDec 1/4: SXGA → 320×256
constexpr int    MS_FIXED_GAIN          = 5;    // locked sensor AGC gain
constexpr int    MS_FIXED_AEC           = 100;  // locked AEC (calibrated on white card)
constexpr int    MS_SYNC_DISCARD_FRAMES = 1;    // frames dropped after LED change so the
                                             // kept frame's exposure happens under it
                                             // (AEC is locked, so one rolling-shutter
                                             // sync frame is enough)
constexpr uint16_t MS_MAX_W            = 320;  // decoded buffer size
constexpr uint16_t MS_MAX_H            = 256;
constexpr float  MS_CM_PER_PIXEL_FULL  = 0.05f; // cm/px at native resolution
constexpr float  MS_CH_GAIN_R           = 1.16f;// white-card per-channel balance
constexpr float  MS_CH_GAIN_G           = 0.90f;
constexpr float  MS_CH_GAIN_B           = 0.58f;
constexpr bool   MS_AMBIENT_DEFAULT     = true; // subtract no-flash baseline

// White flash settle: 0 — the LED is instantaneous and AEC/AGC are already
// locked to the flash state before the ambient frame, so there is nothing to
// wait for. Exposure convergence is handled by the discard frame(s) above.
constexpr int FLASH_SETTLE_DELAY_MS = 0;

// AWB warm-up (item 1, 2026-09-05): the OV sensor converges white-balance over
// the first frames after wake; locking the sensor too early freezes the
// cold-start green cast into the captured gains. Discard the first ~10 frames
// with AWB still ACTIVE before multispectral_lock_sensor(), so the real
// capture freezes the converged gains. User-specified spec.
constexpr int    MS_WARMUP_FRAMES   = 10;
constexpr uint32_t MS_WARMUP_DELAY_MS = 15;

// White flash intensity (0-255). 255 saturates a glossy fruit at close range
// -> specular glare pixels (S just above the 0.15 floor) leak noisy hue into
// every histogram bin. Keep it modest; AEC/AGC adapt to the rest.
constexpr uint8_t WHITE_FLASH_BRIGHTNESS = 120;

// Flash/no-flash differential segmentation: a pixel belongs to the foreground
// object only if the white flash brightens its luminance clearly above the
// no-flash baseline (flash falloff distance-gates the scene — far background
// receives negligible flash light).
constexpr int   FLASH_DELTA_Y_THRESHOLD = 18;    // min flash-vs-ambient Y delta (0-255)
constexpr int   FLASH_MIN_OBJECT_PIXELS = 400;   // refined-mask foreground px to trust bbox

// Hue-binning color filters (applied to the flash frame inside the mask).
// FRUIT cuts floor V/S to kill low-light dark current (which biases R>G>B ->
// false brown/orange) and washed-out background; the glare gate drops bright
// near-achromatic specular hot spots that would flatten the histogram.
constexpr float FRUIT_MIN_V_CUT = 0.25f;  // low-light noise floor
constexpr float FRUIT_MIN_S_CUT = 0.20f;  // desaturated background
constexpr float GLARE_MIN_S_CUT = 0.15f;  // glare desaturation threshold
constexpr float GLARE_MAX_V_CUT = 0.88f;  // cuts specular hot spots
// Raw (pre-WB) saturation floor for hue-report pixels: washed-out neutral cast
// regions (~S 0.24) otherwise read as green after the B WB-scale; real fruit
// sits at ~S 0.5. Applied to the fused flash BEFORE MS_WB_B_SCALE.
constexpr float HUE_RAW_S_MIN = 0.30f;

// Flash white-balance: NO artificial correction (MS_WB_B_SCALE = 1.0). The
// sensor's on-chip AWB now balances the flash naturally — the AWB warm-up
// (MS_WARMUP_FRAMES) converges it before multispectral_lock_sensor() freezes
// the gains, exactly like the working one-shot/process_frame path, which never
// applies a B-scale and renders natural R≈G≈B on neutrals. The earlier 0.62
// B-reduction was a band-aid for disabling AWB too early and froze a cold-green
// cast; with proper AWB settle it is no longer needed and would otherwise push
// yellow fruit (e.g. banana) toward the green bins.
constexpr float MS_WB_B_SCALE = 1.0f;

// Edge-refined trimap segmentation (replaces the single ΔY gate in
// process_differential): a solid core (ΔY >= EDGE_CORE_DY), a definite
// background (ΔY < EDGE_BG_DY), and an ambiguous rim whose pixels are snapped
// to the sharpest flash ΔY gradient so the silhouette locks to the true
// physical edge instead of clipping rounded rims or bleeding into the table.
constexpr int EDGE_CORE_DY  = 25;   // definite fruit interior (fixed path)
constexpr int EDGE_BG_DY    = 12;   // definite background cutoff (fixed path)
constexpr int EDGE_GRAD_MIN = 8;    // |∇ΔY| above which a rim pixel is admitted

// Centre-prior seeded segmentation (process_differential, applied 2026-09-03).
// Replaces the fixed EDGE_CORE_DY solid core with a centre-connected region
// grown from a central foveal seed box: the fruit is the connected foreground
// component that intersects the frame centre, so dark fruit is no longer
// discarded by a global ΔY gate and peripheral table glare / hand motion are
// pruned by construction. Thresholds adapt to the seed (matches the validated
// host compute_local_report()): ΔY >= max(CENTRE_SEED_DY_MIN, P25(seed ΔY)),
// flash-V >= max(CENTRE_SEED_V_MIN, P10(seed V)).
constexpr float CENTRE_SEED_FRACTION = 0.10f;       // 10% of W/H each side → 20% foveal box
constexpr float CENTRE_SEED_V_MIN    = 0.08f;       // lowest allowed flash-V floor
constexpr int   CENTRE_SEED_DY_MIN   = 14;          // lowest allowed ΔY threshold

// Strict foveal fail-fast (item 2, 2026-09-05): report EMPTY unless the central
// foveal seed box alone yields a confident fruit-like foreground. A blob pinned
// to a frame edge or washed to the sensor border reads 0-140 foveal px across
// 28 validated bundles, while live centred captures sit at 1095-2407 px; 400
// sits ≥2σ from both populations. Mirrors final_pipeline._FOVEAL_FG_MIN.
constexpr uint32_t MS_FOVEAL_FG_MIN = 400;

// Gradient Notch (2026-09-04 research pipeline, tests/final_pipeline.py): Sobel
// gradient magnitude on ΔY, fovea-adaptive cut threshold t_grad = μ_fov + K·σ_fov
// computed from the same central seed box, and an UNCONDITIONAL 0px cut — zero
// every foreground pixel with gmag > t_grad, applied BEFORE connected-component
// selection. A raw 1px high-gradient line is mathematically sufficient to sever
// 4/8-connectivity (a dilated trench would slice through small/distant fruit);
// applying the cut after CCL is a no-op because the selected blob is already one
// component. Unconditional: no statistical gate (removal %, gradient CV) can
// separate when the notch helps vs hurts, and it is correct on all test bundles.
// Safety: if the cut removes >80% of the foreground or leaves fewer than
// FLASH_MIN_OBJECT_PIXELS, the raw foreground is restored.
constexpr float FOVEAL_GRAD_K               = 2.8f;   // t_grad = μ_fov + K·σ_fov
constexpr float GRAD_NOTCH_MAX_REMOVAL_FRAC = 0.80f;  // safety fallback bound

// Centre-bias gaussian σ (px) for largest-blob selection. The chosen component
// maximises  size * exp(-d² / (2σ²))  with d = bbox-midpoint distance to frame
// centre, so a background blob pinned to the frame edge (e.g. a warm-coloured
// chair) is pruned even when it is the largest component; a fruit within ~one σ
// of centre always wins. Raised here to bias "grab the central fruit".
constexpr float MS_CENTRE_BIAS_SIGMA = 40.0f;

// ─── Unified 5-layer burst pipeline (process_unified_burst) ─────────
// Replaces the fixed thresholds with a 98th-percentile dynamic gate so that a
// close fruit reflects a peak ΔY of ~160 while room bounce stays at ΔY~28:
//   τ_core = max(min, 0.35 × p98), τ_bg = max(min, 0.18 × p98)
constexpr float BURST_CORE_FRAC   = 0.35f;
constexpr float BURST_BG_FRAC     = 0.18f;
constexpr int   BURST_CORE_MIN_DY = 28;   // floor on the dynamic core threshold
constexpr int   BURST_BG_MIN_DY   = 14;   // floor on the dynamic background cutoff

// Motion-parallax macroblock estimator (SAD, ±3 px). Divides the frame into a
// 20×15 grid of 16×16 blocks; a block is foreground if its SAD displacement is
// >= PARALLAX_MIN_MOTION. An adaptive global classifier (Q1) treats a
// handheld scene (mean block motion >= PARALLAX_GLOBAL_HANDHELD) as motile and
// enforces the gate; a stationary bench/tripod scene bypasses it so resting
// fruit is never falsely rejected (Layer 2 default: distant/floodlit bg cut).
constexpr int   PARALLAX_BLOCK_SIZE    = 16;
constexpr int   PARALLAX_SEARCH_WIN    = 3;
constexpr float PARALLAX_MIN_MOTION    = 1.0f;  // px, per-block foreground threshold
constexpr float PARALLAX_GLOBAL_HANDHELD = 0.40f; // mean px across blocks -> handheld

// Layer 5 semantic gates for the burst path.
// Glossy table / white-wall reject: if the core's mean saturation is under
// TABLE_MIN_MEAN_S the region is neutral flash glare -> fg=0, bbox=0x0, vol=0.
// 0.25 (up from 0.20): the empty-frame dark-room capture reads mean S=0.215,
// just above the old bar -> whole-frame false positive; all 10 validation
// bundles keep byte-identical fg up to 0.28, so 0.25 rejects that scene with
// margin on both sides.
constexpr float TABLE_MIN_MEAN_S = 0.25f;

// 2D skin gate (Q3): keep the 20° hue window (preserves ripe orange/apricot
// 20–35° pigments) and reject human skin by its hue+saturation band rather
// than raising the hue floor.
constexpr float SKIN_HUE_MIN_DEG = 15.0f;
constexpr float SKIN_HUE_MAX_DEG = 32.0f;
constexpr float SKIN_SAT_MAX     = 0.50f;  // fingers are low/moderate S (≤0.48)

// ─── Guide flow timings ───────────────────────────────────────────
constexpr uint32_t PLACE_FRUIT_HOLD_MS   = 800;   // "hold fruit up" beat before scan
constexpr uint32_t PLACEMENT_TIMEOUT_MS  = 30000; // wait for SCAN after cam scan
constexpr uint32_t RESULT_HOLD_MS        = 5000;  // result cue on LED/LCD before idle
constexpr uint32_t CUE_HOLD_MS           = 1400;  // short cues (BLE connect, disarmed...)
constexpr uint32_t LCD_FRAME_MS          = 4;     // min gap between LED frame writes
constexpr uint32_t LCD_ANIM_MS           = 180;   // LCD animation frame interval

// ─── Classifier ───────────────────────────────────────────────────
// The Dual-Latent modality engine (dual_latent.cpp) is the ONLY scorer in
// this firmware — the 32D Diagonal-Gaussian scorer is removed. The acoustic
// front-end still produces absolute-physical-scale features (amplitude survives
// — the cue that separates ripe/unripe); the dual engine maps per-tap
// (FBM-2D shear ratio, bio-moment contrast) onto a single ripeness latent q
// and decides with a masked softmax on q (anchors, temperature DUAL_TAU).
// (The app still mirrors the legacy layout in rules_model32d.dart — see the
// Dual section below for the on-device truth.)
constexpr bool  ACOUSTIC_FORCE_INVARIANT = false;  // amplitude is a real cue
constexpr float IMPACT_AMP_FLOOR         = 0.004f; // div-by-zero guard for weak taps
// Bit i enables class i: bit0=UNRIPE bit1=PERFECTLY_RIPE bit2=OVERRIPE
// bit3=ROTTEN_OR_HOLLOW bit4=ARTIFICIALLY_RIPENED.
// Fallback ONLY: models built by rules_to_model.py carry their own
// active_class_mask (auto-derived from the labels fed in); a nonzero
// per-model mask always wins over this. 0x03 = two-class test default.
constexpr uint8_t ACTIVE_CLASS_MASK      = 0x03;
constexpr float GREEN_MASS_VETO_THRESHOLD   = 0.25f;
constexpr float ANOMALY_CONFIDENCE_THRESHOLD = 0.35f;
constexpr float MIN_MASS23 = 10.0f;   // volume^(2/3) normalization range
constexpr float MAX_MASS23 = 300.0f;

// ─── Dual-Latent modality engine (replaces 32D scorer in the inference path) ──
// The device decision latent q is a single ripeness axis fed by one of three
// MODALITIES (the sensor channel whose q contributes to the decision):
//   MODALITY_ACOUSTIC : q = q_int = 0.5*q_spec + 0.5*q_mom  (optics disabled,
//                       w_optics = 0.0) — the canonical Dual-Latent acoustic
//                       pipeline validated on ggg (LOMO ~87.7% @ N=5) and the
//                       fingerr zero-shot transfer (2/2 = 100%).
//   MODALITY_OPTICS   : q = q_surf (skin-color ripeness: 1 - green-bin mass).
//   MODALITY_DUAL     : q = w_ac*q_int + w_opt*q_surf  (both channels fused).
//
// Per-tap inputs (computed on-device, NOT from an uploaded blob):
//   q_spec  <- sigmoid(ALPHA * (r_spec - R0_SPEC)), r_spec = median over taps of
//              (FBM-2D E(>1kHz)/E(20..300Hz) shear ratio) - BETA*ln(peak_abs)
//   q_mom   <- sigmoid(ALPHA * (r_mom - R0_MOM)),   r_mom  = median over taps of
//              (spectral_entropy + tail_energy) - (abbott_stiffness + STFT t0 band0)
//   q_int   = 0.5*q_spec + 0.5*q_mom                (50/50 Dual-Latent fusion)
//   q_surf  = 1 - sum(hue bins MS_SURF_GREEN_BINS_START..END)   (green = unripe)
// Decision (canonical 1D metric): masked softmax distance to DUAL_ANCHORS on the
// q axis, temperature DUAL_TAU. Anchors sit on the same ripeness manifold as the
// canonical engine (UNRIPE 0.10, PERFECTLY_RIPE 0.85, OVERRIPE 1.10,
// ROTTEN 1.30, ARTIFICIAL 0.10), and for mask 0x03 the flip point is the
// (0.10+0.85)/2 = 0.475 boundary reproduced by the fingerr experiment.
constexpr int  MS_DUAL_RFFT_N      = 2048;   // zero-padded FFT for the shear ratio
constexpr float DUAL_ALPHA         = 12.0f;  // sigmoid slope (validated sweep constant)
constexpr float DUAL_BETA          = 0.20f;  // force de-coating factor (validated constant)
constexpr float DUAL_TAU           = 0.08f;  // masked-softmax temperature
constexpr float DUAL_R0_SPEC       = -1.0819f; // ggg-fit sigmoid midpoint, spectral
constexpr float DUAL_R0_MOM        = -0.4104f; // ggg-fit sigmoid midpoint, moments
constexpr float DUAL_ANCHORS[5]    = {0.10f, 0.85f, 1.10f, 1.30f, 0.10f};
constexpr uint8_t MODAL_ACTIVE_CLASS_MASK = 0x03;  // UNRIPE vs PERFECTLY_RIPE (test default)
constexpr int   MS_SURF_GREEN_BINS_START = 5; // hue bins 82.5-120 deg = green skin
constexpr int   MS_SURF_GREEN_BINS_END   = 8;
// Vol^(2/3) -> [0,1] normalizer (abbott stiffness term, mirrors Python).
constexpr float DUAL_VOL_CLAMP_LO   = 10.0f;
constexpr float DUAL_VOL_CLAMP_HI   = 600.0f;
constexpr float DUAL_VOL23_LO       = 4.64f;   // 10^(2/3)
constexpr float DUAL_VOL23_HI       = 71.13f;  // 600^(2/3)

// Modality selection. Compile-time for this release: the acoustic test rig is
// MODALITY_ACOUSTIC (optics channel zero-weighted). Weights only apply when
// ACTIVE_MODALITY == MODALITY_DUAL.
enum Modality : uint8_t {
    MODALITY_ACOUSTIC = 0,   // optics disabled (w_optics = 0.0)
    MODALITY_OPTICS   = 1,   // vision only (w_acoustic = 0.0)
    MODALITY_DUAL     = 2    // 0.5*q_int + 0.5*q_surf
};
constexpr Modality ACTIVE_MODALITY        = MODALITY_ACOUSTIC;
constexpr float    MODAL_W_ACOUSTIC       = 0.5f;
constexpr float    MODAL_W_OPTICS         = 0.5f;

// ─── Runtime-tunable vision gates (set over BLE via "vision_config") ───
// Defaults come from the constexpr originals below.
extern float g_visionValueMin;
extern float g_visionSatMin;
