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

// Frame-invariant tables, built once alongside the mel bank. The Hann window
// (512 cosf) and the DCT-II basis (N_MFCC*N_MEL cosf) are constant across every
// frame, so extract_frame used to recompute ~750 cosf per frame — the single
// biggest cost on the soft-float ESP32. Baking them here turns that into table
// lookups. s_dct[c] is the basis row for cepstral coefficient c (1..N_MFCC).
static float s_hann[FFT_N];
static float s_dct[N_MFCC + 1][N_MEL];

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
    for (int k = 0; k < FFT_N; k++)
        s_hann[k] = 0.5f - 0.5f * cosf(2.f * (float)M_PI * k / (FFT_N - 1));
    for (int c = 1; c <= N_MFCC; c++)
        for (int m = 0; m < N_MEL; m++)
            s_dct[c][m] = cosf((float)M_PI * c * (m + 0.5f) / N_MEL);
    s_mel_ready = true;
}

// One frame's features (energy + static MFCCs; deltas added separately). `s`
// points at `flen` native samples; `fx` is FFT_N*2 caller-owned scratch.
static void extract_frame(const int16_t *s, uint32_t flen, uint32_t hz,
                          float *fx, Frame *out)
{
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
    out->raw_db = (float)(10.0 * log10(e + 1e-10));
    out->f[0] = out->raw_db;

    // MFCCs from a 64ms Hann-windowed slice centered in the frame, gathered at
    // 16k spacing from the native-rate samples.
    uint32_t span16 = (uint32_t)((uint64_t)flen * FEAT_HZ / hz); // ~1600
    uint32_t off16 = span16 > FFT_N ? (span16 - FFT_N) / 2 : 0;
    for (int k = 0; k < FFT_N; k++) {
        uint32_t j = (uint32_t)((uint64_t)(off16 + k) * hz / FEAT_HZ);
        if (j >= flen) j = flen - 1;
        fx[2 * k] = (s[j] / 32768.f) * s_hann[k];
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
            acc += logmel[m] * s_dct[c][m];
        out->f[c] = acc;
    }
}

// Extract per-frame features from mono s16 at any rate (decimated to 16k by
// nearest-neighbor gather — fine for 40ms-frame features). Returns count.
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
    for (int i = 0; i < nf; i++)
        extract_frame(pcm + (uint32_t)i * flen, flen, hz, fx, &out[i]);
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

    // GLOBAL tempo normalization: resample the user's frame sequence to the
    // reference's length before aligning. Field data: a fluent user recites
    // 2.5-3.5x faster than the murattal reference, which the slope-limited
    // DTW (max 2x local warp) structurally cannot align — every take came
    // back "couldn't judge". Overall pace is not an error; after removing
    // it, the slope limits govern only LOCAL deviations, as intended.
    // map[] carries each normalized frame back to its original index so the
    // reported user spans stay on the recording's real timeline.
    int map[MAX_FRAMES];
    Frame *ur = malloc(sizeof(Frame) * rn);
    if (!ur) { free(rf); return false; }
    for (int k = 0; k < rn; k++) {
        map[k] = (int)((int64_t)k * un / rn);
        ur[k] = uf[map[k]];
    }
    int un_n = rn;   // normalized user length

    // Constrained DTW: Sakoe-Chiba band around the length-scaled diagonal +
    // slope-limited steps {(1,1),(1,2),(2,1)} so the local warp stays within
    // 0.5-2x, with a small penalty on the warping steps. The aligner can now
    // FAIL (no valid path) instead of explaining a mistake away by cramming
    // reference frames onto slivers of audio — failure itself is a verdict
    // ("couldn't align").
    const float INF = 1e30f;
    float *cost = malloc(sizeof(float) * rn * un_n);
    uint8_t *bp = malloc((size_t)rn * un_n);
    if (!cost || !bp) { free(cost); free(bp); free(ur); free(rf); return false; }
