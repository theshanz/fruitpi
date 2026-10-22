#!/usr/bin/env python3
"""Fruit-specific discriminator rules -> 852-byte Dual-Latent model blob (v2).

The v1 DLAT blob carries ONE global ripeness latent that every fruit shares.
The v2 blob keeps the exact same 852 bytes, the same 'DLAT' magic and the
same BLE transfer contract, but spends the previously reserved 728 bytes at
offset 124 on a RULE BLOCK: handcrafted, per-fruit discriminators.

A rule file is JSON:

    {
      "name": "Banana Spots v1",
      "active_class_mask": 19,              # optional, derived when omitted
      "modality": 2,
      "min_peak_adc": 160,

      "temperature": 0.12,                  # softmax temperature on class scores
      "margin": 0.05,                       # min winner/runner-up gap, else default
      "default_class": null,                # class used when the gap is too small
      "anomaly_threshold": 0.5,             # min match ratio for an anomaly class
      "ripeness_gain": 1.0,

      "slots": [                            # the discriminators THIS fruit uses
        {"feature": "r_spec",   "weight": 1.0},
        {"feature": "green",    "weight": 0.6},
        {"feature": "dark_spot","weight": 1.4}
      ],

      "classes": {
        "UNRIPE": {
          "slots": ["r_spec", "green"],
          "bands": {"r_spec": [-2.00, -1.20, 0.25],   # [lo, hi, softness]
                    "green":  [ 0.35,  1.00, 0.15]}
        },
        "PERFECTLY_RIPE": {
          "slots": ["r_spec", "green"],
          "bands": {"r_spec": [-1.20, -0.20, 0.25],
                    "green":  [ 0.00,  0.35, 0.15]}
        },
        "ROTTEN_OR_HOLLOW": {
          "slots": ["dark_spot"],
          "bands": {"dark_spot": [0.05, 1.00, 0.02]}   # spots -> anomaly
        }
      },

      "override": {                          # optional hard rule
        "class": "ARTIFICIALLY_RIPENED",
        "conditions": [
          {"feature": "warm",   "op": ">", "value": 0.55},
          {"feature": "r_spec", "op": ">", "value": -1.00}
        ]
      }
    }

An override may also carry ONE condition (a pure defect check, e.g. "black
spots"): it is packed into BOTH wire condition slots, which the firmware
ANDs - so the wire keeps its exact shape and old blobs are unaffected.  An
override whose conditions are all OPTICAL is evaluated by the device RIGHT
AFTER the camera scan: a defect the camera can see does not wait for the
taps.

Band semantics: the membership is 1.0 inside [lo, hi] and decays as
exp(-0.5*(d/soft)^2) outside, where d is the distance to the nearest edge.
A class score is the weighted MEAN of its band memberships over the slots it
uses, so every class score lands in [0,1] and classes are comparable. A slot
whose feature could not be measured contributes 0 but still counts in the
denominator — a class that needs a channel we do not have cannot win by
default (that is what makes the black-spot discriminator safe: with no
camera, ROTTEN_OR_HOLLOW cannot win).

The black-spot discriminator is OPT-IN: a model that does not put
"dark_spot" in a slot is completely unaffected by it.
"""
import argparse
import json
import struct
import sys

WIRE_BYTES = 852
RULE_OFFSET = 124
RULE_BYTES = 728

# ─── Mirrors esp/main/fruit_rule.h (DiscFeature) ─────────────────────
FEATURES = {
    "shear":        0,   # median FBM-2D shear ratio
    "r_spec":       1,   # median de-coated shear  (shear - beta*ln(peak*FS))
    "r_mom":        2,   # median moment contrast
    "entropy":      3,   # median spectral entropy
    "centroid":     4,   # median bio_moments[0] temporal centroid
    "tail":         5,   # median bio_moments[1] tail energy
    "harmonic":     6,   # median bio_moments[2] harmonic absorption
    "abbott":       7,   # median bio_moments[3] abbott stiffness
    "bio_entropy":  8,   # median bio_moments[4] spectral entropy
    "damping":      9,   # median bio_moments[5] dynamic damping
    "stft_t1":     10,   # median STFT T1 early-decay cell
    "peak":        11,   # median peak_abs
    "green":       12,   # optics: green mass
    "warm":        13,   # optics: warm mass
    "hue_mean":    14,   # optics: mean hue (deg)
    "dispersion":  15,   # optics: chromatic dispersion
    "hue_entropy": 16,   # optics: hue-histogram entropy
    "dark_spot":   17,   # optics: black/brown SPOT area fraction
    "dark_mass":   18,   # optics: total dark area fraction
    "mode_band":   19,   # MEAN of per-tap mode-cell share (262-332 Hz)
}

