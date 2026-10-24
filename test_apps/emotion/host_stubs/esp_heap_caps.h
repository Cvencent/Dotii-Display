#pragma once
/* Host-build shim for ESP-IDF heap_caps. The dotii renderer only needs
   aligned heap allocation; on the host a plain malloc/free is equivalent. */
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
static inline void heap_caps_free(void *p) { free(p); }
