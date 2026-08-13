#ifndef RUN_INFERENCE_DISPATCHER_IMU_H
#define RUN_INFERENCE_DISPATCHER_IMU_H

/* Pure C header -- mirrors run_inference_dispatcher.h's isolation rule.
 * Must NOT include any Edge Impulse SDK headers here. */

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reuses the same 3-tier naming as the ADS1263 dispatcher, but this is a
 * DISTINCT enum type (not shared with model_variant_t) since the IMU demo
 * runs its own independent state machine, separate from the ADS1263 one. */
typedef enum {
    IMU_VARIANT_FAST,
    IMU_VARIANT_BALANCED,
    IMU_VARIANT_ACCURATE
} imu_model_variant_t;

/* 3-class result -- ICM models output fault/idle/normal, NOT the 2-class
 * anomaly/normal shape used by the ADS1263 inference_result_t. Do not
 * reuse that struct here. */
typedef struct {
    int error_code;     /* 0 == EI_IMPULSE_OK; nonzero == EI_IMPULSE_ERROR value */
    float p_fault;       /* classification[0].value, label "fault" */
    float p_idle;         /* classification[1].value, label "idle" */
    float p_normal;      /* classification[2].value, label "normal" */
} imu_inference_result_t;

/* Returns raw_sample_count * raw_samples_per_frame for a given variant --
 * i.e. the number of raw floats needed (100*6=600 / 200*6=1200 / 200*6=1200),
 * NOT just the frame count. Caller must allocate a window of this size. */
size_t imu_run_inference_get_required_floats(imu_model_variant_t variant);

/* sample_window must point to exactly imu_run_inference_get_required_floats(variant)
 * contiguous float values, frame-interleaved as
 * [accX,accY,accZ,gyrX,gyrY,gyrZ, accX,accY,accZ,gyrX,gyrY,gyrZ, ...]
 * to match the trained fusion_string "accX + accY + accZ + gyrX + gyrY + gyrZ".
 * Returns 0 on success (EI_IMPULSE_OK), nonzero on error; out_result is
 * always populated (zeroed on failure). */
int aria_run_inference_imu(imu_model_variant_t variant,
                            const float *sample_window,
                            imu_inference_result_t *out_result,
                            bool debug);

#ifdef __cplusplus
}
#endif

#endif // RUN_INFERENCE_DISPATCHER_IMU_H
