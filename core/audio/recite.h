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
