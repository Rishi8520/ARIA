#include <adc_thread.h>
#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "run_inference_dispatcher.h"
#include "meta_controller_weights.h"
#include "telemetry.h"
#include <math.h>
#include <stdlib.h>

#define CMD_RESET     (0x06U)
#define CMD_START1    (0x08U)
#define CMD_STOP1     (0x0AU)
#define CMD_RDATA1    (0x12U)
#define CMD_RREG      (0x20U)
#define CMD_WREG      (0x40U)

#define REG_MODE0     (0x03U)
#define REG_MODE1     (0x04U)
#define REG_MODE2     (0x05U)
#define REG_INPMUX    (0x06U)
#define REG_REFMUX    (0x0FU)

#define CAPTURE_BUFFER_SIZE 4000   /* adjust based on your sample rate and desired duration */

static volatile double g_capture_buffer[CAPTURE_BUFFER_SIZE];
static volatile uint32_t g_capture_index = 0;
static volatile bool g_capture_active = true;   /* auto-start, no debugger interaction needed */
static volatile bool g_capture_done = false;
static volatile uint32_t g_capture_total_count = 0;
static float g_inference_window[300];  /* max across all variants (accurate = 300) */
static volatile model_variant_t g_active_variant = VARIANT_FAST;
static uint32_t g_last_inference_capture_count = 0;
#define CONFIDENCE_HISTORY_LEN 3

static float g_confidence_history[CONFIDENCE_HISTORY_LEN] = { 0 };
static uint32_t g_confidence_history_idx = 0;
static float g_prev_window_rms = 0.0f;

volatile uint32_t g_debug_needed_samples = 0;
volatile uint32_t g_debug_active_variant = 0;

typedef struct {
    float confidence_avg;   /* mean of last N p_anomaly values */
    float confidence_trend; /* latest p_anomaly - previous p_anomaly */
    float signal_delta;     /* current window RMS - previous window RMS */
    float latency_us;       /* wall-clock time of the aria_run_inference() call */
    float signal_rms;       /* absolute current window RMS */
} meta_controller_features_t;

static volatile meta_controller_features_t g_meta_features = { 0 };

/* ---- Meta-controller training data buffer ---- */
#define META_LOG_SIZE 500
static volatile float g_meta_log[META_LOG_SIZE][5]; /* conf_avg, conf_trend, signal_delta, latency_us, signal_rms */
static volatile uint32_t g_meta_log_index = 0;

//#define ADC_CS_PORT   (BSP_IO_PORT_04_PIN_13)
#define ADC_RST_PORT  (BSP_IO_PORT_05_PIN_07)
#define ADC_DRDY_PORT (BSP_IO_PORT_00_PIN_10)

static SemaphoreHandle_t g_spi_xfer_done_sem;
static StaticSemaphore_t g_spi_sem_buffer;

volatile fsp_err_t g_adc_last_err = FSP_SUCCESS;
volatile uint32_t g_adc_stage = 0;
volatile int32_t g_adc_code = 0;
volatile double g_adc_voltage = 0.0;
volatile uint32_t g_spi_cb_fire_count = 0;
volatile uint8_t g_adc_id = 0;
volatile uint8_t g_drdy_debug_level = 0xFF;
volatile uint32_t g_bench_infer_avg_us = 0;
volatile uint32_t g_bench_infer_min_us = 0;
volatile uint32_t g_bench_infer_max_us = 0;

volatile bool g_adc_spi_ok = true;
volatile uint32_t g_adc_fail_point = 0;
volatile fsp_err_t g_adc_fail_err = FSP_SUCCESS;
volatile uint8_t g_last_rx[5] = { 0 };

#define ESCALATE_CONFIRM_WINDOWS_PER_LEVEL   (1U)
#define DEESCALATE_CONFIRM_WINDOWS_PER_LEVEL (3U)
#define MIN_VARIANT_HOLD_WINDOWS             (2U)

