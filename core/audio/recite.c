#include "recite.h"
#include "plat.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// 100ms frames: coarse enough that a full DTW matrix for a ~20s ayah is small
// (~200x300 floats), fine enough to resolve word spans (words run 300ms+).
#define FRAME_MS   100
#define N_FEAT     3        // log-RMS, zero-crossing rate, spectral tilt
#define MAX_FRAMES 600      // 60s cap per side

// Feature distance thresholds on z-normalized features (empirical; identical
// audio scores ~0). Deliberately generous — V1 flags, it doesn't grade.
#define TH_GOOD    0.9f
#define TH_UNSURE  1.6f
// A user span whose pre-normalization energy is this far under the utterance
// median counts as silence -> the word is "missing".
#define SILENCE_DB 18.0f

typedef struct { float f[N_FEAT]; float raw_db; } Frame;

// Extract per-frame features from mono s16. Returns frame count.
static int extract(const int16_t *pcm, uint32_t n, uint32_t hz, Frame *out)
{
    uint32_t flen = hz * FRAME_MS / 1000;
    if (flen == 0) return 0;
    int nf = (int)(n / flen);
    if (nf > MAX_FRAMES) nf = MAX_FRAMES;
    for (int i = 0; i < nf; i++) {
        const int16_t *s = pcm + (uint32_t)i * flen;
        double e = 0, ed = 0;
        int zc = 0;
        for (uint32_t j = 0; j < flen; j++) {
            double v = s[j] / 32768.0;
            e += v * v;
            if (j) {
                double d = (s[j] - s[j - 1]) / 32768.0;
                ed += d * d;
                if ((s[j] >= 0) != (s[j - 1] >= 0)) zc++;
            }
        }
        e /= flen; ed /= flen;
        out[i].raw_db = (float)(10.0 * log10(e + 1e-10));
        out[i].f[0] = out[i].raw_db;
        out[i].f[1] = (float)zc / (float)flen;             // pitch-ish proxy
        out[i].f[2] = (float)(10.0 * log10(ed / (e + 1e-10) + 1e-10)); // tilt
    }
    return nf;
}

// Z-normalize each feature dim over the utterance: cancels mic gain, overall
// voice brightness, and level differences between the reference and the user.
static void znorm(Frame *fr, int n)
{
    for (int d = 0; d < N_FEAT; d++) {
        double mu = 0, sd = 0;
        for (int i = 0; i < n; i++) mu += fr[i].f[d];
        mu /= n;
        for (int i = 0; i < n; i++) { double v = fr[i].f[d] - mu; sd += v * v; }
        sd = sqrt(sd / n);
        if (sd < 1e-6) sd = 1e-6;
        for (int i = 0; i < n; i++) fr[i].f[d] = (float)((fr[i].f[d] - mu) / sd);
    }
}

static float fdist(const Frame *a, const Frame *b)
{
    float s = 0;
    for (int d = 0; d < N_FEAT; d++) {
        float v = a->f[d] - b->f[d];
        s += v * v;
    }
    return sqrtf(s);
}

// Median of the raw frame energies (dB) — the utterance's "voiced" floor ref.
static float median_db(const Frame *fr, int n)
{
    float tmp[MAX_FRAMES];
    for (int i = 0; i < n; i++) tmp[i] = fr[i].raw_db;
    // insertion sort (n <= 600)
    for (int i = 1; i < n; i++) {
        float v = tmp[i]; int j = i - 1;
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = v;
    }
    return tmp[n / 2];
}

