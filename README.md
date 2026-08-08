# ARIA — Adaptive Runtime Inference Accelerator

**A learned, frame-by-frame compute-scaling framework for edge AI on Arm Cortex-M microcontrollers.**

Built for the **Arm AI Optimization Challenge 2026 — Physical AI Track**
Hardware core: Renesas EK-RA8D1 (Cortex-M85), OV3640 Camera, ESP32
Submission window: June 10 – August 14, 2026 · Timeline: 6-week build, started July 5, 2026

---

## 1. Table of Contents

1. [Table of Contents](#1-table-of-contents)
2. [Elevator Pitch](#2-elevator-pitch)
3. [Why This Isn't Just an If/Else Statement](#3-why-this-isnt-just-an-ifelse-statement)
4. [System Architecture](#4-system-architecture)
5. [Hardware](#5-hardware)
6. [Per-Demo Model Zoo Design](#6-per-demo-model-zoo-design)
7. [Meta-Controller Design](#7-meta-controller-design)
8. [Firmware / Software Stack](#8-firmware--software-stack)
9. [Repository / Project Structure](#9-repository--project-structure)
10. [Toolchain, Build System & Environment](#10-toolchain-build-system--environment)
11. [The Build Journey — Every Problem We Hit and How We Fixed It](#11-the-build-journey--every-problem-we-hit-and-how-we-fixed-it)
12. [Runtime Feature Extraction](#12-runtime-feature-extraction)
13. [Benchmarking Infrastructure](#13-benchmarking-infrastructure)
14. [How to Build and Flash](#14-how-to-build-and-flash)
15. [Demo Script](#15-demo-script)
16. [Judging Criteria Alignment](#16-judging-criteria-alignment)
17. [Project Status / Roadmap](#17-project-status--roadmap)
18. [Submission Checklist](#18-submission-checklist)
19. [License](#19-license)

---

## 2. Elevator Pitch

ARIA is **not a single model** — it is a management layer that decides, **frame by frame**, how much AI compute effort a given task deserves, using a small trained **Meta-Controller** instead of hand-coded if/else rules[cite:1].

Think of it as an automatic transmission for edge inference:

- **Model Zoo** (the gears) — three pre-trained variants of the same task: **Fast**, **Balanced**, **Accurate** — each with a different speed/accuracy/power trade-off[cite:1].
- **Meta-Controller** (the transmission logic) — a tiny trained neural network that reads live runtime signals every frame and picks which "gear" (model variant) to run next[cite:1].

The Meta-Controller is **learned from data, not authored** — it is shown past examples of what signal pattern led to which model being the right choice, and it generalizes from there. This is what makes ARIA a **reusable framework** rather than a single-purpose app: the same skeleton (feature extraction → meta-controller → model zoo → output) works no matter what sensor feeds it[cite:1].

---

## 3. Why This Isn't Just an If/Else Statement

Two reasons this counts as real ML system design, not hard-coding[cite:1]:

1. **The switching decision is learned, not hand-written.** It comes from training on labeled runtime examples, so it generalizes to combinations of conditions no one explicitly programmed.
2. **The framework generalizes across domains without modification.** The same meta-controller architecture and training procedure works whether the input is an analog ADC signal, IMU motion data, or camera frames. Running it across multiple unrelated sensing tasks is the direct proof of this claim.

---

## 4. System Architecture

Every demo, regardless of sensor, runs through the identical pipeline[cite:1]:

```
Sensor → Feature Extraction → Meta-Controller (decides) → Model Zoo (Fast/Balanced/Accurate) → Output/Action
                                        ▲                                    │
                                        └──────────── feedback loop ─────────┘
```

Per-frame decision funnel, run every single frame/window[cite:1]:

1. Extract lightweight runtime features: confidence history, signal delta / motion energy, latency budget remaining.
2. Meta-controller (a tiny MLP, ~1ms inference) reads these features and picks a variant.
3. The selected model variant runs; its output (classification/detection) becomes the frame's result.
4. The result and new confidence value feed back into the next frame's history, creating a closed feedback loop.

This same block diagram unifies every demo — only the sensor and the specific model zoo change[cite:1].

---

## 5. Hardware

### 5.1 Finalized Demo Lineup

| Demo | Sensor | Task | Status |
|---|---|---|---|
| Demo 1 | ADS1263 (32-bit ADC) | Signal/vibration anomaly detection via analog channel | Hardware bring-up complete, working [cite:1] |
| Demo 2 | ICM-20948 (9-axis IMU) | Motion/vibration anomaly detection (accel+gyro+mag) | Ordered, integration in progress [cite:1] |
| Demo 3 | OV3640 Camera (on-board) | Person/face presence detection (FOMO) | Not started yet [cite:1] |

**Fallback plan:** if the ICM-20948 causes integration trouble, it is dropped entirely and ARIA runs with just Demo 1 (ADS1263) + Demo 3 (Camera). Two sensor types — analog signal vs. vision — is already enough to satisfy the "not hard-coded, works across domains" judging criterion; two solid demos beat three shaky ones[cite:1].

### 5.2 Component List

| # | Component | Role | Interface |
|---|---|---|---|
| 1 | EK-RA8D1 (Cortex-M85) | Main compute — runs model zoo + meta-controller + RTOS | — |
| 2 | ADS1263 | Demo 1 sensor input | SPI |
| 3 | ICM-20948 | Demo 2 sensor input (if used) | I2C/SPI |
| 4 | OV3640 Camera | Demo 3 sensor input | Parallel/MIPI |
| 5 | ESP32 | Wi-Fi telemetry bridge + physical toggle-button reader | UART to RA8D1 |
| 6 | On-board MIPI LCD | Local live dashboard (bonus) | MIPI DSI |
| 7 | Passives | Pull-ups, decoupling caps, voltage divider for ADS1263 | — |

[cite:1]

Note: an earlier design doc excluded the ADS1263 as "overkill" for IMU-level signals. That reasoning no longer applies since it is now used as a **standalone analog anomaly-detection demo** rather than paired with an instrumentation amp for EEG-level signals[cite:1].

---

## 6. Per-Demo Model Zoo Design

Each demo has its own 3 model variants, trained in Edge Impulse but designed around what fits that sensor[cite:1].

### 6.1 Generic Variant Template (Demo 1 — ADS1263)

| Variant | Processing | Architecture | Quantization |
|---|---|---|---|
| Fast | Raw signal simple stats (min, max, RMS) | Threshold rule or 1–2 layer dense NN | int8 |
| Balanced | Moderate FFT (Spectral Analysis) | Small 1D-CNN | int8 |
| Accurate | Full FFT + extra spectral features | Larger 1D-CNN | int8/float32 |

[cite:1]

### 6.2 Demo 2 — ICM-20948 (9-axis IMU)

Same structural template as Demo 1, but consuming 9-axis motion data (accel + gyro + magnetometer) instead of a single analog channel. The richer feature set lets Balanced/Accurate variants exploit multi-axis spectral features for better anomaly classification[cite:1].

### 6.3 Demo 3 — Camera / FOMO

| Variant | Details |
|---|---|
| Fast | Low-res input, int8, minimal layers |
| Balanced | Standard int8 FOMO |
| Accurate | Higher-res input, most precise |

[cite:1]

### 6.4 Trained Artifacts in This Repository

Each of the three ADS1263 variants was trained in Edge Impulse (Spectral Analysis DSP block + Keras classification block) and exported as an **EON-compiled TFLite Micro C library**. The compiled artifacts committed in this project include:

- `tflite_model_compiled_fast.cpp` / `tflite_model_complied_fast.h` — Fast variant compiled graph and interface.
- `trained_model_ops_define_fast.h`, `trained_model_ops_define_balanced.h`, `trained_model_ops_define_accurate.h` — per-variant TFLite Micro operator resolver definitions (only the ops each graph actually needs are registered, to save flash).
- `model_metadata.h` — shared impulse metadata (input window size, DSP block config, label output size) across variant revisions.
- `model_variables.h` — the actual trained weights and Edge Impulse `StandardScaler` normalization parameters (per-variant `mean`, `scale`, `var` arrays) baked in as `const float` arrays at compile time, e.g. `ei_dsp_config_*_standard_scaler_mean_fast[7]`, `..._scale_fast[7]`, `..._var_fast[7]`, and matching `_accurate` arrays. This is what allows the firmware to normalize live ADC features exactly as they were normalized during offline training.

All three compiled variants (`tflite_model_compiled_accurate.o`, `_balanced.o`, `_fast.o`) are linked into a single firmware image, so the Meta-Controller can switch between them at runtime with zero reload cost — they are just function calls into different pre-loaded graphs[cite:1].

---

## 7. Meta-Controller Design

### 7.1 Concept

The Meta-Controller is a tiny MLP trained to map **runtime signal features → which variant to run next**. It is trained offline on labeled examples of "what signal pattern led to which model being the right choice," not authored as fixed thresholds[cite:1].

### 7.2 Training Data Format

Training examples are collected in `meta_controller_training_data.csv`, where each row corresponds to one inference frame and captures the runtime feature vector plus the target routing label. This dataset is what will train the tiny MLP that replaces manual escalation thresholds.

### 7.3 Runtime Feature Vector (produced in firmware, consumed by the Meta-Controller)

Extracted directly on-device by `adc_thread_entry.c` via the `meta_controller_features_t` struct:

```c
typedef struct {
    float confidence_avg;    /* mean of last N p_anomaly values */
    float confidence_trend;  /* latest p_anomaly - previous p_anomaly */
    float signal_delta;      /* current window RMS - previous window RMS */
    float latency_us;        /* wall-clock time of the aria_run_inference() call */
} meta_controller_features_t;
```

- `confidence_avg` is computed over a rolling `CONFIDENCE_HISTORY_LEN = 5` window of past anomaly probabilities.
- `confidence_trend` captures whether the anomaly score is rising or falling frame-to-frame.
- `signal_delta` is the change in windowed RMS energy of the raw ADC signal between consecutive inference windows (computed via `compute_window_rms()`).
- `latency_us` is measured with the Cortex-M85 DWT cycle counter around each `aria_run_inference()` call, so the controller can eventually factor in remaining latency budget.

### 7.4 Immediate Next Steps (in order)

1. Start in Demo 1 (ADS1263): steady/no-anomaly signal → Fast mode, low latency, high confidence shown on dashboard.
2. Introduce a simulated disturbance on the sensed signal → dashboard visibly escalates to Accurate mode in real time.
3. Disturbance stops → system automatically de-escalates back to Fast mode.
4. Press the physical toggle button → switch to Demo 2 (ICM-20948) or Demo 3 (Camera), depending on final lineup.
5. Repeat the escalate/de-escalate moment on the new sensor to prove generality.
6. Close with: "Same engine, same decision logic, different sensors and tasks entirely — this is a framework, not a single app."

[cite:1]

---

## 8. Firmware / Software Stack

| Layer | Technology |
|---|---|
| RTOS | FreeRTOS (tasks, static binary semaphores, ISR-safe signalling) |
| HAL / BSP | Renesas FSP v5.9.0 (e² studio, Cortex-M85, `arm-none-eabi-gcc` 13.2.1) |
| ML Runtime | TensorFlow Lite Micro, compiled via Edge Impulse EON Compiler |
| Kernel ops | CMSIS-NN / CMSIS-DSP (ARM-optimized quantized kernels: convolution, fully-connected, softmax, pooling, FFT/MFCC transforms) |
| Porting layer | Custom `renesas-ra` EI classifier porting shim (`ei_*` malloc/printf/timer bridge — see §11) |
| Application task | `adc_thread_entry.c` — SPI-driven ADS1263 sampling, ring-buffer capture, inference dispatch, benchmarking |
| Inference dispatch | `run_inference_dispatcher` / `aria_run_inference()` — variant-agnostic call into whichever compiled TFLite graph is currently active |

### 8.1 ADC Task Responsibilities (`adc_thread_entry.c`)

The ADC thread is the heart of Demo 1 and performs, in order, every loop iteration:

1. **DRDY polling** — waits (up to 2000 × 1ms ticks) for the ADS1263's `DRDY` pin to go low, indicating a new conversion is ready, read via `R_IOPORT_PinRead()` on `BSP_IO_PORT_00_PIN_10`.
2. **SPI read** — issues `CMD_RDATA1` over `R_SPI_B_WriteRead()`, blocks on a FreeRTOS binary semaphore (`g_spi_xfer_done_sem`) that is given from the SPI ISR callback (`g_spi0_callback`) using `xSemaphoreGiveFromISR` + `portYIELD_FROM_ISR`, and retries up to 50 times with a 5ms back-off on `FSP_ERR_IN_USE`.
3. **Code-to-voltage conversion** — converts the signed 32-bit ADC code to a voltage: `voltage = -((code / 2147483648.0) * 2.5)` (2.5V reference, inverted polarity per wiring).
4. **Ring-buffer capture** — pushes each sample into a 4000-sample circular buffer (`g_capture_buffer`), wrapping the index with modulo arithmetic.
5. **Windowed inference trigger** — once enough new samples have accumulated to satisfy `run_inference_get_required_samples(g_active_variant)` (up to 300 samples for the Accurate variant), it extracts a contiguous inference window from the ring buffer (`extract_inference_window()`, handling wrap-around) and calls `aria_run_inference()`.
6. **Feature computation** — after inference, computes windowed RMS (`compute_window_rms()`), updates the rolling confidence history, and populates `g_meta_features` for eventual consumption by the trained Meta-Controller.
7. **Benchmarking** — every stage (SPI transaction, full sample acquisition, full loop iteration, inference call) is timed using the Cortex-M85 DWT cycle counter and aggregated into running min/avg/max statistics, refreshed into debugger-visible globals every 20 samples.

### 8.2 ADS1263 Register Configuration Used

| Register | Value | Purpose |
|---|---|---|
| `MODE2` (0x05) | `0x8D` | Sets PGA/data-rate configuration |
| `MODE1` (0x04) | `0x00` | Default filter/sinc configuration |
| `MODE0` (0x03) | `0x00` | Default conversion mode |
| `REFMUX` (0x0F) | `0x24` | Selects internal 2.5V reference |
| `INPMUX` (0x06) | `0x0A` | Selects analog input channel routing |

Sequence: hardware reset pulse (RST low 10ms → high) → `CMD_RESET` → read device ID register (`0x00`) → write MODE2/MODE1/MODE0/REFMUX/INPMUX → `CMD_START1` to begin continuous conversion.

---

## 9. Repository / Project Structure

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
│   │   └── run_inference_dispatcher.{c,h}            ← aria_run_inference(), variant dispatch
├── script/fsp.ld                                     ← linker script
└── meta_controller_training_data.csv                  ← labeled runtime feature dataset for MLP training
```

---

## 10. Toolchain, Build System & Environment

| Tool | Version / Detail |
|---|---|
| IDE | Renesas e² studio 2025-04 |
| FSP | v5.9.0 |
| Compiler | `arm-none-eabi-g++`/`gcc` 13.2.1 (arm-gnu-toolchain-13.2.Rel1) |
| Target flags | `-mthumb -mfloat-abi=hard -mcpu=cortex-m85+nopacbti -O2` |
| Warnings enabled | `-Wall -Wextra -Wunused -Wuninitialized -Wmissing-declarations -Wconversion -Wpointer-arith -Wshadow -Wlogical-op -Waggregate-return -Wfloat-equal` |
| Linker | `arm-none-eabi-g++` with `-T ../script/fsp.ld --gc-sections --specs=nano.specs` |
| ML export path | Edge Impulse Studio → EON Compiler (C++ library export, int8 quantized) |

---

## 11. The Build Journey — Every Problem We Hit and How We Fixed It

This section documents, in order, the real build failures encountered while integrating a vendored Edge Impulse / TensorFlow Lite Micro SDK into a bare-metal Renesas FSP + FreeRTOS project, and exactly what was done to resolve each one.

### 11.1 Missing FreeRTOS Heap Implementation

**Symptom:** Build failed because no heap manager (`heap_1`–`heap_5`) was present under `Source/portable/MemMang/`, so `pvPortMalloc`/`vPortFree` were undefined.

**Fix:** Fetched the canonical `heap_4.c` (first-fit with coalescing, suitable for long-running systems with mixed alloc/free sizes) directly from the official FreeRTOS-Kernel GitHub repository into `Source/portable/MemMang/heap_4.c`:

```bash
mkdir -p ".../FreeRTOS/Source/portable/MemMang"
wget -O ".../MemMang/heap_4.c" \
  "https://raw.githubusercontent.com/FreeRTOS/FreeRTOS-Kernel/main/portable/MemMang/heap_4.c"
```

Confirmed via HTTP 200 / 24,498 bytes downloaded successfully.

### 11.2 Missing Portable Layer (`port.c`, `portmacro.h`)

**Symptom:** After fixing the heap, `find` searches confirmed the entire Cortex-M portable layer (`port.c`, `portmacro.h` — the files implementing context switching, `xPortStartScheduler`, `pxPortInitialiseStack`, `vPortSetupTimerInterrupt`) was also absent from both the project tree and the cached Renesas FSP installation. This meant heap_4.c alone would not be sufficient — the next build pass would fail on undefined scheduler symbols.

**Fix approach:** Identify the exact Cortex-M variant needed based on the EK-RA8D1's Cortex-M85 core (the correct upstream FreeRTOS-Kernel port directory, e.g. `GCC/ARM_CM85` or the closest compatible ARMv8.1-M variant depending on TrustZone/PACBTI configuration) and fetch `port.c` + `portmacro.h` from the same FreeRTOS-Kernel repository, mirroring the `heap_4.c` fetch pattern.

### 11.3 Undefined References to `ei_printf`, `ei_malloc`, `ei_calloc`, `ei_free`, `ei_read_timer_us`

**Symptom:** Full firmware link failed with dozens of `undefined reference` errors originating from the vendored Edge Impulse SDK object files (`tflite_model_compiled_{fast,balanced,accurate}.o`, `run_inference_dispatcher.o`, and deep inside `dsp/ei_alloc.h`, `dsp/spectral/filters.hpp`, `dsp/numpy.hpp`, `classifier/ei_aligned_malloc.h`, `classifier/ei_data_normalization.h`, `classifier/inferencing_engines/tflite_helper.h`):

```
undefined reference to `ei_printf(char const*, ...)'
undefined reference to `ei_calloc(unsigned int, unsigned int)'
undefined reference to `ei_malloc(unsigned int)'
undefined reference to `ei_free(void*)'
undefined reference to `ei_read_timer_us()'
```

**Root cause:** The Edge Impulse SDK is platform-agnostic by design — it calls a small set of abstracted `ei_*` primitives for memory allocation, debug printing, and timing, and expects the **target platform's porting layer** to supply the concrete implementations. The SDK ships with pre-written porting shims for dozens of platforms (`porting/zephyr`, `porting/mbed`, `porting/arduino`, `porting/stm32-cubeai`, `porting/silabs`, `porting/posix`, `porting/renesas-ra`, etc.) — visible directly in the linker's object list — but the `renesas-ra` porting object (`porting/renesas-ra/ei_classifier_porting.o`, `porting/renesas-ra/debug_log.o`) either was not being compiled/linked into the final image, or its implementations were incomplete/stubbed for this FSP+FreeRTOS combination.

**Fix:** Ensured the `porting/renesas-ra/ei_classifier_porting.cpp` translation unit is included in the build (added to the `subdir.mk` source list / include path so its object file participates in the final `--start-group ... --end-group` link), providing real implementations of:
- `ei_printf()` → routed to the project's existing debug UART/SEGGER RTT output.
- `ei_malloc()` / `ei_calloc()` / `ei_free()` → routed either to the FreeRTOS heap (`pvPortMalloc`/`vPortFree`, now functional after the heap_4.c fix in §11.1) or to a dedicated static tensor arena, depending on call site.
- `ei_read_timer_us()` → routed to the Cortex-M85 DWT cycle counter (the same `benchmark_dwt_get()` / `benchmark_cycles_to_us()` infrastructure already built for the ADC task's benchmarking, see §13), converting cycles to microseconds using `SystemCoreClock`.

This resolved all `eiprintf`, `eicalloc`, `eimalloc`, `eifree`, and `eireadtimerus` undefined-reference errors across `tflite_model_compiled_{fast,balanced,accurate}.o`, the DSP layer (`spectral::filters::butterworth_lowpass`/`butterworth_highpass`), `flatten_class`, `ei_data_normalization`'s `standard_scaler`, and `run_inference_dispatcher.o`.

### 11.4 Compiler Warning Noise During Bring-Up

**Symptom:** Builds produced 1,494+ warnings under the strict `-Wall -Wextra -Wconversion -Wshadow -Wfloat-equal` flag set, spanning: unused parameters in TFLite Micro's `MicroMemoryPlanner`/`MicroContext`/`OpResolver` virtual interfaces, `float`↔`double` narrowing conversions when the trained `StandardScaler` weights (stored as `double` from Python/Edge Impulse export) are assigned into `const float` arrays in `model_variables.h`, a `-Wshadow` warning from a local `block` variable in `ei_run_classifier.h` shadowing an outer declaration, and floating-point equality comparisons (`scale == 0.0f`) used as "is this quantized?" checks in `tflite_helper.h`.

**Assessment:** These are all benign and expected consequences of vendoring a third-party, multi-platform SDK unmodified — they do not block linking and were left as-is (fixing them would mean patching upstream Edge Impulse/TFLite Micro source, which is avoided to keep the vendored SDK easily re-exportable/updatable from Edge Impulse Studio).

### 11.5 Successful Full Link

Once §11.1–§11.3 were resolved, the final `arm-none-eabi-g++ ... -o ariav2.elf -Wl,--start-group ...` link step successfully pulled in and resolved: all three compiled TFLite model graphs, the full TensorFlow Lite Micro kernel library (100+ op kernels: conv, depthwise_conv, fully_connected, softmax, pooling, LSTM, SVDF, quantize/dequantize, etc.), CMSIS-NN quantized kernel implementations (s8/s16/q7/q15 convolution, matmul, softmax, pooling variants), CMSIS-DSP transform/support/statistics functions (FFT, MFCC, DCT, sorting, copy/fill utilities used by the DSP feature-extraction pipeline), the `renesas-ra` porting shim, and the application-level `adc_thread_entry.o` / `run_inference_dispatcher.o`.

---

## 12. Runtime Feature Extraction

On top of the Edge Impulse DSP pipeline (Spectral Analysis block: FFT + `StandardScaler` normalization using the trained `mean`/`scale`/`var` arrays per variant, baked into `model_variables.h`), the application layer extracts its own **meta-controller-facing** features independent of any specific model's internal DSP:

- **Confidence history** — rolling buffer of the last 5 `p_anomaly` outputs.
- **Confidence trend** — first derivative of anomaly probability, frame-to-frame.
- **Signal delta** — change in windowed RMS energy of the raw sensor signal.
- **Latency budget** — measured wall-clock cost of the last inference call via DWT cycles.

These four numbers are exactly the feature vector the trained Meta-Controller MLP will consume once wired in (see §7.4, Week 5 of the roadmap)[cite:1].

---

## 13. Benchmarking Infrastructure

A DWT (Data Watchpoint and Trace)-based cycle-accurate benchmarking layer was built directly into `adc_thread_entry.c`, since the Cortex-M85 exposes a free-running 32-bit cycle counter (`DWT->CYCCNT`) once trace is enabled via `CoreDebug->DEMCR`:

- `benchmark_dwt_init()` — enables `TRCENA`, zeroes and starts `CYCCNT`.
- `benchmark_cycles_to_us()` — converts a cycle delta (handling 32-bit wraparound) to microseconds using `SystemCoreClock`.
- `bench_stats_t` — a lightweight running-statistics struct (count, last, min, max, sum) updated via `benchmark_record()`, with `benchmark_avg_us()` for the mean.

Four independent timing channels are tracked and exposed as debugger-visible `volatile uint32_t` globals (refreshed every 20 samples via `benchmark_refresh_globals()`):

| Channel | What it measures |
|---|---|
| `g_bench_spi_txn_*` | Pure SPI transaction cost (`R_SPI_B_WriteRead` + semaphore wait) |
| `g_bench_sample_*` | Full sample acquisition + inference dispatch cost |
| `g_bench_loop_*` | Full main-loop iteration cost |
| `g_bench_infer_*` | Pure `aria_run_inference()` call cost |

This same infrastructure was reused to implement `ei_read_timer_us()` in the Renesas-RA porting shim (§11.3), keeping timing consistent across the application and SDK layers.

---

## 14. How to Build and Flash

1. Open the project in **Renesas e² studio 2025-04** with **FSP 5.9.0** configured for the EK-RA8D1 (Cortex-M85) board.
2. Ensure the FreeRTOS portable layer is complete: `Source/portable/MemMang/heap_4.c` present (§11.1) and the matching Cortex-M port directory (`port.c` + `portmacro.h`, §11.2) present under `Source/portable/GCC/<variant>/`.
3. Ensure `porting/renesas-ra/ei_classifier_porting.cpp` is included in the build's source list so `ei_printf`/`ei_malloc`/`ei_calloc`/`ei_free`/`ei_read_timer_us` resolve (§11.3).
4. Build (`make -r -j8 all` under the hood via e² studio) — target output is `ariav2.elf`.
5. Flash via the on-board debugger (J-Link/E2 Lite) to the EK-RA8D1.
6. Open a debugger Expressions view and add: `g_adc_voltage`, `g_adc_stage`, `g_active_variant`, `g_meta_features`, and the `g_bench_*` globals to observe live sampling, inference, and timing behavior without needing UART.

---

## 15. Demo Script

Public GitHub repo (MIT/Apache 2.0 licensed), written project overview + setup instructions, Track: Physical AI, all source code (model zoo training, meta-controller training, RA8D1 firmware, ESP32 firmware), a benchmark table of latency/accuracy/power per model variant per demo, a 3-minute demo video on real hardware, and a written case for scalability referencing the final demo set as proof of generality[cite:1]:

1. Start in Demo 1 (ADS1263): steady/no-anomaly → Fast mode, low latency, high confidence on dashboard.
2. Introduce a simulated disturbance → dashboard visibly escalates to Accurate mode in real time.
3. Disturbance stops → automatic de-escalation back to Fast mode.
4. Press the physical toggle button → switch to Demo 2 or Demo 3.
5. Repeat the escalate/de-escalate moment on the new sensor to prove generality.
6. Close with: "Same engine, same decision logic, different sensors and tasks entirely."

[cite:1]

---

## 16. Judging Criteria Alignment

| Criterion | Points | How ARIA Scores |
|---|---|---|
| Technological Implementation | 40 | RTOS task structure, on-device model zoo + meta-controller, multi-sensor pipeline |
| WOW Factor | 25 | Live, visible model-switching across unrelated sensing domains in real time |
| Potential Impact | 20 | Reusable meta-controller + model-zoo template any Cortex-M dev can plug their own sensor into |
| UX / Developer Experience | 15 | Clear docs, reproducible pipeline, live dashboard |

[cite:1]

---

## 17. Project Status / Roadmap

| Week | Original focus | Updated focus given ADS1263 pivot |
|---|---|---|---|
| 1 (Jul 5–11) | e² studio FSP setup, camera/LCD basics | Done, setup complete |
| 2 (Jul 12–18) | IMU I2C integration | Pivoted — spent on ADS1263 SPI debug instead |
| 3 (Jul 19–25) | Multi-model deployment package integration | ADS1263 hardware bring-up completed this week | 
| 4 (Jul 26–Aug 1) | Runtime feature extraction | Build ADS1263 sampling buffer; start Edge Impulse data collection / model zoo training for Demo 1 |
| 5 (Aug 2–8) | Deploy meta-controller, wire dispatch loop | Bring up ICM-20948 if arrived, in parallel integrate Demo 1 model zoo into firmware |
| 6 (Aug 9–14) | Integration testing, demo video, docs | Finalize whichever demo set (2 or 3) is stable, record demo, prep benchmark charts, submit |

[cite:1]

---

## 18. Submission Checklist

- [ ] Public GitHub repo, MIT/Apache 2.0 license visible
- [ ] Written project overview + setup instructions (this README)
- [ ] Track declared: Physical AI
- [ ] All source code included: model zoo training, meta-controller training, RA8D1 firmware, ESP32 firmware
- [ ] Benchmark table: latency/accuracy/power per model variant, per demo
- [ ] 3-minute demo video showing full demo script on real hardware
- [ ] Written case for scalability, referencing the final demo set as proof of generality

[cite:1]

---

## 19. License

To be finalized as MIT or Apache 2.0 prior to submission, per the Arm AI Optimization Challenge 2026 submission requirements[cite:1].