/* 1 = pin to Fast + log features; 0 = normal closed-loop control */
#define ARIA_META_TRAINING_MODE (0)

#define GATE_SIGNAL_RMS_FOR_BALANCED   (0.490f)
#define GATE_SIGNAL_RMS_FOR_ACCURATE   (0.954f)

/* Within the intermediate signal band, the trained meta-controller may
 * promote BALANCED -> ACCURATE only after the physical signal has already
 * reached this level. */
#define GATE_SIGNAL_RMS_FOR_MID_ACCURATE (0.750f)

static model_variant_t g_controller_candidate = VARIANT_FAST;
static uint32_t g_candidate_streak = 0U;
static uint32_t g_variant_hold_windows = 0U;

/* Useful to expose on dashboard/debugger. */
volatile uint32_t g_debug_controller_candidate = 0U;
volatile uint32_t g_debug_candidate_streak = 0U;
volatile uint32_t g_debug_variant_hold_windows = 0U;
volatile int32_t  g_debug_inference_err = -999;
volatile uint32_t g_debug_inference_count = 0U;
volatile uint32_t g_debug_real_telemetry_count = 0U;

/* ============================================================
   DWT-Based Benchmarking (Cortex-M85 cycle counter)
   ============================================================ */

static inline void benchmark_dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t benchmark_dwt_get(void)
{
    return DWT->CYCCNT;
}

static inline uint32_t benchmark_cycles_to_us(uint32_t start, uint32_t end)
{
    uint32_t delta = (end >= start) ? (end - start) : (0xFFFFFFFFu - start + end);
    return (uint32_t)((uint64_t)delta * 1000000ULL / SystemCoreClock);
}

typedef struct
{
    uint32_t count;
    uint32_t last_us;
    uint32_t min_us;
    uint32_t max_us;
    uint64_t sum_us;
} bench_stats_t;

static bench_stats_t g_spi_txn_stats  = { 0, 0, 0xFFFFFFFF, 0, 0 };
static bench_stats_t g_sample_stats   = { 0, 0, 0xFFFFFFFF, 0, 0 };
static bench_stats_t g_loop_stats     = { 0, 0, 0xFFFFFFFF, 0, 0 };
static bench_stats_t g_infer_stats = { 0, 0, 0xFFFFFFFF, 0, 0 };

static inline void benchmark_record(bench_stats_t *s, uint32_t elapsed_us)
{
    s->last_us = elapsed_us;
    if (elapsed_us < s->min_us) s->min_us = elapsed_us;
    if (elapsed_us > s->max_us) s->max_us = elapsed_us;
    s->sum_us += elapsed_us;
    s->count++;
}

static inline uint32_t benchmark_avg_us(const bench_stats_t *s)
{
    return (s->count == 0) ? 0 : (uint32_t)(s->sum_us / s->count);
}

/* Exposed globals -- add these as Expressions in the e2 studio debugger */
volatile uint32_t g_bench_spi_txn_avg_us   = 0;
volatile uint32_t g_bench_spi_txn_min_us   = 0;
volatile uint32_t g_bench_spi_txn_max_us   = 0;

volatile uint32_t g_bench_sample_avg_us    = 0;
volatile uint32_t g_bench_sample_min_us    = 0;
volatile uint32_t g_bench_sample_max_us    = 0;

volatile uint32_t g_bench_loop_avg_us      = 0;
volatile uint32_t g_bench_loop_min_us      = 0;
volatile uint32_t g_bench_loop_max_us      = 0;

volatile uint32_t g_bench_sample_count     = 0;

