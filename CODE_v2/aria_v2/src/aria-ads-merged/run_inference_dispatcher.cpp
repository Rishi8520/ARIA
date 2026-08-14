#include "run_inference_dispatcher.h"
#include "run_inference_dispatcher_imu.h"

/*
 * ============================================================
 * IMPORTANT
 * ============================================================
 *
 * ADS1263 and ICM-20948 Edge Impulse inference are intentionally
 * kept in this ONE C++ translation unit.
 *
 * Do NOT manually define Edge Impulse DSP capability macros here.
 *
 * The ADS path continues to use the normal Edge Impulse
 * run_classifier() flow.
 *
 * The ICM path intentionally BYPASSES the generated spectral-DSP
 * function because the merged ADS metadata disables the FFT
 * capability required by the ICM model.
 *
 * For ICM only:
 *
 *     raw IMU samples
 *          ->
 *     local EI-compatible spectral feature extraction
 *          ->
 *     direct generated EON graph init/input/invoke/output/reset
 *          ->
 *     direct int8 classification dequantization
 *
 * The ICM path does NOT call run_nn_inference() or
 * run_postprocessing(); it calls the generated EON graph directly.
 *
 * No Edge Impulse SDK .hpp file is modified.
 * No shared model_metadata.h FFT capability change is required.
 *
 * ============================================================
 */

/* Edge Impulse SDK headers. */
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/classifier/ei_run_dsp.h"
#include "edge-impulse-sdk/classifier/ei_data_normalization.h"
#include "edge-impulse-sdk/classifier/postprocessing/ei_postprocessing.h"

/* Generated model definitions. */
#include "model-parameters/model_variables.h"
#include "model-parameters/model_variables_ICM.h"

#include <cstring>
#include <cmath>
#include <cstdint>

/* ============================================================
 * ADS1263 dispatcher
 * ============================================================
 *
 * Keep this path unchanged from the working ADS implementation.
 * ============================================================ */

static const float *g_active_window = nullptr;

extern "C" volatile int32_t g_ads_direct_dsp_result = -999;
extern "C" volatile uint32_t g_ads_dsp_output_size = 0U;
extern "C" volatile uint32_t g_ads_dsp_block_count = 0U;
extern "C" volatile uint32_t g_ads_nn_input_size = 0U;

/* Edge Impulse signal callback for ADS. */
static int get_signal_data(
    size_t offset,
    size_t length,
    float *out_ptr)
{
    memcpy(
        out_ptr,
        g_active_window + offset,
        length * sizeof(float));

    return 0;
}

/* Return number of samples required by the selected ADS model. */
size_t run_inference_get_required_samples(
    model_variant_t variant)
{
    switch (variant)
    {
        case VARIANT_FAST:
            return impulse_fast.raw_sample_count;

        case VARIANT_BALANCED:
            return impulse_balanced.raw_sample_count;

        case VARIANT_ACCURATE:
            return impulse_accurate.raw_sample_count;

        default:
            return 0U;
    }
}

/* ============================================================
 * ADS1263 inference
 * ============================================================ */

/*
 * Direct ADS DSP diagnostic probe.
 *
 * This does NOT replace the normal ADS run_classifier() path. It only calls
 * the selected impulse's first DSP block directly so the underlying EIDSP
 * return code can be inspected in the debugger instead of only seeing the
 * generic EI_IMPULSE_DSP_ERROR (-5).
 */
static int ads_probe_dsp_direct(
    ei_impulse_handle_t *handle,
    const float *sample_window,
    size_t sample_count)
{
    if ((handle == nullptr) ||
        (handle->impulse == nullptr) ||
        (sample_window == nullptr))
    {
        return -100;
    }

    const ei_impulse_t *impulse =
        handle->impulse;

    g_ads_dsp_block_count =
        (uint32_t)impulse->dsp_blocks_size;

    g_ads_nn_input_size =
        (uint32_t)impulse->nn_input_frame_size;

    if (impulse->dsp_blocks_size == 0U)
    {
        return -101;
    }

    ei_model_dsp_t *dsp_block =
        &impulse->dsp_blocks[0];

    g_ads_dsp_output_size =
        (uint32_t)dsp_block->n_output_features;

    g_active_window =
        sample_window;

    signal_t signal;
    signal.total_length =
        sample_count;
    signal.get_data =
        &get_signal_data;

    matrix_t output_matrix(
        1,
        dsp_block->n_output_features);

    int result =
        dsp_block->extract_fn(
            &signal,
            &output_matrix,
            dsp_block->config,
            impulse->frequency);

    g_active_window =
        nullptr;

    return result;
}

