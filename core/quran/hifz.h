// hifz.h — memorization: what you know, what's due today, and what's next.
//
// This is the engine behind Lessons. It models the classical three-stream
// system rather than a generic flashcard scheduler:
//
//   sabaq   — today's NEW portion. Small, drilled hard, revisited same-day.
//   sabqi   — recent material, on tight dated intervals (1,1,2,3,4,5 days).
//   manzil  — consolidated material, cycled under a daily page budget.
//
// Two decisions are worth knowing before reading the code.
//
// **The scheduling unit is a portion, not an ayah.** You never review one ayah
// alone — the seam between ayat IS the skill. Per-ayah state exists, but only
// as a strength signal for the heat map and weak-spot targeting. The scheduler
// works on the contiguous run you memorized in one sitting.
//
// **Manzil is cycle-driven, not date-driven.** "Manzil" names the seven-part
// division used to finish the Quran in a week: the classical rule is that no
// memorized portion goes more than about seven days unseen, and the daily load
// is capped by TIME, not by an ease factor. Giving manzil due-dates would just
// show a hafiz a permanent 200-item backlog. Instead we take the oldest-seen
// first under a page budget, which self-balances: 3 juz memorized gives a
// 3-day cycle, 30 juz gives a 30-day cycle, with no extra logic.
//
// Like khatm, everything here degrades honestly when the clock is unknown (the
// board has no RTC): grades, boxes and tiers still advance, only DATES stop.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define HIFZ_TOTAL_AYAT   6236
// x ~13 ayat each covers the whole Quran. Capped so the persisted blob stays
// under 16384 bytes — hal_state_save chunks SD writes at 16 KB, and staying a
// single burst avoids the transient multi-block failures the ESP32 HAL logs.
#define HIFZ_MAX_PORTIONS 480
#define HIFZ_DAYS         64     // days of session history
#define HIFZ_PLAN_MAX     32     // tasks listed per tier

typedef enum { HZ_NEW = 0, HZ_SABAQ, HZ_SABQI, HZ_MANZIL } HifzTier;

// How the recall went. Deliberately three-valued: binary pass/fail loses the
// "I got it but it was rough" signal, which is exactly what should hold an
// interval steady instead of advancing it.
typedef enum { HZ_NO = 0, HZ_SHAKY, HZ_GOT } HifzGrade;

typedef struct {                 // 18 bytes
    uint16_t first_g, last_g;    // inclusive global ayah index; (0,0) = free
    uint16_t due_day;            // 0 = "due whenever" (created with no clock)
    uint16_t last_day;           // 0 = never reviewed with a known clock
    uint16_t created_day;        // 0 = created without a clock
    uint16_t last_ord;           // session ordinal — the no-clock ordering key
    uint8_t  tier;               // HifzTier
    uint8_t  box;                // interval index within the tier
    uint8_t  streak;             // consecutive HZ_GOT
    uint8_t  lapses;             // lifetime HZ_NO count; never reset
    uint8_t  settle;             // same-day sabaq sittings completed
    uint8_t  flags;
} HifzPortion;

// --- Target ---------------------------------------------------------------
typedef enum {
    HZ_SCOPE_NONE = 0, HZ_SCOPE_SURAH, HZ_SCOPE_JUZ, HZ_SCOPE_QURAN,
} HifzScopeKind;

typedef struct {
    uint16_t first_g, last_g;   // inclusive global ayah range
    uint8_t  kind;              // HifzScopeKind
    uint8_t  reverse;           // learn last-to-first (Juz Amma is done An-Nas up)
    uint16_t label_arg;         // surah or juz number, for the display label
} HifzScope;

// --- Tunables -------------------------------------------------------------
// `set` is a bitmask of which fields the user actually configured, so an
// all-zero struct means "all defaults" — required, because the persisted blob
// treats all-bits-zero as valid (a raw listen_reps of 0 would mean "play it
// zero times" on a fresh device).
enum {
    HZ_CFG_NEW_WORDS = 1 << 0, HZ_CFG_LISTEN = 1 << 1, HZ_CFG_ECHO = 1 << 2,
    HZ_CFG_PACE = 1 << 3, HZ_CFG_MANZIL = 1 << 4, HZ_CFG_BLOCK = 1 << 5,
};

typedef struct {
    uint16_t set;
    uint16_t new_words;       // new material per day (words)
    uint8_t  listen_reps;
    uint8_t  echo_reps;
    uint16_t pace_pct;        // echo countdown as % of the reference duration
    uint32_t manzil_mpages;   // daily manzil budget in milli-pages
    uint8_t  sabqi_block;     // overdue sabqi count that blocks new sabaq
    uint8_t  pad[3];
} HifzPlanCfg;