static void benchmark_refresh_globals(void)
{
    g_bench_spi_txn_avg_us = benchmark_avg_us(&g_spi_txn_stats);
    g_bench_spi_txn_min_us = g_spi_txn_stats.min_us;
    g_bench_spi_txn_max_us = g_spi_txn_stats.max_us;

    g_bench_sample_avg_us  = benchmark_avg_us(&g_sample_stats);
    g_bench_sample_min_us  = g_sample_stats.min_us;
    g_bench_sample_max_us  = g_sample_stats.max_us;

    g_bench_loop_avg_us    = benchmark_avg_us(&g_loop_stats);
    g_bench_loop_min_us    = g_loop_stats.min_us;
    g_bench_loop_max_us    = g_loop_stats.max_us;

    g_bench_sample_count   = g_sample_stats.count;

    g_bench_infer_avg_us = benchmark_avg_us(&g_infer_stats);
    g_bench_infer_min_us = g_infer_stats.min_us;
    g_bench_infer_max_us = g_infer_stats.max_us;
}

#define CHECK_SPI(step) \
    do { if (!g_adc_spi_ok) { g_adc_fail_point = (step); g_adc_fail_err = g_adc_last_err; } } while (0)

void g_spi0_callback(spi_callback_args_t *p_args)
{
    BaseType_t higher_priority_woken = pdFALSE;

    g_spi_cb_fire_count++;

    if (SPI_EVENT_TRANSFER_COMPLETE == p_args->event)
    {
        xSemaphoreGiveFromISR(g_spi_xfer_done_sem, &higher_priority_woken);
    }

    portYIELD_FROM_ISR(higher_priority_woken);
}

//static void cs_low(void)  { R_IOPORT_PinWrite(&g_ioport_ctrl, ADC_CS_PORT, BSP_IO_LEVEL_LOW); }
//static void cs_high(void) { R_IOPORT_PinWrite(&g_ioport_ctrl, ADC_CS_PORT, BSP_IO_LEVEL_HIGH); }

static bool spi_transfer(uint8_t *p_tx, uint8_t *p_rx, uint32_t length)
{
    uint32_t retries = 50;

    /* ---- Benchmark: pure SPI transaction cost ---- */
    uint32_t t0 = benchmark_dwt_get();

    while (retries > 0)
    {
        g_adc_last_err = R_SPI_B_WriteRead(&g_spi0_ctrl, p_tx, p_rx, length, SPI_BIT_WIDTH_8_BITS);

        if (FSP_ERR_IN_USE == g_adc_last_err)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
            retries--;
            continue;
        }

        break;
    }

    if (FSP_SUCCESS != g_adc_last_err)
    {
        return false;
    }

    bool ok = (pdTRUE == xSemaphoreTake(g_spi_xfer_done_sem, pdMS_TO_TICKS(200)));

    uint32_t t1 = benchmark_dwt_get();
    benchmark_record(&g_spi_txn_stats, benchmark_cycles_to_us(t0, t1));

    return ok;
}

static void adc_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[3] = { (uint8_t)(CMD_WREG | reg), 0x00, value };
    uint8_t rx[3] = { 0 };

    //cs_low();
    g_adc_spi_ok = spi_transfer(tx, rx, sizeof(tx));
    //cs_high();
    vTaskDelay(pdMS_TO_TICKS(10));
}
volatile uint8_t g_adc_id_cmd_rx = 0;
volatile uint8_t g_adc_id_data_rx = 0;

static uint8_t adc_read_reg(uint8_t reg)
{
    uint8_t tx[3] = { (uint8_t)(CMD_RREG | reg), 0x00U, 0x00U };
    uint8_t rx[3] = { 0U, 0U, 0U };

    g_adc_spi_ok = spi_transfer(tx, rx, sizeof(tx));

    g_adc_id_cmd_rx = rx[1];
    g_adc_id_data_rx = rx[2];
    g_last_rx[0] = tx[0];
    g_last_rx[1] = rx[1];
    g_last_rx[2] = rx[2];
    g_last_rx[3] = 0;
    g_last_rx[4] = 0;

    return rx[2];
}

