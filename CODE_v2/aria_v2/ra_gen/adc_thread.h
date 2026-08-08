/* generated thread header file - do not edit */
#ifndef ADC_THREAD_H_
#define ADC_THREAD_H_
#include "bsp_api.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "hal_data.h"
#ifdef __cplusplus
                extern "C" void adc_thread_entry(void * pvParameters);
                #else
extern void adc_thread_entry(void *pvParameters);
#endif
FSP_HEADER
FSP_FOOTER
#endif /* ADC_THREAD_H_ */
