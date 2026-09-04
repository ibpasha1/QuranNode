// player.h — recitation transport + word-sync clock.
//
// Owns the "what's playing" state for Quran mode: current surah/ayah, the audio
// clip (via the HAL), the word timings, playback speed, and the derived active
// word. The reader scene reads current_ayah()/active_word() to draw, and calls
// toggle()/set_rate()/next_ayah() from input. player_update() (once per frame)
// refreshes the active word from the audio playhead and auto-advances to the next
// ayah when one finishes.
#pragma once

#include "timing.h"
#include <stdint.h>
#include <stdbool.h>

struct HalAudioClip;

// Ayah Loop — the memorization engine. Repeat each ayah N times, the whole range
// M times (0 = forever), with a pause between clips, at a fixed speed.
typedef struct {
    bool  active;
    int   start_ayah, end_ayah;
    int   each_reps;      // repeat each ayah this many times
    int   section_reps;   // repeat the whole range this many times (0 = infinite)
    uint32_t pause_ms;    // silence between clips
    float rate;
} LoopConfig;

typedef struct {
    const char *reciter;      // e.g. "abdulbasit"
    int   surah;
    int   ayah;
    int   ayah_min, ayah_max; // navigable range within the current surah
    int   active_word;        // -1 = none
    float rate;               // 1.0 = normal, 0.85 = slower (pitch preserved)
    bool  playing;            // user intent (survives natural clip end handling)
    bool  autoplay_next;      // continue into the next ayah when one ends

    struct HalAudioClip *clip;
    TimingTable timing;
    bool  timing_ok;

    // Loop state.
    LoopConfig loop;
    int   loop_each_i;        // which repeat of the current ayah (0-based)
    int   loop_section_i;     // which pass over the range (0-based)
    bool  in_pause;           // currently in an inter-clip pause
    uint32_t pause_until;     // plat_millis() deadline for the pause
} Player;

// The single shared transport instance (reader + loop editor drive the same one).
extern Player g_player;

void player_init(Player *p, const char *reciter);

// Point the player at an ayah: (re)loads its audio clip + the surah timings.
// Does not start playback. Clamps ayah into the available range.
void player_load(Player *p, int surah, int ayah);

void player_play(Player *p);
void player_pause(Player *p);
void player_toggle(Player *p);

void player_next_ayah(Player *p);   // stops at surah end (M1c: wrap to next surah)
void player_prev_ayah(Player *p);

void player_set_rate(Player *p, float rate);
void player_nudge_rate(Player *p, int dir);   // dir=+/-1, steps of 0.05

// Start / stop an Ayah Loop. start_loop loads the range's first ayah and plays.
void player_start_loop(Player *p, const LoopConfig *cfg);
void player_stop_loop(Player *p);

// Call once per frame. Updates active_word from the playhead and handles
// end-of-ayah auto-advance.
void player_update(Player *p);

uint32_t player_pos_ms(const Player *p);
uint32_t player_len_ms(const Player *p);
