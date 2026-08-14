# ARIA: Adaptive Runtime Inference Accelerator

**A learned, frame-by-frame compute-scaling framework for edge AI on Arm Cortex-M microcontrollers.**

Built for the **Arm AI Optimization Challenge 2026 - Physical AI Track**

Hardware core: Renesas EK-RA8D1 (Cortex-M85), ESP32, 7SEMI ADS1263, ICM20948. 

Submission window: June 10 to August 14, 2026 | Timeline: 6-week build, started July 5, 2026

---

## 1. Table of Contents

1. [Table of Contents](#1-table-of-contents)
2. [Overview](#2-overview)
3. [Why This Isn't Just an If/Else Statement](#3-why-this-isnt-just-an-ifelse-statement)
4. [System Architecture](#4-system-architecture)
5. [Hardware](#5-hardware)
6. [Per-Demo Model Zoo Design](#6-per-demo-model-zoo-design)
7. [Meta-Controller Design](#7-meta-controller-design)
8. [Firmware / Software Stack](#8-firmware--software-stack)
9. [Inference Dispatch Architecture](#9-inference-dispatch-architecture)
10. [Repository / Project Structure](#10-repository--project-structure)
11. [Toolchain, Build System & Environment](#11-toolchain-build-system--environment)
12. [The Build Journey: Every Problem We Hit and How We Fixed It](#12-the-build-journey--every-problem-we-hit-and-how-we-fixed-it)
13. [Runtime Feature Extraction](#13-runtime-feature-extraction)
14. [Meta-Controller Training Data Extraction](#14-meta-controller-training-data-extraction)
15. [Benchmarking Infrastructure](#15-benchmarking-infrastructure)
16. [Dynamic Latency Measurement](#16-dynamic-latency-measurement)
17. [How to Build and Flash](#17-how-to-build-and-flash)
18. [Demo Script](#18-demo-script)
19. [Toolchain, Build System and Environment](#19-toolchain-build-system-and-environment)
20. [Submission Checklist](#20-submission-checklist)
21. [License](#21-license)
22. [Developers](#20-developers)

---

## 2. Overview

ARIA is **not a single model**. It is an on-device runtime layer that decides how much inference capacity a physical sensing task should use next, choosing among **Fast**, **Balanced**, and **Accurate** model variants already resident in the same MCU flash image.

Think of it as an automatic transmission for edge inference:

- **Model Zoo** (the gears): three pre-trained variants of the same task, with different speed/accuracy/compute trade-offs.
- **Meta-Controller** (the learned transmission logic): a compact neural network that consumes live runtime features and proposes the next model tier.
- **Sensor-specific event guard** (deployed safety/responsiveness layer): small deterministic logic used only where the physical sensor needs an immediate reaction that the longer inference window may average away. For ADS1263 this guard is triggered only by **sudden sample-to-sample spikes or dips**; ordinary and gradual behavior remains under the learned controller.

A major implementation advantage is that **all six EON-compiled model variants and the trained Meta-Controller engine are linked into one RA8D1 firmware image**: Three ADS1263 models, three ICM-20948 models and the trained meta-controller. Runtime switching requires no model download, no filesystem access, no cloud request, and no reboot; the dispatcher simply invokes the already-linked graph selected for that sensor.

The reusable idea is the common skeleton:

```text
sensor -> runtime features -> adaptive controller -> model zoo -> output -> feedback
```

The sensor-specific feature definitions and protective event logic can differ, while the model-zoo / controller / dispatcher structure remains the same.

## 3. Why This Isn't Just an If/Else Statement

ARIA's **normal model-selection path is learned from labeled runtime data** rather than being a single hand-written mapping from sensor amplitude to model tier. The ADS meta-controller, for example, is a trained `4 -> 16 -> 3` MLP that consumes confidence average, confidence trend, signal delta, and signal RMS and produces learned evidence for Fast, Balanced, and Accurate.

The deployed system is intentionally described accurately as **learned routing with sensor-specific event guards**, not as a threshold-free system:

1. **Normal routing is learned.** The trained Meta-Controller supplies the model-selection evidence used during ordinary operation.
2. **The ADS transient override is exceptional, not continuous.** It runs on fresh ADC samples but only overrides normal routing when a sudden positive or negative sample-to-sample change is detected. A gradual change does not automatically force Balanced or Accurate.
3. **The ICM path is physics-guided.** Stationary/strong-motion safeguards use motion statistics appropriate to a six-axis IMU, while the trained controller contributes within the adaptive policy.
4. **The architecture generalizes across domains.** Both demos use the same model-zoo + runtime-feature + controller + dispatch + telemetry skeleton even though their sensor-specific features and guards differ.

This distinction is important: ARIA is not claiming that every deployed control decision is neural-network-only. It demonstrates a practical edge-AI architecture in which learned compute selection is combined with minimal, explicit physical event handling when responsiveness or safety requires it.

### 3.1 Real-World Use Cases & Deployment Scenarios

ARIA's model-zoo-plus-meta-controller pattern isn't tied to any one sensor, it's a template for any Cortex-M deployment where "always run the expensive model" wastes power and "always run the cheap model" misses rare, important events.

**1. Industrial predictive maintenance (direct analogue of Demo 1)**
A sensor node bolted to a motor, pump, or conveyor bearing housing reads vibration or current draw through an analog front-end identical in shape to the ADS1263 pipeline. Fast mode runs during normal operation (near-zero anomaly probability, minimal power draw); the moment RMS energy or waveform shape drifts, ARIA escalates to Balanced or Accurate and flags bearing wear or an impending fault before failure, without an absolute operating-level-to-model table tuned per machine; the deployed sensor may still use a small event guard for exceptional transients.

**2. Battery & electrical system anomaly detection**
The ADS1263's 32-bit resolution is built for exactly this: battery-management-system cell-imbalance detection in EV energy-storage packs, solar inverter output-anomaly monitoring, or panel-level circuit-breaker predictive fault detection, all cases where 99% of readings are boring and 1% matter enormously.

**3. Precision lab, test & calibration equipment**
Bench instrumentation and calibration rigs that log analog reference signals over long unattended runs (24 to 72 hour stability tests, environmental chambers, calibration drift-tracking) can run ARIA's Fast tier for the vast majority of a run and escalate to Accurate only when a reading suggests actual drift or an out-of-spec event worth flagging for a human to review, reducing how much of a multi-day log actually needs close scrutiny.

**4. The developer-impact case**
Because the model-zoo/controller/dispatcher skeleton is reusable across sensors, a Cortex-M85 developer can train three tiers for a new task and reuse the ARIA dispatch structure while adapting sensor-specific runtime features and guards as required. Every use case above is a drop-in replacement for the ADS1263 signal/ ICM20948, not a rewrite of the pipeline.


**5. Drone and UAV flight-anomaly detection**
Flight controllers already carry six-axis IMUs for stabilization. The same accel+gyro feature pipeline used in Demo 2 can run alongside the flight-control loop to flag abnormal motion signatures, a prop strike, an unexpected tumble, an impending stall, escalating compute only when the motion pattern looks abnormal rather than running an expensive anomaly model on every single control loop tick.

---

## 4. System Architecture

Both live demos use the same high-level runtime skeleton:

```text
Sensor
  |
  v
Runtime / physical feature extraction
  |
  v
Adaptive controller
  |
  +--> normal learned Meta-Controller path
  |
  +--> sensor-specific event guard when required
  |
  v
Model Zoo: Fast / Balanced / Accurate
  |
  v
Real inference result + measured latency
  |
  +--------------------> feedback into the next inference history
```

The control cadence is sensor-dependent:

1. A model inference completes on the window size required by the currently active variant.
2. Confidence and signal/motion features are updated.
3. The trained Meta-Controller proposes the next tier.
4. The dispatcher selects one of the already-linked model graphs for the next inference.
5. The result, confidence history, and DWT-measured latency feed the next decision.

For ADS1263 there is one additional fast path: every fresh ADC sample is checked for an **abrupt step**. Only a sufficiently sudden positive spike or negative dip can temporarily override the normal learned route to Balanced or Accurate. The selected transient tier is latched until one genuine inference from that model completes, then the normal learned path resumes.

For ICM-20948, domain-specific still/strong-motion logic uses gravity-removed accelerometer activity and gyroscope RMS around the same learned-controller/model-zoo skeleton.

The ESP32 and browser dashboard are observation layers only; they do not participate in inference or model selection.

## 5. Hardware


### 5.1 Finalized Demo Lineup


| Demo | Sensor | Task | Status |
|---|---|---|---|
| Demo 1 | ADS1263 (32-bit ADC) | Signal/vibration anomaly detection via analog channel | Hardware bring-up complete, working |
| Demo 2 | ICM-20948 (9-axis IMU) | Motion/vibration anomaly detection (accel + gyro, 6 axes used) | Hardware bring-up complete, working |


ARIA runs on **two live demos**: ADS1263 (analog signal anomaly detection) as the guaranteed core, and ICM-20948 (motion/vibration anomaly detection, 6 of its 9 axes used) as the second demo proving the framework generalizes across sensor domains.

**Demo 1 (ADS1263)** is the guaranteed core submission: a single sensor, three model variants (Fast/Balanced/Accurate), and a trained Meta-Controller making the runtime switching decision. This alone demonstrates the full pipeline, switching logic and sensor-agnostic architecture, end to end.

**Demo 2 (ICM-20948)** reuses the same high-level Meta-Controller/model-zoo architecture and training pattern on a structurally different sensing task (single-channel analog signal vs. multi-axis motion data), which is the direct, hardware-proven demonstration of generality across domains that the judging criteria call out.


### 5.2 Component List


| # | Component | Role | Interface |
|---|---|---|---|
| 1 | EK-RA8D1 (Cortex-M85) | Main compute: runs model zoo + meta-controller + RTOS | - |
| 2 | ADS1263 | Demo 1 sensor input | SPI |
| 3 | ICM-20948 (9-axis device; 6 axes used) | Demo 2 sensor input | SPI |
| 4 | ESP32 | Wi-Fi telemetry bridge + Dashboard to view results | UART to RA8D1 |


### 5.3 Physical Wiring → ADS1263 to EK-RA8D1


| ADS1263 Pin | EK-RA8D1 Pin | Notes |
|---|---|---|
| 5V | 5V | |
| GND | GND | |
| DRDY | **P010** (`MIKROBUS_INT`) | Configured as a plain GPIO input, polled every loop iteration via `R_IOPORT_PinRead()` — not a hardware ISR (see §11.1 for why). |
| MISO | **P410** (`SPI1.MISO1`) | Hardware SPI1 peripheral pin |
| MOSI | **P411** (`SPI1.MOSI1`) | Hardware SPI1 peripheral pin |
| SCK | **P412** (`SPI1.RSPCK1`) | Hardware SPI1 peripheral pin |
| CS | **P413** (`SPI1.SSLB0`) | Hardware chip-select, driven by the SPI peripheral itself, not a manual GPIO toggle |
| START | 5V | Strapped high in hardware to keep the ADC in continuous-conversion mode; `CMD_START1` in firmware (§8.2) is issued on top of this as the software-side start command |
| RST | **P507** (`MIKROBUS_RES`) | GPIO output, held low at init; pulsed per the reset sequence in §8.2 |
| AIN0 / A0 | analog divider / test-signal node | Positive signal input used by the demo; do **not** short this node directly to ground. |
| AINCOM | star GND | Negative input for `INPMUX = 0x0A` (`AIN0 - AINCOM`). |
| AGND | star GND | Analog ground reference shared by the divider/test circuit. |

All four SPI signals and the DRDY/RST GPIOs share the RA8D1's mikroBUS/Arduino-compatible header (`ARDUINO_D10`–`D13` silkscreen), which is why DRDY and RST land on the `MIKROBUS_INT`/`MIKROBUS_RES` pins rather than a separate PMOD connector.


### 5.4 Physical Wiring → ICM-20948 to EK-RA8D1

| ICM-20948 Pin | EK-RA8D1 Pin | Notes |
|---|---|---|
| VIN | 3V3 | Sensor supply |
| GND | GND | Common ground |
| SDO (MISO) | **P700** | SPI0 MISO |
| SDA (MOSI) | **P701** | SPI0 MOSI |
| SCL (SCK) | **P702** | SPI0 RSPCK0 |
| CS | **P705** | SPI0 SSLA2, active low |

This is the **working final SPI0 mapping**. P705 is intentional for chip-select. The final FSP configuration uses SPI mode 0, MSB-first, full-duplex 4-wire operation (`SPI_B_SSL_MODE_SPI`) at approximately 1 MHz.

The ICM path is fully brought up and live: `WHO_AM_I` returns `0xEA`, six-axis accel+gyro samples are acquired at 100 Hz, and the three ICM EON-compiled models run on the RA8D1.

### 5.5 UART Wiring --> ESP32 Telemetry Bridge (via PMOD2 / SCI2)


| ESP32 Pin | EK-RA8D1 Pin | Notes |
|---|---|---|
| GND | GND | |
| VIN | 3V3  | Depends on the specific ESP32 dev board's regulator |
| TX (GPIO17) | **J25-2** &#183; PA02 (`sci2.rxd2`) | Crossed: ESP32 TX feeds the RA8D1's RX |
| RX (GPIO16) | **J25-3** &#183; PA03 (`sci2.txd2`) | Crossed: RA8D1 TX feeds the ESP32's RX |


The UART telemetry path is now **active and proven with both sensor streams**. The RA8D1 sends ADS and ICM telemetry over the shared UART transport; a static binary semaphore serializes transmissions so the two FreeRTOS tasks cannot corrupt each other's packets.

The ESP32 parses the two streams independently and serves the live browser dashboard over Wi-Fi. Telemetry is deliberately outside the inference critical path: disconnecting Wi-Fi or the dashboard does not stop local model selection or inference on the RA8D1.

---

## 6. Per-Demo Model Zoo Design

Each demo has its own 3 model variants, trained in Edge Impulse but designed around what fits that sensor.

### 6.1 Generic Variant Template (Demo- ADS1263)

| Variant | Processing | Architecture | Quantization |
|---|---|---|---|
| Fast | Flatten (7 statistical features) | 1-layer dense NN (12-20 neurons) | int8 |
| Balanced | Flatten + Spectral Analysis | Dense NN (32→16→8) | int8 |
| Accurate | Flatten + Spectral Analysis (full) | Dense NN (128→64→32, dropout) | int8 |

### 6.2 Variant Template -- Demo 2: ICM-20948

The ICM-20948 model zoo consumes six interleaved inertial channels:

```text
accX + accY + accZ + gyrX + gyrY + gyrZ
```

The generated Edge Impulse metadata for all three ICM variants specifies:

- Sensor type: sensor fusion.
- Sampling frequency: 100 Hz (`10 ms` interval).
- Six input axes.
- Spectral Analysis DSP.
- `FFT length = 16`.
- FFT overlap enabled.
- Log spectral output enabled.
- `78` DSP / neural-network input features.
- Three classification outputs: `fault`, `idle`, `normal`.
- EON-compiled / TFLite compiled graph.
- Quantized int8 inference.

The deployed variant table is:

| Variant | Raw Input | DSP Block | NN / Deployment Architecture | Quantization |
|---|---|---|---|---|
| Fast | `100 frames × 6 = 600 floats` | Six-axis Spectral Analysis, FFT-16, overlap + log, 78 output features | EON-compiled TFLite classification graph, 78-feature input → 3-class output | int8 |
| Balanced | `100 frames × 6 = 600 floats` | Six-axis Spectral Analysis, FFT-16, overlap + log, 78 output features | EON-compiled TFLite classification graph, 78-feature input → 3-class output | int8 |
| Accurate | `200 frames × 6 = 1200 floats` | Six-axis Spectral Analysis, FFT-16, overlap + log, 78 output features | EON-compiled TFLite classification graph, 78-feature input → 3-class output | int8 |

> The generated metadata exposes the deployment graph interface, DSP configuration, feature count and quantization state. The exact layer-by-layer neural-network topology is compiled into the generated EON C++ graph, so this README does not invent an unverified Dense/CNN layer description.

The input layout is:

```text
[ax, ay, az, gx, gy, gz,
 ax, ay, az, gx, gy, gz,
 ...]
```

The dispatcher returns required **floats**, not frames:

```c
imu_run_inference_get_required_floats(variant)
```

and inference runs through:

```c
aria_run_inference_imu(
    variant,
    sample_window,
    &result,
    false);
```

The ICM classifier exposes:

```c
result.p_fault
result.p_idle
result.p_normal
```

The final merged implementation locally reproduces the ICM spectral feature path because the ADS-side generated spectral metadata cannot safely be reused for the ICM model. The local DSP reproduces the required six-axis 78-feature spectral input before direct EON graph execution.

### 6.3 Compiled Model Artifacts

#### ADS1263 artifacts

ADS model artifacts live under:

```text
src/aria-ads-merged/
```

Important files include:

```text
src/aria-ads-merged/
├── model-parameters/
│   ├── model_metadata.h
│   └── model_variables.h
├── tflite-model/
│   ├── tflite_model_compiled_fast.cpp
│   ├── tflite_model_compiled_fast.h
│   ├── tflite_model_compiled_balanced.cpp
│   ├── tflite_model_compiled_balanced.h
│   ├── tflite_model_compiled_accurate.cpp
│   ├── tflite_model_compiled_accurate.h
│   └── trained_model_ops_define.h
├── run_inference_dispatcher.cpp
└── run_inference_dispatcher.h
```

`model_variables.h` binds the generated model handles, DSP configuration and model-specific runtime metadata used by the ADS dispatcher.

#### ICM-20948 artifacts

The equivalent IMU-generated artifacts live under:

```text
src/aria-imu-merged/
```

Important files are:

```text
src/aria-imu-merged/
├── model-parameters/
│   ├── model_metadata_ICM.h
│   └── model_variables_ICM.h
├── tflite-model/
│   ├── tflite_model_compiled_icm_fast.cpp
│   ├── tflite_model_compiled_icm_fast.h
│   ├── tflite_model_compiled_icm_balanced.cpp
│   ├── tflite_model_compiled_icm_balanced.h
│   ├── tflite_model_compiled_icm_accurate.cpp
│   └── tflite_model_compiled_icm_accurate.h
└── run_inference_dispatcher_imu.h
```

`model_variables_ICM.h` binds the three ICM EON graph callbacks (`init`, `input`, `invoke`, `output`, `reset`), the spectral DSP configuration, quantization metadata, class labels and impulse handles.

The executable implementation remains in the merged dispatcher translation unit on the ADS side because ADS and ICM generated Edge Impulse definitions are intentionally kept together to avoid multiple-definition/linker conflicts.


### 6.4 Demo 2  -- ICM-20948

Same structural template as Demo 1, but consuming 6-axis motion data (accelerometer + gyroscope) instead of a single analog channel. The ICM-20948 is a genuine 9-axis IMU (accelerometer + gyroscope + magnetometer), but ARIA intentionally reads only the 6-axis accel + gyro data. Vibration and motion anomalies show up as changes in acceleration and rotation rate, not heading, so the magnetometer is irrelevant to this task. The richer multi-axis feature set lets Balanced/Accurate variants exploit spectral features across all six channels for improved fault/anomaly classification.

### 6.5 Trained Artifacts in This Repository

Each sensor owns a three-tier model zoo:

- **ADS1263:** Fast, Balanced, Accurate
- **ICM-20948:** Fast, Balanced, Accurate

The generated EON/TFLite artifacts and per-model metadata are compiled into the project rather than loaded dynamically at runtime.

**Key deployment advantage — six models, one flash image:** all six model graphs are present in the RA8D1 firmware at the same time. Switching a tier does not copy a new model into RAM, reload from external flash, contact the ESP32, or call the cloud. The runtime dispatcher simply selects the appropriate already-linked graph.

This is especially useful for embedded systems because it makes model switching deterministic and removes model-loading latency from the decision loop. The ESP32 is only a telemetry/dashboard bridge; the complete adaptive inference stack remains on the Cortex-M85.

### 6.6 Benchmark / Model-Zoo Results

| Demo | Variant | Edge Impulse test accuracy | Compiled source-size proxy¹ | Runtime latency |
|---|---|---:|---:|---|
| Demo 1 (ADS1263) | Fast | 83.3% | 27.3 KB | DWT-measured live |
| Demo 1 (ADS1263) | Balanced | 89.8% | 35.3 KB | DWT-measured live |
| Demo 1 (ADS1263) | Accurate | 96.3% | 75.8 KB | DWT-measured live |
| Demo 2 (ICM-20948) | Fast | 90.1% | not reported here | DWT-measured live |
| Demo 2 (ICM-20948) | Balanced | 89.1% | not reported here | DWT-measured live |
| Demo 2 (ICM-20948) | Accurate | 98.8% | not reported here | DWT-measured live |

¹ For ADS, the listed size is the combined generated `.cpp` + `.h` source size used as a relative complexity proxy. It is **not** a linker-verified per-model flash allocation.

The latency displayed by ARIA is not a table constant. It is measured around the model that actually ran using the Cortex-M85 DWT cycle counter, preserved in microseconds, and transmitted to the dashboard as both `latency_us` and decimal `latency_ms`. This means the dashboard demonstrates the real compute-cost change when ARIA switches model tiers rather than showing a pre-filled benchmark number.

## 7. Meta-Controller Design

### 7.1 Core Learned Controller

The ADS Meta-Controller is a trained MLP exported in `meta_controller_weights.h`:

```text
4 inputs -> 16 ReLU hidden units -> 3 output scores
```

Labels are:

```text
0 = FAST
1 = BALANCED
2 = ACCURATE
```

Its deployed inputs are exactly:

```text
confidence_avg
confidence_trend
signal_delta
signal_rms
```

`latency_us` is still logged and measured, but it is intentionally **not** an input to the current ADS MLP.

The current header was trained on 354 labeled runtime rows and reports a full-data training accuracy of 0.791. The firmware also reproduces the final MLP forward pass so the three raw learned output scores can be exposed in the debugger:

```text
g_debug_meta_score_fast
g_debug_meta_score_balanced
g_debug_meta_score_accurate
g_debug_meta_argmax
```

This allows the runtime decision to be inspected instead of treating the MLP as a black box.

### 7.2 ADS Runtime Feature Vector

`adc_thread_entry.c` maintains:

```c
typedef struct {
    float confidence_avg;
    float confidence_trend;
    float signal_delta;
    float latency_us;
    float signal_rms;
} meta_controller_features_t;
```

For the current ADS runtime:

- `confidence_avg` is the mean of the last **3** `p_anomaly` values.
- `confidence_trend` is the newest anomaly confidence minus the previous value.
- `signal_delta` is current inference-window RMS minus the previous inference-window RMS.
- `signal_rms` is the absolute RMS of the current inference window.
- `latency_us` is the DWT-measured inference duration and is logged/telemetried, but excluded from the four-input MLP.

### 7.3 Learned-Score Routing

Normal ADS routing is driven by the existing trained MLP. Instead of discarding all information except the argmax, the deployed firmware also examines the three raw learned output scores.

The current learned-score policy keeps Fast and Accurate argmax decisions directly. When Balanced is the argmax, the two neighboring alternatives are compared; if the learned Accurate score is stronger than the learned Fast score, the requested destination is Accurate, otherwise it remains Balanced.

A hierarchical scheduler then moves through the ordered compute tiers one level at a time:

```text
FAST <-> BALANCED <-> ACCURATE
```

A learned Fast-to-Accurate request therefore traverses a genuine Balanced inference before reaching Accurate. This scheduler does not inspect ADC voltage or RMS thresholds.

### 7.4 ADS Sudden-Transient Advancement

The latest ADS advancement adds a fast, sample-level **transient severity guard** so a brief spike or dip is not averaged away by a 100-300-sample inference window.

**This guard does not override the model on every sample.** It only takes control when the newest sample changes abruptly relative to the immediately preceding sample and the recent step-size baseline. If the signal changes gradually or remains ordinary, the trained Meta-Controller continues to control model selection.

The detector uses:

```text
abs_step = abs(current_sample - previous_sample)
```

and an exponential moving average of recent ordinary step magnitudes. The current bands are:

```c
ADS_BALANCED_STEP_MULTIPLIER = 3.0
ADS_BALANCED_MIN_STEP_V      = 0.100 V

ADS_ACCURATE_STEP_MULTIPLIER = 6.0
ADS_ACCURATE_MIN_STEP_V      = 0.650 V
```

Conceptually:

```text
normal / gradual signal
        |
        v
trained Meta-Controller
        |
        +--> FAST / BALANCED / ACCURATE

moderate sudden spike OR dip
        |
        v
BALANCED immediately
        |
        v
hold until one real BALANCED inference succeeds
        |
        v
release latch -> learned controller resumes

strong sudden spike OR dip
        |
        v
ACCURATE immediately
        |
        v
hold until one real ACCURATE inference succeeds
        |
        v
release latch -> learned controller resumes
```

Accurate has priority: a strong transient can preempt a pending Balanced transient, but a moderate transient cannot demote an active Accurate latch.

This is **not an absolute voltage-to-model mapping** such as `voltage > 0.954 V -> Accurate`. The event detector operates on sudden **change magnitude**, in either direction. A high but slowly changing voltage does not automatically invoke Accurate through this guard.

### 7.5 ICM-20948 Adaptive Policy

The IMU uses the same high-level feature/controller/model-zoo skeleton but a sensor-appropriate physics-guided policy. Per-axis acceleration DC/gravity is removed before dynamic accelerometer activity is computed, gyroscope RMS is tracked separately, and physical still/strong-motion regions protect the controller from implausible routing.

This is why the two demos should be described as sharing the same **adaptive inference architecture**, not literally identical sensor logic.

### 7.6 Runtime Escalation / De-Escalation Proof

**Demo 1 — ADS1263**

1. Under ordinary or gradual signal behavior, the trained MLP / learned-score scheduler chooses the next tier.
2. A moderate **sudden** positive spike or negative dip can request **Balanced immediately**.
3. A stronger **sudden** positive spike or negative dip can request **Accurate immediately**.
4. The transient-selected tier remains active until one real inference from that model completes successfully.
5. The transient latch then releases and normal learned routing resumes.

**Demo 2 — ICM-20948**

1. Keep the module physically still -> **Fast**.
2. Apply moderate motion -> **Balanced**.
3. Apply strong acceleration or rotation -> **Accurate**.
4. Stop moving the module -> the controller returns toward lower-compute tiers.

Together these demos prove dynamic three-tier compute selection across a single-channel precision ADC and a six-axis inertial sensor.

## 8. Firmware / Software Stack

| Layer | Technology |
|---|---|
| MCU | Renesas RA8D1 / Arm Cortex-M85 |
| RTOS | FreeRTOS (tasks, static binary semaphores, ISR-safe signalling) |
| HAL / BSP | Renesas FSP v5.9.0 (e² studio, Cortex-M85, `arm-none-eabi-gcc` 13.2.1) |
| IDE | Renesas e² studio |
| Compiler | Arm GNU Toolchain 13.2.1 |
| ML Runtime | Edge Impulse EON compiled models / TensorFlow Lite Micro |
| DSP / kernels | CMSIS-DSP / CMSIS-NN where used by generated models |
| Application task — ADS1263 | `adc_thread_entry.c` |
| Application task — ICM-20948 | `imu_thread_entry.c` |
| ADS + ICM inference | merged dispatcher implementation in `run_inference_dispatcher.cpp` plus C-facing dispatcher headers |
| ADS Meta-Controller | `meta_controller_weights.h` |
| ICM Meta-Controller | `meta_controller_weights_ICM.h` |
| Telemetry | `telemetry.c` / `telemetry.h` |
| Remote UI | ESP32 UART bridge + self-contained HTML dashboard |

### 8.1 ADC Task Responsibilities (`adc_thread_entry.c`)

The ADC thread is the heart of Demo 1 and performs the complete live pipeline:

1. **DRDY polling:** waits for the ADS1263 `DRDY` pin on `BSP_IO_PORT_00_PIN_10`.
2. **SPI read:** issues `CMD_RDATA1` over the established `g_spi0` / SPI channel-1 hardware path and synchronizes completion with the FreeRTOS semaphore/callback mechanism.
3. **Signed code-to-voltage conversion:** `g_adc_voltage = (((double)g_adc_code / 2147483648.0) * 2.5);`.
4. **Sample-level transient check:** compares the newest sample with the previous sample. Only abrupt changes can invoke the Balanced/Accurate transient latches described in §7.4.
5. **Ring-buffer capture:** stores the continuous signal in the 4000-sample circular buffer.
6. **Variant-sized inference:** waits until the active Fast/Balanced/Accurate model has enough new samples, extracts the required contiguous window, and calls `aria_run_inference()`.
7. **Dynamic timing:** measures the inference that actually ran with `DWT->CYCCNT`.
8. **Meta features:** updates confidence average/trend, signal delta, signal RMS, and measured latency.
9. **Learned routing:** runs the trained score-based Meta-Controller policy when no transient latch has temporary priority.
10. **Live telemetry:** sends the latest ADS voltage and current active model about every 100 ms; confidence and latency correspond to the most recent successful real inference.

On inference failure, the ADS path falls back to Fast and clears pending transition/transient state so the controller can recover cleanly.

### 8.2 ADS1263 Register Configuration Used

| Register | Value | Purpose |
|---|---|---|
| `MODE2` (0x05) | `0x8D` | Sets PGA/data-rate configuration |
| `MODE1` (0x04) | `0x00` | Default filter/sinc configuration |
| `MODE0` (0x03) | `0x00` | Default conversion mode |
| `REFMUX` (0x0F) | `0x24` | Selects internal 2.5V reference |
| `INPMUX` (0x06) | `0x0A` | Selects analog input channel routing |

Sequence: hardware reset pulse (RST low 10ms → high) → `CMD_RESET` → read device ID register (`0x00`) → write MODE2/MODE1/MODE0/REFMUX/INPMUX → `CMD_START1` to begin continuous conversion.

### 8.3 ICM-20948 Task Responsibilities (`imu_thread_entry.c`)

The IMU task performs the equivalent full runtime pipeline:

1. **SPI / synchronization setup**: creates the static SPI completion semaphore, opens the ICM's SPI instance (§8.5), and waits for callback-driven transaction completion.
2. **Identity and bring-up**: probes `WHO_AM_I` (§8.6), disables the I²C interface for SPI operation, resets and wakes the sensor, configures accel/gyro registers, and returns to Bank 0.
3. **Six-axis acquisition**: reads 12 consecutive data bytes starting at `ACCEL_XOUT_H` (§8.7), reconstructing signed 16-bit `ax/ay/az/gx/gy/gz`.
4. **Engineering-unit conversion**: converts accelerometer samples to `g` and gyroscope samples to `deg/s`.
5. **Frame buffering**: stores six floats per frame in the ICM circular capture buffer at 100 Hz (§8.8).
6. **Variant-sized inference trigger**: calls `imu_run_inference_get_required_floats(variant_used)`, derives the required frame count, and extracts a contiguous six-axis inference window.
7. **Inference + timing**: runs `aria_run_inference_imu()` and measures its real DWT/CYCCNT inference duration.
8. **Runtime feature extraction**: updates fault-confidence history, confidence trend/average, legacy motion features, gravity-removed accelerometer AC RMS and gyroscope RMS (§8.9).
9. **Adaptive motion routing**: still → Fast, moderate motion → Balanced, strong motion → Accurate; the trained ICM meta-controller may refine only the intermediate region (§7.5's ADS-side equivalent describes the same confirmation/hold-window pattern applied here to motion).
10. **Telemetry**: reports the model that actually produced the current fault confidence and measured latency while the newly selected variant is reserved for the next inference window.

### 8.4 ICM-20948 Register Configuration Used

The ICM-20948 uses register banks selected through `REG_BANK_SEL`.

#### Bank 0

| Register | Address | Value / Use | Purpose |
|---|---:|---|---|
| `WHO_AM_I` | `0x00` | expected `0xEA` | Sensor identity verification |
| `USER_CTRL` | `0x03` | set `I2C_IF_DIS = 0x10` | Disable I²C interface for SPI operation |
| `PWR_MGMT_1` | `0x06` | `0x80` then `0x01` | Device reset, then wake / clock selection |
| `PWR_MGMT_2` | `0x07` | `0x00` | Enable accelerometer and gyroscope |
| `ACCEL_XOUT_H` | `0x2D` | read start address | First byte of 12-byte accel + gyro sample block |
| `REG_BANK_SEL` | `0x7F` | `bank << 4` | Select register bank |

#### Bank 2

| Register | Address | Value Used | Purpose |
|---|---:|---:|---|
| `GYRO_SMPLRT_DIV` | `0x00` | `0x00` | Gyroscope sample-rate divider |
| `GYRO_CONFIG_1` | `0x01` | `0x00` | Gyroscope full-scale / filter configuration used by firmware |
| `ACCEL_SMPLRT_DIV_1` | `0x10` | `0x00` | Accelerometer divider MSB |
| `ACCEL_SMPLRT_DIV_2` | `0x11` | `0x00` | Accelerometer divider LSB |
| `ACCEL_CONFIG` | `0x14` | `0x00` | Accelerometer full-scale / filter configuration used by firmware |

Initialization sequence:

```text
WHO_AM_I == 0xEA
    ↓
Bank 0
    ↓
USER_CTRL.I2C_IF_DIS = 1
    ↓
PWR_MGMT_1 = 0x80
    ↓
250 ms reset delay
    ↓
PWR_MGMT_1 = 0x01
PWR_MGMT_2 = 0x00
    ↓
re-assert USER_CTRL.I2C_IF_DIS
    ↓
Bank 2
    ↓
configure gyro + accel divider/config registers
    ↓
Bank 0
    ↓
continuous 12-byte accel + gyro reads
```

The I²C-disable bit is deliberately re-applied after reset because reset clears interface-control state.

### 8.5 ICM-20948 SPI Instance Configuration

The ICM path runs on its own dedicated FSP SPI instance, confirmed working with the following configuration:

```text
channel      = 0
mode         = master
SPI mode     = mode 0
bit order    = MSB first
communication= full duplex
SSL          = SSL2 / P705
SSL polarity = active low
interface    = SPI_B_SSL_MODE_SPI (4-wire)
bitrate      ≈ 1 MHz
```

A critical integration detail is that the SPI-B operation must be configured as **4-wire SPI**:

```c
.spi_clksyn = SPI_B_SSL_MODE_SPI
```

and not the clock-synchronous/3-wire mode. Getting this wrong is what originally caused the `WHO_AM_I = 0x00` symptom documented in §12.6.

### 8.6 WHO_AM_I Probe

The SPI read convention sets bit 7 of the register address:

```c
tx[0] = REG_WHO_AM_I | 0x80;
tx[1] = 0x00;
```

Expected response:

```text
RX[1] = 0xEA
```

The firmware retries the identity probe up to 10 times with a short delay between attempts.

Debugger-visible diagnostics include:

```text
g_imu_who_am_i
g_imu_who_am_i_attempts
g_imu_last_tx[]
g_imu_last_rx[]
g_imu_spi_cb_fire_count
g_imu_last_spi_event
g_imu_stage
```

These proved useful for distinguishing a completed SPI transaction from a physical wiring / MISO / CS problem.

### 8.7 Six-Axis Data Read

ARIA reads 12 consecutive bytes beginning at:

```text
ACCEL_XOUT_H = 0x2D
```

and reconstructs:

```text
accel X
accel Y
accel Z
gyro X
gyro Y
gyro Z
```

The raw signed 16-bit values are converted using the configured full-scale conversion constants:

```text
accelerometer → g
gyroscope     → degrees/second
```

The six converted floats are placed into the ICM frame ring buffer.

### 8.8 IMU Capture Buffer and Inference Call

The task maintains:

```text
1400 frames × 6 floats/frame
```

of circular capture capacity.

Each model requests the amount of history it needs. The latest frames are copied into a contiguous inference window before dispatch.

The IMU inference call is:

```c
aria_run_inference_imu(
    variant_used,
    g_imu_inference_window,
    &result,
    false);
```

### 8.9 IMU Motion Detection Debug Signals

The physical motion controller does **not** use the gravity-contaminated absolute acceleration vector directly.

Instead, the accelerometer window mean is removed per axis before RMS calculation. The gyroscope retains absolute RMS because constant angular rotation is itself real motion.

Debugger-visible motion signals:

```text
g_imu_debug_accel_ac_rms_g
g_imu_debug_gyro_rms_dps
g_imu_debug_motion_band
```

Motion-band meaning:

```text
0 = still
1 = moderate
2 = strong
```
---

## 9. Inference Dispatch Architecture

### 9.1 Why the Dispatcher Is Merged

The final project intentionally keeps the ADS and IMU inference implementations inside the merged C++ inference translation unit.

This is important because generated Edge Impulse SDK headers contain function bodies and global/generated model content. Splitting the combined generated environment across multiple independent C++ translation units produced duplicate-definition/linker conflicts, the same class of problem documented in §12.10.

The public C-facing headers remain small:

```text
run_inference_dispatcher.h
run_inference_dispatcher_imu.h
```

and deliberately avoid pulling the full Edge Impulse SDK into the FreeRTOS C application threads.

### 9.2 ADS Inference Path

ADS uses the generated Edge Impulse runtime path and per-variant model handles.

```text
ADC inference window
    ↓
aria_run_inference()
    ↓
selected Fast / Balanced / Accurate model
    ↓
p_anomaly / p_normal
```

### 9.3 ICM Inference Path

The IMU integration required extra isolation because the generated spectral metadata from the two model sets is not interchangeable (§12.11). The final IMU path uses:

- local spectral feature generation matching the ICM model configuration;
- six-axis features;
- direct compiled EON graph invocation;
- quantization / dequantization using generated tensor scale and zero point;
- three output probabilities: fault, idle and normal.

This is the same inference path affected by the task-stack-exhaustion hard fault documented in §12.9; increasing the IMU thread's stack resolved it while retaining Cortex-M85 MVE/Helium acceleration for the model's kernels.

---

## 10. Repository / Project Structure

```
aria_v2/
├── ra/
│   └── aws/
│       └── FreeRTOS/
│           └── FreeRTOS/
│               └── Source/
│                   ├── include/                     ← FreeRTOS kernel headers
│                   └── portable/
│                       ├── MemMang/heap_4.c          ← heap implementation (fetched from FreeRTOS-Kernel repo)
│                       └── GCC/ARM_CM85/... (or ARM_CM33_NTZ, depending on core config)
├── src/
│   ├── adc_thread_entry.c                            ← ADS1263 sampling + inference dispatch task
│   ├── aria-ads-merged/
│   │   ├── edge-impulse-sdk/                         ← vendored Edge Impulse C++ SDK
│   │   │   ├── tensorflow/lite/micro/...             ← TensorFlow Lite Micro kernels
│   │   │   ├── CMSIS-NN/ , CMSIS-DSP/                 ← ARM CMSIS optimized kernels
│   │   │   ├── classifier/                           ← ei_run_classifier, ei_aligned_malloc, ei_data_normalization
│   │   │   ├── dsp/                                  ← ei_alloc, numpy, spectral filters, flatten
│   │   │   └── porting/renesas-ra/                   ← MCU-specific porting shim (ei_*, malloc/printf bridge)
│   │   ├── model-parameters/model_variables.h        ← trained weights + scaler params
│   │   ├── tflite-model/
│   │   │   ├── tflite_model_compiled_fast.cpp/.h
│   │   │   ├── tflite_model_compiled_balanced.cpp/.h
│   │   │   ├── tflite_model_compiled_accurate.cpp/.h
│   │   │   └── trained_model_ops_define_{fast,balanced,accurate}.h
│   │   └── run_inference_dispatcher.{cpp,h}            ← aria_run_inference(), variant dispatch
├── script/fsp.ld                                     ← linker script
```
### 10.1 Model / Firmware Tree

The exact shell transcript above is preserved as captured. Expanded logically, the relevant source tree is:

```text
src/
├── adc_thread_entry.c
├── adc_thread.h
├── imu_thread_entry.c
├── imu_thread.h
├── meta_controller_weights.h
├── meta_controller_weights_ICM.h
├── telemetry.c
├── telemetry.h
├── aria-ads-merged/
│   ├── edge-impulse-sdk/
│   ├── ei_classifier_porting_ra.cpp
│   ├── model-parameters/
│   │   ├── model_metadata.h
│   │   └── model_variables.h
│   ├── tflite-model/
│   │   ├── tflite_model_compiled_fast.cpp
│   │   ├── tflite_model_compiled_fast.h
│   │   ├── tflite_model_compiled_balanced.cpp
│   │   ├── tflite_model_compiled_balanced.h
│   │   ├── tflite_model_compiled_accurate.cpp
│   │   ├── tflite_model_compiled_accurate.h
│   │   └── trained_model_ops_define.h
│   ├── run_inference_dispatcher.cpp
│   └── run_inference_dispatcher.h
└── aria-imu-merged/
    ├── model-parameters/
    │   ├── model_metadata_ICM.h
    │   └── model_variables_ICM.h
    ├── tflite-model/
    │   ├── tflite_model_compiled_icm_fast.cpp
    │   ├── tflite_model_compiled_icm_fast.h
    │   ├── tflite_model_compiled_icm_balanced.cpp
    │   ├── tflite_model_compiled_icm_balanced.h
    │   ├── tflite_model_compiled_icm_accurate.cpp
    │   └── tflite_model_compiled_icm_accurate.h
    └── run_inference_dispatcher_imu.h
```

### 10.2 Important Source Files

| File | Purpose |
|---|---|
| `src/adc_thread_entry.c` | ADS1263 acquisition, buffering, inference, runtime features and model routing |
| `src/imu_thread_entry.c` | ICM-20948 bring-up, six-axis acquisition, inference, motion features and routing |
| `src/meta_controller_weights.h` | ADS trained meta-controller weights |
| `src/meta_controller_weights_ICM.h` | ICM trained meta-controller weights |
| `src/telemetry.c` | Shared UART telemetry transport |
| `src/telemetry.h` | ADS / IMU telemetry interface |
| `src/aria-ads-merged/run_inference_dispatcher.cpp` | Combined inference implementation required by the merged Edge Impulse build |
| `src/aria-ads-merged/run_inference_dispatcher.h` | ADS dispatcher API |
| `src/aria-imu-merged/run_inference_dispatcher_imu.h` | ICM dispatcher API |
| `src/aria-imu-merged/model-parameters/model_metadata_ICM.h` | ICM generated model/DSP metadata |
| `src/aria-imu-merged/model-parameters/model_variables_ICM.h` | ICM model variables / generated parameters |

---

## 11. Toolchain, Build System & Environment

| Tool | Version / Detail |
|---|---|
| IDE | Renesas e² studio 2025-04 |
| FSP | v5.9.0 |
| Compiler | `arm-none-eabi-g++`/`gcc` 13.2.1 (arm-gnu-toolchain-13.2.Rel1) |
| Target flags | `-mthumb -mfloat-abi=hard -mcpu=cortex-m85+nopacbti -O2` |
| Warnings enabled | `-Wall -Wextra -Wunused -Wuninitialized -Wmissing-declarations -Wconversion -Wpointer-arith -Wshadow -Wlogical-op -Waggregate-return -Wfloat-equal` |
| Linker | `arm-none-eabi-g++` with `-T ../script/fsp.ld --gc-sections --specs=nano.specs` |
| ML export path | Edge Impulse Studio → EON Compiler (C++ library export, int8 quantized) |

## 11.1 Why ARIA Is a Genuine Arm-Native, Fully-Local Edge System

It's easy to claim "runs on Arm." Here's exactly what makes that true, sourced
directly from the FSP project configuration rather than asserted:

**Cross-compiled, not cross-run.** The entire firmware is cross-compiled on a development host using `arm-none-eabi-gcc`/`g++` 13.2.1, targeting `-mcpu=cortex-m85+nopacbti -mthumb -mfloat-abi=hard`, and flashed as a FreeRTOS-based ELF image directly onto the EK-RA8D1 (`R7FA8D1BHECBD`) via J-Link/E2 Lite. FreeRTOS handles task scheduling and ISR-to-task signaling. The `adc_thread` runs at priority 1 with a statically-allocated 8192-word stack, on a 1000 Hz tick, backed by a heap_4 allocator with a 20480-byte static heap, but there is no general-purpose OS beneath it. This is a real embedded cross-toolchain build, not a Python script or container on an Arm64 cloud VM.

**Helium (MVE) confirmed active, three independent ways.** Rather than assert MVE usage, we verified it: 

(1) the compiler's resolved target architecture is `armv8.1-m.main+fp.dp+mve.fp`

(2) the preprocessor defines `__ARM_FEATURE_MVE 3` at build time

(3) the linked ELF's build attributes show `Tag_MVE_arch: MVE Integer and FP`. 

CMSIS-NN's quantized int8 kernels (for example fully-connected, softmax, and pooling kernels used by the generated models) execute through this vector unit where applicable. The scalar-only instructions visible in the top-level EON wrapper are expected and by design; the MVE instructions live inside the CMSIS-NN kernel objects themselves.

**SPI: real register-level sensor bring-up, with DMA offload.** The ADS1263 is driven over **SPI channel 1** (`g_spi0`, on pins P410-P413) configured as master, full-duplex, MSB-first, at a **1 MHz bitrate**, CPOL=low / CPHA=edge-even, with chip-select handled directly by the peripheral's hardware `SSLB0` line rather than a manually toggled GPIO. Both the TX and RX paths are offloaded to the RA8D1's **DTC (Data Transfer Controller)**, `g_transfer0` / `g_transfer1`, so SPI bytes move without the CPU babysitting every transfer, and the SPI RXI/TXI/TEI/ERI interrupts run at priority level 2. On top of this, the ADS1263 gets full register-level configuration (MODE0-2, REFMUX, INPMUX) and `DRDY`-pin monitoring, production-style sensor integration, not a vendor demo API call.

**DRDY is intentionally polled, not interrupt-driven, and that's documented, not hidden.** DRDY connects to P010(`MIKROBUS_INT`), configured as a bare GPIO input. The RA8D1 does have a true external-interrupt-capable pin available on this board (P508/`PMOD2_7_INT`, wired to IRQ channel 14 with a pull-up), but it isn't the one this sensor uses. The ADC thread polls DRDY via `R_IOPORT_PinRead()` on its own schedule (§8.1). We're calling this out explicitly rather than letting the README imply interrupt-driven acquisition it doesn't have.

**UART: telemetry, never a dependency.** The ESP32 bridges live telemetry (active model, confidence, latency) to a browser dashboard over UART + Wi-Fi. This path is a *convenience layer for visualization*. It is never in the critical path of an actual inference decision, and it lives entirely in the ESP32-side Arduino sketch rather than the RA8D1's FSP project.

**Inference runs entirely in the microcontroller's own memory.** All **six** quantized model variants — ADS Fast/Balanced/Accurate plus ICM Fast/Balanced/Accurate — are statically compiled into the firmware image and linked directly into flash. Nothing is loaded from external storage at runtime. Model switching is a dispatcher choice between already-linked graphs, so there is no model reload delay, no cloud call, no external inference server, and no network dependency anywhere in the decision loop. Wi-Fi/UART can be disconnected without stopping local inference or model selection; they are used only for telemetry.

---

## 12. The Build Journey: Every Problem We Hit and How We Fixed It

This section documents, in order, the real build failures encountered while integrating a vendored Edge Impulse / TensorFlow Lite Micro SDK into a bare-metal Renesas FSP + FreeRTOS project, and exactly what was done to resolve each one.

### 12.1 Missing FreeRTOS Heap Implementation

**Symptom:** Build failed because no heap manager (`heap_1`-`heap_5`) was present under `Source/portable/MemMang/`, so `pvPortMalloc`/`vPortFree` were undefined.

**Fix:** Fetched the canonical `heap_4.c` (first-fit with coalescing, suitable for long-running systems with mixed alloc/free sizes) directly from the official FreeRTOS-Kernel GitHub repository into `Source/portable/MemMang/heap_4.c`:

```bash
mkdir -p ".../FreeRTOS/Source/portable/MemMang"
wget -O ".../MemMang/heap_4.c" \
  "https://raw.githubusercontent.com/FreeRTOS/FreeRTOS-Kernel/main/portable/MemMang/heap_4.c"
```

Confirmed via HTTP 200 / 24,498 bytes downloaded successfully.

### 12.2 Missing Portable Layer (`port.c`, `portmacro.h`)

**Symptom:** After fixing the heap, `find` searches confirmed the entire Cortex-M portable layer (`port.c`, `portmacro.h`. The files implementing context switching, `xPortStartScheduler`, `pxPortInitialiseStack`, `vPortSetupTimerInterrupt`) was also absent from both the project tree and the cached Renesas FSP installation. This meant heap_4.c alone would not be sufficient; the next build pass would fail on undefined scheduler symbols.

**Fix approach:** Identify the exact Cortex-M variant needed based on the EK-RA8D1's Cortex-M85 core (the correct upstream FreeRTOS-Kernel port directory, e.g. `GCC/ARM_CM85` or the closest compatible ARMv8.1-M variant depending on TrustZone/PACBTI configuration) and fetch `port.c` + `portmacro.h` from the same FreeRTOS-Kernel repository, mirroring the `heap_4.c` fetch pattern.

### 12.3 Undefined References to `ei_printf`, `ei_malloc`, `ei_calloc`, `ei_free`, `ei_read_timer_us`

**Symptom:** Full firmware link failed with dozens of `undefined reference` errors originating from the vendored Edge Impulse SDK object files (`tflite_model_compiled_{fast,balanced,accurate}.o`, `run_inference_dispatcher.o`, and deep inside `dsp/ei_alloc.h`, `dsp/spectral/filters.hpp`, `dsp/numpy.hpp`, `classifier/ei_aligned_malloc.h`, `classifier/ei_data_normalization.h`, `classifier/inferencing_engines/tflite_helper.h`):

```
undefined reference to `ei_printf(char const*, ...)'
undefined reference to `ei_calloc(unsigned int, unsigned int)'
undefined reference to `ei_malloc(unsigned int)'
undefined reference to `ei_free(void*)'
undefined reference to `ei_read_timer_us()'
```

**Root cause:** The Edge Impulse SDK is platform-agnostic by design, which is, it calls a small set of abstracted `ei_*` primitives for memory allocation, debug printing, and timing, and expects the **target platform's porting layer** to supply the concrete implementations. The SDK ships with pre-written porting shims for dozens of platforms (`porting/zephyr`, `porting/mbed`, `porting/arduino`, `porting/stm32-cubeai`, `porting/silabs`, `porting/posix`, `porting/renesas-ra`, etc.) which are visible directly in the linker's object list. However, the `renesas-ra` porting object (`porting/renesas-ra/ei_classifier_porting.o`, `porting/renesas-ra/debug_log.o`) either was not being compiled/linked into the final image, or its implementations were incomplete/stubbed for this FSP+FreeRTOS combination.

**Fix:** Ensured the `porting/renesas-ra/ei_classifier_porting.cpp` translation unit is included in the build (added to the `subdir.mk` source list / include path so its object file participates in the final `--start-group ... --end-group` link), providing real implementations of:
- `ei_printf()` --> routed to the project's existing debug UART/SEGGER RTT output.
- `ei_malloc()` / `ei_calloc()` / `ei_free()` --> routed either to the FreeRTOS heap (`pvPortMalloc`/`vPortFree`, now functional after the heap_4.c fix in §12.1) or to a dedicated static tensor arena, depending on call site.
- `ei_read_timer_us()` --> routed to the Cortex-M85 DWT cycle counter (the same `benchmark_dwt_get()` / `benchmark_cycles_to_us()` infrastructure already built for the ADC task's benchmarking, see §15), converting cycles to microseconds using `SystemCoreClock`.

This resolved all `eiprintf`, `eicalloc`, `eimalloc`, `eifree`, and `eireadtimerus` undefined-reference errors across `tflite_model_compiled_{fast,balanced,accurate}.o`, the DSP layer (`spectral::filters::butterworth_lowpass`/`butterworth_highpass`), `flatten_class`, `ei_data_normalization`'s `standard_scaler`, and `run_inference_dispatcher.o`.

### 12.4 Compiler Warning Noise During Bring-Up

**Symptom:** Builds produced 1,494+ warnings under the strict `-Wall -Wextra -Wconversion -Wshadow -Wfloat-equal` flag set, spanning: unused parameters in TFLite Micro's `MicroMemoryPlanner`/`MicroContext`/`OpResolver` virtual interfaces, `float`<-->`double` narrowing conversions when the trained `StandardScaler` weights (stored as `double` from Python/Edge Impulse export) are assigned into `const float` arrays in `model_variables.h`, a `-Wshadow` warning from a local `block` variable in `ei_run_classifier.h` shadowing an outer declaration, and floating-point equality comparisons (`scale == 0.0f`) used as "is this quantized?" checks in `tflite_helper.h`.

**Assessment:** These are all benign and expected consequences of vendoring a third-party, multi-platform SDK unmodified. They do not block linking and were left as-is (fixing them would mean patching upstream Edge Impulse/TFLite Micro source, which is avoided to keep the vendored SDK easily re-exportable/updatable from Edge Impulse Studio).

### 12.5 Successful Full Link

Once 12.1-12.3 were resolved, the final `arm-none-eabi-g++ ... -o ariav2.elf -Wl,--start-group ...` link step successfully pulled in and resolved: all three compiled TFLite model graphs, the full TensorFlow Lite Micro kernel library (100+ op kernels: conv, depthwise_conv, fully_connected, softmax, pooling, LSTM, SVDF, quantize/dequantize, etc.), CMSIS-NN quantized kernel implementations (s8/s16/q7/q15 convolution, matmul, softmax, pooling variants), CMSIS-DSP transform/support/statistics functions (FFT, MFCC, DCT, sorting, copy/fill utilities used by the DSP feature-extraction pipeline), the `renesas-ra` porting shim, and the application-level `adc_thread_entry.o` / `run_inference_dispatcher.o`.

### 12.6 ICM-20948 SPI Initially Returned `0x00`

The IMU initially failed the identity probe:

```text
WHO_AM_I = 0x00
```

while the SPI callback itself was firing.

The final solution required the complete physical + peripheral configuration to match:

```text
P700 = MISO0
P701 = MOSI0
P702 = RSPCK0
P705 = SSLA2
```

with:

```text
SPI_B_SSL_MODE_SPI
full duplex
Mode 0
MSB first
SSL2 active low
```

The final expected identity is:

```text
WHO_AM_I = 0xEA
```

### 12.7 ICM Reset Clears SPI-Interface Control State

`USER_CTRL.I2C_IF_DIS` must be re-asserted after reset. The working bring-up therefore disables I2C both before and after the reset/wake sequence.

### 12.8 SPI Callback Race

The SPI completion state must be armed **before** starting `R_SPI_B_WriteRead()`. Otherwise a very fast completion callback can set the success flag before the task clears it, making a successful transfer appear to fail.

### 12.9 IMU Inference Hard Fault

Direct EON graph invocation initially faulted inside inference.

Fault inspection showed the IMU FreeRTOS task PSP had reached `PSPLIM`, proving **task stack exhaustion**. Increasing the IMU task stack fixed the issue while retaining Cortex-M85 MVE/Helium acceleration.

### 12.10 Merged Edge Impulse Model Integration

The ADS and ICM generated model sets could not be treated as ordinary independent SDK translation units because generated headers provide definitions that conflict when included multiple times.

The final architecture keeps the model implementations within the merged dispatcher strategy and exposes minimal C APIs to the FreeRTOS tasks.

### 12.11 ICM Spectral DSP Metadata Conflict

The ICM model expects its own spectral configuration while the merged ADS environment carries different generated metadata.

The final dispatcher reproduces the required ICM spectral feature extraction locally rather than forcing incompatible global Edge Impulse spectral macros.

### 12.12 Stationary IMU Incorrectly Escalated

Using absolute acceleration magnitude as "motion RMS" included gravity, so a stationary module could appear highly active.

The deployed controller now removes per-axis acceleration DC/gravity before computing dynamic accelerometer RMS and also observes gyroscope RMS.

### 12.13 UART ADS / IMU Concurrency

ADS and IMU are independent FreeRTOS tasks but share one UART transport.

A static binary semaphore TX gate serializes access to the UART buffer and peripheral, preventing concurrent telemetry writes from corrupting each other.

---

## 13. Runtime Feature Extraction

The application layer maintains lightweight controller features in addition to each Edge Impulse model's own DSP pipeline.

For the current ADS controller:

- **Confidence history:** last **3** `p_anomaly` outputs.
- **Confidence average:** mean of those three values.
- **Confidence trend:** newest `p_anomaly` minus the previous value.
- **Signal delta:** current inference-window RMS minus previous inference-window RMS.
- **Signal RMS:** absolute RMS of the current inference window.
- **Inference latency:** real DWT-measured duration of the model that actually ran.

The trained ADS MLP consumes the first four learned inputs listed in §7.1 (`confidence_avg`, `confidence_trend`, `signal_delta`, `signal_rms`). `latency_us` remains a measured/logged runtime result but is intentionally excluded from the current MLP.

Separately, the ADS transient guard tracks **sample-to-sample absolute step magnitude** plus an exponential moving average of recent ordinary steps. These transient values are not fed into the frozen MLP; they are used only for the exceptional sudden-spike/dip override described in §7.4.

### 13.1 Meta-Controller Log Layout

The ADS binary training/runtime log records five little-endian floats per row:

```text
[0] confidence_avg
[1] confidence_trend
[2] signal_delta
[3] latency_us
[4] signal_rms
```

The binary row size is therefore:

```text
5 × 4 bytes = 20 bytes
```

The current `meta_controller_weights.h` was trained using columns 0, 1, 2, and 4; latency was excluded from the deployed MLP.

### 13.2 ADS Debug Visibility

The current ADS firmware exposes both learned and transient routing state:

```text
g_debug_meta_score_fast
g_debug_meta_score_balanced
g_debug_meta_score_accurate
g_debug_meta_argmax

g_debug_controller_candidate
g_debug_controller_target
g_debug_target_pending
g_debug_active_variant

g_debug_transient_abs_step_v
g_debug_transient_balanced_threshold_v
g_debug_transient_accurate_threshold_v
g_debug_transient_balanced_latched
g_debug_transient_accurate_latched
```

This makes it possible to distinguish a learned MLP request from a sudden-event override while debugging.

### 13.3 ICM Runtime Features

The ICM training-compatible log keeps the same five-float shape for consistency:

```text
fault confidence average
fault confidence trend
legacy signal/motion delta
inference latency
legacy signal/motion RMS
```

The deployed motion policy additionally tracks gravity-removed accelerometer AC RMS and gyroscope RMS so a truly stationary module is not confused with a fixed-orientation `~1 g` reading.

## 14. Meta-Controller Training Data Extraction

During a labeled training/data-collection run, place the firmware in the corresponding meta-training mode and let the desired condition run until the meta log has accumulated samples (the log's binary structure is documented in §13.1).

The binary log can then be extracted directly from the **GDB debugger console**.

### 14.1 ADS Meta-Controller Log

Use the following command exactly in the "Debugger Console", after doing a test run for training the meta-controller, when "suspend" is clicked. This command is used to retrieve the log:

```gdb
dump binary memory "YOUR_DESIRED_PATH/YOUR_DESIRED_NAME.bin" &g_meta_log ((char *)&g_meta_log + sizeof(g_meta_log))
```

Example:

```gdb
dump binary memory "/home/rishi/ARIA_DATA/meta_log_still.bin" &g_meta_log ((char *)&g_meta_log + sizeof(g_meta_log))
```

### 14.2 ICM-20948 Meta-Controller Log

The IMU task uses the corresponding symbol:

```gdb
dump binary memory "YOUR_DESIRED_PATH/YOUR_DESIRED_NAME.bin" &g_imu_meta_log ((char *)&g_imu_meta_log + sizeof(g_imu_meta_log))
```

Example:

```gdb
dump binary memory "/home/rishi/ARIA_DATA/meta_log_gentle.bin" &g_imu_meta_log ((char *)&g_imu_meta_log + sizeof(g_imu_meta_log))
```

### 14.3 Recommended Collection Procedure

1. Set the appropriate meta-training-mode macro to `1`.
2. Rebuild and flash the RA8D1.
3. Run one clearly labeled physical condition.
4. Allow the log buffer to accumulate.
5. Halt the target in the debugger.
6. Use `dump binary memory ...` from the **GDB debugger console**.
7. Convert the `.bin` file to CSV with the project conversion script.
8. Label the session.
9. Repeat for the next physical condition.
10. Train / validate the meta-controller.
11. Export the generated weights header.
12. Set the corresponding training-mode macro back to `0` for deployment.

For repeatable validation, sessions should remain identifiable instead of randomly mixing every row across train/test splits.

---

## 15. Benchmarking Infrastructure

A DWT (Data Watchpoint and Trace)-based cycle-accurate benchmarking layer was built directly into `adc_thread_entry.c`, since the Cortex-M85 exposes a free-running 32-bit cycle counter (`DWT->CYCCNT`) once trace is enabled via `CoreDebug->DEMCR`:

- `benchmark_dwt_init()`: enables `TRCENA`, zeroes and starts `CYCCNT`.
- `benchmark_cycles_to_us()`: converts a cycle delta (handling 32-bit wraparound) to microseconds using `SystemCoreClock`.
- `bench_stats_t`: a lightweight running-statistics struct (count, last, min, max, sum) updated via `benchmark_record()`, with `benchmark_avg_us()` for the mean.

Four independent timing channels are tracked and exposed as debugger-visible `volatile uint32_t` globals (refreshed every 20 samples via `benchmark_refresh_globals()`):

| Channel | What it measures |
|---|---|
| `g_bench_spi_txn_*` | Pure SPI transaction cost (`R_SPI_B_WriteRead` + semaphore wait) |
| `g_bench_sample_*` | Full sample acquisition + inference dispatch cost |
| `g_bench_loop_*` | Full main-loop iteration cost |
| `g_bench_infer_*` | Pure `aria_run_inference()` call cost |

This same infrastructure was reused to implement `ei_read_timer_us()` in the Renesas-RA porting shim (§12.3), keeping timing consistent across the application and SDK layers. §16 covers how this same DWT counter feeds the live telemetry latency figures shown on the dashboard.

---

## 16. Dynamic Latency Measurement

Latency displayed by ARIA is **not hardcoded**.

Both sensor inference paths use the Cortex-M85 DWT cycle counter (the same infrastructure described in §15).

Initialization:

```c
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
DWT->CYCCNT = 0;
DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
```

Inference timing:

```c
uint32_t t0 = DWT->CYCCNT;

/* run Fast / Balanced / Accurate inference */

uint32_t t1 = DWT->CYCCNT;
```

Cycle count is converted to microseconds using:

```text
SystemCoreClock
```

The telemetry protocol preserves both exact microseconds and decimal milliseconds:

```json
{
  "latency_us": 842,
  "latency_ms": 0.842
}
```

This avoids the earlier integer truncation problem where any inference below 1 ms appeared as `0 ms`.

---

## 17. How to Build and Flash

1. Open the project in **Renesas e² studio 2025-04** with **FSP 5.9.0** configured for the EK-RA8D1 (Cortex-M85) board.
2. Ensure the FreeRTOS portable layer is complete: `Source/portable/MemMang/heap_4.c` present (§12.1) and the matching Cortex-M port directory (`port.c` + `portmacro.h`, §12.2) present under `Source/portable/GCC/<variant>/`.
3. Ensure `porting/renesas-ra/ei_classifier_porting.cpp` is included in the build's source list so `ei_printf`/`ei_malloc`/`ei_calloc`/`ei_free`/`ei_read_timer_us` resolve (§12.3).
4. Build (`make -r -j8 all` under the hood via e² studio); Target output is `ariav2.elf`.
5. Flash via the on-board debugger (J-Link/E2 Lite) to the EK-RA8D1.
6. Open a debugger Expressions view and add `g_adc_voltage`, `g_active_variant`, `g_meta_features`, the `g_bench_*` globals, and the `g_debug_meta_*` / `g_debug_transient_*` variables to observe learned routing, sudden-event overrides, and real inference timing without needing UART.

---

## 18. Demo Script

1. Start with the ADS1263 signal in a stable condition and show that the system remains under normal learned Meta-Controller routing.
2. Create a **moderate sudden** voltage step/dip and show **Balanced** being selected immediately; keep the dashboard visible until a real Balanced inference completes and the transient latch releases.
3. Create a **strong sudden** voltage step/dip and show **Accurate** being selected immediately; again show that the tier is held until a real Accurate inference completes.
4. Point out that a gradual voltage change does **not** automatically trigger the transient override — the new advancement is specifically for sudden spikes/dips.
5. Show the ICM-20948 stream: still -> Fast, moderate motion -> Balanced, strong motion -> Accurate.
6. Highlight that both sensor model zoos are already present in the same RA8D1 flash image and every inference decision is local; the ESP32 only visualizes telemetry.
7. Close with: **"Same adaptive inference skeleton, two very different physical sensors, six resident models, one Cortex-M85."**

## 19. Toolchain, Build System and Environment

| Tool | Version / Detail |
|---|---|
| Target | Renesas EK-RA8D1 |
| CPU | Arm Cortex-M85 |
| IDE | Renesas e² studio |
| FSP | 5.9.0 |
| FreeRTOS | 11.1.0 |
| Compiler | GCC / G++ 13.2.1 |
| ML tooling | Edge Impulse / EON Compiler |
| Debug probe | J-Link-compatible RA8D1 debug configuration |
| Telemetry bridge | ESP32 UART at 115200 baud |

The project is built with Cortex-M85/Helium support enabled project-wide.

---

## 20. Submission Checklist

- [ ] Public GitHub repo, MIT/Apache 2.0 license visible
- [ ] Written project overview + setup instructions (this README)
- [ ] Track declared: Physical AI
- [ ] All source code included: model zoo training, meta-controller training, RA8D1 firmware, ESP32 firmware
- [ ] Benchmark table: latency/accuracy/power per model variant, per demo
- [ ] 3-minute demo video showing full demo script on real hardware
- [ ] Written case for scalability, referencing the final demo set as proof of generality

---

## 21. License

To be finalized as MIT or Apache 2.0 prior to submission, per the Arm AI Optimization Challenge 2026 submission requirements.

---

## 22. Developers

Sachi Patwardhan [linkedIn](https://www.linkedin.com/in/sachi-patwardhan-8a560430b/)
Rishiraj Rakesh Kumar [GitHub](https://www.github.com/Rishi8520)

