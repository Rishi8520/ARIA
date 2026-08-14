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

/*
 * Learned ADS routing with adaptive transient escalation.
 *
 * Normal model selection remains driven by the trained meta-controller.
 * Abrupt sample-to-sample changes are split into two severity bands:
 *
 *   moderate sudden spike/dip -> BALANCED immediately
 *   strong sudden spike/dip   -> ACCURATE immediately
 *
 * Both transient requests are latched until one real inference from the
 * requested model completes successfully.  ACCURATE always has priority and
 * may preempt a pending BALANCED transient.
 *
 * This does NOT use an absolute signal-voltage band such as
 * "voltage > X -> model".  It compares the newest step magnitude against the
 * recent step-size baseline, with minimum step floors for noise rejection.
 */

static model_variant_t g_controller_candidate = VARIANT_FAST;
static uint32_t g_candidate_streak = 0U;
static uint32_t g_variant_hold_windows = 0U;

/*
 * Hierarchical scheduler target.  A learned two-tier request is latched long
 * enough to traverse the real model hierarchy one level at a time:
 *
 *     FAST <-> BALANCED <-> ACCURATE
 *
 * This is scheduler state only.  No voltage/RMS threshold participates.
 */
static model_variant_t g_controller_target = VARIANT_FAST;
static bool g_controller_target_pending = false;

/* Useful to expose on dashboard/debugger. */
volatile uint32_t g_debug_controller_candidate = 0U;
volatile uint32_t g_debug_candidate_streak = 0U;
volatile uint32_t g_debug_variant_hold_windows = 0U;
volatile uint32_t g_debug_controller_target = 0U;
volatile uint32_t g_debug_target_pending = 0U;
volatile int32_t  g_debug_inference_err = -999;
volatile uint32_t g_debug_inference_count = 0U;
volatile uint32_t g_debug_real_telemetry_count = 0U;

/*
 * Raw learned output scores (pre-softmax logits) from the existing
 * 4 -> 16 -> 3 trained meta-controller.  These are debugger-visible so the
 * routing decision can be inspected without changing the generated weights.
 */
volatile float g_debug_meta_score_fast = 0.0f;
volatile float g_debug_meta_score_balanced = 0.0f;
volatile float g_debug_meta_score_accurate = 0.0f;
volatile int32_t g_debug_meta_argmax = 0;

/* ---- Adaptive sudden-transient guard ----
 * Detects abrupt positive OR negative sample-to-sample changes and maps them
 * to a compute tier by transient severity.
 *
 * balanced_threshold = max(BALANCED_MIN_STEP,
 *                          BALANCED_MULTIPLIER * recent mean absolute step)
 *
 * accurate_threshold = max(ACCURATE_MIN_STEP,
 *                          ACCURATE_MULTIPLIER * recent mean absolute step)
 *
 * The comparison is against rate-of-change, not absolute ADC voltage.
 * ACCURATE is checked first so a strong event cannot be swallowed by the
 * BALANCED band.
 *
 * Initial tuning deliberately leaves a useful BALANCED region.  If the real
 * hardware needs retuning, only these four severity constants need changing.
 */
#define ADS_TRANSIENT_STEP_EMA_ALPHA          (0.05f)
#define ADS_BALANCED_STEP_MULTIPLIER          (3.0f)
#define ADS_BALANCED_MIN_STEP_V               (0.100f)
#define ADS_ACCURATE_STEP_MULTIPLIER          (6.0f)
#define ADS_ACCURATE_MIN_STEP_V               (0.650f)

typedef enum
{
    ADS_TRANSIENT_NONE = 0,
    ADS_TRANSIENT_BALANCED = 1,
    ADS_TRANSIENT_ACCURATE = 2
} ads_transient_level_t;

static bool  g_ads_transient_initialized = false;
static float g_ads_transient_prev_sample = 0.0f;
static float g_ads_transient_step_ema = 0.0f;
static bool  g_ads_transient_balanced_latched = false;
static bool  g_ads_transient_accurate_latched = false;

volatile float g_debug_transient_signed_step_v = 0.0f;
volatile float g_debug_transient_abs_step_v = 0.0f;
volatile float g_debug_transient_balanced_threshold_v = 0.0f;
volatile float g_debug_transient_accurate_threshold_v = 0.0f;
/* Backward-compatible alias: reports the current ACCURATE threshold. */
volatile float g_debug_transient_threshold_v = 0.0f;
volatile uint32_t g_debug_transient_count = 0U;
volatile uint32_t g_debug_transient_balanced_count = 0U;
volatile uint32_t g_debug_transient_accurate_count = 0U;
/* 0 = no transient latch, 1 = BALANCED latch, 2 = ACCURATE latch. */
volatile uint32_t g_debug_transient_latched = 0U;
volatile uint32_t g_debug_transient_balanced_latched = 0U;
volatile uint32_t g_debug_transient_accurate_latched = 0U;