static void adc_send_cmd(uint8_t cmd)
{
    uint8_t tx[1] = { cmd };
    uint8_t rx[1] = { 0 };

    //cs_low();
    g_adc_spi_ok = spi_transfer(tx, rx, sizeof(tx));
    //cs_high();
    vTaskDelay(pdMS_TO_TICKS(2));
}

static bool drdy_is_low(void)
{
    bsp_io_level_t level = BSP_IO_LEVEL_HIGH;
    R_IOPORT_PinRead(&g_ioport_ctrl, ADC_DRDY_PORT, &level);
    return (BSP_IO_LEVEL_LOW == level);
}

static int32_t adc_read_data(void)
{
    uint8_t tx[5] = { CMD_RDATA1, 0x00, 0x00, 0x00, 0x00 };
    uint8_t rx[5] = { 0 };

    //cs_low();
    g_adc_spi_ok = spi_transfer(tx, rx, sizeof(tx));
    //cs_high();
    vTaskDelay(pdMS_TO_TICKS(2));

    g_last_rx[0] = rx[0];
    g_last_rx[1] = rx[1];
    g_last_rx[2] = rx[2];
    g_last_rx[3] = rx[3];
    g_last_rx[4] = rx[4];

    int32_t result = ((int32_t)rx[1] << 24) |
                      ((int32_t)rx[2] << 16) |
                      ((int32_t)rx[3] << 8)  |
                      ((int32_t)rx[4]);

    return result;
}

static void extract_inference_window(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        int32_t idx = (int32_t)g_capture_index - (int32_t)n + (int32_t)i;
        while (idx < 0) {
            idx += CAPTURE_BUFFER_SIZE;
        }
        idx %= CAPTURE_BUFFER_SIZE;
        g_inference_window[i] = (float)g_capture_buffer[idx];
    }
}

static float compute_window_rms(const float *window, uint32_t n)
{
    double sum_sq = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        sum_sq += (double)window[i] * (double)window[i];
    }
    return (float)sqrt(sum_sq / (double)n);
}

/*
 * Signal-first ADS meta-controller.
 *
 * Hard physical guarantees:
 *   low RMS    -> FAST
 *   high RMS   -> ACCURATE
 *
 * In the middle band, BALANCED is the default. The trained meta-controller is
 * retained as a secondary refinement and may promote to ACCURATE only when the
 * physical RMS itself is already substantial.
 */
static model_variant_t aria_signal_meta_candidate(
    float confidence_avg,
    float confidence_trend,
    float signal_delta,
    float signal_rms)
{
    if (signal_rms < GATE_SIGNAL_RMS_FOR_BALANCED)
    {
        return VARIANT_FAST;
    }

    if (signal_rms >= GATE_SIGNAL_RMS_FOR_ACCURATE)
    {
        return VARIANT_ACCURATE;
    }

    int learned_class = meta_controller_predict(
        confidence_avg,
        confidence_trend,
        signal_delta,
        signal_rms);

    if ((learned_class == (int)VARIANT_ACCURATE) &&
        (signal_rms >= GATE_SIGNAL_RMS_FOR_MID_ACCURATE))
    {
        return VARIANT_ACCURATE;
    }

    return VARIANT_BALANCED;
}

