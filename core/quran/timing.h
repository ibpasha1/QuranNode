// timing.h — per-ayah word timings (drives word-by-word highlight sync).
//
// Loads a per-surah ".qtm" file (built by tools/fetch_sample.py from the same
// recitation as the audio, so timings and audio can't drift). Given the audio
// playhead in ms (ayah-relative), timing_active_word() returns which word the
// reciter is on — the reader highlights that word's pre-baked box.
//
// File format (little-endian) — see tools/fetch_sample.py:
//   header { magic "QNTM", u16 version, u16 surah, u16 n_ayat, u16 reserved }
//   per ayah { u16 ayah, u16 n_words, n_words x (u32 start_ms, u32 end_ms) }
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint32_t start_ms, end_ms; } WordTiming;

typedef struct {
    uint8_t *data;
    size_t   len;
    int      surah;
    int      n_ayat;
    // Parsed index: for each ayah entry, its ayah number, word count, and the
    // byte offset of its word array within `data`.
    struct { uint16_t ayah, n_words; uint32_t words_off; } *idx;
} TimingTable;

// Load quran/timings/<surah>.qtm via the HAL. Returns false if absent.
bool timing_open(TimingTable *t, int surah);
void timing_close(TimingTable *t);

int  timing_word_count(const TimingTable *t, int ayah);
bool timing_word(const TimingTable *t, int ayah, int word, WordTiming *out);

// Word index the reciter is on at `pos_ms` (ayah-relative), or -1 before the
// first word. Holds on the last word until the ayah's audio ends.
int  timing_active_word(const TimingTable *t, int ayah, uint32_t pos_ms);
