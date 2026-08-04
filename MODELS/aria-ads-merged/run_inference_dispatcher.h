#ifndef RUN_INFERENCE_DISPATCHER_H
#define RUN_INFERENCE_DISPATCHER_H

#include <cstddef>
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

typedef enum {
    VARIANT_FAST,
    VARIANT_BALANCED,
    VARIANT_ACCURATE
} model_variant_t;

// Max raw_sample_count across all variants (accurate = 300).
#define ARIA_ADC_BUFFER_MAX_SAMPLES 300

// Shared ADC signal buffer, defined in run_inference_dispatcher.cpp.
// Callers (adc_thread_entry.c / test_main.cpp) write raw samples here
// before calling run_inference().
extern float g_aria_adc_signal_buffer[ARIA_ADC_BUFFER_MAX_SAMPLES];

// Convenience helper: copies `count` floats into the shared buffer.
void aria_set_adc_signal(const float *samples, size_t count);

EI_IMPULSE_ERROR run_inference(model_variant_t variant, ei_impulse_result_t *result, bool debug);

#endif // RUN_INFERENCE_DISPATCHER_H