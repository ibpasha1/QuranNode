#include "recite.h"
#include "plat.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// 100ms frames: coarse enough that a full DTW matrix for a ~20s ayah is small
// (~200x300 floats), fine enough to resolve word spans (words run 300ms+).
#define FRAME_MS   100
#define MAX_FRAMES 600      // 60s cap per side

// Features: log-energy + ZCR + 12 MFCCs. The cepstral coefficients capture
// the vowel/consonant CONTENT of each frame, which is what separates wrong
// words recited with the right rhythm from the real thing — the hand-rolled
// envelope features that preceded them could not (field-proven: deliberate
// gibberish with matching cadence scored as genuine). The per-utterance
// z-norm below doubles as cepstral mean/variance normalization, cancelling
// mic/channel coloring and voice-brightness differences.
#define N_MFCC     12
#define N_FEAT     (2 + N_MFCC)
#define FFT_N      1024     // 64ms window @16k, centered in each frame
#define N_MEL      20
#define MEL_LO_HZ  100.f
#define MEL_HI_HZ  7000.f
#define FEAT_HZ    16000    // analysis domain; other rates are decimated in

// Feature distance thresholds on z-normalized features (empirical; identical
// audio scores ~0). Deliberately generous — V1 flags, it doesn't grade.
#define TH_GOOD    1.0f
#define TH_UNSURE  1.8f

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
        out[i].raw_db = (float)(10.0 * log10(e + 1e-10));
        out[i].f[0] = out[i].raw_db;
        out[i].f[1] = (float)zc * (float)FEAT_HZ / (float)flen / (float)hz;

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
            out[i].f[1 + c] = acc;
        }
    }
    free(fx);
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

// Distance normalized by dimension count so scores stay in the same ballpark
// as before (~0 identical, ~1-2 matched cross-voice, higher for divergence)
// regardless of the feature-vector width.
static float fdist(const Frame *a, const Frame *b)
{
    float s = 0;
    for (int d = 0; d < N_FEAT; d++) {
        float v = a->f[d] - b->f[d];
        s += v * v;
    }
    return sqrtf(s * (3.f / N_FEAT));
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
    znorm(rf, rn, ref_floor);
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
        // Missing = the mapped stretch is mostly SILENCE. Deliberately not a
        // duration test: connected recitation ("alhamdu-lillahi") blends
        // words, and a fast take squeezes some words to slivers of voiced
        // audio — that's merged delivery, not a skipped word. Flagging it
        // "not heard" was a field bug; V1 prefers missing a real skip over
        // flagging correct recitation.
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
        // Ceilings calibrated on real device takes (Sep 2026 field logs):
        // genuine words score <=1.9 against the reference; deliberate
        // gibberish mostly >=2.0. Keeping the ceiling near that line flags
        // most wrong-content words while genuine recitation stays green —
        // full wrong-word detection still needs the V2 phoneme model.
        float th_g = base * 1.30f; if (th_g < TH_GOOD) th_g = TH_GOOD;
        float th_u = base * 1.70f; if (th_u < TH_UNSURE) th_u = TH_UNSURE;
        if (th_g > 1.95f) th_g = 1.95f;
        if (th_u > 2.55f) th_u = 2.55f;
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
