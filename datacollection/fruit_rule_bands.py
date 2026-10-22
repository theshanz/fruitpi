#!/usr/bin/env python3
"""Derive per-fruit DISCRIMINATOR BANDS from the collected datasets.

The firmware decides with the wire-v2 rule block (esp/main/fruit_rule.h): each
class owns a band per discriminator, and the class score is the weighted mean
of the band memberships. This tool measures the SAME acoustic coordinates
offline so the bands can be written from data instead of guesses.

The acoustic twin is exact (same estimator as esp/main/dual_latent.cpp):

    shear  = E(>1kHz) / E(20..300Hz)      2048-pt zero-padded FFT
    r_spec = shear - beta * ln(peak_counts)

`beta` is the blob's de-coating factor (default 0.20) and `peak_counts` is
the raw tap amplitude in ADC counts, exactly like the firmware's
`peak_abs * PIEZO_ADC_FULL_SCALE`.

Usage:
    python3 datacollection/fruit_rule_bands.py mango
    python3 datacollection/fruit_rule_bands.py banana
    python3 datacollection/fruit_rule_bands.py sessions
"""
import glob
import json
import os
import sys

import numpy as np

FS = 8820.0
FFT_N = 2048
WINDOW = 512
BETA = 0.20

DATA_MANGO = "/home/SharedWorkspace/school/project/data/dataset/Mango"
DATA_BANANA = "/home/SharedWorkspace/school/project/data/dataset/banana-ggg-withnoise"
SESSIONS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "sessions")


def load_waveform_csv(path):
    w = np.genfromtxt(path, delimiter=",")
    if w.ndim > 1:
        w = w[:, 0]
    return np.asarray(w, dtype=np.float64)[:WINDOW]


def shear_ratio(w):
    """E(>1kHz)/E(20..300Hz) — the FBM-2D shear ratio (firmware twin)."""
    sp = np.abs(np.fft.rfft(w, n=FFT_N)) ** 2
    df = FS / FFT_N
    n = len(sp)

    def idx(hz):
        return min(int(round(hz / df)), n - 1)

    i20, i300, i1k = idx(20.0), idx(300.0), idx(1000.0)
    e_low = sp[i20:i300].sum() + 1e-12
    e_high = sp[i1k:].sum() + 1e-12
    return float(e_high / e_low)


def tap_coords(w, beta=BETA):
    """(shear, r_spec, peak_counts) for one tap."""
    shear = shear_ratio(w)
    peak = float(np.max(np.abs(w)))
    r_spec = shear - beta * np.log(max(peak, 1e-6))
    return shear, float(r_spec), peak


# Mirror of esp/main/config.h (LOG_BAND_EDGES_HZ / LOG_MODE_BAND_INDEX).
LOG_BAND_EDGES_HZ = (
    50.0, 63.4, 80.3, 101.7, 128.9, 163.3, 206.9, 262.2, 332.2, 420.9,
    533.4, 675.8, 856.4, 1085.3, 1375.1, 1742.3, 2200.0,
)
LOG_MODE_BAND_INDEX = 7


def mode_band_fraction(w):
    """Mode-cell share of |FFT| — the wire's mode_band discriminator.

    Firmware twin (piezo_acoustic.cpp): mean-centred Hann window, 512-point
    rFFT, MAGNITUDES summed into the 16 log cells, cell[7] (262.2-332.2 Hz)
    over the total.  The device's session value is the MEAN over taps.
    """
    centred = w - w.mean()
    spectrum = np.abs(np.fft.rfft(centred * np.hanning(len(centred)), n=512))
    freqs = np.fft.rfftfreq(512, 1.0 / FS)
    cells = np.array([
        spectrum[(freqs >= LOG_BAND_EDGES_HZ[i]) &
                 (freqs < LOG_BAND_EDGES_HZ[i + 1])].sum()
        for i in range(16)
    ])
    total = cells.sum()
    return float((cells[LOG_MODE_BAND_INDEX] + 1e-12) / (total + 1e-12))


def session_mode_band(d, gate=160):
    """(mean mode_band over taps with peak >= gate, kept, total) or None."""
    kept = []
    for p in session_tap_paths(d):
        w = load_waveform_csv(p)
        if float(np.max(np.abs(w))) >= gate:
            kept.append(mode_band_fraction(w))
    if not kept:
        return None
    return float(np.mean(kept)), len(kept), len(session_tap_paths(d))


def session_tap_paths(d):
    return sorted(glob.glob(os.path.join(d, "waveform_*.csv")))


def session_taps(d):
    return [load_waveform_csv(p)
            for p in sorted(glob.glob(os.path.join(d, "waveform_*.csv")))]