/* ---- Decoupled live ADS telemetry ----
 * Voltage is transmitted periodically from fresh ADC samples instead of only
 * when an inference window completes.  The model field reports the current
 * learned controller state; confidence/latency remain from the most recent
 * successful real inference. */
#define ADS_LIVE_TELEMETRY_PERIOD_MS (100U)

static float g_ads_last_inference_confidence = 0.0f;
static uint32_t g_ads_last_inference_latency_us = 0U;
static TickType_t g_ads_last_live_telemetry_tick = 0U;

/*
 * Live telemetry is decoupled from inference completion.  Normal routing is
 * driven by the trained meta-controller.  In addition, the adaptive transient
 * guard below can immediately request BALANCED for a moderate abrupt change
 * or ACCURATE for a stronger abrupt change.
 */

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
 * Classify a sudden positive spike or negative dip from sample-to-sample
 * change.  The detector is adaptive: ordinary movement updates an exponential
 * moving average of absolute step size.  Moderate/strong transient samples are
 * not folded into that EMA, preventing a single event from immediately
 * desensitizing the detector.
 */
static ads_transient_level_t aria_ads_detect_sudden_transient(float sample)
{
    if (!g_ads_transient_initialized)
    {
        g_ads_transient_prev_sample = sample;
        g_ads_transient_initialized = true;

        g_debug_transient_signed_step_v = 0.0f;
        g_debug_transient_abs_step_v = 0.0f;
        g_debug_transient_balanced_threshold_v = ADS_BALANCED_MIN_STEP_V;
        g_debug_transient_accurate_threshold_v = ADS_ACCURATE_MIN_STEP_V;
        g_debug_transient_threshold_v = ADS_ACCURATE_MIN_STEP_V;
        return ADS_TRANSIENT_NONE;
    }

    float signed_step = sample - g_ads_transient_prev_sample;
    float abs_step = fabsf(signed_step);
    g_ads_transient_prev_sample = sample;

    float balanced_threshold =
        ADS_BALANCED_STEP_MULTIPLIER * g_ads_transient_step_ema;

    if (balanced_threshold < ADS_BALANCED_MIN_STEP_V)
    {
        balanced_threshold = ADS_BALANCED_MIN_STEP_V;
    }

    float accurate_threshold =
        ADS_ACCURATE_STEP_MULTIPLIER * g_ads_transient_step_ema;

    if (accurate_threshold < ADS_ACCURATE_MIN_STEP_V)
    {
        accurate_threshold = ADS_ACCURATE_MIN_STEP_V;
    }

    /* Keep the ordering valid even if constants are retuned later. */
    if (accurate_threshold <= balanced_threshold)
    {
        accurate_threshold = balanced_threshold + 0.001f;
    }

    g_debug_transient_signed_step_v = signed_step;
    g_debug_transient_abs_step_v = abs_step;
    g_debug_transient_balanced_threshold_v = balanced_threshold;
    g_debug_transient_accurate_threshold_v = accurate_threshold;
    g_debug_transient_threshold_v = accurate_threshold;

    ads_transient_level_t level = ADS_TRANSIENT_NONE;

    /* Strong transient wins over the moderate transient band. */
    if (abs_step >= accurate_threshold)
    {
        level = ADS_TRANSIENT_ACCURATE;
    }
    else if (abs_step >= balanced_threshold)
    {
        level = ADS_TRANSIENT_BALANCED;
    }

    if (level == ADS_TRANSIENT_NONE)
    {
        g_ads_transient_step_ema +=
            ADS_TRANSIENT_STEP_EMA_ALPHA *
            (abs_step - g_ads_transient_step_ema);
    }

    return level;
}

/*
 * Compute the three raw output scores of the EXISTING trained ADS
 * meta-controller without changing meta_controller_weights.h.
 *
 * This reproduces the exact scaler -> ReLU hidden layer -> output layer
 * already used by meta_controller_predict().  The values are logits; only
 * their relative ordering is used here, so no softmax is required.
 */
