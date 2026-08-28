#!/usr/bin/env python3
"""Human rule-space engine for FruitPi — knobs to 28-D model, no GUI needed.

A human describes each ripeness CATEGORY with four physical knobs:

    skin_hue_deg : dominant skin colour as a hue angle inside the camera's
                   20-120 deg window (20 = golden/orange edge, 120 = deep green)
    spread_deg   : how blotchy/mixed the colouring is = width of the hue bump
    firmness     : 0 = hard/unripe ping ... 1 = soft/ripe thud
                   (moves the tap's spectral centre 1000 Hz -> 300 Hz)
    character    : 0 = crisp resonant ping ... 1 = dull mushy thud
                   (widens the spectral bump and raises entropy)

Everything else is derived with the EXACT math the firmware runs
(extract_hues.cpp / sci_28d.cpp / piezo_acoustic.cpp):

    hue_histogram[8]  gaussian bump over bin centres, normalised to sum 1
    chromatic_dispersion  1 - var(hist)/EIGHT_MAX_VAR   (device formula!)
    volume dim        shared neutral value -> range-zero -> provably ignored
    fft_bins[15]      bump in conditioned log-power space centred at f_c
    spectral_entropy  linear in 'character'
    dims 26/27        zeroed by force-invariance (tap strength never matters)

Any dimension kept identical across categories cancels out of every class
score exactly, so 'Size' deliberately has no knob.

Model bridge (identical to rules_to_model.py): nearest-range-normalised-
prototype == argmax of linear score w = 2p/r^2, b = -sum(p^2/r^2), packed as
616-byte .bin with the active-class mask byte at offset 612.
"""
import json
import numpy as np

# ─── Device-exact constants (esp/main/config.h, sci_28d.cpp) ──────────
HUE_WINDOW_MIN, HUE_WINDOW_MAX, HUE_BIN_COUNT = 20.0, 120.0, 8
HUE_BIN_WIDTH = (HUE_WINDOW_MAX - HUE_WINDOW_MIN) / HUE_BIN_COUNT
HUE_BIN_CENTRES = np.array([HUE_WINDOW_MIN + HUE_BIN_WIDTH * (i + 0.5)
                            for i in range(HUE_BIN_COUNT)])
EIGHT_MAX_VAR = 0.875                    # extract_hues.cpp max-variance norm
GREEN_BINS_START, GREEN_BINS_END = 5, 8  # bins covering 82.5-120 deg
GREEN_VETO_THRESHOLD = 0.25              # config.h GREEN_MASS_VETO_THRESHOLD

FFT_CENTERS = np.array([150, 250, 350, 450, 550, 650, 750, 850, 950,
                        1100, 1300, 1500, 1700, 1900, 2100], dtype=np.float64)
F2_NORM = 441000.0                       # NOTE: matches firmware; the old
                                         # trainer used 4410000 (10x off)

NUM_CLASSES, DIMS = 5, 28
CLASS_LABELS = ["UNRIPE", "PERFECTLY_RIPE", "OVERRIPE",
                "ROTTEN_OR_HOLLOW", "ARTIFICIALLY_RIPENED"]
WIRE_BYTES = 32 + NUM_CLASSES * DIMS * 4 + NUM_CLASSES * 4 + 4   # 616

VOLUME_NEUTRAL_CM3 = 150.0               # shared by every category -> dead dim

# Knob ranges shown to humans
KNOB_LIMITS = {
    "skin_hue_deg": (20.0, 120.0),
    "spread_deg":   (5.0, 45.0),
    "firmness":     (0.0, 1.0),
    "character":    (0.0, 1.0),
}

# ─── Category presets (knobs tell the story of each class) ────────────
PRESETS = {
    "UNRIPE":               {"skin_hue_deg": 102, "spread_deg": 14,
                             "firmness": 0.05, "character": 0.15},
    "PERFECTLY_RIPE":       {"skin_hue_deg": 38,  "spread_deg": 18,
                             "firmness": 0.80, "character": 0.70},
    "OVERRIPE":             {"skin_hue_deg": 28,  "spread_deg": 34,
                             "firmness": 0.97, "character": 0.95},
    "ROTTEN_OR_HOLLOW":     {"skin_hue_deg": 60,  "spread_deg": 42,
                             "firmness": 0.90, "character": 0.88},
    "ARTIFICIALLY_RIPENED": {"skin_hue_deg": 36,  "spread_deg": 10,
                             "firmness": 0.10, "character": 0.25},
}

DEFAULT_ENABLED = ["UNRIPE", "PERFECTLY_RIPE"]


