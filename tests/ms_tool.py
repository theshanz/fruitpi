#!/usr/bin/env python3
"""BLE tool for the multispectral subsystem (ms_capture / ms_debug / ms_config).

Usage:
    .venv/bin/python tests/ms_tool.py capture [--wait 25]
    .venv/bin/python tests/ms_tool.py debug [--outdir /tmp] [--wait 60]
    .venv/bin/python tests/ms_tool.py config --gain-r 1.5 --gain-g 1.0 --gain-b 0.8 [--aec 60]
"""
import argparse
import asyncio
import json
import os
import sys
import time

from bleak import BleakClient, BleakScanner
from PIL import Image, ImageStat

import numpy as np

from final_pipeline import _segment, _vol_ellipse, _vol_bbox, _VOL_MAX, foveal_center_box_hue

SERVICE_UUID = "4fa10001-2241-4cf5-9988-34824317f012"
CHAR_SCAN_CONFIG_UUID = "4fa10003-2241-4cf5-9988-34824317f012"
CHAR_SCAN_RESULTS_UUID = "4fa10004-2241-4cf5-9988-34824317f012"
CHAR_RAW_STREAM_UUID = "4fa10005-2241-4cf5-9988-34824317f012"

PKT_HEADER = 0x03
PKT_PASS_DONE = 0x05


def mean_rgb_jpeg(path):
    im = Image.open(path).convert("RGB")
    stat = ImageStat.Stat(im)
    r, g, b = stat.mean
    s = r + g + b or 1
    return r, g, b, r / s, g / s, b / s


class RxAssembler:
    """Reassembles sequential begin_transfer() streams from the raw-stream char."""

    def __init__(self):
        self.transfers = {}  # id -> {type, data, chunks, seen, done}

    def feed(self, data: bytearray):
        if not data:
            return None
        d = data
        if len(d) >= 8 and d[0] == PKT_HEADER:
            tid = (d[1] << 8) | d[2]
            total = (d[3] << 24) | (d[4] << 16) | (d[5] << 8) | d[6]
            ptype = d[7]
            chunks = (total + 499) // 500
            self.transfers[tid] = {
                "type": ptype,
                "data": bytearray(),
                "chunks": chunks,
                "seen": set(),
                "done": False,
            }
            return None
        if len(d) >= 3 and d[0] == PKT_PASS_DONE:
            tid = (d[1] << 8) | d[2]
            t = self.transfers.get(tid)
            if t:
                t["done"] = True
            return ("pass_done", tid)
        if len(d) >= 6:
            ptype = d[0]
            tid = (d[1] << 8) | d[2]
            seq = (d[3] << 8) | d[4]
            end = d[5] == 0x01
            payload = d[6:]
            t = self.transfers.get(tid)
            if t is None or seq in t["seen"]:
                return None
            t["seen"].add(seq)
            offset = seq * 500
            if offset + len(payload) <= len(t["data"]):
                t["data"][offset:offset + len(payload)] = payload
            else:
                t["data"] += b"\x00" * (offset + len(payload) - len(t["data"]))
                t["data"][offset:offset + len(payload)] = payload
            if end:
                t["done"] = True
                return ("transfer", tid, bytes(t["data"]), t["type"])
        return None


async def connect(name, scan_timeout):
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: d.name and name.lower() in d.name.lower(),
        timeout=scan_timeout,
    )
    if device is None:
        raise SystemExit(f"device '{name}' not found")
    print(f"[*] found {device.name} ({device.address})")
    return BleakClient(device)


async def cmd_capture(args):
    async with await connect(args.name, args.scan_timeout) as client:
        results = []

        def on_results(_s, data):
            text = data.decode("utf-8", "replace").strip()
            if text.startswith("{"):
                results.append(text)

        await client.start_notify(CHAR_SCAN_RESULTS_UUID, on_results)
        await client.write_gatt_char(
            CHAR_SCAN_CONFIG_UUID, b'{"command": "ms_capture"}', response=True)
        print("[*] ms_capture sent")
        await asyncio.sleep(args.wait)
        await client.stop_notify(CHAR_SCAN_RESULTS_UUID)
        for r in results:
            print(r)