int aria_run_inference(
    model_variant_t variant,
    const float *sample_window,
    inference_result_t *out_result,
    bool debug)
{
    if (out_result == nullptr)
    {
        return -1;
    }

    out_result->error_code = -1;
    out_result->p_anomaly = 0.0f;
    out_result->p_normal = 0.0f;

    ei_impulse_handle_t *handle = nullptr;

    size_t sample_count =
        run_inference_get_required_samples(
            variant);

    switch (variant)
    {
        case VARIANT_FAST:
            handle = &impulse_handle_fast;
            break;

        case VARIANT_BALANCED:
            handle = &impulse_handle_balanced;
            break;

        case VARIANT_ACCURATE:
            handle = &impulse_handle_accurate;
            break;

        default:
            return -1;
    }

    if ((sample_window == nullptr) ||
        (sample_count == 0U))
    {
        return -1;
    }

    g_active_window = sample_window;

    signal_t signal;

    signal.total_length = sample_count;
    signal.get_data = &get_signal_data;

    ei_impulse_result_t result;

    memset(
        &result,
        0,
        sizeof(result));

    g_ads_direct_dsp_result =
        ads_probe_dsp_direct(
            handle,
            sample_window,
            sample_count);

    EI_IMPULSE_ERROR err =
        run_classifier(
            handle,
            &signal,
            &result,
            debug);

    g_active_window = nullptr;

    out_result->error_code = (int)err;

    if (err == EI_IMPULSE_OK)
    {
        out_result->p_anomaly =
            result.classification[0].value;

        out_result->p_normal =
            result.classification[1].value;
    }

    return (int)err;
}

/* ============================================================
 * ICM-20948 local spectral DSP
 * ============================================================
 *
 * The generated ICM models use spectral analysis implementation
 * version 4 with:
 *
 *     axes                   = 6
 *     scale_axes             = 1
 *     input_decimation_ratio = 1
 *     filter_type            = "none"
 *     analysis_type          = "FFT"
 *     fft_length             = 16
 *     do_log                 = true
 *     do_fft_overlap         = true
 *     extra_low_freq         = false
 *
 * The generated DSP output size is 78:
 *
 *     13 features per axis x 6 axes = 78
 *
 * Per axis, EI v4 produces:
 *
 *     [0] RMS after mean removal
 *     [1] time-domain skewness
 *     [2] time-domain Fisher kurtosis
 *     [3] Welch-spectrum skewness over bins 0..8
 *     [4] Welch-spectrum Fisher kurtosis over bins 0..8
 *     [5..12] log10(max-hold power spectrum bins 1..8)
 *
 * The implementation below mirrors the source flow in
 * dsp/spectral/feature.hpp and dsp/numpy.hpp, but uses a tiny
 * fixed 16-point direct DFT rather than the globally configured
 * EI FFT backend.
 * ============================================================ */

static constexpr size_t IMU_LOCAL_AXES = 6U;
static constexpr size_t IMU_LOCAL_FFT_LENGTH = 16U;
static constexpr size_t IMU_LOCAL_FFT_BINS = (IMU_LOCAL_FFT_LENGTH / 2U) + 1U;
static constexpr size_t IMU_LOCAL_FEATURES_PER_AXIS = 13U;
static constexpr size_t IMU_LOCAL_FEATURE_COUNT =
    IMU_LOCAL_AXES * IMU_LOCAL_FEATURES_PER_AXIS;

/*
 * ACCURATE currently uses 1200 floats = 200 frames x 6 axes.
 * Keep a little headroom without putting the work buffer on the
 * FreeRTOS thread stack.
 */
