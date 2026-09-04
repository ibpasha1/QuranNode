// plat.h — QuranNode portability shim.
//
// The core/ tree is platform-agnostic C99. Anything that would otherwise reach
// for an ESP-IDF header (logging, a millisecond clock, memory) goes through this
// shim so the exact same source compiles for the SDL desktop simulator and, later,
// the ESP32-S3 firmware. Platform I/O proper lives behind hal.h; this is just the
// small set of primitives the core itself needs inline.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// --- Logging -------------------------------------------------------------
// Lightweight printf logging. On the ESP32 build these could be remapped to
// ESP_LOGx; for now stderr is fine on both targets.
#ifndef QN_LOG_LEVEL
#define QN_LOG_LEVEL 2  // 0=silent 1=err 2=info
#endif

#define QN_LOGE(tag, ...) do { if (QN_LOG_LEVEL >= 1) { fprintf(stderr, "[E %s] ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define QN_LOGI(tag, ...) do { if (QN_LOG_LEVEL >= 2) { fprintf(stderr, "[I %s] ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// --- Monotonic clock -----------------------------------------------------
// Milliseconds since app start. Implemented by the active HAL (sim: SDL_GetTicks;
// esp32: esp_timer). Declared here so core modules can read time without pulling
// in a platform header.
uint32_t plat_millis(void);
