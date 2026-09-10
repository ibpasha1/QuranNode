// recite_live_test.c — deterministic checks for the live score-follower.
//
// Builds a synthetic "ayah" of N words, each a distinct tone so the aligner
// can actually localize, then streams a user recording at it in 100ms feeds
// (as the scene does) and checks the live per-word verdicts + cursor for three
// cases: a clean take, a garbled word, and a mid-ayah RESTART (redo a word).
#include "recite.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HZ        16000
#define WORD_MS   1000
#define N_WORDS   6
#define FRAME_MS  40

static int fails = 0;
#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { printf("  FAIL: " msg "\n", ##__VA_ARGS__); fails++; } \
    else         { printf("  ok:   " msg "\n", ##__VA_ARGS__); } } while (0)

static const char *vname(ReciteVerdict v)
{
    switch (v) {
    case RECITE_GOOD: return "GOOD"; case RECITE_UNSURE: return "UNSURE";
    case RECITE_MISMATCH: return "MISMATCH"; case RECITE_MISSING: return "MISSING";
    default: return "UNCLEAR";
    }
}

// Append `ms` of a tone at frequency f (with a 2nd harmonic) to buf.
static void tone(int16_t *buf, uint32_t *n, float f, uint32_t ms)
{
    uint32_t samples = (uint32_t)ms * HZ / 1000;
    for (uint32_t i = 0; i < samples; i++) {
        double t = (double)(*n + i) / HZ;
        double s = 0.55 * sin(2 * M_PI * f * t) + 0.30 * sin(2 * M_PI * 2 * f * t);
        buf[*n + i] = (int16_t)(s * 9000.0);
    }
    *n += samples;
}

static float word_freq(int w) { return 260.f + 150.f * w; }   // distinct per word

// A weak tone buried in deterministic noise: simulates real cross-voice audio
// where the acoustic match is too weak to align (the case that stalled the
// grading follower).
static void tone_noisy(int16_t *buf, uint32_t *n, float f, uint32_t ms)
{
    uint32_t samples = (uint32_t)ms * HZ / 1000;
    for (uint32_t i = 0; i < samples; i++) {
        double t = (double)(*n + i) / HZ;
        double s = 0.30 * sin(2 * M_PI * f * t);
        uint32_t r = (*n + i) * 1103515245u + 12345u;
        double noise = (double)((r >> 16) & 0x7fff) / 32768.0 - 0.5;
        buf[*n + i] = (int16_t)((s + 0.70 * noise) * 9000.0);
    }
    *n += samples;
}

// Feed `usr[0..usr_n)` to rl in 100ms slices (streaming, like the scene).
static void stream(ReciteLive *rl, const int16_t *usr, uint32_t usr_n)
{
    uint32_t step = HZ / 10;   // 100ms
    for (uint32_t got = 0; got < usr_n; ) {
        got += step; if (got > usr_n) got = usr_n;
        recite_live_feed(rl, usr, got, HZ);
    }
    recite_live_finish(rl);
}

int main(void)
{
    // Reference: N_WORDS distinct tones back to back.
    int16_t *ref = malloc(sizeof(int16_t) * HZ * 20);
    int16_t *usr = malloc(sizeof(int16_t) * HZ * 20);
    uint32_t ref_n = 0;
    WordTiming words[N_WORDS];
    for (int w = 0; w < N_WORDS; w++) {
        words[w].start_ms = w * WORD_MS;
        words[w].end_ms   = (w + 1) * WORD_MS;
        tone(ref, &ref_n, word_freq(w), WORD_MS);
    }

    ReciteLive *rl = recite_live_create();

    // --- 1: clean take (user == reference) -------------------------------
    printf("[clean take]\n");
    recite_live_begin(rl, ref, ref_n, HZ, words, N_WORDS);
    uint32_t un = 0;
    for (int w = 0; w < N_WORDS; w++) tone(usr, &un, word_freq(w), WORD_MS);
    stream(rl, usr, un);
    int good = 0;
    for (int w = 0; w < N_WORDS; w++) {
        ReciteVerdict v = recite_live_verdict(rl, w);
        printf("    word %d: %s\n", w, vname(v));
        if (v == RECITE_GOOD) good++;
    }
    CHECK(good >= N_WORDS - 1, "clean take: >= %d/%d words GOOD (got %d)",
          N_WORDS - 1, N_WORDS, good);

    // --- 2: word 3 garbled (wrong tone) ----------------------------------
    printf("[garbled word 3]\n");
    recite_live_begin(rl, ref, ref_n, HZ, words, N_WORDS);
    un = 0;
    for (int w = 0; w < N_WORDS; w++)
        tone(usr, &un, w == 3 ? 3300.f : word_freq(w), WORD_MS);
    stream(rl, usr, un);
    for (int w = 0; w < N_WORDS; w++)
        printf("    word %d: %s\n", w, vname(recite_live_verdict(rl, w)));
    CHECK(recite_live_verdict(rl, 3) != RECITE_GOOD,
          "garbled word 3 is not GOOD (got %s)", vname(recite_live_verdict(rl, 3)));
    CHECK(recite_live_verdict(rl, 0) == RECITE_GOOD &&
          recite_live_verdict(rl, 5) == RECITE_GOOD,
          "words around the garble still GOOD");

    // --- 3: restart — recite 0,1,2 then REDO word 2, then 3,4,5 ----------
    printf("[restart / redo word 2]\n");
    recite_live_begin(rl, ref, ref_n, HZ, words, N_WORDS);
    un = 0;
    tone(usr, &un, word_freq(0), WORD_MS);
    tone(usr, &un, word_freq(1), WORD_MS);
    tone(usr, &un, word_freq(2), WORD_MS);
    tone(usr, &un, word_freq(2), WORD_MS);   // <-- redo word 2
    tone(usr, &un, word_freq(3), WORD_MS);
    tone(usr, &un, word_freq(4), WORD_MS);
    tone(usr, &un, word_freq(5), WORD_MS);
    stream(rl, usr, un);
    good = 0;
    for (int w = 0; w < N_WORDS; w++) {
        ReciteVerdict v = recite_live_verdict(rl, w);
        printf("    word %d: %s\n", w, vname(v));
        if (v == RECITE_GOOD) good++;
    }
    CHECK(recite_live_verdict(rl, 2) == RECITE_GOOD,
          "redone word 2 ends GOOD (got %s)", vname(recite_live_verdict(rl, 2)));
    CHECK(good >= N_WORDS - 1,
          "restart didn't derail: >= %d/%d GOOD (got %d)", N_WORDS - 1, N_WORDS, good);

    // --- 4: FOLLOW mode — weak/noisy audio, slower reader, must NOT stall ---
    printf("[follow: weak signal, no stall]\n");
    recite_live_begin(rl, ref, ref_n, HZ, words, N_WORDS);
    recite_live_set_follow(rl, true);
    un = 0;
    for (int w = 0; w < N_WORDS; w++) tone_noisy(usr, &un, word_freq(w), 1500); // 1.5x slower + noisy
    int maxcur = -1;
    uint32_t step = HZ / 10;
    for (uint32_t got = 0; got < un; ) {
        got += step; if (got > un) got = un;
        recite_live_feed(rl, usr, got, HZ);
        int c = recite_live_cursor_word(rl);
        if (c > maxcur) maxcur = c;
    }
    recite_live_finish(rl);
    printf("    cursor reached word %d of %d\n", maxcur, N_WORDS - 1);
    CHECK(maxcur >= N_WORDS - 1, "follow cursor reached the last word (no stall)");

    recite_live_destroy(rl);
    free(ref); free(usr);
    printf(fails ? "\nrecite-live-test: %d FAILED\n" : "\nrecite-live-test: all passed\n", fails);
    return fails ? 1 : 0;
}
