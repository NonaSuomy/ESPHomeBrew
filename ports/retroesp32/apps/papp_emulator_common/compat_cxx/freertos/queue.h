#pragma once

/* Legacy wrappers compile against queues, but PAPP execution is
 * single-threaded. These no-op declarations are enough for ports that
 * submit their frames inline. */
typedef void *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{ (void)length; (void)item_size; return (QueueHandle_t)0; }
static inline int xQueuePeek(QueueHandle_t queue, void *item, unsigned ticks)
{ (void)queue; (void)item; (void)ticks; return 0; }
static inline int xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks)
{ (void)queue; (void)item; (void)ticks; return 0; }
static inline int xQueueSend(QueueHandle_t queue, const void *item, unsigned ticks)
{ (void)queue; (void)item; (void)ticks; return 0; }
static inline int xQueueOverwrite(QueueHandle_t queue, const void *item)
{ (void)queue; (void)item; return 0; }
static inline void vQueueDelete(QueueHandle_t queue) { (void)queue; }
