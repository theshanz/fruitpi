#!/usr/bin/env python3
"""Final production pipeline: notch0px unconditional + heal + 2.5σ ellipse + solidity branch.

Implements the exact 6-step pipeline:
1. Compute Sobel gradient magnitude on ΔY
2. Unconditional 0px Gradient Notch before CCL
3. Centre-Bias Connected Component Selection
4. Safe Constrained Healing (1-pass dilation inside raw_fg)
5. Shape Fitting & Metrics (solidity branch)
6. Perimeter Overflow Detection (2% edge buffer)
"""
import math, os, sys
import numpy as np
from PIL import Image
from scipy.ndimage import label, binary_fill_holes, binary_dilation, binary_erosion, sobel, generate_binary_structure

_TW, _TH = 320, 256
_CM_PX = 0.2
_DY_BG_MIN = 14
_CENTRE_FRAC = 0.10
_V_MIN = 0.08
_SIGMA = 40.0
_TBL_S_MIN = 0.25
_HUE_S_MIN = 0.30
_FG_MIN = 400
_FOVEAL_FG_MIN = 400
_H_LO, _H_HI = 20.0, 120.0
_H_BINS = 8
_VOL_MAX, _VOL_MIN = 500.0, 10.0
_FOVEAL_K = 2.8

# Foveal Center-Box hue extraction (simple, fool-proof)
# Central ROI around cx=160, cy=128: x [110..210], y [78..178]
# Only pixels with dY >= 14 and V >= 0.15 survive. Skin rejection: H [15..35], S <= 0.50.
# 10-bin histogram [20°..155°]. If valid pixels < 250 return None (fg=0).
def foveal_center_box_hue(flash_img, ambient_img, w=320, h=256):
    X0, X1, Y0, Y1 = 110, 210, 78, 178
    MIN_DY, MIN_V = 14.0, 0.15
    MIN_PIXELS = 250
    hist = np.zeros(10, dtype=np.float32)
    valid_pixels = 0
    for y in range(Y0, Y1 + 1):
        for x in range(X0, X1 + 1):
            dY = float(np.abs(flash_img[y, x] - ambient_img[y, x]))
            r, g, b = flash_img[y, x]
            max_c = max(int(r), int(g), int(b))
            min_c = min(int(r), int(g), int(b))
            delta = max_c - min_c
            V_approx = max_c / 255.0
            if max_c > 0:
                s_approx = delta / max_c
            else:
                s_approx = 0.0
            # Approximate hue from dominant channel
            if delta == 0:
                h_approx = 0.0
            elif max_c == r:
                h_approx = (60.0 * ((g - b) / delta) + 360.0) % 360.0
            elif max_c == g:
                h_approx = (60.0 * ((b - r) / delta) + 120.0) % 360.0
            else:
                h_approx = (60.0 * ((r - g) / delta) + 240.0) % 360.0
            # Saturation + value floor + dY gate
            if dY < MIN_DY or V_approx < MIN_V or s_approx < 0.25 or V_approx < 0.25:
                continue
            # Skin rejection
            if (15.0 <= h_approx <= 35.0) and (s_approx <= 0.50):
                continue
            valid_pixels += 1
            bin_idx = int((h_approx - 20.0) / 13.5)
            if bin_idx < 0: bin_idx = 0
            if bin_idx > 9: bin_idx = 9
            hist[bin_idx] += 1.0
    if valid_pixels < MIN_PIXELS:
        return None  # fg = 0
    total = hist.sum()
    if total > 0:
        hist = hist / total
    else:
        hist = np.ones(10, dtype=np.float32) / 10.0
    green = hist[4:7].sum()  # approximate green bins
    yellow = hist[2].sum() if len(hist) > 2 else 0.0
    return {"green": float(green), "yellow": float(yellow), "valid_pixels": valid_pixels, "hist": hist}
_HEAL_PX = 1
_SOLIDITY_CONVEX_THRESH = 0.88
_OVFL_L, _OVFL_R, _OVFL_T, _OVFL_B = 0x01, 0x02, 0x04, 0x08