# Mirrors esp/main/sci_32d.h (CLASS_LABELS)
CLASS_NAMES = [
    "UNRIPE",
    "PERFECTLY_RIPE",
    "OVERRIPE",
    "ROTTEN_OR_HOLLOW",
    "ARTIFICIALLY_RIPENED",
]
CLASS_INDEX = {n: i for i, n in enumerate(CLASS_NAMES)}

# The classic (v1) latent constants. In a v2 blob they only feed the
# features the rules reuse: beta (de-coated shear), the volume clamp (the
# abbott term) and the green-bin range.
CLASSIC_DEFAULTS = {
    "r0_spec":    -1.150,
    "r0_mom":    100.0,
    "alpha":      12.0,
    "beta":        0.20,
    "tau":         0.08,
    "w_acoustic":  0.5,
    "w_optics":    0.5,
    "vol_lo":     10.0,
    "vol_hi":    600.0,
    "vol23_lo":   4.64,
    "vol23_hi":  71.13,
    "anchors": (0.10, 0.85, 1.10, 1.30, 0.10),
    "green_start": 4,
    "green_end":   8,
}

# struct format of FruitRuleBlock (packed, little-endian) — 728 bytes.
RULE_FMT = "<IIffIff8B5B3x8f40f40f40fI4Bff156x"
assert struct.calcsize(RULE_FMT) == RULE_BYTES, "rule struct size mismatch"


class RuleError(Exception):
    pass


def _feature_id(name):
    if name not in FEATURES:
        raise RuleError(f"unknown feature {name!r} (known: {', '.join(FEATURES)})")
    return FEATURES[name]


# The override target that flags an ANOMALY without naming a class: the
# class decision stands and the result is marked anomalous.  It is how a model
# uses a defect that has no training samples of its own - a black spot on a
# banana, say, where calling it ROTTEN would be a claim the corpus cannot
# support.  The firmware's reserved value is 0xFE (FRM_ANOMALY_ONLY).
OVERRIDE_ANOMALY_ONLY = 0xFE


def _class_index(name):
    key = str(name).strip().upper()
    if key in ("ANOMALY_ONLY", "ANOMALY", "FLAG_ONLY"):
        return OVERRIDE_ANOMALY_ONLY
    if key not in CLASS_INDEX:
        raise RuleError(f"unknown class {name!r} (known: {', '.join(CLASS_NAMES)})")
    return CLASS_INDEX[key]