def bar_8(vals):
    chars = " ▁▂▃▄▅▆▇█"
    out = []
    for v in vals:
        idx = int(round(v * (len(chars) - 1)))
        out.append(chars[max(0, min(idx, len(chars) - 1))])
    return "".join(out)


def summarize_results(results):
    """Compact one-line readout for the last capture JSON in `results`."""
    doc = None
    for r in results:
        try:
            doc = json.loads(r)
        except Exception:
            continue
    if not doc:
        return "[*] no capture JSON received yet"
    hist = doc.get("hue_histogram", [0.0] * _HUE_NUM_BINS)
    ratios = doc.get("rgb_ratios", [0.0] * 3)
    dom = max(range(len(hist)), key=lambda i: hist[i])
    bins = " ".join(f"{v:.3f}" for v in hist)
    return (f"[*] {time.strftime('%H:%M:%S')} bins[{bins}] dom=bin{dom}({hist[dom]:.3f}) "
            f"rat=({ratios[0]:.3f},{ratios[1]:.3f},{ratios[2]:.3f}) "
            f"disp={doc.get('chromatic_dispersion', 0):.3f} vol={doc.get('volume_cm3', 0):.0f} "
            f"|{bar_8(hist)}|")


async def cmd_poll(args):
    async with await connect(args.name, args.scan_timeout) as client:
        results = []

        def on_results(_s, data):
            text = data.decode("utf-8", "replace").strip()
            if text.startswith("{"):
                results.append(text)

        await client.start_notify(CHAR_SCAN_RESULTS_UUID, on_results)
        print("[*] polling ms_capture; move the sensor across color surfaces. Ctrl-C to stop.")
        t0 = time.time()
        count = 0
        while args.count is None or count < args.count:
            results.clear()
            await client.write_gatt_char(
                CHAR_SCAN_CONFIG_UUID, b'{"command": "ms_capture"}', response=True)
            deadline = time.time() + args.timeout
            while time.time() < deadline and not results:
                await asyncio.sleep(0.2)
            print(summarize_results(results))
            count += 1
            await asyncio.sleep(args.interval)
        await client.stop_notify(CHAR_SCAN_RESULTS_UUID)


async def cmd_debug(args):
    async with await connect(args.name, args.scan_timeout) as client:
        os.makedirs(args.outdir, exist_ok=True)
        assembler = RxAssembler()
        received = []

        def on_raw(_s, data):
            evt = assembler.feed(data)
            if evt and evt[0] == "transfer":
                received.append(evt)

        await client.start_notify(CHAR_RAW_STREAM_UUID, on_raw)
        await client.write_gatt_char(
            CHAR_SCAN_CONFIG_UUID, b'{"command": "ms_debug"}', response=True)
        print("[*] ms_debug sent")
        t0 = time.time()
        while len(received) < 4 and time.time() - t0 < args.wait:
            await asyncio.sleep(0.2)
        await client.stop_notify(CHAR_RAW_STREAM_UUID)

        if len(received) < 4:
            print(f"[!] only {len(received)}/4 JPEGs received")
        names = ["ch0_ambient.jpg", "ch1_red.jpg", "ch2_green.jpg", "ch3_blue.jpg"]
        for i, (_, tid, blob, ptype) in enumerate(received):
            path = os.path.join(args.outdir, names[i])
            with open(path, "wb") as f:
                f.write(blob)
            r, g, b, rr, gg, bb = mean_rgb_jpeg(path)
            print(f"[*] {names[i]} ({ptype:#x} id={tid}): {len(blob)} B "
                  f"mean RGB=({r:.1f},{g:.1f},{b:.1f}) ratios=({rr:.3f},{gg:.3f},{bb:.3f}) -> {path}")