static model_variant_t aria_apply_transition_policy(
    model_variant_t active,
    model_variant_t candidate,
    float current_signal_rms)
{
    /* ---- Physical plausibility gate (runs before any hysteresis logic) ----
     * Clamp an implausible candidate down to the highest variant the raw
     * signal actually supports, regardless of what the network predicted.
     * This never blocks a *correct* escalation -- it only vetoes cases where
     * physical signal_rms is far below what that variant's training data
     * ever showed. */
    if (candidate == VARIANT_ACCURATE && current_signal_rms < GATE_SIGNAL_RMS_FOR_ACCURATE) {
        candidate = (current_signal_rms < GATE_SIGNAL_RMS_FOR_BALANCED) ? VARIANT_FAST : VARIANT_BALANCED;
    } else if (candidate == VARIANT_BALANCED && current_signal_rms < GATE_SIGNAL_RMS_FOR_BALANCED) {
        candidate = VARIANT_FAST;
    }

    /* Always advance hold-time in the current active state. */
    if (g_variant_hold_windows < 0xFFFFFFFFU) {
        g_variant_hold_windows++;
    }

    if (candidate == active) {
        g_candidate_streak = 0U;
        return active;
    }

    if (candidate != g_controller_candidate) {
        g_controller_candidate = candidate;
        g_candidate_streak = 1U;
    } else {
        g_candidate_streak++;
    }

    /* ---- Magnitude-scaled confirmation ----
     * A 2-level jump (e.g. Fast -> Accurate) now requires proportionally
     * more consecutive confirming windows than a 1-level jump, instead of
     * being treated identically to a small step. This directly targets the
     * "one noisy window jumps straight to Accurate" failure mode. */
    uint32_t jump_size = (uint32_t) abs((int)candidate - (int)active);
    bool is_escalation = (candidate > active);

    uint32_t required_windows = jump_size *
        (is_escalation ? ESCALATE_CONFIRM_WINDOWS_PER_LEVEL : DEESCALATE_CONFIRM_WINDOWS_PER_LEVEL);

    if ((g_variant_hold_windows >= MIN_VARIANT_HOLD_WINDOWS) &&
        (g_candidate_streak >= required_windows)) {
        g_variant_hold_windows = 0U;
        g_candidate_streak = 0U;
        return candidate;
    }

    return active;
}