static void aria_meta_controller_scores(
    float confidence_avg,
    float confidence_trend,
    float signal_delta,
    float signal_rms,
    float out[3])
{
    float x[4] = {
        confidence_avg,
        confidence_trend,
        signal_delta,
        signal_rms
    };

    float xs[4];
    for (int i = 0; i < 4; i++)
    {
        xs[i] = (x[i] - META_SCALER_MEAN[i]) / META_SCALER_SCALE[i];
    }

    float h[META_HIDDEN_UNITS];
    for (int j = 0; j < META_HIDDEN_UNITS; j++)
    {
        float sum = META_B1[j];
        for (int i = 0; i < 4; i++)
        {
            sum += xs[i] * META_W1[i][j];
        }
        h[j] = (sum > 0.0f) ? sum : 0.0f;
    }

    for (int k = 0; k < 3; k++)
    {
        float sum = META_B2[k];
        for (int j = 0; j < META_HIDDEN_UNITS; j++)
        {
            sum += h[j] * META_W2[j][k];
        }
        out[k] = sum;
    }
}

/*
 * Learned-score ADS candidate policy.
 *
 * There are still NO voltage/RMS thresholds here.  The decision is based
 * entirely on the trained MLP's own three output scores.
 *
 * Normal argmax behaviour is kept for FAST and ACCURATE.  When BALANCED is
 * the argmax, the policy looks at which adjacent alternative the learned
 * network supports more strongly:
 *
 *   BALANCED wins + ACCURATE score > FAST score -> ACCURATE
 *   BALANCED wins + FAST score >= ACCURATE score -> BALANCED
 *
 * This is a cost-sensitive policy over learned scores, not a sensor-value
 * threshold.  It prevents a near-tie BALANCED/ACCURATE decision from
 * permanently starving the highest-capacity model.
 */
static model_variant_t aria_learned_meta_candidate(
    float confidence_avg,
    float confidence_trend,
    float signal_delta,
    float signal_rms)
{
    float out[3];
    aria_meta_controller_scores(
        confidence_avg,
        confidence_trend,
        signal_delta,
        signal_rms,
        out);

    g_debug_meta_score_fast = out[(int)VARIANT_FAST];
    g_debug_meta_score_balanced = out[(int)VARIANT_BALANCED];
    g_debug_meta_score_accurate = out[(int)VARIANT_ACCURATE];

    int best = 0;
    for (int k = 1; k < 3; k++)
    {
        if (out[k] > out[best])
        {
            best = k;
        }
    }
    g_debug_meta_argmax = best;

    if (best == (int)VARIANT_FAST)
    {
        return VARIANT_FAST;
    }

    if (best == (int)VARIANT_ACCURATE)
    {
        return VARIANT_ACCURATE;
    }

    /* BALANCED is argmax.  Use the learned runner-up direction. */
    if (out[(int)VARIANT_ACCURATE] > out[(int)VARIANT_FAST])
    {
        return VARIANT_ACCURATE;
    }

    return VARIANT_BALANCED;
}

/*
 * Pure learned hierarchical transition scheduler.
 *
 * The learned score policy above chooses the requested destination.  This
 * scheduler never inspects ADC voltage, RMS, or any physical threshold.  It
 * only enforces one-model-tier-at-a-time movement:
 *
 *     FAST <-> BALANCED <-> ACCURATE
 *
 * A two-level learned request is latched until the intermediate BALANCED
 * model has genuinely run and the requested destination is reached.
 */