# ─── Conservative foveal center lock (Item 1) ──────────────────────
# The selected foreground blob MUST physically intersect this central box —
# width/height = 2*0.15*W, 2*0.15*H. If the chosen blob sits on a side and the
# centre crosshair is empty, we fail fast instead of guessing at a peripheral
# warm-coloured chair / hand. 0.15 of W=320 → 48 px, H=256 → 38 px, generous
# enough to admit a centred mango while rejecting off-centre mis-aims.
_FOVEAL_LOCK_FRAC = 0.15

# ─── Conservative skin gate (Item 2a) ───────────────────────────────
# Strip human-skin pixels from the hue histogram. Skin band: H 15–35°, S ≤ 0.55
# (fingers are moderate S), V ≥ 0.20 (avoid mistaking dark shadow for skin).
_SKIN_HUE_MIN, _SKIN_HUE_MAX = 15.0, 35.0
_SKIN_S_MAX, _SKIN_V_MIN     = 0.55, 0.20

# ─── Foveal core sampling (Item 2b) ────────────────────────────────
# Extract the final hue histogram from the central 70% erosion of the mask
# (one morphological erosion pass: 1-px diamond), cutting off edge boundaries
# where background and fingers blend with fruit skin.
_FOVEAL_CORE_FRAC   = 0.70
_BBOX_BOTTOM_STRIP  = 0.30   # bottom 30% of bbox gets the strict skin gate


def _load(p):
    return np.asarray(Image.open(p).convert("RGB").resize((_TW, _TH), Image.BILINEAR), dtype=np.uint8)


def _hsv(r, g, b):
    r, g, b = r.astype(np.float64)/255, g.astype(np.float64)/255, b.astype(np.float64)/255
    mx, mn = np.maximum(np.maximum(r, g), b), np.minimum(np.minimum(r, g), b)
    d = mx - mn
    h = np.zeros_like(mx)
    m = d > 0
    h = np.where(m & (mx == r), 60*(((g-b)/d)%6), h)
    h = np.where(m & (mx == g), 60*(((b-r)/d)+2), h)
    h = np.where(m & (mx == b), 60*(((r-g)/d)+4), h)
    h = np.where(h < 0, h+360, h)
    s = np.where(mx == 0, 0.0, d/mx)
    return h, s, mx


def _centre_bias_select(raw, w, h):
    lab, nb = label(raw)
    if nb == 0:
        return np.zeros_like(raw), np.zeros_like(raw, dtype=bool)
    sz = np.bincount(lab.ravel())[1:]
    W2, H2 = w/2.0, h/2.0
    inv = 0.5/(_SIGMA**2)
    best, bs, best_lbl = None, -1.0, 0
    for i in range(1, nb+1):
        b = lab == i
        ys, xs = np.where(b)
        cxm, cym = (xs.min()+xs.max())/2.0, (ys.min()+ys.max())/2.0
        sc = float(sz[i-1])*np.exp(-(((cxm-W2)**2+(cym-H2)**2)*inv))
        if best is None or sc > bs:
            bs, best, best_lbl = sc, b, i
    return best, (lab > 0) & (lab != best_lbl)


def _ell_crop(mask, sigma=2.5):
    oys, oxs = np.where(mask)
    if len(oys) < 4: return mask.copy(), 0.0, 0.0, 0.0, 0.0
    cx, cy = oxs.mean(), oys.mean()
    u20, u02, u11 = float(((oxs-cx)**2).mean()), float(((oys-cy)**2).mean()), float(((oxs-cx)*(oys-cy)).mean())
    tr = u20+u02
    di = math.sqrt((u20-u02)**2+4*u11**2)
    L1 = (tr+di)/2
    a = sigma*math.sqrt(max(L1, 1e-9))
    b = sigma*math.sqrt(max(tr-L1, 1e-9))
    if a < 1e-6 or b < 1e-6: return mask.copy(), 0.0, 0.0, cx, cy
    ang = 0.5*math.atan2(2*u11, u20-u02)
    co, si = math.cos(ang), math.sin(ang)
    dx, dy = (oxs-cx).astype(np.float64), (oys-cy).astype(np.float64)
    u, v = co*dx+si*dy, -si*dx+co*dy
    inside = ((u/a)**2+(v/b)**2) <= 1.0
    r = mask.copy(); r[oys[~inside], oxs[~inside]] = False
    return r, float(a), float(b), cx, cy


