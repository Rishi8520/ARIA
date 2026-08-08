#ifndef RUN_INFERENCE_DISPATCHER_H
#define RUN_INFERENCE_DISPATCHER_H

/* Pure C header -- must NOT include any Edge Impulse SDK headers here.
 * Those headers contain C++-only constructs (class, std::, <cfloat>) that
 * a plain C translation unit (like adc_thread_entry.c) cannot parse, even
 * with extern "C" wrapping -- extern "C" only affects linkage/name
 * mangling, not C++ syntax legality. Keep all EI SDK includes confined to
 * run_inference_dispatcher.cpp. */

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VARIANT_FAST,
    VARIANT_BALANCED,
    VARIANT_ACCURATE
} model_variant_t;

/* Plain-C result struct -- deliberately does not expose ei_impulse_result_t. */
typedef struct {
    int   error_code;   /* 0 == EI_IMPULSE_OK; nonzero == EI_IMPULSE_ERROR value */
    float p_anomaly;    /* classification[0].value, label "anamoly" */
    float p_normal;     /* classification[1].value, label "normal"  */
} inference_result_t;

/* Returns the raw_sample_count required by a given variant (100 / 200 / 300). */
size_t run_inference_get_required_samples(model_variant_t variant);

/* Named aria_run_inference (NOT run_inference) deliberately -- the Edge
 * Impulse SDK's own edge-impulse-sdk/classifier/ei_run_classifier.h already
 * declares an internal function literally named run_inference with
 * extern "C" linkage. Since extern "C" strips C++ name mangling, using the
 * same name here would collide with the SDK's own symbol at link time
 * ("already defined"), even though the two functions are unrelated and take
 * different arguments. Keep this name distinct from anything in the SDK.
 *
 * sample_window must point to exactly run_inference_get_required_samples(variant)
 * contiguous float values, oldest-first, in the same physical units (volts)
 * the model was trained on. Returns 0 on success (EI_IMPULSE_OK), nonzero on
 * error; out_result is always populated (zeroed on failure). */
int aria_run_inference(model_variant_t variant,
                        const float *sample_window,
                        inference_result_t *out_result,
                        bool debug);

#ifdef __cplusplus
}
#endif

#endif // RUN_INFERENCE_DISPATCHER_H