static constexpr size_t IMU_LOCAL_MAX_FRAMES = 256U;

static float g_imu_axis_work[IMU_LOCAL_MAX_FRAMES];
static float g_imu_feature_buffer[IMU_LOCAL_FEATURE_COUNT];

static constexpr float IMU_LOCAL_PI =
    3.14159265358979323846f;

/*
 * Population skewness, matching numpy::skew() software path.
 */
static float imu_local_skew(
    const float *data,
    size_t count)
{
    if ((data == nullptr) || (count == 0U))
    {
        return 0.0f;
    }

    float mean = 0.0f;

    for (size_t i = 0U; i < count; i++)
    {
        mean += data[i];
    }

    mean /= (float)count;

    float m2 = 0.0f;
    float m3 = 0.0f;

    for (size_t i = 0U; i < count; i++)
    {
        float d = data[i] - mean;
        float d2 = d * d;

        m2 += d2;
        m3 += d2 * d;
    }

    m2 /= (float)count;
    m3 /= (float)count;

    float denom =
        sqrtf(m2 * m2 * m2);

    if (denom == 0.0f)
    {
        return 0.0f;
    }

    return m3 / denom;
}

/*
 * Fisher kurtosis, matching numpy::kurtosis() software path.
 */
static float imu_local_kurtosis(
    const float *data,
    size_t count)
{
    if ((data == nullptr) || (count == 0U))
    {
        return -3.0f;
    }

    float mean = 0.0f;

    for (size_t i = 0U; i < count; i++)
    {
        mean += data[i];
    }

    mean /= (float)count;

    float variance = 0.0f;
    float m4 = 0.0f;

    for (size_t i = 0U; i < count; i++)
    {
        float d = data[i] - mean;
        float d2 = d * d;

        variance += d2;
        m4 += d2 * d2;
    }

    variance /= (float)count;
    m4 /= (float)count;

    variance *= variance;

    if (variance == 0.0f)
    {
        return -3.0f;
    }

    return (m4 / variance) - 3.0f;
}

/*
 * Compute the EI power spectrum for one frame.
 *
 * EI does:
 *
 *     magnitude = sqrt(real^2 + imag^2)
 *     power     = magnitude^2 / fft_points
 *
 * Therefore power is exactly:
 *
 *     (real^2 + imag^2) / 16
 *
 * Missing input points are zero padded.
 */
static void imu_local_power_spectrum_16(
    const float *frame,
    size_t frame_size,
    float *power_out)
{
    for (size_t k = 0U; k < IMU_LOCAL_FFT_BINS; k++)
    {
        float real_sum = 0.0f;
        float imag_sum = 0.0f;

        for (size_t n = 0U; n < IMU_LOCAL_FFT_LENGTH; n++)
        {
            float x =
                (n < frame_size) ? frame[n] : 0.0f;

            float angle =
                -2.0f *
                IMU_LOCAL_PI *
                (float)k *
                (float)n /
                (float)IMU_LOCAL_FFT_LENGTH;

            real_sum += x * cosf(angle);
            imag_sum += x * sinf(angle);
        }

        power_out[k] =
            ((real_sum * real_sum) +
             (imag_sum * imag_sum)) /
            (float)IMU_LOCAL_FFT_LENGTH;
    }
}

/*
 * EI-compatible Welch max hold.
 *
 * With overlap enabled, the next frame starts fft_length/2 later.
 * With a 16-point FFT the hop is therefore 8 samples.
 */
