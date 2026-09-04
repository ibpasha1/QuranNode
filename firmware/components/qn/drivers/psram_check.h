#pragma once

#include "esp_heap_caps.h"
#include <stdbool.h>
#include <stddef.h>

// Returns true if PSRAM is available at runtime
static inline bool dspmini_has_psram(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

// Allocates from PSRAM if available, otherwise falls back to default heap.
// Returns NULL on failure (same as malloc).
static inline void *dspmini_alloc_prefer_psram(size_t size, uint32_t extra_caps)
{
    if (dspmini_has_psram()) {
        void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | extra_caps);
        if (p) return p;
    }
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT | extra_caps);
}

// Calloc variant: allocates zeroed memory, preferring PSRAM
static inline void *dspmini_calloc_prefer_psram(size_t count, size_t size, uint32_t extra_caps)
{
    if (dspmini_has_psram()) {
        void *p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | extra_caps);
        if (p) return p;
    }
    return heap_caps_calloc(count, size, MALLOC_CAP_DEFAULT | extra_caps);
}
