// recite_eval.c — host-side batch evaluator for the Quran Teacher scorer.
//
// Replays a labeled training dataset (WAVs captured by the on-device TRAIN
// mode, or any 16-bit PCM WAV) through the exact recite_analyze() the device
// runs, against the real reference MP3 + word timings. One command re-scores
// the whole dataset after any algorithm tweak:
//
//   make recite-eval
//   ./quran-recite-eval sdcard/audio/abdulbasit/1/2.mp3 \
//                       sdcard/quran/timings/1.qtm 2 sdcard/state/train_*.wav
//
// Output: one line per word per file — filename, word, verdict, score, span —
// ready to eyeball or pipe into sort/awk for distribution stats.
#include "recite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

static int load_timings(const char *path, int ayah, WordTiming *out, int max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "QNTM", 4)) { fclose(f); return -2; }
    int n_ayat = h[8] | (h[9] << 8);
    for (int a = 0; a < n_ayat; a++) {
        unsigned char eh[4];
        if (fread(eh, 1, 4, f) != 4) break;
        int ay = eh[0] | (eh[1] << 8), nw = eh[2] | (eh[3] << 8);
        if (ay == ayah) {
            int n = nw < max ? nw : max;
            for (int w = 0; w < n; w++) {
                unsigned char b[8];
                if (fread(b, 1, 8, f) != 8) { fclose(f); return -4; }
                out[w].start_ms = b[0]|(b[1]<<8)|(b[2]<<16)|((unsigned)b[3]<<24);
                out[w].end_ms   = b[4]|(b[5]<<8)|(b[6]<<16)|((unsigned)b[7]<<24);
            }
            fclose(f);
            return n;
        }
        fseek(f, nw * 8, SEEK_CUR);
    }
    fclose(f);
    return -3;
}

// Minimal WAV reader: 16-bit PCM, any header layout with a proper data chunk.
static int16_t *load_wav(const char *path, uint32_t *out_n, uint32_t *out_hz)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fclose(f); return NULL;
    }
    uint32_t hz = 0; uint16_t ch = 1, bits = 16;
    int16_t *pcm = NULL; uint32_t n = 0;
    unsigned char ck[8];
    while (fread(ck, 1, 8, f) == 8) {
        uint32_t len = ck[4]|(ck[5]<<8)|(ck[6]<<16)|((uint32_t)ck[7]<<24);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fmt[16];
            fread(fmt, 1, len < 16 ? len : 16, f);
            if (len > 16) fseek(f, len - 16, SEEK_CUR);
            ch = fmt[2]|(fmt[3]<<8);
            hz = fmt[4]|(fmt[5]<<8)|(fmt[6]<<16)|((uint32_t)fmt[7]<<24);
            bits = fmt[14]|(fmt[15]<<8);
        } else if (!memcmp(ck, "data", 4)) {
            if (bits != 16 || ch < 1) break;
            uint32_t frames = len / 2 / ch;
            pcm = malloc(frames * 2);
            int16_t *tmp = malloc(len);
            fread(tmp, 1, len, f);
            for (uint32_t i = 0; i < frames; i++) {
                int32_t s = 0;
                for (int c = 0; c < ch; c++) s += tmp[i * ch + c];
                pcm[i] = (int16_t)(s / ch);
            }
            free(tmp);
            n = frames;
            break;
        } else {
            fseek(f, len + (len & 1), SEEK_CUR);
        }
    }
    fclose(f);
    *out_n = n; *out_hz = hz;
    return pcm;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <ref.mp3> <timings.qtm> <ayah> <take.wav...>\n", argv[0]);
        return 2;
    }
    drmp3_config cfg; drmp3_uint64 frames;
    float *fpcm = drmp3_open_file_and_read_pcm_frames_f32(argv[1], &cfg, &frames, NULL);
    if (!fpcm) { fprintf(stderr, "mp3 decode failed: %s\n", argv[1]); return 1; }
    int16_t *ref = malloc(frames * 2);
    for (drmp3_uint64 i = 0; i < frames; i++) {
        float s = 0;
        for (unsigned c = 0; c < cfg.channels; c++) s += fpcm[i * cfg.channels + c];
        s /= cfg.channels;
        if (s > 1.f) s = 1.f; else if (s < -1.f) s = -1.f;
        ref[i] = (int16_t)(s * 32767.f);
    }
    drmp3_free(fpcm, NULL);

    WordTiming wt[64];
    int nw = load_timings(argv[2], atoi(argv[3]), wt, 64);
    if (nw <= 0) { fprintf(stderr, "timings load failed (%d)\n", nw); return 1; }
    fprintf(stderr, "ref %.1fs @%uHz, %d words\n",
            (double)frames / cfg.sampleRate, cfg.sampleRate, nw);

    static const char *V[] = { "GOOD", "UNSURE", "MISMATCH", "MISSING", "UNCLEAR" };
    printf("file,word,verdict,score,user_start_ms,user_end_ms\n");
    for (int a = 4; a < argc; a++) {
        uint32_t un, uhz;
        int16_t *usr_base = load_wav(argv[a], &un, &uhz);
        int16_t *usr = usr_base;
        if (!usr || !un) { fprintf(stderr, "skip (bad wav): %s\n", argv[a]); free(usr_base); continue; }

        // Same endpointing the scene applies before analysis: trim to the
        // voiced span (peak >= 700 per 33ms chunk) with 1.5s preroll and
        // 500ms tailpad — so offline scores match on-device scores.
        {
            uint32_t chunk = uhz * 33 / 1000, va = un, vb = 0;
            for (uint32_t p = 0; p + chunk <= un; p += chunk) {
                int peak = 0;
                for (uint32_t i = 0; i < chunk; i++) {
                    int v = usr[p + i] < 0 ? -usr[p + i] : usr[p + i];
                    if (v > peak) peak = v;
                }
                if (peak >= 700) {
                    if (va == un) va = p;
                    vb = p + chunk;
                }
            }
            if (vb > 0) {
                uint32_t pre = uhz * 3 / 2, pad = uhz / 2;
                uint32_t s = va > pre ? va - pre : 0;
                uint32_t e = vb + pad < un ? vb + pad : un;
                usr += s;
                un = e - s;
                // (leaks the original pointer offset; fine for a batch tool)
            }
        }
        ReciteWord out[64];
        if (!recite_analyze(ref, (uint32_t)frames, cfg.sampleRate,
                            usr, un, uhz, wt, nw, out)) {
            fprintf(stderr, "analyze failed: %s\n", argv[a]);
            free(usr_base);
            continue;
        }
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        for (int w = 0; w < nw; w++)
            printf("%s,%d,%s,%.3f,%u,%u\n", base, w + 1, V[out[w].verdict],
                   out[w].score, out[w].user_start_ms, out[w].user_end_ms);
        free(usr_base);
    }
    free(ref);
    return 0;
}
