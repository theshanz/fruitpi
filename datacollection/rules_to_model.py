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

IMPORTANT: feed states assembled EXACTLY as the device does (run
extract_28d.py with the same ACOUSTIC_FORCE_INVARIANT setting as
esp/main/config.h).

Input prototype files: JSON {"label": "UNRIPE", "state": [28 floats]}
Output: <name>.bin (612 bytes: name[32] + W[5][28] row-major + b[5], f32)

Usage:
  .venv/bin/python datacollection/rules_to_model.py \
      --name Mango --out Mango_rules.bin \
      --proto proto_unripe.json --proto proto_ripe.json \
      [--temp 2.0] [--selftest N]
"""
import argparse, json, struct, sys
import numpy as np

NUM_CLASSES, DIMS = 5, 28
LABELS = ["UNRIPE","PERFECTLY_RIPE","OVERRIPE","ROTTEN_OR_HOLLOW","ARTIFICIALLY_RIPENED"]

def load_protos(paths):
    ps, labels = [], []
    for p in paths:
        d = json.load(open(p))
        st = np.asarray(d["state"], dtype=np.float64)
        assert st.size == DIMS, f"{p}: need {DIMS} dims"
        lab = d.get("label", "")
        if lab and lab not in labels: labels.append(lab)
        else: labels.append(f"class{len(labels)}")
        ps.append(st)
    return np.array(ps), labels

def build_model(protos, temp):
    K, _ = protos.shape
    ranges = protos.max(axis=0) - protos.min(axis=0)
    ranges[ranges == 0] = 1.0                       # dead dims contribute nothing
    W = np.zeros((NUM_CLASSES, DIMS)); b = np.zeros(NUM_CLASSES)
    for c in range(K):
        p = protos[c]
        W[c] = 2.0 * p / (ranges**2)
        b[c] = -np.sum((p**2) / (ranges**2))
    return W / temp, b / temp, ranges

def pack(name, W, b):
    nb = name.encode()[:32].ljust(32, b"\0")
    return nb + W.astype("<f4").tobytes() + b.astype("<f4").tobytes()

def selftest(protos, W, b, n):
    rng = np.random.default_rng(0)
    ok, worst = True, 0.0
    for k in range(len(protos)):
        trials = [protos[k]] + [protos[k] + rng.normal(0,.05,DIMS)*(np.abs(rng.random(DIMS))<.3) for _ in range(n)]
        for x in trials:
            lin = int(np.argmax(W @ x + b))
            # reference: exact nearest-proto with the same per-dim ranges
            r = protos.max(0)-protos.min(0); r[r==0]=1
            ref = int(np.argmin((((x[None,:]-protos)/r)**2).sum(1)))
            worst = max(worst, abs(lin-ref))
            ok &= lin == ref
    print(f"[selftest] {'PASS' if ok else 'FAIL'} ({len(protos)+n*len(protos)} trials, mismatches={int(worst)})")
    return ok

main = argparse.ArgumentParser()
main.add_argument("--name", required=True)
main.add_argument("--proto", action="append", required=True)
main.add_argument("--out", default=None)
main.add_argument("--temp", type=float, default=2.0)
main.add_argument("--selftest", type=int, default=200)
a = main.parse_args()

P, labels = load_protos(a.proto)
W, b, ranges = build_model(P, a.temp)
print("ranges :", np.array2string(ranges, precision=5))
print("labels :", labels[:len(P)])
ok = selftest(P, W, b, a.selftest)

blob = pack(a.name, W, b)
out = a.out or f"{a.name}_rules.bin"
open(out,"wb").write(blob)
print(f"[+] wrote {out} ({len(blob)} bytes; expected 612: {'OK' if len(blob)==612 else 'BAD'})")
sys.exit(0 if ok and len(blob)==612 else 1)
