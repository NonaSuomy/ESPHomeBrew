#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1 << 10)
/* Use the tracked PAPP allocator, including on fatal-error cleanup. */
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
