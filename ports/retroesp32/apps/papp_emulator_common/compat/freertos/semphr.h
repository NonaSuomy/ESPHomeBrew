#pragma once

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void)s; (void)t; return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    (void)s; return pdTRUE;
}
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }

