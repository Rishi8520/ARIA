#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "imu_thread.h"
#include "run_inference_dispatcher_imu.h"
#include "telemetry.h"

/*
 * IMU meta-controller operating mode
 *
 * 1 = training/data-collection mode:
 *     - collect the existing 5-float meta-feature log
 *     - force the IMU inference variant to FAST
 *
 * 0 = deployed/runtime mode:
 *     - do NOT collect training rows
 *     - use a MOTION-FIRST meta-controller
 *     - still -> FAST
 *     - moderate motion -> BALANCED
 *     - strong motion -> ACCURATE
 *     - the trained MLP is used only as a secondary refinement inside
 *       the intermediate-motion region; it can never override the hard
 *       still->FAST or strong-motion->ACCURATE guarantees.
 */
#ifndef ARIA_META_TRAINING_MODE_IMU
#define ARIA_META_TRAINING_MODE_IMU (0)
#endif

#if ((ARIA_META_TRAINING_MODE_IMU != 0) && (ARIA_META_TRAINING_MODE_IMU != 1))
#error "ARIA_META_TRAINING_MODE_IMU must be 0 (runtime) or 1 (training)"
#endif

#if !ARIA_META_TRAINING_MODE_IMU
#include "meta_controller_weights_ICM.h"
#endif

/* g_spi1: FSP r_spi_b instance, hardware SPI channel 0.
 * Current wiring: P700=MISO0<-SDO, P701=MOSI0->SDA/SDI,
 * P702=RSPCK0->SCL/SCLK, P705=SSLA2->CS.
 * FSP must use Master, Mode 0, Full Duplex, MSB first, SSL2 active-low,
 * SPI_B_SSL_MODE_SPI (4-wire), and Callback=spi_callback. */
#define REG_WHO_AM_I            (0x00U)
#define REG_USER_CTRL           (0x03U)
#define REG_PWR_MGMT_1          (0x06U)
#define REG_PWR_MGMT_2          (0x07U)
#define REG_ACCEL_XOUT_H        (0x2DU)
#define REG_BANK_SEL            (0x7FU)

#define REG_GYRO_SMPLRT_DIV     (0x00U)  /* Bank 2 */
#define REG_GYRO_CONFIG_1       (0x01U)  /* Bank 2 */
#define REG_ACCEL_SMPLRT_DIV_1  (0x10U)  /* Bank 2 */
#define REG_ACCEL_SMPLRT_DIV_2  (0x11U)  /* Bank 2 */
#define REG_ACCEL_CONFIG        (0x14U)  /* Bank 2 */

#define USER_CTRL_I2C_IF_DIS     (0x10U)
#define ICM20948_WHO_AM_I_EXPECTED (0xEAU)
#define ICM_SPI_READ_BIT          (0x80U)

#define IMU_SPI_MAX_RETRIES       (3U)
#define IMU_SPI_RETRY_DELAY_MS    (2U)
#define IMU_SPI_TIMEOUT_MS        (200U)
#define IMU_WHO_AM_I_MAX_RETRIES  (10U)
#define IMU_WHO_AM_I_RETRY_DELAY_MS (20U)

#define IMU_CAPTURE_BUFFER_FRAMES (1400U)
#define IMU_MAX_WINDOW_FLOATS     (1200U)
#define IMU_CONFIDENCE_HISTORY_LEN (3U)
#define IMU_WARMUP_WINDOWS        (IMU_CONFIDENCE_HISTORY_LEN + 1U)
#define IMU_META_LOG_SIZE          (500U)

/*
 * Motion-first runtime policy.
 *
 * IMPORTANT:
 * The old "motion_rms" used sqrt(ax^2 + ay^2 + az^2), which contains gravity.
 * A perfectly stationary board therefore measures roughly 1 g and can look
 * like "strong motion".  Runtime gating now uses:
 *
 *   accel_ac_rms_g : accelerometer RMS AFTER removing each axis' DC mean
 *                    (gravity/orientation is removed)
 *   gyro_rms_dps   : absolute 3-axis gyroscope RMS
 *
 * The legacy RMS is still calculated only for compatibility with the trained
 * MLP's original feature distribution in the intermediate-motion region.
 */
#define IMU_STILL_ACCEL_AC_RMS_MAX_G       (0.025f)
#define IMU_STILL_GYRO_RMS_MAX_DPS         (3.0f)

#define IMU_STRONG_ACCEL_AC_RMS_MIN_G      (0.120f)
#define IMU_STRONG_GYRO_RMS_MIN_DPS        (25.0f)

#define IMU_MID_ACCURATE_ACCEL_MIN_G       (0.060f)
#define IMU_MID_ACCURATE_GYRO_MIN_DPS      (10.0f)


volatile uint32_t g_imu_stage = 0U;
volatile fsp_err_t g_imu_last_err = FSP_SUCCESS;
volatile bool g_imu_spi_ok = true;
volatile uint32_t g_imu_spi_retry_count = 0U;
volatile uint32_t g_imu_spi_fail_count = 0U;
volatile uint32_t g_imu_spi_cb_fire_count = 0U;
volatile spi_event_t g_imu_last_spi_event = SPI_EVENT_TRANSFER_COMPLETE;

volatile uint8_t g_imu_who_am_i = 0U;
volatile uint32_t g_imu_who_am_i_attempts = 0U;
volatile uint8_t g_imu_last_tx[2] = { 0U, 0U };
volatile uint8_t g_imu_last_rx[2] = { 0U, 0U };
volatile uint32_t g_imu_last_spi_len = 0U;

volatile uint8_t g_imu_current_bank = 0xFFU;
volatile int16_t g_imu_accel_raw[3] = { 0, 0, 0 };
volatile int16_t g_imu_gyro_raw[3] = { 0, 0, 0 };
volatile float g_imu_accel_g[3] = { 0.0f, 0.0f, 0.0f };
volatile float g_imu_gyro_dps[3] = { 0.0f, 0.0f, 0.0f };

volatile imu_model_variant_t g_imu_debug_active_variant = IMU_VARIANT_FAST;
volatile int32_t g_imu_debug_inference_err = -999;

/*
 * Meta-controller diagnostics:
 *   g_imu_debug_meta_candidate     = latest raw trained-controller decision
 *   g_imu_debug_meta_predict_count = number of deployed meta predictions
 *
 * During training mode the candidate remains FAST and predict_count remains 0.
 */
volatile imu_model_variant_t g_imu_debug_meta_candidate = IMU_VARIANT_FAST;
volatile uint32_t g_imu_debug_meta_predict_count = 0U;

/* Runtime motion/latency diagnostics. */
volatile float g_imu_debug_motion_rms = 0.0f;          /* legacy RMS for trained MLP */
volatile float g_imu_debug_motion_delta = 0.0f;        /* legacy RMS delta */
volatile float g_imu_debug_motion_abs_delta = 0.0f;

volatile float g_imu_debug_accel_ac_rms_g = 0.0f;      /* gravity/DC removed */
volatile float g_imu_debug_gyro_rms_dps = 0.0f;
volatile uint32_t g_imu_debug_motion_band = 0U;        /* 0=still, 1=moderate, 2=strong */

volatile uint32_t g_imu_debug_inference_latency_us = 0U;
volatile uint32_t g_imu_debug_inference_latency_min_us = 0xFFFFFFFFU;
volatile uint32_t g_imu_debug_inference_latency_max_us = 0U;
volatile uint64_t g_imu_debug_inference_latency_sum_us = 0ULL;
volatile uint32_t g_imu_debug_inference_count = 0U;
volatile uint32_t g_imu_debug_inference_latency_avg_us = 0U;

static volatile float g_imu_capture_buffer[IMU_CAPTURE_BUFFER_FRAMES][6];
static volatile uint32_t g_imu_capture_index = 0U;
static volatile uint32_t g_imu_capture_total_count = 0U;
static uint32_t g_imu_last_inference_capture_count = 0U;
static float g_imu_inference_window[IMU_MAX_WINDOW_FLOATS];
static volatile imu_model_variant_t g_imu_active_variant = IMU_VARIANT_FAST;

static float g_imu_fault_history[IMU_CONFIDENCE_HISTORY_LEN] = { 0.0f };
static uint32_t g_imu_fault_history_idx = 0U;
static float g_imu_prev_motion_rms = 0.0f;
static uint32_t g_imu_warmup_windows_remaining = IMU_WARMUP_WINDOWS;

static volatile float g_imu_meta_log[IMU_META_LOG_SIZE][5];
static volatile uint32_t g_imu_meta_log_index = 0U;

static SemaphoreHandle_t g_imu_spi_xfer_done_sem;
static StaticSemaphore_t g_imu_spi_sem_buffer;