def _overflow(mask, w, h):
    if mask.sum() == 0: return 0
    ys, xs = np.where(mask)
    fx, fy = 0.02*w, 0.02*h
    o = 0
    if xs.min() <= fx: o |= _OVFL_L
    if xs.max() >= (w-1)-fx: o |= _OVFL_R
    if ys.min() <= fy: o |= _OVFL_T
    if ys.max() >= (h-1)-fy: o |= _OVFL_B
    return o


def _bbox_str(mask):
    ys, xs = np.where(mask)
    return "—" if len(xs)==0 else f"{int(xs.max()-xs.min()+1)}×{int(ys.max()-ys.min()+1)}"


def _solidity(mask):
    hull = binary_fill_holes(mask)
    return float(mask.sum())/float(hull.sum()) if hull.sum()>0 else 0.0


def _vol_ellipse(a_px, b_px, cm_per_px):
    a_cm, b_cm = a_px * cm_per_px, b_px * cm_per_px
    lo, hi = min(a_cm, b_cm), max(a_cm, b_cm)
    return max(_VOL_MIN, min((3.1415926535/6)*(lo*lo)*hi, _VOL_MAX))


def _vol_bbox(mask):
    if mask.sum() == 0: return 0.0
    ys, xs = np.where(mask)
    wc, hc = (int(xs.max())-int(xs.min())+1)*_CM_PX, (int(ys.max())-int(ys.min())+1)*_CM_PX
    a, b = min(wc, hc), max(wc, hc)
    return max(_VOL_MIN, min((3.1415926535/6)*(a*a)*b, _VOL_MAX))


def _hue(mask, fr, fr_v, vmin, foveal_core=None):
    """Conservative hue histogram from a 2D mask of candidate pixels.

    Item 2 (foveal core + skin gate):
      - when `foveal_core` is supplied, the hue histogram is built only from the
        central eroded region (mask edges excluded), so boundary hand/background
        blending does not pollute the colour readout.
      - the human-skin band (H 15-35°, S ≤ 0.55, V ≥ 0.20) is stripped
        everywhere; the bottom 30% of the bbox (where fingers grip the fruit)
        gets the stricter "any low-S red-orange" removal so a finger pad or
        palm cannot masquerade as ripe pigment.
    """
    fw = fr.astype(np.float64).copy()
    hh, ss, _ = _hsv(fw[..., 0], fw[..., 1], fw[..., 2])
    # Gate hue-report pixels on the raw (pre-WB) saturation: washed-out neutral
    # cast regions (~S 0.24) must not count as green, while real fruit (~S 0.5)
    # stays in. No artificial WB B-scale is applied — the sensor's AWB balances
    # the flash frame naturally (mirrors the one-shot/process_frame path).
    _, sr, _ = _hsv(fr[..., 0], fr[..., 1], fr[..., 2])

    # Use the foveal core (central 70% erosion) as the histogram source.
    region = foveal_core if foveal_core is not None else mask

    # Skin gate (Item 2a): strip the skin band everywhere, then apply the
    # stricter finger-grip removal in the bottom 30% of the bbox.
    skin_band = (hh >= _SKIN_HUE_MIN) & (hh <= _SKIN_HUE_MAX) & \
                (ss <= _SKIN_S_MAX) & (fr_v >= _SKIN_V_MIN)
    if region.any():
        ys, xs = np.where(region)
        if len(xs):
            y0, y1 = int(ys.min()), int(ys.max())
            bottom_strip_y = y1 - int(_BBOX_BOTTOM_STRIP * (y1 - y0 + 1))
            bottom_strip = np.zeros_like(region, dtype=bool)
            bottom_strip[bottom_strip_y:, :] = True
            # Strict finger-grip: a flesh-tone pad in the bottom strip is almost
            # certainly a finger covering the fruit — remove it.
            finger_zone = region & bottom_strip
            region = region & ~(skin_band & finger_zone)
    # Global skin-band removal (final pass).
    region = region & ~skin_band

    keep = region & (fr_v >= vmin) & (ss >= 0.15) & (sr >= _HUE_S_MIN)
    if keep.sum() == 0: return np.full(_H_BINS, 1.0/_H_BINS)
    hv = hh[keep]
    w = (hv >= _H_LO) & (hv <= _H_HI)
    if w.sum() == 0: return np.full(_H_BINS, 1.0/_H_BINS)
    bw = (_H_HI-_H_LO)/_H_BINS
    bi = np.clip(((hv[w]-_H_LO)/bw).astype(int), 0, _H_BINS-1)
    return np.bincount(bi, minlength=_H_BINS).astype(float)/w.sum()