static void imu_local_welch_max_hold_16(
    const float *input,
    size_t input_size,
    bool do_overlap,
    float *output)
{
    for (size_t k = 0U; k < IMU_LOCAL_FFT_BINS; k++)
    {
        output[k] = 0.0f;
    }

    size_t input_ix = 0U;
    const size_t hop =
        do_overlap ?
        (IMU_LOCAL_FFT_LENGTH / 2U) :
        IMU_LOCAL_FFT_LENGTH;

    while (input_ix < input_size)
    {
        size_t remaining =
            input_size - input_ix;

        size_t frame_size =
            (remaining < IMU_LOCAL_FFT_LENGTH) ?
            remaining :
            IMU_LOCAL_FFT_LENGTH;

        float frame_power[IMU_LOCAL_FFT_BINS];

        imu_local_power_spectrum_16(
            input + input_ix,
            frame_size,
            frame_power);

        for (size_t k = 0U; k < IMU_LOCAL_FFT_BINS; k++)
        {
            if (frame_power[k] > output[k])
            {
                output[k] = frame_power[k];
            }
        }

        input_ix += hop;
    }
}

/*
 * Validate that the generated model still uses the DSP layout this
 * local implementation mirrors.
 *
 * Return:
 *   0  -> supported
 *  -1  -> null config
 *  -2  -> wrong implementation version
 *  -3  -> wrong axes
 *  -4  -> wrong decimation
 *  -5  -> filter is not "none"
 *  -6  -> analysis type is not FFT
 *  -7  -> FFT length is not 16
 *  -8  -> extra-low-frequency path enabled
 */
static int imu_local_validate_config(
    const ei_dsp_config_spectral_analysis_t *config)
{
    if (config == nullptr)
    {
        return -1;
    }

    if (config->implementation_version != 4)
    {
        return -2;
    }

    if (config->axes != (int)IMU_LOCAL_AXES)
    {
        return -3;
    }

    if (config->input_decimation_ratio != 1)
    {
        return -4;
    }

    if ((config->filter_type == nullptr) ||
        (strcmp(config->filter_type, "none") != 0))
    {
        return -5;
    }

    if ((config->analysis_type == nullptr) ||
        (strcmp(config->analysis_type, "FFT") != 0))
    {
        return -6;
    }

    if (config->fft_length != (int)IMU_LOCAL_FFT_LENGTH)
    {
        return -7;
    }

    if (config->extra_low_freq)
    {
        return -8;
    }

    return 0;
}

/*
 * Extract exactly the 78 features expected by the generated ICM
 * neural network.
 */
