#!/usr/bin/env python3
"""Visualize a scan: time-domain waveform, FFT magnitude, and annotated peaks."""

import sys
import json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from scipy.signal import find_peaks

SAMPLE_RATE = 80000
FFT_MAX_FREQ = 10000
PEAK_HEIGHT = 2000
PEAK_DISTANCE_HZ = 50


def load_scan(path):
    taps = np.load(f"{path}/taps.npy")
    with open(f"{path}/metadata.json") as f:
        meta = json.load(f)
    return taps, meta


def compute_fft(taps):
    signal = taps.astype(float) - taps.mean()
    mag = np.abs(np.fft.rfft(signal))
    freqs = np.fft.rfftfreq(len(taps), 1.0 / SAMPLE_RATE)
    mask = freqs <= FFT_MAX_FREQ
    return freqs[mask], mag[mask]


def find_significant_peaks(freqs, mag):
    min_distance = int(PEAK_DISTANCE_HZ / (freqs[1] - freqs[0])) if len(freqs) > 1 else 5
    indices, props = find_peaks(mag, height=PEAK_HEIGHT, distance=max(min_distance, 3), prominence=150)
    sorted_by_mag = np.argsort(mag[indices])[::-1]
    return indices[sorted_by_mag], props["peak_heights"][sorted_by_mag]


def plot_scan(taps, meta, freqs, mag, peak_idx, peak_heights, out_path):
    t_ms = np.arange(len(taps)) / SAMPLE_RATE * 1000.0

    fig, axes = plt.subplots(3, 1, figsize=(14, 10), gridspec_kw={"height_ratios": [1, 1.2, 0.8]})
    fig.suptitle(
        f"Scan {meta.get('id', '?')} — {meta.get('fruit_type', '?')} / {meta.get('label', '?')}",
        fontsize=14, fontweight="bold",
    )

    # --- Time domain ---
    ax = axes[0]
    ax.plot(t_ms, taps, color="#2563eb", linewidth=0.6)
    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("ADC value")
    ax.set_title("Time-domain waveform")
    ax.set_xlim(0, t_ms[-1])
    ax.grid(True, alpha=0.3)

    # --- FFT magnitude ---
    ax = axes[1]
    ax.plot(freqs / 1000.0, mag, color="#7c3aed", linewidth=0.7, label="FFT magnitude")
    if len(peak_idx) > 0:
        ax.plot(
            freqs[peak_idx] / 1000.0, peak_heights,
            "v", color="#dc2626", markersize=8, zorder=5, label=f"Peaks ({len(peak_idx)})"
        )
        for i, (f, h) in enumerate(zip(freqs[peak_idx], peak_heights)):
            ax.annotate(
                f"{f:.0f} Hz",
                (f / 1000.0, h),
                textcoords="offset points",
                xytext=(0, 8),
                fontsize=7,
                ha="center",
                color="#dc2626",
                fontweight="bold",
            )
    ax.set_xlabel("Frequency (kHz)")
    ax.set_ylabel("Magnitude")
    ax.set_title("FFT magnitude")
    # linear scale
    ax.set_xlim(0, FFT_MAX_FREQ / 1000.0)
    ax.legend(loc="upper right")
    ax.grid(True, alpha=0.3, which="both")

    # --- Band energy bar chart ---
    ax = axes[2]
    bands = [
        (0, 100, "Sub-100 Hz"),
        (100, 500, "100–500 Hz"),
        (500, 1000, "0.5–1 kHz"),
        (1000, 5000, "1–5 kHz"),
        (5000, 10000, "5–10 kHz"),
    ]
    total_energy = np.sum(mag[1:] ** 2)
    energies = []
    for lo, hi, _ in bands:
        mask = (freqs >= lo) & (freqs < hi)
        energies.append(np.sum(mag[mask] ** 2) / total_energy * 100 if total_energy > 0 else 0)

    colors = ["#64748b", "#2563eb", "#7c3aed", "#dc2626", "#f59e0b"]
    labels = [f"{name}\n{e:.1f}%" for (_, _, name), e in zip(bands, energies)]
    ax.bar(labels, energies, color=colors, edgecolor="white", linewidth=0.5)
    ax.set_ylabel("Energy (%)")
    ax.set_title("Energy distribution by frequency band")
    ax.set_ylim(0, max(energies) * 1.3 if energies else 100)
    for i, v in enumerate(energies):
        ax.text(i, v + 1, f"{v:.1f}%", ha="center", fontsize=9, fontweight="bold")

    plt.tight_layout()
    plt.savefig(out_path, dpi=150, bbox_inches="tight")
    print(f"Saved to {out_path}")
    plt.close()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <scan_dir> [output.png]")
        sys.exit(1)

    scan_dir = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else f"{scan_dir}/fft_peaks.png"

    taps, meta = load_scan(scan_dir)
    freqs, mag = compute_fft(taps)
    peak_idx, peak_heights = find_significant_peaks(freqs, mag)

    print(f"Scan: {meta.get('id')} — {meta['fruit_type']} / {meta['label']}")
    print(f"Signal: {taps.min()}–{taps.max()} (span {taps.max() - taps.min()})")
    print(f"Peaks found: {len(peak_idx)}")
    for f, h in zip(freqs[peak_idx], peak_heights):
        print(f"  {f:8.1f} Hz  mag={h:.0f}")

    plot_scan(taps, meta, freqs, mag, peak_idx, peak_heights, out)
