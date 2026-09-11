// khatm_test.c — deterministic tests for the reading-coverage engine.
//
// Drives khatm.c with a controllable clock and an in-memory state store, so we
// can assert the things that actually matter and can't be checked by eye:
// that fast-scrolling earns no credit, that page fractions never drift, that a
// torn save falls back to the other slot, and that the day ring survives a
// clock that jumps backwards.
#include "khatm.h"
#include "quran_db.h"
#include "progress.h"
#include "hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, ...) do {                                            \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

// --- controllable clock ---------------------------------------------------
static uint32_t g_ms = 0;
static int64_t  g_epoch = 0;     // 0 = "clock unknown"
uint32_t plat_millis(void) { return g_ms; }
int64_t hal_wall_clock(void) { return g_epoch; }
int hal_tz_offset_min(void) { return 0; }
QnClockSource hal_clock_source(void) { return QN_CLOCK_SYNCED; }
void hal_clock_persist(void) {}

// --- in-memory state store ------------------------------------------------
#define MAX_SLOTS 8
static struct { char name[24]; uint8_t buf[8192]; size_t len; bool used; } g_store[MAX_SLOTS];

bool hal_state_save(const char *name, const void *data, size_t len)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_store[i].used && strcmp(g_store[i].name, name) == 0) {
            memcpy(g_store[i].buf, data, len); g_store[i].len = len; return true;
        }
    }
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!g_store[i].used) {
            g_store[i].used = true;
            snprintf(g_store[i].name, sizeof g_store[i].name, "%s", name);
            memcpy(g_store[i].buf, data, len); g_store[i].len = len; return true;
        }
    }
    return false;
}

bool hal_state_load(const char *name, void *buf, size_t cap, size_t *out_len)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (g_store[i].used && strcmp(g_store[i].name, name) == 0) {
            size_t n = g_store[i].len < cap ? g_store[i].len : cap;
            memcpy(buf, g_store[i].buf, n);
            if (out_len) *out_len = n;
            return true;
        }
    }
    return false;
}

static void store_reset(void) { memset(g_store, 0, sizeof g_store); }

static void store_corrupt(const char *name, int flip_byte)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_store[i].used && strcmp(g_store[i].name, name) == 0)
            g_store[i].buf[flip_byte] ^= 0xFF;
}

static void store_truncate(const char *name, size_t to)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_store[i].used && strcmp(g_store[i].name, name) == 0)
            g_store[i].len = to;
}

// --- progress.c stubs (khatm's plan reads the resume point) ---------------
static bool s_has_resume = false;
static ResumePoint s_resume = { 1, 1, 1.0f };
bool progress_has_resume(void) { return s_has_resume; }
ResumePoint progress_resume(void) { return s_resume; }

// --- helpers --------------------------------------------------------------
// Simulate reading an ayah: focus it, then advance the clock in frame-sized
// steps until the dwell threshold is crossed.
static void read_ayah(int surah, int ayah, int n_words, int total_ms, int step_ms)
{
    khatm_focus(surah, ayah, n_words);
    for (int t = 0; t < total_ms; t += step_ms) {
        g_ms += step_ms;
        khatm_tick(step_ms);
    }
}

#define DAY(n) ((int64_t)(n) * 86400 + 43200)   // noon of day n, UTC

