#include "run_inference_dispatcher.h"

/* All Edge Impulse SDK / C++-only includes are confined to this .cpp file.
 * Nothing above this line is visible to plain C translation units. */
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/classifier/ei_run_dsp.h"
#include "edge-impulse-sdk/classifier/ei_data_normalization.h"
#include "model-parameters/model_variables.h"
#include <cstring>

static const float *g_active_window = nullptr;

static int get_signal_data(size_t offset, size_t length, float *out_ptr) {
    memcpy(out_ptr, g_active_window + offset, length * sizeof(float));
    return 0;
}

size_t run_inference_get_required_samples(model_variant_t variant) {
    switch (variant) {
        case VARIANT_FAST:     return impulse_fast.raw_sample_count;
        case VARIANT_BALANCED: return impulse_balanced.raw_sample_count;
        case VARIANT_ACCURATE: return impulse_accurate.raw_sample_count;
        default:                return 0;
    }
}

int aria_run_inference(model_variant_t variant,
                        const float *sample_window,
                        inference_result_t *out_result,
                        bool debug) {
    if (out_result == nullptr) {
        return -1;
    }
    out_result->error_code = -1;
    out_result->p_anomaly = 0.0f;
    out_result->p_normal = 0.0f;

    ei_impulse_handle_t *handle;
    size_t sample_count = run_inference_get_required_samples(variant);

    switch (variant) {
        case VARIANT_FAST:     handle = &impulse_handle_fast;     break;
        case VARIANT_BALANCED: handle = &impulse_handle_balanced; break;
        case VARIANT_ACCURATE: handle = &impulse_handle_accurate; break;
        default:
            return -1;
    }

    if (sample_window == nullptr || sample_count == 0) {
        return -1;
    }

    g_active_window = sample_window;

    signal_t signal;
    signal.total_length = sample_count;
    signal.get_data = &get_signal_data;

    ei_impulse_result_t result;
    memset(&result, 0, sizeof(result));

    EI_IMPULSE_ERROR err = run_classifier(handle, &signal, &result, debug);

    g_active_window = nullptr;

    out_result->error_code = (int)err;
    if (err == EI_IMPULSE_OK) {
        out_result->p_anomaly = result.classification[0].value;
        out_result->p_normal  = result.classification[1].value;
    }

    return (int)err;
}