# ─── Local (host-side) rebuild of the unified 5-layer burst pipeline ─────────
# The firmware now streams ONLY the three raw JPEGs + a tiny timing meta; every
# segmentation / colour feature is recomputed here (numpy) so the host is the
# single source of truth. Mirrors esp/main/extract_hues.cpp process_unified_burst
# with the same constants (see esp/main/config.h).
_MS_DECODE_SCALE = 2                 # firmware decodes JPEGs at 1/4 (TJpgDec)
_CM_PER_PIXEL_FULL = 0.05
_HUE_WINDOW_MIN, _HUE_WINDOW_MAX = 20.0, 155.0
_HUE_NUM_BINS = 10
_PARALLAX_BLOCK, _PARALLAX_WIN = 16, 3
_PARALLAX_MIN_MOTION, _PARALLAX_GLOBAL_HANDHELD = 1.0, 0.40
_MS_HUE_RAW_S_MIN = 0.30       # raw (pre-WB) saturation floor for hue-report pixels (firmware HUE_RAW_S_MIN)
_SKIN_HUE_MIN_DEG, _SKIN_HUE_MAX_DEG, _SKIN_SAT_MAX = 15.0, 32.0, 0.50
_CM_PER_PIXEL = _CM_PER_PIXEL_FULL * (1 << _MS_DECODE_SCALE)  # 0.2 cm/px


def _load_quarter_rgb(path, target_w, target_h):
    """Open a JPEG and return (h, w, 3) uint8 RGB downscaled to target size.

    Mirrors the firmware's TJpgDec 1/4 downscale (MS_DECODE_SCALE=2): a native
    1280×1024 frame becomes 320×256, the resolution the burst pipeline runs on.
    """
    im = Image.open(path).convert("RGB")
    im = im.resize((target_w, target_h), Image.BILINEAR)
    return np.asarray(im, dtype=np.uint8), target_w, target_h


def _rgb_to_hsv(r, g, b):
    """Vectorized RGB888 (uint8 arrays) → H[0,120], S[0,1], V[0,1] (OpenCV-ish)."""
    r = r.astype(np.float64) / 255.0
    g = g.astype(np.float64) / 255.0
    b = b.astype(np.float64) / 255.0
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    d = mx - mn
    h = np.zeros_like(mx)
    with np.errstate(divide="ignore", invalid="ignore"):
        h = np.where(d == 0, 0.0, h)
        h = np.where((mx == r) & (d > 0), 60 * (((g - b) / d) % 6), h)
        h = np.where((mx == g) & (d > 0), 60 * (((b - r) / d) + 2), h)
        h = np.where((mx == b) & (d > 0), 60 * (((r - g) / d) + 4), h)
    h = np.where(h < 0, h + 360, h)
    s = np.where(mx == 0, 0.0, d / mx)
    v = mx
    return h, s, v


def _compute_parallax_grid_np(f1, f2, block=16, win=3):
    """Parallax SAD macroblock grid (faithful host port of compute_parallax_grid).

    f1/f2 are (H, W, 3) uint8. Returns (global_px, moving_blocks, total_blocks,
    interior_mag). Each interior 16×16 block searches a ±3 px (full-res) window
    on the SECOND flash frame, SAD sampled every 2 px per axis on per-channel
    |ΔR|+|ΔG|+|ΔB|; the best-match (dx,dy) magnitude is the block motion. Blocks
    at the border are excluded, matching the firmware.
    """
    H, W, _ = f1.shape
    bx_count = W // block
    by_count = H // block
    f1i = f1.astype(np.int32)
    f2i = f2.astype(np.int32)
    # 2×-subsampled samples within each block (as the firmware step-2 loop)
    rows = np.arange(0, block, 2)          # 0,2,...,14
    cols = np.arange(0, block, 2)
    interior_mag = np.zeros((by_count - 2, bx_count - 2), dtype=np.float64)
    for by in range(1, by_count - 1):
        for bx in range(1, bx_count - 1):
            ox, oy = bx * block, by * block
            # source samples (shape 8 x 8 x 3)
            src = f1i[oy + rows][:, cols + ox]                     # (8,8,3)
            best_sad = np.inf
            bdx = bdy = 0
            for dy in range(-win, win + 1):
                ty = oy + rows + dy
                for dx in range(-win, win + 1):
                    tx = ox + cols + dx
                    if ty[0] < 0 or ty[-1] >= H or tx[0] < 0 or tx[-1] >= W:
                        continue
                    tgt = f2i[ty[:, None], tx[None, :]]            # (8,8,3)
                    sad = np.abs(src - tgt).sum()
                    if sad < best_sad:
                        best_sad = sad
                        bdx, bdy = dx, dy
            interior_mag[by - 1, bx - 1] = np.hypot(bdx, bdy)
    global_px = float(interior_mag.mean())
    moving = int((interior_mag >= _PARALLAX_MIN_MOTION).sum())
    return global_px, moving, bx_count * by_count, interior_mag


