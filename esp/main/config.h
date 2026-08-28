// ─────────────────────────────────────────────────────────────────
//  config.h — every adjustment variable in one place.
//  Pins, thresholds, timings, gains. If you tune the device, do it here.
// ─────────────────────────────────────────────────────────────────
#pragma once

#include <Arduino.h>
#include <driver/adc.h>

// ─── Buttons ──────────────────────────────────────────────────────
constexpr uint8_t BOOT_BUTTON_PIN   = 0;   // strapping pin, active-low (INPUT_PULLUP)
constexpr uint8_t SCAN_BUTTON_PIN   = 14;  // active-high, wired to 5V (prefer 3.3V!)
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
#define N_FFT_BINS          15       // 15 band slots in the 28-D state vector
#define F2_NORM             441000.0f// frequency² conditioning normalizer
#define EPS_LOG             1e-10f   // log() guard
#define FFT_CLAMP_MIN       -10.0f   // conditioned-bin floor
#define PRE_TRIGGER_SAMPLES 64       // pre-tap samples kept in each window
#define RING_BUFFER_SIZE    1024     // circular buffer (2× FFT_SIZE)
#define PIEZO_FLUSH_SAMPLES 256      // ~29 ms discarded after arm(): stale ring bleed-off

constexpr uint32_t ARM_SETTLE_MS  = 800;    // ignore triggers this long after arming
                                     // (button/placement/LCD bursts decay)
constexpr uint32_t ARM_TIMEOUT_MS = 5000;   // auto-disarm when no tap arrives
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
constexpr float VOLUME_DEFAULT_CM3 = 50.0f;  // fallback when bbox fails

// ─── Multispectral scan ───────────────────────────────────────────
constexpr int    MS_DECODE_SCALE        = 2;    // TJpgDec 1/4: SXGA → 320×256
constexpr int    MS_FIXED_GAIN          = 5;    // locked sensor AGC gain
constexpr int    MS_FIXED_AEC           = 100;  // locked AEC (calibrated on white card)
constexpr int    MS_SYNC_DISCARD_FRAMES = 2;    // frames dropped after LED change so the
                                             // kept frame's exposure happens under it
constexpr uint16_t MS_MAX_W            = 320;  // decoded buffer size
constexpr uint16_t MS_MAX_H            = 256;
constexpr float  MS_CM_PER_PIXEL_FULL  = 0.05f; // cm/px at native resolution
constexpr float  MS_CH_GAIN_R           = 1.16f;// white-card per-channel balance
constexpr float  MS_CH_GAIN_G           = 0.90f;
constexpr float  MS_CH_GAIN_B           = 0.58f;
constexpr bool   MS_AMBIENT_DEFAULT     = true; // subtract no-flash baseline

// ─── Guide flow timings ───────────────────────────────────────────
constexpr uint32_t PLACE_FRUIT_HOLD_MS   = 800;   // "hold fruit up" beat before scan
constexpr uint32_t PLACEMENT_TIMEOUT_MS  = 30000; // wait for SCAN after cam scan
constexpr uint32_t RESULT_HOLD_MS        = 5000;  // result cue on LED/LCD before idle
constexpr uint32_t CUE_HOLD_MS           = 1400;  // short cues (BLE connect, disarmed...)
constexpr uint32_t LCD_FRAME_MS          = 4;     // min gap between LED frame writes
constexpr uint32_t LCD_ANIM_MS           = 180;   // LCD animation frame interval

// ─── Classifier ───────────────────────────────────────────────────
// Force-invariant acoustics: subtract 2*ln(amp)*f^2/NORM from each band so
// tap strength cancels (power was ~amp^2); dim 26 then carries no class
// signal and is zeroed. Prototypes/models MUST be built with the same flag
// (extract_28d.py / rules_to_model.py mirror it).
constexpr bool  ACOUSTIC_FORCE_INVARIANT = true;
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

// ─── Runtime-tunable vision gates (set over BLE via "vision_config") ───
// Defaults come from the constexpr originals below.
extern float g_visionValueMin;
extern float g_visionSatMin;