void spi_callback(spi_callback_args_t *p_args)
{
    BaseType_t higher_priority_woken = pdFALSE;

    g_imu_spi_cb_fire_count++;
    g_imu_last_spi_event = p_args->event;
    g_imu_spi_ok = (SPI_EVENT_TRANSFER_COMPLETE == p_args->event);

    if (NULL != g_imu_spi_xfer_done_sem)
    {
        xSemaphoreGiveFromISR(g_imu_spi_xfer_done_sem, &higher_priority_woken);
    }

    portYIELD_FROM_ISR(higher_priority_woken);
}

static bool imu_spi_transfer_once(uint8_t *tx, uint8_t *rx, uint32_t len)
{
    if ((NULL == tx) || (NULL == rx) || (0U == len) || (NULL == g_imu_spi_xfer_done_sem))
    {
        return false;
    }

    while (pdTRUE == xSemaphoreTake(g_imu_spi_xfer_done_sem, 0U))
    {
    }

    /*
     * IMPORTANT:
     * Arm the completion state BEFORE starting the SPI transfer.
     *
     * The r_spi_b transfer can complete very quickly.  If g_imu_spi_ok were
     * cleared after R_SPI_B_WriteRead(), the callback could already have run,
     * set g_imu_spi_ok=true and released the semaphore, after which this task
     * would overwrite g_imu_spi_ok back to false.  That race makes a successful
     * WHO_AM_I transaction look like a failure.
     */
    uint32_t in_use_retries = 50U;

    do
    {
        g_imu_spi_ok = false;
        g_imu_last_spi_event = SPI_EVENT_TRANSFER_COMPLETE;

        g_imu_last_err = R_SPI_B_WriteRead(
            &g_spi1_ctrl,
            tx,
            rx,
            len,
            SPI_BIT_WIDTH_8_BITS);

        if (FSP_ERR_IN_USE == g_imu_last_err)
        {
            vTaskDelay(pdMS_TO_TICKS(5U));
        }
    } while ((FSP_ERR_IN_USE == g_imu_last_err) &&
             (--in_use_retries > 0U));

    if (FSP_SUCCESS != g_imu_last_err)
    {
        return false;
    }

    bool callback_received = (pdTRUE == xSemaphoreTake(
        g_imu_spi_xfer_done_sem,
        pdMS_TO_TICKS(IMU_SPI_TIMEOUT_MS)));

    return callback_received &&
           g_imu_spi_ok &&
           (SPI_EVENT_TRANSFER_COMPLETE == g_imu_last_spi_event);
}

static bool imu_spi_transfer(uint8_t *tx, uint8_t *rx, uint32_t len)
{
    for (uint32_t attempt = 0U; attempt < IMU_SPI_MAX_RETRIES; attempt++)
    {
        if (imu_spi_transfer_once(tx, rx, len))
        {
            return true;
        }

        g_imu_spi_retry_count++;
        vTaskDelay(pdMS_TO_TICKS(IMU_SPI_RETRY_DELAY_MS));
    }

    g_imu_spi_fail_count++;
    return false;
}

static bool imu_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { (uint8_t)(reg & 0x7FU), value };
    uint8_t rx[2] = { 0U, 0U };
    return imu_spi_transfer(tx, rx, sizeof(tx));
}

static bool imu_read_reg(uint8_t reg, uint8_t *out)
{
    uint8_t tx[2] = { (uint8_t)(reg | ICM_SPI_READ_BIT), 0x00U };
    uint8_t rx[2] = { 0U, 0U };

    if (!imu_spi_transfer(tx, rx, sizeof(tx)))
    {
        return false;
    }

    *out = rx[1];
    return true;
}

static bool imu_read_multi(uint8_t reg, uint8_t *out, uint32_t len)
{
    uint8_t tx[13] = { 0U };
    uint8_t rx[13] = { 0U };

    if ((NULL == out) || (len > 12U))
    {
        return false;
    }

    tx[0] = (uint8_t)(reg | ICM_SPI_READ_BIT);
    if (!imu_spi_transfer(tx, rx, len + 1U))
    {
        return false;
    }

    for (uint32_t i = 0U; i < len; i++)
    {
        out[i] = rx[i + 1U];
    }

    return true;
}

static bool imu_select_bank(uint8_t bank)
{
    if (g_imu_current_bank == bank)
    {
        return true;
    }

    if (!imu_write_reg(REG_BANK_SEL, (uint8_t)(bank << 4U)))
    {
        return false;
    }

    g_imu_current_bank = bank;
    return true;
}

static bool imu_write_reg_banked(uint8_t bank, uint8_t reg, uint8_t value)
{
    return imu_select_bank(bank) && imu_write_reg(reg, value);
}

