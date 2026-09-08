// hifz_test.c — the memorization scheduler, headless and deterministic.
//
// The scheduler is the part of Lessons that can be quietly wrong for weeks: a
// bad interval or a graduation gate that fires early doesn't crash, it just
// stops working as hifz. So the intervals, the tier transitions, the daily plan
// composition, persistence and the clock-unknown path are all pinned here.
//
// Harness mirrors tools/khatm_test.c, with one important difference: the store
// buffer is 32768, because a HifzBlob is ~15 KB and khatm's 8192 would silently
// truncate every save — making the persistence tests pass for the wrong reason.
#include "hifz.h"
#include "qday.h"
#include "quran_db.h"
#include "hal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, ...) do {                                             \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

// --- controllable clock ---------------------------------------------------
static uint32_t g_ms = 0;
static int64_t  g_epoch = 0;     // 0 = "clock unknown"
uint32_t plat_millis(void) { return g_ms; }
int64_t hal_wall_clock(void) { return g_epoch; }
int hal_tz_offset_min(void) { return 0; }

#define DAY(n) ((int64_t)(n) * 86400 + 43200)   // noon of day n, UTC
#define D0 20000

// --- in-memory state store ------------------------------------------------
#define MAX_SLOTS 8
#define SLOT_CAP  32768          // must exceed sizeof(HifzBlob)
static struct { char name[24]; uint8_t buf[SLOT_CAP]; size_t len; bool used; } g_store[MAX_SLOTS];

bool hal_state_save(const char *name, const void *data, size_t len)
{
    if (len > SLOT_CAP) { printf("FAIL: blob %zu > slot cap %d\n", len, SLOT_CAP); fails++; return false; }
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_store[i].used && strcmp(g_store[i].name, name) == 0) {
            memcpy(g_store[i].buf, data, len); g_store[i].len = len; return true;
        }
    for (int i = 0; i < MAX_SLOTS; i++)
        if (!g_store[i].used) {
            g_store[i].used = true;
            snprintf(g_store[i].name, sizeof g_store[i].name, "%s", name);
            memcpy(g_store[i].buf, data, len); g_store[i].len = len; return true;
        }
    return false;
}

bool hal_state_load(const char *name, void *buf, size_t cap, size_t *out_len)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_store[i].used && strcmp(g_store[i].name, name) == 0) {
            size_t n = g_store[i].len < cap ? g_store[i].len : cap;
            memcpy(buf, g_store[i].buf, n);
            if (out_len) *out_len = n;
            return true;
        }
    return false;
}

static void store_reset(void) { memset(g_store, 0, sizeof g_store); }
static void store_corrupt(const char *name, int at)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_store[i].used && strcmp(g_store[i].name, name) == 0) g_store[i].buf[at] ^= 0xFF;
}
static void store_truncate(const char *name, size_t to)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_store[i].used && strcmp(g_store[i].name, name) == 0) g_store[i].len = to;
}

// --- helpers --------------------------------------------------------------
static void set_day(int d) { g_epoch = DAY(d); hifz_service(); }

static void fresh(int day, HifzScopeKind kind, int arg, bool reverse)
{
    store_reset();
    g_epoch = DAY(day);
    hifz_init();
    if (kind != HZ_SCOPE_NONE) hifz_set_scope(kind, arg, reverse);
}

static const HifzPortion *P(int i) { return hifz_portion(i); }

