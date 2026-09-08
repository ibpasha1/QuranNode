// wordmeaning.h — per-word English glosses (drives the drill's meaning sheet).
//
// Loads a per-surah ".qwm" file (built by tools/build_wordmeanings.py). Like the
// glyph packs, the file is kept OPEN and streamed: only the header + a small
// index live in RAM, and one ayah's gloss record is read on demand into a single
// cache slot. Al-Baqarah's glosses are ~106 KB — far too large to hold resident.
//
// CRUCIAL: this is keyed to the glyph-pack / qdb_word_count word split (the one
// actually drawn and selectable), NOT the timing split. build_wordmeanings.py
// reconciles to that split at build time and writes empty glosses for any ayah
// it can't align, so word N here always means drawn word N.
//
// File format (little-endian) — see tools/build_wordmeanings.py:
//   header { "QNWM", u16 version, u16 surah, u16 n_ayat, u16 reserved }
//   index[n_ayat] { u16 ayah, u16 n_words, u32 blob_off, u32 blob_len }
//   blob (per ayah) { u16 off[n_words] (rel to blob start), packed NUL-term utf8 }
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct HalFile;

typedef struct {
    struct HalFile *file;
    uint16_t version, surah, n_ayat;
    uint8_t *index;         // owned: n_ayat * 12 bytes

    int      cache_ayah;    // -1 = empty
    int      cache_nwords;
    uint8_t *rec;           // owned: one ayah's blob (off table + strings)
    size_t   rec_cap, rec_len;
} WordMeaning;

// Open quran/wbw/<surah>.qwm via the HAL. False if absent/malformed.
bool wordmeaning_open(WordMeaning *wm, int surah);
void wordmeaning_close(WordMeaning *wm);

int  wordmeaning_word_count(WordMeaning *wm, int ayah);

// Gloss for a drawn word (reading order). Returns "" if out of range or unglossed;
// the pointer stays valid until another ayah is fetched (single cache slot).
const char *wordmeaning_word(WordMeaning *wm, int ayah, int word);