static bool imu_read_who_am_i_direct(void)
{
    uint8_t tx[2] = { (uint8_t)(REG_WHO_AM_I | ICM_SPI_READ_BIT), 0x00U };
    uint8_t rx[2] = { 0U, 0U };

    g_imu_last_tx[0] = tx[0];
    g_imu_last_tx[1] = tx[1];
    g_imu_last_rx[0] = 0U;
    g_imu_last_rx[1] = 0U;
    g_imu_last_spi_len = sizeof(tx);

    if (!imu_spi_transfer(tx, rx, sizeof(tx)))
    {
        return false;
    }

    g_imu_last_rx[0] = rx[0];
    g_imu_last_rx[1] = rx[1];
    g_imu_who_am_i = rx[1];

    return (ICM20948_WHO_AM_I_EXPECTED == rx[1]);
}

static bool imu_probe_who_am_i(void)
{
    g_imu_current_bank = 0xFFU;

    for (uint32_t attempt = 0U; attempt < IMU_WHO_AM_I_MAX_RETRIES; attempt++)
    {
        g_imu_who_am_i_attempts = attempt + 1U;
        if (imu_read_who_am_i_direct())
        {
            g_imu_current_bank = 0U;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_WHO_AM_I_RETRY_DELAY_MS));
    }

    return false;
}

static bool imu_disable_i2c_interface(void)
{
    uint8_t user_ctrl = 0U;

    if (!imu_select_bank(0U) || !imu_read_reg(REG_USER_CTRL, &user_ctrl))
    {
        return false;
    }

    return imu_write_reg(REG_USER_CTRL, (uint8_t)(user_ctrl | USER_CTRL_I2C_IF_DIS));
}

static bool imu_hardware_bringup(void)
{
    if (!imu_probe_who_am_i())
    {
        return false;
    }

    if (!imu_disable_i2c_interface())
    {
        return false;
    }

    if (!imu_write_reg_banked(0U, REG_PWR_MGMT_1, 0x80U))
    {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(250U));
    g_imu_current_bank = 0xFFU;

    if (!imu_select_bank(0U) ||
        !imu_write_reg(REG_PWR_MGMT_1, 0x01U) ||
        !imu_write_reg(REG_PWR_MGMT_2, 0x00U))
    {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(20U));

    if (!imu_disable_i2c_interface() ||
        !imu_write_reg_banked(2U, REG_GYRO_CONFIG_1, 0x00U) ||
        !imu_write_reg_banked(2U, REG_GYRO_SMPLRT_DIV, 0x00U) ||
        !imu_write_reg_banked(2U, REG_ACCEL_CONFIG, 0x00U) ||
        !imu_write_reg_banked(2U, REG_ACCEL_SMPLRT_DIV_1, 0x00U) ||
        !imu_write_reg_banked(2U, REG_ACCEL_SMPLRT_DIV_2, 0x00U))
    {
        return false;
    }

    return imu_select_bank(0U);
}

static bool imu_read_accel_gyro_raw(void)
{
    uint8_t buf[12];

    if (!imu_select_bank(0U) || !imu_read_multi(REG_ACCEL_XOUT_H, buf, sizeof(buf)))
    {
        return false;
    }

    g_imu_accel_raw[0] = (int16_t)(((uint16_t)buf[0] << 8U) | buf[1]);
    g_imu_accel_raw[1] = (int16_t)(((uint16_t)buf[2] << 8U) | buf[3]);
    g_imu_accel_raw[2] = (int16_t)(((uint16_t)buf[4] << 8U) | buf[5]);
    g_imu_gyro_raw[0]  = (int16_t)(((uint16_t)buf[6] << 8U) | buf[7]);
    g_imu_gyro_raw[1]  = (int16_t)(((uint16_t)buf[8] << 8U) | buf[9]);
    g_imu_gyro_raw[2]  = (int16_t)(((uint16_t)buf[10] << 8U) | buf[11]);

    for (uint32_t i = 0U; i < 3U; i++)
    {
        g_imu_accel_g[i] = (float)g_imu_accel_raw[i] / 16384.0f;
        g_imu_gyro_dps[i] = (float)g_imu_gyro_raw[i] / 131.0f;
    }

    return true;
}

/* float_count is the value returned by imu_run_inference_get_required_floats().
 * The model input is frame-interleaved:
 * [accX, accY, accZ, gyrX, gyrY, gyrZ] repeated once per sample frame. */
