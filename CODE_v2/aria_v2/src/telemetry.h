#ifndef TELEMETRY_H_
#define TELEMETRY_H_

#include <stdint.h>
#include "run_inference_dispatcher.h"

#ifdef __cplusplus
extern "C" {
#endif

void telemetry_init(void);

/* ADS1263 telemetry. latency_us is the real measured inference time. */
void telemetry_send(
    model_variant_t variant,
    float confidence,
    uint32_t latency_us,
    float voltage);

/* ICM-20948 telemetry. latency_us is the real measured inference time. */
void telemetry_send_imu(
    model_variant_t variant,
    float confidence,
    uint32_t latency_us);

bool telemetry_toggle_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_H_ */
