/* generated vector source file - do not edit */
#include "bsp_api.h"
/* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
#if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_NUM_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = spi_b_rxi_isr, /* SPI1 RXI (Receive buffer full) */
            [1] = spi_b_txi_isr, /* SPI1 TXI (Transmit buffer empty) */
            [2] = spi_b_tei_isr, /* SPI1 TEI (Transmission complete event) */
            [3] = spi_b_eri_isr, /* SPI1 ERI (Error) */
            [4] = sci_b_uart_rxi_isr, /* SCI2 RXI (Receive data full) */
            [5] = sci_b_uart_txi_isr, /* SCI2 TXI (Transmit data empty) */
            [6] = sci_b_uart_tei_isr, /* SCI2 TEI (Transmit end) */
            [7] = sci_b_uart_eri_isr, /* SCI2 ERI (Receive error) */
            [8] = spi_b_rxi_isr, /* SPI0 RXI (Receive buffer full) */
            [9] = spi_b_txi_isr, /* SPI0 TXI (Transmit buffer empty) */
            [10] = spi_b_tei_isr, /* SPI0 TEI (Transmission complete event) */
            [11] = spi_b_eri_isr, /* SPI0 ERI (Error) */
        };
        #if BSP_FEATURE_ICU_HAS_IELSR
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_NUM_ENTRIES] =
        {
            [0] = BSP_PRV_VECT_ENUM(EVENT_SPI1_RXI,GROUP0), /* SPI1 RXI (Receive buffer full) */
            [1] = BSP_PRV_VECT_ENUM(EVENT_SPI1_TXI,GROUP1), /* SPI1 TXI (Transmit buffer empty) */
            [2] = BSP_PRV_VECT_ENUM(EVENT_SPI1_TEI,GROUP2), /* SPI1 TEI (Transmission complete event) */
            [3] = BSP_PRV_VECT_ENUM(EVENT_SPI1_ERI,GROUP3), /* SPI1 ERI (Error) */
            [4] = BSP_PRV_VECT_ENUM(EVENT_SCI2_RXI,GROUP4), /* SCI2 RXI (Receive data full) */
            [5] = BSP_PRV_VECT_ENUM(EVENT_SCI2_TXI,GROUP5), /* SCI2 TXI (Transmit data empty) */
            [6] = BSP_PRV_VECT_ENUM(EVENT_SCI2_TEI,GROUP6), /* SCI2 TEI (Transmit end) */
            [7] = BSP_PRV_VECT_ENUM(EVENT_SCI2_ERI,GROUP7), /* SCI2 ERI (Receive error) */
            [8] = BSP_PRV_VECT_ENUM(EVENT_SPI0_RXI,GROUP0), /* SPI0 RXI (Receive buffer full) */
            [9] = BSP_PRV_VECT_ENUM(EVENT_SPI0_TXI,GROUP1), /* SPI0 TXI (Transmit buffer empty) */
            [10] = BSP_PRV_VECT_ENUM(EVENT_SPI0_TEI,GROUP2), /* SPI0 TEI (Transmission complete event) */
            [11] = BSP_PRV_VECT_ENUM(EVENT_SPI0_ERI,GROUP3), /* SPI0 ERI (Error) */
        };
        #endif
        #endif