def _overflow_str(ovf):
    if ovf == 0: return "none"
    edges = []
    if ovf & _OVFL_L: edges.append("L")
    if ovf & _OVFL_R: edges.append("R")
    if ovf & _OVFL_T: edges.append("T")
    if ovf & _OVFL_B: edges.append("B")
    return "+".join(edges)


def _segment(amb, f1, W=_TW, H=_TH):
    """Canonical 6-step segmentation of one burst (ambient + flash #1 only).

    Single source of truth for the on-host mirror (ms_tool.compute_local_report)
    and the firmware burst path. Returns a dict with the final mask + metrics,
    or None when no foreground survives.
    """
    # ΔY map from flash #1 only (canonical reference; flash2 is unused here).
    f1i, ai = f1.astype(np.int32), amb.astype(np.int32)
    dY = np.clip(0.299*(f1i[...,0]-ai[...,0])+0.587*(f1i[...,1]-ai[...,1])+0.114*(f1i[...,2]-ai[...,2]), 0, 255).astype(np.uint8)

    # ── Foveal threshold (adaptive ΔY/V) ──
    cx, cy = W//2, H//2
    rw, rh = int(W*_CENTRE_FRAC), int(H*_CENTRE_FRAC)
    sv = f1[cy-rh:cy+rh, cx-rw:cx+rw].astype(float).max(axis=-1)/255.0
    vmin = max(_V_MIN, float(np.percentile(sv, 10)))
    fv = f1.astype(float).max(axis=-1)/255.0
    sp = dY[cy-rh:cy+rh, cx-rw:cx+rw]
    dy_thresh = max(_DY_BG_MIN, int(np.percentile(sp, 25)))
    raw_fg = (dY >= dy_thresh) & (fv >= vmin)

    # ── Strict foveal fail-fast (Item 2, 2026-09-05) ──
    # The central foveal box must itself yield a confident fruit-like foreground.
    # A blob pinned to a frame edge / washed to the sensor border reads 0-140
    # foveal px and returns no-fg rather than being guessed at; live centred
    # captures sit at 1095-2407 px. The 400 floor is ≥2σ from both populations,
    # so it is not a hair-trigger threshold. Mirrors firmware MS_FOVEAL_FG_MIN.
    if int(raw_fg[cy-rh:cy+rh, cx-rw:cx+rw].sum()) < _FOVEAL_FG_MIN:
        return None

    if raw_fg.sum() < _FG_MIN:
        return None

    # ── Step 1: Sobel gradient magnitude on ΔY ──
    gx = sobel(dY.astype(np.float64), axis=1)
    gy = sobel(dY.astype(np.float64), axis=0)
    gmag = np.sqrt(gx**2 + gy**2)

    # Foveal-adaptive gradient threshold
    foveal_gmag = gmag[cy-rh:cy+rh, cx-rw:cx+rw]
    mu_fov = float(foveal_gmag.mean())
    sigma_fov = float(foveal_gmag.std())
    t_grad = mu_fov + _FOVEAL_K * sigma_fov

    # ── Step 2: Unconditional 0px Gradient Notch before CCL ──
    grad_edges = gmag > t_grad
    ccl_input = raw_fg.copy()
    ccl_input[grad_edges] = 0

    # Safety: if notch removes >80% of fg, fall back to raw_fg
    notch_fg = int(ccl_input.sum())
    removal_frac = 1.0 - notch_fg / int(raw_fg.sum())
    if notch_fg < _FG_MIN or removal_frac > 0.80:
        ccl_input = raw_fg.copy()
        notch_applied = False
    else:
        notch_applied = True

    # ── Step 3: Centre-Bias Connected Component Selection ──
    core, rejected = _centre_bias_select(ccl_input, W, H)
    core_count = int(core.sum())

    # ── Strict Foveal Center Lock (Item 1) ─────────────────────────
    # The chosen blob MUST physically intersect the central fovea box
    # (cx ± 0.15*W, cy ± 0.15*H). If the centre crosshair is empty, the
    # detected region is a peripheral table / hand / chair and the system
    # must fail fast — never guess at off-centre blobs.
    cyx, cyy = W // 2, H // 2
    fxh = int(_FOVEAL_LOCK_FRAC * W)   # half-width  (48 px at 320)
    fyh = int(_FOVEAL_LOCK_FRAC * H)   # half-height (38 px at 256)
    foveal_box_x0, foveal_box_y0 = max(0, cyx - fxh), max(0, cyy - fyh)
    foveal_box_x1, foveal_box_y1 = min(W, cyx + fxh), min(H, cyy + fyh)
    if core_count > 0:
        ys_c, xs_c = np.where(core)
        hits = ((xs_c >= foveal_box_x0) & (xs_c < foveal_box_x1) &
                (ys_c >= foveal_box_y0) & (ys_c < foveal_box_y1)).any()
        if not hits:
            return None

    # Texture gate on the SELECTED blob (not the raw ΔY mask): neutral flash
    # glare off a glossy table / white wall is low-S even when — e.g. a dark
    # room — raw ΔY over-selects. Mirrors the firmware's blob-based
    # TABLE_MIN_MEAN_S check, evaluated before the opposite-border gate.
    if core_count >= _FG_MIN:
        _, ss, _ = _hsv(f1[..., 0], f1[..., 1], f1[..., 2])
        if ss[core].mean() < _TBL_S_MIN:
            return None

    if core_count < _FG_MIN:
        return None

    # ── Step 4: Safe Constrained Healing (1-pass dilation inside raw_fg) ──
    struct3 = generate_binary_structure(2, 2)
    healed = binary_dilation(core, structure=struct3, iterations=_HEAL_PX) & raw_fg
    healed = healed & ~rejected
    # Re-select largest connected component after healing
    hlab, hnb = label(healed)
    if hnb > 0:
        hsz = np.bincount(hlab.ravel())[1:]
        healed = hlab == (hsz.argmax()+1)
    if int(healed.sum()) < _FG_MIN:
        healed = core

    # ── Step 5: Shape Fitting & Metrics ──
    solid = _solidity(healed)
    if solid >= _SOLIDITY_CONVEX_THRESH:
        # Convex: 2.5σ covariance ellipse
        final, emaj_px, emin_px, ecx, ecy = _ell_crop(healed, sigma=2.5)
        branch = "convex→ellipse"
    else:
        # Concave: medial axis tube (dilate 1px constrained to raw_fg)
        final = binary_dilation(healed, structure=generate_binary_structure(2, 1)) & raw_fg
        flab, fnb = label(final)
        if fnb > 0:
            fsz = np.bincount(flab.ravel())[1:]
            final = flab == (fsz.argmax()+1)
        # Fit ellipse on the dilated result for volume
        _, emaj_px, emin_px, ecx, ecy = _ell_crop(final, sigma=2.5)
        branch = "concave→tube"

    # ── Step 6: Perimeter Overflow Detection (2% edge buffer) ──
    ovf = _overflow(final, W, H)

    # Opposite-border glare rejection: wall/table flash glare sweeps the whole
    # sensor (a blob touching BOTH Top+Bottom, or BOTH Left+Right, frame edges).
    # No real fruit can span opposing boundaries, so this structurally separates
    # the empty dark-room floods from valid captures (not a threshold).
    if (ovf & (_OVFL_T | _OVFL_B)) == (_OVFL_T | _OVFL_B) or \
       (ovf & (_OVFL_L | _OVFL_R)) == (_OVFL_L | _OVFL_R):
        return None

    # ── Foveal core sampling (Item 2b) ─────────────────────────────
    # Central 70% erosion: one morphological erosion pass with a 1-px diamond
    # discards the mask's outer boundary where background / fingers blend with
    # fruit skin. The interior is the trustworthy hue-report region.
    foveal_core = binary_erosion(
        final, structure=generate_binary_structure(2, 1), iterations=1
    ) if int(final.sum()) >= 9 else np.zeros_like(final)
    if int(foveal_core.sum()) < _FG_MIN:
        # Mask is too thin to erode further without losing it entirely — fall
        # back to the full mask so we still get a hue readout for small fruit.
        foveal_core = final.copy()

    return {
        "mask": final,
        "foveal_core": foveal_core,
        "fv": fv,
        "vmin": vmin,
        "dy_thresh": dy_thresh,
        "solid": solid,
        "emaj_px": emaj_px,
        "emin_px": emin_px,
        "ecx": ecx,
        "ecy": ecy,
        "branch": branch,
        "ovf": ovf,
        "core_count": core_count,
        "notch_applied": notch_applied,
        "removal_frac": removal_frac,
        "t_grad": t_grad,
    }


