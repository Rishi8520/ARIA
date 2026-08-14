# ARIA — Results & Judging Criteria Alignment

This document summarizes the **current, hardware-proven ARIA results** and maps them to the challenge criteria. The complete implementation details, wiring, training/log extraction, build history, and firmware architecture are documented in [README.md](./README.md).

## Results Snapshot

ARIA now demonstrates a complete on-device adaptive inference stack on the **Renesas EK-RA8D1 / Arm Cortex-M85** using two structurally different physical sensors:

- **ADS1263:** single-channel precision analog anomaly inference.
- **ICM-20948:** six-axis accelerometer + gyroscope fault/motion inference.
- **Three real model tiers per sensor:** Fast, Balanced, Accurate.
- **Six EON-compiled model graphs resident in one firmware image.** No model is downloaded or reloaded when ARIA changes tiers.
- **All inference and routing runs locally on the RA8D1.** ESP32/Wi-Fi is telemetry and visualization only.
- **Live DWT-measured inference latency**, not a hardcoded dashboard value.
- **Dual-stream dashboard** shows the two sensors independently.
- **ADS learned routing now includes a sudden-transient response path:** moderate abrupt spike/dip -> Balanced; stronger abrupt spike/dip -> Accurate. The transient override happens **only for sudden changes**, not continuously and not for every ordinary sample.

## System Architecture

<img width="1000" height="420" alt="aria_pipeline_diagram" src="https://github.com/user-attachments/assets/6cd6d914-0296-40a4-85de-ed9c45a189d1" />

*High-level ARIA skeleton. The deployed implementation keeps this common model-zoo/controller/feedback structure, while ADS and ICM use sensor-specific runtime features and event guards.*

The core flow is:

```text
sensor -> runtime features -> meta controller -> model zoo -> real output
                                      |                        |
                                      +------ feedback --------+
```

A key implementation result is that the model zoo is **already present in flash**. A model switch is a dispatcher decision between pre-linked graphs, not a model-loading operation.

---

## Scorecard

| Criterion | Points | Where ARIA delivers |
|---|---:|---|
| Technological Implementation | 40 | Cortex-M85 + FreeRTOS, dual SPI sensors, six on-device model graphs, learned/meta routing, transient/motion guards, DWT timing, dual-stream telemetry |
| "WOW" Factor | 25 | Live Fast/Balanced/Accurate switching on two very different physical sensors with real latency visible |
| Potential Impact | 20 | Reusable model-zoo/controller/dispatcher pattern with no runtime model loading or cloud dependency |
| UX / Developer Experience | 15 | Pin-level docs, debugger-visible controller state, live dashboard, reproducible data extraction/training path |

---

## 1. Technological Implementation — 40 pts

### 1.1 Model-Zoo Results

| Demo | Variant | Edge Impulse test accuracy | Compiled model object flash¹ | Runtime latency |
|---|---|---:|---:|---|
| ADS1263 | Fast | 83.3% | 3.50 KiB | measured live with DWT |
| ADS1263 | Balanced | 89.8% | 4.93 KiB | measured live with DWT |
| ADS1263 | Accurate | 96.3% | 15.48 KiB | measured live with DWT |
| ICM-20948 | Fast | 90.1% | 3.87KiB | measured live with DWT |
| ICM-20948 | Balanced | 89.1% | 4.03KiB | measured live with DWT |
| ICM-20948 | Accurate | 98.8% | 5.30KiB | measured live with DWT |

¹ Compiled model object flash contribution calculated as `text + data` from the individual linked `.o` files using `arm-none-eabi-size`. This is more representative of the compiled firmware contribution than generated C/C++ source-file size, but it does not include all shared Edge Impulse/TFLM/CMSIS runtime code used by the models.

The complete ARIA firmware, with all six model variants linked simultaneously together with both meta-controllers, FreeRTOS, DSP/runtime support, sensor drivers and telemetry, occupies approximately 288.6 KiB of linked flash content (`text + data`) in the final ELF.

### 1.2 Seven Models in One Flash Image — One of ARIA's Biggest Practical Advantages

The complete model set is linked into one RA8D1 firmware image:

```text
ADS1263 model zoo
  Fast
  Balanced
  Accurate

ICM-20948 model zoo
  Fast
  Balanced
  Accurate

Meta-Controller Engine
```

This gives ARIA several concrete embedded-system advantages:

- **No runtime model download.**
- **No external flash/file-system model loading.**
- **No reboot between tiers.**
- **No network or ESP32 dependency for inference.**
- **No model-loading latency in the adaptive decision loop.**
- **Deterministic switching:** the dispatcher invokes a different already-linked graph.
- **The MCU can change compute effort at runtime while staying inside one flashed application.**

This is more than a dashboard effect: the selected Fast, Balanced, or Accurate graph genuinely executes on the Cortex-M85.

### 1.3 ADS Learned Meta-Controller Result

The current ADS Meta-Controller is a trained:

