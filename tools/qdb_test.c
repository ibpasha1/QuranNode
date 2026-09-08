// qdb_test.c — invariants for the generated Quran metadata tables.
//
// The khatm coverage bitmap indexes ayat by their global mushaf ordinal, and
// all page math assumes each ayah belongs to exactly one page. Both are
// properties of the GENERATED tables, so assert them here rather than trusting
// that tools/build_db.py fetched a sane snapshot. No HAL, no SDL — just the db.
#include "quran_db.h"
#include <stdio.h>

static int fails = 0;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

int main(void)
{
    // --- global index is a bijection onto 1..6236, in mushaf order ---------
    int g = 0;
    for (int s = 1; s <= QDB_SURAH_COUNT; s++) {
        for (int a = 1; a <= qdb_ayah_count(s); a++) {
            int idx = qdb_global_index(s, a);
            CHECK(idx == ++g, "global_index(%d:%d) = %d, expected %d", s, a, idx, g);
            QRef r = qdb_from_global(idx);
            CHECK(r.surah == s && r.ayah == a,
                  "round-trip %d:%d -> %d -> %d:%d", s, a, idx, r.surah, r.ayah);
        }
    }
    CHECK(g == QDB_AYAH_TOTAL, "walked %d ayat, expected %d", g, QDB_AYAH_TOTAL);

    // --- out-of-range refs are rejected, not clamped -----------------------
    CHECK(qdb_global_index(0, 1) == 0, "surah 0 accepted");
    CHECK(qdb_global_index(115, 1) == 0, "surah 115 accepted");
    CHECK(qdb_global_index(1, 0) == 0, "ayah 0 accepted");
    CHECK(qdb_global_index(1, 8) == 0, "1:8 accepted (Al-Fatihah has 7)");
    CHECK(qdb_from_global(0).surah == 0, "gidx 0 accepted");
    CHECK(qdb_from_global(QDB_AYAH_TOTAL + 1).surah == 0, "gidx 6237 accepted");
    CHECK(qdb_page_of(0, 0) == 0, "invalid ref got a page");

    // --- every ayah is on exactly one page; pages partition the mushaf -----
    // This is what makes per-page read fractions sum to exactly 604.
    int total = 0, prev_page = 0;
    for (int p = 1; p <= QDB_PAGE_COUNT; p++) {
        int n = qdb_page_ayah_count(p);
        CHECK(n >= 1, "page %d has %d ayat", p, n);
        total += n;
        int first = qdb_page_first_global(p);
        CHECK(first == prev_page + 1 || p == 1,
              "page %d starts at %d, previous ended at %d", p, first, prev_page);
        prev_page = first + n - 1;
        // Every ayah attributed to the page must agree that it's on it.
        for (int i = first; i < first + n; i++)
            CHECK(qdb_page_of_global(i) == p,
                  "gidx %d says page %d, expected %d", i, qdb_page_of_global(i), p);
    }
    CHECK(total == QDB_AYAH_TOTAL, "pages cover %d ayat, expected %d",
          total, QDB_AYAH_TOTAL);

    // --- page start/end round-trip ----------------------------------------
    for (int p = 1; p <= QDB_PAGE_COUNT; p++) {
        QRef s = qdb_page_start(p), e = qdb_page_end(p);
        CHECK(qdb_page_of(s.surah, s.ayah) == p, "page %d start off-page", p);
        CHECK(qdb_page_of(e.surah, e.ayah) == p, "page %d end off-page", p);
        CHECK(qdb_global_index(e.surah, e.ayah) -
              qdb_global_index(s.surah, s.ayah) + 1 == qdb_page_ayah_count(p),
              "page %d start..end span disagrees with ayah count", p);
    }

    // --- known landmarks (spot-checked against the Madani mushaf) ----------
    CHECK(qdb_page_of(1, 1) == 1, "1:1 is not on page 1");
    CHECK(qdb_page_of(2, 1) == 2, "2:1 is not on page 2");
    CHECK(qdb_page_of(2, 282) == 48, "2:282 (the debt verse) is not on page 48");
    CHECK(qdb_page_of(112, 1) == 604, "112:1 is not on page 604");
    CHECK(qdb_page_of(114, 6) == 604, "the last ayah is not on page 604");
    CHECK(qdb_global_index(1, 1) == 1, "1:1 is not global index 1");
    CHECK(qdb_global_index(114, 6) == QDB_AYAH_TOTAL, "last ayah index wrong");

    // --- juz pages are ascending and start at page 1 -----------------------
    int last = 0;
    for (int j = 1; j <= QDB_JUZ_COUNT; j++) {
        int p = qdb_juz_page(j);
        CHECK(p > last, "juz %d starts on page %d, not after %d", j, p, last);
        last = p;
    }
    CHECK(qdb_juz_page(1) == 1, "juz 1 does not start on page 1");

    // --- the milli-page credit scheme sums exactly (no drift) --------------
    // credit() differences mp_of() so a fully-read page is always exactly 1000
    // milli-pages, regardless of how many ayat it holds. 1000/n per ayah would
    // drift (1000/42*42 = 966); this asserts the differencing is exact.
    unsigned long mp_total = 0;
    for (int p = 1; p <= QDB_PAGE_COUNT; p++) {
        int n = qdb_page_ayah_count(p);
        unsigned long page_mp = 0;
        for (int c = 0; c < n; c++) {
            unsigned long a = (1000UL * (c + 1) + n / 2) / n;
            unsigned long b = (1000UL * c + n / 2) / n;
            page_mp += a - b;
        }
        CHECK(page_mp == 1000, "page %d (%d ayat) sums to %lu mpages, not 1000",
              p, n, page_mp);
        mp_total += page_mp;
    }
    CHECK(mp_total == 604000UL, "whole mushaf sums to %lu mpages, not 604000",
          mp_total);

    if (fails == 0) printf("qdb-test: all invariants hold (%d ayat, %d pages)\n",
                           QDB_AYAH_TOTAL, QDB_PAGE_COUNT);
    else            printf("qdb-test: %d FAILURES\n", fails);
    return fails != 0;
}