def hue_to_rgb(h_deg):
    """Standard HSV hue (S=V=1) -> (r,g,b) 0..255, for UI swatches only."""
    h = (h_deg % 360.0) / 60.0
    i = int(h) % 6
    f = h - int(h)
    table = [(1, f, 0), (1 - f, 1, 0), (0, 1, f),
             (0, 1 - f, 1), (f, 0, 1), (1, 0, 1 - f)]
    r, g, b = table[i]
    return (int(r * 255), int(g * 255), int(b * 255))


# ─── Knobs -> 28-D state ──────────────────────────────────────────────
def hue_histogram(skin_hue_deg, spread_deg):
    bump = np.exp(-0.5 * ((HUE_BIN_CENTRES - skin_hue_deg) / spread_deg) ** 2)
    total = bump.sum()
    return bump / total if total > 0 else np.full(8, 0.125)


def chromatic_dispersion(hist):
    """Device formula (extract_hues.cpp:55): 1 - variance/max_var."""
    var = float(np.var(hist))
    d = 1.0 - var / EIGHT_MAX_VAR
    return min(max(d, 0.0), 1.0)


def green_mass(state_or_hist):
    h = np.asarray(state_or_hist)[:8] if len(state_or_hist) == 28 \
        else np.asarray(state_or_hist)
    return float(h[GREEN_BINS_START:GREEN_BINS_END].sum())


def state_from_knobs(knobs):
    """dict(skin_hue_deg, spread_deg, firmness, character) -> 28 floats."""
    h = float(np.clip(knobs["skin_hue_deg"], *KNOB_LIMITS["skin_hue_deg"]))
    s = float(np.clip(knobs["spread_deg"], *KNOB_LIMITS["spread_deg"]))
    firm = float(np.clip(knobs["firmness"], *KNOB_LIMITS["firmness"]))
    char_ = float(np.clip(knobs["character"], *KNOB_LIMITS["character"]))

    state = np.zeros(DIMS)
    hist = hue_histogram(h, s)
    state[0:8] = hist
    state[8] = chromatic_dispersion(hist)

    vol23 = VOLUME_NEUTRAL_CM3 ** (2.0 / 3.0)
    state[9] = min(max((vol23 - 10.0) / (300.0 - 10.0), 0.0), 1.0)

    # Spectral bump: hard fruit rings high (f_c ~1 kHz), soft thuds low
    # (~300 Hz); 'character' widens the bump (crisp -> dull).
    f_centre = 1000.0 * (0.3 ** firm)          # 1 kHz hard .. 300 Hz soft
    sigma_ln = 0.35 + 0.85 * char_
    ln_ratio = np.log(FFT_CENTERS / f_centre)
    peak, floor = -0.9, -7.6                   # conditioned-space amplitudes
    prof = floor + (peak - floor) * np.exp(-0.5 * (ln_ratio / sigma_ln) ** 2)
    state[10:25] = np.maximum(prof, -10.0)     # respect FFT_CLAMP_MIN

    state[25] = 0.30 + 0.55 * char_            # crisp low-H .. mushy high-H
    # state[26] impact amplitude: force-invariant -> always 0
    # state[27]: reserved 0
    return state


# ─── Nearest-proto bridge (same math as rules_to_model.py) ────────────
def _scale_aware_ranges(P):
    """Range floor that mirrors RulesModel.buildFromPrototypes in Dart:
       r_d = max(range_d, 0.10, 0.05*max_c|p_c,d|)

    Without it, two classes that nearly agree on a dim (diff 0.02) get their
    1/r^2 term amplified into ~2000-weight dims that drown the real signal
    (recorded mango: 6/13 -> 11/13 leave-one-out with this floor).
    """
    ranges = P.max(axis=0) - P.min(axis=0)
    max_abs = np.max(np.abs(P), axis=0)
    ranges = np.maximum(ranges, np.maximum(0.10, 0.05 * max_abs))
    return ranges


def _auto_temp(states_by_label, W, b, mask, temp_factor=1.0):
    """Auto-calibrated softmax temperature, mirroring the Dart
    buildFromPrototypes: the worst winner-vs-runner-up margin at the
    prototypes maps to ~90% confidence. Returns the effective temperature
    that W,b should be divided by (so the firmware's temp=1 softmax yields
    honest posteriors)."""
    labels = [l for l in CLASS_LABELS if l in states_by_label]
    worst_margin = np.inf
    for lab in labels:
        c = CLASS_LABELS.index(lab)
        scores = classify(states_by_label[lab], W, b, mask)
        own = scores[c]
        best_other = -np.inf
        for i, s in enumerate(scores):
            if i == c or not ((mask >> i) & 1):
                continue
            if s > best_other:
                best_other = s
        margin = own - best_other
        if margin < worst_margin:
            worst_margin = margin
    if worst_margin == np.inf or worst_margin <= 0:
        auto_temp = 2.0
    else:
        auto_temp = worst_margin / 2.1972   # ln(9): 90/10 odds at prototype
    return float(np.clip(auto_temp * temp_factor, 0.05, 500.0))


