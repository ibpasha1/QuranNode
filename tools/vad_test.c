// vad_test.c — pins the mic endpointer that ran for months with no coverage.
//
// The auto-start/auto-stop constants (VOICE_PEAK, preroll, tailpad, AUTO_STOP,
// NO_VOICE, warmup) came from INMP441 field recordings and lived inline in
// scene_teacher.c. This drives voice_activity with synthetic chunks and asserts
// the exact endpoint decisions, so the extraction (and any future tweak) can't
// silently regress the teacher — or the drill that now shares it.
#include "voice_activity.h"
#include <stdio.h>
#include <string.h>

#define HZ 16000u
static int16_t buf[1u << 20];   // 1M samples — plenty for any test

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// Place `count` samples of amplitude `amp` at buf[*n], then run one va_feed.
static VaResult feed(VoiceActivity *va, uint32_t cap, uint32_t *n, int amp, int count)
{
    for (int i = 0; i < count; i++) buf[*n + i] = (int16_t)amp;
    return va_feed(va, buf, cap, n, count);
}

static uint32_t ms_samples(uint32_t ms) { return HZ * ms / 1000; }

int main(void)
{
    printf("-- warmup drops the mic-start transient (no false voice) --\n");
    {
        VoiceActivity va; va_init(&va, HZ);
        uint32_t n = 0;
        uint32_t warm = ms_samples(VA_WARMUP_MS);   // 4000 @16k
        VaResult r = feed(&va, 1u << 20, &n, 5000, (int)warm);  // loud, all warmup
        CHECK(r == VA_RUNNING, "warmup chunk not running");
        CHECK(!va.heard, "warmup transient counted as voice");
        CHECK(n == 0, "warmup samples not dropped (n=%u)", n);
        // now past warmup: a loud chunk registers
        r = feed(&va, 1u << 20, &n, 5000, 1000);
        CHECK(va.heard, "voice after warmup not heard");
        CHECK(n == 1000, "n=%u want 1000", n);
        CHECK(va.voice_a == 0, "voice_a=%u want 0", va.voice_a);
        CHECK(va.voice_b == 1000, "voice_b=%u want 1000", va.voice_b);
    }

    printf("-- voiced then a beat of silence -> DONE_SILENCE --\n");
    {
        VoiceActivity va; va_init(&va, HZ);
        uint32_t n = 0;
        feed(&va, 1u << 20, &n, 0, (int)ms_samples(VA_WARMUP_MS));  // clear warmup
        VaResult r = feed(&va, 1u << 20, &n, 5000, (int)ms_samples(500));  // voice
        CHECK(r == VA_RUNNING && va.heard, "voice chunk state wrong");
        uint32_t vb = va.voice_b;
        // 500ms silence chunks until AUTO_STOP (2200ms) is crossed
        int chunks = 0;
        do { r = feed(&va, 1u << 20, &n, 50, (int)ms_samples(500)); chunks++; }
        while (r == VA_RUNNING && chunks < 20);
        CHECK(r == VA_DONE_SILENCE, "did not auto-stop on silence (r=%d)", r);
        CHECK(chunks == 5, "auto-stop after %d x500ms, want 5 (>=2200ms)", chunks);
        CHECK(va.voice_b == vb, "voice_b moved during silence");

        uint32_t a, b;
        va_span(&va, n, &a, &b);
        CHECK(a == 0, "span a=%u want 0", a);
        CHECK(b == vb + ms_samples(VA_TAILPAD_MS), "span b=%u want %u",
              b, vb + ms_samples(VA_TAILPAD_MS));
    }

    printf("-- never any voice -> DONE_NOVOICE after NO_VOICE_MS --\n");
    {
        VoiceActivity va; va_init(&va, HZ);
        uint32_t n = 0;
        feed(&va, 1u << 20, &n, 0, (int)ms_samples(VA_WARMUP_MS));
        VaResult r = VA_RUNNING;
        int chunks = 0;
        do { r = feed(&va, 1u << 20, &n, 50, (int)ms_samples(500)); chunks++; }
        while (r == VA_RUNNING && chunks < 40);
        CHECK(r == VA_DONE_NOVOICE, "did not give up (r=%d)", r);
        CHECK(!va.heard, "heard set on a silent take");
        CHECK(chunks == 24, "gave up after %d x500ms, want 24 (12000ms)", chunks);
        // (No span assertion here: a no-voice take is discarded, never analysed,
        // so va_span's value in this case is a faithfully-preserved don't-care.)
    }

    printf("-- buffer fills first -> DONE_FULL --\n");
    {
        VoiceActivity va; va_init(&va, HZ);
        uint32_t n = 0, cap = 10000;
        feed(&va, cap, &n, 0, (int)ms_samples(VA_WARMUP_MS));  // n stays 0
        feed(&va, cap, &n, 5000, 8000);                        // n=8000
        VaResult r = feed(&va, cap, &n, 5000, 8000);           // n=16000 >= cap
        CHECK(r == VA_DONE_FULL, "did not report full (r=%d)", r);
        CHECK(n >= cap, "n=%u < cap=%u", n, cap);
    }

    printf("-- preroll: voice_a is first-voice minus PREROLL, clamped --\n");
    {
        VoiceActivity va; va_init(&va, HZ);
        uint32_t n = 0;
        feed(&va, 1u << 20, &n, 0, (int)ms_samples(VA_WARMUP_MS));
        // ~1.9s of sub-threshold audio (below NO_VOICE), pushing first voice deep
        uint32_t quiet = 30000;                    // < NO_VOICE (192000 samples)
        feed(&va, 1u << 20, &n, 100, (int)quiet);  // amp 100 < VOICE_PEAK
        CHECK(!va.heard && n == quiet, "quiet lead-in mishandled (heard=%d n=%u)",
              va.heard, n);
        feed(&va, 1u << 20, &n, 5000, 1000);       // first real voice at n=30000
        uint32_t preroll = ms_samples(VA_PREROLL_MS);   // 24000
        CHECK(va.voice_a == quiet - preroll, "voice_a=%u want %u",
              va.voice_a, quiet - preroll);
    }

    printf("-- va_wav_header writes a valid 16k mono s16 header --\n");
    {
        uint8_t d[44];
        va_wav_header(d, 1000, HZ);
        CHECK(memcmp(d, "RIFF", 4) == 0, "no RIFF");
        CHECK(memcmp(d + 8, "WAVE", 4) == 0, "no WAVE");
        CHECK(memcmp(d + 36, "data", 4) == 0, "no data");
        uint32_t rate; memcpy(&rate, d + 24, 4);
        CHECK(rate == HZ, "sample rate %u want %u", rate, HZ);
        uint32_t dlen; memcpy(&dlen, d + 40, 4);
        CHECK(dlen == 1000, "data len %u want 1000", dlen);
    }

    if (fails) { printf("\nvad-test: %d checks FAILED\n", fails); return 1; }
    printf("\nvad-test: all checks passed\n");
    return 0;
}
