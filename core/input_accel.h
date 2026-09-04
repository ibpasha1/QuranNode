// input_accel.h — hold-to-scroll acceleration for list navigation.
//
// The HALs emit a stream of repeated nav events while a key is held (SDL key
// repeat in the sim; auto-repeat on hardware). Scenes feed each event through
// input_accel_step() and get back how many rows to move: 1 for taps, ramping
// to 2/4/8 the longer the same direction is held. Any pause or direction
// change resets the ramp, so single presses always move exactly one row.
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t last_ms;   // when the previous step in this run happened
    int      dir;       // direction of the current run (+1/-1), 0 = idle
    int      streak;    // consecutive rapid steps in the same direction
} InputAccel;

// Feed one nav event (dir = +1 or -1, now = plat_millis()). Returns the number
// of rows to move this event (always >= 1; sign is NOT applied).
int input_accel_step(InputAccel *a, int dir, uint32_t now);