def compute_local_report(ambient_path, flash_path, flash2_path,
                         target_w, target_h, cm_per_pixel=_CM_PER_PIXEL):
    """Rebuild the full ms_debug_diff report from the three raw JPEGs on-host.

    Segmentation is delegated to final_pipeline._segment (the single source of
    truth), so the mask, core, bbox, ellipse, overflow and the opposite-border
    glare rejection are 1:1 with the canonical pipeline by construction. Only
    the parallax-motion and colour features are computed here.
    """
    amb, _, _ = _load_quarter_rgb(ambient_path, target_w, target_h)
    f1, _, _ = _load_quarter_rgb(flash_path, target_w, target_h)
    f2, _, _ = _load_quarter_rgb(flash2_path, target_w, target_h)
    H, W = target_h, target_w

    # Layer 2: parallax motion + adaptive global classifier (report feature).
    global_px, moving_blocks, total_blocks, interior_mag = _compute_parallax_grid_np(f1, f2)
    handheld = global_px >= _PARALLAX_GLOBAL_HANDHELD

    # Layer 1+3: per-channel max(F1,F2) `shadow-heal` fusion (colour features).
    fr = np.maximum(f1, f2).astype(np.int32)

    # Canonical segmentation: mask, core, ellipse, overflow + glare rejection.
    s = _segment(amb, f1, W, H)
    if s is None:
        return {
            "w": W, "h": H, "fg_pixels": 0, "core_pixels": 0, "snap_pixels": 0,
            "bbox": {"x": 0, "y": 0, "w": 0, "h": 0},
            "motion": {"global_px": global_px, "handheld": handheld,
                       "moving_blocks": moving_blocks, "total_blocks": total_blocks},
            "volume_cm3": 0.0, "chromatic_dispersion": 1.0,
            "ellipse": {"major_px": 0.0, "minor_px": 0.0, "cx": 0.0, "cy": 0.0},
            "overflow_mask": 0,
            "hue_histogram": [0.0] * _HUE_NUM_BINS, "raw_rgb_means": [0.0, 0.0, 0.0],
            "ambient_rgb_means": [0.0, 0.0, 0.0], "rgb_ratios": [0.0, 0.0, 0.0],
            "cm_per_pixel": cm_per_pixel,
        }

    final = s["mask"]
    fg_count = int(final.sum())
    core_count = s["core_count"]
    snap_count = 0

    # Layer 6: bbox from the canonical mask (tight fruit extent).
    ys, xs = np.where(final)
    if fg_count:
        min_x, max_x = int(xs.min()), int(xs.max())
        min_y, max_y = int(ys.min()), int(ys.max())
        bbox = {"x": min_x, "y": min_y,
                "w": max_x - min_x + 1, "h": max_y - min_y + 1}
    else:
        bbox = {"x": 0, "y": 0, "w": 0, "h": 0}

    # Volume from the canonical ellipse when the fruit is fully in view; an
    # overflowing mask is a lower bound (mirrors final_pipeline.run()).
    emaj, emin, overflow = s["emaj_px"], s["emin_px"], s["ovf"]
    if overflow == 0 and emaj > 1.0 and emin > 1.0:
        volume = _vol_ellipse(min(emaj, emin), max(emaj, emin), cm_per_pixel)
    else:
        volume = _VOL_MAX if overflow != 0 else _vol_bbox(final)
    ellipse = {"major_px": round(emaj, 1), "minor_px": round(emin, 1),
               "cx": round(s["ecx"], 1), "cy": round(s["ecy"], 1)}

    # Layer 5 hue extraction on the fused flash inside the canonical mask.
    # No artificial WB B-scale — the sensor's AWB balances the flash naturally
    # (mirrors process_frame / the one-shot path). The keep floor is the
    # canonical seed-adaptive V minimum plus a low-saturation cut + skin band.
    fr_wb = fr.astype(np.float64).copy()
    hsv_h, hsv_s, hsv_v = _rgb_to_hsv(fr_wb[..., 0], fr_wb[..., 1], fr_wb[..., 2])
    # Raw (pre-WB) saturation gate on hue-report pixels: washed-out neutral cast
    # regions (~S 0.24) otherwise read as green after the B WB-scale; real fruit
    # sits at ~S 0.5. Mirrors final_pipeline._hue and firmware HUE_RAW_S_MIN.
    _, raw_s, _ = _rgb_to_hsv(fr[..., 0], fr[..., 1], fr[..., 2])
    keep = final & (hsv_v >= s["vmin"]) & (hsv_s >= 0.15) & (raw_s >= _MS_HUE_RAW_S_MIN)
    keep = keep & ~((hsv_h >= _SKIN_HUE_MIN_DEG) & (hsv_h <= _SKIN_HUE_MAX_DEG) &
                    (hsv_s < _SKIN_SAT_MAX))
    keep_idx = np.where(keep)
    fg_color_pixels = int(keep.sum())
    bin_width = (_HUE_WINDOW_MAX - _HUE_WINDOW_MIN) / _HUE_NUM_BINS
    raw_counts = np.zeros(_HUE_NUM_BINS, dtype=np.int64)
    if fg_color_pixels:
        h = hsv_h[keep_idx]
        bin_idx = np.clip(
            ((h - _HUE_WINDOW_MIN) / bin_width).astype(np.int64), 0, _HUE_NUM_BINS - 1
        )
        within = (h >= _HUE_WINDOW_MIN) & (h <= _HUE_WINDOW_MAX)
        valid_hue = within.sum()
        if valid_hue > 0:
            hist = np.bincount(bin_idx[within], minlength=_HUE_NUM_BINS).astype(np.float64)
            chroma = hist / valid_hue
            dispersion = _chromatic_dispersion(chroma)
        else:
            chroma = np.full(_HUE_NUM_BINS, 1.0 / _HUE_NUM_BINS)
            dispersion = 1.0
        # mean RGB of mask pixels (WB'd flash, matching firmware reflectance means)
        mr = float(fr_wb[keep_idx][:, 0].mean())
        mg = float(fr_wb[keep_idx][:, 1].mean())
        mb = float(fr_wb[keep_idx][:, 2].mean())
        ar = float(amb[keep_idx][:, 0].mean())
        ag = float(amb[keep_idx][:, 1].mean())
        ab = float(amb[keep_idx][:, 2].mean())
        ssum = mr + mg + mb or 1.0
        ratios = [mr / ssum, mg / ssum, mb / ssum]
    else:
        chroma = np.full(_HUE_NUM_BINS, 1.0 / _HUE_NUM_BINS)
        dispersion = 1.0
        mr = mg = mb = ar = ag = ab = 0.0
        ratios = [0.0, 0.0, 0.0]

    return {
        "w": W, "h": H, "fg_pixels": fg_count, "core_pixels": core_count,
        "snap_pixels": snap_count, "bbox": bbox,
        "motion": {"global_px": global_px, "handheld": handheld,
                   "moving_blocks": moving_blocks, "total_blocks": total_blocks},
        "volume_cm3": round(volume, 3), "chromatic_dispersion": round(dispersion, 3),
        "ellipse": ellipse, "overflow_mask": overflow,
        "hue_histogram": [round(float(x), 4) for x in chroma],
        "raw_rgb_means": [round(mr, 1), round(mg, 1), round(mb, 1)],
        "ambient_rgb_means": [round(ar, 1), round(ag, 1), round(ab, 1)],
        "rgb_ratios": [round(float(x), 3) for x in ratios],
        "cm_per_pixel": cm_per_pixel,
    }


