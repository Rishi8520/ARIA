#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "hal_data.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "telemetry.h"

/* ------------------------------------------------------------------
 * RA8D1 <-> ESP32 UART telemetry
 *
 * Expected ESP32 packet:
 * {"mode":"motion","model":"fast","confidence":0.930,"latency_ms":7}\n
 *
 * FSP UART instance:
 *   g_uart0_ctrl / g_uart0_cfg
 * FSP callback:
 *   g_uart0_callback
 * ------------------------------------------------------------------ */

static SemaphoreHandle_t g_uart_tx_done_sem;
static StaticSemaphore_t g_uart_tx_sem_buffer;

static char g_uart_tx_buf[160];

static char g_uart_rx_line[64];
static volatile uint8_t g_uart_rx_idx = 0;
static volatile bool g_toggle_requested = false;

/* ------------------------------------------------------------------
 * Debug-visible globals.
 * Add all of these to e² studio Expressions.
 * ------------------------------------------------------------------ */

volatile fsp_err_t g_uart_open_err = FSP_SUCCESS;

volatile uint32_t g_telemetry_send_count = 0U;
volatile uint32_t g_telemetry_write_err_count = 0U;
volatile fsp_err_t g_telemetry_last_write_err = FSP_SUCCESS;
volatile uint32_t g_telemetry_last_len = 0U;

volatile uint32_t g_uart_callback_count = 0U;
volatile uint32_t g_uart_last_event = 0U;

volatile uint32_t g_uart_tx_start_count = 0U;
volatile uint32_t g_uart_tx_complete_count = 0U;
volatile uint32_t g_uart_tx_timeout_count = 0U;

volatile uint32_t g_uart_rx_char_count = 0U;
volatile uint32_t g_uart_toggle_count = 0U;

/* ------------------------------------------------------------------
 * This function name must be entered exactly as g_uart0_callback in:
 *
 * FSP Configuration -> UART stack -> Properties -> Callback
 * ------------------------------------------------------------------ */
void g_uart0_callback(uart_callback_args_t *p_args)
{
    BaseType_t higher_priority_woken = pdFALSE;

    g_uart_callback_count++;
    g_uart_last_event = (uint32_t) p_args->event;

    switch (p_args->event)
    {
        case UART_EVENT_TX_COMPLETE:
        {
            g_uart_tx_complete_count++;

            if (NULL != g_uart_tx_done_sem)
            {
                xSemaphoreGiveFromISR(g_uart_tx_done_sem,
                                      &higher_priority_woken);
            }

            break;
        }

        case UART_EVENT_RX_CHAR:
        {
            char c = (char) p_args->data;

            g_uart_rx_char_count++;

            if ((c == '\n') || (c == '\r'))
            {
                g_uart_rx_line[g_uart_rx_idx] = '\0';

                if (0 == strcmp(g_uart_rx_line, "TOGGLE"))
                {
                    g_toggle_requested = true;
                    g_uart_toggle_count++;
                }

                g_uart_rx_idx = 0U;
            }
            else if (g_uart_rx_idx < (sizeof(g_uart_rx_line) - 1U))
            {
                g_uart_rx_line[g_uart_rx_idx++] = c;
            }

            break;
        }

        default:
        {
            /*
             * This also records errors such as:
             * UART_EVENT_ERR_PARITY
             * UART_EVENT_ERR_FRAMING
             * UART_EVENT_ERR_OVERFLOW
             *
             * Inspect g_uart_last_event in Expressions if needed.
             */
            break;
        }
    }

    portYIELD_FROM_ISR(higher_priority_woken);
}

void telemetry_init(void)
{
    g_uart_tx_done_sem = xSemaphoreCreateBinaryStatic(&g_uart_tx_sem_buffer);

    if (NULL == g_uart_tx_done_sem)
    {
        /* No custom error code needed for now; UART must not be used. */
        g_uart_open_err = FSP_ERR_ASSERTION;
        return;
    }

    g_uart_open_err = R_SCI_B_UART_Open(&g_uart0_ctrl, &g_uart0_cfg);
}

static const char *variant_name(model_variant_t variant)
{
    switch (variant)
    {
        case VARIANT_FAST:
            return "fast";

        case VARIANT_BALANCED:
            return "balanced";

        case VARIANT_ACCURATE:
            return "accurate";

        default:
            return "unknown";
    }
}

void telemetry_send(model_variant_t variant, float confidence, uint32_t latency_us, float voltage)
{
    g_telemetry_send_count++;

    if (FSP_SUCCESS != g_uart_open_err)
    {
        g_telemetry_last_write_err = g_uart_open_err;
        g_telemetry_write_err_count++;
        return;
    }

    float bounded_confidence = confidence;
    if (bounded_confidence < 0.0f) bounded_confidence = 0.0f;
    else if (bounded_confidence > 1.0f) bounded_confidence = 1.0f;

    uint32_t confidence_milli = (uint32_t)((bounded_confidence * 1000.0f) + 0.5f);
    uint32_t confidence_whole = confidence_milli / 1000U;
    uint32_t confidence_fraction = confidence_milli % 1000U;

    /* Voltage can be negative (differential ADC reading) -- newlib-nano still
     * has no float printf support, so handle the sign manually same as we
     * do the whole/fraction split for confidence. */
    bool v_negative = (voltage < 0.0f);
    float v_abs = v_negative ? -voltage : voltage;
    uint32_t v_milli = (uint32_t)((v_abs * 1000.0f) + 0.5f);
    uint32_t v_whole = v_milli / 1000U;
    uint32_t v_frac = v_milli % 1000U;

    int len = snprintf(
        g_uart_tx_buf,
        sizeof(g_uart_tx_buf),
        "{\"mode\":\"motion\",\"model\":\"%s\",\"confidence\":%lu.%03lu,"
        "\"latency_ms\":%lu,\"voltage\":%s%lu.%03lu}\n",
        variant_name(variant),
        (unsigned long) confidence_whole,
        (unsigned long) confidence_fraction,
        (unsigned long) (latency_us / 1000U),
        v_negative ? "-" : "",
        (unsigned long) v_whole,
        (unsigned long) v_frac
    );

    if ((len <= 0) || ((size_t) len >= sizeof(g_uart_tx_buf)))
    {
        g_telemetry_last_write_err = FSP_ERR_ASSERTION;
        g_telemetry_write_err_count++;
        return;
    }

    g_telemetry_last_len = (uint32_t) len;

    (void) xSemaphoreTake(g_uart_tx_done_sem, 0);
    g_uart_tx_start_count++;

    fsp_err_t err = R_SCI_B_UART_Write(&g_uart0_ctrl, (uint8_t *) g_uart_tx_buf, (uint32_t) len);

    if (FSP_SUCCESS != err)
    {
        g_telemetry_last_write_err = err;
        g_telemetry_write_err_count++;
        return;
    }

    if (pdTRUE != xSemaphoreTake(g_uart_tx_done_sem, pdMS_TO_TICKS(50)))
    {
        g_uart_tx_timeout_count++;
    }
}

bool telemetry_toggle_requested(void)
{
    if (g_toggle_requested)
    {
        g_toggle_requested = false;
        return true;
    }

    return false;
}
