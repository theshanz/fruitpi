#!/usr/bin/env python3
"""Convert human-readable rules (nearest-prototype tables) into an OG 28-D
linear model (.bin) that the stock firmware engine runs directly.

Math bridge: argmin over range-normalized squared distance
    d_c(x) = sum_i ((x_i-p_ci)/r_i)^2
is identical to argmax of the linear score
    score_c(x) = w_c.x + b_c,   w_c = 2*p_c/r^2,   b_c = -sum_i p_ci^2/r_i^2
(the quadratic term sum x_i^2/r_i^2 is class-independent and drops out).
A softmax temperature keeps reported probabilities sane; decisions are
temperature-invariant.

CATEGORIES: every prototype JSON carries a "label"; each recognized label
enables its class bit inside the model itself (active_class_mask byte at
offset 612). Feed two protos ("UNRIPE" + "PERFECTLY_RIPE") and the device
classifies with exactly those two categories — no rebuild needed. The file
order of --proto arguments does not matter; weights always land on the
canonical CLASS_LABELS row. Duplicate labels are rejected (one prototype
per category — the linear bridge is nearest-single-proto).

IMPORTANT: feed states assembled EXACTLY as the device does (run
extract_28d.py with the same ACOUSTIC_FORCE_INVARIANT setting as
esp/main/config.h).

Input prototype files: JSON {"label": "UNRIPE", "state": [28 floats]}
Output: <name>.bin (616 bytes: name[32] + W[5][28] row-major + b[5] +
        active_class_mask + reserved[3], f32)

Usage:
  .venv/bin/python datacollection/rules_to_model.py \
      --name Mango --out Mango_rules.bin \
      --proto proto_unripe.json --proto proto_ripe.json \
      [--temp 2.0] [--selftest N] [--mask 0x03]
"""
import argparse, json, struct, sys
import numpy as np

NUM_CLASSES, DIMS = 5, 28
LABELS = ["UNRIPE","PERFECTLY_RIPE","OVERRIPE","ROTTEN_OR_HOLLOW","ARTIFICIALLY_RIPENED"]
WIRE_BYTES = 32 + NUM_CLASSES * DIMS * 4 + NUM_CLASSES * 4 + 4      # 616

def load_protos(paths):
    """[(class_index, state)] with strict, duplicate-free labels."""
    by_class = {}
    for p in paths:
        d = json.load(open(p))
        st = np.asarray(d["state"], dtype=np.float64)
        assert st.size == DIMS, f"{p}: need {DIMS} dims"
        lab = d.get("label", "")
        if lab not in LABELS:
            raise SystemExit(f"{p}: label {lab!r} not one of {LABELS}")
        c = LABELS.index(lab)
        if c in by_class:
            raise SystemExit(f"{p}: duplicate label {lab!r} "
                             f"(one prototype per category)")
        by_class[c] = st
    return by_class

def build_model(by_class, temp):
    protos = np.stack(list(by_class.values()))
    ranges = protos.max(axis=0) - protos.min(axis=0)
    ranges[ranges == 0] = 1.0                       # dead dims contribute nothing
    W = np.zeros((NUM_CLASSES, DIMS)); b = np.zeros(NUM_CLASSES)
    for c, p in by_class.items():
        W[c] = 2.0 * p / (ranges**2)
        b[c] = -np.sum((p**2) / (ranges**2))
    mask = 0
    for c in by_class:
        mask |= 1 << c
    return W / temp, b / temp, ranges, mask

def pack(name, W, b, mask):
    nb = name.encode()[:32].ljust(32, b"\0")
    return (nb + W.astype("<f4").tobytes() + b.astype("<f4").tobytes() +
            bytes([mask & 0xFF, 0, 0, 0]))

def selftest(by_class, W, b, mask, n):
    """Mirror the device: disabled classes score -100000 before argmax."""
    rng = np.random.default_rng(0)
    enabled = sorted(by_class)
    r = np.stack(list(by_class.values()))
    r = r.max(0) - r.min(0); r[r == 0] = 1.0

    ok = True
    trials_total = 0
    for k in enabled:
        trials = [by_class[k]] + [by_class[k] +
                 rng.normal(0,.05,DIMS)*(np.abs(rng.random(DIMS))<.3)
                 for _ in range(n)]
        for x in trials:
            gated = W @ x + b
            for c in range(NUM_CLASSES):
                if not ((mask >> c) & 1):
                    gated[c] = -100000.0
            lin = int(np.argmax(gated))
            ref_d = [(((x - by_class[c]) / r)**2).sum() for c in enabled]
            ref = enabled[int(np.argmin(ref_d))]
            ok &= lin == ref
            trials_total += 1
    print(f"[selftest] {'PASS' if ok else 'FAIL'} "
          f"({trials_total} trials, classes={[LABELS[c] for c in enabled]})")
    return ok

main = argparse.ArgumentParser()
main.add_argument("--name", required=True)
main.add_argument("--proto", action="append", required=True)
main.add_argument("--out", default=None)
main.add_argument("--temp", type=float, default=2.0)
main.add_argument("--selftest", type=int, default=200)
main.add_argument("--mask", default=None,
                  help="override class bitmask (e.g. 0x03); default: auto "
                       "from the labels found in the prototypes")
a = main.parse_args()

by_class = load_protos(a.proto)
W, b, ranges, auto_mask = build_model(by_class, a.temp)
mask = int(a.mask, 0) if a.mask else auto_mask
if mask == 0:
    raise SystemExit("empty class mask — nothing would be classified")
print("ranges :", np.array2string(ranges, precision=5))
print("classes:", ", ".join(LABELS[c] for c in sorted(by_class)),
      f"(mask=0x{mask:02X}" + (", overridden" if a.mask else ", from labels") + ")")
ok = selftest(by_class, W, b, mask, a.selftest)

blob = pack(a.name, W, b, mask)
out = a.out or f"{a.name}_rules.bin"
open(out,"wb").write(blob)
print(f"[+] wrote {out} ({len(blob)} bytes; expected {WIRE_BYTES}: "
      f"{'OK' if len(blob)==WIRE_BYTES else 'BAD'})")
sys.exit(0 if ok and len(blob)==WIRE_BYTES else 1)