def _chromatic_dispersion(chroma):
    """∑ over bin pairs (i<j) of bin[i]*bin[j]*dist(bin_i, bin_j); normalised to [0,1]."""
    n = len(chroma)
    total = 0.0
    for i in range(n):
        for j in range(i + 1, n):
            total += chroma[i] * chroma[j] * (j - i)
    return min(1.0, total)


async def cmd_debug_diff(args):
    """ms_debug_diff: raw 3-frame burst.

    The firmware streams ONLY the three raw JPEGs (ambient / flash1 / flash2)
    plus a tiny timing meta notify. The host recomputes every segmentation and
    colour feature from those JPEGs (numpy port of process_unified_burst), so
    the report is produced locally and is the single source of truth. Everything
    is saved into a timestamped bundle under --outdir.
    """
    async with await connect(args.name, args.scan_timeout) as client:
        os.makedirs(args.outdir, exist_ok=True)
        session = os.path.join(
            args.outdir, "cap_" + time.strftime("%Y%m%d-%H%M%S"))
        os.makedirs(session, exist_ok=True)
        assembler = RxAssembler()
        received = []
        meta = None

        def on_results(_s, data):
            nonlocal meta
            text = data.decode("utf-8", "replace").strip()
            if text.startswith("{"):
                try:
                    doc = json.loads(text)
                except Exception:
                    return
                if doc.get("status") == "ms_debug_diff":
                    meta = doc
                else:
                    print(f"[notify] {text}")

        def on_raw(_s, data):
            evt = assembler.feed(data)
            if evt and evt[0] == "transfer":
                received.append(evt)

        await client.start_notify(CHAR_SCAN_RESULTS_UUID, on_results)
        await client.start_notify(CHAR_RAW_STREAM_UUID, on_raw)

        # BLE transfers of three ~50KB JPEGs can drop a stream; auto-retry the
        # capture a few times so a flaky connection self-heals instead of wasting
        # the whole wait on a partial bundle.
        attempts = 0
        conn_ok = False
        while attempts < args.retries and not conn_ok:
            attempts += 1
            received.clear()
            meta = None
            await client.write_gatt_char(
                CHAR_SCAN_CONFIG_UUID, b'{"command": "ms_debug_diff"}', response=True)
            print(f"[*] ms_debug_diff sent (attempt {attempts}/{args.retries})")
            t0 = time.time()
            while time.time() - t0 < args.wait:
                if meta is not None and len(received) >= 3:
                    break
                await asyncio.sleep(0.2)
            conn_ok = (meta is not None and len(received) >= 3)
            if not conn_ok:
                print(f"[!] attempt {attempts} incomplete "
                      f"(meta={'yes' if meta else 'no'}, received={len(received)}/3)")
                await asyncio.sleep(1.0)
        await client.stop_notify(CHAR_SCAN_RESULTS_UUID)
        await client.stop_notify(CHAR_RAW_STREAM_UUID)

        if len(received) < 3:
            print(f"[!] only {len(received)}/3 JPEGs received")

        names = ["ambient.jpg", "flash.jpg", "flash2.jpg"]
        jpeg_stats = {}
        for i, (_, tid, blob, ptype) in enumerate(received[:3]):
            path = os.path.join(session, names[i])
            with open(path, "wb") as f:
                f.write(blob)
            try:
                r, g, b, rr, gg, bb = mean_rgb_jpeg(path)
                stats = f"mean RGB=({r:.1f},{g:.1f},{b:.1f}) ratios=({rr:.3f},{gg:.3f},{bb:.3f})"
                jpeg_stats[names[i]] = [r, g, b, rr, gg, bb]
            except Exception:
                stats = "not a decodable JPEG?"
            print(f"[*] {names[i]} ({ptype:#x} id={tid}): {len(blob)} B {stats} -> {path}")

        # Rebuild the full feature report locally from the raw JPEGs. The meta
        # provides only device-side timing + decoded frame dimensions.
        w = int(meta.get("w", 0)) if meta else 0
        h = int(meta.get("h", 0)) if meta else 0
        ambient_p, flash_p, flash2_p = (
            os.path.join(session, "ambient.jpg"),
            os.path.join(session, "flash.jpg"),
            os.path.join(session, "flash2.jpg"),
        )
        if all(os.path.exists(p) for p in (ambient_p, flash_p, flash2_p)) and w > 0 and h > 0:
            report = compute_local_report(ambient_p, flash_p, flash2_p, w, h)
        else:
            report = None

        if report is not None and meta:
            report.setdefault("timing_ms", {}).update(meta.get("timing_ms", {}))

        if report is None:
            print("[!] no capture/meta received; no report.json written")
        else:
            b = report.get("bbox", {})
            t = report.get("timing_ms", {})
            m = report.get("motion", {})
            print(f"[*] report: {report.get('w')}x{report.get('h')} "
                  f"fg={report.get('fg_pixels')}px "
                  f"(core={report.get('core_pixels')} snap={report.get('snap_pixels')}) "
                  f"bbox=({b.get('x')},{b.get('y')},{b.get('w')}x{b.get('h')})")
            print(f"[*] motion: glob={m.get('global_px')}px "
                  f"{'HANDHELD' if m.get('handheld') else 'BENCH'} "
                  f"({m.get('moving_blocks')}/{m.get('total_blocks')} blocks moving)")
            print(f"[*] timing: lock={t.get('lock')} amb={t.get('ambient_grab')} "
                  f"f1={t.get('flash_grab')} f2={t.get('flash2_grab')} "
                  f"decode={t.get('decode')} process={t.get('process')} total={t.get('total')} ms")
            disp = report.get("chromatic_dispersion")
            print(f"[*] vol={report.get('volume_cm3')} "
                  f"disp={f'{disp:.3f}' if isinstance(disp, (int, float)) else disp} "
                  f"rat=({[round(x,3) for x in report.get('rgb_ratios',[])]})")

        report_path = os.path.join(session, "report.json")
        with open(report_path, "w") as f:
            f.write(json.dumps(report, indent=2) if report is not None else "null")
        if report is not None:
            print(f"[*] bundle saved -> {session} (report.json, ambient.jpg, flash.jpg, flash2.jpg)")
            print(f"[*] full JSON: {json.dumps(report)}")
        else:
            print(f"[!] frames saved to {session} but no report.json (report missing)")


