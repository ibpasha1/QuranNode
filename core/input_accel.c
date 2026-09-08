#include "input_accel.h"

// A run is "held" while same-direction events arrive within this gap. HAL/OS
// repeat rates vary a lot: the device auto-repeats every 40-180 ms, but the sim
// leans on the host key-repeat rate, which on a slow "Key Repeat" setting can be
// ~500 ms between events. The gap must span the slowest repeat we expect, or the
// streak resets on every repeat and acceleration never engages (holding crawls
// one row at a time). 600 ms covers those cases while still resetting on the
// deliberate one-at-a-time tapping people do when they're not holding.
#define ACCEL_GAP_MS 600

int input_accel_step(InputAccel *a, int dir, uint32_t now)
{
    if (dir != a->dir || (uint32_t)(now - a->last_ms) > ACCEL_GAP_MS)
        a->streak = 0;
    a->dir = dir;
    a->last_ms = now;
    if (a->streak < 127) a->streak++;

    // Ramp: the first few repeats stay 1:1 for precise nudging, then climb hard
    // so a sustained hold sweeps a long list (114 surahs) in a second or two.
    // Overshoot at the top end is harmless — callers clamp to the list bounds.
    if (a->streak < 3)  return 1;
    if (a->streak < 6)  return 2;
    if (a->streak < 10) return 4;
    if (a->streak < 16) return 8;
    if (a->streak < 24) return 16;
    return 32;
}
