# Handheld Multi-Sensor Fruit Quality Scanner 🥭

A two-tier, non-destructive fruit quality classification system designed for the ESP32-S3-CAM platform. The system combines deterministic physical digital signal processing (DSP) on the edge with machine learning diagnostics running on a mobile companion application.

---

## 📐 System Architecture

```text
.
├── app/
│   └── android_app/              # Flutter companion app (UI & ML diagnostics)
└── esp/                          # ESP32-S3 Hardware Firmware
    ├── include/                  # Headers & configs
    ├── lib/                      # Edge DSP libraries
    ├── main/
    │   └── main.ino              # DSP core, capture loop, WebSocket server
    └── test/                     # Calibration scripts
```

---

## ⚙️ Module Breakdown

| Module | Role | Core Technologies | Run Target |
| :--- | :--- | :--- | :--- |
| **`esp`** | Captures piezo and camera inputs, performs hardware FFT and Hue calculation, evaluates the deterministic decision matrix, and hosts a local WebSocket server. | C++, `esp-dsp`, Arduino ESP32 | ESP32-S3-CAM |
| **`app`** | Connects to the local ESP32 Access Point, receives telemetry payloads and JPEG frames, and runs pattern recognition models. | Flutter, Mobile Machine Learning | Android Smartphone |

---

## 🔄 How the System Works

The scanner operates via a decoupled, two-tier architecture to deliver rapid, explainable diagnostics:

```text
 ┌───────────────────┐      ┌───────────────────┐      ┌───────────────────┐      ┌───────────────────┐
 │ 1. IMPULSE & SCAN │ ───> │  2. HARDWARE DSP  │ ───> │ 3. DECISION MATRIX│ ───> │ 4. OFFLOAD (OPT)  │
 │ (Piezo + Camera)  │      │ (FFT & Hue Angle) │      │ (Deterministic)   │      │(Wi-Fi WebSocket)  │
 └───────────────────┘      └───────────────────┘      └───────────────────┘      └───────────────────┘
```

### Tier 1: Local On-Device DSP (Maturity Scan)
* **Acoustic Density Extraction:** A physical tap on the fruit generates an internal resonance. The piezo sensor samples this vibration at **16 kHz**. The ESP32-S3 processes the signal with a Hann window and calculates a **1024-point Fast Fourier Transform (FFT)** using the hardware-accelerated `esp-dsp` library to identify the peak resonant frequency ($f_0$).
* **Stiffness Calculation:** The internal elasticity is estimated via the Cooke-Rand formula:
  $$S = f_0^2 \cdot m^{2/3}$$
  *(where $m$ is the fruit's mass).*
* **Optical Pigment Extraction:** The OV2640 camera captures a center-weighted image under standardized flash lighting. The firmware converts the raw RGB values into the **HSV color space** to measure the average Hue angle ($H$), indicating the concentration of chlorophyll.
* **Deterministic Classification:** These parameters ($S$ and $H$) are evaluated through a localized, non-black-box 2D Decision Matrix to classify the fruit as **Unripe, Ripe, or Overripe**.

### Tier 2: Companion App ML (Pathology Scan)
* When advanced diagnostic checks are initiated, the ESP32 acts as a local Wi-Fi Access Point and streams raw sensor data alongside JPEG image payloads over a WebSocket connection.
* The Flutter companion application receives these frames and evaluates them using lightweight mobile networks (such as **EfficientNet-B4** or **MobileNetV3**) to identify external pathologies (e.g., Anthracnose or stem-end rot).

---

## 🛠️ Hardware Specification

| Component | Part | Role |
| :--- | :--- | :--- |
| **Edge Controller** | ESP32-S3-CAM | Manages the main DSP loop, sensor I/O, and WebSocket server. |
| **Acoustic Sensor** | Analog Piezo Module | Measures mechanical vibration waveforms from the physical tap. |
| **Optical Capture** | OV2640 2MP Lens | Captures surface frames under standard flash illumination. |
| **Local Display** | SSD1306 I2C OLED | Renders localized zero-latency ripeness results in the field. |
| **Power Management** | LiPo Battery + Charger | Supplies regulated power for handheld portability. |

---

## 🏃 Development & Compilation Setup



### 1. Firmware Setup (Arduino IDE)

#### Step A: Configure Board Manager
1. Open the Arduino IDE.
2. Go to **File** → **Preferences** (or **Arduino IDE** → **Settings** on macOS).
3. Under **Additional Boards Manager URLs**, paste the Espressif system index:
   ```text
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
4. Click **OK**.

#### Step B: Install ESP32 Platform Support
1. Go to **Tools** → **Board** → **Boards Manager...**
2. Search for `esp32` and install the official **esp32 by Espressif Systems** package.

#### Step C: Install the DSP Core Library
1. Go to **Tools** → **Manage Libraries...**
2. Search for `ESP-DSP` and install the library **ESP-DSP by Espressif Systems**.

#### Step D: Open and Configure Settings
1. Open the project file: **File** → **Open...** → Navigate and select `esp/main/main.ino`.
2. Ensure your toolchain parameters under the **Tools** menu match these specifications:
   * **Board:** `ESP32S3 Dev Module`
   * **USB CDC On Boot:** `Enabled` (necessary for serial output monitoring)
   * **Flash Size:** `8MB` or `16MB` (depending on your specific ESP32-S3-CAM variant)
   * **Partition Scheme:** `Huge APP (3MB No OTA/1MB SPIFFS)` (necessary to fit the DSP mathematical libraries)
   * **PSRAM:** `OPI PSRAM` (or `QSPI PSRAM` depending on hardware; **must be enabled** to allocate memory buffers for camera frames)

3. Connect your board over USB and click **Upload** (Ctrl+U).

---

### 2. Flutter Companion App Setup

To run the mobile companion application on an Android device:

```bash
# Navigate to the Flutter project folder
cd app/android_app

# Fetch dependency packages
flutter pub get

# Launch the app on your connected device
flutter run
```


---

## 🤝 Contributing


```bash
git clone https://github.com/theshanz/fruitpi.git
cd fruitpi
```

---

## 📄 License
