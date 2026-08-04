import asyncio
import json
import os
import queue
import struct
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
F2_NORM = 4410000.0
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
        self.tab_ble_mgr = ttk.Frame(self.notebook)

        self.notebook.add(self.tab_collector, text="Data Collection & Live Preview")
        self.notebook.add(self.tab_trainer, text="Calibration")
        self.notebook.add(self.tab_ble_mgr, text="Calibration Manager")

        self.setup_collector_tab()
        self.setup_trainer_tab()
        self.setup_ble_mgr_tab()

        # Bottom Console Log Bar
        self.lbl_log = ttk.Label(self.root, text="Ready.", relief=tk.SUNKEN, anchor=tk.W)
        self.lbl_log.pack(fill=tk.X, side=tk.BOTTOM, padx=5, pady=2)

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
        ttk.Label(left, text="Trigger Threshold:").grid(row=14, column=0, sticky=tk.W, pady=4)
        self.var_threshold = tk.DoubleVar(value=0.02)
        self.spin_threshold = ttk.Spinbox(left, from_=0.01, to=0.60, increment=0.01, textvariable=self.var_threshold, width=7)
        self.spin_threshold.grid(row=14, column=1, sticky=tk.EW, pady=4)
        ttk.Button(left, text="Apply", command=self.apply_threshold).grid(row=14, column=2, sticky=tk.EW, pady=4)
        ttk.Button(left, text="Arm & Record Acoustic Tap", command=self.arm_tap).grid(row=15, column=0, columnspan=2, sticky=tk.EW, pady=4) # FIXED
        ttk.Button(left, text="Cancel Arming", command=self.cancel_arm).grid(row=16, column=0, columnspan=2, sticky=tk.EW, pady=4) # FIXED

        ttk.Separator(left, orient=tk.HORIZONTAL).grid(row=17, column=0, columnspan=2, sticky=tk.EW, pady=10) # FIXED

        self.lbl_counts = ttk.Label(left, text="Pictures: 0 | Taps: 0", font=("Arial", 10, "bold"))
        self.lbl_counts.grid(row=18, column=0, columnspan=2, pady=4)

        ttk.Button(left, text="Save Sample Session", command=self.save_session).grid(row=19, column=0, columnspan=2, sticky=tk.EW, pady=4) # FIXED
        ttk.Button(left, text="Clear Session", command=self.clear_session).grid(row=20, column=0, columnspan=2, sticky=tk.EW, pady=4) # FIXED

        # Right Previews
        right = ttk.Frame(pane)
        pane.add(right, weight=2)

        card_img = ttk.LabelFrame(right, text=" Camera Preview ", padding=5)
        card_img.pack(fill=tk.BOTH, expand=True, side=tk.TOP, pady=5)
        self.canvas_img = tk.Canvas(card_img, bg="#222222", height=220)
        self.canvas_img.pack(fill=tk.BOTH, expand=True)
        self.canvas_img.bind("<Configure>", self._on_preview_resize)

        card_graph = ttk.LabelFrame(right, text=" Acoustic Impact Waveform ", padding=5)
        card_graph.pack(fill=tk.BOTH, expand=True, side=tk.BOTTOM, pady=5)

        self.fig = Figure(figsize=(5, 3), dpi=100)
        self.ax = self.fig.add_subplot(111)
        self.ax.set_title("No Tap Data")
        self.ax.grid(True)
        self.canvas_graph = FigureCanvasTkAgg(self.fig, master=card_graph)
        self.canvas_graph.get_tk_widget().pack(fill=tk.BOTH, expand=True)

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

    def pack_model_binary(self, fruit_name, W, b):
        name_bytes = fruit_name.encode('utf-8').ljust(32, b'\x00')[:32]
        w_flat = W.T.flatten().astype(np.float32)
        b_flat = b.astype(np.float32)
        return name_bytes + w_flat.tobytes() + b_flat.tobytes()

    def log_train(self, text):
        self.txt_train_log.insert(tk.END, f"{text}\n")
        self.txt_train_log.see(tk.END)

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
        ttk.Button(f_btn, text="Delete Selected Calibration", command=self.delete_selected_model).pack(side=tk.LEFT, padx=5)

        self.listbox_models = tk.Listbox(card_flash, height=8, font=("Arial", 11))
        self.listbox_models.pack(fill=tk.BOTH, expand=True, pady=5)

    def upload_selected_model(self):
        if not self.ble_worker or not self.ble_worker.client:
            messagebox.showerror("Error", "Connect to ESP32 over BLE first!")
            return

        path = filedialog.askopenfilename(filetypes=[("Calibration File", "*.bin")])
        if path:
            with open(path, "rb") as f:
                data = f.read()
            if len(data) == 612:
                self.ble_worker.upload_model(data)
                self.log("Uploading calibration...")
            else:
                messagebox.showerror("Error", f"Invalid calibration size: {len(data)} bytes (expected 612 bytes)")

    def fetch_installed_models(self):
        if self.ble_worker:
            self.ble_worker.send_config({"command": "list_models"})
            self.log("Fetching stored calibrations...")

    def delete_selected_model(self):
        sel = self.listbox_models.curselection()
        if sel and self.ble_worker:
            fruit = self.listbox_models.get(sel[0])
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

    def get_threshold(self):
        try:
            thr = float(self.var_threshold.get())
        except Exception:
            thr = 0.15
        return min(0.60, max(0.01, thr))

    def apply_threshold(self):
        if self.ble_worker:
            thr = self.get_threshold()
            self.ble_worker.send_config({"command": "set_threshold", "threshold": thr})
            self.log(f"Trigger threshold set to {thr:.2f}")

    def arm_tap(self):
        if self.ble_worker:
            thr = self.get_threshold()
            self.ble_worker.send_config({"command": "set_threshold", "threshold": thr})
            self.ble_worker.send_config({"command": "arm_acoustic", "mode": "data_collection"})
            self.log(f"Threshold {thr:.2f} — Acoustic Sensor ARMED! Tap fruit now...")

    def cancel_arm(self):
        if self.ble_worker:
            self.ble_worker.send_config({"command": "cancel"})
            self.log("Disarmed.")

    def save_session(self):
        if not self.captured_images and not self.captured_waveforms:
            messagebox.showwarning("Warning", "No images or taps collected!")
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

        meta = {
            "fruit_type": fruit,
            "category": cat,
            "sample_id": sample_id,
            "volume_cm3": float(self.entry_vol.get() or 150.0),
            "mass_grams": float(self.entry_mass.get() or 0.0),
            "num_images": len(self.captured_images),
            "num_taps": len(self.captured_waveforms),
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
        self._last_img = None
        self.lbl_counts.config(text="Pictures: 0 | Taps: 0")
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
                self.lbl_counts.config(text=f"Pictures: {len(self.captured_images)} | Taps: {len(self.captured_waveforms)}")
                self._last_img = img
                self._fit_preview_to_canvas()
            elif msg_type == "waveform":
                self.captured_waveforms.append(payload)
                self.lbl_counts.config(text=f"Pictures: {len(self.captured_images)} | Taps: {len(self.captured_waveforms)}")
                self.ax.clear()
                self.ax.plot(payload, color="red")
                self.ax.set_title(f"Acoustic Tap #{len(self.captured_waveforms)} (Peak: {np.max(payload)})")
                self.ax.grid(True)
                self.canvas_graph.draw()
            elif msg_type == "results_json":
                if "models" in payload:
                    self.listbox_models.delete(0, tk.END)
                    for m in payload["models"]:
                        self.listbox_models.insert(tk.END, m)

        self.root.after(100, self.process_queue)


if __name__ == "__main__":
    root = tk.Tk()
    app = FruitStudioGUI(root)
    root.mainloop()
