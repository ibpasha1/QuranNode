// tglearn.h — the Tafsir Game learner: what meanings you've studied, what's due
// to review, and what's next in your target.
//
// This is the scheduling + persistence half of the Tafsir Game; the card
// GENERATOR (quiz/cloze/assemble) lives in tafsirgame.h and is unaware of any
// of this. The unit here is a single AYAH — you learn what one verse means —
// unlike hifz, whose unit is the contiguous portion you recited in one sitting.
//
// A light Leitner ladder drives review: a correct session bumps the ayah up a
// box (longer interval), a rough one holds it, a wrong one drops it to the
// bottom. Like khatm and hifz it degrades honestly with no RTC — boxes and
// streaks still move, only the DATES stop (everything reads "due whenever").
//
// State persists exactly like khatm/hifz: two alternating slots, a CRC over the
// body, and an append-only tail so a newer firmware can add fields without
// invalidating an old save (all-bits-zero must stay a valid default).
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define TGL_TOTAL_AYAT 6236
// Active ayat under study, capped so the blob stays a single <16 KB SD burst
// (TgItem is 12 bytes; 512 * 12 + header is ~6.2 KB). You review a working set,
// not the whole mushaf at once, so this is generous in practice.
#define TGL_MAX_ITEMS  512
#define TGL_DEF_NEW_PER_DAY 5

// How the meaning session went. Three-valued for the same reason hifz is:
// "got it but shaky" should hold the interval, not advance or reset it.
typedef enum { TG_WRONG = 0, TG_HARD, TG_GOT } TgGrade;

typedef enum {
    TGL_SCOPE_NONE = 0, TGL_SCOPE_SURAH, TGL_SCOPE_JUZ, TGL_SCOPE_QURAN,
} TgScopeKind;

typedef struct {
    uint16_t first_g, last_g;   // inclusive global ayah range; (0,0) = no target
    uint8_t  kind;              // TgScopeKind
    uint8_t  reverse;           // study last-to-first (e.g. Juz Amma from An-Nas)
    uint16_t label_arg;         // surah or juz number, for the display label
} TgScope;

typedef struct {                // 12 bytes
    uint16_t g;                 // global ayah index 1..6236; 0 = empty slot
    uint16_t due_day;           // 0 = due whenever (introduced with no clock)
    uint16_t last_day;          // 0 = never studied with a known clock
    uint16_t last_ord;          // session ordinal — the no-clock ordering key
    uint8_t  box;               // interval index into TGL_BOX_DAYS
    uint8_t  streak;            // consecutive TG_GOT
    uint8_t  lapses;            // lifetime TG_WRONG; never reset
    uint8_t  flags;
} TgItem;

typedef struct {
    int  started;               // ayat introduced
    int  mastered;              // reached the top box
    int  due;                   // due today (all started ayat when clock unknown)
    int  scope_total;           // ayat in the target
    int  scope_done;            // started ayat within the target
    bool have_day;
    int  day;
    uint32_t answered, correct; // lifetime totals
} TgStats;

// The box interval ladder in days (box 0 is "relearn now"). Exposed for the
// play scene's "next review in N days" hint and for the test.
extern const uint16_t TGL_BOX_DAYS[];
extern const int      TGL_BOX_COUNT;

// --- Lifecycle -------------------------------------------------------------
void tglearn_init(void);
void tglearn_service(void);     // per frame: day rollover + debounced save
void tglearn_flush(void);

// --- Target ----------------------------------------------------------------
void    tglearn_set_scope(TgScopeKind kind, int arg, bool reverse);
TgScope tglearn_scope(void);
void    tglearn_clear_scope(void);
void    tglearn_scope_label(char *buf, int n);   // "Al-Fatihah" / "Juz 30" / ...

// --- Study flow ------------------------------------------------------------
// Introduce the next unstarted ayah from the target as a new item (box 0, due
// now). Returns its global index, or 0 if none remain / the table is full.
int tglearn_start_new(void);

// Today's review order: due ayat, most overdue first (then weakest box, then
// oldest). Writes up to `max` global ayah indices; returns the count. New ayat
// are NOT included — pull those with tglearn_start_new() up to the daily cap.
int tglearn_today(int *out_g, int max);
int tglearn_due_count(void);
int tglearn_new_available(void);       // unstarted ayat left in the target

// Record a session result for an ayah; advances / holds / resets its box and
// reschedules it. Grading an ayah that was never introduced starts it.
void tglearn_grade(int surah, int ayah, TgGrade g);

// --- Introspection ---------------------------------------------------------
int            tglearn_item_count(void);
const TgItem  *tglearn_item(int i);                 // NULL if out of range
int            tglearn_item_at(int surah, int ayah); // table index, -1 if none
int            tglearn_box(int surah, int ayah);      // 0..max, -1 if not started
const TgStats *tglearn_stats(void);
uint32_t       tglearn_state_seq(void);              // bumped on any change

// --- Config ----------------------------------------------------------------
int  tglearn_new_per_day(void);
void tglearn_set_new_per_day(int n);
