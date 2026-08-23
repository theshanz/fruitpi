#!/usr/bin/env python3
"""Extract firmware-faithful 28-D feature prototypes from saved data-collector
sample sessions.

Replicates EXACTLY the math in esp/main/piezo_acoustic.cpp and sci_28d.cpp so
prototypes hard-coded into sci_28d_hardcoded.cpp match what the device
computes at inference time:

    state[0..7]  = hue_histogram            (averaged across hue_*.json)
    state[8]     = chromatic_dispersion     (averaged)
    state[9]     = normalize(volume_cm3^(2/3))  (volume from hue JSONs)
    state[10..24]= fft_bins                 (averaged across waveform_*.csv)
    state[25]    = spectral_entropy         (averaged)
    state[26]    = impact_amplitude = max|w - mean(w)| / 4095   (averaged)
    state[27]    = 0

FFT features: per-window DC removal, hanning(512) window, rfft, power summed
over bins k-1..k+1 around the rfft bin nearest each of the 15 center
frequencies (Hann mainlobe integration), `log(power+1e-10)*center^2/441000`
clamped to -10. Entropy normalizes over the 15 integrated band powers.

NOTE: waveform CSVs store RAW ADC counts (0-4095); state[26] is normalized to
match the firmware's AcousticFeatures::impact_amplitude (a deviation from the
window mean, not a raw peak — the raw peak moves with the charge-amp bias).

When both an UNRIPE and a PERFECTLY_RIPE session are passed, the RANGE_SCALE
table (|ripe - unripe| per dimension, zeros -> 1.0) is printed as well.

Usage:
    .venv/bin/python datacollection/extract_28d.py \
        dataset/Mango/Mango_<unripe_id> dataset/Mango/Mango_<ripe_id>
"""
import json
import os
import sys

import numpy as np

SAMPLING_FREQ_HZ = 8820.0
FFT_SIZE = 512
N_FFT_BINS = 15
F2_NORM = 441000.0
EPS_LOG = 1e-10
FFT_CLAMP_MIN = -10.0
MIN_MASS23 = 10.0
MAX_MASS23 = 300.0
ADC_MAX_COUNTS = 4095.0

# Keep in sync with esp/main/config.h:ACOUSTIC_FORCE_INVARIANT.
ACOUSTIC_FORCE_INVARIANT = True
IMPACT_AMP_FLOOR = 0.004

FFT_CENTERS = np.array([
    150.0, 250.0, 350.0, 450.0, 550.0, 650.0, 750.0, 850.0, 950.0,
    1100.0, 1300.0, 1500.0, 1700.0, 1900.0, 2100.0,
])

BIN_RESOLUTION = SAMPLING_FREQ_HZ / FFT_SIZE  # ~17.2266 Hz


def hanning(n):
    i = np.arange(n)
    return 0.5 * (1.0 - np.cos(2.0 * np.pi * i / (n - 1)))


def fft_features(wave):
    """Replicate PiezoAcoustic::process_captured_buffer() -> fft_bins + entropy."""
    wave = np.asarray(wave, dtype=np.float64)

    # Firmware removes the per-window mean before windowing.
    centered = wave - wave.mean()
    impact_amp = float(np.abs(centered).max())  # raw-count deviation

    windowed = centered * hanning(len(wave))
    fft = np.fft.rfft(windowed)
    power = np.abs(fft) ** 2

    raw_power = np.empty(N_FFT_BINS)
    fft_bins = np.empty(N_FFT_BINS)
    for b in range(N_FFT_BINS):
        center = FFT_CENTERS[b]
        bin_idx = int(round(center / BIN_RESOLUTION))
        if bin_idx >= len(power):
            bin_idx = len(power) - 1
        # Firmware integrates the Hann mainlobe: bins k-1 .. k+1.
        lo = max(bin_idx - 1, 0)
        hi = min(bin_idx + 1, len(power) - 1)
        p = float(power[lo:hi + 1].sum())
        raw_power[b] = p
        fft_bins[b] = max(np.log(p + EPS_LOG) * (center * center) / F2_NORM,
                          FFT_CLAMP_MIN)

    total = raw_power.sum()
    if total < 1e-15:
        entropy = 0.0
    else:
        prob = raw_power / total
        entropy = -float(np.sum(prob * np.log(prob))) / np.log(15.0)
        entropy = max(0.0, min(1.0, entropy))

    return fft_bins, entropy, impact_amp