```text
4 inputs -> 16 ReLU hidden units -> 3 outputs
```

with:

```text
0 = Fast
1 = Balanced
2 = Accurate
```

Inputs:

```text
confidence_avg
confidence_trend
signal_delta
signal_rms
```

The current exported header was trained on **354 labeled runtime rows** and reports **0.791 full-data training accuracy**. Runtime firmware exposes all three learned output scores instead of hiding them behind only an argmax:

```text
g_debug_meta_score_fast
g_debug_meta_score_balanced
g_debug_meta_score_accurate
g_debug_meta_argmax
```

This makes the learned routing decision inspectable in the debugger.

### 1.4 Latest ADS Advancement — Sudden-Transient Escalation

The latest ADS firmware solves a real limitation of inference-window features: a short spike can be visible in live voltage telemetry but become diluted inside a 100–300 sample model window.

The solution is an adaptive sample-level transient guard based on:

```text
abs(current_sample - previous_sample)
```

relative to the recent ordinary step-size baseline.

Current transient severity bands are:

```text
Moderate sudden change:
  threshold = max(0.100 V, 3 x recent mean absolute step)
  -> BALANCED

Strong sudden change:
  threshold = max(0.650 V, 6 x recent mean absolute step)
  -> ACCURATE
```

**Important result/behavior:** this new advancement does **not** force Balanced or Accurate all the time. It only takes priority when the signal makes a sufficiently **sudden spike or dip**. Gradual changes and normal operation continue through the trained Meta-Controller / learned-score scheduler.

```text
normal / gradual signal
        |
        v
trained Meta-Controller
        |
        +--> Fast / Balanced / Accurate

moderate sudden spike/dip
        |
        v
Balanced immediately
        |
        v
real Balanced inference completes
        |
        v
latch releases -> learned routing resumes

strong sudden spike/dip
        |
        v
Accurate immediately
        |
        v
real Accurate inference completes
        |
        v
latch releases -> learned routing resumes
```

Both directions are handled because the detector uses absolute **change**, not absolute voltage. Therefore a sudden positive spike and a sudden negative dip are treated symmetrically.

The Accurate transient has priority over Balanced: a stronger event can preempt a pending Balanced latch, while a moderate event cannot demote an active Accurate latch.

This should be described accurately as **learned routing with an adaptive transient guard**, not as a completely threshold-free controller. The guard is also not the old absolute-voltage policy: it does not say that a specific voltage level always equals a specific model.

### 1.5 ICM-20948 Result

The ICM path is fully operational over its own SPI0 peripheral:

```text
P700 = MISO0
P701 = MOSI0
P702 = RSPCK0
P705 = SSLA2 / CS
```

Working integration includes:

- `WHO_AM_I = 0xEA`.
- 100 Hz six-axis sampling.
- accel + gyro conversion to engineering units.
- six-axis Spectral Analysis, FFT-16, overlap + log.
- 78 DSP features.
- three output labels: fault / idle / normal.
- int8 EON-compiled inference.
- Fast = 100 x 6 = 600 floats.
- Balanced = 100 x 6 = 600 floats.
- Accurate = 200 x 6 = 1200 floats.

The deployed IMU policy removes per-axis acceleration DC/gravity before computing dynamic accelerometer activity and also uses gyroscope RMS, preventing a stationary `~1 g` orientation from being mistaken for strong motion.

### 1.6 Dynamic Latency Is Real

Latency on the dashboard is measured around the model that actually ran using the Cortex-M85 DWT cycle counter:

```text
DWT->CYCCNT before inference
        |
        v
Fast / Balanced / Accurate inference
        |
        v
DWT->CYCCNT after inference
        |
        v
cycles -> microseconds -> telemetry
```

The telemetry keeps both exact microseconds and decimal milliseconds. This is important because switching model tiers should visibly change real compute cost; ARIA does not substitute a fixed model-latency table for runtime measurement.

### 1.7 Arm-Native Implementation

- Built with `arm-none-eabi-gcc` / `g++` 13.2.1 for Cortex-M85.
- FreeRTOS runs the application tasks and synchronization.
- Helium/MVE is enabled project-wide and confirmed in compiler/build attributes.
- ADS1263 and ICM-20948 use separate hardware SPI instances.
- ADS SPI uses DTC transfer support.
- ICM uses direct register-bank-aware SPI bring-up and direct EON execution with its own spectral feature path.
- ESP32 is outside the inference critical path.

### 1.8 Integration Problems Solved

The README documents 13 real bring-up/integration failures and their fixes, including:

- missing FreeRTOS heap/portable components,
- unresolved Edge Impulse porting functions,
- ICM `WHO_AM_I = 0x00`,
- SPI callback timing/race behavior,
- IMU task stack exhaustion causing inference hard faults,
- generated Edge Impulse multiple-definition conflicts,
- ADS/ICM spectral metadata conflict,
- gravity-contaminated IMU motion detection,
- concurrent ADS/ICM telemetry writes on one UART.