def run(bd):
    amb = _load(f"{bd}/ambient.jpg")
    f1 = _load(f"{bd}/flash.jpg")
    H, W = _TH, _TW
    s = _segment(amb, f1, W, H)
    if s is None:
        return None

    final, ovf = s["mask"], s["ovf"]
    foveal_core = s.get("foveal_core", final)
    emaj_px, emin_px = s["emaj_px"], s["emin_px"]
    notch_applied, removal_frac, t_grad = s["notch_applied"], s["removal_frac"], s["t_grad"]

    # Volume: ellipse if no overflow, else clamped
    if ovf == 0 and emaj_px > 0 and emin_px > 0:
        vol = _vol_ellipse(min(emaj_px, emin_px), max(emaj_px, emin_px), _CM_PX)
    else:
        vol = _VOL_MAX if ovf != 0 else _vol_bbox(final)

    fg = int(final.sum())
    hist = _hue(final, f1, s["fv"], s["vmin"], foveal_core)

    return {
        "fg": fg,
        "bbox": _bbox_str(final),
        "ovf": ovf,
        "ovf_edges": _overflow_str(ovf),
        "solid": round(s["solid"], 4),
        "emaj": round(emaj_px, 1),
        "emin": round(emin_px, 1),
        "branch": s["branch"],
        "vol": round(vol, 1),
        "dom": int(hist.argmax()),
        "notch_applied": notch_applied,
        "removal_pct": round(removal_frac*100, 1),
        "t_grad": round(t_grad, 1),
    }


def main():
    bundles = sys.argv[1:] or [
        "data/captures/cap_20260904-110101",
        "data/captures/cap_20260904-112221",
        "data/captures/cap_20260904-112332",
        "data/captures/cap_20260904-105652",
    ]
    for bd in bundles:
        name = os.path.basename(bd)
        r = run(bd)
        if r is None:
            print(f"\n{name}: (no fg)\n{'─'*76}")
            continue
        print(f"\n{'='*76}")
        print(f"  {name}")
        print(f"  fg={r['fg']}  bbox={r['bbox']}  ovf={r['ovf']:#06x} ({r['ovf_edges']})")
        print(f"  solidity={r['solid']}  branch={r['branch']}")
        print(f"  ellipse: major={r['emaj']}px  minor={r['emin']}px")
        print(f"  volume={r['vol']} cm³  dominant_bin={r['dom']}")
        print(f"  notch={r['notch_applied']}  removed={r['removal_pct']}%  T_grad={r['t_grad']}")
        print(f"{'─'*76}")


if __name__ == "__main__":
    main()
