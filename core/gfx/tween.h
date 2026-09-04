#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MAX_TWEENS 32

typedef enum {
    EASE_LINEAR,
    EASE_IN_QUAD,
    EASE_OUT_QUAD,
    EASE_IN_OUT_QUAD,
    EASE_IN_CUBIC,
    EASE_OUT_CUBIC,
    EASE_IN_OUT_CUBIC,
    EASE_OUT_BOUNCE,
    EASE_OUT_ELASTIC,
    EASE_OUT_BACK,
} EaseType;

typedef void (*TweenCallback)(void *ctx);

typedef struct {
    float *target;
    float from;
    float to;
    uint32_t duration_ms;
    uint32_t elapsed_ms;
    EaseType ease;
    bool active;
    TweenCallback on_complete;
    void *cb_ctx;
} Tween;

void tween_init(void);
void tween_update_all(uint32_t dt_ms);

// Start a new tween, returns tween ID or -1 if pool full
int tween_start(float *target, float from, float to,
                uint32_t duration_ms, EaseType ease,
                TweenCallback on_complete, void *ctx);

void tween_cancel(int id);
void tween_cancel_target(float *target);
bool tween_is_active(int id);

// Easing functions (exposed for direct use)
float ease_compute(EaseType type, float t);
