#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif
void papp_delay_ms(int ms);
#ifdef __cplusplus
}
#endif

/* Tasks are intentionally not created by PAPP ports.  These declarations
 * keep legacy SDL timing code buildable while mapping delays to the launcher. */
static inline void vTaskDelay(TickType_t ticks)
{
    papp_delay_ms((int)ticks * portTICK_PERIOD_MS);
}

static inline void vTaskDelete(void *task)
{
    (void)task;
}
