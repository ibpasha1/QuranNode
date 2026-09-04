#include "tween.h"
#include <math.h>

static Tween tweens[MAX_TWEENS];

void tween_init(void)
{
    for (int i = 0; i < MAX_TWEENS; i++) {
        tweens[i].active = false;
    }
}

float ease_compute(EaseType type, float t)
{
    switch (type) {
    case EASE_LINEAR:
        return t;
    case EASE_IN_QUAD:
        return t * t;
    case EASE_OUT_QUAD:
        return t * (2.0f - t);
    case EASE_IN_OUT_QUAD:
        return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    case EASE_IN_CUBIC:
        return t * t * t;
    case EASE_OUT_CUBIC: {
        float f = t - 1.0f;
        return f * f * f + 1.0f;
    }
    case EASE_IN_OUT_CUBIC:
        return t < 0.5f ? 4.0f * t * t * t
                        : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
    case EASE_OUT_BOUNCE:
        if (t < 1.0f / 2.75f) {
            return 7.5625f * t * t;
        } else if (t < 2.0f / 2.75f) {
            t -= 1.5f / 2.75f;
            return 7.5625f * t * t + 0.75f;
        } else if (t < 2.5f / 2.75f) {
            t -= 2.25f / 2.75f;
            return 7.5625f * t * t + 0.9375f;
        } else {
            t -= 2.625f / 2.75f;
            return 7.5625f * t * t + 0.984375f;
        }
    case EASE_OUT_ELASTIC:
        if (t == 0.0f || t == 1.0f) return t;
        return powf(2.0f, -10.0f * t) * sinf((t - 0.075f) * (2.0f * M_PI) / 0.3f) + 1.0f;
    case EASE_OUT_BACK: {
        float s = 1.70158f;
        float f = t - 1.0f;
        return f * f * ((s + 1.0f) * f + s) + 1.0f;
    }
    default:
        return t;
    }
}

int tween_start(float *target, float from, float to,
                uint32_t duration_ms, EaseType ease,
                TweenCallback on_complete, void *ctx)
{
    // Cancel any existing tween on same target
    tween_cancel_target(target);

    for (int i = 0; i < MAX_TWEENS; i++) {
        if (!tweens[i].active) {
            tweens[i].target = target;
            tweens[i].from = from;
            tweens[i].to = to;
            tweens[i].duration_ms = duration_ms;
            tweens[i].elapsed_ms = 0;
            tweens[i].ease = ease;
            tweens[i].active = true;
            tweens[i].on_complete = on_complete;
            tweens[i].cb_ctx = ctx;
            *target = from;
            return i;
        }
    }
    return -1;
}

void tween_update_all(uint32_t dt_ms)
{
    for (int i = 0; i < MAX_TWEENS; i++) {
        if (!tweens[i].active) continue;

        tweens[i].elapsed_ms += dt_ms;

        if (tweens[i].elapsed_ms >= tweens[i].duration_ms) {
            *tweens[i].target = tweens[i].to;
            tweens[i].active = false;
            if (tweens[i].on_complete) {
                tweens[i].on_complete(tweens[i].cb_ctx);
            }
        } else {
            float t = (float)tweens[i].elapsed_ms / (float)tweens[i].duration_ms;
            float eased = ease_compute(tweens[i].ease, t);
            *tweens[i].target = tweens[i].from + (tweens[i].to - tweens[i].from) * eased;
        }
    }
}

void tween_cancel(int id)
{
    if (id >= 0 && id < MAX_TWEENS) {
        tweens[id].active = false;
    }
}

void tween_cancel_target(float *target)
{
    for (int i = 0; i < MAX_TWEENS; i++) {
        if (tweens[i].active && tweens[i].target == target) {
            tweens[i].active = false;
        }
    }
}

bool tween_is_active(int id)
{
    if (id >= 0 && id < MAX_TWEENS) {
        return tweens[id].active;
    }
    return false;
}