static int imu_local_extract_features(
    const float *sample_window,
    size_t float_count,
    const ei_dsp_config_spectral_analysis_t *config,
    float *features_out,
    size_t features_capacity)
{
    if ((sample_window == nullptr) ||
        (config == nullptr) ||
        (features_out == nullptr))
    {
        return -20;
    }

    if (features_capacity < IMU_LOCAL_FEATURE_COUNT)
    {
        return -21;
    }

    if ((float_count == 0U) ||
        ((float_count % IMU_LOCAL_AXES) != 0U))
    {
        return -22;
    }

    size_t frame_count =
        float_count / IMU_LOCAL_AXES;

    if ((frame_count == 0U) ||
        (frame_count > IMU_LOCAL_MAX_FRAMES))
    {
        return -23;
    }

    int cfg_res =
        imu_local_validate_config(config);

    if (cfg_res != 0)
    {
        return cfg_res;
    }

    size_t feature_ix = 0U;

    for (size_t axis = 0U; axis < IMU_LOCAL_AXES; axis++)
    {
        /*
         * Equivalent to:
         *
         *   transpose_in_place()
         *   scale()
         *   subtract_mean()
         *
         * for one axis at a time.
         */
        float mean = 0.0f;

        for (size_t frame = 0U; frame < frame_count; frame++)
        {
            float v =
                sample_window[
                    (frame * IMU_LOCAL_AXES) + axis];

            v *= config->scale_axes;

            g_imu_axis_work[frame] = v;
            mean += v;
        }

        mean /= (float)frame_count;

        float square_sum = 0.0f;

        for (size_t frame = 0U; frame < frame_count; frame++)
        {
            g_imu_axis_work[frame] -= mean;

            float v =
                g_imu_axis_work[frame];

            square_sum += v * v;
        }

        /*
         * numpy::rms() after mean removal.
         */
        float rms =
            sqrtf(
                square_sum /
                (float)frame_count);

        features_out[feature_ix++] =
            rms;

        /*
         * feature.hpp's time-domain v4 shortcut uses the
         * already-mean-centered signal and RMS as stddev.
         */
        float stddev = rms;

        if (stddev == 0.0f)
        {
            stddev = 1e-10f;
        }

        float s_sum = 0.0f;
        float k_sum = 0.0f;

        for (size_t frame = 0U; frame < frame_count; frame++)
        {
            float v =
                g_imu_axis_work[frame];

            float v3 =
                v * v * v;

            s_sum += v3;
            k_sum += v3 * v;
        }

        float stddev3 =
            stddev * stddev * stddev;

        features_out[feature_ix++] =
            (s_sum / (float)frame_count) /
            stddev3;

        features_out[feature_ix++] =
            ((k_sum / (float)frame_count) /
             (stddev3 * stddev)) -
            3.0f;

        /*
         * EI v4 obtains the complete 9-bin max-hold power
         * spectrum first so it can calculate spectrum skewness
         * and kurtosis over bins 0..8.
         */
        float spectrum[IMU_LOCAL_FFT_BINS];

        imu_local_welch_max_hold_16(
            g_imu_axis_work,
            frame_count,
            config->do_fft_overlap,
            spectrum);

        features_out[feature_ix++] =
            imu_local_skew(
                spectrum,
                IMU_LOCAL_FFT_BINS);

        features_out[feature_ix++] =
            imu_local_kurtosis(
                spectrum,
                IMU_LOCAL_FFT_BINS);

        /*
         * No filter -> EI sets:
         *
         *     start_bin = 1
         *     stop_bin  = fft_length/2 + 1 = 9
         *
         * Therefore copy bins 1..8.
         *
         * do_log=true in the generated ICM models.  EI replaces
         * exact zeros by 1e-10 before log10().
         */
        for (size_t bin = 1U; bin < IMU_LOCAL_FFT_BINS; bin++)
        {
            float value =
                spectrum[bin];

            if (config->do_log)
            {
                if (value == 0.0f)
                {
                    value = 1e-10f;
                }

                value =
                    log10f(value);
            }

            features_out[feature_ix++] =
                value;
        }
    }

    if (feature_ix != IMU_LOCAL_FEATURE_COUNT)
    {
        return -24;
    }

    return 0;
}


/* ============================================================
 * Direct generated EON graph execution for ICM
 * ============================================================
 *
 * This bypasses the generic EI run_nn_inference() wrapper.
 *
 * The generated model callbacks are taken from the selected
 * ei_learning_block_config_tflite_graph_t, so the same helper works
 * for FAST, BALANCED and ACCURATE.
 *
 * The generated graph owns an int8 input tensor and an int8 output
 * tensor. Quantization parameters are read from the actual tensors;
 * no scale or zero-point is hard-coded here.
 * ============================================================ */

