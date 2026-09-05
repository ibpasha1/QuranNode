#include "recite.h"
#include "plat.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// 100ms frames: coarse enough that a full DTW matrix for a ~20s ayah is small
// (~200x300 floats), fine enough to resolve word spans (words run 300ms+).
#define FRAME_MS   100
#define N_FEAT     6        // log-RMS, ZCR, tilt + 3 spectral band ratios
#define MAX_FRAMES 600      // 60s cap per side

// Band centers for the spectral features (Goertzel single-bin energies,
// expressed relative to total frame energy so they capture timbre, not
// level). Chosen for voice: F0/low harmonics, mid formants, upper formants.
static const float BAND_HZ[3] = { 300.f, 900.f, 2200.f };

// Feature distance thresholds on z-normalized features (empirical; identical
// audio scores ~0). Deliberately generous — V1 flags, it doesn't grade.
#define TH_GOOD    1.0f
#define TH_UNSURE  1.8f
// Silence floor: this far under the voiced level counts as "no voice there".
// Anchored to BOTH the median and the peak — median alone collapses when the
// take is mostly silence (median = the silence itself), peak alone is fooled
// by one loud plosive. Speech sits within ~25dB of its own peak.
#define SILENCE_DB 18.0f
#define PEAK_RANGE_DB 30.0f

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

        // Spectral bands (Goertzel single-bin energy at each center),
        // relative to total energy -> timbre profile independent of level.
        for (int b = 0; b < 3; b++) {
            double w = 2.0 * M_PI * BAND_HZ[b] / (double)hz;
            double coef = 2.0 * cos(w);
            double q0, q1 = 0, q2 = 0;
            for (uint32_t j = 0; j < flen; j++) {
                q0 = coef * q1 - q2 + s[j] / 32768.0;
                q2 = q1; q1 = q0;
            }
            double p = (q1 * q1 + q2 * q2 - coef * q1 * q2) / flen;
            out[i].f[3 + b] = (float)(10.0 * log10(p / (e * flen + 1e-10) + 1e-10));
        }
    }
    return nf;
}

// Z-normalize each feature dim over the utterance: cancels mic gain, overall
// voice brightness, and level differences between the reference and the user.
// Stats come from VOICED frames only — silence log-energy is an extreme
// outlier (~-100dB) that would skew mean/std by how much silence each side
// happens to contain, making identical voice score as different. Values are
// clamped to +/-4 sigma so residual silence frames stay bounded outliers.
static void znorm(Frame *fr, int n, float floor_db)
{
    for (int d = 0; d < N_FEAT; d++) {
        double mu = 0, sd = 0;
        int nv = 0;
        for (int i = 0; i < n; i++)
            if (fr[i].raw_db > floor_db) { mu += fr[i].f[d]; nv++; }
        if (nv < 4) {   // almost nothing voiced: fall back to all frames
            mu = 0; nv = n;
            for (int i = 0; i < n; i++) mu += fr[i].f[d];
        }
        mu /= nv;
        int ns = 0;
        for (int i = 0; i < n; i++) {
            if (nv < n && fr[i].raw_db <= floor_db) continue;
            double v = fr[i].f[d] - mu;
            sd += v * v; ns++;
        }
        sd = sqrt(sd / (ns > 0 ? ns : 1));
        if (sd < 1e-6) sd = 1e-6;
        for (int i = 0; i < n; i++) {
            float z = (float)((fr[i].f[d] - mu) / sd);
            if (z > 4.f) z = 4.f; else if (z < -4.f) z = -4.f;
            fr[i].f[d] = z;
        }
    }
}

// Weighted distance: envelope/rate features carry the match; the spectral
// bands refine it (they separate voice from noise/garble) but are damped so
// legitimate voice-timbre differences don't overwhelm the rhythm agreement.
static const float FEAT_W[N_FEAT] = { 1.f, 1.f, 1.f, 0.45f, 0.45f, 0.45f };

static float fdist(const Frame *a, const Frame *b)
{
    float s = 0;
    for (int d = 0; d < N_FEAT; d++) {
        float v = a->f[d] - b->f[d];
        s += FEAT_W[d] * v * v;
    }
    return sqrtf(s);
}

// Percentile of the raw frame energies (dB). p in [0,100].
static float percentile_db(const Frame *fr, int n, int p)
{
    float tmp[MAX_FRAMES];
    for (int i = 0; i < n; i++) tmp[i] = fr[i].raw_db;
    // insertion sort (n <= 600)
    for (int i = 1; i < n; i++) {
        float v = tmp[i]; int j = i - 1;
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = v;
    }
    int k = n * p / 100;
    if (k >= n) k = n - 1;
    return tmp[k];
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
    // Voiced floor from the 95th-percentile level, not the max — one loud
    // plosive would otherwise push softer (but real) words under the floor.
    float floor_med = percentile_db(uf, un, 50) - SILENCE_DB;
    float floor_p95 = percentile_db(uf, un, 95) - PEAK_RANGE_DB;
    float usr_floor = floor_med > floor_p95 ? floor_med : floor_p95;
    znorm(rf, rn, percentile_db(rf, rn, 95) - PEAK_RANGE_DB);
    znorm(uf, un, usr_floor);

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

    // Score each reference word span. Pause frames in the user audio (a
    // learner breathing between words) are excluded from the mean — pausing
    // isn't a mismatch. Whether the word was voiced AT ALL is judged
    // separately for the "missing" verdict.
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
                if (uf[j].raw_db <= usr_floor) continue;   // pause, not speech
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
        // Missing = the mapped stretch is under 1/3 voice.
        o->verdict = (cnt == 0 || span == 0 || voiced * 3 < span)
                         ? RECITE_MISSING : RECITE_GOOD;   // grade below
    }

    // Verdicts are RELATIVE to this take: a learner's voice against a master
    // reciter never scores near zero, so absolute thresholds would flag
    // everything (the "always unsure" failure). Grade against the take's own
    // LOWER-QUARTILE word score — the best-matched words set the bar, so a
    // take that's half garbled can't drag the baseline up — with absolute
    // floors (identical audio is always good) and ceilings (wholesale
    // divergence can't self-normalize into a pass).
    {
        float sc[128];
        int ns = 0;
        for (int w = 0; w < n_words && ns < 128; w++)
            if (out[w].verdict != RECITE_MISSING) sc[ns++] = out[w].score;
        float base = 0;
        if (ns) {
            for (int i = 1; i < ns; i++) {   // insertion sort
                float v = sc[i]; int j = i - 1;
                while (j >= 0 && sc[j] > v) { sc[j + 1] = sc[j]; j--; }
                sc[j + 1] = v;
            }
            base = sc[ns / 4];
        }
        float th_g = base * 1.30f; if (th_g < TH_GOOD) th_g = TH_GOOD;
        float th_u = base * 1.70f; if (th_u < TH_UNSURE) th_u = TH_UNSURE;
        if (th_g > 2.6f) th_g = 2.6f;
        if (th_u > 3.2f) th_u = 3.2f;
        for (int w = 0; w < n_words; w++) {
            if (out[w].verdict == RECITE_MISSING) continue;
            out[w].verdict = out[w].score <= th_g ? RECITE_GOOD
                           : out[w].score <= th_u ? RECITE_UNSURE
                                                  : RECITE_MISMATCH;
        }
    }

    free(cost); free(bp); free(rf);
    return true;
}