#define HZ_DEF_NEW_WORDS    40      // ~half a page
#define HZ_DEF_LISTEN_REPS   4
#define HZ_DEF_ECHO_REPS     3
#define HZ_DEF_PACE_PCT    115      // learners are slower than murattal
#define HZ_DEF_MANZIL_MP 20000      // 1 juz/day
#define HZ_DEF_SABQI_BLOCK   3

// --- Today's plan ---------------------------------------------------------
typedef struct {
    int16_t portion;    // index into the portion table
    uint8_t tier;
    uint8_t overdue;    // days late; 0 = due today
} HifzTask;

typedef struct {
    bool have_day;
    int  today;

    HifzTask sabaq;             // valid when n_sabaq == 1
    int      n_sabaq;
    HifzTask sabqi[HIFZ_PLAN_MAX];
    int      n_sabqi;
    HifzTask manzil[HIFZ_PLAN_MAX];
    int      n_manzil;

    int      n_sabqi_overdue;
    bool     sabaq_blocked;     // review backlog is gating new material
    bool     new_available;     // unmemorized scope remains
    uint32_t manzil_mpages;     // load actually selected
    int      manzil_backlog;    // due portions the budget excluded

    int      done_today;        // tasks already graded today
    int      total_today;
} HifzPlan;

typedef struct {
    int   memorized_ayat;       // strength > 0
    int   portions;
    int   n_sabqi, n_manzil;
    float scope_frac;           // 0..1 of the chosen target
    int   scope_done, scope_total;
    bool  have_day;
    int   streak, best_streak;
    int   day;
} HifzStats;

// A contiguous recite unit. It may span whole ayat (short surahs) or be a word
// range inside one ayah — 2:282 is 1753px tall at the smallest pack, over five
// screens, and you cannot self-assess a recall you can only partly see.
// When a0 == a1 the range is [w0..w1] of that ayah; otherwise it runs from
// word w0 of a0 through word w1 of a1.
typedef struct { int16_t a0, a1, w0, w1; } HifzSeg;

// --- Lifecycle ------------------------------------------------------------
void hifz_init(void);
void hifz_service(void);    // per frame: day rollover + debounced save
void hifz_flush(void);

// --- Target ---------------------------------------------------------------
void      hifz_set_scope(HifzScopeKind kind, int arg, bool reverse);
HifzScope hifz_scope(void);
void      hifz_clear_scope(void);
void      hifz_scope_label(char *buf, int n);   // "Juz 30" / "An-Naba" / "Quran"

// --- Portions -------------------------------------------------------------
int                hifz_portion_count(void);
const HifzPortion *hifz_portion(int i);
int                hifz_portion_at(int global_idx);   // -1 if none covers it
int                hifz_start_new_portion(void);      // carve sabaq; -1 if none
void               hifz_grade(int portion, HifzGrade g, int peeks);
void               hifz_portion_label(int portion, char *buf, int n);

// --- Per-ayah strength ----------------------------------------------------
int   hifz_strength(int surah, int ayah);    // 0..7
bool  hifz_ayah_lapsed(int surah, int ayah); // the lapsed marker (a HZ_NO)
void  hifz_grade_ayah(int surah, int ayah, HifzGrade g);
float hifz_surah_frac(int surah);            // memorized fraction of a surah
float hifz_juz_frac(int juz);

// --- Heat map + weak spots ------------------------------------------------
// The memorization map reuses the khatm coverage grid, keyed on strength.
float hifz_page_frac(int page);              // memorized fraction of a mushaf page
bool  hifz_page_has_weak(int page);          // a lapsed ayah sits on this page

// The weakest memorized ayat, weakest-first (lapsed, then lowest strength), for
// targeted review. Only ayat that have been learned and slipped — never
// still-unlearned material. Returns the count written (<= max).
typedef struct { uint16_t surah, ayah; uint8_t strength, lapsed; } HifzWeakAyah;
int hifz_weak_ayat(HifzWeakAyah *out, int max);

// --- Plan + stats ---------------------------------------------------------
const HifzPlan  *hifz_plan(void);
const HifzStats *hifz_stats(void);
uint32_t         hifz_state_seq(void);   // bumped on any change, for view caches

// --- Config ---------------------------------------------------------------
int  hifz_cfg_new_words(void);
int  hifz_cfg_listen_reps(void);
int  hifz_cfg_echo_reps(void);
int  hifz_cfg_pace_pct(void);
uint32_t hifz_cfg_manzil_mpages(void);
void hifz_cfg_set_new_words(int words);
void hifz_cfg_set_reps(int listen, int echo);

// --- Chunker (pure; no SD, no state) --------------------------------------
// Split ayat [a0..a1] of `surah` into segments of about target_words, breaking
// at ayah boundaries where it can and inside over-long ayat where it must.
// Segments exactly tile the range. Returns the count written.
int hifz_chunk(int surah, int a0, int a1, int target_words, HifzSeg *out, int max);

// Words in a segment, for pacing and progress.
int hifz_seg_words(int surah, const HifzSeg *s);