class RuleSet:
    """A parsed, validated rule file."""

    def __init__(self, raw: dict):
        self.raw = raw
        self.name = str(raw.get("name", "Fruit Model"))
        self.modality = int(raw.get("modality", 2))
        self.min_peak_adc = int(raw.get("min_peak_adc", 0))

        # ── slots (the discriminators this fruit uses) ──
        self.slot_names = []
        self.slot_weight = []
        for slot in raw.get("slots", []):
            self.slot_names.append(str(slot["feature"]))
            self.slot_weight.append(float(slot.get("weight", 1.0)))
        if not self.slot_names:
            raise RuleError("the rule file declares no slots")
        if len(self.slot_names) > 8:
            raise RuleError(f"at most 8 slots, got {len(self.slot_names)}")
        self.slot_index = {n: i for i, n in enumerate(self.slot_names)}
        for n, w in zip(self.slot_names, self.slot_weight):
            _feature_id(n)
            if w < 0:
                raise RuleError(f"slot {n!r}: weight must be >= 0")
        if len(self.slot_index) != len(self.slot_names):
            raise RuleError("duplicate feature in slots")

        # ── classes ──
        self.class_slots = {}    # class idx -> [slot idx]
        self.class_bands = {}    # class idx -> {slot idx: (lo, hi, soft)}
        for cname, cdef in raw.get("classes", {}).items():
            ci = _class_index(cname)
            used = cdef.get("slots", [])
            bands = cdef.get("bands", {})
            if set(used) != set(bands.keys()):
                raise RuleError(
                    f"class {cname}: slots and bands must name the same features")
            slot_idx, band_map = [], {}
            for sname in used:
                if sname not in self.slot_index:
                    raise RuleError(
                        f"class {cname}: slot {sname!r} is not in the slot list")
                si = self.slot_index[sname]
                band = bands[sname]
                if len(band) != 3:
                    raise RuleError(
                        f"class {cname}/{sname}: band must be [lo, hi, soft]")
                lo, hi, soft = float(band[0]), float(band[1]), float(band[2])
                if hi < lo:
                    raise RuleError(f"class {cname}/{sname}: hi < lo")
                if soft <= 0:
                    raise RuleError(f"class {cname}/{sname}: softness must be > 0")
                slot_idx.append(si)
                band_map[si] = (lo, hi, soft)
            self.class_slots[ci] = slot_idx
            self.class_bands[ci] = band_map
        if not self.class_slots:
            raise RuleError("the rule file declares no classes")

        # ── class mask ──
        mask = raw.get("active_class_mask")
        if mask is None:
            mask = 0
            for ci in self.class_slots:
                mask |= 1 << ci
        self.mask = int(mask)
        if self.mask == 0:
            raise RuleError("active_class_mask must enable at least one class")
        for ci in range(5):
            if (self.mask >> ci) & 1 and ci not in self.class_slots:
                raise RuleError(
                    f"class mask enables {CLASS_NAMES[ci]} but it has no bands")

        # ── global knobs ──
        self.temperature = float(raw.get("temperature", 0.12))
        self.margin = float(raw.get("margin", 0.05))
        self.anomaly_threshold = float(raw.get("anomaly_threshold", 0.5))
        self.ripeness_gain = float(raw.get("ripeness_gain", 1.0))
        if self.temperature <= 0:
            raise RuleError("temperature must be > 0")

        dc = raw.get("default_class")
        self.default_class = None if dc in (None, "", "none") else _class_index(dc)

        # ── hard override ──
        ov = raw.get("override")
        self.override = None
        if ov:
            oci = _class_index(ov["class"])
            conds = ov.get("conditions", [])
            if len(conds) not in (1, 2):
                raise RuleError("the override needs one or two conditions")
            parsed = []
            for c in conds:
                sname = str(c["feature"])
                if sname not in self.slot_index:
                    raise RuleError(
                        f"override: slot {sname!r} is not in the slot list")
                op = str(c.get("op", ">"))[:1]
                if op not in (">", "<"):
                    raise RuleError("override: op must be '>' or '<'")
                parsed.append((self.slot_index[sname], op, float(c["value"])))
            self.override = (oci, parsed)

        # ── classic latent constants (overridable from the rule file) ──
        self.classic = dict(CLASSIC_DEFAULTS)
        for key in ("r0_spec", "r0_mom", "alpha", "beta", "tau", "w_acoustic",
                    "w_optics", "vol_lo", "vol_hi", "vol23_lo", "vol23_hi",
                    "green_start", "green_end"):
            if key in raw:
                self.classic[key] = float(raw[key])
        if "anchors" in raw:
            self.classic["anchors"] = tuple(float(x) for x in raw["anchors"])


def load_rules(path: str) -> RuleSet:
    with open(path, "r", encoding="utf-8") as f:
        return RuleSet(json.load(f))