static EI_IMPULSE_ERROR imu_run_graph_direct(
    ei_learning_block_config_tflite_graph_t *learning_cfg,
    const float *features,
    size_t feature_count,
    ei_impulse_result_t *result)
{
    if ((learning_cfg == nullptr) ||
        (features == nullptr) ||
        (result == nullptr))
    {
        return EI_IMPULSE_TFLITE_ERROR;
    }

    if (feature_count != IMU_LOCAL_FEATURE_COUNT)
    {
        return EI_IMPULSE_INVALID_SIZE;
    }

    ei_config_tflite_eon_graph_t *graph =
        static_cast<ei_config_tflite_eon_graph_t *>(
            learning_cfg->graph_config);

    if ((graph == nullptr) ||
        (graph->model_init == nullptr) ||
        (graph->model_input == nullptr) ||
        (graph->model_invoke == nullptr) ||
        (graph->model_output == nullptr) ||
        (graph->model_reset == nullptr))
    {
        return EI_IMPULSE_TFLITE_ERROR;
    }

    TfLiteTensor input_tensor;
    TfLiteTensor output_tensor;

    memset(&input_tensor, 0, sizeof(input_tensor));
    memset(&output_tensor, 0, sizeof(output_tensor));

    TfLiteStatus status =
        graph->model_init(
            ei_aligned_calloc);

    if (status != kTfLiteOk)
    {
        return EI_IMPULSE_TFLITE_ERROR;
    }

    status =
        graph->model_input(
            0,
            &input_tensor);

    if (status != kTfLiteOk)
    {
        (void)graph->model_reset(ei_aligned_free);
        return EI_IMPULSE_TFLITE_ERROR;
    }

    if ((input_tensor.type != kTfLiteInt8) ||
        (input_tensor.data.int8 == nullptr) ||
        (input_tensor.bytes != feature_count))
    {
        (void)graph->model_reset(ei_aligned_free);
        return EI_IMPULSE_INVALID_SIZE;
    }

    const float input_scale =
        input_tensor.params.scale;

    const int32_t input_zero_point =
        input_tensor.params.zero_point;

    if (!(input_scale > 0.0f))
    {
        (void)graph->model_reset(ei_aligned_free);
        return EI_IMPULSE_TFLITE_ERROR;
    }

    for (size_t i = 0U; i < feature_count; i++)
    {
        float qf =
            (features[i] / input_scale) +
            (float)input_zero_point;

        int32_t q =
            (int32_t)lrintf(qf);

        if (q > 127)
        {
            q = 127;
        }
        else if (q < -128)
        {
            q = -128;
        }

        input_tensor.data.int8[i] =
            (int8_t)q;
    }

    status =
        graph->model_invoke();

    if (status != kTfLiteOk)
    {
        (void)graph->model_reset(ei_aligned_free);
        return EI_IMPULSE_TFLITE_ERROR;
    }

    status =
        graph->model_output(
            0,
            &output_tensor);

    if (status != kTfLiteOk)
    {
        (void)graph->model_reset(ei_aligned_free);
        return EI_IMPULSE_TFLITE_ERROR;
    }

    if ((output_tensor.type != kTfLiteInt8) ||
        (output_tensor.data.int8 == nullptr) ||
        (output_tensor.bytes < 3U))
    {
        (void)graph->model_reset(ei_aligned_free);
        return EI_IMPULSE_INVALID_SIZE;
    }

    const float output_scale =
        output_tensor.params.scale;

    const int32_t output_zero_point =
        output_tensor.params.zero_point;

    if (!(output_scale > 0.0f))
    {
        (void)graph->model_reset(ei_aligned_free);
        return EI_IMPULSE_TFLITE_ERROR;
    }

    for (size_t i = 0U; i < 3U; i++)
    {
        const int32_t q =
            (int32_t)output_tensor.data.int8[i];

        result->classification[i].value =
            ((float)(q - output_zero_point)) *
            output_scale;
    }

    status =
        graph->model_reset(
            ei_aligned_free);

    if (status != kTfLiteOk)
    {
        return EI_IMPULSE_TFLITE_ERROR;
    }

    return EI_IMPULSE_OK;
}


/* ============================================================
 * Required IMU input size
 * ============================================================
 *
 * Returns number of FLOATS, not number of time-domain frames.
 * ============================================================ */

size_t imu_run_inference_get_required_floats(
    imu_model_variant_t variant)
{
    switch (variant)
    {
        case IMU_VARIANT_FAST:
            return
                impulse_icm_fast.raw_sample_count *
                impulse_icm_fast.raw_samples_per_frame;

        case IMU_VARIANT_BALANCED:
            return
                impulse_icm_balanced.raw_sample_count *
                impulse_icm_balanced.raw_samples_per_frame;

        case IMU_VARIANT_ACCURATE:
            return
                impulse_icm_accurate.raw_sample_count *
                impulse_icm_accurate.raw_samples_per_frame;

        default:
            return 0U;
    }
}

/* ============================================================
 * ICM-20948 inference dispatcher
 * ============================================================ */

