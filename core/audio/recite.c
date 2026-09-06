#include "recite.h"
#include "plat.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// 40ms frames (32ms analysis window): fine enough that a word spans 15-25
// frames, so DTW must match the phonetic TRAJECTORY through each word — at
// the previous 100ms a word was 3-8 frames of averaged spectrum, and
// compressed same-voice mumble could cherry-pick a charitable alignment
// (field-proven). A ~20s ayah caps the DTW matrix at 500x500 (~1.25MB,
// malloc'd; bundled ayat are well under).
#define FRAME_MS   40
#define MAX_FRAMES 500      // 20s cap per side

// Features: log-energy + 12 MFCCs + their delta and delta-delta (rate of
// change / acceleration). The cepstral coefficients capture the vowel/
// consonant CONTENT of each frame; the deltas capture the TRANSITIONS into
// and out of consonants, which carry much of what distinguishes Arabic
// articulation (a stationary spectral shape looks the same for many wrong
// sounds; its trajectory doesn't).
#define N_MFCC     12
#define N_FEAT     (1 + 3 * N_MFCC)   // energy, C, dC, ddC
#define FFT_N      512      // 32ms window @16k, centered in each frame
#define N_MEL      20
#define MEL_LO_HZ  100.f
#define MEL_HI_HZ  7000.f
#define FEAT_HZ    16000    // analysis domain; other rates are decimated in

// Feature distance thresholds on normalized features (empirical; identical
// audio scores ~0). Deliberately generous — V1 flags, it doesn't grade.
#define TH_GOOD    1.25f
#define TH_UNSURE  1.8f

// Extra cost per non-diagonal DTW step, and per-word warp penalty gain: the
// aligner pays for stretching/compressing, so it can no longer "explain
// away" a mistake with an aggressive alignment for free.
#define STEP_PEN   0.15f
#define WARP_GAIN  0.35f
#define DTW_BAND   30       // Sakoe-Chiba half-width, frames (~1.2s)

typedef struct { float f[N_FEAT]; float raw_db; } Frame;

