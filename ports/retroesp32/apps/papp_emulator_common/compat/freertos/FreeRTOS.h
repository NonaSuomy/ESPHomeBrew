#pragma once

/* Tiny FreeRTOS compatibility layer for single-threaded PAPP builds. */
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xFFFFFFFFu

typedef unsigned int TickType_t;
typedef unsigned int UBaseType_t;
typedef unsigned int BaseType_t;
typedef unsigned int StackType_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
