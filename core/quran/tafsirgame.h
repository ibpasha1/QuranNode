// tafsirgame.h — the meaning game: learn what an ayah says, word by word.
//
// This is the card GENERATOR behind the Tafsir Game lesson. It turns the
// per-word English glosses (wordmeaning.h / the .qwm packs) into fair
// multiple-choice and ordering puzzles. Scheduling and persistence live
// elsewhere; this file is pure and stateless — given a gloss source and an
// ayah it hands back a self-contained card, or refuses when the ayah can't
// make a fair one (too few glossed words, no plausible distractors).
//
// Three card kinds, rotated for variety and retention:
//   TG_QUIZ     — one Arabic word shown (the scene draws the glyph); pick its
//                 English meaning from the choices.
//   TG_CLOZE    — the ayah's full meaning with one word blanked; pick the
//                 missing piece.
//   TG_ASSEMBLE — the word-meanings shown shuffled; put them in reading order.
//
// TWO THINGS TO KNOW BEFORE READING THE CODE.
//
// **The gloss source has a single cache slot.** wordmeaning_word() returns a
// pointer that dies the moment another ayah is fetched. A card is built by
// reading the target ayah AND scanning other ayat for distractors, so every
// string a card keeps is copied into its own arena the instant it is read.
//
// **Choice/item strings are stored as OFFSETS, not pointers.** A TafsirCard is
// a plain value you can copy, stash in a struct, or hand across a frame; raw
// char* into the arena would dangle after a copy. Use tg_choice()/tg_item().
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "wordmeaning.h"

typedef enum { TG_QUIZ = 0, TG_CLOZE, TG_ASSEMBLE, TG_KIND_COUNT } TafsirCardKind;

#define TG_MAX_CHOICES 4    // multiple-choice options (quiz/cloze)
#define TG_MAX_ITEMS   12   // words handled in a cloze sentence / assemble set
#define TG_ARENA       1024 // backing bytes for all copied strings in one card

// A gloss source: an OPEN WordMeaning plus the surah it was opened for.
typedef struct {
    WordMeaning *wm;
    int          surah;
} TafsirSource;

// A fully built, self-contained card. `arena` backs every string; the *_off
// fields index into it (0 is a valid offset — arena[0] is always a NUL so an
// unused slot reads as ""). Never store the returned tg_choice()/tg_item()
// pointers past the next fetch on the same card.
typedef struct {
    TafsirCardKind kind;
    uint16_t surah, ayah;
    uint8_t  n_words;                    // drawn word count of the ayah

    // TG_QUIZ / TG_CLOZE — multiple choice.
    uint8_t  word;                       // quizzed word (quiz) / blanked word (cloze)
    uint8_t  n_choices;
    uint8_t  correct;                    // index into choices of the right answer
    uint16_t choice_off[TG_MAX_CHOICES];

    // TG_CLOZE  — item_off[] is the full-ayah sentence in reading order, with
    //             the blanked slot interned as "____".
    // TG_ASSEMBLE — item_off[] is the glossed words in CORRECT reading order;
    //             show[] is the presentation order (show[p] = which item sits
    //             in slot p on screen), a non-identity permutation of 0..n-1.
    uint8_t  n_items;
    uint16_t item_off[TG_MAX_ITEMS];
    uint8_t  show[TG_MAX_ITEMS];

    char     arena[TG_ARENA];
    uint16_t arena_len;
} TafsirCard;

const char *tg_choice(const TafsirCard *c, int i);   // "" if i out of range
const char *tg_item(const TafsirCard *c, int i);     // "" if i out of range

// A tiny seeded PRNG so card generation is reproducible in tests and varied on
// the device. Seed with anything non-zero (0 is remapped internally).
uint32_t tg_rng(uint32_t *state);
void     tg_seed(uint32_t *state, uint32_t seed);

// Build a card of a specific kind for (src->surah, ayah). Returns false when
// the ayah can't yield a fair card of that kind; *out is left untouched then.
bool tg_make_card(TafsirSource *src, int ayah, TafsirCardKind kind,
                  uint32_t *rng, TafsirCard *out);

// Build whatever card this ayah can support, preferring kinds in an order
// rotated by `spin` (pass a running counter for variety). False only if the
// ayah supports NO kind at all.
bool tg_make_any(TafsirSource *src, int ayah, uint32_t *rng, int spin,
                 TafsirCard *out);
