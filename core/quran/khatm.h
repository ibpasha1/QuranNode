// khatm.h — durable reading coverage, daily pace, and the finish-by-date goal.
//
// "Khatm" is a complete reading of the Quran. This module answers: how much
// have I read, how much today, am I on pace, and what should I read next.
//
// Two ideas carry the design:
//
//   1. Coverage is a bitmap over the 6236 ayat, keyed by global mushaf index
//      (qdb_global_index). It is time-independent, so it keeps working even
//      when the device has no idea what day it is — which matters, because the
//      board has no RTC and only learns the date if Wi-Fi happens to be up.
//
//   2. An ayah is credited only after you have plausibly READ it: a dwell
//      scaled to its length, or its recitation playing through. Holding the
//      scroll key through a surah earns nothing, which is the whole point —
//      a progress number you can trivially fake is not worth showing.
//
// Everything is reported in Madani mushaf pages, because that is the unit
// people actually use. Internally pages are tracked in *milli-pages* (1000 =
// one page) so partial pages are exact in integer math.
//
// State lives in its own blob, deliberately NOT in ProgressBlob: that struct's
// loader rejects any size mismatch, so growing it would wipe every user's
// bookmarks. See the versioning rules in khatm.c.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define KHATM_TOTAL_AYAT   6236
#define KHATM_TOTAL_PAGES  604
#define KHATM_TOTAL_MPAGES 604000u
#define KHATM_DAYS         128     // days of per-day history kept

// A day is credited toward the streak at one page.
#define KHATM_STREAK_MPAGES 1000u

// Dwell thresholds. The floor keeps one-word ayat honest; the ceiling keeps
// 2:282 (~129 words) from demanding 45 seconds.
#define KHATM_MS_PER_WORD    350
#define KHATM_MIN_DWELL_MS  2000
#define KHATM_MAX_DWELL_MS 30000

typedef struct {
    uint16_t day;      // local day index (days since epoch); 0 = empty slot
    uint16_t ayat;     // ayat first credited that day
    uint32_t mpages;   // milli-pages credited that day
} KhatmDay;

typedef struct {
    uint16_t start_day;          // 0 = no goal set
    uint16_t target_day;         // intended finish
    uint32_t start_mpages;       // coverage when the goal was set (pace baseline)
    uint16_t last_days;          // duration of the last goal, for "start over"
    uint8_t  pad[2];
    uint32_t reserved;
} KhatmGoal;

// Everything the UI needs, recomputed only when coverage or the day changes.
typedef struct {
    uint32_t read_mpages;     // 0 .. 604000
    int      ayat_read;       // 0 .. 6236
    int      pages_full;      // pages with every ayah read
    float    pages;           // read_mpages / 1000
    float    percent;         // 0 .. 100
    int      juz_full;        // fully-read juz (0..30)

    bool     have_day;        // false when the clock is unknown: day stats are void
    uint32_t today_mpages;    // credited today
    int      streak;          // consecutive qualifying days, today inclusive
    int      best_streak;
    float    avg;             // trailing pages/day over the recent window

    bool     have_goal;
    int      days_elapsed;    // since the goal started (>= 1)
    int      days_left;       // to target (>= 0); 0 = due today or overdue
    bool     overdue;         // past target_day and not finished
    uint32_t quota_mpages;    // today's target, re-derived from what's left
    int32_t  delta_mpages;    // + ahead of schedule, - behind
    int      eta_day;         // projected finish (day index), 0 = unknown

    // The goal's scope. A goal targets a window of mushaf pages; a whole-Quran
    // goal is just the widest window (pages 1..604). quota/delta/days_* above
    // are all relative to this scope, not the whole mushaf.
    int      scope_from_page, scope_to_page;   // inclusive; valid when have_goal
    uint32_t scope_mpages;        // span of the scope (pages * 1000)
    uint32_t scope_read_mpages;   // read within the scope so far
    float    scope_percent;       // 0 .. 100 within the scope
    bool     scope_complete;      // the scope is fully read (the goal is done)

    bool     complete;        // the whole mushaf is read
    int      khatms_done;
} KhatmStats;

// What to read today: the next unread stretch, sized to the remaining quota.
typedef struct {
    int      from_page, to_page;   // inclusive; from_page == 0 = nothing to do
    int      from_surah, from_ayah;
    int      to_surah, to_ayah;
    uint32_t mpages;               // unread content in the run
    bool     wrapped;              // ran past page 604 and back to the start
} KhatmPlan;

void khatm_init(void);      // load persisted state (call once at boot)
void khatm_service(void);   // once per frame: day rollover + throttled saves
void khatm_flush(void);     // force a save now (scene exit, goal change, ...)

// --- Reading credit -------------------------------------------------------
// The reader calls focus() each frame with the ayah on screen (n_words = 0 if
// the glyph pack has not loaded yet; it will be supplied on a later frame) and
// tick() with the frame delta. audio_complete() credits immediately.
void khatm_focus(int surah, int ayah, int n_words);
void khatm_tick(uint32_t dt_ms);
void khatm_audio_complete(int surah, int ayah);

// Dwell progress for the current focus ayah, so the reader can show it. A
// visible bar is what teaches the rule — otherwise credit looks like magic.
float    khatm_focus_dwell_frac(void);   // 0 .. 1
uint32_t khatm_focus_dwell_ms(void);
bool     khatm_focus_credited(void);

// --- Coverage queries -----------------------------------------------------
bool  khatm_is_read(int surah, int ayah);
float khatm_page_frac(int page);       // 0 .. 1 of that page's ayat
float khatm_surah_frac(int surah);
float khatm_juz_frac(int juz);
// Bumped whenever coverage changes, so views can cache derived work.
uint32_t khatm_coverage_seq(void);

const KhatmStats *khatm_stats(void);
const KhatmPlan  *khatm_today_plan(void);

// --- Goal -----------------------------------------------------------------
// A goal is "read this page window by this date". khatm_set_goal_days is the
// whole-Quran case (pages 1..604); khatm_set_goal_pages narrows it. Scope is
// snapped to whole mushaf pages, so a surah/juz pulls in its boundary pages —
// resolve those page ranges with the helpers below.
void khatm_set_goal_days(int days);                          // whole Quran in N days
void khatm_set_goal_pages(int from_page, int to_page, int days);
void khatm_extend_goal(int days);     // push the target back (overdue rescue)
void khatm_clear_goal(void);
KhatmGoal khatm_goal(void);

// Resolve a surah / juz to the inclusive page window that contains it. Returns
// false (and leaves the outputs untouched) for an out-of-range index.
bool khatm_surah_page_range(int surah, int *from_page, int *to_page);
bool khatm_juz_page_range(int juz, int *from_page, int *to_page);
// Milli-pages already read within a page window — for previewing a goal before
// it's set (the editor shows pages/day for the dialled-in scope).
uint32_t khatm_scope_read_mpages(int from_page, int to_page);
// Human name for a page window: "Whole Quran" / "Juz 30" / "Al-Baqarah" / a raw
// "Pages 3-9" when it matches no single surah or juz.
void khatm_scope_name(int from_page, int to_page, char *buf, int n);

// --- Bulk edits -----------------------------------------------------------
// Mark a page range read WITHOUT crediting it to today — used by the first-run
// backfill, where dumping 200 pages into today's tally would wreck the average,
// the quota and the streak for a week.
void khatm_mark_pages(int from_page, int to_page);
void khatm_reset_coverage(void);   // start a new khatm; keeps streak + history

bool khatm_backfill_offered(void);  // has the first-run prompt been shown?
void khatm_set_backfill_offered(void);

// Day index helpers (days since the Unix epoch, in local time).
int  khatm_today(void);                       // 0 when the clock is unknown
void khatm_format_day(int day, char *buf, int n);   // "Mar 14" / "--"