int main(void)
{
    // =====================================================================
    printf("-- fast scrolling must earn nothing --\n");
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    // Sweep a whole surah at the encoder's fastest repeat (~40ms/ayah).
    for (int a = 1; a <= 286; a++) {
        khatm_focus(2, a, 10);
        g_ms += 40;
        khatm_tick(40);
    }
    CHECK(khatm_stats()->ayat_read == 0,
          "fast scroll credited %d ayat, expected 0", khatm_stats()->ayat_read);

    // =====================================================================
    printf("-- dwell credits at the length-scaled threshold --\n");
    store_reset();
    khatm_init();
    // 10 words * 350ms = 3500ms. Just under must not credit.
    read_ayah(2, 1, 10, 3400, 100);
    CHECK(khatm_stats()->ayat_read == 0, "credited before the threshold");
    read_ayah(2, 1, 10, 200, 100);   // now past 3500ms
    CHECK(khatm_stats()->ayat_read == 1, "did not credit at the threshold");
    CHECK(khatm_is_read(2, 1), "2:1 not marked read");

    // A one-word ayah still needs the 2s floor, not 350ms.
    read_ayah(114, 1, 1, 1900, 100);
    CHECK(!khatm_is_read(114, 1), "one-word ayah credited before the 2s floor");
    read_ayah(114, 1, 1, 200, 100);
    CHECK(khatm_is_read(114, 1), "one-word ayah never reached the floor");

    // The longest ayah is capped so it can't demand 45 seconds.
    read_ayah(2, 282, 129, KHATM_MAX_DWELL_MS + 100, 100);
    CHECK(khatm_is_read(2, 282), "2:282 not credited within the dwell ceiling");

    // =====================================================================
    printf("-- audio completion credits immediately --\n");
    store_reset();
    khatm_init();
    khatm_audio_complete(36, 1);
    CHECK(khatm_is_read(36, 1), "audio completion did not credit");
    CHECK(khatm_stats()->ayat_read == 1, "audio completion credited wrong count");

    // =====================================================================
    printf("-- re-reading does not double count --\n");
    store_reset();
    khatm_init();
    read_ayah(1, 1, 4, 3000, 100);
    CHECK(khatm_stats()->ayat_read == 1, "first read miscounted");
    uint32_t after_first = khatm_stats()->read_mpages;
    for (int i = 0; i < 5; i++) {          // hifz loop: same ayah again and again
        khatm_focus(1, 2, 4);              // move away
        khatm_audio_complete(1, 1);
    }
    CHECK(khatm_stats()->ayat_read == 1, "re-reading double counted");
    CHECK(khatm_stats()->read_mpages == after_first, "re-reading added mpages");

    // =====================================================================
    printf("-- a fully-read mushaf is exactly 604.000 pages --\n");
    store_reset();
    khatm_init();
    khatm_mark_pages(1, QDB_PAGE_COUNT);
    const KhatmStats *k = khatm_stats();
    CHECK(k->read_mpages == KHATM_TOTAL_MPAGES,
          "full mushaf = %u mpages, expected %u",
          (unsigned)k->read_mpages, KHATM_TOTAL_MPAGES);
    CHECK(k->ayat_read == KHATM_TOTAL_AYAT,
          "full mushaf = %d ayat, expected %d", k->ayat_read, KHATM_TOTAL_AYAT);
    CHECK(k->pages_full == QDB_PAGE_COUNT, "only %d full pages", k->pages_full);
    CHECK(k->juz_full == QDB_JUZ_COUNT, "only %d full juz", k->juz_full);
    CHECK(k->complete, "full mushaf not reported complete");
    CHECK(k->percent > 99.99f && k->percent < 100.01f,
          "full mushaf is %.3f%%", k->percent);

    // Every prefix of the mushaf should also be drift-free at page granularity.
    for (int p = 1; p <= QDB_PAGE_COUNT; p += 37) {
        store_reset();
        khatm_init();
        khatm_mark_pages(1, p);
        CHECK(khatm_stats()->read_mpages == (uint32_t)p * 1000,
              "pages 1..%d = %u mpages, expected %d", p,
              (unsigned)khatm_stats()->read_mpages, p * 1000);
    }

    // =====================================================================
    printf("-- save/load round-trips --\n");
    store_reset();
    khatm_init();
    read_ayah(18, 1, 8, 4000, 100);
    read_ayah(18, 2, 8, 4000, 100);
    khatm_set_goal_days(30);
    khatm_flush();
    int ayat_before = khatm_stats()->ayat_read;
    uint32_t mp_before = khatm_stats()->read_mpages;
    khatm_init();   // reload from the store
    CHECK(khatm_stats()->ayat_read == ayat_before, "ayat lost across reload");
    CHECK(khatm_stats()->read_mpages == mp_before, "mpages lost across reload");
    CHECK(khatm_is_read(18, 1) && khatm_is_read(18, 2), "coverage lost");
    CHECK(khatm_goal().start_day != 0, "goal lost across reload");

    // =====================================================================
    printf("-- a torn write falls back to the other slot --\n");
    // Two saves land in alternating slots; corrupting the newer one must fall
    // back rather than losing everything (hal_state_save truncates on write).
    store_reset();
    khatm_init();
    khatm_mark_pages(1, 10);      // save #1
    khatm_mark_pages(11, 20);     // save #2 -> other slot
    int both = khatm_stats()->ayat_read;
    // Corrupt each slot in turn; a reload must still produce valid state.
    for (int which = 0; which < 2; which++) {
        store_reset();
        khatm_init();
        khatm_mark_pages(1, 10);
        khatm_mark_pages(11, 20);
        store_corrupt(which ? "khatm.a" : "khatm.b", 64);
        khatm_init();
        CHECK(khatm_stats()->ayat_read > 0,
              "corrupting %s lost all state", which ? "khatm.a" : "khatm.b");
        CHECK(khatm_stats()->ayat_read <= both,
              "corrupt reload invented coverage");
    }

    // A truncated slot is rejected, not half-read.
    store_reset();
    khatm_init();
    khatm_mark_pages(1, 10);
    khatm_mark_pages(11, 20);
    store_truncate("khatm.a", 100);
    store_truncate("khatm.b", 100);
    khatm_init();
    CHECK(khatm_stats()->ayat_read == 0,
          "truncated slots should read as empty, got %d ayat",
          khatm_stats()->ayat_read);

    // =====================================================================
    printf("-- coverage accrues with no clock, then lands on the first day --\n");
    store_reset();
    g_epoch = 0;                    // device has never seen the network
    khatm_init();
    read_ayah(93, 1, 5, 3000, 100);
    read_ayah(93, 2, 5, 3000, 100);
    CHECK(khatm_stats()->ayat_read == 2, "coverage blocked while clock unknown");
    CHECK(!khatm_stats()->have_day, "claimed a day with no clock");
    CHECK(khatm_today() == 0, "day index not zero with no clock");
    g_epoch = DAY(20100);           // SNTP finally lands
    khatm_service();
    CHECK(khatm_stats()->have_day, "day not picked up after sync");
    CHECK(khatm_stats()->today_mpages > 0,
          "pending credit was not drained into the first known day");

    // =====================================================================
    printf("-- day rollover and streak --\n");
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    // Read a page a day for 5 days.
    for (int d = 0; d < 5; d++) {
        g_epoch = DAY(20000 + d);
        khatm_service();
        int first = qdb_page_first_global(d + 1), n = qdb_page_ayah_count(d + 1);
        for (int i = first; i < first + n; i++) {
            QRef r = qdb_from_global(i);
            khatm_audio_complete(r.surah, r.ayah);
        }
    }
    CHECK(khatm_stats()->streak == 5, "streak = %d, expected 5",
          khatm_stats()->streak);
    // Skip two days, then read again: the streak restarts.
    g_epoch = DAY(20007);
    khatm_service();
    CHECK(khatm_stats()->streak == 0, "streak survived a two-day gap (%d)",
          khatm_stats()->streak);
    // Today alone must never break a streak that's still in progress.
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    int f1 = qdb_page_first_global(1), n1 = qdb_page_ayah_count(1);
    for (int i = f1; i < f1 + n1; i++) {
        QRef r = qdb_from_global(i);
        khatm_audio_complete(r.surah, r.ayah);
    }
    g_epoch = DAY(20001);   // new day, nothing read yet
    khatm_service();
    CHECK(khatm_stats()->streak == 1,
          "an untouched today broke the streak (%d)", khatm_stats()->streak);

    // =====================================================================
    printf("-- a backwards clock adds to history, never corrupts it --\n");
    store_reset();
    g_epoch = DAY(20050);
    khatm_init();
    khatm_audio_complete(1, 1);
    g_epoch = DAY(20040);   // bad NTP / timezone edit
    khatm_service();
    khatm_audio_complete(1, 2);
    CHECK(khatm_stats()->ayat_read == 2, "coverage lost on a backwards jump");
    g_epoch = DAY(20050);
    khatm_service();
    CHECK(khatm_stats()->today_mpages > 0, "the original day's tally was erased");

    // =====================================================================
    printf("-- goal quota re-derives when ahead and behind --\n");
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    khatm_set_goal_days(30);
    uint32_t q0 = khatm_stats()->quota_mpages;
    CHECK(q0 > 20000 && q0 < 20500, "day-1 quota of a 30-day khatm = %u mpages",
          (unsigned)q0);
    // Get well ahead: quota for the remaining days must drop.
    khatm_mark_pages(1, 100);
    g_epoch = DAY(20001);
    khatm_service();
    uint32_t q_ahead = khatm_stats()->quota_mpages;
    CHECK(q_ahead < q0, "quota did not drop when ahead (%u -> %u)",
          (unsigned)q0, (unsigned)q_ahead);
    CHECK(khatm_stats()->delta_mpages > 0, "delta not positive when ahead");
    // Now fall behind: no reading for 10 days.
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    khatm_set_goal_days(30);
    g_epoch = DAY(20010);
    khatm_service();
    uint32_t q_behind = khatm_stats()->quota_mpages;
    CHECK(q_behind > q0, "quota did not rise when behind (%u -> %u)",
          (unsigned)q0, (unsigned)q_behind);
    CHECK(khatm_stats()->delta_mpages < 0, "delta not negative when behind");

    // Overdue is flagged rather than collapsing the quota silently.
    g_epoch = DAY(20100);
    khatm_service();
    CHECK(khatm_stats()->overdue, "past-target goal not flagged overdue");
    // With no days left to divide by, a naive quota asks for the whole rest of
    // the Quran today. It must fall back to the original daily rate instead.
    uint32_t q_over = khatm_stats()->quota_mpages;
    CHECK(q_over < 30000, "overdue quota is %u mpages (should be ~a day's worth)",
          (unsigned)q_over);
    CHECK(q_over > 0, "overdue quota collapsed to zero");
    // ...and the plan must not then propose reading to the end of the mushaf.
    CHECK(khatm_today_plan()->to_page < QDB_PAGE_COUNT,
          "overdue plan ran to page %d", khatm_today_plan()->to_page);
    khatm_extend_goal(7);
    CHECK(!khatm_stats()->overdue, "extending did not clear overdue");
    CHECK(khatm_stats()->days_left == 7, "extend gave %d days, expected 7",
          khatm_stats()->days_left);

    // =====================================================================
    printf("-- today's plan starts at the first unread ayah --\n");
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    khatm_set_goal_days(30);
    khatm_mark_pages(1, 50);           // pages 1..50 already done
    s_has_resume = true;
    s_resume.surah = 2; s_resume.ayah = 1;   // resume sits inside the read part
    const KhatmPlan *pl = khatm_today_plan();
    CHECK(pl->from_page == 51, "plan starts on page %d, expected 51",
          pl->from_page);
    CHECK(!khatm_is_read(pl->from_surah, pl->from_ayah),
          "plan starts on an already-read ayah %d:%d",
          pl->from_surah, pl->from_ayah);
    CHECK(qdb_page_of(pl->from_surah, pl->from_ayah) == pl->from_page,
          "plan's start ayah is not on its start page");
    CHECK(pl->to_page >= pl->from_page, "plan range is inverted");
    CHECK(pl->mpages > 0, "plan covers nothing");

    // A finished mushaf yields an empty plan rather than a bogus range.
    khatm_mark_pages(1, QDB_PAGE_COUNT);
    CHECK(khatm_today_plan()->from_page == 0,
          "completed khatm still produced a plan");

    // =====================================================================
    printf("-- start over keeps the habit, clears the coverage --\n");
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    int fp = qdb_page_first_global(1), np = qdb_page_ayah_count(1);
    for (int i = fp; i < fp + np; i++) {
        QRef r = qdb_from_global(i);
        khatm_audio_complete(r.surah, r.ayah);
    }
    khatm_set_goal_days(30);
    int streak_before = khatm_stats()->streak;
    khatm_mark_pages(1, QDB_PAGE_COUNT);
    CHECK(khatm_stats()->khatms_done >= 0, "khatm count went negative");
    khatm_reset_coverage();
    CHECK(khatm_stats()->ayat_read == 0, "reset did not clear coverage");
    CHECK(khatm_stats()->streak == streak_before,
          "reset destroyed the streak (%d -> %d)", streak_before,
          khatm_stats()->streak);
    CHECK(khatm_goal().start_day != 0, "reset dropped the goal window");

    // =====================================================================
    printf("-- a scoped goal measures pace inside its window --\n");
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    s_has_resume = false;
    // Scope to juz 30 — a clean multi-page window at the end of the mushaf.
    int jp0 = 0, jp1 = 0;
    CHECK(khatm_juz_page_range(30, &jp0, &jp1), "juz 30 range unresolved");
    CHECK(jp1 == QDB_PAGE_COUNT, "juz 30 should end at the last page, got %d", jp1);
    int jpages = jp1 - jp0 + 1;
    khatm_set_goal_pages(jp0, jp1, 10);
    const KhatmStats *ks = khatm_stats();
    CHECK(ks->have_goal, "scoped goal not registered");
    CHECK(ks->scope_from_page == jp0 && ks->scope_to_page == jp1,
          "scope window = %d..%d, expected %d..%d",
          ks->scope_from_page, ks->scope_to_page, jp0, jp1);
    CHECK(ks->scope_mpages == (uint32_t)jpages * 1000,
          "scope span = %u, expected %d", (unsigned)ks->scope_mpages, jpages * 1000);
    CHECK(ks->scope_read_mpages == 0, "fresh scope should read 0");
    CHECK(!ks->scope_complete, "empty scope reported complete");
    // Day-1 quota is the scope's span over its days — a fraction of the
    // whole-mushaf quota, which is the whole point.
    uint32_t sq = ks->quota_mpages;
    CHECK(sq < 5000, "scoped quota %u looks whole-mushaf sized", (unsigned)sq);
    CHECK(sq > (uint32_t)jpages * 100 - 300 && sq < (uint32_t)jpages * 100 + 300,
          "scoped day-1 quota = %u, expected ~%d", (unsigned)sq, jpages * 100);
    // Reading OUTSIDE the window advances the odometer but not the goal.
    khatm_mark_pages(1, 5);
    CHECK(khatm_stats()->scope_read_mpages == 0,
          "out-of-scope reading counted toward the goal");
    CHECK(khatm_stats()->read_mpages > 0, "odometer should still move");
    CHECK(!khatm_stats()->scope_complete, "goal completed by out-of-scope reading");
    // Filling the window completes the goal — but not the whole khatm.
    khatm_mark_pages(jp0, jp1);
    ks = khatm_stats();
    CHECK(ks->scope_read_mpages == ks->scope_mpages, "scope not fully credited");
    CHECK(ks->scope_complete, "filled scope not reported complete");
    CHECK(!ks->complete, "a juz-sized goal wrongly completed the whole mushaf");
    CHECK(!ks->overdue, "a completed scope must not read as overdue");

    // =====================================================================
    printf("-- a scoped plan never leaves its window --\n");
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    s_has_resume = false;
    int sp0 = 0, sp1 = 0;
    CHECK(khatm_surah_page_range(2, &sp0, &sp1), "surah 2 range unresolved");
    khatm_set_goal_pages(sp0, sp1, 30);
    const KhatmPlan *spl = khatm_today_plan();
    CHECK(spl->from_page >= sp0 && spl->to_page <= sp1,
          "scoped plan %d..%d escaped window %d..%d",
          spl->from_page, spl->to_page, sp0, sp1);
    CHECK(spl->from_page == sp0, "scoped plan should open at page %d, got %d",
          sp0, spl->from_page);
    // With a gentle deadline and all but the last page done, the plan should
    // sit on exactly that page instead of wrapping to the window's start.
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    s_has_resume = false;
    khatm_set_goal_pages(sp0, sp1, 300);
    khatm_mark_pages(sp0, sp1 - 1);
    spl = khatm_today_plan();
    CHECK(spl->from_page == sp1 && spl->to_page == sp1,
          "plan should land on the last in-window page %d, got %d..%d",
          sp1, spl->from_page, spl->to_page);
    // Fill the window: the plan goes empty even though the mushaf is unfinished.
    khatm_mark_pages(sp0, sp1);
    CHECK(khatm_today_plan()->from_page == 0,
          "completed scope still produced a plan");
    CHECK(!khatm_stats()->complete, "surah scope wrongly completed the mushaf");

    // =====================================================================
    printf("-- a whole-Quran goal and an explicit 1..604 goal agree --\n");
    store_reset();
    g_epoch = DAY(20000);
    khatm_init();
    khatm_set_goal_days(30);
    uint32_t whole_q = khatm_stats()->quota_mpages;
    CHECK(khatm_stats()->scope_from_page == 1 &&
          khatm_stats()->scope_to_page == QDB_PAGE_COUNT,
          "legacy goal scope = %d..%d, expected 1..%d",
          khatm_stats()->scope_from_page, khatm_stats()->scope_to_page,
          QDB_PAGE_COUNT);
    CHECK(khatm_goal().reserved == 0, "whole-Quran goal should pack scope as 0");
    // The zero default is exactly what an old save carries: reload proves the
    // migration is a no-op.
    khatm_flush();
    khatm_init();
    CHECK(khatm_stats()->scope_to_page == QDB_PAGE_COUNT,
          "reloaded legacy goal lost its whole-mushaf scope");
    khatm_set_goal_pages(1, QDB_PAGE_COUNT, 30);
    CHECK(khatm_goal().reserved == 0,
          "explicit full-range goal should collapse to the zero default");
    CHECK(khatm_stats()->quota_mpages == whole_q,
          "explicit full-range goal disagreed with the legacy path");

    if (fails == 0) printf("\nkhatm-test: all checks passed\n");
    else            printf("\nkhatm-test: %d FAILURES\n", fails);
    return fails != 0;
}
