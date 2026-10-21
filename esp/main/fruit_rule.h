#pragma once

#include <cstdint>

// ══════════════════════════════════════════════════════════════════════
// FRUIT-SPECIFIC DISCRIMINATOR RULES — Dual-Latent wire v2 extension
// ══════════════════════════════════════════════════════════════════════
// The v1 DLAT blob carries ONE global ripeness latent q (r0_spec, r0_mom,
// five anchors) that every fruit shares. That is why a banana blob and a
// mango blob differ only by a threshold nudge: the blob cannot say
// "for THIS fruit the discriminators are the shear ratio and the skin
// green mass, and UNRIPE means shear in [0.30,0.55] AND green > 0.40".
//
// Wire v2 keeps the 852-byte size and the 'DLAT' magic (the BLE transfer
// contract and the NVS store are untouched) and spends the previously
// reserved 728 bytes at offset 124 on a RULE BLOCK:
//
//   • up to 8 DISCRIMINATOR SLOTS — each slot names one feature from the
//     catalogue below (an acoustic per-tap median or an optical aggregate)
//     and carries a weight;
//   • per class: a slot mask saying WHICH discriminators that class cares
//     about, plus a band [lo, hi] and a softness sigma per slot. The class
//     score is the weighted mean of its band memberships, so every class
//     score lands in [0,1] and the classes are directly comparable.
//     The bands are handcrafted per fruit — that is the point of the upgrade;
//   • an optional two-condition HARD OVERRIDE (e.g. "warm skin > 0.55 AND
//     shear > 0.45 => ARTIFICIALLY_RIPENED") that wins outright.
//
// v1 blobs (wire_version == 1) keep running the legacy single-latent path
// untouched, so every model already in the store still works.

// ─── Discriminator catalogue ─────────────────────────────────────────
// Every id below is computable on-device TODAY (no new DSP): the acoustic
// ids read the per-tap AcousticFeatures the piezo front-end already
// produces, the optical ids read the 8-bin hue histogram the camera
// pipeline already produces.
enum DiscFeature : uint8_t {
    DISC_SHEAR       = 0,   // median fbm2d_r_high_low (E>1kHz / E 20..300Hz)
    DISC_R_SPEC      = 1,   // median (shear - beta*ln(peak*FS))  [de-coated]
    DISC_R_MOM       = 2,   // median moment contrast (entropy+tail)-(abbott+t0)
    DISC_ENTROPY     = 3,   // median spectral_entropy
    DISC_CENTROID    = 4,   // median bio_moments[0] temporal centroid
    DISC_TAIL        = 5,   // median bio_moments[1] tail energy
    DISC_HARMONIC    = 6,   // median bio_moments[2] harmonic absorption
    DISC_ABBOTT      = 7,   // median bio_moments[3] abbott stiffness
    DISC_BIO_ENTROPY = 8,   // median bio_moments[4] spectral entropy
    DISC_DAMPING     = 9,   // median bio_moments[5] dynamic damping
    DISC_STFT_T1     = 10,  // median stft_t1_decay (early-decay cell)
    DISC_PEAK        = 11,  // median peak_abs (tap strength)
    DISC_GREEN       = 12,  // optics: green mass (bins [green_start,green_end))
    DISC_WARM        = 13,  // optics: warm mass (bins [0,green_start))
    DISC_HUE_MEAN    = 14,  // optics: histogram mean hue (deg)
    DISC_DISPERSION  = 15,  // optics: chromatic_dispersion
    DISC_HUE_ENT     = 16,  // optics: hue-histogram entropy
    DISC_DARK_SPOT   = 17,  // optics: black/brown SPOT area fraction (0..1)
                            //   — the black-spot discriminator. A model that
                            //   wants it puts it in a slot and gives the
                            //   ROTTEN_OR_HOLLOW class a band on it; a spot
                            //   then drives the decision into the anomaly
                            //   classes. Models that do NOT put it in a slot
                            //   are completely unaffected (it is opt-in).
    DISC_DARK_MASS   = 18,  // optics: total dark area fraction (0..1)
    // Acoustic: the share of the spectrum's energy in the species' MODE BAND
    // (config.h: the 16 log cells, cell LOG_MODE_BAND_INDEX).  This is the
    // discriminator that separated a green from a yellow mango on the
    // Mango-capped corpus: the ripe mango's mode sits at ~258 Hz and the
    // unripe one at 138-172 Hz, and the band ENERGY separates them cleanly
    // where the structural peak does not (the peak lands on the 50/100/150 Hz
    // mains comb and moves with the placement).  Appended at the end so every
    // existing catalogue id keeps its value.
    DISC_MODE_BAND   = 19,
    DISC_COUNT       = 20,
};

// Comparison op for the hard-override conditions.
enum DiscOp : uint8_t {
    DISC_OP_GT = 0,   // x >  value
    DISC_OP_LT = 1,   // x <  value
};

constexpr uint8_t FRM_MAX_SLOTS     = 8;
constexpr uint8_t FRM_NUM_CLASSES   = 5;
constexpr uint8_t FRM_NO_CLASS      = 0xFF;
constexpr uint8_t FRM_BLOCK_VERSION = 1;

