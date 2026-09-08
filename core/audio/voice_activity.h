// voice_activity.h — the shared mic endpointer (+ WAV header helper).
//
// Lifted VERBATIM out of scene_teacher.c so the Quran Teacher and the hifz drill
// share ONE copy of the recite auto-start/auto-stop logic. Every constant below
// was tuned against INMP441 field recordings and, until this extraction, had
// ZERO test coverage — see tools/vad_test.c, which now pins the behaviour.
//
// The endpointer lets a learner just recite and pause: capture auto-starts, and
// finishes on its own once voice is followed by a beat of silence. It trims the
// analysed span to [voice_a, voice_b + tailpad) so the silent lead-in (drawing
// breath) can't align to the first word and score it "not heard" — the original
// V1 field bug the preroll/tailpad exist to prevent.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Endpointer constants (field-derived; do not "tidy" without re-recording).
#define VA_VOICE_PEAK    700     // s16 peak that counts as voice (~0.021 fs);
                                 // low on purpose — quiet mics/AGC. The scorer
                                 // has its own noise-relative floor.
#define VA_PREROLL_MS   1500     // keep this much lead-in before first voice:
                                 // mic AGC ramps over ~1s, so word 1 may sit
                                 // below VOICE_PEAK; a trimmed-off first word
                                 // can only ever score "not heard".
#define VA_TAILPAD_MS    500     // keep this much after last voice (analysis).
#define VA_AUTO_STOP_MS 2200     // silence after voice = done. Generous: learners
                                 // pause mid-ayah; don't cut them off.
#define VA_NO_VOICE_MS 12000     // never heard anything at all = give up.
#define VA_WARMUP_MS     250     // discard the I2S DC-settle transient the
                                 // INMP441 emits right after the channel starts
                                 // (it trips VOICE_PEAK at t=0 otherwise).

typedef enum {
    VA_RUNNING = 0,
    VA_DONE_SILENCE,   // voiced, then VA_AUTO_STOP_MS of silence — the normal end
    VA_DONE_NOVOICE,   // VA_NO_VOICE_MS elapsed with nothing heard
    VA_DONE_FULL,      // the capture buffer filled first
} VaResult;

typedef struct {
    uint32_t hz;
    uint32_t warmup_left;        // samples still to discard at the start
    bool     heard;              // any voice yet this take
    uint32_t voice_a, voice_b;   // first/last voiced sample bounds
    uint32_t sil_ms;             // silence since the last voiced chunk
    uint32_t wait_ms;            // total time waiting with no voice at all
    float    peak;               // last chunk's peak, 0..1 (drives the UI meter)
} VoiceActivity;

// Reset for a fresh take at sample rate `hz`.
void va_init(VoiceActivity *va, uint32_t hz);

// Feed `got` samples that hal_mic_read just wrote at buf[*n]. Applies the warmup
// drop IN PLACE (memmove within buf), updates the endpoint state, advances *n by
// the kept sample count, and returns whether the take is still running or why it
// finished. `cap` is the buffer capacity (a full buffer ends the take).
VaResult va_feed(VoiceActivity *va, int16_t *buf, uint32_t cap, uint32_t *n, int got);

// The span to analyse once done: [*a, *b) = [voice_a, voice_b + tailpad], clamped
// to [0, n). Falls back to the whole take if no voice was ever marked.
void va_span(const VoiceActivity *va, uint32_t n, uint32_t *a, uint32_t *b);

// 44-byte canonical WAV header for `bytes` of 16-bit mono PCM at `hz`.
void va_wav_header(uint8_t *d, uint32_t bytes, uint32_t hz);
