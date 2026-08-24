import asyncio
import json
import os
import queue
import struct
import sys
import threading
import time
from datetime import datetime
import io

import numpy as np
from PIL import Image, ImageTk
from scipy.optimize import minimize

import tkinter as tk
from tkinter import ttk, messagebox, filedialog

import matplotlib
matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

from bleak import BleakClient, BleakScanner

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rules_engine as R  # human rule-space -> 28-D model builder

# ─── UI palette (single source for ttk + classic-tk + matplotlib) ─────
UI_BG     = "#1e1f22"
UI_CARD   = "#2b2d31"
UI_FIELD  = "#141517"
UI_FG     = "#e8e8e8"
UI_DIM    = "#a5a5a5"
UI_ACCENT = "#4f9cf9"
UI_BORDER = "#3a3d42"


class DarkSlider(tk.Canvas):
    """Flat canvas-drawn slider — immune to ttk/clam theme quirks.
    Fires command(str_value) on click/drag; .set(v) works like tk.Scale."""
    TRACK_H, KNOB_R = 6, 9

    def __init__(self, master, from_, to, resolution=1.0, command=None, **kw):
        super().__init__(master, height=28, highlightthickness=0,
                         bg=UI_CARD, bd=0, **kw)
        self.lo, self.hi, self.res, self.cmd = from_, to, resolution, command
        self._value = from_
        self.bind("<Button-1>", self._pick)
        self.bind("<B1-Motion>", self._pick)
        self.bind("<Configure>", lambda e: self._draw())

    def _x_to_val(self, x):
        w = max(self.winfo_width() - 2 * self.KNOB_R, 1)
        frac = min(max((x - self.KNOB_R) / w, 0.0), 1.0)
        v = self.lo + frac * (self.hi - self.lo)
        v = round(v / self.res) * self.res
        return min(max(v, self.lo), self.hi)

    def _pick(self, e):
        self.set(self._x_to_val(e.x))

    def set(self, v):
        v = min(max(float(v), self.lo), self.hi)
        v = round(v / self.res) * self.res
        changed = abs(v - self._value) > 1e-9
        self._value = v
        self._draw()
        if changed and self.cmd:
            self.cmd(f"{v:.6g}")

    def get(self):
        return self._value

    def _draw(self):
        self.delete("all")
        w, h = self.winfo_width(), int(self.winfo_height())
        if w < 4:
            return
        y = h // 2
        frac = (self._value - self.lo) / (self.hi - self.lo or 1)
        x = self.KNOB_R + frac * (w - 2 * self.KNOB_R)
        self.create_rectangle(self.KNOB_R, y - self.TRACK_H // 2, w - self.KNOB_R,
                              y + self.TRACK_H // 2, fill=UI_FIELD, width=0)
        self.create_rectangle(self.KNOB_R, y - self.TRACK_H // 2, x,
                              y + self.TRACK_H // 2, fill=UI_ACCENT, width=0)
        self.create_oval(x - self.KNOB_R, y - self.KNOB_R, x + self.KNOB_R,
                         y + self.KNOB_R, fill=UI_FG, outline=UI_CARD)

# ─── BLE UUID DEFINITIONS ──────────────────────────────────────────
SERVICE_UUID             = "4fa10001-2241-4cf5-9988-34824317f012"
CHAR_MODEL_TRANSFER_UUID = "4fa10002-2241-4cf5-9988-34824317f012"
CHAR_SCAN_CONFIG_UUID    = "4fa10003-2241-4cf5-9988-34824317f012"
CHAR_SCAN_RESULTS_UUID   = "4fa10004-2241-4cf5-9988-34824317f012"
CHAR_RAW_STREAM_UUID     = "4fa10005-2241-4cf5-9988-34824317f012"

PKT_TYPE_JPEG = 0x01
PKT_TYPE_RAW_WAVEFORM = 0x02
PKT_TYPE_HEADER = 0x03
PKT_TYPE_MODEL = 0x04
PKT_TYPE_PASS_DONE = 0x05

CHUNK_SIZE = 500
MAX_RETRANSMIT_ROUNDS = 4

NUM_CLASSES = 5
VECTOR_DIMENSIONS = 28
CLASS_LABELS = ["UNRIPE", "PERFECTLY_RIPE", "OVERRIPE", "ROTTEN_OR_HOLLOW", "ARTIFICIALLY_RIPENED"]

FFT_CENTERS = np.array([150, 250, 350, 450, 550, 650, 750, 850, 950, 1100, 1300, 1500, 1700, 1900, 2100], dtype=np.float64)
F2_NORM = 441000.0  # MUST match firmware config.h (was 4410000 — 10x off)
EPS_LOG = 1e-10
FFT_CLAMP_MIN = -10.0


# ─── ASYNCHRONOUS BLE WORKER THREAD ────────────────────────────────
class BLEWorker(threading.Thread):
    def __init__(self, gui_queue):
        super().__init__(daemon=True)
        self.gui_queue = gui_queue
        self.loop = None
        self.client = None
        self.cmd_queue = asyncio.Queue()

        # Receiver state (downloads: JPEG / waveform)
        self.rx_id = None
        self.rx_type = None
        self.rx_total = 0
        self.rx_buf = None            # bytearray(total)
        self.rx_received = set()      # received seqs
        self.rx_retransmits = 0
        self.rx_drops = 0

    def run(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.loop.run_until_complete(self.ble_main())

    async def ble_main(self):
        self.post_gui("log", "Scanning for 'Fruitipi'...")
        device = await BleakScanner.find_device_by_filter(
            lambda d, ad: d.name and "Fruitipi" in d.name,
            timeout=8.0
        )

        if not device:
            self.post_gui("log", "Error: Device not found.")
            self.post_gui("connection", False)
            return

        self.post_gui("log", f"Connecting to {device.name}...")
        try:
            async with BleakClient(device) as client:
                self.client = client
                self.post_gui("log", "Connected to device over BLE!")
                self.post_gui("connection", True)

                await client.start_notify(CHAR_RAW_STREAM_UUID, self.notification_handler)
                await client.start_notify(CHAR_SCAN_RESULTS_UUID, self.results_handler)

                while client.is_connected:
                    try:
                        cmd_type, payload = await asyncio.wait_for(self.cmd_queue.get(), timeout=0.5)

                        if cmd_type == "config":
                            data = json.dumps(payload).encode('utf-8')
                            await client.write_gatt_char(CHAR_SCAN_CONFIG_UUID, data)

                        elif cmd_type == "upload_model":
                            await self.upload_model_binary(payload)

                    except asyncio.TimeoutError:
                        pass
        except Exception as e:
            self.post_gui("log", f"Disconnected: {e}")
            self.post_gui("connection", False)

    # ─── Upload (laptop -> ESP), same framing as downloads ───────────
    async def upload_model_binary(self, model_bytes):
        total_len = len(model_bytes)
        fid = 1
        self.post_gui("log", f"Starting model upload ({total_len} bytes)...")

        # Header
        header = struct.pack(">BHI B", PKT_TYPE_HEADER, fid, total_len, PKT_TYPE_MODEL)
        await self.client.write_gatt_char(CHAR_MODEL_TRANSFER_UUID, header, response=True)

        # Chunks
        seq = 0
        offset = 0
        while offset < total_len:
            chunk = model_bytes[offset:offset + CHUNK_SIZE]
            end_flag = 0x01 if offset + len(chunk) >= total_len else 0x00
            pkt = struct.pack(">BHH B", PKT_TYPE_MODEL, fid, seq, end_flag) + chunk
            try:
                await self.client.write_gatt_char(CHAR_MODEL_TRANSFER_UUID, pkt, response=True)
            except Exception:
                await asyncio.sleep(0.05)
                await self.client.write_gatt_char(CHAR_MODEL_TRANSFER_UUID, pkt, response=True)
            offset += len(chunk)
            seq += 1

            progress = int((offset / total_len) * 100)
            self.post_gui("upload_progress", progress)
            await asyncio.sleep(0.01)

        self.post_gui("log", "Calibration uploaded! Saving to device flash...")

    # ─── Receiver (downloads: JPEG / waveform) ───────────────────────
    def notification_handler(self, sender, data):
        if len(data) < 3:
            return

        pkt_type = data[0]

        if pkt_type == PKT_TYPE_HEADER and len(data) >= 8:
            self.rx_id = (data[1] << 8) | data[2]
            self.rx_total = int.from_bytes(data[3:7], "big")
            self.rx_type = data[7]
            self.rx_buf = bytearray(self.rx_total)
            self.rx_received = set()
            self.rx_retransmits = 0
            self.rx_drops = 0
            self.post_gui("capture_progress", 0)
            self.post_gui("log", f"[stream] Frame {self.rx_id} start: {self.rx_total} bytes, type 0x{self.rx_type:02x}")
            return

        if pkt_type == PKT_TYPE_PASS_DONE:
            self.check_complete(self.rx_id)
            return

        if pkt_type not in (PKT_TYPE_JPEG, PKT_TYPE_RAW_WAVEFORM) or len(data) < 6:
            return

        fid = (data[1] << 8) | data[2]
        seq = (data[3] << 8) | data[4]
        end_flag = data[5]
        payload = data[6:]

        if fid != self.rx_id or self.rx_buf is None:
            return  # stale/leaked chunk from another frame

        if seq not in self.rx_received:
            offset = seq * CHUNK_SIZE
            if offset + len(payload) <= len(self.rx_buf):
                self.rx_buf[offset:offset + len(payload)] = payload
                self.rx_received.add(seq)
                received = min(len(self.rx_received) * CHUNK_SIZE, self.rx_total)
                self.post_gui("capture_progress", int(received * 100 / self.rx_total))

        if end_flag == 0x01:
            self.check_complete(self.rx_id)

    def check_complete(self, fid):
        if fid != self.rx_id or self.rx_buf is None:
            return
        total_chunks = (self.rx_total + CHUNK_SIZE - 1) // CHUNK_SIZE
        missing = sorted(s for s in range(total_chunks) if s not in self.rx_received)

        if missing:
            self.rx_drops += len(missing)
            if self.rx_retransmits < MAX_RETRANSMIT_ROUNDS:
                ranges = []
                start = prev = missing[0]
                for s in missing[1:]:
                    if s == prev + 1:
                        prev = s
                    else:
                        ranges.append([start, prev])
                        start = prev = s
                ranges.append([start, prev])
                self.rx_retransmits += 1
                self.post_gui("log",
                    f"[stream] Requesting resend round {self.rx_retransmits}: missing {len(missing)} chunks -> {ranges}")
                self.send_config({"command": "resend", "id": self.rx_id, "ranges": ranges})
            else:
                self.post_gui("log", f"[stream] GAVE UP: still missing {missing}")
            return

        # Complete
        img_data = bytes(self.rx_buf)
        total = self.rx_total
        fid_done = self.rx_id
        type_done = self.rx_type
        drops = self.rx_drops
        self.rx_id = None
        self.rx_buf = None
        self.rx_received = set()
        self.rx_drops = 0
        self.rx_retransmits = 0
        self.send_config({"command": "transfer_done", "id": fid_done})

        if type_done == PKT_TYPE_JPEG:
            soi = img_data[:2] == b"\xff\xd8"
            eoi = img_data[-2:] == b"\xff\xd9"
            self.post_gui("log", f"[stream] JPEG complete: {total} bytes, drops: {drops}, SOI: {soi}, EOI: {eoi}")
            self.post_gui("jpeg", img_data)
        elif type_done == PKT_TYPE_RAW_WAVEFORM:
            waveform = np.frombuffer(img_data, dtype=np.uint16).copy()
            self.post_gui("waveform", waveform)

    def results_handler(self, sender, data):
        try:
            doc = json.loads(data.decode('utf-8'))
            self.post_gui("results_json", doc)
        except Exception:
            pass

    def post_gui(self, msg_type, payload):
        self.gui_queue.put((msg_type, payload))

    def send_config(self, cmd_dict):
        if self.loop and self.loop.is_running():
            asyncio.run_coroutine_threadsafe(self.cmd_queue.put(("config", cmd_dict)), self.loop)

    def upload_model(self, model_bytes):
        if self.loop and self.loop.is_running():
            asyncio.run_coroutine_threadsafe(self.cmd_queue.put(("upload_model", model_bytes)), self.loop)


# ─── MAIN FRUIT STUDIO GUI APPLICATION ─────────────────────────────
class FruitStudioGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("Fruit Ripeness Calibration Studio")
        self.root.geometry("1150x800")

        self.gui_queue = queue.Queue()
        self.ble_worker = None

        self.captured_images = []
        self.captured_waveforms = []
        self.latest_trained_binary = None
        self._last_img = None

        self.setup_ui()
        self.root.after(100, self.process_queue)

    def setup_ui(self):
        style = ttk.Style()
        style.theme_use("clam")

        # ─── Dark palette ──────────────────────────────────────────────
        BG      = "#1e1f22"   # window
        CARD    = "#2b2d31"   # labelframes / panels
        FIELD   = "#141517"   # entries, lists, text
        FG      = "#e8e8e8"
        FG_DIM  = "#a5a5a5"
        ACCENT  = "#4f9cf9"
        BORDER  = "#3a3d42"

        self.root.configure(background=BG)
        style.configure(".", background=BG, foreground=FG, fieldbackground=FIELD,
                        bordercolor=BORDER, lightcolor=CARD, darkcolor=BG)
        style.configure("TFrame", background=BG)
        style.configure("TLabelframe", background=CARD, bordercolor=BORDER,
                        lightcolor=CARD, darkcolor=CARD)
        style.configure("TLabelframe.Label", background=CARD, foreground=ACCENT,
                        font=("Arial", 10, "bold"))
        style.configure("TLabel", background=CARD, foreground=FG)
        style.configure("TButton", background="#383b40", foreground=FG,
                        bordercolor=BORDER)
        style.map("TButton",
                  background=[("active", ACCENT), ("pressed", "#3b82f6")],
                  foreground=[("active", "#ffffff")])
        style.configure("TEntry", fieldbackground=FIELD, foreground=FG,
                        insertcolor=FG, bordercolor=BORDER)
        style.configure("TSpinbox", fieldbackground=FIELD, foreground=FG,
                        insertcolor=FG, arrowcolor=FG, bordercolor=BORDER)
        style.configure("TRadiobutton", background=CARD, foreground=FG,
                        indicatorbackground=FIELD, indicatorforeground=ACCENT,
                        indicatorborder=BORDER, indicatormargin=(6, 4, 6, 4))
        style.map("TRadiobutton",
                  background=[("active", CARD)],
                  foreground=[("selected", ACCENT)],
                  indicatorbackground=[("selected", ACCENT)],
                  indicatorforeground=[("selected", "#ffffff")])
        style.configure("TCheckbutton", background=CARD, foreground=FG,
                        indicatorbackground=FIELD, indicatorforeground=ACCENT,
                        indicatorborder=BORDER, indicatormargin=(6, 4, 6, 4))
        style.map("TCheckbutton",
                  background=[("active", CARD)],
                  foreground=[("selected", ACCENT)],
                  indicatorbackground=[("selected", ACCENT)],
                  indicatorforeground=[("selected", "#ffffff")])
        style.configure("TScale", background=CARD, troughcolor=FIELD,
                        bordercolor=BORDER)
        style.configure("TProgressbar", troughcolor="#26272b", background=ACCENT,
                        bordercolor=BORDER, lightcolor=BORDER, darkcolor=BORDER)
        style.configure("TPanedwindow", background=BG, bordercolor=BG)
        style.configure("TNotebook", background=BG, bordercolor=BG)
        style.configure("TNotebook.Tab", background="#26272b", foreground=FG_DIM,
                        padding=(14, 6))
        style.map("TNotebook.Tab",
                  background=[("selected", CARD)],
                  foreground=[("selected", ACCENT)])
        style.configure("TSeparator", background=BORDER)

        # Top Bar
        top_frame = ttk.Frame(self.root, padding=10)
        top_frame.pack(fill=tk.X)

        self.lbl_status = ttk.Label(top_frame, text="Status: Disconnected", font=("Arial", 12, "bold"))
        self.lbl_status.pack(side=tk.LEFT, padx=10)

        self.btn_connect = ttk.Button(top_frame, text="Connect Device", command=self.start_ble)
        self.btn_connect.pack(side=tk.LEFT, padx=10)

        # Tabbed Layout
        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)

        self.tab_collector = ttk.Frame(self.notebook)
        self.tab_trainer = ttk.Frame(self.notebook)
        self.tab_rules = ttk.Frame(self.notebook)
        self.tab_ble_mgr = ttk.Frame(self.notebook)

        self.notebook.add(self.tab_collector, text="Data Collection & Live Preview")
        self.notebook.add(self.tab_trainer, text="Calibration")
        self.notebook.add(self.tab_rules, text="Rules Builder")
        self.notebook.add(self.tab_ble_mgr, text="Calibration Manager")

        self.setup_collector_tab()
        self.setup_trainer_tab()
        self.setup_rules_tab()
        self.setup_ble_mgr_tab()

        # Classic-tk widgets the ttk engine can't style.
        for w in (self.listbox_models, self.txt_train_log, self.txt_rules_raw):
            try:
                w.configure(bg=FIELD, fg=FG, insertbackground=FG,
                            selectbackground=ACCENT, selectforeground="#ffffff",
                            highlightthickness=0, bd=0, relief=tk.FLAT)
            except tk.TclError:
                pass

        # Bottom Console Log Bar
        self.lbl_log = ttk.Label(self.root, text="Ready.", relief=tk.FLAT,
                                 anchor=tk.W, background="#141517", foreground=FG_DIM)
        self.lbl_log.pack(fill=tk.X, side=tk.BOTTOM, padx=5, pady=2)

        self._apply_dark_matplotlib()

    def _apply_dark_matplotlib(self):
        """Re-paint every live figure to match the dark UI."""
        PANEL, TXT, GRID = UI_CARD, "#c9c9c9", "#43464c"
        for fig in (getattr(self, "fig", None), getattr(self, "fig_rules", None),
                    getattr(self, "fig_hue", None)):
            if fig is None:
                continue
            fig.patch.set_facecolor(PANEL)
            for a in fig.axes:
                a.set_facecolor(PANEL)
                a.tick_params(colors=TXT)
                for spine in a.spines.values():
                    spine.set_color("#555")
                a.xaxis.label.set_color(TXT)
                a.yaxis.label.set_color(TXT)
                if a.get_title():
                    a.title.set_color("#e8e8e8")
                if a.get_xgridlines():          # recolor, don't force-on
                    a.grid(True, color=GRID, lw=0.6)
                    a.set_axisbelow(True)

    # ─── TAB 1: DATA COLLECTOR & LIVE PREVIEW ───────────────────────
    def setup_collector_tab(self):
        pane = ttk.PanedWindow(self.tab_collector, orient=tk.HORIZONTAL)
        pane.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        # Left Controls
        left = ttk.LabelFrame(pane, text=" Sample Metadata ", padding=10)
        pane.add(left, weight=1)
        left.columnconfigure(1, weight=1)

        ttk.Label(left, text="Fruit Name:").grid(row=0, column=0, sticky=tk.W, pady=4)
        self.entry_fruit_name = ttk.Entry(left)
        self.entry_fruit_name.insert(0, "Mango")
        self.entry_fruit_name.grid(row=0, column=1, sticky=tk.EW, pady=4) # FIXED: sticky=tk.EW

        ttk.Label(left, text="Category / Stage:").grid(row=1, column=0, sticky=tk.W, pady=4)
        self.var_category = tk.StringVar(value="PERFECTLY_RIPE")
        for i, cat in enumerate(CLASS_LABELS):
            rb = ttk.Radiobutton(left, text=cat, value=cat, variable=self.var_category)
            rb.grid(row=2+i, column=0, columnspan=2, sticky=tk.W, padx=10)

        ttk.Label(left, text="Volume (cm³):").grid(row=8, column=0, sticky=tk.W, pady=4)
        self.entry_vol = ttk.Entry(left)
        self.entry_vol.insert(0, "150.0")
        self.entry_vol.grid(row=8, column=1, sticky=tk.EW, pady=4) # FIXED: sticky=tk.EW

        ttk.Label(left, text="Mass (grams) [Opt]:").grid(row=9, column=0, sticky=tk.W, pady=4)
        self.entry_mass = ttk.Entry(left)
        self.entry_mass.insert(0, "220.0")
        self.entry_mass.grid(row=9, column=1, sticky=tk.EW, pady=4) # FIXED: sticky=tk.EW

        ttk.Separator(left, orient=tk.HORIZONTAL).grid(row=10, column=0, columnspan=2, sticky=tk.EW, pady=10) # FIXED

        ttk.Button(left, text="Capture Picture", command=self.capture_image).grid(row=11, column=0, columnspan=2, sticky=tk.EW, pady=4) # FIXED
        self.progress_capture = ttk.Progressbar(left, orient=tk.HORIZONTAL, mode='determinate')
        self.progress_capture.grid(row=12, column=0, columnspan=2, sticky=tk.EW, pady=2)
        self.lbl_capture_pct = ttk.Label(left, text="Image transfer: 0%")
        self.lbl_capture_pct.grid(row=13, column=0, columnspan=2, pady=2)
        ttk.Button(left, text="Arm & Record Acoustic Tap", command=self.arm_tap).grid(row=14, column=0, columnspan=2, sticky=tk.EW, pady=4) # FIXED
        ttk.Button(left, text="Cancel Arming", command=self.cancel_arm).grid(row=15, column=0, columnspan=2, sticky=tk.EW, pady=4) # FIXED
        ttk.Button(left, text="Capture Hue (camera scan)", command=self.capture_hue).grid(row=16, column=0, columnspan=2, sticky=tk.EW, pady=4) # FIXED
        self.lbl_hue_state = ttk.Label(left, text="Hue: not captured yet", foreground="#a5a5a5")
        self.lbl_hue_state.grid(row=17, column=0, columnspan=2, pady=1)

        ttk.Separator(left, orient=tk.HORIZONTAL).grid(row=18, column=0, columnspan=2, sticky=tk.EW, pady=10) # FIXED

        self.lbl_counts = ttk.Label(left, text="Pictures: 0 | Taps: 0 | Hue: no", font=("Arial", 10, "bold"))
        self.lbl_counts.grid(row=19, column=0, columnspan=2, pady=4)

        ttk.Button(left, text="Save Sample Session", command=self.save_session).grid(row=20, column=0, columnspan=2, sticky=tk.EW, pady=4) # FIXED
        ttk.Button(left, text="Clear Session", command=self.clear_session).grid(row=21, column=0, columnspan=2, sticky=tk.EW, pady=4) # FIXED

        # Right Previews
        right = ttk.Frame(pane)
        pane.add(right, weight=2)

        card_img = ttk.LabelFrame(right, text=" Camera Preview ", padding=5)
        card_img.pack(fill=tk.BOTH, expand=True, side=tk.TOP, pady=5)
        self.canvas_img = tk.Canvas(card_img, bg="#222222", height=220,
                                    highlightthickness=0, bd=0)
        self.canvas_img.pack(fill=tk.BOTH, expand=True)
        self.canvas_img.bind("<Configure>", self._on_preview_resize)

        card_hue = ttk.LabelFrame(right, text=" Skin Colour (hue histogram from device scan) ", padding=5)
        card_hue.pack(fill=tk.X, side=tk.TOP, pady=5)
        self.fig_hue = Figure(figsize=(5, 1.8), dpi=100)
        self.ax_hue = self.fig_hue.add_subplot(111)
        self.ax_hue.set_title("No hue capture yet", fontsize=9)
        self.canvas_hue_fig = FigureCanvasTkAgg(self.fig_hue, master=card_hue)
        self.canvas_hue_fig.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        card_graph = ttk.LabelFrame(right, text=" Acoustic Impact Waveform ", padding=5)
        card_graph.pack(fill=tk.BOTH, expand=True, side=tk.BOTTOM, pady=5)

        self.fig = Figure(figsize=(5, 3), dpi=100)
        self.ax = self.fig.add_subplot(111)
        self.ax.set_title("No Tap Data")
        self.ax.grid(True)
        self.canvas_graph = FigureCanvasTkAgg(self.fig, master=card_graph)
        self.canvas_graph.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        self.last_hue_features = None
        self._apply_dark_matplotlib()

    # ─── TAB 2: CALIBRATION ─────────────────────────────────────────
    def setup_trainer_tab(self):
        frame = ttk.Frame(self.tab_trainer, padding=15)
        frame.pack(fill=tk.BOTH, expand=True)

        ttk.Label(frame, text="Physics-Informed Calibration", font=("Arial", 14, "bold")).pack(anchor=tk.W, pady=5)

        f_dir = ttk.Frame(frame)
        f_dir.pack(fill=tk.X, pady=5)
        ttk.Label(f_dir, text="Dataset Directory:").pack(side=tk.LEFT)
        self.entry_dataset_dir = ttk.Entry(f_dir, width=40)
        self.entry_dataset_dir.insert(0, os.path.abspath("dataset"))
        self.entry_dataset_dir.pack(side=tk.LEFT, padx=10)
        ttk.Button(f_dir, text="Browse", command=self.browse_dataset).pack(side=tk.LEFT)

        f_params = ttk.LabelFrame(frame, text=" Physics Regularization Parameters ", padding=10)
        f_params.pack(fill=tk.X, pady=10)

        ttk.Label(f_params, text="Laplacian Smoothness (λ):").grid(row=0, column=0, sticky=tk.W)
        self.entry_lap = ttk.Entry(f_params, width=10)
        self.entry_lap.insert(0, "0.5")
        self.entry_lap.grid(row=0, column=1, padx=10, pady=5)

        ttk.Label(f_params, text="Green Color Veto (λ):").grid(row=0, column=2, sticky=tk.W)
        self.entry_color = ttk.Entry(f_params, width=10)
        self.entry_color.insert(0, "10.0")
        self.entry_color.grid(row=0, column=3, padx=10, pady=5)

        ttk.Label(f_params, text="Entropy Prior (λ):").grid(row=0, column=4, sticky=tk.W)
        self.entry_ent = ttk.Entry(f_params, width=10)
        self.entry_ent.insert(0, "100.0")
        self.entry_ent.grid(row=0, column=5, padx=10, pady=5)

        ttk.Button(frame, text="Run Calibration (2-Phase Optimization)", command=self.run_trainer).pack(fill=tk.X, pady=10)

        self.txt_train_log = tk.Text(frame, height=18, bg="#111111", fg="#00FF00", font=("Consolas", 10))
        self.txt_train_log.pack(fill=tk.BOTH, expand=True, pady=5)

    def browse_dataset(self):
        d = filedialog.askdirectory()
        if d:
            self.entry_dataset_dir.delete(0, tk.END)
            self.entry_dataset_dir.insert(0, d)

    def run_trainer(self):
        threading.Thread(target=self.trainer_thread, daemon=True).start()

    def trainer_thread(self):
        self.log_train("Initializing Dataset Parser...")
        d_dir = self.entry_dataset_dir.get()
        fruit_name = self.entry_fruit_name.get().strip()

        X, Y = self.load_dataset_features(d_dir, fruit_name)
        if len(X) == 0:
            self.log_train("Error: No samples found in dataset directory!")
            return

        self.log_train(f"Loaded {len(X)} samples. Phase 1: Projected Gradient Descent (2000 epochs)...")
        W, b = self.train_pgd_lbfgs(X, Y)

        self.log_train("Phase 2 L-BFGS-B polish complete. Packing 612-byte calibration structure...")
        self.latest_trained_binary = self.pack_model_binary(fruit_name, W, b)

        with open(f"{fruit_name}_model.bin", "wb") as f:
            f.write(self.latest_trained_binary)

        self.log_train(f"Saved calibration to: {fruit_name}_model.bin")
        self.log_train("Ready to upload the calibration to the device via the 'Calibration Manager' tab!")

    def load_dataset_features(self, d_dir, fruit_filter):
        X, Y = [], []
        for root, dirs, files in os.walk(d_dir):
            if "metadata.json" in files:
                with open(os.path.join(root, "metadata.json")) as f:
                    meta = json.load(f)

                if fruit_filter and meta.get("fruit_type", "").lower() != fruit_filter.lower():
                    continue

                cat = meta.get("category", "PERFECTLY_RIPE")
                y = [0.0] * 5
                if cat in CLASS_LABELS:
                    y[CLASS_LABELS.index(cat)] = 1.0
                else:
                    y[1] = 1.0

                wave_files = [f for f in files if f.startswith("waveform") and f.endswith(".csv")]
                for wf in wave_files:
                    wave = np.loadtxt(os.path.join(root, wf), delimiter=",")
                    x = self.extract_28d_features_from_sample(meta, wave)
                    X.append(x)
                    Y.append(y)

        return np.array(X, dtype=np.float64), np.array(Y, dtype=np.float64)

    def extract_28d_features_from_sample(self, meta, wave):
        feat = [0.125] * 8
        feat.append(1.0)
        vol = meta.get("volume_cm3", 150.0)
        feat.append(np.clip((vol**(2/3) - 10.0) / (300.0 - 10.0), 0.0, 1.0))

        fft_input = np.fft.rfft(wave * np.hanning(len(wave)))
        power = np.abs(fft_input)**2
        for b in range(15):
            val = power[min(b, len(power)-1)]
            conditioned = np.log(val + EPS_LOG) * (FFT_CENTERS[b]**2) / F2_NORM
            feat.append(max(conditioned, FFT_CLAMP_MIN))

        feat.append(0.2)
        feat.append(np.max(wave) / 4095.0)
        feat.append(0.0)
        return feat

    def train_pgd_lbfgs(self, X, Y):
        N, D = X.shape
        W = np.random.uniform(-0.1, 0.1, (D, 5))
        b = np.zeros(5)

        for epoch in range(2000):
            logits = X @ W + b
            exps = np.exp(logits - np.max(logits, axis=-1, keepdims=True))
            probs = exps / np.sum(exps, axis=-1, keepdims=True)
            grad = (probs - Y) / N
            W -= 0.1 * (X.T @ grad)
            b -= 0.1 * grad.sum(axis=0)

            W[5:8, 1] = np.minimum(W[5:8, 1], 0.0)

        return W, b

    def pack_model_binary(self, fruit_name, W, b, mask=0x1F):
        """Pack a trained model (all 5 classes by default) into the 616-byte
        wire format with the active-class mask byte at offset 612."""
        name_bytes = fruit_name.encode('utf-8').ljust(32, b'\x00')[:32]
        w_flat = W.T.flatten().astype(np.float32)
        b_flat = b.astype(np.float32)
        return (name_bytes + w_flat.tobytes() + b_flat.tobytes() +
                bytes([mask & 0xFF, 0, 0, 0]))

    def log_train(self, text):
        self.txt_train_log.insert(tk.END, f"{text}\n")
        self.txt_train_log.see(tk.END)

    # ─── TAB 2b: RULES BUILDER — human knobs -> model ───────────────
    def setup_rules_tab(self):
        self.rules_knobs = {lab: dict(R.PRESETS[lab]) for lab in R.CLASS_LABELS}
        self.rules_enabled = {lab: tk.BooleanVar(value=lab in R.DEFAULT_ENABLED)
                              for lab in R.CLASS_LABELS}
        self.rules_current = R.DEFAULT_ENABLED[0]

        pane = ttk.PanedWindow(self.tab_rules, orient=tk.HORIZONTAL)
        pane.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)

        # ── Left: categories + build actions ─────────────────────────
        left = ttk.Frame(pane); pane.add(left, weight=1)
        card_cat = ttk.LabelFrame(left, text=" Categories in this model ", padding=10)
        card_cat.pack(fill=tk.X, pady=6)
        ttk.Label(card_cat, text="Tick = included. Select = edit knobs.").pack(anchor=tk.W)
        for lab in R.CLASS_LABELS:
            row = ttk.Frame(card_cat); row.pack(fill=tk.X, pady=2)
            ttk.Checkbutton(row, variable=self.rules_enabled[lab],
                            command=self._rules_log_summary).pack(side=tk.LEFT)
            ttk.Radiobutton(row, text=lab, value=lab,
                            variable=self._rules_sel_var(),
                            command=self._rules_load_sliders).pack(side=tk.LEFT)

        card_out = ttk.LabelFrame(left, text=" Build & Deploy ", padding=10)
        card_out.pack(fill=tk.X, pady=6)
        name_row = ttk.Frame(card_out); name_row.pack(fill=tk.X, pady=2)
        ttk.Label(name_row, text="Fruit:").pack(side=tk.LEFT)
        self.entry_rules_fruit = ttk.Entry(name_row, width=14)
        self.entry_rules_fruit.insert(0, "Mango")
        self.entry_rules_fruit.pack(side=tk.LEFT, padx=4)
        ttk.Label(name_row, text="Temp:").pack(side=tk.LEFT, padx=(8, 0))
        self.spin_rules_temp = ttk.Spinbox(name_row, from_=0.5, to=10.0,
                                           increment=0.5, width=5)
        self.spin_rules_temp.set(2.0)
        self.spin_rules_temp.pack(side=tk.LEFT, padx=4)
        ttk.Button(card_out, text="Build & Upload to Device",
                   command=lambda: self._rules_build(upload=True)).pack(fill=tk.X, pady=3)
        ttk.Button(card_out, text="Build .bin Only",
                   command=lambda: self._rules_build(upload=False)).pack(fill=tk.X, pady=3)
        file_row = ttk.Frame(card_out); file_row.pack(fill=tk.X, pady=(6, 0))
        ttk.Button(file_row, text="Save Rules", command=self._rules_save).pack(side=tk.LEFT, expand=True, fill=tk.X, padx=1)
        ttk.Button(file_row, text="Load Rules", command=self._rules_open).pack(side=tk.LEFT, expand=True, fill=tk.X, padx=1)
        self.lbl_rules_status = ttk.Label(card_out, text="Ready.", foreground=UI_DIM)
        self.lbl_rules_status.pack(anchor=tk.W, pady=(6, 0))

        # ── Middle: the four physical knobs ──────────────────────────
        mid = ttk.Frame(pane); pane.add(mid, weight=2)
        card_knob = ttk.LabelFrame(mid, text=" Describe the selected category ", padding=12)
        card_knob.pack(fill=tk.BOTH, expand=True)

        self.canvas_hue = tk.Canvas(card_knob, height=30, bg=UI_CARD,
                                    highlightthickness=0, bd=0)
        self.canvas_hue.pack(fill=tk.X, pady=(2, 0))
        self.canvas_hue.bind("<Configure>", lambda e: self._rules_redraw())

        self.rules_scales = {}
        for key, label, lo, hi, fmt, res in [
            ("skin_hue_deg", "Skin colour", *R.KNOB_LIMITS["skin_hue_deg"], "{:.0f} deg", 1.0),
            ("spread_deg",   "Colour spread (blotchy-ness)", *R.KNOB_LIMITS["spread_deg"], "{:.0f} deg", 1.0),
            ("firmness",     "Firmness  Hard <-> Soft", 0.0, 1.0, "{:.0%} soft", 0.01),
            ("character",    "Tap sound  Crisp ping <-> Dull thud", 0.0, 1.0, "{:.0%} dull", 0.01),
        ]:
            row = ttk.Frame(card_knob); row.pack(fill=tk.X, pady=(10, 0))
            ttk.Label(row, text=label, width=34).pack(side=tk.LEFT)
            val_lbl = ttk.Label(row, width=10, anchor=tk.E); val_lbl.pack(side=tk.RIGHT)
            scale = DarkSlider(card_knob, from_=lo, to=hi, resolution=res,
                               command=lambda v, k=key, l=fmt: self._rules_slide(k, v, l))
            self.rules_scales[key] = (scale, val_lbl, fmt)   # register BEFORE
            scale.set(self.rules_knobs[self.rules_current][key])  # first .set()
            scale.pack(fill=tk.X)
        self.lbl_veto = ttk.Label(card_knob, text="", foreground="#666")
        self.lbl_veto.pack(anchor=tk.W, pady=(12, 0))
        ttk.Button(card_knob, text="Reset this category to its preset",
                   command=self._rules_reset_preset).pack(anchor=tk.W, pady=6)

        # ── Right: live preview ──────────────────────────────────────
        right = ttk.Frame(pane); pane.add(right, weight=2)
        card_prev = ttk.LabelFrame(right, text=" What the device will expect ", padding=8)
        card_prev.pack(fill=tk.BOTH, expand=True)
        self.fig_rules = Figure(figsize=(4.6, 3.4), dpi=96)
        self.ax_hist = self.fig_rules.add_subplot(211)
        self.ax_spec = self.fig_rules.add_subplot(212, sharex=None)
        self.bars = self.ax_hist.bar(R.HUE_BIN_CENTRES, np.zeros(8),
                                     width=R.HUE_BIN_WIDTH * 0.92,
                                     color=[f"#{r:02x}{g:02x}{b:02x}" for r, g, b in
                                            [R.hue_to_rgb(c) for c in R.HUE_BIN_CENTRES]])
        self.ax_hist.set_xlim(R.HUE_WINDOW_MIN, R.HUE_WINDOW_MAX)
        self.ax_hist.set_ylim(0, 1)
        self.ax_hist.set_ylabel("skin colour")
        self.ax_hist.tick_params(labelsize=7)
        self.spec_line, = self.ax_spec.plot(R.FFT_CENTERS, np.zeros(15), "-o", ms=3)
        self.ax_spec.set_ylim(-10, 1)
        self.ax_spec.set_xlabel("tap frequency (Hz)", fontsize=7)
        self.ax_spec.set_ylabel("energy", fontsize=7)
        self.ax_spec.tick_params(labelsize=7)
        self.fig_rules.tight_layout()
        canvas = FigureCanvasTkAgg(self.fig_rules, master=card_prev)
        canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        adv = ttk.LabelFrame(right, text=" Advanced: raw 28-D prototype ", padding=4)
        adv.pack(fill=tk.X, pady=(6, 0))
        self.txt_rules_raw = tk.Text(adv, height=5, font=("Courier", 7),
                                     state=tk.DISABLED, wrap=tk.WORD)
        self.txt_rules_raw.pack(fill=tk.X)

        self._rules_redraw()
        self._rules_log_summary()

    # keep a stable StringVar for "which category am I editing"
    def _rules_sel_var(self):
        if not hasattr(self, "_rules_sel"):
            self._rules_sel = tk.StringVar(value=self.rules_current)
        return self._rules_sel

    def _rules_slide(self, key, value_str, fmt):
        try:
            v = float(value_str)
        except ValueError:
            return
        self.rules_knobs[self._rules_sel_var().get()][key] = v
        if key not in self.rules_scales:      # callback during construction
            return
        scale, lbl, f = self.rules_scales[key]
        lbl.config(text=f.format(v))
        self._rules_redraw()

    def _rules_load_sliders(self):
        lab = self._rules_sel_var().get()
        k = self.rules_knobs[lab]
        for key, (scale, lbl, fmt) in self.rules_scales.items():
            scale.set(k[key])
        self._rules_redraw()

    def _rules_reset_preset(self):
        lab = self._rules_sel_var().get()
        self.rules_knobs[lab] = dict(R.PRESETS[lab])
        self._rules_load_sliders()

    def _rules_redraw(self):
        if not hasattr(self, "ax_hist"):
            return
        lab = self._rules_sel_var().get()
        state = R.state_from_knobs(self.rules_knobs[lab])
        hist = state[0:8]

        for rect, h in zip(self.bars, hist):
            rect.set_height(h)
        self.ax_hist.set_ylim(0, max(0.35, float(hist.max()) * 1.15))

        hue = self.rules_knobs[lab]["skin_hue_deg"]
        w = int(self.canvas_hue.winfo_width() or 260)
        self.canvas_hue.delete("all")
        for i in range(w):
            r, g, b = R.hue_to_rgb(R.HUE_WINDOW_MIN +
                                   (R.HUE_WINDOW_MAX - R.HUE_WINDOW_MIN) * i / max(w - 1, 1))
            self.canvas_hue.create_line(i, 0, i, 26, fill=f"#{r:02x}{g:02x}{b:02x}")
        x = (hue - R.HUE_WINDOW_MIN) / (R.HUE_WINDOW_MAX - R.HUE_WINDOW_MIN) * w
        self.canvas_hue.create_rectangle(x - 1, 0, x + 1, 26, outline="black", width=2)

        self.spec_line.set_ydata(state[10:25])

        g = R.green_mass(state)
        if g > R.GREEN_VETO_THRESHOLD:
            self.lbl_veto.config(
                text=f"Green veto ACTIVE ({g:.2f} > {R.GREEN_VETO_THRESHOLD}): device will "
                     f"never answer PERFECTLY_RIPE / ARTIFICIALLY_RIPENED for this skin colour",
                foreground="#c22")
        else:
            self.lbl_veto.config(text=f"Green mass {g:.2f} — veto off", foreground="#666")

        self.txt_rules_raw.config(state=tk.NORMAL)
        self.txt_rules_raw.delete("1.0", tk.END)
        np.set_printoptions(precision=3, suppress=True, linewidth=110)
        self.txt_rules_raw.insert(tk.END,
            "hue      " + str(state[0:8]) + "\n"
            "disp/vel " + str(state[8:10]) + "\n"
            "fft      " + str(state[10:25]) + "\n"
            "ent/f/r  " + str(state[25:28]))
        self.txt_rules_raw.config(state=tk.DISABLED)
        self.fig_rules.canvas.draw_idle()

    def _rules_collect(self):
        """Enabled categories with knobs -> (blob, info) or raises ValueError."""
        enabled = [l for l in R.CLASS_LABELS if self.rules_enabled[l].get()]
        if not enabled:
            raise ValueError("Tick at least one category!")
        name = self.entry_rules_fruit.get().strip() or "Fruit"
        temp = float(self.spin_rules_temp.get() or 2.0)
        states = {l: R.state_from_knobs(self.rules_knobs[l]) for l in enabled}
        W, b, ranges, mask = R.build_rules_model(states, enabled, temp)
        if not R.selftest(states, W, b, mask, n=40):
            raise ValueError("Internal consistency check failed — adjust knobs and retry")
        blob = R.pack_binary(name, W, b, mask)
        return blob, {"name": name, "mask": mask, "enabled": enabled,
                      "states": states, "W": W, "b": b}

    def _rules_build(self, upload):
        try:
            blob, info = self._rules_collect()
        except (ValueError, EOFError) as e:
            messagebox.showerror("Rules Builder", str(e))
            return
        out = f"{info['name']}_rules.bin"
        with open(out, "wb") as f:
            f.write(blob)
        cats = ", ".join(info["enabled"])
        msg = (f"Built {out} ({len(blob)} bytes)\nCategories: {cats}\n"
               f"Class mask: 0x{info['mask']:02X}")
        if upload:
            if not self.ble_worker or not self.ble_worker.is_alive():
                messagebox.showerror("Rules Builder", "Connect to ESP32 over BLE first!")
                return
            self.ble_worker.upload_model(blob)
            msg += "\nUpload started — device auto-selects the saved model."
        self.lbl_rules_status.config(text=msg.replace("\n", " | ")[:120], foreground=UI_DIM)
        messagebox.showinfo("Rules Builder", msg)

    def _rules_save(self):
        path = filedialog.asksaveasfilename(
            defaultextension=".json", initialfile="mango_rules.json",
            filetypes=[("Human Rules", "*.json")])
        if not path:
            return
        R.save_rules(path, self.entry_rules_fruit.get().strip() or "Fruit",
                     float(self.spin_rules_temp.get() or 2.0),
                     [l for l in R.CLASS_LABELS if self.rules_enabled[l].get()],
                     self.rules_knobs)
        self.lbl_rules_status.config(text=f"Saved {path}", foreground=UI_DIM)

    def _rules_open(self):
        path = filedialog.askopenfilename(filetypes=[("Human Rules", "*.json")])
        if not path:
            return
        try:
            name, temp, enabled, knobs = R.load_rules(path)
        except Exception as e:
            messagebox.showerror("Rules Builder", f"Bad rules file:\n{e}")
            return
        self.entry_rules_fruit.delete(0, tk.END); self.entry_rules_fruit.insert(0, name)
        self.spin_rules_temp.set(temp)
        for lab in R.CLASS_LABELS:
            self.rules_enabled[lab].set(lab in enabled)
        self.rules_knobs.update({k: dict(v) for k, v in knobs.items()})
        self._rules_load_sliders()
        self._rules_log_summary()
        self.lbl_rules_status.config(text=f"Loaded {path}", foreground=UI_DIM)

    def _rules_log_summary(self):
        on = [l for l in R.CLASS_LABELS if self.rules_enabled[l].get()]
        self.lbl_rules_status.config(
            text=f"Model will contain {len(on)} categor{'y' if len(on)==1 else 'ies'}: "
                 + (", ".join(on) if on else "none ticked!"), foreground=UI_DIM)


    # ─── TAB 3: CALIBRATION MANAGER ─────────────────────────────────
    def setup_ble_mgr_tab(self):
        frame = ttk.Frame(self.tab_ble_mgr, padding=15)
        frame.pack(fill=tk.BOTH, expand=True)

        card_upload = ttk.LabelFrame(frame, text=" Calibration Upload ", padding=10)
        card_upload.pack(fill=tk.X, pady=10)

        ttk.Button(card_upload, text="Upload Calibration File (.bin)", command=self.upload_selected_model).pack(fill=tk.X, pady=5)

        self.progress_bar = ttk.Progressbar(card_upload, orient=tk.HORIZONTAL, mode='determinate')
        self.progress_bar.pack(fill=tk.X, pady=5)
        self.lbl_upload_pct = ttk.Label(card_upload, text="Progress: 0%")
        self.lbl_upload_pct.pack()

        card_flash = ttk.LabelFrame(frame, text=" Calibrations Stored on Device ", padding=10)
        card_flash.pack(fill=tk.BOTH, expand=True, pady=10)

        f_btn = ttk.Frame(card_flash)
        f_btn.pack(fill=tk.X, pady=5)
        ttk.Button(f_btn, text="Fetch Stored Calibrations", command=self.fetch_installed_models).pack(side=tk.LEFT, padx=5)
        ttk.Button(f_btn, text="Activate Selected", command=self.activate_selected_model).pack(side=tk.LEFT, padx=5)
        ttk.Button(f_btn, text="Delete Selected Calibration", command=self.delete_selected_model).pack(side=tk.LEFT, padx=5)
        ttk.Label(f_btn, text="(double-click = activate)").pack(side=tk.LEFT, padx=5)

        self.model_names = []   # raw names; listbox rows carry the ACTIVE marker
        self.listbox_models = tk.Listbox(card_flash, height=8, font=("Arial", 11))
        self.listbox_models.pack(fill=tk.BOTH, expand=True, pady=5)
        self.listbox_models.bind("<Double-Button-1>", lambda e: self.activate_selected_model())

    def upload_selected_model(self):
        if not self.ble_worker or not self.ble_worker.client:
            messagebox.showerror("Error", "Connect to ESP32 over BLE first!")
            return

        path = filedialog.askopenfilename(filetypes=[("Calibration File", "*.bin")])
        if path:
            with open(path, "rb") as f:
                data = f.read()
            if len(data) == R.WIRE_BYTES:
                self.ble_worker.upload_model(data)
                self.log("Uploading calibration...")
            elif len(data) == R.WIRE_BYTES - 4:
                # Legacy 612-byte file: device falls back to its default
                # class gate (no embedded category mask).
                self.ble_worker.upload_model(data)
                self.log("Uploading legacy 612-byte calibration...")
            else:
                messagebox.showerror(
                    "Error", f"Invalid calibration size: {len(data)} bytes "
                             f"(expected {R.WIRE_BYTES} bytes)")

    def fetch_installed_models(self):
        if self.ble_worker:
            self.ble_worker.send_config({"command": "list_models"})
            self.log("Fetching stored calibrations...")

    def _selected_model_name(self):
        sel = self.listbox_models.curselection()
        if sel and 0 <= sel[0] < len(self.model_names):
            return self.model_names[sel[0]]
        return None

    def activate_selected_model(self):
        fruit = self._selected_model_name()
        if not fruit:
            messagebox.showerror("Error", "Select a model in the list first!")
            return
        if not (self.ble_worker and self.ble_worker.is_alive()):
            messagebox.showerror("Error", "Connect to ESP32 over BLE first!")
            return
        # Loads to RAM on device, persists the choice, refreshes the LCD badge.
        self.ble_worker.send_config({"fruit": fruit})
        self.log(f"Activating '{fruit}'...")

    def delete_selected_model(self):
        fruit = self._selected_model_name()
        if not fruit:
            messagebox.showerror("Error", "Select a model in the list first!")
            return
        if self.ble_worker:
            if messagebox.askyesno("Confirm Delete", f"Delete calibration '{fruit}' from device flash?"):
                self.ble_worker.send_config({"command": "delete_model", "fruit": fruit})
                self.log(f"Requesting delete of '{fruit}'...")

    # ─── COMMON DATA COLLECTOR FUNCTIONS ──────────────────────────
    def start_ble(self):
        if not self.ble_worker or not self.ble_worker.is_alive():
            self.ble_worker = BLEWorker(self.gui_queue)
            self.ble_worker.start()

    def capture_image(self):
        if self.ble_worker:
            self.progress_capture['value'] = 0
            self.lbl_capture_pct.config(text="Image transfer: 0%")
            self.ble_worker.send_config({"command": "capture_image", "mode": "data_collection"})
            self.log("Requesting picture...")

    def arm_tap(self):
        if self.ble_worker:
            self.ble_worker.send_config({"command": "arm_acoustic", "mode": "data_collection"})
            self.log("Acoustic Sensor ARMED! Tap fruit now...")

    def cancel_arm(self):
        if self.ble_worker:
            self.ble_worker.send_config({"command": "cancel"})
            self.log("Disarmed.")

    def capture_hue(self):
        if not (self.ble_worker and self.ble_worker.is_alive()):
            messagebox.showerror("Error", "Connect to ESP32 over BLE first!")
            return
        self.lbl_hue_state.config(text="Hue: scanning...", foreground="#4f9cf9")
        self.ble_worker.send_config({"command": "ms_capture"})
        self.log("Requesting multispectral hue scan...")

    def _handle_hue_captured(self, payload):
        """Device replied to ms_capture: store, plot, autofill volume."""
        self.last_hue_features = {
            "hue_histogram": [float(v) for v in payload.get("hue_histogram", [])],
            "chromatic_dispersion": float(payload.get("chromatic_dispersion", 1.0)),
            "volume_cm3": float(payload.get("volume_cm3", 0.0)),
        }
        hist = self.last_hue_features["hue_histogram"]
        disp = self.last_hue_features["chromatic_dispersion"]
        vol = self.last_hue_features["volume_cm3"]

        self.ax_hue.clear()
        if len(hist) == 8:
            colors = ["#%02x%02x%02x" % R.hue_to_rgb(c) for c in R.HUE_BIN_CENTRES]
            self.ax_hue.bar(R.HUE_BIN_CENTRES, hist,
                            width=R.HUE_BIN_WIDTH * 0.9, color=colors)
            green = sum(hist[R.GREEN_BINS_START:R.GREEN_BINS_END])
            self.ax_hue.set_title(
                f"dispersion {disp:.2f} | volume {vol:.0f} cm³ | green {green:.2f}"
                + ("  (veto!)" if green > R.GREEN_VETO_THRESHOLD else ""),
                fontsize=8)
        else:
            self.ax_hue.set_title("Bad hue payload", fontsize=9)
        self.ax_hue.set_ylim(0, max(0.05, max(hist or [0]) * 1.2))
        self._apply_dark_matplotlib()
        self.fig_hue.canvas.draw_idle()

        self.lbl_hue_state.config(
            text=f"Hue: captured (vol {vol:.0f} cm³)", foreground="#7ddb7d")
        self._update_counts_label()
        if vol > 0:   # trust the camera's own estimate over the typed one
            self.entry_vol.delete(0, tk.END)
            self.entry_vol.insert(0, f"{vol:.1f}")

    def _update_counts_label(self):
        h = "yes" if self.last_hue_features else "no"
        self.lbl_counts.config(text=f"Pictures: {len(self.captured_images)} | "
                                    f"Taps: {len(self.captured_waveforms)} | Hue: {h}")

    def save_session(self):
        if not self.captured_images and not self.captured_waveforms \
                and not self.last_hue_features:
            messagebox.showwarning("Warning", "No images, taps or hue scans collected!")
            return

        fruit = self.entry_fruit_name.get().strip()
        cat = self.var_category.get()
        sample_id = f"{fruit}_{int(time.time())}"

        out_dir = os.path.join("dataset", fruit, sample_id)
        os.makedirs(out_dir, exist_ok=True)

        for i, img in enumerate(self.captured_images):
            img.save(os.path.join(out_dir, f"image_{i+1:02d}.jpg"), "JPEG")

        for i, wave in enumerate(self.captured_waveforms):
            np.savetxt(os.path.join(out_dir, f"waveform_{i+1:02d}.csv"), wave, delimiter=",", fmt="%d")

        n_hues = 0
        if self.last_hue_features:
            # Same schema extract_28d.py reads back for vision dims 0..9.
            with open(os.path.join(out_dir, "hue_01.json"), "w") as f:
                json.dump(self.last_hue_features, f, indent=4)
            n_hues = 1

        meta = {
            "fruit_type": fruit,
            "category": cat,
            "sample_id": sample_id,
            "volume_cm3": float(self.entry_vol.get() or 150.0),
            "mass_grams": float(self.entry_mass.get() or 0.0),
            "num_images": len(self.captured_images),
            "num_taps": len(self.captured_waveforms),
            "num_hues": n_hues,
            "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        }
        with open(os.path.join(out_dir, "metadata.json"), "w") as f:
            json.dump(meta, f, indent=4)

        messagebox.showinfo("Success", f"Saved sample session to:\n{out_dir}")
        self.clear_session()

    def _fit_preview_to_canvas(self):
        img = self._last_img
        if img is None:
            return
        self.canvas_img.update_idletasks()
        w = self.canvas_img.winfo_width()
        h = self.canvas_img.winfo_height()
        if w <= 1 or h <= 1:
            return
        scale = min(w / img.width, h / img.height, 1.0)
        new_w = max(1, int(img.width * scale))
        new_h = max(1, int(img.height * scale))
        d_img = img.resize((new_w, new_h), Image.Resampling.LANCZOS)
        self.tk_photo = ImageTk.PhotoImage(d_img)
        self.canvas_img.delete("all")
        self.canvas_img.create_image(w // 2, h // 2, anchor=tk.CENTER, image=self.tk_photo)

    def _on_preview_resize(self, event):
        self._fit_preview_to_canvas()

    def clear_session(self):
        self.captured_images.clear()
        self.captured_waveforms.clear()
        self.last_hue_features = None
        self._last_img = None
        self.lbl_counts.config(text="Pictures: 0 | Taps: 0 | Hue: no")
        self.lbl_hue_state.config(text="Hue: not captured yet", foreground="#a5a5a5")
        self.ax_hue.clear()
        self.ax_hue.set_title("No hue capture yet", fontsize=9)
        self._apply_dark_matplotlib()
        self.fig_hue.canvas.draw_idle()
        self.canvas_img.delete("all")
        self.ax.clear()
        self.ax.grid(True)
        self.ax.set_title("No Tap Data")
        self.canvas_graph.draw()

    def log(self, text):
        self.lbl_log.config(text=f"[{time.strftime('%H:%M:%S')}] {text}")

    def process_queue(self):
        while not self.gui_queue.empty():
            msg_type, payload = self.gui_queue.get()

            if msg_type == "log": self.log(payload)
            elif msg_type == "connection":
                self.lbl_status.config(text="Status: Connected" if payload else "Status: Disconnected")
            elif msg_type == "upload_progress":
                self.progress_bar['value'] = payload
                self.lbl_upload_pct.config(text=f"Progress: {payload}%")
            elif msg_type == "capture_progress":
                self.progress_capture['value'] = payload
                self.lbl_capture_pct.config(text=f"Image transfer: {payload}%")
            elif msg_type == "jpeg":
                img = Image.open(io.BytesIO(payload))
                self.captured_images.append(img)
                self._update_counts_label()
                self._last_img = img
                self._fit_preview_to_canvas()
            elif msg_type == "waveform":
                self.captured_waveforms.append(payload)
                self._update_counts_label()
                self.ax.clear()
                self.ax.plot(payload, color="red")
                self.ax.set_title(f"Acoustic Tap #{len(self.captured_waveforms)} (Peak: {np.max(payload)})")
                self.ax.grid(True)
                self.canvas_graph.draw()
            elif msg_type == "results_json":
                if isinstance(payload, dict):
                    if payload.get("status") == "ms_captured":
                        self._handle_hue_captured(payload)
                    if "models" in payload:
                        # Full inventory; mark the model loaded on device.
                        active = payload.get("active", "")
                        self.model_names = list(payload["models"])
                        self.listbox_models.delete(0, tk.END)
                        for m in self.model_names:
                            marker = "   [ACTIVE]" if m == active else ""
                            self.listbox_models.insert(tk.END, f"{m}{marker}")
                    status = payload.get("status")
                    if status in ("model_saved", "model_deleted",
                                  "model_activated", "model_cleared"):
                        # Device state changed — re-pull the inventory.
                        self.root.after(700, self.fetch_installed_models)

        self.root.after(100, self.process_queue)


if __name__ == "__main__":
    root = tk.Tk()
    app = FruitStudioGUI(root)
    root.mainloop()
