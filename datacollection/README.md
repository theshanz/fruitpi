# FruitPi Data Collection Toolkit

> **Note:** This module was developed with AI assistance (GitHub Copilot / LLM-based coding tools).

A dedicated toolset for gathering labeled acoustic + optical fruit samples to train the FruitPi ML classification pipeline.

---

## Structure

```
datacollection/
├── esp/                        # ESP32-S3-CAM firmware (Arduino/PlatformIO)
│   └── main/
│       ├── main.ino            # Serial command handler & state machine
│       ├── tap_detector.h/.ino # Piezo tap detection & raw ADC capture
│       └── camera_handler.h/.ino # OV2640 JPEG capture + base64 encoding
├── python/                     # Desktop collection UI & data pipeline
│   ├── collector_ui.py         # Tkinter GUI — main collection interface
│   ├── serial_bridge.py        # Serial / mock communication abstraction
│   ├── mock_esp32.py           # Simulated ESP32 (damped sinusoid waveforms)
│   ├── real_dataset.csv        # Master CSV index of all collected samples
│   ├── samples/                # Raw tap arrays (.npy) + photos per scan
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

---

## Workflow

1. Select **Fruit Type** (mango / guava / papaya) and **Ripeness Label** from the top bar.
2. Click **Collect Sample** — the ESP32 arms and waits for 3 piezo taps.
3. Tap the fruit 3 times. Raw 1024-sample ADC buffers are streamed over serial.
4. Click **Take Photo** to capture a JPEG frame (repeatable).
5. Enter **Mass (g)** and **Circumference (cm)**.
6. Click **Finish Datapoint** — data is saved to `samples/` and indexed in `real_dataset.csv`.
7. Repeat.

---

## What Gets Saved Per Sample

| Field | Description |
|---|---|
| `taps.npy` | Raw 1024-sample uint16 ADC buffers (3 taps × 1024 samples) |
| `photo_*.jpg` | JPEG frames captured during the scan |
| CSV metadata | Timestamp, fruit type, label, mass, circumference, volume estimate, soft-label vector, file paths |

No FFT or derived features are computed at collection time — all signal processing is deferred to the training pipeline.

---

## ESP32 Firmware

The datacollection firmware is a stripped-down variant of the main `esp/` firmware, purpose-built for data acquisition:

- **Tap detection:** Adaptive noise floor tracking with configurable threshold and cooldown.
- **Raw ADC capture:** 1024 samples at ~16 kHz per tap, stored in PSRAM buffers.
- **Camera capture:** QVGA JPEG frames encoded to base64 over serial.
- **Serial protocol:** JSON lines — commands (`ARM`, `CAPTURE`, `PING`) in, JSON objects out.

Build with PlatformIO:

```bash
cd datacollection/esp
pio run -e esp32s3cam
```
