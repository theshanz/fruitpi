#!/usr/bin/env python3
"""Replay a fruit rule file over the collected datasets — the offline twin of
the firmware's rule engine (esp/main/dual_latent.cpp :: evaluate_fruit_rules).

The device takes N taps and MEDIANS every acoustic discriminator over them
before deciding (that is where the multi-tap gain is), then scores each class
as the weighted mean of its band memberships and picks the best. This tool
reproduces exactly that on the stored datasets:

    per tap  -> shear / r_spec / peak          (firmware-identical estimators)
    per session -> median over the N taps      (what the device decides on)
    per class   -> weighted mean of memberships
    decision   -> softmax(temperature) argmax  (+ default / override)

Features that could not be measured offline (the black-spot statistics need
the live camera) are reported as UNAVAILABLE and contribute 0 — the same
"no evidence" rule the firmware applies, so a class that needs the camera
cannot win offline either.

Usage:
    python3 datacollection/replay_fruit_rules.py rules/banana_v2.json banana
    python3 datacollection/replay_fruit_rules.py rules/mango_v2.json mango -v
"""
import argparse
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fruit_rule_bands as B          # noqa: E402
import fruit_rules as FR              # noqa: E402
import tap_quality_filter as TQ       # noqa: E402


# ─── the firmware's scoring, mirrored ─────────────────────────────────
def membership(x, lo, hi, soft):
    if hi < lo:
        lo, hi = hi, lo
    if soft <= 0:
        soft = 1e-3
    d = 0.0 if lo <= x <= hi else (lo - x if x < lo else x - hi)
    if d <= 0:
        return 1.0
    return math.exp(-0.5 * (d / soft) ** 2)


def class_score(rs, cls, feats):
    """Weighted mean of the class's band memberships (missing feature -> 0)."""
    num = den = 0.0
    for si in rs.class_slots.get(cls, []):
        w = rs.slot_weight[si]
        den += w
        if w <= 0:
            continue
        fname = rs.slot_names[si]
        if fname not in feats:
            continue                      # no evidence -> 0, but still in the mean
        lo, hi, soft = rs.class_bands[cls][si]
        num += w * membership(feats[fname], lo, hi, soft)
    return (num / den) if den > 0 else 0.0


def decide(rs, feats, mask):
    """Returns (winner, probs, scores, note)."""
    scores = {}
    for c in range(5):
        if not ((mask >> c) & 1):
            continue
        scores[c] = class_score(rs, c, feats)
    if not scores:
        return None, {}, scores, "no class enabled"

    order = sorted(scores.items(), key=lambda kv: kv[1], reverse=True)
    best, second = order[0], (order[1] if len(order) > 1 else (None, 0.0))
    note = ""
    if rs.default_class is not None and (mask >> rs.default_class) & 1:
        gap = best[1] - (second[1] if second[0] is not None else 0.0)
        if gap < rs.margin:
            scores[rs.default_class] = best[1] + 1e-3
            best = (rs.default_class, scores[rs.default_class])
            note = "default"

    logits = {c: s / rs.temperature for c, s in scores.items()}
    m = max(logits.values())
    exps = {c: math.exp(v - m) for c, v in logits.items()}
    tot = sum(exps.values()) or 1.0
    probs = {c: v / tot for c, v in exps.items()}

    if rs.override is not None:
        oci, conds = rs.override
        ok = True
        for si, op, val in conds:
            fname = rs.slot_names[si]
            if fname not in feats:
                ok = False
                break
            x = feats[fname]
            ok = ok and (x > val if op == ">" else x < val)
        if ok and oci == 0xFE:
            # ANOMALY ONLY: the class decision STANDS and the result is
            # flagged anomalous.  The firmware's reserved value is 0xFE
            # (FRM_ANOMALY_ONLY) - a model opts in by carrying the override,
            # and a model without one never trips.
            note = "anomaly"
        elif ok:
            probs = {c: 0.0 for c in probs}
            probs[oci] = 0.9
            others = [c for c in probs if c != oci]
            for c in others:
                probs[c] = 0.1 / len(others) if others else 0.0
            best = (oci, 1.0)
            note = "override"
    return best[0], probs, scores, note


