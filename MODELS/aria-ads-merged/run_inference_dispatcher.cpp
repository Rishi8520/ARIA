#include "run_inference_dispatcher.h"
#include "model-parameters/model_variables.h"
#include <cstring>

// Definition of the shared buffer declared extern in the header.
float g_aria_adc_signal_buffer[ARIA_ADC_BUFFER_MAX_SAMPLES];

void aria_set_adc_signal(const float *samples, size_t count) {
    if (count > ARIA_ADC_BUFFER_MAX_SAMPLES) {
        count = ARIA_ADC_BUFFER_MAX_SAMPLES;
    }
    memcpy(g_aria_adc_signal_buffer, samples, count * sizeof(float));
}

static int get_signal_data(size_t offset, size_t length, float *out_ptr) {
    memcpy(out_ptr, g_aria_adc_signal_buffer + offset, length * sizeof(float));
    return 0;
}

EI_IMPULSE_ERROR run_inference(model_variant_t variant, ei_impulse_result_t *result, bool debug) {
    ei_impulse_handle_t *handle;
    size_t sample_count;

    switch (variant) {
        case VARIANT_FAST:
            handle = &impulse_handle_fast;
            sample_count = impulse_fast.raw_sample_count;
            break;
        case VARIANT_BALANCED:
            handle = &impulse_handle_balanced;
            sample_count = impulse_balanced.raw_sample_count;
            break;
        case VARIANT_ACCURATE:
            handle = &impulse_handle_accurate;
            sample_count = impulse_accurate.raw_sample_count;
            break;
        default:
            return EI_IMPULSE_UNSUPPORTED_INFERENCING_ENGINE;
    }

    signal_t signal;
    signal.total_length = sample_count;
    signal.get_data = &get_signal_data;

    return run_classifier(handle, &signal, result, debug);
}