static bool imu_extract_inference_window(uint32_t float_count)
{
    if ((float_count == 0U) || ((float_count % 6U) != 0U) ||
        (float_count > IMU_MAX_WINDOW_FLOATS))
    {
        return false;
    }

    uint32_t frame_count = float_count / 6U;

    for (uint32_t frame = 0U; frame < frame_count; frame++)
    {
        int32_t index = (int32_t)g_imu_capture_index - (int32_t)frame_count + (int32_t)frame;
        while (index < 0)
        {
            index += (int32_t)IMU_CAPTURE_BUFFER_FRAMES;
        }
        index %= (int32_t)IMU_CAPTURE_BUFFER_FRAMES;

        g_imu_inference_window[(frame * 6U) + 0U] = g_imu_capture_buffer[index][0];
        g_imu_inference_window[(frame * 6U) + 1U] = g_imu_capture_buffer[index][1];
        g_imu_inference_window[(frame * 6U) + 2U] = g_imu_capture_buffer[index][2];
        g_imu_inference_window[(frame * 6U) + 3U] = g_imu_capture_buffer[index][3];
        g_imu_inference_window[(frame * 6U) + 4U] = g_imu_capture_buffer[index][4];
        g_imu_inference_window[(frame * 6U) + 5U] = g_imu_capture_buffer[index][5];
    }

    return true;
}

/* ============================================================
 * DWT-based inference timing (Cortex-M85)
 * ============================================================ */

static inline void imu_benchmark_dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t imu_benchmark_dwt_get(void)
{
    return DWT->CYCCNT;
}

static inline uint32_t imu_benchmark_cycles_to_us(uint32_t start, uint32_t end)
{
    /* Unsigned subtraction correctly handles one CYCCNT wrap. */
    uint32_t delta = end - start;

    if (SystemCoreClock == 0U)
    {
        return 0U;
    }

    return (uint32_t)(((uint64_t)delta * 1000000ULL) /
                      (uint64_t)SystemCoreClock);
}

static void imu_record_inference_latency(uint32_t latency_us)
{
    g_imu_debug_inference_latency_us = latency_us;
    g_imu_debug_inference_count++;

    if (latency_us < g_imu_debug_inference_latency_min_us)
    {
        g_imu_debug_inference_latency_min_us = latency_us;
    }

    if (latency_us > g_imu_debug_inference_latency_max_us)
    {
        g_imu_debug_inference_latency_max_us = latency_us;
    }

    g_imu_debug_inference_latency_sum_us += (uint64_t)latency_us;

    g_imu_debug_inference_latency_avg_us =
        (uint32_t)(g_imu_debug_inference_latency_sum_us /
                   (uint64_t)g_imu_debug_inference_count);
}

static float imu_compute_legacy_motion_rms(uint32_t frame_count)
{
    if (frame_count == 0U)
    {
        return 0.0f;
    }

    double sum_sq = 0.0;

    for (uint32_t frame = 0U; frame < frame_count; frame++)
    {
        double ax = g_imu_inference_window[(frame * 6U) + 0U];
        double ay = g_imu_inference_window[(frame * 6U) + 1U];
        double az = g_imu_inference_window[(frame * 6U) + 2U];

        sum_sq += (ax * ax) + (ay * ay) + (az * az);
    }

    return (float)sqrt(sum_sq / (double)frame_count);
}

typedef struct
{
    float accel_ac_rms_g;
    float gyro_rms_dps;
} imu_motion_activity_t;

/*
 * Compute physical motion without gravity contamination.
 *
 * Accelerometer:
 *   subtract the per-axis window mean first, then calculate vector RMS.
 *   A stationary sensor at any orientation therefore approaches 0 g dynamic
 *   acceleration rather than approximately 1 g.
 *
 * Gyroscope:
 *   keep absolute angular-rate RMS. Constant rotation is real motion and must
 *   not be removed by mean subtraction.
 */