# ─── dataset -> per-session features ──────────────────────────────────
def session_features(collect, label_key="label", keep=None, filt=None):
    """Per session: the median over its N taps of every acoustic feature.

    When `filt` (a TapFilterSpec) is given, the taps that the filter rejects
    are dropped BEFORE the median — the device would never have counted them.
    """
    rows = collect()
    by = {}
    for r in rows:
        if keep and r["label"] not in keep:
            continue
        by.setdefault((r["label"], r["session"]), []).append(r)
    out = []
    for (lab, sess), rs in by.items():
        if filt is not None:
            waves = [B.load_waveform_csv(r["path"]) for r in rs
                     if r.get("path") and os.path.exists(r["path"])]
            if waves:
                kept, _, _, _ = TQ.apply_filter(waves, filt)
                if kept:
                    rs = [rs[i] for i in kept]
        feats = {
            "shear":  float(np.median([r["shear"] for r in rs])),
            "r_spec": float(np.median([r["r_spec"] for r in rs])),
            "peak":   float(np.median([r["peak"] for r in rs])),
        }
        greens = [r["green"] for r in rs if r.get("green") is not None]
        if greens:
            feats["green"] = float(np.median(greens))
        out.append(dict(label=lab, session=sess, n=len(rs), feats=feats))
    return out


def load_rule_set(path):
    return FR.load_rules(path)


def replay(rs, sessions, verbose=False):
    mask = rs.mask
    ok = bad = 0
    rows = []
    for s in sessions:
        winner, probs, scores, note = decide(rs, s["feats"], mask)
        if winner is None:
            continue
        pred = FR.CLASS_NAMES[winner]
        truth = s["label"]
        # RIPE / PERFECTLY_RIPE are the same biological class.
        norm = {"RIPE": "PERFECTLY_RIPE"}.get(truth, truth)
        good = pred == norm
        ok += good
        bad += (not good)
        rows.append((s, pred, norm, good, probs, scores, note))
        if verbose:
            fl = " ".join(f"{k}={v:+.4f}" for k, v in s["feats"].items())
            print(f"  {'OK ' if good else 'ERR'} {s['session']:<28} "
                  f"n={s['n']:<2} {fl}  -> {pred:<20} (truth {norm}) {note}")
    return ok, bad, rows


def main():
    ap = argparse.ArgumentParser(description="Replay a fruit rule file offline.")
    ap.add_argument("rules")
    ap.add_argument("dataset", choices=["mango", "banana", "sessions"])
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--filter-min-peak-adc", type=int, default=None,
                    help="apply the device tap-quality gate before the median")
    ap.add_argument("--filter-keep-min", type=int, default=1,
                    help="never keep fewer than this many taps (the guard)")
    ap.add_argument("--filter-max-reject", type=float, default=1.0,
                    help="never reject more than this fraction of the taps")
    a = ap.parse_args()

    filt = None
    if a.filter_min_peak_adc is not None:
        filt = TQ.TapFilterSpec(min_peak_adc=a.filter_min_peak_adc,
                                keep_min=a.filter_keep_min,
                                max_reject=a.filter_max_reject)

    rs = load_rule_set(a.rules)
    if a.dataset == "mango":
        sessions = session_features(B.collect_mango, filt=filt)
    elif a.dataset == "banana":
        sessions = session_features(B.collect_banana, keep=("UNRIPE", "PERFECTLY_RIPE"),
                                    filt=filt)
    else:
        sessions = session_features(B.collect_sessions, filt=filt)

    print(f"rule file : {a.rules}  (name={rs.name!r}, mask=0x{rs.mask:02X})")
    print(f"slots     : {', '.join(f'{n}(w={w:g})' for n, w in zip(rs.slot_names, rs.slot_weight))}")
    for c in range(5):
        if c not in rs.class_slots:
            continue
        bands = ", ".join(
            f"{rs.slot_names[si]}[{rs.class_bands[c][si][0]:g},{rs.class_bands[c][si][1]:g}]"
            f"+-{rs.class_bands[c][si][2]:g}" for si in rs.class_slots[c])
        print(f"  {FR.CLASS_NAMES[c]:<22} {bands}")
    if filt is not None:
        kept = sum(s["n"] for s in sessions)
        print(f"\ntap filter: {filt.describe()}")
    print(f"\nreplaying {len(sessions)} sessions on '{a.dataset}':")
    ok, bad, rows = replay(rs, sessions, a.verbose)
    total = ok + bad
    if total:
        print(f"\naccuracy: {ok}/{total} ({100.0*ok/total:.1f}%)   errors {bad}")
    else:
        print("\nno labelled sessions to score")
    return 0


if __name__ == "__main__":
    sys.exit(main())