#define C(i, j) cost[(i) * un_n + (j)]
    // Only the Sakoe-Chiba band is ever visited, so INF-init just the band
    // instead of the whole rn*un_n matrix (~8x fewer writes at DTW_BAND=30).
    // Fused into the compute loop: when row i is computed it reads only rows
    // i-1/i-2, already initialised on prior iterations. Those steps read one
    // cell PAST the band on each side of row i's neighbours — {(1,2)} reads
    // j-2 (left of row i-1), {(2,1)} reads j-1 in row i-2 out to its band's
    // right edge + 1 — so seed [jlo-2, jhi+1]. Those border cells stay INF and
    // stand in for the old full init's out-of-band INF wall; without them the
    // reads would hit uninitialised malloc garbage and corrupt the alignment.
    for (int i = 0; i < rn; i++) {
        int jc = i;   // tempo-normalized: the diagonal is 1:1
        int jlo = jc - DTW_BAND < 0 ? 0 : jc - DTW_BAND;
        int jhi = jc + DTW_BAND >= un_n ? un_n - 1 : jc + DTW_BAND;
        int i0 = jlo - 2 < 0 ? 0 : jlo - 2;
        int i1 = jhi + 1 > un_n - 1 ? un_n - 1 : jhi + 1;
        for (int j = i0; j <= i1; j++) C(i, j) = INF;
        for (int j = jlo; j <= jhi; j++) {
            float d = fdist(&rf[i], &ur[j]);
            if (i == 0 && j == 0) { C(0, 0) = d; bp[0] = 0; continue; }
            float best = INF;
            uint8_t dir = 0;
            if (i >= 1 && j >= 1 && C(i - 1, j - 1) < best)
                { best = C(i - 1, j - 1); dir = 1; }
            if (i >= 1 && j >= 2 && C(i - 1, j - 2) + STEP_PEN < best)
                { best = C(i - 1, j - 2) + STEP_PEN; dir = 2; }
            if (i >= 2 && j >= 1 && C(i - 2, j - 1) + STEP_PEN < best)
                { best = C(i - 2, j - 1) + STEP_PEN; dir = 3; }
            if (dir) { C(i, j) = best + d; bp[i * un_n + j] = dir; }
        }
    }
    bool aligned = C(rn - 1, un_n - 1) < INF / 2;

    // Backtrack: for each ref frame, the user frame range it aligned to.
    int lo[MAX_FRAMES], hi[MAX_FRAMES];
    for (int i = 0; i < rn; i++) { lo[i] = un_n; hi[i] = -1; }
    if (aligned) {
        int i = rn - 1, j = un_n - 1;
        while (1) {
            if (j < lo[i]) lo[i] = j;
            if (j > hi[i]) hi[i] = j;
            uint8_t dir = bp[i * un_n + j];
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
        free(cost); free(bp); free(ur); free(rf);
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
        int ja = un_n, jb = -1;
        for (int i = fa; i <= fb; i++) {
            if (hi[i] < 0) continue;
            if (lo[i] < ja) ja = lo[i];
            if (hi[i] > jb) jb = hi[i];
            for (int j = lo[i]; j <= hi[i]; j++) {
                if (ur[j].raw_db <= usr_floor) continue;   // pause, not speech
                if (cnt < MAX_FRAMES) dl[cnt++] = fdist(&rf[i], &ur[j]);
            }
        }
        if (jb >= ja)
            for (int j = ja; j <= jb; j++, span++)
                if (ur[j].raw_db > usr_floor) voiced++;

        ReciteWord *o = &out[w];
        // Spans mapped back to the recording's real timeline.
        o->user_start_ms = (jb >= ja) ? (uint32_t)map[ja] * FRAME_MS : 0;
        o->user_end_ms   = (jb >= ja) ? (uint32_t)(map[jb] + 1) * FRAME_MS : 0;
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
        float wr = fabsf(log2f(ratio_l));   // tempo-normalized: global = 1:1
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

    free(cost); free(bp); free(ur); free(rf);
    return true;
}

// ===========================================================================
// Live (streaming) score-following.
//
// The batch path above needs the whole take (global tempo resample + adaptive
// thresholds). The live follower can't wait, so it uses an ONLINE time-warp:
// each user frame advances the reference pointer 0..RL_MAXK frames (whichever
// the accumulated cost prefers), which absorbs a globally faster reciter the
// way the batch resample does — no fixed slope. A word settles a few frames
// after the cursor clears its end; a backward-jump probe re-opens a word when
// the user redoes it.
// ===========================================================================
#define RL_MAXW        128
#define RL_MAXK        4      // max ref frames advanced per user frame (~4x fast)
#define RL_BANDHW      24     // online band half-width, ref frames
#define RL_SETTLE      5      // finalize a word this many frames past its end
#define RL_HISTN       10     // recent user frames kept for the restart probe
#define RL_PROBE_EVERY 3      // run the restart probe every N frames
#define RL_LOOKBACK    200    // how far back the probe searches (ref frames)
#define RL_RESTART_GAP 12     // min backward jump to count as a redo
#define RL_RESTART_RATIO 0.55f // earlier match must be this much cheaper
#define RL_MEAN_PRIOR  8       // prior frame count seeding the user cepstral mean
#define RL_TOPK        4       // per-word largest distances kept, for a p90 proxy
#define RL_FOLLOW_DRIFT 0.85f  // reading-aid: ref frames the floor creeps / voiced frame
#define RL_INF         1e30f

struct ReciteLive {
    Frame  *rf;                 // reference frames [rn], normalized (ref mean+scale)
    int     rn;
    float   sd[N_FEAT];         // reference per-dim scale (also applied to user)
    int8_t *ref_word;           // ref frame -> word index, or -1 between words
    int     n_words;
    int     fa[RL_MAXW], fb[RL_MAXW];   // ref frame span per word

    uint32_t hz, flen;          // user rate + native samples/frame
    uint32_t consumed;          // user samples already turned into frames
    uint64_t start_samp;        // sample index of the first processed frame
    float   *fx;                // FFT scratch

    // running user cepstral-mean (voiced frames only) + energy peak for floor.
    // Seeded from the reference mean as a prior so the FIRST word normalizes
    // sensibly instead of cold-starting from zero (which flagged word 1).
    float    refmu[N_FEAT];
    double   umu[N_FEAT];
    long     umu_n;
    float    umax_db;
    bool     have_peak;

    // streaming delta pipeline (centered d/dd => 2-frame lag)
    Frame    st[3]; int stn;    // static-only ring
    Frame    dl[3]; int dln;    // static+delta ring

    // online DTW frontier
    float   *g;                 // [rn] accumulated cost (band valid, else INF)
    int      jcur;
    bool     started;
    int      uframe;

    // reading-aid FOLLOW mode: a monotonic floor that creeps forward with
    // voiced time so a weak acoustic match can't stall the cursor (grading is
    // off in this mode; we only care where the reader is). The floor's PACE is
    // learned from the acoustic cursor's own advance (ref frames per voiced
    // frame) so it keeps up with THIS reader instead of a fixed slow guess.
    bool     follow;
    float    jfloor_f;
    float    pace;          // ref frames advanced per voiced frame (learned)
    int      acoustic_prev; // last frame's acoustic argmin, for the pace estimate

    Frame    hist[RL_HISTN]; int histn;   // recent normalized user frames (ring)

    // per-word live accumulators. wtop = the RL_TOPK largest frame distances
    // (descending), a streaming stand-in for the batch grader's p90 — using the
    // single worst frame instead made one transient poison the whole word.
    double   wsum[RL_MAXW]; float wtop[RL_MAXW][RL_TOPK];
    int      wcnt[RL_MAXW], wvis[RL_MAXW];
    uint32_t wu_lo[RL_MAXW], wu_hi[RL_MAXW];
    float    wscore[RL_MAXW];
    ReciteVerdict verdict[RL_MAXW];
    bool     finalized[RL_MAXW];

    float    basebuf[RL_MAXW]; int nbase;   // finalized scores -> adaptive base
};

ReciteLive *recite_live_create(void)
{
    ReciteLive *rl = calloc(1, sizeof(*rl));
    if (!rl) return NULL;
    rl->fx = malloc(sizeof(float) * FFT_N * 2);
    if (!rl->fx) { free(rl); return NULL; }
    return rl;
}

void recite_live_destroy(ReciteLive *rl)
{
    if (!rl) return;
    free(rl->rf); free(rl->g); free(rl->ref_word); free(rl->fx); free(rl);
}

bool recite_live_begin(ReciteLive *rl, const int16_t *ref, uint32_t ref_n,
                       uint32_t ref_hz, const WordTiming *words, int n_words)
{
    if (!rl || !ref || !ref_n || n_words <= 0) return false;
    if (n_words > RL_MAXW) n_words = RL_MAXW;

    free(rl->rf); free(rl->g); free(rl->ref_word);
    rl->rf = malloc(sizeof(Frame) * MAX_FRAMES);
    rl->g  = malloc(sizeof(float) * MAX_FRAMES);
    if (!rl->rf || !rl->g) { free(rl->rf); free(rl->g); rl->rf = NULL; rl->g = NULL; return false; }

    int rn = extract(ref, ref_n, ref_hz, rl->rf);
    if (rn < 2) { free(rl->rf); free(rl->g); rl->rf = NULL; rl->g = NULL; return false; }
    rl->rn = rn;
    rl->ref_word = malloc(rn);
    if (!rl->ref_word) { free(rl->rf); free(rl->g); rl->rf = NULL; rl->g = NULL; return false; }

    // Normalize the reference exactly as the batch path does: own mean + own
    // scale over voiced frames; keep the scale to apply to the user side too.
    float ref_floor = percentile_db(rl->rf, rn, 5) + 8.f;
    float ref_cap   = percentile_db(rl->rf, rn, 95) - 12.f;
    if (ref_floor > ref_cap) ref_floor = ref_cap;
    add_deltas(rl->rf, rn);
    float mu[N_FEAT];
    cmn_stats(rl->rf, rn, ref_floor, mu, rl->sd);
    cmn_apply(rl->rf, rn, mu, rl->sd);
    for (int d = 0; d < N_FEAT; d++) rl->refmu[d] = mu[d];

    s_wsum = 0;
    for (int d = 0; d < N_FEAT; d++) s_wsum += feat_w(d);

    rl->n_words = n_words;
    for (int j = 0; j < rn; j++) rl->ref_word[j] = -1;
    for (int w = 0; w < n_words; w++) {
        int fa = (int)(words[w].start_ms / FRAME_MS);
        int fb = (int)(words[w].end_ms / FRAME_MS);
        if (fb >= rn) fb = rn - 1;
        if (fa < 0) fa = 0;
        if (fa > fb) fa = fb;
        rl->fa[w] = fa; rl->fb[w] = fb;
        for (int j = fa; j <= fb; j++) if (rl->ref_word[j] < 0) rl->ref_word[j] = (int8_t)w;
    }
    recite_live_reset(rl);   // clear per-word + streaming state
    return true;
}

// Reading-aid mode: forward-only cursor that follows the reader's voice and
// can't stall (no grading, no backward restart). Off = the grading follower.
void recite_live_set_follow(ReciteLive *rl, bool on) { if (rl) rl->follow = on; }

// Re-arm for another take of the SAME ayah without re-extracting the reference.
void recite_live_reset(ReciteLive *rl)
{
    if (!rl || !rl->rf) return;
    for (int w = 0; w < rl->n_words; w++) {
        rl->verdict[w] = RECITE_UNCLEAR;
        rl->finalized[w] = false;
        rl->wsum[w] = 0; rl->wcnt[w] = 0; rl->wvis[w] = 0;
        for (int t = 0; t < RL_TOPK; t++) rl->wtop[w][t] = 0;
        rl->wu_lo[w] = 0; rl->wu_hi[w] = 0; rl->wscore[w] = 0;
    }
    rl->consumed = 0; rl->start_samp = 0; rl->umax_db = -120.f;
    rl->have_peak = false; rl->stn = 0; rl->dln = 0; rl->jcur = 0;
    rl->started = false; rl->uframe = 0; rl->histn = 0; rl->nbase = 0;
    rl->hz = 0; rl->flen = 0; rl->jfloor_f = 0;
    rl->pace = 1.2f; rl->acoustic_prev = 0;   // slightly faster than the ref to start
    // Prior: pretend we've already seen RL_MEAN_PRIOR frames at the reference
    // mean, so word 1 isn't normalized against a half-empty running mean.
    rl->umu_n = RL_MEAN_PRIOR;
    for (int d = 0; d < N_FEAT; d++) rl->umu[d] = (double)rl->refmu[d] * RL_MEAN_PRIOR;
}

// Insert a frame distance into a per-word top-K (descending), dropping the
// smallest — a cheap running estimate of the word's high-percentile frames.
static void rl_top_insert(float *top, float d)
{
    if (d <= top[RL_TOPK - 1]) return;
    int i = RL_TOPK - 1;
    while (i > 0 && top[i - 1] < d) { top[i] = top[i - 1]; i--; }
    top[i] = d;
}

// Finalize one word from its accumulated distances.
static void rl_settle_word(ReciteLive *rl, int w)
{
    if (rl->finalized[w]) return;
    rl->finalized[w] = true;
    // Mapped stretch mostly silence / never voiced => the word wasn't recited.
    if (rl->wcnt[w] == 0 || rl->wcnt[w] * 3 < rl->wvis[w]) {
        rl->verdict[w] = RECITE_MISSING;
        rl->wscore[w] = 99.f;
        return;
    }
    float mean = (float)(rl->wsum[w] / rl->wcnt[w]);
    // p90 proxy: the (cnt/10)-th LARGEST frame distance, from the top-K. Using
    // the single worst frame here made one transient poison every word (=> all
    // stuck at "unsure"); this matches the batch grader's mean+p90 blend.
    int r = rl->wcnt[w] / 10; if (r >= RL_TOPK) r = RL_TOPK - 1;
    float p90 = rl->wtop[w][r];
    float ac = 0.65f * mean + 0.35f * p90;
    rl->wscore[w] = ac;

    // Adaptive base = lower-quartile of settled scores so far. Include THIS
    // word (>=1), so the very first words grade against a real bar instead of
    // the harsh absolute floor — the streaming stand-in for the batch grader
    // seeing the whole take at once.
    if (ac < 3.f && rl->nbase < RL_MAXW) rl->basebuf[rl->nbase++] = ac;
    float base = 0;
    if (rl->nbase >= 1) {
        float tmp[RL_MAXW];
        for (int i = 0; i < rl->nbase; i++) tmp[i] = rl->basebuf[i];
        for (int i = 1; i < rl->nbase; i++) {
            float v = tmp[i]; int j = i - 1;
            while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
            tmp[j + 1] = v;
        }
        base = tmp[rl->nbase / 4];
    }
    float th_g = base * 1.50f; if (th_g < TH_GOOD) th_g = TH_GOOD;
    float th_u = base * 1.90f; if (th_u < TH_UNSURE) th_u = TH_UNSURE;
    if (th_g > 1.95f) th_g = 1.95f;
    if (th_u > 2.55f) th_u = 2.55f;
    rl->verdict[w] = ac <= th_g ? RECITE_GOOD
                   : ac <= th_u ? RECITE_UNSURE
                                : RECITE_MISMATCH;
}

// Re-open the words whose reference span lies in (from_j, to_j] — the stretch
// the user is redoing — so they grey out and re-score from the new audio.
static void rl_reopen_after(ReciteLive *rl, int from_j)
{
    for (int w = 0; w < rl->n_words; w++) {
        if (rl->fa[w] <= from_j) continue;
        rl->finalized[w] = false;
        rl->verdict[w] = RECITE_UNCLEAR;
        rl->wsum[w] = 0; rl->wcnt[w] = 0; rl->wvis[w] = 0;
        for (int t = 0; t < RL_TOPK; t++) rl->wtop[w][t] = 0;
        rl->wu_lo[w] = 0; rl->wu_hi[w] = 0;
    }
}

// One fully-featured (normalized) user frame -> advance the follower.
static void rl_step(ReciteLive *rl, const Frame *N, bool voiced, uint32_t ms)
{
    int rn = rl->rn;
    if (!rl->started) {
        for (int j = 0; j < rn; j++) rl->g[j] = RL_INF;
        rl->g[0] = 0; rl->jcur = 0; rl->started = true;
    }

    int lo = rl->jcur - RL_BANDHW; if (lo < 0) lo = 0;
    int hi = rl->jcur + RL_BANDHW + RL_MAXK; if (hi > rn - 1) hi = rn - 1;

    // gnew[j] = d(j) + min_{k=0..MAXK} g[j-k] (+ small penalty for big jumps).
    static float gnew[MAX_FRAMES], drow[MAX_FRAMES];
    float gmin = RL_INF; int jbest = rl->jcur;
    for (int j = lo; j <= hi; j++) {
        float d = fdist(&rl->rf[j], N);
        drow[j] = d;
        float best = RL_INF;
        for (int k = 0; k <= RL_MAXK; k++) {
            int p = j - k; if (p < 0) break;
            float c = rl->g[p]; if (c >= RL_INF) continue;
            if (k >= 2) c += STEP_PEN * (k - 1);   // discourage huge skips
            if (c < best) best = c;
        }
        if (best >= RL_INF) { gnew[j] = RL_INF; continue; }
        gnew[j] = best + d;
        if (gnew[j] < gmin) { gmin = gnew[j]; jbest = j; }
    }
    // Commit, normalizing so the running cost can't drift to infinity.
    for (int j = 0; j < rn; j++) rl->g[j] = RL_INF;
    for (int j = lo; j <= hi; j++)
        if (gnew[j] < RL_INF) rl->g[j] = gnew[j] - gmin;

    // Reading-aid follow: a monotonic floor creeps forward with voiced time so
    // a weak acoustic match can't stall the cursor, and it never backs up. When
    // the floor gets ahead of the acoustic best, re-anchor the DTW there so it
    // keeps matching from the reader's real position.
    if (rl->follow) {
        int amin = jbest;   // acoustic argmin this frame (before flooring)
        if (voiced) {
            // Learn the reader's pace from how fast the acoustic match advances,
            // then creep the floor at THAT pace so weak stretches keep up.
            int dp = amin - rl->acoustic_prev;
            if (dp >= 0 && dp <= RL_MAXK)
                rl->pace += ((float)dp - rl->pace) * 0.06f;
            if (rl->pace < 0.6f) rl->pace = 0.6f;
            if (rl->pace > 3.0f) rl->pace = 3.0f;
            if (rl->jfloor_f < (float)(rn - 1)) rl->jfloor_f += rl->pace;
        }
        rl->acoustic_prev = amin;
        int floor = (int)rl->jfloor_f; if (floor > rn - 1) floor = rn - 1;
        if (jbest < floor) jbest = floor;
        if (jbest < rl->jcur) jbest = rl->jcur;   // monotonic
        if (jbest >= rn) jbest = rn - 1;
        if (gnew[jbest] >= RL_INF || rl->g[jbest] >= RL_INF) {
            for (int j = 0; j < rn; j++) rl->g[j] = RL_INF;
            rl->g[jbest] = 0;   // re-anchor the frontier at the reader's spot
        }
    }
    rl->jcur = jbest;

    // Attribute this frame's distance to the word under the cursor.
    int w = rl->ref_word[jbest];
    if (w >= 0) {
        rl->wvis[w]++;
        if (rl->wu_lo[w] == 0 && rl->wu_hi[w] == 0) rl->wu_lo[w] = ms;
        rl->wu_hi[w] = ms + FRAME_MS;
        if (voiced) {
            float d = drow[jbest];
            rl->wsum[w] += d; rl->wcnt[w]++;
            rl_top_insert(rl->wtop[w], d);
        }
    }

    // Settle words the cursor has cleared (with a small confirmation margin).
    for (int wi = 0; wi < rl->n_words; wi++)
        if (!rl->finalized[wi] && rl->jcur > rl->fb[wi] + RL_SETTLE)
            rl_settle_word(rl, wi);

    // Restart probe: does the recent window match a point well BEHIND us?
    // (Disabled in reading-aid follow mode — there the cursor is monotonic.)
    rl->hist[rl->histn % RL_HISTN] = *N;
    rl->histn++;
    if (!rl->follow && rl->histn >= RL_HISTN && (rl->uframe % RL_PROBE_EVERY) == 0 &&
        rl->jcur > RL_RESTART_GAP + RL_HISTN) {
        // hist as a contiguous [oldest..newest] window.
        Frame win[RL_HISTN];
        for (int m = 0; m < RL_HISTN; m++)
            win[m] = rl->hist[(rl->histn - RL_HISTN + m) % RL_HISTN];
        float cur_c = 0;   // cost of the window ending at the cursor
        for (int m = 0; m < RL_HISTN; m++) {
            int rj = rl->jcur - (RL_HISTN - 1) + m;
            if (rj < 0) rj = 0;
            cur_c += fdist(&rl->rf[rj], &win[m]);
        }
        int plo = rl->jcur - RL_LOOKBACK; if (plo < 0) plo = 0;
        int phi = rl->jcur - RL_RESTART_GAP - RL_HISTN;
        float best_c = RL_INF; int best_p = -1;
        for (int p = plo; p <= phi; p++) {
            float c = 0;
            for (int m = 0; m < RL_HISTN; m++) c += fdist(&rl->rf[p + m], &win[m]);
            if (c < best_c) { best_c = c; best_p = p; }
        }
        if (best_p >= 0 && best_c < cur_c * RL_RESTART_RATIO &&
            best_c / RL_HISTN < TH_UNSURE) {
            int newj = best_p + RL_HISTN - 1;
            rl_reopen_after(rl, newj);
            for (int j = 0; j < rn; j++) rl->g[j] = RL_INF;
            rl->g[newj] = 0; rl->jcur = newj;
        }
    }
    rl->uframe++;
}

// Push one static frame through the centered-delta pipeline; emit finished
// (fully-featured) frames to rl_step. 2-frame lag by construction.
static void rl_push_static(ReciteLive *rl, const Frame *cur, bool voiced, uint32_t ms)
{
    // shift static ring
    if (rl->stn < 3) rl->st[rl->stn++] = *cur;
    else { rl->st[0] = rl->st[1]; rl->st[1] = rl->st[2]; rl->st[2] = *cur; }
    if (rl->stn < 3) return;

    // centered delta of the middle static frame
    Frame df = rl->st[1];
    for (int c = 1; c <= N_MFCC; c++)
        df.f[N_MFCC + c] = (rl->st[2].f[c] - rl->st[0].f[c]) * 0.5f;

    if (rl->dln < 3) rl->dl[rl->dln++] = df;
    else { rl->dl[0] = rl->dl[1]; rl->dl[1] = rl->dl[2]; rl->dl[2] = df; }
    if (rl->dln < 3) return;

    // centered delta-delta of the middle delta frame => fully featured
    Frame full = rl->dl[1];
    for (int c = 1; c <= N_MFCC; c++)
        full.f[2 * N_MFCC + c] =
            (rl->dl[2].f[N_MFCC + c] - rl->dl[0].f[N_MFCC + c]) * 0.5f;

    // Running user cepstral mean over voiced frames, ref scale; then normalize.
    // (voiced/ms belong to `full`, which is 2 frames behind `cur` — carried
    // through the rings below.)
    bool fvoiced = full.f[0] > (rl->umax_db - 22.f);
    if (fvoiced) { for (int d = 0; d < N_FEAT; d++) rl->umu[d] += full.f[d]; rl->umu_n++; }
    Frame nrm = full;
    for (int d = 0; d < N_FEAT; d++) {
        float mu = rl->umu_n ? (float)(rl->umu[d] / rl->umu_n) : 0.f;
        float z = (full.f[d] - mu) / rl->sd[d];
        if (z > 5.f) z = 5.f; else if (z < -5.f) z = -5.f;
        nrm.f[d] = z;
    }
    // We don't have the exact per-frame ms of `full` here (it lags `cur` by 2
    // frames); approximate from the follower's own frame counter, which is
    // plenty for replay spans.
    uint32_t fms = (uint32_t)((rl->start_samp + (uint64_t)rl->uframe * rl->flen)
                              * 1000ull / rl->hz);
    (void)voiced; (void)ms;
    rl_step(rl, &nrm, fvoiced, fms);
}

void recite_live_feed(ReciteLive *rl, const int16_t *pcm, uint32_t total_n,
                      uint32_t hz)
{
    if (!rl || !rl->rf || !pcm || hz == 0) return;
    if (rl->flen == 0) {
        rl->hz = hz;
        rl->flen = hz * FRAME_MS / 1000;
        rl->start_samp = 0;
    }
    if (rl->flen == 0) return;
    while (rl->consumed + rl->flen <= total_n) {
        const int16_t *s = pcm + rl->consumed;
        Frame fr;
        extract_frame(s, rl->flen, hz, rl->fx, &fr);
        if (fr.raw_db > rl->umax_db) rl->umax_db = fr.raw_db;
        uint32_t ms = (uint32_t)((uint64_t)rl->consumed * 1000ull / hz);
        rl_push_static(rl, &fr, fr.raw_db > (rl->umax_db - 22.f), ms);
        rl->consumed += rl->flen;
    }
}

void recite_live_finish(ReciteLive *rl)
{
    if (!rl || !rl->rf) return;
    for (int w = 0; w < rl->n_words; w++)
        if (!rl->finalized[w]) rl_settle_word(rl, w);
}

ReciteVerdict recite_live_verdict(const ReciteLive *rl, int word)
{
    if (!rl || word < 0 || word >= rl->n_words) return RECITE_UNCLEAR;
    return rl->verdict[word];
}

float recite_live_score(const ReciteLive *rl, int word)
{
    if (!rl || word < 0 || word >= rl->n_words) return 0.f;
    return rl->wscore[word];
}

int recite_live_cursor_word(const ReciteLive *rl)
{
    if (!rl || !rl->started) return -1;
    int j = rl->jcur;
    if (j < 0 || j >= rl->rn) return -1;
    return rl->ref_word[j];
}

bool recite_live_word_span(const ReciteLive *rl, int word,
                           uint32_t *start_ms, uint32_t *end_ms)
{
    if (!rl || word < 0 || word >= rl->n_words) return false;
    if (rl->wu_hi[word] <= rl->wu_lo[word]) return false;
    if (start_ms) *start_ms = rl->wu_lo[word];
    if (end_ms)   *end_ms   = rl->wu_hi[word];
    return true;
}
