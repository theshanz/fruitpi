

# FruitPi 🥭

An edge-computing sorting system designed for real-time fruit anomaly detection. The system runs an INT8 quantized YOLOv11 model on a Raspberry Pi 4 using ONNX Runtime, controlled via a local Flutter Web Dashboard.

---

## 📐 System Architecture

```text
theshanz/fruitpi/
├── .github/workflows/
│   └── release.yml               # CI/CD: Compiles Rust & builds Flutter Web UI
├── training/                     # 💻 Run Target: Training PC (CPU or GPU)
│   ├── train_initial.py          # PyTorch training script
│   ├── export_onnx.py            # Model exporter (PyTorch -> INT8 ONNX)
│   ├── requirements.txt          # Python dependencies (PyTorch, Ultralytics)
│   └── dataset_tools/            # Configuration files and dataset utilities
├── raspi/                        # 🍓 Run Target: Raspberry Pi 4
│   ├── rust-core/                # Rust main loop, GStreamer, and Axum API
│   │   ├── Cargo.toml
│   │   └── src/                  # Inference loop, Camera capture, Web Server
│   ├── setup_pi.sh               # Systemd installer & dependency setup script
│   └── trigger_update.sh         # Update script triggered via Dashboard
└── dashboard/                    # 📱 Run Target: Web Browser (Served by Rust)
    ├── pubspec.yaml
    └── lib/                      # Flutter UI (Dashboard, Manual Updates)
```

---

## ⚙️ Module Breakdown

| Module | Role | Core Technologies | Run Target |
| :--- | :--- | :--- | :--- |
| **`training`** | Handles dataset preparation, model training, and exporting to INT8 ONNX. | Python, PyTorch, Ultralytics YOLOv11 | PC (CPU or GPU) |
| **`raspi`** | Captures camera streams, crops mango images, runs ONNX inference, and triggers actuators. | Rust, ONNX Runtime (XNNPACK), OpenCV | Raspberry Pi 4 |
| **`dashboard`** | Displays real-time sorting metrics and provides an interface to trigger updates. | Flutter Web, Axum Web Server (Rust backend) | Local Web Browser |

---

## 🛠️ Hardware Components

| Component | Hardware | Role |
| :--- | :--- | :--- |
| **Training PC** | Standard PC (CPU or GPU) | Used for the initial model training and ONNX export. |
| **Edge Compute** | Raspberry Pi 4 | Runs the real-time Rust engine and hosts the local dashboard. |
| **Camera** | Global Shutter Camera | Captures sharp images of the fruit on the conveyor belt. |
| **Control UI** | Any Phone, Tablet, or PC | Displays stats and allows manual system updates via web browser. |

---

## 🧠 Why PyTorch?

We utilize PyTorch for the initial model training phase because of its integration with modern computer vision tools:
* **Native YOLOv11 Support:** The Ultralytics YOLOv11 framework is built directly on PyTorch, ensuring stable training and robust optimization utilities.
* **Flexible Hardware Acceleration:** PyTorch seamlessly utilizes standard CPUs or dedicated GPUs (via CUDA or ROCm) without altering the codebase.
* **Clean ONNX Translation:** PyTorch weights (`.pt`) translate reliably into quantized ONNX format for efficient edge CPU execution.

---

## 🔄 The Operational Pipeline

The system processes fruit on the conveyor belt through a continuous, four-stage pipeline. (Collaborators are encouraged to dive into `raspi/rust-core/src/` to study the low-level implementation details):

```text
 ┌───────────────────┐      ┌───────────────────┐      ┌───────────────────┐      ┌───────────────────┐
 │ 1. IMAGE CAPTURE  │ ───> │ 2. PREPROCESSING  │ ───> │  3. ONNX RUNTIME  │ ───> │  4. ACTUATION/UI  │
 │ (GStreamer Stream)│      │  (CV Mango Crop)  │      │  (YOLOv11 INT8)   │      │ (Arduino & Web)   │
 └───────────────────┘      └───────────────────┘      └───────────────────┘      └───────────────────┘
```

1. **Image Capture:** A low-latency GStreamer pipeline captures sharp frames from the global shutter camera.
2. **Preprocessing & ROI (Region of Interest):** Traditional computer vision filters detect the presence of the fruit, calculate its bounding area, and crop the image.
3. **ONNX Inference:** The cropped fruit image is normalized and run through the lightweight INT8 quantized model using ONNX Runtime.
4. **Action & Visualization:** Based on the model's confidence scores, the Rust engine logs the decision to SQLite, updates the Flutter Web Dashboard via WebSockets, and signals the mechanical actuator.

---

## 🏃 Quick Start Guide

### 1. Training on the PC
Configure your Python environment and train your initial model:
```bash
# Install required dependencies
pip install -r training/requirements.txt

# Train the model
python training/train_initial.py

# Convert the trained model to optimized INT8 ONNX format
python training/export_onnx.py
```

### 2. Local Rapid-Testing (No Pi Required)
To iterate on the Rust engine and Flutter UI directly on your PC:
```bash
# 1. Run the UI in your browser
cd dashboard
flutter run -d chrome

# 2. Test Rust engine locally using a dummy video instead of a physical camera
cd ../raspi/rust-core
cargo run -- --input path/to/conveyor_belt.mp4
```

### 3. Deploying to the Raspberry Pi 4
Run this setup script on a fresh Raspberry Pi 4 to install the background services and dependencies:
```bash
# SSH into the Pi, clone the repository, and run setup
git clone https://github.com/theshanz/fruitpi.git
cd fruitpi/raspi
bash setup_pi.sh
```
