# FruitPi Data Collection Toolkit

> **Note:** This module was developed with AI assistance (GitHub Copilot / LLM-based coding tools).

A dedicated toolset for gathering labeled acoustic + optical fruit samples to train the FruitPi ML classification pipeline.

---

## Structure

```
datacollection/
├── esp/                        # ESP32-S3-CAM firmware (Arduino/PlatformIO)
│   └── main/
│       ├── main.ino            # Serial command handler & threshold state machine
│       ├── adc_capture.h/.ino  # DMA continuous ADC capture (80 kHz, 4096 samples)
│       └── camera_handler.h/.ino # OV2640 JPEG capture + base64 encoding
├── python/                     # Desktop collection UI & data pipeline
│   ├── collector_ui.py         # Tkinter GUI — main collection interface
│   ├── serial_bridge.py        # Serial / mock communication abstraction
│   ├── mock_esp32.py           # Simulated ESP32 (damped sinusoid waveforms)
│   ├── samples/                # Self-contained scan folders
│   └── requirements.txt        # Python dependencies
└── README.md
```

---

## Quick Start

### 1. Install Python dependencies

```bash
cd datacollection/python
pip install -r requirements.txt
```

### 2. Run in mock mode (no hardware)

```bash
python collector_ui.py --mock
```

### 3. Run with real ESP32

```bash
python collector_ui.py --port /dev/ttyUSB0   # Linux
python collector_ui.py --port COM3            # Windows
```

### 4. Set custom threshold

```bash
python collector_ui.py --mock --threshold 500
```

### 5. Rapid batch collection

```bash
python collector_ui.py --mock --rapid --keep-fields
```

---

## Keyboard Shortcuts

| Key | Action |
|---|---|
| `Space` | Arm (IDLE) / Stop (ARMED) / Finish (READY/PHOTO) |
| `Enter` | Finish datapoint |
| `P` | Take photo |
| `R` | Toggle rapid mode on/off |

---

## Workflow

1. Select **Fruit Type** (mango / guava / papaya) and **Ripeness Label** from the top bar.
2. Adjust **Threshold** (0-4095) if needed — default is 300.
3. Press `Space` or click **Arm** — ESP32 starts monitoring ADC for a rising edge.
4. **Tap the fruit** — when the signal crosses the threshold, ESP32 captures 4096 samples at 80 kHz (~51 ms) and sends them back.
5. Press `P` to capture a JPEG frame (repeatable).
6. Enter **Mass (g)** and **Circumference (cm)**.
7. Press `Enter` or click **Finish** — data is saved to `samples/`.
8. In **rapid mode**, the next arm cycle starts automatically.

---

## Threshold Detection

The ESP32 uses **rising-edge detection** for reliable tap triggering:

- Monitors ADC continuously after ARM at 80 kHz
- Triggers when signal crosses from **below** to **at/above** the threshold
- Single-tap capture: one tap = one 4096-sample buffer
- Threshold is configurable from the UI (0-4095) or via CLI `--threshold`
- Default threshold (300) works well for most piezo sensors — tune if you get false triggers or missed taps

---

## Data Format

Each scan is a self-contained folder under `samples/`:

```
samples/scan_042/
├── metadata.json     # All labels + measurements (authoritative)
├── taps.npy          # Raw ADC data (4096 samples, uint16)
├── photo_0.jpg       # Captured JPEG frames
├── photo_1.jpg
└── ...
```

### `metadata.json`

```json
{
  "id": "scan_042",
  "timestamp": 1783801748,
  "fruit_type": "mango",
  "label": "ripe",
  "mass_g": 250.0,
  "circumference_cm": 32.5,
  "vol_est": 0.5,
  "n_taps": 1,
  "tap_file": "taps.npy",
  "photo_files": ["photo_0.jpg", "photo_1.jpg"],
  "soft_labels": [0.0, 1.0, 0.0, 0.0, 0.0, 0.0],
  "sample_rate": 80000,
  "n_samples": 4096
}
```

### Loading the dataset

```python
import json, glob
import numpy as np
import pandas as pd

scans = []
for f in glob.glob("samples/*/metadata.json"):
    meta = json.load(open(f))
    meta["taps"] = np.load(f.rsplit("/", 1)[0] + "/taps.npy")
    scans.append(meta)
df = pd.DataFrame(scans)
```

No FFT or derived features are computed at collection time — all signal processing is deferred to the training pipeline.

---

## CLI Options

| Flag | Default | Description |
|---|---|---|
| `--mock` | off | Use simulated ESP32 (no hardware) |
| `--port` | `/dev/ttyUSB0` | Serial port |
| `--baud` | `921600` | Baud rate |
| `--threshold` | `300` | Tap detection threshold (0-4095) |
| `--rapid` | off | Auto-rearm after each save |
| `--keep-fields` | off | Keep mass/circ values between samples |

---

## ESP32 Firmware

The datacollection firmware is a stripped-down variant of the main `esp/` firmware, purpose-built for data acquisition:

- **Threshold-triggered capture:** ARM command starts continuous DMA monitoring; rising-edge detection triggers a 4096-sample capture at 80 kHz.
- **Non-blocking:** DMA runs in the background via `adc_continuous`; `loop()` polls for samples and serial commands interleaved.
- **Configurable threshold:** Set via `THRESHOLD <value>` command from Python UI (0-4095).
- **Serial transfer:** Base64-encoded uint16 LE (`{"samples_b64":"..."}`).
- **Camera capture:** QVGA JPEG frames encoded to base64 over serial.
- **Serial protocol:** JSON lines — commands (`ARM`, `THRESHOLD`, `CAPTURE`, `PING`) in, JSON objects out.

Build with PlatformIO:
