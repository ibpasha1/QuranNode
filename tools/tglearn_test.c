// tglearn_test.c — the Tafsir Game learner: scheduler + persistence, headless.
//
// Harness mirrors tools/hifz_test.c: a controllable clock and an in-memory
// state store stand in for the RTC and SD. We pin the parts that can be quietly
// wrong for weeks — box transitions, due scheduling across day rollovers, the
// scope walk, the dual-slot/CRC persistence and its torn-write fallback, the
// append-only upgrade path, and the clock-unknown degradation.
#include "tglearn.h"
#include "qday.h"
#include "quran_db.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(cond, ...) do {                                             \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

// --- controllable clock ----------------------------------------------------
static uint32_t g_ms = 0;
static int64_t  g_epoch = 0;           // 0 = "clock unknown"
uint32_t plat_millis(void) { return g_ms; }
int64_t hal_wall_clock(void) { return g_epoch; }
int hal_tz_offset_min(void) { return 0; }

#define DAY(n) ((int64_t)(n) * 86400 + 43200)   // noon of day n, UTC
#define D0 20000

// --- in-memory state store --------------------------------------------------
#define MAX_SLOTS 8
#define SLOT_CAP  32768
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

// Mirror of the blob header, just to read `seq` out of a raw stored slot.
typedef struct { uint32_t magic; uint16_t version, bytes; uint32_t seq; } THdr;
static uint32_t slot_seq_raw(int i) { THdr h; memcpy(&h, g_store[i].buf, sizeof h); return h.seq; }

static int slot_index(const char *name)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_store[i].used && strcmp(g_store[i].name, name) == 0) return i;
    return -1;
}
static void store_reset(void) { memset(g_store, 0, sizeof g_store); }

// --- helpers ----------------------------------------------------------------
static void set_day(int n) { g_epoch = DAY(n); tglearn_service(); }

// A fresh learner on a blank store at day D0.
static void boot_fresh(int day)
{
    store_reset();
    g_epoch = day > 0 ? DAY(day) : 0;
    g_ms = 1;                    // past 0 but far under the 5s debounce
    tglearn_init();
}

static int GI(int surah, int ayah) { return qdb_global_index(surah, ayah); }