def pack_rule_block(rs: RuleSet) -> bytes:
    """Pack the 728-byte FruitRuleBlock (offset 124..851 of the wire)."""
    slot_mask = 0
    slot_feature = [0] * 8
    slot_weight = [0.0] * 8
    for i, n in enumerate(rs.slot_names):
        slot_mask |= 1 << i
        slot_feature[i] = FEATURES[n]
        slot_weight[i] = rs.slot_weight[i]

    class_slot_mask = [0] * 5
    class_lo = [0.0] * 40
    class_hi = [0.0] * 40
    class_soft = [0.0] * 40
    for ci, slots in rs.class_slots.items():
        for si in slots:
            class_slot_mask[ci] |= 1 << si
            lo, hi, soft = rs.class_bands[ci][si]
            class_lo[ci * 8 + si] = lo
            class_hi[ci * 8 + si] = hi
            class_soft[ci * 8 + si] = soft

    default_class = 0xFF if rs.default_class is None else rs.default_class
    if rs.override:
        oci, conds = rs.override
        if len(conds) == 1:
            # A ONE-condition override is packed into BOTH wire condition
            # slots.  The firmware ANDs the two conditions, so the same
            # condition twice is exactly the same rule - and the wire keeps
            # its shape (no new field, no new op code, old blobs unchanged).
            conds = [conds[0], conds[0]]
        (s1, op1, v1), (s2, op2, v2) = conds
        # The firmware evaluates the override against the FEATURE table
        # (fs.ok[slot] / fs.v[slot] in esp/main/dual_latent.cpp), so the
        # wire carries the feature id, not the slot index. The device's
        # validator additionally requires that id to be < DISC_COUNT and to
        # name an ACTIVE slot, which is why an override can only use one of
        # the first eight features.
        s1 = FEATURES[rs.slot_names[s1]]
        s2 = FEATURES[rs.slot_names[s2]]
    else:
        # No override: every override field must be canonical zero, the
        # device rejects the blob otherwise (fruit_rule_block_valid).
        oci, s1, op1, v1 = 0xFF, 0, ">", 0.0
        s2, op2, v2 = 0, ">", 0.0

    block = struct.pack(
        RULE_FMT,
        1,                                  # block_version
        slot_mask,
        rs.temperature,
        rs.margin,
        default_class,
        rs.anomaly_threshold,
        rs.ripeness_gain,
        *slot_feature,
        *class_slot_mask,
        *slot_weight,
        *class_lo,
        *class_hi,
        *class_soft,
        oci,
        s1, 0 if op1 == ">" else 1, s2, 0 if op2 == ">" else 1,
        v1, v2,
    )
    assert len(block) == RULE_BYTES, f"rule block is {len(block)} bytes"
    return block


def build_blob(rs: RuleSet) -> bytes:
    """Pack the full 852-byte DLAT wire (v2)."""
    c = rs.classic
    name = rs.name.encode("ascii", "replace")[:32].ljust(32, b"\x00")

    blob = bytearray(WIRE_BYTES)
    blob[0:32] = name
    struct.pack_into("<I", blob, 32, 0x444C4154)      # "DLAT"
    blob[36] = 2                                       # wire_version (v2 = rules)
    blob[37] = rs.mask
    blob[38] = rs.modality
    blob[39] = int(c["green_start"])
    blob[40] = int(c["green_end"])
    struct.pack_into("<f", blob, 48, c["r0_spec"])
    struct.pack_into("<f", blob, 52, c["r0_mom"])
    struct.pack_into("<f", blob, 56, c["alpha"])
    struct.pack_into("<f", blob, 60, c["beta"])
    struct.pack_into("<f", blob, 64, c["tau"])
    struct.pack_into("<f", blob, 68, c["w_acoustic"])
    struct.pack_into("<f", blob, 72, c["w_optics"])
    struct.pack_into("<f", blob, 76, c["vol_lo"])
    struct.pack_into("<f", blob, 80, c["vol_hi"])
    struct.pack_into("<f", blob, 84, c["vol23_lo"])
    struct.pack_into("<f", blob, 88, c["vol23_hi"])
    for i, a in enumerate(c["anchors"]):
        struct.pack_into("<f", blob, 92 + 4 * i, a)
    struct.pack_into("<H", blob, 113, rs.min_peak_adc)
    blob[116] = 0                                      # acoustic_feature_mode
    struct.pack_into("<f", blob, 120, 0.5)             # stft_t1_prior
    blob[RULE_OFFSET:RULE_OFFSET + RULE_BYTES] = pack_rule_block(rs)
    return bytes(blob)