int main(void)
{
    printf("-- scope + carving the first sabaq --\n");
    fresh(D0, HZ_SCOPE_JUZ, 30, true);       // Juz Amma, An-Nas first
    int p0 = hifz_start_new_portion();
    CHECK(p0 == 0, "first portion index %d, expected 0", p0);
    CHECK(P(p0)->tier == HZ_SABAQ, "new portion is not sabaq");
    CHECK(P(p0)->created_day == D0, "created_day not stamped");
    {   // reverse means it must start at the END of the juz (An-Nas), not An-Naba
        QRef a = qdb_from_global(P(p0)->last_g);
        CHECK(a.surah == 114, "reverse scope started at surah %d, expected 114", a.surah);
    }

    printf("-- sabaq needs two clean sittings to settle into sabqi --\n");
    hifz_grade(p0, HZ_GOT, 0);
    CHECK(P(p0)->tier == HZ_SABAQ, "sabaq graduated after ONE sitting");
    CHECK(P(p0)->due_day == D0, "first sitting should re-due today");
    hifz_grade(p0, HZ_GOT, 0);
    CHECK(P(p0)->tier == HZ_SABQI, "sabaq did not settle into sabqi");
    CHECK(P(p0)->box == 0, "sabqi should start at box 0");
    CHECK(P(p0)->due_day == D0 + 1, "sabqi due %d, expected %d", P(p0)->due_day, D0 + 1);

    printf("-- sabqi walks the interval table exactly --\n");
    {
        static const int IV[] = { 1, 2, 3, 4, 5 };   // box 1..5 after each GOT
        int day = D0 + 1;
        for (int i = 0; i < 5; i++) {
            set_day(day);
            hifz_grade(p0, HZ_GOT, 0);
            CHECK(P(p0)->box == i + 1, "box %d, expected %d", P(p0)->box, i + 1);
            CHECK(P(p0)->due_day == day + IV[i],
                  "box %d due %d, expected %d", P(p0)->box, P(p0)->due_day, day + IV[i]);
            day += IV[i];
        }
        CHECK(P(p0)->tier == HZ_SABQI, "graduated before the age gate allowed it");
    }

    printf("-- graduation needs box + streak + real age --\n");
    fresh(D0, HZ_SCOPE_JUZ, 30, true);
    int q = hifz_start_new_portion();
    hifz_grade(q, HZ_GOT, 0); hifz_grade(q, HZ_GOT, 0);   // -> sabqi
    for (int i = 0; i < 6; i++) { set_day(D0 + 1 + i); hifz_grade(q, HZ_GOT, 0); }
    CHECK(P(q)->box == 5, "box %d, expected 5", P(q)->box);
    CHECK(P(q)->streak >= 3, "streak %d, expected >= 3", P(q)->streak);
    CHECK(P(q)->tier == HZ_SABQI,
          "graduated at age %d days — the 14-day gate should have held it",
          D0 + 6 - D0);
    set_day(D0 + 20);                                  // now old enough
    hifz_grade(q, HZ_GOT, 0);
    CHECK(P(q)->tier == HZ_MANZIL, "did not graduate to manzil when age was met");
    CHECK(P(q)->due_day == D0 + 20 + 7, "manzil due %d, expected %d",
          P(q)->due_day, D0 + 20 + 7);

    printf("-- shaky holds the box; no resets it --\n");
    fresh(D0, HZ_SCOPE_JUZ, 30, true);
    int r = hifz_start_new_portion();
    hifz_grade(r, HZ_GOT, 0); hifz_grade(r, HZ_GOT, 0);
    set_day(D0 + 1); hifz_grade(r, HZ_GOT, 0);          // box 1
    int box_before = P(r)->box;
    set_day(D0 + 2); hifz_grade(r, HZ_SHAKY, 0);
    CHECK(P(r)->box == box_before, "shaky moved the box");
    CHECK(P(r)->streak == 0, "shaky did not clear the streak");
    CHECK(P(r)->due_day == D0 + 3, "shaky due %d, expected %d", P(r)->due_day, D0 + 3);
    set_day(D0 + 3);
    int lapses_before = P(r)->lapses;
    hifz_grade(r, HZ_NO, 0);
    CHECK(P(r)->box == 0, "NO did not reset the box");
    CHECK(P(r)->lapses == lapses_before + 1, "NO did not count a lapse");
    CHECK(P(r)->due_day == D0 + 3, "NO should re-due today as repair work");

    printf("-- manzil NO demotes back to sabqi --\n");
    set_day(D0 + 40);
    hifz_grade(q, HZ_NO, 0);
    CHECK(P(q)->tier == HZ_SABQI, "manzil NO did not demote to sabqi");
    CHECK(P(q)->box == 0, "demoted portion kept its box");

    printf("-- peeking caps a GOT at shaky --\n");
    fresh(D0, HZ_SCOPE_JUZ, 30, true);
    int pk = hifz_start_new_portion();
    hifz_grade(pk, HZ_GOT, 0); hifz_grade(pk, HZ_GOT, 0);   // sabqi box 0
    set_day(D0 + 1);
    hifz_grade(pk, HZ_GOT, 2);                              // "got it" but peeked
    CHECK(P(pk)->box == 0, "a peeked GOT advanced the box (box %d)", P(pk)->box);
    CHECK(P(pk)->streak == 0, "a peeked GOT kept the streak");

    printf("-- lapses shorten intervals, never below 1 day --\n");
    fresh(D0, HZ_SCOPE_JUZ, 30, true);
    int lp = hifz_start_new_portion();
    hifz_grade(lp, HZ_GOT, 0); hifz_grade(lp, HZ_GOT, 0);
    for (int i = 0; i < 5; i++) { set_day(D0 + 1); hifz_grade(lp, HZ_NO, 0); }
    CHECK(P(lp)->lapses >= 4, "lapses %d, expected >= 4", P(lp)->lapses);
    // Four clean recalls walk box 0->4. The box-4 interval is nominally 4 days;
    // with lapses > 3 it must come back one day sooner, i.e. +3.
    for (int i = 0; i < 4; i++) { set_day(D0 + 2 + i); hifz_grade(lp, HZ_GOT, 0); }
    CHECK(P(lp)->box == 4, "box %d, expected 4", P(lp)->box);
    CHECK(P(lp)->due_day == D0 + 5 + 3,
          "lapsed interval due %d, expected %d (+3, not the nominal +4)",
          P(lp)->due_day, D0 + 5 + 3);
    {   // shortening must never push a due date into the past
        bool ok = true;
        for (int i = 0; i < hifz_portion_count(); i++)
            if (P(i)->due_day && P(i)->last_day && P(i)->due_day < P(i)->last_day) ok = false;
        CHECK(ok, "an interval went backwards");
    }

    printf("-- plan: sabaq is gated by an overdue review backlog --\n");
    fresh(D0, HZ_SCOPE_JUZ, 30, true);
    for (int i = 0; i < 5; i++) {          // five portions settled into sabqi
        int idx = hifz_start_new_portion();
        CHECK(idx >= 0, "could not carve portion %d", i);
        hifz_grade(idx, HZ_GOT, 0); hifz_grade(idx, HZ_GOT, 0);
    }
    set_day(D0 + 10);                      // all now well overdue
    {
        const HifzPlan *pl = hifz_plan();
        CHECK(pl->n_sabqi == 5, "n_sabqi %d, expected 5", pl->n_sabqi);
        CHECK(pl->n_sabqi_overdue == 5, "overdue %d, expected 5", pl->n_sabqi_overdue);
        CHECK(pl->sabaq_blocked, "sabaq not blocked despite 5 overdue reviews");
        CHECK(!pl->new_available, "new material offered while review is collapsing");
        // sorted most-fragile-first
        for (int i = 1; i < pl->n_sabqi; i++)
            CHECK(P(pl->sabqi[i - 1].portion)->due_day <= P(pl->sabqi[i].portion)->due_day,
                  "sabqi list is not sorted by due date");
    }

    printf("-- plan: manzil cycles under the daily budget --\n");
    {
        const HifzPlan *pl = hifz_plan();
        CHECK(pl->manzil_mpages <= hifz_cfg_manzil_mpages(),
              "manzil load %u exceeded the budget %u",
              (unsigned)pl->manzil_mpages, (unsigned)hifz_cfg_manzil_mpages());
    }

    printf("-- chunker tiles the range exactly --\n");
    {
        HifzSeg seg[64];
        // A very long ayah must be split INSIDE itself (2:282 is 128 words).
        int n = hifz_chunk(2, 282, 282, 40, seg, 64);
        CHECK(n >= 3, "2:282 split into %d segments, expected >= 3", n);
        int total = 0;
        for (int i = 0; i < n; i++) {
            CHECK(seg[i].a0 == 282 && seg[i].a1 == 282, "segment left the ayah");
            total += seg[i].w1 - seg[i].w0 + 1;
            if (i) CHECK(seg[i].w0 == seg[i - 1].w1 + 1, "segments do not tile (gap/overlap)");
        }
        CHECK(total == qdb_word_count(2, 282), "segments cover %d of %d words",
              total, qdb_word_count(2, 282));

        // A short surah collapses into one segment spanning whole ayat.
        n = hifz_chunk(114, 1, 6, 40, seg, 64);
        CHECK(n == 1, "An-Nas split into %d segments, expected 1", n);
        CHECK(seg[0].a0 == 1 && seg[0].a1 == 6, "An-Nas segment does not span 1..6");

        // A medium surah tiles without gaps across ayat.
        n = hifz_chunk(78, 1, 40, 40, seg, 64);
        CHECK(n > 1, "An-Naba did not split");
        CHECK(seg[0].a0 == 1, "first segment does not start at ayah 1");
        CHECK(seg[n - 1].a1 == 40, "last segment does not end at ayah 40");
        for (int i = 1; i < n; i++)
            CHECK(seg[i].a0 == seg[i - 1].a1 + 1 ||
                  (seg[i].a0 == seg[i - 1].a1 && seg[i].w0 == seg[i - 1].w1 + 1),
                  "segments %d/%d do not tile", i - 1, i);
    }

    printf("-- persistence round-trips --\n");
    fresh(D0, HZ_SCOPE_JUZ, 30, true);
    {
        int a = hifz_start_new_portion();
        hifz_grade(a, HZ_GOT, 0);
        hifz_flush();
        int n_before = hifz_portion_count();
        int box_b = P(a)->box, tier_b = P(a)->tier, due_b = P(a)->due_day;
        hifz_init();
        CHECK(hifz_portion_count() == n_before, "portion count lost across reload");
        CHECK(P(a)->box == box_b && P(a)->tier == tier_b && P(a)->due_day == due_b,
              "portion state lost across reload");
        CHECK(hifz_scope().kind == HZ_SCOPE_JUZ && hifz_scope().reverse,
              "scope lost across reload");
    }

    printf("-- a torn write falls back to the other slot --\n");
    for (int which = 0; which < 2; which++) {
        fresh(D0, HZ_SCOPE_JUZ, 30, true);
        int a = hifz_start_new_portion();
        hifz_grade(a, HZ_GOT, 0);      // save #1
        hifz_grade(a, HZ_GOT, 0);      // save #2 -> other slot
        store_corrupt(which ? "hifz.a" : "hifz.b", 200);
        hifz_init();
        CHECK(hifz_portion_count() > 0, "corrupting %s lost all state",
              which ? "hifz.a" : "hifz.b");
    }
    {   // both slots truncated -> empty, never half-loaded
        fresh(D0, HZ_SCOPE_JUZ, 30, true);
        int a = hifz_start_new_portion();
        hifz_grade(a, HZ_GOT, 0);
        hifz_grade(a, HZ_GOT, 0);
        store_truncate("hifz.a", 100);
        store_truncate("hifz.b", 100);
        hifz_init();
        CHECK(hifz_portion_count() == 0, "truncated slots half-loaded (%d portions)",
              hifz_portion_count());
    }

    printf("-- no clock: everything but dates keeps working --\n");
    store_reset();
    g_epoch = 0;
    hifz_init();
    hifz_set_scope(HZ_SCOPE_JUZ, 30, true);
    {
        int a = hifz_start_new_portion();
        CHECK(a >= 0, "could not carve a portion without a clock");
        CHECK(P(a)->due_day == 0, "due date invented with no clock");
        hifz_grade(a, HZ_GOT, 0);
        hifz_grade(a, HZ_GOT, 0);
        CHECK(P(a)->tier == HZ_SABQI, "tier did not advance without a clock");
        const HifzPlan *pl = hifz_plan();
        CHECK(!pl->have_day, "claimed a day with no clock");
        CHECK(pl->n_sabqi == 1, "sabqi not listed in cycle mode");

        printf("-- ...and a late sync preserves boxes, only fixing dates --\n");
        int box_b = P(a)->box, tier_b = P(a)->tier, streak_b = P(a)->streak;
        set_day(D0 + 100);
        CHECK(P(a)->box == box_b, "late clock sync changed the box");
        CHECK(P(a)->tier == tier_b, "late clock sync changed the tier");
        CHECK(P(a)->streak == streak_b, "late clock sync changed the streak");
        CHECK(P(a)->due_day == D0 + 100, "due date not set to today on sync");
        CHECK(P(a)->created_day == D0 + 100,
              "created_day left at 0 — the portion could never graduate");
        CHECK(hifz_plan()->have_day, "day not picked up after sync");
    }

    printf("-- a backwards clock jump produces no negative intervals --\n");
    fresh(D0 + 50, HZ_SCOPE_JUZ, 30, true);
    {
        int a = hifz_start_new_portion();
        hifz_grade(a, HZ_GOT, 0); hifz_grade(a, HZ_GOT, 0);
        set_day(D0 + 40);                       // bad NTP / timezone edit
        hifz_grade(a, HZ_GOT, 0);
        CHECK(P(a)->due_day >= D0 + 40, "due date landed in the past");
        CHECK(P(a)->last_day <= D0 + 40, "last_day exceeded today");
    }

    printf("-- strength tracks grades per ayah --\n");
    fresh(D0, HZ_SCOPE_SURAH, 114, false);
    {
        int a = hifz_start_new_portion();
        CHECK(hifz_strength(114, 1) == 0, "strength before any grade");
        hifz_grade(a, HZ_GOT, 0);
        CHECK(hifz_strength(114, 1) > 0, "strength did not rise on GOT");
        CHECK(hifz_surah_frac(114) > 0.f, "surah fraction did not move");
    }

    if (fails == 0) printf("\nhifz-test: all checks passed\n");
    else            printf("\nhifz-test: %d FAILURES\n", fails);
    return fails != 0;
}
