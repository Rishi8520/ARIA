#ifndef TELEMETRY_H_
#define TELEMETRY_H_

#include <stdint.h>
#include "run_inference_dispatcher.h"   /* for model_variant_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, from adc_thread_entry() setup, AFTER g_ioport/HAL init.
 * Opens the SCI UART instance and creates the TX-done semaphore. */
void telemetry_init(void);

/* Call every time a fresh inference result is available.
 * variant       - which model just ran (VARIANT_FAST / BALANCED / ACCURATE)
 * confidence    - result.p_anomaly (0.0 - 1.0)
 * latency_us    - measured latency of the aria_run_inference() call, in microseconds
 * voltage
 * Sends one newline-terminated JSON line to the ESP32 over UART, matching:
 *   {"mode":"motion","model":"fast","confidence":0.93,"latency_ms":7}
 */
void telemetry_send(model_variant_t variant, float confidence, uint32_t latency_us, float voltage);

/* Returns true exactly once if the ESP32 sent a "TOGGLE" line since the
 * last call (clears the flag on read). Poll this once per main loop iteration. */
bool telemetry_toggle_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_H_ */