static imu_motion_activity_t imu_compute_motion_activity(uint32_t frame_count)
{
    imu_motion_activity_t activity = { 0.0f, 0.0f };

    if (frame_count == 0U)
    {
        return activity;
    }

    double mean_ax = 0.0;
    double mean_ay = 0.0;
    double mean_az = 0.0;

    for (uint32_t frame = 0U; frame < frame_count; frame++)
    {
        mean_ax += g_imu_inference_window[(frame * 6U) + 0U];
        mean_ay += g_imu_inference_window[(frame * 6U) + 1U];
        mean_az += g_imu_inference_window[(frame * 6U) + 2U];
    }

    mean_ax /= (double)frame_count;
    mean_ay /= (double)frame_count;
    mean_az /= (double)frame_count;

    double accel_sum_sq = 0.0;
    double gyro_sum_sq = 0.0;

    for (uint32_t frame = 0U; frame < frame_count; frame++)
    {
        double ax = g_imu_inference_window[(frame * 6U) + 0U] - mean_ax;
        double ay = g_imu_inference_window[(frame * 6U) + 1U] - mean_ay;
        double az = g_imu_inference_window[(frame * 6U) + 2U] - mean_az;

        double gx = g_imu_inference_window[(frame * 6U) + 3U];
        double gy = g_imu_inference_window[(frame * 6U) + 4U];
        double gz = g_imu_inference_window[(frame * 6U) + 5U];

        accel_sum_sq += (ax * ax) + (ay * ay) + (az * az);
        gyro_sum_sq += (gx * gx) + (gy * gy) + (gz * gz);
    }

    activity.accel_ac_rms_g =
        (float)sqrt(accel_sum_sq / (double)frame_count);

    activity.gyro_rms_dps =
        (float)sqrt(gyro_sum_sq / (double)frame_count);

    return activity;
}


/*
 * Convert the trained controller's integer class to the IMU variant enum.
 * The generated header uses:
 *   0 = Fast, 1 = Balanced, 2 = Accurate.
 *
 * Any unexpected value fails safely to FAST.
 */
static imu_model_variant_t imu_variant_from_meta_class(int meta_class)
{
    switch (meta_class)
    {
        case 0:
            return IMU_VARIANT_FAST;

        case 1:
            return IMU_VARIANT_BALANCED;

        case 2:
            return IMU_VARIANT_ACCURATE;

        default:
            return IMU_VARIANT_FAST;
    }
}

/*
 * Motion-first deployed meta-controller.
 *
 * Hard guarantees:
 *   - stationary module -> FAST
 *   - clearly strong motion -> ACCURATE
 *
 * Intermediate motion is BALANCED by default.  The trained MLP may promote
 * the intermediate case to ACCURATE only if the measured motion itself has
 * crossed the mid-motion promotion gate.  This prevents confidence features
 * from selecting ACCURATE while the board is sitting still.
 */
static imu_model_variant_t imu_motion_meta_controller_predict(
    float confidence_avg,
    float confidence_trend,
    float legacy_motion_delta,
    float legacy_motion_rms,
    imu_motion_activity_t activity)
{
    g_imu_debug_motion_rms = legacy_motion_rms;
    g_imu_debug_motion_delta = legacy_motion_delta;
    g_imu_debug_motion_abs_delta = fabsf(legacy_motion_delta);

    g_imu_debug_accel_ac_rms_g = activity.accel_ac_rms_g;
    g_imu_debug_gyro_rms_dps = activity.gyro_rms_dps;

    /*
     * HARD still guarantee.
     * No neural-network confidence is allowed to override a physically
     * stationary IMU.
     */
    if ((activity.accel_ac_rms_g <= IMU_STILL_ACCEL_AC_RMS_MAX_G) &&
        (activity.gyro_rms_dps <= IMU_STILL_GYRO_RMS_MAX_DPS))
    {
        g_imu_debug_motion_band = 0U;
        return IMU_VARIANT_FAST;
    }

    /*
     * HARD strong-motion guarantee.
     */
    if ((activity.accel_ac_rms_g >= IMU_STRONG_ACCEL_AC_RMS_MIN_G) ||
        (activity.gyro_rms_dps >= IMU_STRONG_GYRO_RMS_MIN_DPS))
    {
        g_imu_debug_motion_band = 2U;
        return IMU_VARIANT_ACCURATE;
    }

    /*
     * Moderate physical motion: BALANCED by default.
     * The existing trained MLP is retained only as a secondary refinement.
     * It may promote to ACCURATE only if physical motion itself is already
     * above the intermediate promotion threshold.
     */
    g_imu_debug_motion_band = 1U;

#if !ARIA_META_TRAINING_MODE_IMU
    int learned_class = meta_controller_predict(
        confidence_avg,
        confidence_trend,
        legacy_motion_delta,
        legacy_motion_rms);

    if ((2 == learned_class) &&
        ((activity.accel_ac_rms_g >= IMU_MID_ACCURATE_ACCEL_MIN_G) ||
         (activity.gyro_rms_dps >= IMU_MID_ACCURATE_GYRO_MIN_DPS)))
    {
        return IMU_VARIANT_ACCURATE;
    }
#else
    FSP_PARAMETER_NOT_USED(confidence_avg);
    FSP_PARAMETER_NOT_USED(confidence_trend);
#endif

    return IMU_VARIANT_BALANCED;
}


