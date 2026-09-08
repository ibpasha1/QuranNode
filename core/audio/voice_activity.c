#include "voice_activity.h"
#include <string.h>

void va_init(VoiceActivity *va, uint32_t hz)
{
    memset(va, 0, sizeof(*va));
    va->hz = hz;
    va->warmup_left = hz * VA_WARMUP_MS / 1000;
}

VaResult va_feed(VoiceActivity *va, int16_t *buf, uint32_t cap, uint32_t *n, int got)
{
    if (got <= 0) return VA_RUNNING;

    // Drop the mic-start transient: discard the first VA_WARMUP_MS of samples by
    // shifting the rest down over them (they were written at buf[*n]).
    if (va->warmup_left > 0) {
        uint32_t drop = (uint32_t)got < va->warmup_left ? (uint32_t)got : va->warmup_left;
        va->warmup_left -= drop;
        if ((uint32_t)got > drop)
            memmove(buf + *n, buf + *n + drop,
                    ((uint32_t)got - drop) * sizeof(int16_t));
        got -= (int)drop;
    }
    if (got <= 0) return VA_RUNNING;

    // Peak of this chunk drives both the meter and the endpointer.
    int peak = 0;
    for (int i = 0; i < got; i++) {
        int v = buf[*n + i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    va->peak = (float)peak / 32768.f;

    uint32_t chunk_ms = (uint32_t)got * 1000u / va->hz;
    if (peak >= VA_VOICE_PEAK) {
        if (!va->heard) {
            va->heard = true;
            uint32_t preroll = va->hz * VA_PREROLL_MS / 1000;
            va->voice_a = *n > preroll ? *n - preroll : 0;
        }
        va->voice_b = *n + (uint32_t)got;
        va->sil_ms = 0;
    } else if (va->heard) {
        va->sil_ms += chunk_ms;
    } else {
        va->wait_ms += chunk_ms;
    }

    *n += (uint32_t)got;

    // Recited then paused -> done; never spoke at all -> give up; buffer full.
    if (va->heard && va->sil_ms >= VA_AUTO_STOP_MS) return VA_DONE_SILENCE;
    if (!va->heard && va->wait_ms >= VA_NO_VOICE_MS) return VA_DONE_NOVOICE;
    if (*n >= cap) return VA_DONE_FULL;
    return VA_RUNNING;
}

void va_span(const VoiceActivity *va, uint32_t n, uint32_t *a, uint32_t *b)
{
    uint32_t tail = va->hz * VA_TAILPAD_MS / 1000;
    uint32_t aa = va->voice_a, bb = va->voice_b + tail;
    if (bb > n) bb = n;
    if (aa >= bb) { aa = 0; bb = n; }   // no voice marked -> analyse the lot
    *a = aa;
    *b = bb;
}

void va_wav_header(uint8_t *d, uint32_t bytes, uint32_t hz)
{
    memcpy(d, "RIFF", 4);
    uint32_t v = 36 + bytes;           memcpy(d + 4, &v, 4);
    memcpy(d + 8, "WAVEfmt ", 8);
    v = 16;                            memcpy(d + 16, &v, 4);
    uint16_t h = 1;                    memcpy(d + 20, &h, 2);   // PCM
    h = 1;                             memcpy(d + 22, &h, 2);   // mono
    v = hz;                            memcpy(d + 24, &v, 4);
    v = hz * 2;                        memcpy(d + 28, &v, 4);   // byte rate
    h = 2;                             memcpy(d + 32, &h, 2);   // block align
    h = 16;                            memcpy(d + 34, &h, 2);   // bits
    memcpy(d + 36, "data", 4);
    memcpy(d + 40, &bytes, 4);
}