def describe(blob: bytes) -> None:
    name = blob[0:32].split(b"\x00", 1)[0].decode("ascii", "replace")
    ver, mask, mod = blob[36], blob[37], blob[38]
    print(f"name        : {name}")
    print(f"wire_version: {ver} ({'classic latent' if ver == 1 else 'fruit rules'})")
    enabled = [CLASS_NAMES[c] for c in range(5) if (mask >> c) & 1]
    print(f"class mask  : 0x{mask:02X} -> {', '.join(enabled)}")

    if ver < 2:
        return

    (bver, slot_mask, temp, margin, dclass, anom, gain) = struct.unpack_from(
        "<IIffIff", blob, RULE_OFFSET)
    slot_feature = list(blob[RULE_OFFSET + 28:RULE_OFFSET + 36])
    class_slot_mask = list(blob[RULE_OFFSET + 36:RULE_OFFSET + 41])
    slot_weight = struct.unpack_from("<8f", blob, RULE_OFFSET + 44)
    lo = struct.unpack_from("<40f", blob, RULE_OFFSET + 76)
    hi = struct.unpack_from("<40f", blob, RULE_OFFSET + 236)
    soft = struct.unpack_from("<40f", blob, RULE_OFFSET + 396)
    oclass = struct.unpack_from("<I", blob, RULE_OFFSET + 556)[0]
    os1, oop1, os2, oop2 = blob[RULE_OFFSET + 560:RULE_OFFSET + 564]
    ov1, ov2 = struct.unpack_from("<ff", blob, RULE_OFFSET + 564)

    inv = {v: k for k, v in FEATURES.items()}
    print(f"rules       : v{bver} temperature={temp:g} margin={margin:g} "
          f"default={CLASS_NAMES[dclass] if dclass < 5 else 'none'} "
          f"anomaly>={anom:g} ripeness_gain={gain:g}")
    print("slots:")
    for i in range(8):
        if not (slot_mask >> i) & 1:
            continue
        users = [CLASS_NAMES[c] for c in range(5)
                 if (class_slot_mask[c] >> i) & 1]
        print(f"  [{i}] {inv.get(slot_feature[i], slot_feature[i]):<12} "
              f"weight={slot_weight[i]:g}  used by: {', '.join(users) or '-'}")
    print("classes:")
    for c in range(5):
        used = [i for i in range(8) if (class_slot_mask[c] >> i) & 1]
        if not used:
            continue
        bands = ", ".join(
            f"{inv.get(slot_feature[i], slot_feature[i])}"
            f"[{lo[c*8+i]:g},{hi[c*8+i]:g}]+-{soft[c*8+i]:g}" for i in used)
        print(f"  {CLASS_NAMES[c]:<22} {bands}")
    if oclass < 5:
        opname = {0: ">", 1: "<"}
        print(f"override    : {CLASS_NAMES[oclass]} if "
              f"{inv.get(slot_feature[os1], os1)} {opname.get(oop1,'?')} {ov1:g} "
              f"AND {inv.get(slot_feature[os2], os2)} {opname.get(oop2,'?')} {ov2:g}")
    elif oclass == OVERRIDE_ANOMALY_ONLY:
        opname = {0: ">", 1: "<"}
        f1 = inv.get(os1, os1)
        f2 = inv.get(os2, os2)
        optical = {"green", "warm", "hue_mean", "dispersion", "hue_entropy",
                   "dark_spot", "dark_mass"}
        when = ("RIGHT AFTER the camera scan (optical)"
                if f1 in optical and f2 in optical else "after the taps")
        print(f"override    : ANOMALY_ONLY if "
              f"{f1} {opname.get(oop1,'?')} {ov1:g} "
              f"AND {f2} {opname.get(oop2,'?')} {ov2:g}  -> {when}")


