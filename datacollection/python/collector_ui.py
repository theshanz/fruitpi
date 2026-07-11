"""
collector_ui.py — IRL Data Collection UI for Fruit Quality Scanner.

Collects raw piezo tap data + photos. FFT is computed in Python for
display only. Raw data saved to disk for post-processing.

Usage:
    python collector_ui.py --mock                # Simulated ESP32
    python collector_ui.py --port COM3           # Real ESP32
    python collector_ui.py --mock --rapid        # Fast batch mode
    python collector_ui.py --mock --taps 5       # 5 taps per sample
"""

import argparse
import base64
import csv
import io
import os
import shutil
import tkinter as tk
from collections import defaultdict
from tkinter import ttk, messagebox
from datetime import datetime
from pathlib import Path

import numpy as np
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure
from PIL import Image, ImageTk

from serial_bridge import SerialBridge

FRUIT_TYPES = ["mango", "guava", "papaya"]
STATE_LABELS = [
    "unripe", "ripe", "transitioning", "overripe",
    "rotten_hollow", "artificially_ripened",
]
N_CLASSES = 6
N_SAMPLES = 1024
SAMPLE_RATE = 16000

SAMPLES_DIR = Path(__file__).parent / "samples"
CSV_PATH = Path(__file__).parent / "real_dataset.csv"

CSV_HEADER = (
    ["Timestamp", "Fruit_Type", "Actual_State",
     "Actual_Mass", "Circumference_cm", "Vol_Est",
     "Photo_Paths", "Raw_Tap_Path"]
    + [f"Soft_{i}" for i in range(N_CLASSES)]
)


