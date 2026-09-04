// app.h — the portable core entry point.
//
// The platform main loop (sim/main.c, or the ESP32 display task) owns timing and
// I/O; it drives these four calls. Everything they touch is platform-agnostic.
#pragma once

#include "canvas.h"
#include "input.h"

// One-time core init: scene system, data, saved state. Call after qn_hal_init().
void app_init(void);

// Advance time-based state (animations, audio playhead, auto-scroll) by dt_ms.
void app_tick(uint32_t dt_ms);

// Deliver one input event to the active scene / global handlers.
void app_input(InputEvent ev);

// Render the current frame into the canvas (already cleared by the caller).
void app_render(Canvas *c);