def hue_green_warm(d):
    """(green mass, warm mass) from the session's hue captures, if any."""
    hist = []
    for p in sorted(glob.glob(os.path.join(d, "hue_*.json"))):
        j = json.load(open(p))
        for v in j.values():
            if isinstance(v, list) and len(v) == 8:
                hist.append([float(x) for x in v])
                break
    if not hist:
        return None
    h = np.mean(hist, axis=0)
    return float(h[4:8].sum()), float(h[0:4].sum())


def report(rows, title):
    print(f"\n=== {title} ===")
    labels = sorted({r["label"] for r in rows})
    for lab in labels:
        sub = [r for r in rows if r["label"] == lab]
        rs = np.array([r["r_spec"] for r in sub])
        sh = np.array([r["shear"] for r in sub])
        pk = np.array([r["peak"] for r in sub])
        print(f"{lab:<18} n={len(sub):3d} taps")
        print(f"   r_spec  {rs.min():+.3f} .. {rs.max():+.3f}   "
              f"median {np.median(rs):+.3f}  p25 {np.percentile(rs,25):+.3f} "
              f"p75 {np.percentile(rs,75):+.3f}")
        print(f"   shear   {sh.min():+.4f} .. {sh.max():+.4f}  "
              f"median {np.median(sh):+.4f}")
        print(f"   peak    {pk.min():.1f} .. {pk.max():.1f}  "
              f"median {np.median(pk):.1f}")
        for r in sub[:8]:
            print(f"      {r['session']:<28} tap{r['tap']:<2} "
                  f"r_spec={r['r_spec']:+.3f} shear={r['shear']:.4f} "
                  f"peak={r['peak']:.0f}")


def collect_mango():
    rows = []
    for d in sorted(glob.glob(os.path.join(DATA_MANGO, "Mango_*"))):
        meta_p = os.path.join(d, "metadata.json")
        if not os.path.exists(meta_p):
            continue
        meta = json.load(open(meta_p))
        lab = meta.get("category", "?")
        gw = hue_green_warm(d)
        for i, w in enumerate(session_taps(d)):
            shear, r_spec, peak = tap_coords(w)
            rows.append(dict(label=lab, session=os.path.basename(d), tap=i + 1,
                             shear=shear, r_spec=r_spec, peak=peak,
                             green=gw[0] if gw else None,
                             path=session_tap_paths(d)[i]))
    return rows


def collect_banana():
    assign = {}
    ap = "/home/SharedWorkspace/school/project/data/dataset/assignments.json"
    if os.path.exists(ap):
        assign = json.load(open(ap))
    rows = []
    for d in sorted(glob.glob(os.path.join(DATA_BANANA, "banana-ggg-withnoise_*"))):
        base = os.path.basename(d)
        lab = assign.get(base, "?")
        gw = hue_green_warm(d)
        for i, w in enumerate(session_taps(d)):
            shear, r_spec, peak = tap_coords(w)
            rows.append(dict(label=lab, session=base, tap=i + 1,
                         shear=shear, r_spec=r_spec, peak=peak,
                         green=gw[0] if gw else None,
                         path=session_tap_paths(d)[i]))
    return rows


def collect_sessions():
    """The in-repo captured sessions (mixed fruit — labels only)."""
    lab_path = os.path.join(SESSIONS, "labels.json")
    if not os.path.exists(lab_path):
        return []
    labels = json.load(open(lab_path))
    rows = []
    for s, lab in labels.items():
        d = os.path.join(SESSIONS, s)
        for i, w in enumerate(session_taps(d)):
            shear, r_spec, peak = tap_coords(w)
            rows.append(dict(label=lab, session=s, tap=i + 1, shear=shear,
                             r_spec=r_spec, peak=peak, green=None,
                             path=session_tap_paths(d)[i]))
    return rows


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "mango"
    if what == "mango":
        rows = collect_mango()
        title = "Mango (Mango_* dataset, metadata category)"
    elif what == "banana":
        rows = collect_banana()
        title = "Banana (banana-ggg-withnoise, assignments.json)"
    elif what == "sessions":
        rows = collect_sessions()
        title = "Captured sessions (MIXED fruit — no per-fruit label)"
    else:
        print(f"unknown target {what!r} (mango|banana|sessions)", file=sys.stderr)
        return 2
    if not rows:
        print("no data", file=sys.stderr)
        return 1
    report(rows, title)

    # Threshold suggestion: the best single split on r_spec (and on shear).
    labels = sorted({r["label"] for r in rows})
    if len(labels) == 2:
        for key in ("r_spec", "shear"):
            vals = sorted({round(r[key], 6) for r in rows})
            best = None
            for i in range(len(vals) - 1):
                thr = (vals[i] + vals[i + 1]) / 2
                err = 0
                for r in rows:
                    pred = labels[1] if r[key] > thr else labels[0]
                    if pred != r["label"]:
                        err += 1
                if best is None or err < best[1]:
                    best = (thr, err)
            print(f"\nbest single {key} threshold: {best[0]:+.4f}  "
                  f"errors {best[1]}/{len(rows)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
