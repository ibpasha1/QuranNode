// recite.h — recitation similarity analysis (the Quran Teacher's V1 "ear").
//
// Aligns the user's recording of an ayah to the reference recitation with DTW
// over coarse acoustic features, then scores each word of the reference against
// the stretch of user audio the alignment mapped it to.
//
// HONESTY NOTE (V1): this is acoustic SIMILARITY, not pronunciation grading.
// It reliably catches omissions, skipped/garbled stretches, and pacing that
// diverges badly; it cannot judge whether an Arabic letter was articulated
// correctly (that needs a phoneme-level model — V2). Verdicts are therefore
// phrased as match confidence, not correctness.
#pragma once
#include "timing.h"
#include <stdint.h>

typedef enum {
    RECITE_GOOD = 0,    // close match
    RECITE_UNSURE,      // somewhat different — worth a listen
    RECITE_MISMATCH,    // significantly different from the reference
    RECITE_MISSING,     // little/no voice where this word should be
    RECITE_UNCLEAR,     // couldn't align confidently — no judgement made
} ReciteVerdict;

typedef struct {
    ReciteVerdict verdict;
    float    score;                    // mean feature distance (lower = closer)
    uint32_t user_start_ms, user_end_ms;  // matched span in the USER recording
} ReciteWord;

// Align `usr` (mono s16 @ usr_hz) against `ref` and score each of the ayah's
// `n_words` reference word spans (`words`, ayah-relative ms). Fills out[] and
// returns true; false on empty input or allocation failure.
bool recite_analyze(const int16_t *ref, uint32_t ref_n, uint32_t ref_hz,
                    const int16_t *usr, uint32_t usr_n, uint32_t usr_hz,
                    const WordTiming *words, int n_words, ReciteWord *out);

// --- Live (streaming) score-following -------------------------------------
// Real-time variant: feed the recording incrementally as it's captured and get
// per-word verdicts that settle a beat behind the reciter, plus a moving word
// cursor. Where recite_analyze() is a whole-take batch pass (it needs the full
// recording for global tempo + adaptive thresholds), this follows the reciter
// with an ONLINE, tempo-free time-warp: it advances the reference pointer as
// many frames per user frame as the pace demands (so a 3x-faster fluent user
// no longer defeats the slope limits), and it can jump BACKWARD when the user
// redoes a word — re-opening that word for a fresh verdict. Same ReciteVerdict
// scale; a word not yet reached (or just re-opened) reads RECITE_UNCLEAR.
typedef struct ReciteLive ReciteLive;

ReciteLive *recite_live_create(void);
void recite_live_destroy(ReciteLive *rl);

// Load an ayah: extract + cache the reference features and per-word ref spans.
// Call once when the ayah loads. false on alloc failure / empty reference.
bool recite_live_begin(ReciteLive *rl, const int16_t *ref, uint32_t ref_n,
                       uint32_t ref_hz, const WordTiming *words, int n_words);

// Re-arm for another take of the SAME ayah (keeps the cached reference).
void recite_live_reset(ReciteLive *rl);

// Reading-aid FOLLOW mode: a forward-only, non-stalling cursor that tracks
// where the reader is in the ayah (for highlight + auto-scroll). No grading,
// no backward restart. Call after begin/reset, before feeding.
void recite_live_set_follow(ReciteLive *rl, bool on);

// Feed the capture buffer: pcm[0..total_n) is the whole recording so far (mono
// s16 @ hz); only frames past what was already consumed get processed. Cheap
// to call every tick.
void recite_live_feed(ReciteLive *rl, const int16_t *pcm, uint32_t total_n,
                      uint32_t hz);

// Settle any still-pending words at end of capture.
void recite_live_finish(ReciteLive *rl);

ReciteVerdict recite_live_verdict(const ReciteLive *rl, int word);
float recite_live_score(const ReciteLive *rl, int word);   // blended distance (lower=closer)
int  recite_live_cursor_word(const ReciteLive *rl);   // -1 before first voice
// Matched user span (ms, recording timeline) for replay; false if none yet.
bool recite_live_word_span(const ReciteLive *rl, int word,
                           uint32_t *start_ms, uint32_t *end_ms);