// In-place iterative radix-2 complex FFT (re/im interleaved pairs).
static void fft_c(float *x, int n)
{
    for (int i = 1, j = 0; i < n; i++) {   // bit-reverse permutation
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = x[2 * i], ti = x[2 * i + 1];
            x[2 * i] = x[2 * j]; x[2 * i + 1] = x[2 * j + 1];
            x[2 * j] = tr; x[2 * j + 1] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.f, ci = 0.f;
            for (int k = 0; k < len / 2; k++) {
                int a = 2 * (i + k), b = 2 * (i + k + len / 2);
                float vr = x[b] * cr - x[b + 1] * ci;
                float vi = x[b] * ci + x[b + 1] * cr;
                x[b] = x[a] - vr; x[b + 1] = x[a + 1] - vi;
                x[a] += vr; x[a + 1] += vi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

static float mel_of(float hz) { return 2595.f * log10f(1.f + hz / 700.f); }

// Mel filterbank: for each FFT bin, which triangular filter it falls in and
// the weight (each bin contributes w to filter k and 1-w to filter k-1).
static uint8_t s_mel_bin[FFT_N / 2];
static float   s_mel_w[FFT_N / 2];
static bool    s_mel_ready;

static void mel_init(void)
{
    float mlo = mel_of(MEL_LO_HZ), mhi = mel_of(MEL_HI_HZ);
    for (int b = 0; b < FFT_N / 2; b++) {
        float hz = (float)b * FEAT_HZ / FFT_N;
        float m = (mel_of(hz) - mlo) / (mhi - mlo) * N_MEL;   // 0..N_MEL
        if (m <= 0.f || m >= (float)N_MEL) { s_mel_bin[b] = 0xFF; continue; }
        s_mel_bin[b] = (uint8_t)m;
        s_mel_w[b] = m - (float)s_mel_bin[b];
    }
    s_mel_ready = true;
}

// Extract per-frame features from mono s16 at any rate (decimated to 16k by
// nearest-neighbor gather — fine for 100ms-frame features). Returns count.
static int extract(const int16_t *pcm, uint32_t n, uint32_t hz, Frame *out)
{
    if (hz == 0) return 0;
    if (!s_mel_ready) mel_init();
    uint32_t flen = hz * FRAME_MS / 1000;   // native samples per frame
    if (flen == 0) return 0;
    int nf = (int)(n / flen);
    if (nf > MAX_FRAMES) nf = MAX_FRAMES;

    float *fx = malloc(sizeof(float) * FFT_N * 2);
    if (!fx) return 0;

    for (int i = 0; i < nf; i++) {
        const int16_t *s = pcm + (uint32_t)i * flen;

        // Energy + ZCR over the full frame in the native domain.
        double e = 0;
        int zc = 0;
        for (uint32_t j = 0; j < flen; j++) {
            double v = s[j] / 32768.0;
            e += v * v;
            if (j && ((s[j] >= 0) != (s[j - 1] >= 0))) zc++;
        }
        e /= flen;
        (void)zc;
        out[i].raw_db = (float)(10.0 * log10(e + 1e-10));
        out[i].f[0] = out[i].raw_db;

        // MFCCs from a 64ms Hann-windowed slice centered in the frame,
        // gathered at 16k spacing from the native-rate samples.
        uint32_t span16 = (uint32_t)((uint64_t)flen * FEAT_HZ / hz); // ~1600
        uint32_t off16 = span16 > FFT_N ? (span16 - FFT_N) / 2 : 0;
        for (int k = 0; k < FFT_N; k++) {
            uint32_t j = (uint32_t)((uint64_t)(off16 + k) * hz / FEAT_HZ);
            if (j >= flen) j = flen - 1;
            float w = 0.5f - 0.5f * cosf(2.f * (float)M_PI * k / (FFT_N - 1));
            fx[2 * k] = (s[j] / 32768.f) * w;
            fx[2 * k + 1] = 0.f;
        }
        fft_c(fx, FFT_N);

        float mel[N_MEL] = { 0 };
        for (int b = 1; b < FFT_N / 2; b++) {
            if (s_mel_bin[b] == 0xFF) continue;
            float p = fx[2 * b] * fx[2 * b] + fx[2 * b + 1] * fx[2 * b + 1];
            int m = s_mel_bin[b];
            float w = s_mel_w[b];
            mel[m] += p * (1.f - w);
            if (m + 1 < N_MEL) mel[m + 1] += p * w;
        }
        float logmel[N_MEL];
        for (int m = 0; m < N_MEL; m++) logmel[m] = log10f(mel[m] + 1e-9f);
        for (int c = 1; c <= N_MFCC; c++) {   // DCT-II, c0 (level) dropped
            float acc = 0;
            for (int m = 0; m < N_MEL; m++)
                acc += logmel[m] *
                       cosf((float)M_PI * c * (m + 0.5f) / N_MEL);
            out[i].f[c] = acc;
        }
    }
    free(fx);
    return nf;
}

// Delta (+/-1 frame slope) and delta-delta of the cepstral coefficients.
static void add_deltas(Frame *fr, int n)
{
    for (int c = 1; c <= N_MFCC; c++) {
        for (int t = 0; t < n; t++) {
            int a = t > 0 ? t - 1 : 0, b = t < n - 1 ? t + 1 : n - 1;
            fr[t].f[N_MFCC + c] = (fr[b].f[c] - fr[a].f[c]) * 0.5f;
        }
        for (int t = 0; t < n; t++) {
            int a = t > 0 ? t - 1 : 0, b = t < n - 1 ? t + 1 : n - 1;
            fr[t].f[2 * N_MFCC + c] =
                (fr[b].f[N_MFCC + c] - fr[a].f[N_MFCC + c]) * 0.5f;
        }
    }
}

// Cepstral MEAN normalization per side (cancels mic/channel coloring and
// overall voice brightness), but the per-dim SCALE comes from the REFERENCE
// only, applied to both sides. Full per-side variance normalization would
// make a monotone mumble look as spectrally lively as the reciter — the
// variance difference IS information about articulation, so we keep it.
// Stats over VOICED frames only; silence frames would skew them by how much
// silence each side happens to contain. Output clamped to +/-5 ref-sigma.
static void cmn_stats(const Frame *fr, int n, float floor_db,
                      float *mu, float *sd /* sd may be NULL */)
{
    for (int d = 0; d < N_FEAT; d++) {
        double m = 0, s = 0;
        int nv = 0;
        for (int i = 0; i < n; i++)
            if (fr[i].raw_db > floor_db) { m += fr[i].f[d]; nv++; }
        if (nv < 4) {
            m = 0; nv = n;
            for (int i = 0; i < n; i++) m += fr[i].f[d];
        }
        m /= nv;
        mu[d] = (float)m;
        if (!sd) continue;
        int ns = 0;
        for (int i = 0; i < n; i++) {
            if (fr[i].raw_db <= floor_db && nv < n) continue;
            double v = fr[i].f[d] - m;
            s += v * v; ns++;
        }
        s = sqrt(s / (ns > 0 ? ns : 1));
        sd[d] = s < 1e-4 ? 1e-4f : (float)s;
    }
}

static void cmn_apply(Frame *fr, int n, const float *mu, const float *sd)
{
    for (int d = 0; d < N_FEAT; d++)
        for (int i = 0; i < n; i++) {
            float z = (fr[i].f[d] - mu[d]) / sd[d];
            if (z > 5.f) z = 5.f; else if (z < -5.f) z = -5.f;
            fr[i].f[d] = z;
        }
}

// Weighted distance: static MFCCs and energy carry the match; deltas refine
// (transition shape), delta-deltas gently. Normalized so scores stay in the
// same ballpark (~0 identical, ~1-2 matched, higher diverging) regardless of
// the vector width.
static float feat_w(int d)
{
    if (d == 0) return 1.0f;                  // energy
    if (d <= N_MFCC) return 1.0f;             // C1..C12
    if (d <= 2 * N_MFCC) return 0.7f;         // deltas
    return 0.4f;                              // delta-deltas
}

static float s_wsum;   // set once in recite_analyze

static float fdist(const Frame *a, const Frame *b)
{
    float s = 0;
    for (int d = 0; d < N_FEAT; d++) {
        float v = a->f[d] - b->f[d];
        s += feat_w(d) * v * v;
    }
    return sqrtf(s * (3.f / s_wsum));
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
    // Voiced floor = just above the take's own NOISE floor (5th percentile),
    // not relative to the voice level: mic auto-gain records the first word
    // far quieter than the rest, and a voice-relative floor then reads that
    // quiet-but-present word as silence ("not heard"). Guard for takes with
    // no silence at all: stay at least 12dB under the p95 voice level.
    float usr_floor = percentile_db(uf, un, 5) + 8.f;
    float usr_cap = percentile_db(uf, un, 95) - 12.f;
    if (usr_floor > usr_cap) usr_floor = usr_cap;
    float ref_floor = percentile_db(rf, rn, 5) + 8.f;
    float ref_cap = percentile_db(rf, rn, 95) - 12.f;
    if (ref_floor > ref_cap) ref_floor = ref_cap;

    add_deltas(rf, rn);
    add_deltas(uf, un);
    {
        static float mu[N_FEAT], sd[N_FEAT];
        cmn_stats(rf, rn, ref_floor, mu, sd);
        cmn_apply(rf, rn, mu, sd);
        cmn_stats(uf, un, usr_floor, mu, NULL);   // own mean, REF scale
        cmn_apply(uf, un, mu, sd);
    }
    s_wsum = 0;
    for (int d = 0; d < N_FEAT; d++) s_wsum += feat_w(d);

    // Constrained DTW: Sakoe-Chiba band around the length-scaled diagonal +
    // slope-limited steps {(1,1),(1,2),(2,1)} so the local warp stays within
    // 0.5-2x, with a small penalty on the warping steps. The aligner can now
    // FAIL (no valid path) instead of explaining a mistake away by cramming
    // reference frames onto slivers of audio — failure itself is a verdict
    // ("couldn't align").
    const float INF = 1e30f;
    float *cost = malloc(sizeof(float) * rn * un);
    uint8_t *bp = malloc((size_t)rn * un);
    if (!cost || !bp) { free(cost); free(bp); free(rf); return false; }
#define C(i, j) cost[(i) * un + (j)]
    for (int i = 0; i < rn * un; i++) cost[i] = INF;
    for (int i = 0; i < rn; i++) {
        int jc = (int)((int64_t)i * un / rn);
        int jlo = jc - DTW_BAND < 0 ? 0 : jc - DTW_BAND;
        int jhi = jc + DTW_BAND >= un ? un - 1 : jc + DTW_BAND;
        for (int j = jlo; j <= jhi; j++) {
            float d = fdist(&rf[i], &uf[j]);
            if (i == 0 && j == 0) { C(0, 0) = d; bp[0] = 0; continue; }
            float best = INF;
            uint8_t dir = 0;
            if (i >= 1 && j >= 1 && C(i - 1, j - 1) < best)
                { best = C(i - 1, j - 1); dir = 1; }
            if (i >= 1 && j >= 2 && C(i - 1, j - 2) + STEP_PEN < best)
                { best = C(i - 1, j - 2) + STEP_PEN; dir = 2; }
            if (i >= 2 && j >= 1 && C(i - 2, j - 1) + STEP_PEN < best)
                { best = C(i - 2, j - 1) + STEP_PEN; dir = 3; }
            if (dir) { C(i, j) = best + d; bp[i * un + j] = dir; }
        }
    }
    bool aligned = C(rn - 1, un - 1) < INF / 2;

    // Backtrack: for each ref frame, the user frame range it aligned to.
    int lo[MAX_FRAMES], hi[MAX_FRAMES];
    for (int i = 0; i < rn; i++) { lo[i] = un; hi[i] = -1; }
    if (aligned) {
        int i = rn - 1, j = un - 1;
        while (1) {
            if (j < lo[i]) lo[i] = j;
            if (j > hi[i]) hi[i] = j;
            uint8_t dir = bp[i * un + j];
            if (dir == 0) break;
            if (dir == 1) { i--; j--; }
            else if (dir == 2) { i--; j -= 2; }
            else {   // (2,1): ref frame i-1 was skipped; pin it to j
                if (j < lo[i - 1]) lo[i - 1] = j;
                if (j > hi[i - 1]) hi[i - 1] = j;
                i -= 2; j--;
            }
        }
    }

    // Alignment failed outright (take too warped for the slope limits):
    // that's information, not an error — every word is "couldn't judge".
    if (!aligned) {
        for (int w = 0; w < n_words; w++)
            out[w] = (ReciteWord){ RECITE_UNCLEAR, 98.f, 0, 0 };
        free(cost); free(bp); free(rf);
        return true;
    }

    // Score each reference word span. Pause frames in the user audio (a
    // learner breathing between words) are excluded — pausing isn't a
    // mismatch. The score blends the MEAN distance with the 90th percentile:
    // a 150ms contiguous mismatch inside an otherwise-fine word barely moves
    // the mean but shows up hard in the upper tail. A warp penalty is added
    // when the word needed a much more aggressive stretch/compression than
    // the take overall — pathological alignments reduce confidence.
    for (int w = 0; w < n_words; w++) {
        int fa = (int)(words[w].start_ms / FRAME_MS);
        int fb = (int)(words[w].end_ms / FRAME_MS);
        if (fb >= rn) fb = rn - 1;
        if (fa > fb) fa = fb;

        float dl[MAX_FRAMES];
        int cnt = 0, voiced = 0, span = 0;
        int ja = un, jb = -1;
        for (int i = fa; i <= fb; i++) {
            if (hi[i] < 0) continue;
            if (lo[i] < ja) ja = lo[i];
            if (hi[i] > jb) jb = hi[i];
            for (int j = lo[i]; j <= hi[i]; j++) {
                if (uf[j].raw_db <= usr_floor) continue;   // pause, not speech
                if (cnt < MAX_FRAMES) dl[cnt++] = fdist(&rf[i], &uf[j]);
            }
        }
        if (jb >= ja)
            for (int j = ja; j <= jb; j++, span++)
                if (uf[j].raw_db > usr_floor) voiced++;

        ReciteWord *o = &out[w];
        o->user_start_ms = (jb >= ja) ? (uint32_t)ja * FRAME_MS : 0;
        o->user_end_ms   = (jb >= ja) ? (uint32_t)(jb + 1) * FRAME_MS : 0;
        if (jb < ja) {   // word never visited by the path: can't judge it
            o->verdict = RECITE_UNCLEAR;
            o->score = 98.f;
            continue;
        }
        if (cnt == 0 || span == 0 || voiced * 3 < span) {
            // Mapped stretch is mostly SILENCE. Deliberately not a duration
            // test: connected recitation blends words; merged delivery is
            // not a skipped word.
            o->verdict = RECITE_MISSING;
            o->score = 99.f;
            continue;
        }
        // sort dl for mean + p90
        for (int i = 1; i < cnt; i++) {
            float v = dl[i]; int j = i - 1;
            while (j >= 0 && dl[j] > v) { dl[j + 1] = dl[j]; j--; }
            dl[j + 1] = v;
        }
        double mean = 0;
        for (int i = 0; i < cnt; i++) mean += dl[i];
        mean /= cnt;
        float p90 = dl[cnt * 9 / 10 >= cnt ? cnt - 1 : cnt * 9 / 10];
        float ac = 0.65f * (float)mean + 0.35f * p90;

        float ratio_l = (float)(jb - ja + 1) / (float)(fb - fa + 1);
        float ratio_g = (float)un / (float)rn;
        float wr = fabsf(log2f(ratio_l / ratio_g));
        float wp = WARP_GAIN * wr;
        if (wp > 0.9f) wp = 0.9f;

        o->score = ac + wp;
        o->verdict = RECITE_GOOD;   // graded below
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
            if (out[w].verdict == RECITE_GOOD) sc[ns++] = out[w].score;
        float base = 0;
        if (ns) {
            for (int i = 1; i < ns; i++) {   // insertion sort
                float v = sc[i]; int j = i - 1;
                while (j >= 0 && sc[j] > v) { sc[j + 1] = sc[j]; j--; }
                sc[j + 1] = v;
            }
            base = sc[ns / 4];
        }
        // Multipliers/ceilings anchored on acoustic-loopback runs with the
        // delta-MFCC engine (correct ayah = 1.2-1.9; same-voice WRONG ayah =
        // 1.8-2.7 + missing/unaligned). To be re-tuned on the labeled
        // device-take dataset — see the field-tuning notes in the repo log.
        float th_g = base * 1.50f; if (th_g < TH_GOOD) th_g = TH_GOOD;
        float th_u = base * 1.90f; if (th_u < TH_UNSURE) th_u = TH_UNSURE;
        if (th_g > 1.95f) th_g = 1.95f;
        if (th_u > 2.55f) th_u = 2.55f;
        for (int w = 0; w < n_words; w++) {
            if (out[w].verdict != RECITE_GOOD) continue;   // missing/unclear
            out[w].verdict = out[w].score <= th_g ? RECITE_GOOD
                           : out[w].score <= th_u ? RECITE_UNSURE
                                                  : RECITE_MISMATCH;
        }
    }

    free(cost); free(bp); free(rf);
    return true;
}
