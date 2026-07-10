# Handheld Multi-Sensor Fruit Quality Scanner 🥭

A two-tier, non-destructive fruit quality classification system built on the ESP32-S3-CAM platform. The system combines deterministic physical DSP on the edge with high-tier machine learning models on a mobile companion application to replace traditional destructive testing methods.

---

## 📐 System Architecture

```text
.
├── app/
│   └── android_app/              # Mobile companion application (UI & heavy ML diagnostics)
└── esp/                          # 🍓 Run Target: ESP32-S3 Hardware
    ├── include/                  # Header files and configurations
    ├── lib/                      # Edge processing and helper libraries
    ├── main/
    │   └── main.ino              # Main DSP core, capture loop, and WebSocket server
    └── test/                     # Local test scripts and sensor calibration tools
```

---

## ⚙️ Module Breakdown

| Module | Role | Core Technologies | Run Target |
| :--- | :--- | :--- | :--- |
| **`esp`** | Captures piezo signals and surface images, runs physical DSP, handles instant ripeness classification, and acts as a local Wi-Fi AP. | C++, `esp-dsp` library, Arduino ESP32 | ESP32-S3-CAM Hardware |
| **`app`** | Establishes a local WebSocket connection, receives image frames, and executes complex computer vision models for surface pathologies. | Android (Kotlin/Java/Flutter), Mobile ML (EfficientNet-B4 / MobileNetV3) | Android Smartphone |

---

## 🛠️ Hardware Components

| Component | Hardware | Role |
| :--- | :--- | :--- |
| **Edge Controller** | ESP32-S3-CAM (Xtensa LX7) | Runs the real-time DSP logic, manages sensor interfaces, and serves image data. |
| **Acoustic Sensor** | Analog Piezo Vibration Module | Captures mechanical resonance frequencies generated from a physical knuckle tap. |
| **Optical Capture** | Onboard 2MP OV2640 Sensor | Captures surface frames under standardized lighting conditions using the flash LED. |
| **On-device Display** | SSD1306 I2C OLED | Renders localized zero-latency classification outputs (Unripe, Ripe, Overripe) in the field. |
| **Power Management** | Lithium-Polymer Battery + Charger | Enables standalone, handheld operation of the instrument. |

---

## 🧠 Architectural Philosophy: The Two-Tier System

To balance high-density diagnostic capability with instantaneous field operation, the system splits computational workloads into two independent tiers:

* **Stage 1: Ripeness Scan (On-Device DSP Core):** Runs entirely on the handheld ESP32-S3 using deterministic physical models. By avoiding black-box neural networks for core mechanical metrics, it generates a mathematically explainable, zero-latency maturity verdict without requiring an active internet connection.
* **Stage 2: Surface Health Check (Companion App Diagnostics):** Offloads computationally heavy pattern recognition to an external smartphone GPU via a local Wi-Fi AP. This allows high-parameter classifiers (like EfficientNet-B4) to identify complex fungal or bacterial pathogens without straining the power and thermal limits of the handheld microcontroller.

---

## 🔄 The Operational Pipeline

The handheld instrument evaluates fruit through a sequential pipeline designed for speed and diagnostic rigor:

```text
 ┌───────────────────┐      ┌───────────────────┐      ┌───────────────────┐      ┌───────────────────┐
 │ 1. IMPULSE & SCAN │ ───> │  2. HARDWARE DSP  │ ───> │ 3. DECISION MATRIX│ ───> │ 4. OFFLOAD (OPT)  │
 │ (Piezo + Camera)  │      │ (FFT & Hue Angle) │      │ (Deterministic)   │      │(Wi-Fi WebSocket)  │
 └───────────────────┘      └───────────────────┘      └───────────────────┘      └───────────────────┘
```

1. **Acoustic & Optical Impulse:** The user taps the fruit while holding the device against the surface. The piezo sensor registers the internal mechanical resonance frequency, and the camera captures a local surface matrix under standard illumination.
2. **On-Chip DSP Processing:**
   * **Mechanical Stiffness:** The ESP32-S3 samples the analog signal at $16\text{ kHz}$, processes it through a Hann window, and runs a $1024$-point hardware-accelerated Fast Fourier Transform (FFT) via `esp-dsp` to isolate the peak resonant frequency ($f_0$). Internal elasticity is resolved using the **Cooke-Rand Stiffness Coefficient formula**:
     $$S = f_0^2 \cdot m^{2/3}$$
     *(where $m$ represents the estimated mass).*
   * **Colorimetric Decay:** The camera frame is mapped from RGB to the HSV color space, extracting the average Hue angle ($H$) of the center-weighted region. This yields a light-invariant metric of chlorophyll degradation.
3. **Maturity Classification:** A localized 2D Decision Matrix cross-references stiffness ($S$) against hue ($H$). This easily isolates anomalies like artificially-ripened specimens (yellow skin with hard interior) or internally bruised specimens (green skin with soft interior).
4. **Diagnostic Offloading:** If deep pathological verification is required, the ESP32 streams compressed JPEG images and scalar sensor telemetry via local WebSockets to the companion Android application for ML inference.

---

## 🏃 Quick Start Guide

### 1. Flashing the Handheld Firmware
To build and upload the core firmware to your ESP32-S3 board:

```bash
# Navigate to the ESP firmware directory
cd esp/main

# Open main.ino using your preferred IDE (e.g., Arduino IDE, VS Code with ESP-IDF/Arduino extensions)
# Set your board target to: ESP32S3 Dev Module
# Verify that the 'esp-dsp' library is installed in your local library path.
# Build and flash the firmware over USB-C.
```

### 2. Launching the Companion Android App
To configure the companion app for advanced ML diagnostic testing:

```bash
# Navigate to the Android application folder
cd app/android_app

# Open the project directory in Android Studio.
# Sync the Gradle files to resolve the dependencies (including TensorFlow Lite / PyTorch Mobile).
# Build and deploy the APK to your test Android device.
# Connect your mobile device to the local Wi-Fi AP generated by the ESP32-S3 to start listening to the sensor telemetry stream.
```