class CollectorUI:
    IDLE = "IDLE"
    ARMED = "ARMED"
    RECEIVING = "RECEIVING"
    READY = "READY"
    PHOTO = "PHOTO"

    def __init__(self, root, mock=False, port="/dev/ttyUSB0", baud=115200,
                 n_taps=3, rapid=False, keep_fields=False):
        self.root = root
        self.root.title("FruitPi Data Collector")
        self.root.minsize(1100, 680)

        self.state = self.IDLE
        self.sample_count = 0
        self.scan_count = 0
        self.taps_received = 0
        self.raw_taps = []
        self.accepted_photos = []

        self.n_taps = n_taps
        self.rapid = rapid
        self.keep_fields = keep_fields

        self.bridge = SerialBridge(
            on_message=self._on_message, port=port, baud=baud, mock=mock,
            n_taps=n_taps,
        )

        self._build_ui()
        self._set_state(self.IDLE)
        self._refresh_sidebar()
        self._bind_keys()

        if mock:
            self.bridge.connect(fruit=self._get_fruit(), label=self._get_label())

    # ─── Keyboard shortcuts ───────────────────────────────────────

    def _bind_keys(self):
        self.root.bind("<space>", lambda e: self._key_space())
        self.root.bind("<Return>", lambda e: self._key_enter())
        self.root.bind("<Key-p>", lambda e: self._key_photo())
        self.root.bind("<Key-P>", lambda e: self._key_photo())
        self.root.bind("<Key-r>", lambda e: self._key_rapid_toggle())
        self.root.bind("<Key-R>", lambda e: self._key_rapid_toggle())

    def _key_space(self):
        if self.state == self.IDLE:
            self._on_collect()
        elif self.state == self.READY:
            self._on_finish()
        elif self.state == self.PHOTO:
            self._on_finish()

    def _key_enter(self):
        if self.state in (self.READY, self.PHOTO):
            self._on_finish()

    def _key_photo(self):
        if self.state in (self.READY, self.PHOTO):
            self._on_take_photo()

    def _key_rapid_toggle(self):
        self.rapid = not self.rapid
        self.rapid_var.set(self.rapid)
        self._update_rapid_indicator()

    def _update_rapid_indicator(self):
        if self.rapid:
            self.rapid_label.config(text="RAPID ON", foreground="#388E3C")
        else:
            self.rapid_label.config(text="RAPID OFF", foreground="#999")

    # ─── UI Build ─────────────────────────────────────────────────

    def _build_ui(self):
        top = ttk.Frame(self.root, padding=8)
        top.pack(fill=tk.X)

        ttk.Label(top, text="Fruit:").pack(side=tk.LEFT)
        self.fruit_var = tk.StringVar(value="mango")
        fruit_cb = ttk.Combobox(top, textvariable=self.fruit_var,
                                values=FRUIT_TYPES, state="readonly", width=10)
        fruit_cb.pack(side=tk.LEFT, padx=(2, 12))

        ttk.Label(top, text="Label:").pack(side=tk.LEFT)
        self.label_var = tk.StringVar(value="unripe")
        label_cb = ttk.Combobox(top, textvariable=self.label_var,
                                values=STATE_LABELS, state="readonly", width=20)
        label_cb.pack(side=tk.LEFT, padx=(2, 12))

        self.rapid_var = tk.BooleanVar(value=self.rapid)
        self.rapid_check = ttk.Checkbutton(
            top, text="Rapid", variable=self.rapid_var,
            command=self._on_rapid_toggle,
        )
        self.rapid_check.pack(side=tk.LEFT, padx=(16, 4))
        self.rapid_label = ttk.Label(top, text="", font=("monospace", 9, "bold"))
        self.rapid_label.pack(side=tk.LEFT, padx=(0, 4))
        self._update_rapid_indicator()

        ttk.Label(top, text=f"({self.n_taps} taps)", font=("monospace", 9)
                  ).pack(side=tk.LEFT, padx=(8, 0))

        self.tap_indicator = ttk.Label(top, text="", font=("monospace", 10))
        self.tap_indicator.pack(side=tk.RIGHT)

        # ── Main area ──
        main = ttk.Frame(self.root)
        main.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        self._build_sidebar(main)

        right = ttk.Frame(main)
        right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self.fig = Figure(figsize=(8, 3.5), dpi=100)
        self.fig.set_facecolor("#f0f0f0")
        self.ax_photo = self.fig.add_subplot(121)
        self.ax_fft = self.fig.add_subplot(122)
        self.fig.subplots_adjust(left=0.05, bottom=0.25, right=0.98,
                                 top=0.90, wspace=0.30)
        self.canvas = FigureCanvasTkAgg(self.fig, master=right)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        self.ax_photo.set_title("Photo")
        self.ax_photo.set_xticks([])
        self.ax_photo.set_yticks([])
        self.ax_photo.text(0.5, 0.5, "No photo yet", ha="center", va="center",
                           transform=self.ax_photo.transAxes, color="#999",
                           fontsize=11)
        self._init_fft_line()

        # ── Measurements ──
        meas = ttk.Frame(self.root, padding=8)
        meas.pack(fill=tk.X)

        ttk.Label(meas, text="Mass (g):").pack(side=tk.LEFT)
        self.mass_var = tk.StringVar(value="")
        ttk.Entry(meas, textvariable=self.mass_var, width=8).pack(
            side=tk.LEFT, padx=(4, 16))
        ttk.Label(meas, text="Circumference (cm):").pack(side=tk.LEFT)
        self.circ_var = tk.StringVar(value="")
        ttk.Entry(meas, textvariable=self.circ_var, width=8).pack(
            side=tk.LEFT, padx=(4, 16))
        self.vol_label = ttk.Label(meas, text="Vol: —")
        self.vol_label.pack(side=tk.LEFT, padx=(8, 0))
        self.circ_var.trace_add("write", self._update_vol_preview)

        # ── Buttons ──
        bottom = ttk.Frame(self.root, padding=8)
        bottom.pack(fill=tk.X)

        self.btn_collect = ttk.Button(bottom, text="Collect Sample [Space]",
                                      command=self._on_collect)
        self.btn_collect.pack(side=tk.LEFT, padx=(0, 8))
        self.btn_photo = ttk.Button(bottom, text="Take Photo [P]",
                                    command=self._on_take_photo)
        self.btn_accept = ttk.Button(bottom, text="Accept",
                                     command=self._on_accept_photo)
        self.btn_retake = ttk.Button(bottom, text="Retake",
                                     command=self._on_take_photo)
        self.btn_another = ttk.Button(bottom, text="Another Photo",
                                      command=self._on_another_photo)
        self.btn_finish = ttk.Button(bottom, text="Finish [Enter]",
                                     command=self._on_finish)

        self.sample_label = ttk.Label(bottom, text="Samples: 0")
        self.sample_label.pack(side=tk.RIGHT)
        self.status_label = ttk.Label(bottom, text="",
                                      font=("monospace", 11, "bold"))
        self.status_label.pack(side=tk.RIGHT, padx=(0, 16))

        # ── Shortcut hint ──
        hint = ttk.Label(bottom, text="Keys: Space=Collect/Finish  P=Photo  R=Rapid",
                         font=("monospace", 8), foreground="#999")
        hint.pack(side=tk.RIGHT, padx=(0, 16))

    # ─── Sidebar ───────────────────────────────────────────────────

    def _build_sidebar(self, parent):
        sidebar = ttk.Frame(parent, width=240)
        sidebar.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 4))
        sidebar.pack_propagate(False)

        # ── Batch counters ──
        counter_frame = ttk.LabelFrame(sidebar, text="Batch Count", padding=4)
        counter_frame.pack(fill=tk.X, padx=4, pady=(0, 4))

        self.count_labels = {}
        for r, fruit in enumerate(FRUIT_TYPES):
            ttk.Label(counter_frame, text=fruit[:3].upper(),
                      font=("monospace", 8, "bold")).grid(
                          row=r, column=0, sticky=tk.W, padx=(0, 4))
            for c, label in enumerate(STATE_LABELS):
                lbl = ttk.Label(counter_frame, text="0",
                                font=("monospace", 8), width=3,
                                anchor=tk.CENTER)
                lbl.grid(row=r, column=c + 1, padx=1, pady=1)
                self.count_labels[(fruit, label)] = lbl
                if r == 0:
                    ttk.Label(counter_frame, text=label[:4],
                              font=("monospace", 7), foreground="#666"
                              ).grid(row=len(FRUIT_TYPES), column=c + 1,
                                     padx=1)

        # ── Sample list ──
        ttk.Label(sidebar, text="Collected", font=("monospace", 10, "bold")
                  ).pack(anchor=tk.W, padx=4, pady=(4, 2))

        list_frame = ttk.Frame(sidebar)
        list_frame.pack(fill=tk.BOTH, expand=True)

        self.sample_list = tk.Listbox(
            list_frame, font=("monospace", 9), activestyle="none",
            selectbackground="#90CAF9", selectforeground="#000",
        )
        scrollbar = ttk.Scrollbar(list_frame, orient=tk.VERTICAL,
                                  command=self.sample_list.yview)
        self.sample_list.config(yscrollcommand=scrollbar.set)
        self.sample_list.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.sample_list.bind("<<ListboxSelect>>", self._on_select_sample)

        info_frame = ttk.LabelFrame(sidebar, text="Info", padding=4)
        info_frame.pack(fill=tk.X, pady=(4, 0))

        self.info_text = tk.Text(
            info_frame, height=5, font=("monospace", 8),
            state=tk.DISABLED, wrap=tk.WORD, bg="#f8f8f8",
        )
        self.info_text.pack(fill=tk.X)

        btn_frame = ttk.Frame(sidebar)
        btn_frame.pack(fill=tk.X, pady=(4, 0))

        ttk.Button(btn_frame, text="Delete",
                   command=self._on_delete_sample).pack(
                       side=tk.LEFT, fill=tk.X, expand=True)

    def _refresh_sidebar(self):
        self.sample_list.delete(0, tk.END)
        self.scan_dirs = []

        counts = defaultdict(lambda: defaultdict(int))

        if CSV_PATH.exists():
            with open(CSV_PATH) as f:
                reader = csv.DictReader(f)
                for row in reader:
                    tag = Path(row.get("Raw_Tap_Path", "")).parent.name
                    fruit = row.get("Fruit_Type", "?")
                    label = row.get("Actual_State", "?")
                    mass = row.get("Actual_Mass", "?")
                    display = f"{tag} | {fruit} | {label} | {mass}g"
                    self.sample_list.insert(tk.END, display)
                    self.scan_dirs.append({
                        "dir": Path(row.get("Raw_Tap_Path", "")).parent,
                        "row": row,
                    })
                    counts[fruit][label] += 1

        for fruit in FRUIT_TYPES:
            for label in STATE_LABELS:
                n = counts[fruit][label]
                key = (fruit, label)
                if key in self.count_labels:
                    self.count_labels[key].config(
                        text=str(n),
                        foreground="#388E3C" if n > 0 else "#999",
                    )

        self.sample_count = self.sample_list.size()
        self.sample_label.config(text=f"Samples: {self.sample_count}")

    def _on_select_sample(self, event):
        sel = self.sample_list.curselection()
        if not sel:
            return
        idx = sel[0]
        if idx >= len(self.scan_dirs):
            return

        info = self.scan_dirs[idx]
        row = info["row"]
        scan_dir = info["dir"]

        taps_path = scan_dir / "taps.npy"
        n_taps = 0
        if taps_path.exists():
            arr = np.load(taps_path)
            n_taps = arr.shape[0] if arr.ndim == 2 else 0

        n_photos = len(list(scan_dir.glob("photo_*.jpg"))) if scan_dir.exists() else 0

        lines = [
            f"Scan: {scan_dir.name}",
            f"Fruit: {row.get('Fruit_Type', '?')}",
            f"Label: {row.get('Actual_State', '?')}",
            f"Mass: {row.get('Actual_Mass', '?')} g",
            f"Circ: {row.get('Circumference_cm', '?')} cm",
            f"Taps: {n_taps}  |  Photos: {n_photos}",
        ]

        self.info_text.config(state=tk.NORMAL)
        self.info_text.delete("1.0", tk.END)
        self.info_text.insert(tk.END, "\n".join(lines))
        self.info_text.config(state=tk.DISABLED)

    def _on_delete_sample(self):
        sel = self.sample_list.curselection()
        if not sel:
            return
        idx = sel[0]
        if idx >= len(self.scan_dirs):
            return

        info = self.scan_dirs[idx]
        scan_dir = info["dir"]

        if not messagebox.askyesno("Delete Sample",
                                   f"Delete {scan_dir.name}?\n\nThis cannot be undone."):
            return

        if scan_dir.exists():
            shutil.rmtree(scan_dir)

        self._remove_csv_row(info["row"])
        self._refresh_sidebar()
        self._clear_info()

    def _remove_csv_row(self, target_row):
        if not CSV_PATH.exists():
            return
        rows = []
        with open(CSV_PATH) as f:
            reader = csv.DictReader(f)
            fieldnames = reader.fieldnames
            for row in reader:
                if row.get("Raw_Tap_Path") != target_row.get("Raw_Tap_Path"):
                    rows.append(row)

        with open(CSV_PATH, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(rows)

    def _clear_info(self):
        self.info_text.config(state=tk.NORMAL)
        self.info_text.delete("1.0", tk.END)
        self.info_text.config(state=tk.DISABLED)

    # ─── Plots ─────────────────────────────────────────────────────

    def _init_fft_line(self):
        self.ax_fft.clear()
        self.ax_fft.set_title("FFT Spectrum (display only)")
        x = np.arange(N_SAMPLES // 2)
        self.fft_line, = self.ax_fft.plot(
            x, np.zeros(N_SAMPLES // 2), color="#2196F3", linewidth=0.8,
        )
        self.ax_fft.set_xlabel("Frequency (Hz)")
        self.ax_fft.set_ylabel("Magnitude")
        self.ax_fft.set_xlim(0, 4000)
        self.ax_fft.set_ylim(0, 1)
        self.ax_fft.grid(alpha=0.3)

    def _compute_fft_display(self):
        if not self.raw_taps:
            return
        all_mag = []
        for tap in self.raw_taps:
            samples = np.array(tap, dtype=np.float32)
            samples -= np.mean(samples)
            windowed = samples * np.hanning(len(samples))
            fft = np.fft.rfft(windowed)
            mag = np.abs(fft) / N_SAMPLES
            all_mag.append(mag)
        avg_mag = np.mean(all_mag, axis=0)
        freqs = np.fft.rfftfreq(N_SAMPLES, 1.0 / SAMPLE_RATE)
        mask = freqs <= 4000
        self.fft_line.set_data(freqs[mask], avg_mag[mask])
        ymax = max(np.max(avg_mag[mask]) * 1.3, 0.01)
        self.ax_fft.set_ylim(0, ymax)
        self.canvas.draw_idle()

    # ─── State machine ─────────────────────────────────────────────

    def _set_state(self, new_state):
        self.state = new_state
        for w in (self.btn_photo, self.btn_accept, self.btn_retake,
                  self.btn_another, self.btn_finish):
            w.pack_forget()

        if new_state == self.IDLE:
            self.btn_collect.pack(side=tk.LEFT, padx=(0, 8))
            self.btn_collect.config(state=tk.NORMAL)
            self._set_status("IDLE — Space to collect", "#666")
            self.taps_received = 0
            self.tap_indicator.config(text="")
            self.accepted_photos = []
            self.raw_taps = []

        elif new_state == self.ARMED:
            self.btn_collect.config(state=tk.DISABLED)
            self._set_status(f"ARMED — tap the fruit {self.n_taps}x...",
                             "#F57C00")

        elif new_state == self.RECEIVING:
            self._set_status(
                f"TAP {self.taps_received}/{self.n_taps}", "#1976D2")

        elif new_state == self.READY:
            self.btn_collect.pack(side=tk.LEFT, padx=(0, 8))
            self.btn_collect.config(state=tk.NORMAL)
            self.btn_photo.pack(side=tk.LEFT, padx=(0, 8))
            self.btn_finish.pack(side=tk.LEFT, padx=(0, 8))
            self._set_status(
                f"{len(self.raw_taps)}/{self.n_taps} taps — Enter to finish",
                "#388E3C")

        elif new_state == self.PHOTO:
            self.btn_collect.pack(side=tk.LEFT, padx=(0, 8))
            self.btn_collect.config(state=tk.NORMAL)
            self.btn_photo.pack(side=tk.LEFT, padx=(0, 8))
            self.btn_accept.pack(side=tk.LEFT, padx=(0, 4))
            self.btn_retake.pack(side=tk.LEFT, padx=(0, 4))
            self.btn_another.pack(side=tk.LEFT, padx=(0, 8))
            self.btn_finish.pack(side=tk.LEFT, padx=(0, 8))
            n = len(self.accepted_photos)
            self._set_status(
                f"Photo captured ({n} accepted) — Enter to finish",
                "#7B1FA2")

        self.canvas.draw_idle()

    def _set_status(self, text, color="#333"):
        self.status_label.config(text=f"● {text}", foreground=color)

    def _get_fruit(self):
        return self.fruit_var.get()

    def _get_label(self):
        return self.label_var.get()

    # ─── Rapid mode ───────────────────────────────────────────────

    def _on_rapid_toggle(self):
        self.rapid = self.rapid_var.get()
        self._update_rapid_indicator()

    # ─── Button callbacks ──────────────────────────────────────────

    def _on_collect(self):
        if not self.bridge.is_connected():
            self.bridge.connect(fruit=self._get_fruit(), label=self._get_label())
        self.taps_received = 0
        self.raw_taps = []
        self.accepted_photos = []
        self.ax_photo.clear()
        self.ax_photo.set_title("Photo")
        self.ax_photo.set_xticks([])
        self.ax_photo.set_yticks([])
        self.ax_photo.text(0.5, 0.5, "Waiting for taps...", ha="center",
                           va="center", transform=self.ax_photo.transAxes,
                           color="#999", fontsize=11)
        self._init_fft_line()
        self._set_state(self.ARMED)
        self.bridge.send("ARM")

    def _on_take_photo(self):
        self.bridge.send("CAPTURE")

    def _on_accept_photo(self):
        self._set_state(self.READY)

    def _on_another_photo(self):
        self._set_state(self.READY)

    def _on_finish(self):
        mass_str = self.mass_var.get().strip()
        circ_str = self.circ_var.get().strip()

        if not self.raw_taps:
            messagebox.showwarning("Missing Data", "No tap data collected yet.")
            return
        if not mass_str:
            messagebox.showwarning("Missing Data", "Enter mass in grams.")
            return
        if not circ_str:
            messagebox.showwarning("Missing Data", "Enter circumference in cm.")
            return

        try:
            mass_g = float(mass_str)
        except ValueError:
            messagebox.showwarning("Invalid", "Mass must be a number.")
            return
        try:
            circ_cm = float(circ_str)
        except ValueError:
            messagebox.showwarning("Invalid", "Circumference must be a number.")
            return

        vol_est = (mass_g ** (2.0 / 3.0) - 10.0) / (300.0 - 10.0)
        vol_est = max(0.0, min(1.0, vol_est))

        label = self._get_label()
        soft = [0.0] * N_CLASSES
        soft[STATE_LABELS.index(label)] = 1.0

        self.scan_count += 1
        scan_dir = SAMPLES_DIR / f"scan_{self.scan_count:03d}"
        scan_dir.mkdir(parents=True, exist_ok=True)

        photo_paths = []
        for idx, jpg_bytes in enumerate(self.accepted_photos):
            p = scan_dir / f"photo_{idx}.jpg"
            p.write_bytes(jpg_bytes)
            photo_paths.append(str(p))

        taps_array = np.array(self.raw_taps, dtype=np.float32)
        raw_path = str(scan_dir / "taps.npy")
        np.save(raw_path, taps_array)

        row = {
            "Timestamp": int(datetime.now().timestamp()),
            "Fruit_Type": self._get_fruit(),
            "Actual_State": label,
            "Actual_Mass": round(mass_g, 1),
            "Circumference_cm": round(circ_cm, 1),
            "Vol_Est": round(vol_est, 6),
            "Photo_Paths": ";".join(photo_paths),
            "Raw_Tap_Path": raw_path,
        }
        for i in range(N_CLASSES):
            row[f"Soft_{i}"] = round(soft[i], 4)

        write_header = not CSV_PATH.exists() or CSV_PATH.stat().st_size == 0
        with open(CSV_PATH, "a", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=CSV_HEADER)
            if write_header:
                writer.writeheader()
            writer.writerow(row)

        if not self.keep_fields:
            self.mass_var.set("")
            self.circ_var.set("")
            self.vol_label.config(text="Vol: —")

        self._set_status(f"Saved scan_{self.scan_count:03d}!", "#388E3C")
        self._refresh_sidebar()

        if self.rapid:
            self.root.after(600, self._on_collect)
        else:
            self.root.after(1500, lambda: self._set_state(self.IDLE))

    # ─── ESP32 message handler ─────────────────────────────────────

    def _on_message(self, obj):
        self.root.after(0, self._handle_msg, obj)

    def _handle_msg(self, obj):
        if "tap" in obj and "samples" in obj:
            self.taps_received = obj["tap"]
            self.raw_taps.append(obj["samples"])
            indicators = " ".join(
                "●" if i < self.taps_received else "○"
                for i in range(self.n_taps)
            )
            self.tap_indicator.config(text=f"Taps: {indicators}")
            self._compute_fft_display()
            self._set_state(self.RECEIVING)

        elif "status" in obj and obj["status"] == "done":
            self._set_state(self.READY)

        elif "photo" in obj:
            jpg_bytes = base64.b64decode(obj["photo"])
            self.accepted_photos.append(jpg_bytes)
            img = Image.open(io.BytesIO(jpg_bytes))
            img.thumbnail((400, 300))
            arr = np.array(img)
            self.ax_photo.clear()
            self.ax_photo.imshow(arr)
            self.ax_photo.set_title(
                f"Photo ({len(self.accepted_photos)} accepted)")
            self.ax_photo.set_xticks([])
            self.ax_photo.set_yticks([])
            self.canvas.draw_idle()

            if self.rapid:
                self._set_state(self.PHOTO)
                self.root.after(300, self._on_finish)
            else:
                self._set_state(self.PHOTO)

    # ─── Helpers ───────────────────────────────────────────────────

    def _update_vol_preview(self, *_args):
        circ_str = self.circ_var.get().strip()
        if not circ_str:
            self.vol_label.config(text="Vol: —")
            return
        try:
            c = float(circ_str)
            v = (c ** 2) / 15.19
            self.vol_label.config(text=f"Vol^(2/3): {v:.2f}")
        except ValueError:
            self.vol_label.config(text="Vol: ?")

    def close(self):
        self.bridge.disconnect()
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description="FruitPi Data Collector")
    parser.add_argument("--mock", action="store_true",
                        help="Use simulated ESP32 (no hardware)")
    parser.add_argument("--port", default="/dev/ttyUSB0",
                        help="Serial port (default: /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=115200,
                        help="Baud rate (default: 115200)")
    parser.add_argument("--taps", type=int, default=3,
                        help="Number of taps per sample (default: 3)")
    parser.add_argument("--rapid", action="store_true",
                        help="Rapid mode: auto-rearm after each sample")
    parser.add_argument("--keep-fields", action="store_true",
                        help="Keep mass/circ values between samples")
    args = parser.parse_args()

    root = tk.Tk()
    app = CollectorUI(root, mock=args.mock, port=args.port, baud=args.baud,
                      n_taps=args.taps, rapid=args.rapid,
                      keep_fields=args.keep_fields)
    root.protocol("WM_DELETE_WINDOW", app.close)
    root.mainloop()


if __name__ == "__main__":
    main()
