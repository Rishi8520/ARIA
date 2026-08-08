/* Edge Impulse porting layer for Renesas RA (FreeRTOS) targets.
 * Implements the ei_* primitives the SDK expects the platform to provide.
 *
 * IMPORTANT: These functions must NOT be wrapped in extern "C". The
 * declarations in edge-impulse-sdk/porting/ei_classifier_porting.h are
 * plain C++ declarations (no extern "C" guard around them), so every call
 * site in the SDK (tflite_model_compiled_*.cpp, run_inference_dispatcher.cpp,
 * dsp/*.hpp, etc.) resolves to the C++-mangled symbol, e.g.
 * _Z9ei_printfPKcz for ei_printf(const char*, ...). If this file defines
 * them as extern "C", it emits the plain unmangled symbol instead, and the
 * linker reports "undefined reference to ei_printf(char const*, ...)" even
 * though the object file with the definition is present and linked in.
 */
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_data.h"

/* ---- Memory ---- */
void *ei_malloc(size_t size) {
    return pvPortMalloc(size);
}

void *ei_calloc(size_t nitems, size_t size) {
    size_t total = nitems * size;
    void *ptr = pvPortMalloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void ei_free(void *ptr) {
    if (ptr) {
        vPortFree(ptr);
    }
}

/* ---- Logging ----
 * No-op for now: no UART/console retarget (_write) exists in this project yet.
 * Pulling in real printf/vprintf drags in newlib syscall stubs (_exit, _getpid,
 * _kill, _write) this bare-metal FreeRTOS build doesn't provide.
 * Wire this up to a real UART transmit function later if needed.
 */
void ei_printf(const char *format, ...) {
    (void)format;
}

void ei_printf_float(float f) {
    (void)f;
}

/* ---- Timing ----
 * Uses FreeRTOS tick count converted to microseconds.
 */
uint64_t ei_read_timer_us(void) {
    TickType_t ticks = xTaskGetTickCount();
    return (uint64_t)ticks * (1000000ULL / configTICK_RATE_HZ);
}

uint64_t ei_read_timer_ms(void) {
    TickType_t ticks = xTaskGetTickCount();
    return (uint64_t)ticks * (1000ULL / configTICK_RATE_HZ);
}

/* ---- Cancellation ---- */
bool ei_run_impulse_check_canceled(void) {
    return false;
}

/* ---- Sleep ---- */
void ei_sleep(int32_t time_ms) {
    vTaskDelay(pdMS_TO_TICKS(time_ms));
}
