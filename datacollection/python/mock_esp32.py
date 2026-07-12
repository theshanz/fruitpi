"""
mock_esp32.py — Simulates ESP32 button-triggered ADC capture.

Generates realistic piezo waveforms (damped sinusoids + harmonics + ADC
artefacts) and sends base64-encoded uint16 buffers over the mock Serial
link. Matches the real firmware's 80 kHz / 4096-sample capture.
"""

import base64
import io
import json
import queue
import random
import struct
import threading
import time

import numpy as np
from PIL import Image

N_SAMPLES = 4096
SAMPLE_RATE = 80000

ACOUSTIC_PROTOTYPES = {
    "mango": {
        "unripe":            {"freq": 800,  "damp": 50,   "amp": 0.90},
        "ripe":              {"freq": 400,  "damp": 120,  "amp": 0.75},
        "overripe":          {"freq": 200,  "damp": 220,  "amp": 0.45},
        "rotten_hollow":     {"freq": 500,  "damp": 350,  "amp": 0.20},
        "transitioning":     {"freq": 600,  "damp": 80,   "amp": 0.65},
        "artificially_ripened": {"freq": 750, "damp": 60, "amp": 0.85},
    },
    "guava": {
        "unripe":            {"freq": 1100, "damp": 30,   "amp": 0.95},
        "ripe":              {"freq": 500,  "damp": 110,  "amp": 0.75},
        "overripe":          {"freq": 220,  "damp": 250,  "amp": 0.40},
        "rotten_hollow":     {"freq": 450,  "damp": 350,  "amp": 0.18},
        "transitioning":     {"freq": 800,  "damp": 60,   "amp": 0.60},
        "artificially_ripened": {"freq": 1000, "damp": 40, "amp": 0.88},
    },
    "papaya": {
        "unripe":            {"freq": 900,  "damp": 40,   "amp": 0.92},
        "ripe":              {"freq": 350,  "damp": 100,  "amp": 0.70},
        "overripe":          {"freq": 180,  "damp": 200,  "amp": 0.38},
        "rotten_hollow":     {"freq": 420,  "damp": 320,  "amp": 0.22},
        "transitioning":     {"freq": 650,  "damp": 70,   "amp": 0.58},
        "artificially_ripened": {"freq": 850, "damp": 55, "amp": 0.82},
    },
}


def _jitter(val, pct=0.10):
    return val * random.uniform(1 - pct, 1 + pct)


def _simulate_capture(fruit, label):
    """Generate one 4096-sample ADC capture as uint16 numpy array."""
    proto = ACOUSTIC_PROTOTYPES.get(fruit, ACOUSTIC_PROTOTYPES["mango"])
    p = proto.get(label, proto["ripe"])

    f0 = _jitter(p["freq"], 0.08)
    damp = _jitter(p["damp"], 0.15)
    amp = _jitter(p["amp"], 0.10)

    t = np.arange(N_SAMPLES) / SAMPLE_RATE

    signal = amp * np.exp(-damp * t) * np.sin(2 * np.pi * f0 * t)
    signal += (amp * 0.15 * np.exp(-damp * 1.5 * t)
               * np.sin(2 * np.pi * f0 * 2.03 * t))

    n1f = np.cumsum(np.random.normal(0, 0.003, N_SAMPLES))
    n1f -= np.linspace(n1f[0], n1f[-1], N_SAMPLES)
    signal += n1f

    signal += np.random.normal(0, 0.012, N_SAMPLES)

    adc_f = ((signal + 1.65) / 3.3) * 4095
    adc_u16 = np.clip(np.round(adc_f).astype(np.uint16), 0, 4095)
    return adc_u16


def _make_test_photo():
    """Generate a small colourful test JPEG as base64."""
    img = Image.new("RGB", (160, 120))
    pixels = img.load()
    for y in range(120):
        for x in range(160):
            r = int(128 + 100 * np.sin(x * 0.05))
            g = int(100 + 80 * np.cos(y * 0.07))
            b = int(150 + 60 * np.sin((x + y) * 0.04))
            pixels[x, y] = (min(255, max(0, r)),
                            min(255, max(0, g)),
                            min(255, max(0, b)))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=60)
    return base64.b64encode(buf.getvalue()).decode("ascii")


class MockESP32:
    def __init__(self, outgoing: queue.Queue, incoming: queue.Queue):
        self.outgoing = outgoing
        self.incoming = incoming
        self.fruit = "mango"
        self.label = "unripe"
        self.running = False
        self._thread = None

    def start(self, fruit="mango", label="unripe"):
        self.fruit = fruit
        self.label = label
        self.running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self.running = False

    def _send(self, obj):
        self.outgoing.put(json.dumps(obj))

    def _run(self):
        while self.running:
            try:
                cmd = self.incoming.get(timeout=0.1)
            except queue.Empty:
                continue

            cmd = cmd.strip()

            if cmd == "PING":
                self._send({"status": "pong"})

            elif cmd == "ARM":
                self._send({"status": "armed"})
                time.sleep(random.uniform(0.3, 1.5))
                if not self.running:
                    break
                data = _simulate_capture(self.fruit, self.label)
                b64 = base64.b64encode(data.tobytes()).decode("ascii")
                self._send({"samples_b64": b64})

            elif cmd.startswith("THRESHOLD "):
                pass  # ack silently (mock doesn't use threshold)

            elif cmd == "CAPTURE":
                self._send({"photo": _make_test_photo()})