int main(void)
{
    // -----------------------------------------------------------------------
    printf("-- scope: range, sequential + reverse introduction, new_available --\n");
    {
        boot_fresh(D0);
        tglearn_set_scope(TGL_SCOPE_SURAH, 1, false);   // Al-Fatihah, 7 ayat
        TgScope sc = tglearn_scope();
        CHECK(sc.first_g == (uint16_t)GI(1, 1) && sc.last_g == (uint16_t)GI(1, 7),
              "surah scope range %u..%u", sc.first_g, sc.last_g);
        CHECK(tglearn_new_available() == 7, "new_available %d != 7", tglearn_new_available());

        int g1 = tglearn_start_new(), g2 = tglearn_start_new();
        CHECK(g1 == GI(1, 1), "first new %d != ayah1", g1);
        CHECK(g2 == GI(1, 2), "second new %d != ayah2", g2);
        CHECK(tglearn_item_count() == 2, "item count %d", tglearn_item_count());
        CHECK(tglearn_new_available() == 5, "new_available %d != 5", tglearn_new_available());
        CHECK(tglearn_box(1, 1) == 0, "fresh box != 0");

        // reverse target introduces from the end
        boot_fresh(D0);
        tglearn_set_scope(TGL_SCOPE_SURAH, 114, true);  // An-Nas, 6 ayat, last-first
        int r1 = tglearn_start_new();
        CHECK(r1 == GI(114, 6), "reverse first %d != ayah6", r1);
    }

    // -----------------------------------------------------------------------
    printf("-- box transitions: GOT climbs, WRONG resets, HARD holds --\n");
    {
        boot_fresh(D0);
        tglearn_set_scope(TGL_SCOPE_SURAH, 1, false);
        tglearn_start_new();                            // ayah 1, box 0

        tglearn_grade(1, 1, TG_GOT);
        CHECK(tglearn_box(1, 1) == 1, "GOT: box %d != 1", tglearn_box(1, 1));
        const TgItem *it = tglearn_item(tglearn_item_at(1, 1));
        CHECK(it->due_day == (uint16_t)(D0 + TGL_BOX_DAYS[1]), "GOT due_day %u", it->due_day);
        CHECK(it->streak == 1, "streak %d != 1", it->streak);

        tglearn_grade(1, 1, TG_GOT);
        CHECK(tglearn_box(1, 1) == 2, "GOT2: box %d != 2", tglearn_box(1, 1));

        tglearn_grade(1, 1, TG_HARD);
        CHECK(tglearn_box(1, 1) == 2, "HARD held box %d != 2", tglearn_box(1, 1));
        it = tglearn_item(tglearn_item_at(1, 1));
        CHECK(it->streak == 0, "HARD reset streak? %d", it->streak);

        tglearn_grade(1, 1, TG_WRONG);
        CHECK(tglearn_box(1, 1) == 0, "WRONG: box %d != 0", tglearn_box(1, 1));
        it = tglearn_item(tglearn_item_at(1, 1));
        CHECK(it->lapses == 1, "WRONG lapses %d != 1", it->lapses);
        CHECK(it->due_day == (uint16_t)D0, "WRONG due_day %u != today", it->due_day);
    }

    // -----------------------------------------------------------------------
    printf("-- mastery caps at the top box --\n");
    {
        boot_fresh(D0);
        tglearn_set_scope(TGL_SCOPE_SURAH, 1, false);
        tglearn_start_new();
        for (int k = 0; k < 20; k++) tglearn_grade(1, 1, TG_GOT);
        CHECK(tglearn_box(1, 1) == TGL_BOX_COUNT - 1, "box didn't cap: %d", tglearn_box(1, 1));
        const TgStats *st = tglearn_stats();
        CHECK(st->mastered == 1, "mastered %d != 1", st->mastered);
    }

    // -----------------------------------------------------------------------
    printf("-- due scheduling across day rollover --\n");
    {
        boot_fresh(D0);
        tglearn_set_scope(TGL_SCOPE_SURAH, 1, false);
        tglearn_start_new();
        tglearn_grade(1, 1, TG_GOT);                    // box1, due D0+1
        CHECK(tglearn_due_count() == 0, "should not be due same day");
        set_day(D0 + 1);
        CHECK(tglearn_due_count() == 1, "should be due next day");
        int q[16], n = tglearn_today(q, 16);
        CHECK(n == 1 && q[0] == GI(1, 1), "today() didn't surface the due ayah");
    }

    // -----------------------------------------------------------------------
    printf("-- today() order: most overdue first --\n");
    {
        boot_fresh(D0);
        tglearn_set_scope(TGL_SCOPE_SURAH, 1, false);
        tglearn_start_new();                            // ayah 1
        tglearn_start_new();                            // ayah 2
        tglearn_grade(1, 1, TG_WRONG);                  // box0, due D0
        tglearn_grade(1, 2, TG_GOT);                    // box1, due D0+1
        set_day(D0 + 5);                                // a1 overdue 5, a2 overdue 4
        int q[16], n = tglearn_today(q, 16);
        CHECK(n == 2, "expected 2 due, got %d", n);
        CHECK(q[0] == GI(1, 1), "most-overdue ayah not first");
        CHECK(q[1] == GI(1, 2), "second-overdue ayah not second");
    }

    // -----------------------------------------------------------------------
    printf("-- persistence round-trips scope + item state --\n");
    {
        boot_fresh(D0);
        tglearn_set_scope(TGL_SCOPE_JUZ, 30, false);
        int g = tglearn_start_new();
        QRef r = qdb_from_global(g);
        tglearn_grade(r.surah, r.ayah, TG_GOT);
        tglearn_grade(r.surah, r.ayah, TG_GOT);         // box 2
        tglearn_flush();

        tglearn_init();                                 // reload from the store
        TgScope sc = tglearn_scope();
        CHECK(sc.kind == TGL_SCOPE_JUZ && sc.label_arg == 30, "scope lost across reload");
        CHECK(tglearn_item_count() == 1, "items lost: %d", tglearn_item_count());
        CHECK(tglearn_box(r.surah, r.ayah) == 2, "box lost: %d", tglearn_box(r.surah, r.ayah));
    }

    // -----------------------------------------------------------------------
    printf("-- torn write: newer slot corrupt => older slot loads --\n");
    {
        boot_fresh(D0);
        tglearn_set_scope(TGL_SCOPE_SURAH, 1, false);
        tglearn_start_new();
        tglearn_grade(1, 1, TG_GOT);
        tglearn_flush();                                // seq S -> slot X
        tglearn_grade(1, 1, TG_GOT);                    // box 2
        tglearn_flush();                                // seq S+1 -> slot Y (newer)

        // Corrupt the newer slot's body so its CRC fails.
        int ia = slot_index("tglearn.a"), ib = slot_index("tglearn.b");
        CHECK(ia >= 0 && ib >= 0, "both slots should exist after 2 saves");
        int newer = slot_seq_raw(ia) >= slot_seq_raw(ib) ? ia : ib;
        g_store[newer].buf[300] ^= 0xFF;                // flip a CRC-covered byte

        tglearn_init();                                 // must fall back to older
        CHECK(tglearn_box(1, 1) == 1, "fallback box %d != 1 (older slot)", tglearn_box(1, 1));
    }

    // -----------------------------------------------------------------------
    printf("-- clock-unknown: boxes advance, everything 'due whenever' --\n");
    {
        boot_fresh(0);                                  // no clock
        CHECK(qday_today() == 0, "clock should be unknown");
        tglearn_set_scope(TGL_SCOPE_SURAH, 1, false);
        int g = tglearn_start_new();
        CHECK(g == GI(1, 1), "no-clock introduce failed");
        const TgItem *it = tglearn_item(tglearn_item_at(1, 1));
        CHECK(it->due_day == 0, "no-clock due_day should be 0");
        CHECK(tglearn_due_count() == 1, "no-clock item should read as due");
        tglearn_grade(1, 1, TG_GOT);
        CHECK(tglearn_box(1, 1) == 1, "no-clock box didn't advance");
        it = tglearn_item(tglearn_item_at(1, 1));
        CHECK(it->due_day == 0, "no-clock due_day still 0 after grade");
        // clock arrives: scheduling resumes without losing the box
        set_day(D0);
        CHECK(tglearn_box(1, 1) == 1, "box lost when clock arrived");
        CHECK(tglearn_due_count() == 1, "due-whenever item should still be due");
    }

    if (fails) { printf("\ntglearn-test: %d checks FAILED\n", fails); return 1; }
    printf("\ntglearn-test: all checks passed\n");
    return 0;
}