def validate_blob(blob: bytes):
    """Mirror of the device's dual_wire_valid() for a v2 blob.

    Every rule of esp/main/fruit_rule.cpp :: fruit_rule_block_valid() is
    checked here so a blob that builds on the host is guaranteed to be
    accepted by the firmware (and a blob that does not is rejected before it
    is ever pushed over BLE). Raises RuleError on the first violation.
    """
    if len(blob) != WIRE_BYTES:
        raise RuleError(f"blob is {len(blob)} bytes, expected {WIRE_BYTES}")
    if struct.unpack_from("<I", blob, 32)[0] != 0x444C4154:
        raise RuleError("bad magic at offset 32 (expected 'DLAT')")
    if blob[36] != 2:
        raise RuleError(f"wire_version is {blob[36]}, expected 2")
    if blob[37] == 0:
        raise RuleError("active_class_mask is empty")

    b = blob[RULE_OFFSET:RULE_OFFSET + RULE_BYTES]
    (bver, slot_mask, temp, margin, dclass, anom, gain) = struct.unpack_from(
        "<IIffIff", b, 0)
    slot_feature = list(b[28:36])
    class_slot_mask = list(b[36:41])
    pad0 = b[41:44]
    slot_weight = struct.unpack_from("<8f", b, 44)
    lo = struct.unpack_from("<40f", b, 76)
    hi = struct.unpack_from("<40f", b, 236)
    soft = struct.unpack_from("<40f", b, 396)
    oclass = struct.unpack_from("<I", b, 556)[0]
    os1, oop1, os2, oop2 = b[560:564]
    ov1, ov2 = struct.unpack_from("<ff", b, 564)
    reserved = b[572:728]

    if bver != 1:
        raise RuleError(f"block_version is {bver}, expected 1")
    if slot_mask == 0:
        raise RuleError("slot_mask is empty")
    if slot_mask & ~((1 << 8) - 1):
        raise RuleError("slot_mask has bits outside the slot array")
    if not (temp > 0) or temp != temp or temp in (float("inf"), float("-inf")):
        raise RuleError("temperature must be finite and > 0")
    if margin < 0 or margin != margin:
        raise RuleError("margin must be finite and >= 0")
    if not (0.0 <= anom <= 1.0) or anom != anom:
        raise RuleError("anomaly_threshold must be in [0,1]")
    if gain < 0 or gain != gain:
        raise RuleError("ripeness_gain must be finite and >= 0")
    if dclass != 0xFF and dclass >= 5:
        raise RuleError(f"default_class {dclass} is out of range")
    if any(pad0):
        raise RuleError("pad0 is not canonical (must be zero)")
    if any(reserved):
        raise RuleError("reserved tail is not canonical (must be zero)")

    for i in range(8):
        active = (slot_mask >> i) & 1
        if not active:
            if slot_feature[i] != 0:
                raise RuleError(f"inactive slot {i} has a non-zero feature id")
            if slot_weight[i] != 0.0:
                raise RuleError(f"inactive slot {i} has a non-zero weight")
            continue
        if slot_feature[i] >= len(FEATURES):
            raise RuleError(f"slot {i}: feature id {slot_feature[i]} is unknown")
        if slot_weight[i] < 0 or slot_weight[i] != slot_weight[i]:
            raise RuleError(f"slot {i}: weight must be finite and >= 0")

    for c in range(5):
        if class_slot_mask[c] & ~slot_mask:
            raise RuleError(f"class {c} uses a slot that is not active")
        for i in range(8):
            used = (class_slot_mask[c] >> i) & 1
            if not used:
                if lo[c * 8 + i] or hi[c * 8 + i] or soft[c * 8 + i]:
                    raise RuleError(f"class {c} slot {i}: unused band is not zero")
                continue
            if lo[c * 8 + i] != lo[c * 8 + i] or hi[c * 8 + i] != hi[c * 8 + i]:
                raise RuleError(f"class {c} slot {i}: non-finite band edge")
            if soft[c * 8 + i] <= 0 or soft[c * 8 + i] != soft[c * 8 + i]:
                raise RuleError(f"class {c} slot {i}: softness must be > 0")
            if hi[c * 8 + i] < lo[c * 8 + i]:
                raise RuleError(f"class {c} slot {i}: hi < lo")

    if oclass != 0xFF:
        if oclass != OVERRIDE_ANOMALY_ONLY and oclass >= 5:
            raise RuleError(f"override_class {oclass} is out of range")
        if oop1 > 1 or oop2 > 1:
            raise RuleError("override op must be 0 (>) or 1 (<)")
        if os1 >= len(FEATURES) or os2 >= len(FEATURES):
            raise RuleError("override slot id is unknown")
        # The override names a FEATURE id, and the firmware requires it to
        # name a feature one of the ACTIVE slots carries - so the check is
        # against the active slots' feature ids, not against the slot mask
        # as if the id were a slot index.
        active_features = {slot_feature[i] for i in range(8)
                           if (slot_mask >> i) & 1}
        if os1 not in active_features or os2 not in active_features:
            raise RuleError("override references a slot that is not active")
    else:
        if os1 or os2 or oop1 or oop2 or ov1 or ov2:
            raise RuleError("no override class, but the override fields are set")
    return True


def main():
    ap = argparse.ArgumentParser(
        description="Build an 852-byte v2 Dual-Latent blob from a fruit rule file.")
    ap.add_argument("rules", help="rule file (JSON)")
    ap.add_argument("out", nargs="?", default=None, help="output .bin path")
    a = ap.parse_args()

    try:
        rs = load_rules(a.rules)
    except (RuleError, OSError, ValueError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    blob = build_blob(rs)
    assert len(blob) == WIRE_BYTES, f"bad size {len(blob)}"
    try:
        validate_blob(blob)
    except RuleError as e:
        print(f"error: the device would reject this blob: {e}", file=sys.stderr)
        return 3
    out = a.out or (rs.name.replace(" ", "_") + ".bin")
    with open(out, "wb") as f:
        f.write(blob)
    print(f"wrote {out} ({len(blob)} bytes)")
    describe(blob)
    print("\nupload over BLE: the device dual_wire_valid()s it and lists it as a model.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
