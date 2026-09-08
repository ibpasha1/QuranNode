// hifz_drill.h — the talqeen drill: hear it, echo it, recall it under a veil.
//
// This is the phase machine behind a single memorization sitting. The dashboard
// (scene_hifz) picks a portion and chunks it into word-range SEGMENTS; this
// engine walks each segment through:
//
//   LISTEN → ECHO → FADE(33%) → FADE(66%) → RECALL(100%) → ASSESS
//
// then stitches segments together with a SLIDING connect window and one final
// full-chunk run. Three rules are load-bearing and must not be "simplified":
//
//  * The connect window SLIDES, it does not restart. After grading segment k we
//    recite [max(0,k-2) .. k], never "1, then 1+2, then 1+2+3" — restarting
//    from the first segment is exactly what breeds strong openings and weak
//    middles. Every seam gets practised twice; every segment gets equal reps.
//
//  * A grade may only be taken from a HIDDEN state. ASSESS is reachable only
//    out of RECALL. Grading while the text is visible measures reading fluency,
//    not recall.
//
//  * "Recite it x times" is a TIMED rep, not a button press. The countdown runs
//    the segment's reference duration × pace%, so it enforces a realistic tempo
//    instead of a rushed mumble and gives the UI something to show during
//    silence. OK finishes a phase early; "<" replays the current rep.
//
// The engine is pure bookkeeping + playback: it records a grade per segment and
// exposes the worst-of as the portion grade, but never touches hifz state — the
// scene applies hifz_grade()/hifz_grade_ayah() on completion. It drives the
// shared g_player for LISTEN (and restores surah/ayah/rate on end), so it is
// exercisable headlessly against loop_test.c's virtual-clip harness.
#pragma once

#include "hifz.h"          // HifzSeg, HifzGrade
#include "arabic_text.h"   // VeilMode
#include <stdint.h>
#include <stdbool.h>

#define HIFZ_DRILL_MAX_SEGS 32   // a portion is ~5-20 ayat; chunks well under this

typedef enum {
    DRILL_LISTEN = 0,   // teacher plays the segment; text + word highlight
    DRILL_ECHO,         // timed reps, text fully visible
    DRILL_FADE1,        // recite with the ending 33% veiled (box scaffold)
    DRILL_FADE2,        // recite with 66% veiled
    DRILL_RECALL,       // recite fully veiled — the state a grade comes from
    DRILL_ASSESS,       // pick GOT / SHAKY / NO (reachable only from RECALL)
    DRILL_CONNECT,      // recite the sliding window, veiled (k > 0 only)
    DRILL_CHUNK_CONNECT,// one final full-chunk recite, veiled
    DRILL_DONE,         // finished; read the portion grade and apply it
} DrillPhase;

typedef struct {
    uint8_t  listen_reps;   // LISTEN playthroughs
    uint8_t  echo_reps;     // ECHO timed reps
    uint16_t pace_pct;      // recall countdown as % of the reference duration
} HifzDrillCfg;

// Start a sitting over `segs` (copied). Saves g_player surah/ayah/rate. The cfg
// mirrors hifz_cfg_* defaults; pass NULL for all-defaults.
void hifz_drill_begin(int surah, const HifzSeg *segs, int nsegs,
                      const HifzDrillCfg *cfg);
void hifz_drill_end(void);              // restores g_player; safe to call twice
void hifz_drill_tick(uint32_t now_ms);  // per frame: audio + timed-rep advance

// --- view ----------------------------------------------------------------
DrillPhase     hifz_drill_phase(void);
int            hifz_drill_seg(void);        // current segment index (0-based)
int            hifz_drill_nsegs(void);
int            hifz_drill_surah(void);
const HifzSeg *hifz_drill_cur_seg(void);    // the unit currently on screen
void           hifz_drill_window(int *from, int *to);  // connect window (seg idx)
void           hifz_drill_veil(VeilMode *mode, int *pct);
int            hifz_drill_rep(void);        // current rep (timed phases)
int            hifz_drill_reps(void);       // total reps this phase
uint32_t       hifz_drill_rep_remaining(uint32_t now);  // ms left in this rep
uint32_t       hifz_drill_rep_ms(void);     // a timed rep's full duration
bool           hifz_drill_is_audio(void);   // LISTEN is playing audio, not timed

// --- input (all pass the frame clock so transitions can arm the timer) ----
void hifz_drill_grade(HifzGrade g, uint32_t now);  // ASSESS only
void hifz_drill_skip(uint32_t now);    // OK — end the current phase early
void hifz_drill_repeat(uint32_t now);  // "<" — replay the current rep/phase
void hifz_drill_peek(uint32_t now);    // reveal the text (caps this seg's GOT)
bool hifz_drill_peeking(void);         // is the text currently revealed?

// --- result (valid once phase == DRILL_DONE) ------------------------------
bool      hifz_drill_done(void);
HifzGrade hifz_drill_portion_grade(void);  // worst recorded segment grade
int       hifz_drill_seg_grade(int k);     // -1 = not yet graded, else HifzGrade
int       hifz_drill_peeks(void);          // total peeks this sitting