void adc_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    g_adc_stage = 1;

    g_spi_xfer_done_sem = xSemaphoreCreateBinaryStatic(&g_spi_sem_buffer);

    if (NULL == g_spi_xfer_done_sem)
    {
        g_adc_stage = 0xE001;
        vTaskSuspend(NULL);
    }
    telemetry_init();
    //R_IOPORT_PinCfg(&g_ioport_ctrl, ADC_CS_PORT,
    //                 IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_HIGH);

    R_IOPORT_PinCfg(&g_ioport_ctrl, ADC_RST_PORT,
                     IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_HIGH);

    R_IOPORT_PinCfg(&g_ioport_ctrl, ADC_DRDY_PORT,
                     IOPORT_CFG_PORT_DIRECTION_INPUT);

    g_adc_stage = 2;

    g_adc_last_err = R_SPI_B_Open(&g_spi0_ctrl, &g_spi0_cfg);

    if (FSP_SUCCESS != g_adc_last_err)
    {
        g_adc_stage = 0xE002;
        vTaskSuspend(NULL);
    }

    g_adc_stage = 3;
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Hardware reset pulse: LOW then HIGH */
    R_IOPORT_PinWrite(&g_ioport_ctrl, ADC_RST_PORT, BSP_IO_LEVEL_LOW);
    vTaskDelay(pdMS_TO_TICKS(1));
    R_IOPORT_PinWrite(&g_ioport_ctrl, ADC_RST_PORT, BSP_IO_LEVEL_HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));

    g_adc_spi_ok = true;
    g_adc_fail_err = FSP_SUCCESS;
    adc_send_cmd(CMD_RESET);
    CHECK_SPI(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    g_adc_id = adc_read_reg(0x00);     // ADS1263 ID register

    g_adc_spi_ok = true;
    g_adc_fail_err = FSP_SUCCESS;
    adc_write_reg(REG_MODE2, 0x8D);
    CHECK_SPI(2);

    g_adc_spi_ok = true;
    g_adc_fail_err = FSP_SUCCESS;
    adc_write_reg(REG_MODE1, 0x00);
    CHECK_SPI(3);

    g_adc_spi_ok = true;
    g_adc_fail_err = FSP_SUCCESS;
    adc_write_reg(REG_MODE0, 0x00);
    CHECK_SPI(4);

    g_adc_spi_ok = true;
    g_adc_fail_err = FSP_SUCCESS;
    adc_write_reg(REG_REFMUX, 0x24);
    CHECK_SPI(5);

    g_adc_spi_ok = true;
    g_adc_fail_err = FSP_SUCCESS;
    adc_write_reg(REG_INPMUX, (0x00 << 4) | 0x0A);
    CHECK_SPI(6);

    vTaskDelay(pdMS_TO_TICKS(10));

    g_adc_spi_ok = true;
    g_adc_fail_err = FSP_SUCCESS;
    adc_send_cmd(CMD_START1);
    CHECK_SPI(7);

    g_adc_stage = 4;

    /* ---- Benchmark: initialize DWT cycle counter once before loop ---- */
    benchmark_dwt_init();
    while (1)
    {
        /* ---- Benchmark: full loop iteration timing starts here ---- */
        uint32_t loop_t0 = benchmark_dwt_get();

        uint32_t wait_count = 0;
        bsp_io_level_t lvl;
        R_IOPORT_PinRead(&g_ioport_ctrl, ADC_DRDY_PORT, &lvl);
        g_drdy_debug_level = (uint8_t)lvl;
        while (!drdy_is_low() && wait_count < 2000)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            wait_count++;
        }

        if (drdy_is_low())
        {
            /* ---- Benchmark: full sample acquisition timing ---- */
            uint32_t sample_t0 = benchmark_dwt_get();

            g_adc_code = adc_read_data();
            g_adc_voltage = (((double) g_adc_code / 2147483648.0) * 2.5);
            g_adc_stage = 5;

            if (g_capture_active)
            {
                g_capture_buffer[g_capture_index] = g_adc_voltage;
                g_capture_index = (g_capture_index + 1) % CAPTURE_BUFFER_SIZE;
                g_capture_total_count++;

                /*
                 * Snapshot the model that will actually produce this result.
                 * The controller may choose a different model afterwards for
                 * the next inference window.
                 */
                model_variant_t variant_used = g_active_variant;

                uint32_t needed =
                    (uint32_t)run_inference_get_required_samples(variant_used);

                g_debug_needed_samples = needed;
                g_debug_active_variant = (uint32_t) variant_used;

                if ((g_capture_total_count - g_last_inference_capture_count) >= needed && needed <= 300) {
                    extract_inference_window(needed);

                    /* ---- Single, correctly-timed inference call ---- */
                    inference_result_t result;

                    uint32_t infer_t0 = benchmark_dwt_get();

                    int err = aria_run_inference(variant_used, g_inference_window, &result, false);
                    g_debug_inference_err = err;
                    g_debug_inference_count++;
                    uint32_t infer_t1 = benchmark_dwt_get();
                    benchmark_record(&g_infer_stats, benchmark_cycles_to_us(infer_t0, infer_t1));

                    if (err == 0) {
                        float p_anomaly = result.p_anomaly;
                        float p_normal = result.p_normal;

                        float current_rms = compute_window_rms(g_inference_window, needed);
                        uint32_t prev_idx = (g_confidence_history_idx + CONFIDENCE_HISTORY_LEN - 1) % CONFIDENCE_HISTORY_LEN;
                        g_meta_features.signal_rms = current_rms;
                        g_meta_features.confidence_trend = p_anomaly - g_confidence_history[prev_idx];
                        g_meta_features.signal_delta = current_rms - g_prev_window_rms;
                        g_meta_features.latency_us = (float)benchmark_cycles_to_us(infer_t0, infer_t1);

                        g_confidence_history[g_confidence_history_idx] = p_anomaly;
                        g_confidence_history_idx = (g_confidence_history_idx + 1) % CONFIDENCE_HISTORY_LEN;

                        float sum = 0.0f;
                        for (int i = 0; i < CONFIDENCE_HISTORY_LEN; i++) {
                            sum += g_confidence_history[i];
                        }
                        g_meta_features.confidence_avg = sum / CONFIDENCE_HISTORY_LEN;

                        g_prev_window_rms = current_rms;

                        /* g_meta_features now holds this frame's runtime signals -- log a row
                         * for meta-controller training data collection. This always runs,
                         * training mode or not, since it's the data we actually want. */
                        if (g_meta_log_index < META_LOG_SIZE) {
                            g_meta_log[g_meta_log_index][0] = g_meta_features.confidence_avg;
                            g_meta_log[g_meta_log_index][1] = g_meta_features.confidence_trend;
                            g_meta_log[g_meta_log_index][2] = g_meta_features.signal_delta;
                            g_meta_log[g_meta_log_index][3] = g_meta_features.latency_us;
                            g_meta_log[g_meta_log_index][4] = g_meta_features.signal_rms;
                            g_meta_log_index++;
                        }

                        /* ---- ARIA_META_TRAINING_MODE switch ----
                         * Training mode: pin to Fast for the whole session so every logged
                         * row is generated from the same reference model (no closed-loop
                         * feedback / no variant-identity leakage into the features).
                         * Normal mode: unchanged meta-controller + transition policy path. */
                        model_variant_t candidate;
    #if ARIA_META_TRAINING_MODE
                        candidate = VARIANT_FAST;        /* not used for control, kept for debug symmetry */
                        g_active_variant = VARIANT_FAST; /* pinned reference model during data collection */
    #else
                        candidate = aria_signal_meta_candidate(
                            g_meta_features.confidence_avg,
                            g_meta_features.confidence_trend,
                            g_meta_features.signal_delta,
                            g_meta_features.signal_rms
                        );

                        /*
                         * Applies confirmation-window / hold-window logic.
                         * g_active_variant is now the selected state for the
                         * next inference window.
                        */
                        g_active_variant = aria_apply_transition_policy(
                            g_active_variant,
                            candidate,
                            g_meta_features.signal_rms
                        );
    #endif

                        g_debug_controller_candidate = (uint32_t) candidate;
                        g_debug_active_variant = (uint32_t) g_active_variant;
                        g_debug_candidate_streak = g_candidate_streak;
                        g_debug_variant_hold_windows = g_variant_hold_windows;
                        /*
                         * Send telemetry ONLY after the new active state has
                         * been chosen, so the dashboard reflects ARIA's current
                         * selected variant.
                        */
                        g_debug_real_telemetry_count++;
                        telemetry_send(
                            variant_used,
                            p_anomaly,
                            (uint32_t) g_meta_features.latency_us,
                            (float) g_adc_voltage
                        );
                    } else {
                        /* Inference failed — do NOT update meta-features or log garbage.
                         * Force fallback to Fast so the system recovers cleanly. */
                        g_debug_inference_err = err;
                        g_active_variant = VARIANT_FAST;
                        g_variant_hold_windows = 0U;
                        g_candidate_streak = 0U;
                        g_controller_candidate = VARIANT_FAST;
                    }
                    g_last_inference_capture_count = g_capture_total_count;
                }
            }

            uint32_t sample_t1 = benchmark_dwt_get();
            benchmark_record(&g_sample_stats, benchmark_cycles_to_us(sample_t0, sample_t1));
        }
        else
        {
            g_adc_stage = 0xE003;
        }

        vTaskDelay(pdMS_TO_TICKS(50));

        /* ---- Benchmark: full loop iteration timing ends here ---- */
        uint32_t loop_t1 = benchmark_dwt_get();
        benchmark_record(&g_loop_stats, benchmark_cycles_to_us(loop_t0, loop_t1));

        /* Refresh human-readable globals every 20 samples */
        if ((g_sample_stats.count % 20) == 0)
        {
            benchmark_refresh_globals();
        }
    }
}
