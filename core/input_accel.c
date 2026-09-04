#include "input_accel.h"

// A run is "held" while same-direction events arrive within this gap. OS/HAL
// repeat rates sit around 15-30 Hz, so 250 ms comfortably spans one repeat
// period while still resetting on deliberate discrete presses.
#define ACCEL_GAP_MS 250

int input_accel_step(InputAccel *a, int dir, uint32_t now)
{
    if (dir != a->dir || (uint32_t)(now - a->last_ms) > ACCEL_GAP_MS)
        a->streak = 0;
    a->dir = dir;
    a->last_ms = now;
    if (a->streak < 127) a->streak++;

    // Ramp: ~the first half-second of repeats stays 1:1, then speed up.
    if (a->streak < 10) return 1;
    if (a->streak < 20) return 2;
    if (a->streak < 32) return 4;
    return 8;
}