async def cmd_config(args):
    async with await connect(args.name, args.scan_timeout) as client:
        doc = {
            "command": "ms_config",
            "gain_r": args.gain_r,
            "gain_g": args.gain_g,
            "gain_b": args.gain_b,
        }
        if args.aec is not None:
            doc["aec"] = args.aec
        if args.ambient is not None:
            doc["ambient"] = args.ambient == "true"
        payload = json.dumps(doc).encode("utf-8")
        await client.write_gatt_char(CHAR_SCAN_CONFIG_UUID, payload, response=True)
        print(f"[*] sent: {payload.decode()}")
        await asyncio.sleep(1.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--name", default="Fruitipi")
    ap.add_argument("--scan-timeout", type=float, default=10)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("capture")
    p.add_argument("--wait", type=float, default=25)

    p = sub.add_parser("poll")
    p.add_argument("--interval", type=float, default=2.0,
                   help="seconds between captures")
    p.add_argument("--timeout", type=float, default=8.0,
                   help="seconds to wait per capture for a result")
    p.add_argument("--count", type=int, default=None,
                   help="number of captures (default: infinite until Ctrl-C)")

    p = sub.add_parser("debug")
    p.add_argument("--outdir", default="/tmp")
    p.add_argument("--wait", type=float, default=90)

    p = sub.add_parser("debug-diff")
    p.add_argument("--outdir", default="/tmp")
    p.add_argument("--wait", type=float, default=60,
                   help="seconds to wait for report + all 3 JPEGs")
    p.add_argument("--retries", type=int, default=3,
                   help="auto-retry the capture if a JPEG stream is dropped")

    p = sub.add_parser("config")
    p.add_argument("--gain-r", type=float, required=True)
    p.add_argument("--gain-g", type=float, required=True)
    p.add_argument("--gain-b", type=float, required=True)
    p.add_argument("--aec", type=int, default=None)
    p.add_argument("--ambient", type=str, choices=["true", "false"], default=None)

    args = ap.parse_args()
    if args.cmd == "capture":
        asyncio.run(cmd_capture(args))
    elif args.cmd == "debug":
        asyncio.run(cmd_debug(args))
    elif args.cmd == "debug-diff":
        asyncio.run(cmd_debug_diff(args))
    elif args.cmd == "poll":
        asyncio.run(cmd_poll(args))
    else:
        asyncio.run(cmd_config(args))


if __name__ == "__main__":
    main()