/*
 * telemetry_send_imu() currently accepts the ADS-side model_variant_t type.
 * Do not rely on the two enums having identical integer representation:
 * map the variants explicitly.
 */
static model_variant_t imu_variant_to_telemetry_variant(imu_model_variant_t variant)
{
    switch (variant)
    {
        case IMU_VARIANT_FAST:
            return VARIANT_FAST;

        case IMU_VARIANT_BALANCED:
            return VARIANT_BALANCED;

        case IMU_VARIANT_ACCURATE:
            return VARIANT_ACCURATE;

        default:
            return VARIANT_FAST;
    }
}

void imu_thread_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);

    g_imu_stage = 1U;
    g_imu_spi_xfer_done_sem = xSemaphoreCreateBinaryStatic(&g_imu_spi_sem_buffer);
    if (NULL == g_imu_spi_xfer_done_sem)
    {
        g_imu_stage = 0xE001U;
        vTaskSuspend(NULL);
    }

    g_imu_last_err = R_SPI_B_Open(&g_spi1_ctrl, &g_spi1_cfg);
    if (FSP_SUCCESS != g_imu_last_err)
    {
        g_imu_stage = 0xE002U;
        vTaskSuspend(NULL);
    }

    g_imu_stage = 2U;
    vTaskDelay(pdMS_TO_TICKS(250U));

    if (!imu_hardware_bringup())
    {
        g_imu_stage = 0xE003U;
        vTaskSuspend(NULL);
    }

    g_imu_stage = 3U;

    /* Same Cortex-M85 DWT timing mechanism already used by the ADS path. */
    imu_benchmark_dwt_init();

    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t sample_period = pdMS_TO_TICKS(10U);

    while (true)
    {
        if (!imu_read_accel_gyro_raw())
        {
            g_imu_stage = 0xE004U;
            vTaskDelayUntil(&last_wake_time, sample_period);
            continue;
        }

        g_imu_capture_buffer[g_imu_capture_index][0] = g_imu_accel_g[0];
        g_imu_capture_buffer[g_imu_capture_index][1] = g_imu_accel_g[1];
        g_imu_capture_buffer[g_imu_capture_index][2] = g_imu_accel_g[2];
        g_imu_capture_buffer[g_imu_capture_index][3] = g_imu_gyro_dps[0];
        g_imu_capture_buffer[g_imu_capture_index][4] = g_imu_gyro_dps[1];
        g_imu_capture_buffer[g_imu_capture_index][5] = g_imu_gyro_dps[2];

        g_imu_capture_index = (g_imu_capture_index + 1U) % IMU_CAPTURE_BUFFER_FRAMES;
        g_imu_capture_total_count++;

        /*
         * Snapshot the model that will actually run this inference.  The
         * meta-controller may choose a different model afterwards for the
         * NEXT inference window.
         */
        imu_model_variant_t variant_used = g_imu_active_variant;

        uint32_t needed_floats =
            (uint32_t)imu_run_inference_get_required_floats(variant_used);
        uint32_t needed_frames = needed_floats / 6U;

        g_imu_debug_active_variant = variant_used;

        bool window_is_valid = (needed_floats > 0U) &&
                               ((needed_floats % 6U) == 0U) &&
                               (needed_floats <= IMU_MAX_WINDOW_FLOATS);

        if (window_is_valid &&
            ((g_imu_capture_total_count - g_imu_last_inference_capture_count) >= needed_frames))
        {
            if (!imu_extract_inference_window(needed_floats))
            {
                g_imu_stage = 0xE006U;
                g_imu_last_inference_capture_count = g_imu_capture_total_count;
                vTaskDelayUntil(&last_wake_time, sample_period);
                continue;
            }

            imu_inference_result_t result;

            uint32_t infer_t0 = imu_benchmark_dwt_get();

            int err = aria_run_inference_imu(
                variant_used,
                g_imu_inference_window,
                &result,
                false);

            uint32_t infer_t1 = imu_benchmark_dwt_get();
            uint32_t inference_latency_us =
                imu_benchmark_cycles_to_us(infer_t0, infer_t1);

            imu_record_inference_latency(inference_latency_us);

            g_imu_debug_inference_err = err;
            g_imu_last_inference_capture_count = g_imu_capture_total_count;

            if (0 == err)
            {
                float motion_rms = imu_compute_legacy_motion_rms(needed_frames);
                imu_motion_activity_t motion_activity =
                    imu_compute_motion_activity(needed_frames);

                uint32_t prev_idx = (g_imu_fault_history_idx + IMU_CONFIDENCE_HISTORY_LEN - 1U) % IMU_CONFIDENCE_HISTORY_LEN;
                float fault_trend = result.p_fault - g_imu_fault_history[prev_idx];
                float motion_delta = motion_rms - g_imu_prev_motion_rms;

                g_imu_fault_history[g_imu_fault_history_idx] = result.p_fault;
                g_imu_fault_history_idx = (g_imu_fault_history_idx + 1U) % IMU_CONFIDENCE_HISTORY_LEN;

                float fault_sum = 0.0f;
                for (uint32_t i = 0U; i < IMU_CONFIDENCE_HISTORY_LEN; i++)
                {
                    fault_sum += g_imu_fault_history[i];
                }
                float fault_avg = fault_sum / (float)IMU_CONFIDENCE_HISTORY_LEN;
                g_imu_prev_motion_rms = motion_rms;

                if (g_imu_warmup_windows_remaining > 0U)
                {
                    /*
                     * The confidence average/trend are not meaningful until
                     * the history buffer has been populated.  Keep FAST during
                     * this short warm-up in BOTH modes.
                     */
                    g_imu_warmup_windows_remaining--;
                    g_imu_active_variant = IMU_VARIANT_FAST;
                    g_imu_debug_meta_candidate = IMU_VARIANT_FAST;
                }
                else
                {
#if ARIA_META_TRAINING_MODE_IMU
                    /*
                     * =========================================================
                     * TRAINING / DATA-COLLECTION MODE
                     * =========================================================
                     *
                     * Preserve the exact 5-float binary layout used to train
                     * the current ICM meta-controller:
                     *
                     * [0] confidence_avg
                     * [1] confidence_trend
                     * [2] signal_delta
                     * [3] latency_us   (unused placeholder = 0.0f)
                     * [4] signal_rms
                     *
                     * Training mode deliberately forces FAST so every captured
                     * row is generated under the same base-model policy.
                     */
                    if (g_imu_meta_log_index < IMU_META_LOG_SIZE)
                    {
                        g_imu_meta_log[g_imu_meta_log_index][0] = fault_avg;
                        g_imu_meta_log[g_imu_meta_log_index][1] = fault_trend;
                        g_imu_meta_log[g_imu_meta_log_index][2] = motion_delta;
                        g_imu_meta_log[g_imu_meta_log_index][3] =
                            (float)inference_latency_us;
                        g_imu_meta_log[g_imu_meta_log_index][4] = motion_rms;
                        g_imu_meta_log_index++;
                    }

                    g_imu_active_variant = IMU_VARIANT_FAST;
                    g_imu_debug_meta_candidate = IMU_VARIANT_FAST;

#else
                    /*
                     * =========================================================
                     * DEPLOYED MOTION-FIRST META-CONTROLLER MODE
                     * =========================================================
                     *
                     * The physical motion gate has priority over the learned
                     * classifier:
                     *
                     *   still            -> FAST
                     *   moderate motion  -> BALANCED (MLP may promote)
                     *   strong motion    -> ACCURATE
                     *
                     * This is intentionally different from allowing the MLP
                     * to select a model solely from confidence-related
                     * features, which could produce ACCURATE while stationary.
                     */
                    imu_model_variant_t candidate =
                        imu_motion_meta_controller_predict(
                            fault_avg,
                            fault_trend,
                            motion_delta,
                            motion_rms,
                            motion_activity);

                    g_imu_debug_meta_candidate = candidate;
                    g_imu_debug_meta_predict_count++;

                    /* Selected model is used for the NEXT inference window. */
                    g_imu_active_variant = candidate;
#endif
                }

                /*
                 * Telemetry describes the model that ACTUALLY produced this
                 * confidence/latency pair.  g_imu_active_variant may already
                 * contain the model selected for the next window.
                 */
                g_imu_debug_active_variant = variant_used;

                telemetry_send_imu(
                    imu_variant_to_telemetry_variant(variant_used),
                    result.p_fault,
                    inference_latency_us);
            }
        }

        vTaskDelayUntil(&last_wake_time, sample_period);
    }
}