int aria_run_inference_imu(
    imu_model_variant_t variant,
    const float *sample_window,
    imu_inference_result_t *out_result,
    bool debug)
{
    (void)debug;

    if ((sample_window == nullptr) ||
        (out_result == nullptr))
    {
        return -1;
    }

    out_result->error_code = -1;
    out_result->p_fault = 0.0f;
    out_result->p_idle = 0.0f;
    out_result->p_normal = 0.0f;

    size_t float_count =
        imu_run_inference_get_required_floats(
            variant);

    if (float_count == 0U)
    {
        return -1;
    }

    ei_impulse_handle_t *handle = nullptr;

    ei_dsp_config_spectral_analysis_t *dsp_config =
        nullptr;

    const uint32_t *learning_inputs =
        nullptr;

    uint32_t learning_inputs_size =
        0U;

    void *learning_config =
        nullptr;

    switch (variant)
    {
        case IMU_VARIANT_FAST:
            handle =
                &impulse_handle_icm_fast;

            dsp_config =
                &ei_dsp_config_icm_fast;

            learning_inputs =
                ei_learning_block_icm_fast_inputs;

            learning_inputs_size =
                ei_learning_block_icm_fast_inputs_size;

            learning_config =
                (void*)&ei_learning_block_config_icm_fast;
            break;

        case IMU_VARIANT_BALANCED:
            handle =
                &impulse_handle_icm_balanced;

            dsp_config =
                &ei_dsp_config_icm_balanced;

            learning_inputs =
                ei_learning_block_icm_balanced_inputs;

            learning_inputs_size =
                ei_learning_block_icm_balanced_inputs_size;

            learning_config =
                (void*)&ei_learning_block_config_icm_balanced;
            break;

        case IMU_VARIANT_ACCURATE:
            handle =
                &impulse_handle_icm_accurate;

            dsp_config =
                &ei_dsp_config_icm_accurate;

            learning_inputs =
                ei_learning_block_icm_accurate_inputs;

            learning_inputs_size =
                ei_learning_block_icm_accurate_inputs_size;

            learning_config =
                (void*)&ei_learning_block_config_icm_accurate;
            break;

        default:
            return -1;
    }

    if ((handle == nullptr) ||
        (handle->impulse == nullptr) ||
        (dsp_config == nullptr))
    {
        return -1;
    }

    if ((learning_inputs == nullptr) ||
        (learning_inputs_size == 0U) ||
        (learning_config == nullptr) ||
        (handle->impulse->learning_blocks_size == 0U))
    {
        return -1;
    }

    int cfg_res =
        imu_local_validate_config(
            dsp_config);

    if (cfg_res != 0)
    {
        out_result->error_code =
            (int)EI_IMPULSE_DSP_ERROR;

        return (int)EI_IMPULSE_DSP_ERROR;
    }

    if (handle->impulse->nn_input_frame_size !=
        IMU_LOCAL_FEATURE_COUNT)
    {
        out_result->error_code =
            (int)EI_IMPULSE_DSP_ERROR;

        return (int)EI_IMPULSE_DSP_ERROR;
    }

    int dsp_res =
        imu_local_extract_features(
            sample_window,
            float_count,
            dsp_config,
            g_imu_feature_buffer,
            IMU_LOCAL_FEATURE_COUNT);

    if (dsp_res != 0)
    {
        out_result->error_code =
            (int)EI_IMPULSE_DSP_ERROR;

        return (int)EI_IMPULSE_DSP_ERROR;
    }

    ei_impulse_result_t result;

    memset(
        &result,
        0,
        sizeof(result));

    ei_learning_block_config_tflite_graph_t *icm_learning_cfg =
        static_cast<ei_learning_block_config_tflite_graph_t *>(
            learning_config);

    EI_IMPULSE_ERROR nn_err =
        imu_run_graph_direct(
            icm_learning_cfg,
            g_imu_feature_buffer,
            IMU_LOCAL_FEATURE_COUNT,
            &result);

    if (nn_err != EI_IMPULSE_OK)
    {
        out_result->error_code =
            (int)nn_err;

        return (int)nn_err;
    }

    out_result->p_fault =
        result.classification[0].value;

    out_result->p_idle =
        result.classification[1].value;

    out_result->p_normal =
        result.classification[2].value;

    out_result->error_code =
        (int)EI_IMPULSE_OK;

    return (int)EI_IMPULSE_OK;
}