// ─── The 728-byte rule block (wire offset 124) ──────────────────────
// Packed so the struct IS the binary: the host packs this byte-for-byte.
struct __attribute__((packed)) FruitRuleBlock {
    uint32_t block_version;                      // 124  (== FRM_BLOCK_VERSION)
    uint32_t slot_mask;                          // 128  bit i = slot i is used
    float    temperature;                        // 132  softmax temp on class scores
    float    margin;                             // 136  min winner/runner-up gap
    uint32_t default_class;                      // 140  class when the gap is small
    float    anomaly_threshold;                  // 144  min match ratio for anomaly
    float    ripeness_gain;                      // 148  ripeness contrast gain
    uint8_t  slot_feature[FRM_MAX_SLOTS];        // 152  catalogue id per slot
    uint8_t  class_slot_mask[FRM_NUM_CLASSES];   // 160  bit i = class uses slot i
    uint8_t  pad0[3];                            // 165
    float    slot_weight[FRM_MAX_SLOTS];         // 168  per-discriminator weight
    float    class_lo[FRM_NUM_CLASSES][FRM_MAX_SLOTS];    // 200  band lower bound
    float    class_hi[FRM_NUM_CLASSES][FRM_MAX_SLOTS];    // 360  band upper bound
    float    class_soft[FRM_NUM_CLASSES][FRM_MAX_SLOTS];  // 520  band softness
    uint32_t override_class;                     // 680  0xFF = no override
    uint8_t  override_slot1;                     // 684
    uint8_t  override_op1;                       // 685
    uint8_t  override_slot2;                     // 686
    uint8_t  override_op2;                       // 687
    float    override_val1;                      // 688
    float    override_val2;                      // 692
    uint8_t  reserved[156];                      // 696..851 (must be zero)
};
static_assert(sizeof(FruitRuleBlock) == 728, "FruitRuleBlock must be 728 bytes");

// ─── The per-side anchor table (the 156 reserved bytes) ────────────────
// A fruit's strike SITES do not share one score level: the same mango's
// two cheeks and its edge sit at different mode-band levels, because what
// moves the level is how the fruit is coupled to the piezo (the placement),
// not which fruit it is.  A single band therefore misclassifies the
// sessions that sit near it, so the model carries one anchor per site.
//
// The table lives in the rule block's 156 reserved bytes, which keeps the
// blob at 852 bytes and the wire format otherwise untouched.  The bytes stay
// all-zero when a model has no table, and the firmware then behaves exactly
// as before (legacy blobs are unaffected).
//
// The device does NOT need the site declared: it evaluates every row's
// anchor and takes the one that matches ("if the taps are identical it does
// not matter which site it is; if they differ, that is a different site").
// An app may still declare the site to skip the search.
constexpr uint8_t FRM_SIDE_TABLE_VERSION = 1;
constexpr uint8_t FRM_MAX_SIDES = 14;
constexpr uint8_t FRM_SIDE_FLAG_NONE = 0;

struct __attribute__((packed)) FruitSideEntry {
    uint8_t side_id;    // the site's index (the app's label for it)
    uint8_t flags;      // FRM_SIDE_FLAG_*
    float   anchor;     // the class band edge for this site
    float   softness;   // the band softness for this site
};

struct __attribute__((packed)) FruitSideTable {
    uint32_t table_version;                       // FRM_SIDE_TABLE_VERSION
    uint8_t  side_count;                          // 0 = no table
    uint8_t  reserved[3];
    FruitSideEntry sides[FRM_MAX_SIDES];
    uint8_t  tail[8];                             // the 156-byte run, exactly
};
static_assert(sizeof(FruitSideEntry) == 10, "a side row is 10 bytes packed");
static_assert(sizeof(FruitSideTable) == 156, "the side table must fill the reserved run");

// ─── Decoded, runnable rule parameters ──────────────────────────────
struct FruitRuleParams {
    bool    enabled;                              // wire_version >= 2
    uint8_t slot_mask;
    uint8_t slot_feature[FRM_MAX_SLOTS];
    uint8_t class_slot_mask[FRM_NUM_CLASSES];
    float   slot_weight[FRM_MAX_SLOTS];
    float   class_lo[FRM_NUM_CLASSES][FRM_MAX_SLOTS];
    float   class_hi[FRM_NUM_CLASSES][FRM_MAX_SLOTS];
    float   class_soft[FRM_NUM_CLASSES][FRM_MAX_SLOTS];
    // The per-site anchor table (empty when the model has none).
    uint8_t side_count;
    uint8_t side_id[FRM_MAX_SIDES];
    float   side_anchor[FRM_MAX_SIDES];
    float   side_soft[FRM_MAX_SIDES];
    float   temperature;
    float   margin;
    uint8_t default_class;
    float   anomaly_threshold;
    float   ripeness_gain;
    uint8_t override_class;
    uint8_t override_slot1, override_op1;
    uint8_t override_slot2, override_op2;
    float   override_val1, override_val2;
};

// Structural validation of a v2 rule block (finite floats, sane ranges,
// known feature ids, at least one active slot, canonical padding).
bool fruit_rule_block_valid(const FruitRuleBlock& b);

// True when the whole block is zeroed — the canonical state of the 728
// bytes in a v1 blob (which carries no rules at all).
bool fruit_rule_block_empty(const FruitRuleBlock& b);

// Decodes a validated block into runnable parameters.
FruitRuleParams fruit_rule_decode(const FruitRuleBlock& b);