The UART concurrency issue is solved by a static binary semaphore TX gate.

---

## 2. "WOW" Factor — 25 pts

### Physical Prototype

<img width="5712" height="4284" alt="IMG_2589 2" src="https://github.com/user-attachments/assets/3967a7f2-0abe-46bd-8083-3e628b4eee0a" />

*Top view of the EK-RA8D1, breadboarded sensor/input circuitry, ICM-20948, ESP32 telemetry bridge, and connected development setup.*

### Dashboard

<img width="1600" height="900" alt="aria_telemetry" src="https://github.com/user-attachments/assets/8b32d95a-8b7b-48b8-9a86-f087aec2e9ac" />

*Telemetry live dashboard connected from EK-RA8D1 to ESP32 via UART.*

### Live System + Dashboard

<img width="5712" height="4284" alt="IMG_2587" src="https://github.com/user-attachments/assets/3d3f138d-7684-40e1-81e1-18a9d01acbcf" />

*ARIA running on the physical hardware while the laptop shows the live telemetry dashboard. The dashboard is a visualization layer; inference stays on the Cortex-M85.*

### Demo Video

*Demo run of ARIA.*
[Link](https://youtu.be/goghZ2gUtec)

### What Makes the Demo Stand Out

The strongest visual/technical story is not merely that a model predicts a class. It is that one microcontroller is simultaneously carrying **two complete three-tier model zoos**, and the runtime changes which real model graph executes in response to live physical behavior.

For ADS, the judge can see three distinct behaviors:

```text
ordinary / gradual behavior -> learned controller
moderate abrupt event       -> Balanced transient response
strong abrupt event         -> Accurate transient response
```

For ICM:

```text
still motion   -> Fast
moderate motion -> Balanced
strong motion   -> Accurate
```

The dashboard makes the active tier and measured latency visible, but the decision remains local to the RA8D1.

A concise closing line for the demo is:

> **Same adaptive inference skeleton, two very different physical sensors, six resident models, one Cortex-M85.**

---

## 3. Potential Impact — 20 pts

### The Framework Travels; the Demos Are Proof

| Use case | Signal behavior | ARIA mapping |
|---|---|---|
| Industrial predictive maintenance | Stable baseline, occasional abrupt vibration/current event | ADS or ICM model zoo |
| Battery / grid anomaly monitoring | Long steady periods, rare high-consequence transient | ADS1263 |
| Power-quality monitoring | Normal operation plus sharp disturbance | ADS transient-aware path |
| Process instrumentation | Mostly stable transducer output with rare event | ADS1263 |
| Precision lab/calibration | Long unattended run, occasional drift/event | learned low-compute routing with escalation |
| Machine/bearing health | normal periodic motion vs abnormal vibration | ICM-20948 |
| Vehicle/impact monitoring | calm motion vs sudden impact/roll event | ICM-20948 |

### Reuse Mechanism

The reusable part of ARIA is the **model-zoo + runtime-feature + controller + dispatcher + telemetry skeleton**. A new sensor/task can reuse that structure while defining sensor-appropriate runtime features and event safeguards.

The single-flash model-zoo approach is particularly reusable on microcontrollers: once the variants fit in flash, runtime selection becomes deterministic and independent of storage/network availability.

---

## 4. UX / Developer Experience — 15 pts

### Observable, Not Opaque

The firmware exposes the current routing state in the debugger. For ADS this includes learned scores, requested target, active model, transient step size, adaptive thresholds, and latch state. This is useful when validating whether a model change came from learned evidence or from the sudden-event guard.

### Live Dual-Sensor Dashboard

The ESP32 bridge keeps ADS and ICM state/history separate and presents both streams in the browser. Model colors remain easy to distinguish:

```text
Fast     = green
Balanced = yellow
Accurate = red
```

The RA8D1 sends real model, confidence, sensor value, and DWT-measured latency data over UART; the ESP32 does not make the model-selection decision.

### Reproducibility

The full README contains:

- exact ADS and ICM pin mappings,
- ADS register values,
- ICM register-bank bring-up sequence,
- model artifact layout,
- merged dispatcher rationale,
- meta-controller feature/log format,
- GDB binary-log extraction commands,
- DWT benchmark implementation,
- build/flash procedure,
- bring-up failures and fixes.

---

## Final Result Summary

ARIA's current demonstrated contribution is a **fully local, multi-model runtime inference system on a Cortex-M85**:

```text
2 physical sensor domains
x
3 model tiers each
=
6 resident model graphs in one firmware image
```

Normal ADS compute selection remains learned. The recent transient advancement is deliberately narrow: **only abrupt spikes/dips receive immediate Balanced/Accurate escalation**, and the selected tier is held until a real inference completes before control returns to the learned controller. This provides fast physical-event responsiveness without reverting to the old absolute-voltage model mapping.

The result is a system that demonstrates real on-device model-zoo switching, real measured compute cost, sensor-specific adaptive control, and a reusable architecture that does not depend on the cloud for any inference decision.