def extract_sample(sample_dir):
    files = sorted(os.listdir(sample_dir))

    hues = []
    for f in files:
        if f.startswith("hue_") and f.endswith(".json"):
            with open(os.path.join(sample_dir, f)) as fh:
                hues.append(json.load(fh))

    if hues:
        hist = np.mean([h["hue_histogram"] for h in hues], axis=0)
        dispersion = float(np.mean([h["chromatic_dispersion"] for h in hues]))
        vol = float(np.mean([h.get("volume_cm3", 0.0) for h in hues]))
    else:
        with open(os.path.join(sample_dir, "metadata.json")) as fh:
            meta = json.load(fh)
        hist = np.array(meta.get("hue_histogram", [0.125] * 8))
        dispersion = float(meta.get("chromatic_dispersion", 1.0))
        vol = float(meta.get("volume_cm3", 150.0))

    waves = []
    for f in files:
        if f.startswith("waveform_") and f.endswith(".csv"):
            waves.append(np.loadtxt(os.path.join(sample_dir, f), delimiter=","))

    fft_avg = None
    ent_avg = 0.0
    peak_avg = 0.0
    for w in waves:
        fft_bins, entropy, peak = fft_features(w)
        if fft_avg is None:
            fft_avg = fft_bins
        else:
            fft_avg += fft_bins
        ent_avg += entropy
        peak_avg += peak
    n = len(waves) or 1
    fft_avg = fft_avg / n
    ent_avg /= n
    peak_avg /= n

    state = np.zeros(28)
    state[0:8] = hist
    state[8] = dispersion
    vol_23 = vol ** (2.0 / 3.0)
    state[9] = np.clip((vol_23 - MIN_MASS23) / (MAX_MASS23 - MIN_MASS23), 0.0, 1.0)
    if ACOUSTIC_FORCE_INVARIANT:
        # Mirror assemble_state_28d(): subtract per-band amp^2 correction so
        # tap strength cancels; dim 26 becomes class-neutral zero.
        amp = max(peak_avg / ADC_MAX_COUNTS, IMPACT_AMP_FLOOR)
        corr = 2.0 * float(np.log(amp))
        fft_avg = fft_avg - corr * (FFT_CENTERS**2) / F2_NORM
        peak_avg_norm = 0.0
    else:
        peak_avg_norm = peak_avg / ADC_MAX_COUNTS
    state[10:25] = fft_avg
    state[25] = ent_avg
    state[26] = peak_avg_norm
    state[27] = 0.0

    return {
        "state": state,
        "n_taps": len(waves),
        "n_hues": len(hues),
        "volume_cm3": vol,
        "fft_bins": fft_avg,
        "spectral_entropy": ent_avg,
        "impact_amplitude": peak_avg / ADC_MAX_COUNTS,  # raw value, pre-invariance
        "impact_amp_counts": peak_avg,
    }


def read_category(sample_dir):
    with open(os.path.join(sample_dir, "metadata.json")) as fh:
        return json.load(fh).get("category", "")


def print_c_array(name, state):
    vals = ", ".join(f"{v:.6f}f" for v in state)
    print(f"static const float {name}[VECTOR_DIMENSIONS] = {{ {vals} }};")


def print_range_scale(unripe_state, ripe_state):
    """Per-dimension |ripe - unripe|, zeros replaced by 1.0 — the exact
    normalization sci_28d_hardcoded.cpp dist_sq() divides by."""
    scale = np.abs(ripe_state - unripe_state)
    scale[scale == 0.0] = 1.0
    print("static const float RANGE_SCALE[VECTOR_DIMENSIONS] = { "
          + ", ".join(f"{v:.6f}f" for v in scale) + " };")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    by_label = {}
    for path in sys.argv[1:]:
        if not os.path.isdir(path):
            print(f"[!] not a directory: {path}")
            continue
        print(f"\n=== {path} ===")
        r = extract_sample(path)
        category = read_category(path)
        print(f"taps={r['n_taps']} hues={r['n_hues']} volume={r['volume_cm3']:.0f} cm3")
        print(f"entropy={r['spectral_entropy']:.4f} "
              f"impact_amp={r['impact_amplitude']:.4f} ({r['impact_amp_counts']:.1f} counts)")
        print(f"state = [{', '.join(f'{v:.4f}' for v in r['state'])}]")

        if "UNRIPE" in category:
            name = "UNRIPE_PROTOTYPE"
        elif "RIPE" in category or "PERFECTLY" in category:
            name = "RIPE_PROTOTYPE"
        else:
            name = f"PROTOTYPE_{category or 'UNKNOWN'}"
        print_c_array(name, r["state"])
        print(f"fft_bins = [{', '.join(f'{v:.3f}' for v in r['fft_bins'])}]")
        if name in ("UNRIPE_PROTOTYPE", "RIPE_PROTOTYPE"):
            by_label[name] = r["state"]

    if "UNRIPE_PROTOTYPE" in by_label and "RIPE_PROTOTYPE" in by_label:
        print("\n// ─── RANGE_SCALE (paste alongside the prototypes) ───")
        print_range_scale(by_label["UNRIPE_PROTOTYPE"], by_label["RIPE_PROTOTYPE"])


if __name__ == "__main__":
    main()