static model_variant_t aria_apply_transition_policy(
    model_variant_t active,
    model_variant_t candidate)
{
    if (g_variant_hold_windows < 0xFFFFFFFFU)
    {
        g_variant_hold_windows++;
    }

    /*
     * Start or refresh a destination when no multi-step transition is active.
     * The raw learned candidate is kept separately for debug visibility.
     */
    if (!g_controller_target_pending)
    {
        g_controller_target = candidate;

        if (abs((int)candidate - (int)active) > 1)
        {
            g_controller_target_pending = true;
        }
    }

    model_variant_t target =
        g_controller_target_pending ? g_controller_target : candidate;

    /*
     * Keep the raw learned request visible in g_controller_candidate.
     *
     * When a two-level learned target is pending, the destination has already
     * been latched by the MLP.  Do NOT require the intermediate BALANCED
     * inference to reproduce the original ACCURATE argmax; doing so was the
     * reason a FAST -> BALANCED transition could stall before ACCURATE.
     * During a pending route, the streak therefore tracks scheduler progress
     * toward the already-learned target rather than repeated raw argmax identity.
     */
    bool candidate_changed = (candidate != g_controller_candidate);

    if (candidate_changed)
    {
        g_controller_candidate = candidate;

        if (!g_controller_target_pending)
        {
            g_candidate_streak = 1U;
        }
    }

    if (g_controller_target_pending)
    {
        if (g_candidate_streak < 0xFFFFFFFFU)
        {
            g_candidate_streak++;
        }
    }
    else if (!candidate_changed && g_candidate_streak < 0xFFFFFFFFU)
    {
        g_candidate_streak++;
    }

    if (active == target)
    {
        g_controller_target_pending = false;
        g_controller_target = active;
        g_candidate_streak = 0U;
        return active;
    }

    bool is_escalation = (target > active);

    uint32_t required_windows =
        is_escalation ?
        ESCALATE_CONFIRM_WINDOWS_PER_LEVEL :
        DEESCALATE_CONFIRM_WINDOWS_PER_LEVEL;

    if ((g_variant_hold_windows < MIN_VARIANT_HOLD_WINDOWS) ||
        (g_candidate_streak < required_windows))
    {
        return active;
    }

    model_variant_t next_variant = active;

    if (target > active)
    {
        if (active == VARIANT_FAST)
        {
            next_variant = VARIANT_BALANCED;
        }
        else if (active == VARIANT_BALANCED)
        {
            next_variant = VARIANT_ACCURATE;
        }
    }
    else
    {
        if (active == VARIANT_ACCURATE)
        {
            next_variant = VARIANT_BALANCED;
        }
        else if (active == VARIANT_BALANCED)
        {
            next_variant = VARIANT_FAST;
        }
    }

    g_variant_hold_windows = 0U;
    g_candidate_streak = 0U;

    if (next_variant == target)
    {
        g_controller_target_pending = false;
        g_controller_target = next_variant;
    }

    return next_variant;
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

            ads_transient_level_t transient_level =
                aria_ads_detect_sudden_transient((float)g_adc_voltage);

#if !ARIA_META_TRAINING_MODE
            if (transient_level == ADS_TRANSIENT_ACCURATE)
            {
                /*
                 * Strong abrupt event: ACCURATE has highest priority and may
                 * preempt a pending BALANCED transient.  Keep ACCURATE latched
                 * until one genuine ACCURATE inference succeeds.
                 */
                g_ads_transient_balanced_latched = false;
                g_ads_transient_accurate_latched = true;

                g_debug_transient_count++;
                g_debug_transient_accurate_count++;
                g_debug_transient_latched = 2U;
                g_debug_transient_balanced_latched = 0U;
                g_debug_transient_accurate_latched = 1U;

                g_active_variant = VARIANT_ACCURATE;
                g_controller_target = VARIANT_ACCURATE;
                g_controller_target_pending = false;
                g_candidate_streak = 0U;
                g_variant_hold_windows = 0U;

                g_debug_active_variant = (uint32_t)VARIANT_ACCURATE;
                g_debug_controller_target = (uint32_t)VARIANT_ACCURATE;
                g_debug_target_pending = 0U;
            }
            else if ((transient_level == ADS_TRANSIENT_BALANCED) &&
                     !g_ads_transient_accurate_latched)
            {
                /*
                 * Moderate abrupt event: request BALANCED immediately and
                 * keep it latched until one genuine BALANCED inference
                 * succeeds.  An ACCURATE transient can still preempt it.
                 */
                g_ads_transient_balanced_latched = true;

                g_debug_transient_count++;
                g_debug_transient_balanced_count++;
                g_debug_transient_latched = 1U;
                g_debug_transient_balanced_latched = 1U;
                g_debug_transient_accurate_latched = 0U;

                g_active_variant = VARIANT_BALANCED;
                g_controller_target = VARIANT_BALANCED;
                g_controller_target_pending = false;
                g_candidate_streak = 0U;
                g_variant_hold_windows = 0U;

                g_debug_active_variant = (uint32_t)VARIANT_BALANCED;
                g_debug_controller_target = (uint32_t)VARIANT_BALANCED;
                g_debug_target_pending = 0U;
            }
#else
            FSP_PARAMETER_NOT_USED(transient_level);
#endif

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
                         * Normal mode: trained meta-controller + temporal transition policy only. */
                        model_variant_t candidate;
    #if ARIA_META_TRAINING_MODE
                        candidate = VARIANT_FAST;        /* not used for control, kept for debug symmetry */
                        g_active_variant = VARIANT_FAST; /* pinned reference model during data collection */
    #else
                        candidate = aria_learned_meta_candidate(
                            g_meta_features.confidence_avg,
                            g_meta_features.confidence_trend,
                            g_meta_features.signal_delta,
                            g_meta_features.signal_rms
                        );

                        /*
                         * Normal learned routing uses the hierarchical scheduler.
                         * Transient latches have priority only until one real
                         * inference from the requested tier completes:
                         *
                         *   ACCURATE latch > BALANCED latch > learned scheduler
                         *
                         * A strong transient may preempt BALANCED at sample time;
                         * BALANCED never demotes an active ACCURATE latch.
                         */
                        if (g_ads_transient_accurate_latched)
                        {
                            g_active_variant = VARIANT_ACCURATE;

                            if (variant_used == VARIANT_ACCURATE)
                            {
                                g_ads_transient_accurate_latched = false;
                                g_debug_transient_latched = 0U;
                                g_debug_transient_accurate_latched = 0U;

                                g_controller_target = VARIANT_ACCURATE;
                                g_controller_target_pending = false;
                                g_candidate_streak = 0U;
                                g_variant_hold_windows = 0U;
                            }
                        }
                        else if (g_ads_transient_balanced_latched)
                        {
                            g_active_variant = VARIANT_BALANCED;

                            if (variant_used == VARIANT_BALANCED)
                            {
                                g_ads_transient_balanced_latched = false;
                                g_debug_transient_latched = 0U;
                                g_debug_transient_balanced_latched = 0U;

                                g_controller_target = VARIANT_BALANCED;
                                g_controller_target_pending = false;
                                g_candidate_streak = 0U;
                                g_variant_hold_windows = 0U;
                            }
                        }
                        else
                        {
                            g_active_variant = aria_apply_transition_policy(
                                g_active_variant,
                                candidate
                            );
                        }
    #endif

                        g_debug_controller_candidate = (uint32_t) candidate;
                        g_debug_active_variant = (uint32_t) g_active_variant;
                        g_debug_candidate_streak = g_candidate_streak;
                        g_debug_variant_hold_windows = g_variant_hold_windows;
                        g_debug_controller_target = (uint32_t) g_controller_target;
                        g_debug_target_pending = g_controller_target_pending ? 1U : 0U;
                        /*
                         * Cache the result from the model that actually ran.
                         * Live voltage telemetry is sent independently below,
                         * so the dashboard can update without waiting for the
                         * next inference window to complete.
                         */
                        g_ads_last_inference_confidence = p_anomaly;
                        g_ads_last_inference_latency_us =
                            (uint32_t)g_meta_features.latency_us;
                    } else {
                        /* Inference failed — do NOT update meta-features or log garbage.
                         * Force fallback to Fast so the system recovers cleanly. */
                        g_debug_inference_err = err;
                        g_active_variant = VARIANT_FAST;
                        g_variant_hold_windows = 0U;
                        g_candidate_streak = 0U;
                        g_controller_candidate = VARIANT_FAST;
                        g_controller_target = VARIANT_FAST;
                        g_controller_target_pending = false;
                        g_ads_transient_balanced_latched = false;
                        g_ads_transient_accurate_latched = false;
                        g_debug_transient_latched = 0U;
                        g_debug_transient_balanced_latched = 0U;
                        g_debug_transient_accurate_latched = 0U;
                        g_debug_controller_target = (uint32_t)VARIANT_FAST;
                        g_debug_target_pending = 0U;
                    }
                    g_last_inference_capture_count = g_capture_total_count;
                }
            }

            /*
             * Send live ADS telemetry at ~10 Hz whenever fresh ADC samples are
             * arriving. The voltage is the newest sample. The model is the
             * current learned-controller state; confidence and latency come
             * from the most recent successful real inference. Telemetry does
             * not participate in model selection.
             */
            TickType_t telemetry_now = xTaskGetTickCount();
            if ((telemetry_now - g_ads_last_live_telemetry_tick) >=
                pdMS_TO_TICKS(ADS_LIVE_TELEMETRY_PERIOD_MS))
            {
                g_ads_last_live_telemetry_tick = telemetry_now;
                g_debug_real_telemetry_count++;

                telemetry_send(
                    g_active_variant,
                    g_ads_last_inference_confidence,
                    g_ads_last_inference_latency_us,
                    (float)g_adc_voltage
                );
            }

            uint32_t sample_t1 = benchmark_dwt_get();
            benchmark_record(&g_sample_stats, benchmark_cycles_to_us(sample_t0, sample_t1));
        }
        else
        {
            g_adc_stage = 0xE003;
        }

        vTaskDelay(pdMS_TO_TICKS(5));

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