bool recite_analyze(const int16_t *ref, uint32_t ref_n, uint32_t ref_hz,
                    const int16_t *usr, uint32_t usr_n, uint32_t usr_hz,
                    const WordTiming *words, int n_words, ReciteWord *out)
{
    if (!ref || !usr || !ref_n || !usr_n || n_words <= 0) return false;

    Frame *rf = malloc(sizeof(Frame) * MAX_FRAMES * 2);
    if (!rf) return false;
    Frame *uf = rf + MAX_FRAMES;
    int rn = extract(ref, ref_n, ref_hz, rf);
    int un = extract(usr, usr_n, usr_hz, uf);
    if (rn < 2 || un < 2) { free(rf); return false; }
    float usr_floor = median_db(uf, un) - SILENCE_DB;
    znorm(rf, rn);
    znorm(uf, un);

    // DTW: cost[i][j] = best path cost aligning ref[0..i] with usr[0..j].
    // Full matrix of costs + backpointers (coarse frames keep it ~700KB max,
    // typically ~50KB for real ayat; malloc'd, so failure degrades gracefully).
    float *cost = malloc(sizeof(float) * rn * un);
    uint8_t *bp = malloc((size_t)rn * un);
    if (!cost || !bp) { free(cost); free(bp); free(rf); return false; }
#define C(i, j) cost[(i) * un + (j)]
    for (int i = 0; i < rn; i++) {
        for (int j = 0; j < un; j++) {
            float d = fdist(&rf[i], &uf[j]);
            float best; uint8_t dir;
            if (i == 0 && j == 0) { best = 0; dir = 0; }
            else if (i == 0)      { best = C(0, j - 1); dir = 1; }       // left
            else if (j == 0)      { best = C(i - 1, 0); dir = 2; }       // up
            else {
                float diag = C(i - 1, j - 1), left = C(i, j - 1), up = C(i - 1, j);
                if (diag <= left && diag <= up) { best = diag; dir = 3; }
                else if (left <= up)            { best = left; dir = 1; }
                else                            { best = up;   dir = 2; }
            }
            C(i, j) = best + d;
            bp[i * un + j] = dir;
        }
    }

    // Backtrack: for each ref frame, the user frame range it aligned to.
    int lo[MAX_FRAMES], hi[MAX_FRAMES];
    for (int i = 0; i < rn; i++) { lo[i] = un; hi[i] = -1; }
    {
        int i = rn - 1, j = un - 1;
        while (1) {
            if (j < lo[i]) lo[i] = j;
            if (j > hi[i]) hi[i] = j;
            uint8_t dir = bp[i * un + j];
            if (dir == 0) break;
            if (dir == 3) { i--; j--; }
            else if (dir == 1) j--;
            else i--;
        }
    }

    // Score each reference word span.
    for (int w = 0; w < n_words; w++) {
        int fa = (int)(words[w].start_ms / FRAME_MS);
        int fb = (int)(words[w].end_ms / FRAME_MS);
        if (fb >= rn) fb = rn - 1;
        if (fa > fb) fa = fb;

        double sum = 0; int cnt = 0, voiced = 0, span = 0;
        int ja = un, jb = -1;
        for (int i = fa; i <= fb; i++) {
            if (hi[i] < 0) continue;
            if (lo[i] < ja) ja = lo[i];
            if (hi[i] > jb) jb = hi[i];
            for (int j = lo[i]; j <= hi[i]; j++) {
                sum += fdist(&rf[i], &uf[j]);
                cnt++;
            }
        }
        if (jb >= ja)
            for (int j = ja; j <= jb; j++, span++)
                if (uf[j].raw_db > usr_floor) voiced++;

        ReciteWord *o = &out[w];
        o->score = cnt ? (float)(sum / cnt) : 99.f;
        o->user_start_ms = (jb >= ja) ? (uint32_t)ja * FRAME_MS : 0;
        o->user_end_ms   = (jb >= ja) ? (uint32_t)(jb + 1) * FRAME_MS : 0;
        if (cnt == 0 || span == 0 || voiced * 2 < span)
            o->verdict = RECITE_MISSING;
        else if (o->score < TH_GOOD)
            o->verdict = RECITE_GOOD;
        else if (o->score < TH_UNSURE)
            o->verdict = RECITE_UNSURE;
        else
            o->verdict = RECITE_MISMATCH;
    }

    free(cost); free(bp); free(rf);
    return true;
}
