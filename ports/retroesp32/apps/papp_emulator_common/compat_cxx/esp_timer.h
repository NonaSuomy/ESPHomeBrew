#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern int64_t papp_esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
static inline int64_t esp_timer_get_time(void) { return papp_esp_timer_get_time(); }