def build_rules_model(states_by_label, enabled_labels, temp=1.0):
    """{label: 28-vector} + enabled list -> auto-calibrated (W, b, ranges,
    mask). The softmax temperature is AUTO-CALIBRATED from the prototypes
    (mirrors Dart RulesModel.buildFromPrototypes); [temp] acts as a
    multiplier (1.0 = the validated default)."""
    labels = [l for l in CLASS_LABELS if l in states_by_label]
    assert labels, "no categories given"
    P = np.stack([states_by_label[l] for l in labels])
    ranges = _scale_aware_ranges(P)

    W = np.zeros((NUM_CLASSES, DIMS))
    b = np.zeros(NUM_CLASSES)
    for row, lab in enumerate(labels):
        c = CLASS_LABELS.index(lab)
        p = P[row]
        W[c] = 2.0 * p / (ranges ** 2)
        b[c] = -np.sum((p ** 2) / (ranges ** 2))

    mask = 0
    for lab in enabled_labels:
        mask |= 1 << CLASS_LABELS.index(lab)

    eff_temp = _auto_temp(states_by_label, W, b, mask, temp)
    return W / eff_temp, b / eff_temp, ranges, mask


def classify(state, W, b, mask):
    """Mirror evaluate_fruit_single(): disabled classes score -100000."""
    scores = W @ np.asarray(state) + b
    for c in range(NUM_CLASSES):
        if not ((mask >> c) & 1):
            scores[c] = -100000.0
    return scores


def selftest(states_by_label, W, b, mask, n=200, seed=0):
    """Perturbed prototypes must classify as their own nearest prototype
    (nearest measured with the same per-dim ranges the bridge used)."""
    rng = np.random.default_rng(seed)
    labels = sorted(states_by_label)
    P = np.stack([states_by_label[l] for l in labels])
    r = _scale_aware_ranges(P)
    ok = True
    for i, lab in enumerate(labels):
        base = states_by_label[lab]
        trials = [base] + [base + rng.normal(0, .03, DIMS) *
                           (np.abs(rng.random(DIMS)) < .3) for _ in range(n)]
        for x in trials:
            pred = CLASS_LABELS[int(np.argmax(classify(x, W, b, mask)))]
            d = (((x[None, :] - P) / r) ** 2).sum(axis=1)
            ref = labels[int(np.argmin(d))]
            ok &= pred == ref
    return ok


# ─── Packing / rules files ─────────────────────────────────────────────
def pack_binary(name, W, b, mask):
    nb = name.encode()[:31].ljust(32, b"\0")   # byte 31 must stay clear
    blob = (nb + W.astype("<f4").tobytes() + b.astype("<f4").tobytes() +
            bytes([mask & 0xFF, 0, 0, 0]))
    assert len(blob) == WIRE_BYTES
    return blob


def save_rules(path, name, temp, enabled, knobs_by_label):
    doc = {
        "_comment": "FruitPi human rules — edit knobs, rebuild via "
                    "Rules Builder tab or rules_engine.py",
        "name": name, "temp": temp, "enabled": enabled,
        "categories": knobs_by_label,
    }
    with open(path, "w") as f:
        json.dump(doc, f, indent=2)


def load_rules(path):
    doc = json.load(open(path))
    name = doc.get("name", "Fruit")
    temp = float(doc.get("temp", 2.0))
    enabled = [l for l in doc.get("enabled", DEFAULT_ENABLED)
               if l in CLASS_LABELS]
    knobs = {}
    for lab, k in doc.get("categories", {}).items():
        if lab not in CLASS_LABELS:
            continue
        knobs[lab] = {key: float(np.clip(float(k.get(key, PRESETS[lab][key])),
                                         *lim))
                      for key, lim in KNOB_LIMITS.items()}
    return name, temp, enabled, knobs


def build_from_rules(path):
    """One-call: rules file -> (blob, info dict). Raises on bad input."""
    name, temp, enabled, knobs = load_rules(path)
    if not enabled:
        raise ValueError("no categories enabled in rules file")
    states = {lab: state_from_knobs(k) for lab, k in knobs.items()}
    for lab in enabled:
        if lab not in states:
            raise ValueError(f"'{lab}' enabled but has no knobs/category")
    W, b, ranges, mask = build_rules_model(states, enabled, temp)
    return pack_binary(name, W, b, mask), {
        "name": name, "temp": temp, "enabled": enabled,
        "mask": mask, "W": W, "b": b, "ranges": ranges, "states": states,
    }
