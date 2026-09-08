#!/usr/bin/env python3
"""build_db.py — generate core/quran/quran_db.{c,h} from the Quran.com API.

Fetches the 114 chapters (English + Arabic name, ayah count, revelation place),
the 30 juz boundaries, and the Madani mushaf page of every ayah, and emits a
compiled-in C table. Metadata is tiny and always needed, so it lives in flash
rather than on the SD card. Re-run to refresh.

Page data is fetched per-chapter rather than per-page: 114 requests instead of
604, and each verse carries `id` = its global ayah ordinal 1..6236, which is
exactly the index the khatm coverage bitmap uses.

Word counts come from the LOCAL Uthmani text, not the API, because they must
match the glyph packs exactly — see the WORDS_PATH note below.
"""
import json
import os
import urllib.request

UA = "Mozilla/5.0 QuranNode/0.1"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "core", "quran", "quran_db.c")
OUT_H = os.path.join(ROOT, "core", "quran", "quran_db.h")

TOTAL_AYAT = 6236    # ayat in the mushaf
TOTAL_PAGES = 604    # pages in the Madani mushaf

# Word counts MUST come from the same file tools/shape_quran.py shapes, because
# the glyph packs' word boxes are exactly `text.split(" ")` of these lines. The
# API's word list disagrees for 761 of 6236 ayat (12.2%) — and this file bakes
# the basmala into ayah 1 of every surah but 1 and 9, so those ayat carry 4
# extra words the timings know nothing about. Anything that indexes words on
# screen (veil masks, meanings, per-word verdicts) must use THIS count.
WORDS_PATH = os.path.join(ROOT, "tools", "quran-tajweed", "quran-uthmani.txt")


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req) as r:
        return json.load(r)


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def qdb_page_n(page_first, p):
    """Ayat attributed to page p — mirrors qdb_page_ayah_count() in C."""
    nxt = TOTAL_AYAT + 1 if p == TOTAL_PAGES else page_first[p + 1]
    return nxt - page_first[p]


def load_word_counts():
    """(surah, ayah) -> glyph-pack word count, from the local Uthmani text."""
    out = {}
    with open(WORDS_PATH, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("|")
            if len(parts) < 3:
                continue
            out[(int(parts[0]), int(parts[1]))] = len(parts[2].split())
    return out


def main():
    chapters = get("https://api.quran.com/api/v4/chapters")["chapters"]
    juzs = get("https://api.quran.com/api/v4/juzs")["juzs"]
    words = load_word_counts()

    chapters.sort(key=lambda c: c["id"])

    # Juz start = the earliest surah:ayah in its verse_mapping.
    juz_start = {}
    for j in juzs:
        n = j["juz_number"]
        best = None
        for surah, rng in j["verse_mapping"].items():
            a0 = int(str(rng).split("-")[0])
            ref = (int(surah), a0)
            if best is None or ref < best:
                best = ref
        juz_start[n] = best

    # Per-verse Madani page number. verse["id"] is the global ayah ordinal.
    verses = []
    for c in range(1, 115):
        d = get("https://api.quran.com/api/v4/verses/by_chapter/"
                "%d?fields=page_number&per_page=300" % c)
        for v in d["verses"]:
            s, a = (int(x) for x in v["verse_key"].split(":"))
            verses.append((v["id"], s, a, v["page_number"]))
    verses.sort()

    # Invariants the C lookups rely on. Fail the build rather than ship silent
    # corruption if the API ever changes shape.
    assert [v[0] for v in verses] == list(range(1, TOTAL_AYAT + 1)), \
        "global ayah index is not contiguous 1..%d" % TOTAL_AYAT
    pages = [v[3] for v in verses]
    assert all(pages[i] <= pages[i + 1] for i in range(len(pages) - 1)), \
        "page numbers are not monotone over the global index"

    page_first = {}    # page  -> first global ayah id on it
    surah_first = {}   # surah -> first global ayah id in it
    for gid, s, a, p in verses:
        page_first.setdefault(p, gid)
        surah_first.setdefault(s, gid)
    assert sorted(page_first) == list(range(1, TOTAL_PAGES + 1)), \
        "expected every page 1..%d to have at least one ayah" % TOTAL_PAGES
    assert sorted(surah_first) == list(range(1, 115))

    # Word counts must cover every ayah and fit a uint8.
    assert len(words) == TOTAL_AYAT, \
        "Uthmani text has %d ayat, expected %d" % (len(words), TOTAL_AYAT)
    for (s, a), n in words.items():
        assert 1 <= n <= 255, "word count %d out of uint8 range at %d:%d" % (n, s, a)
    for c in chapters:
        n = c["verses_count"]
        g0 = surah_first[c["id"]]
        assert verses[g0 - 1 + n - 1][1] == c["id"], \
            "surah %d ayah count disagrees with the verse list" % c["id"]

    with open(OUT_H, "w") as f:
        f.write("""// quran_db.h — Quran metadata (surahs + juz). GENERATED by tools/build_db.py.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct { int surah, ayah; } QRef;

typedef struct {
    const char *name_en;   // e.g. "Al-Fatihah"
    const char *name_ar;   // UTF-8 Arabic name (for glyph-pack rendering)
    uint16_t    ayat;      // number of ayat
    uint8_t     makki;     // 1 = Meccan, 0 = Medinan
} SurahInfo;

#define QDB_SURAH_COUNT 114
#define QDB_JUZ_COUNT   30
#define QDB_PAGE_COUNT  604
#define QDB_AYAH_TOTAL  6236

const SurahInfo *qdb_surah(int surah);   // 1-based; NULL if out of range
const char *qdb_surah_name(int surah);   // English name, "" if invalid
int  qdb_ayah_count(int surah);
QRef qdb_juz_start(int juz);             // 1-based
int  qdb_juz_of(int surah, int ayah);    // which juz a ref falls in (1..30)

// --- Flat mushaf ordinal --------------------------------------------------
// Every ayah has a global index 1..6236 in mushaf order. It is the natural key
// for anything that needs one bit or one slot per ayah (see khatm coverage).
int  qdb_global_index(int surah, int ayah);   // 0 if the ref is invalid
QRef qdb_from_global(int gidx);               // {0,0} if out of range

// --- Madani mushaf pages --------------------------------------------------
// An ayah belongs to exactly ONE page (the QPC attribution) even when its text
// visually spans a page break. So per-page ayah counts sum to 6236 and per-page
// read fractions sum to exactly 604 — the invariant the page math relies on.
int  qdb_page_of(int surah, int ayah);        // 1..604, 0 if invalid
int  qdb_page_of_global(int gidx);            // 1..604, 0 if invalid
int  qdb_page_first_global(int page);         // first global ayah on the page
int  qdb_page_ayah_count(int page);           // ayat attributed to the page
QRef qdb_page_start(int page);                // first ayah on the page
QRef qdb_page_end(int page);                  // last ayah attributed to it
int  qdb_juz_page(int juz);                   // page a juz starts on

// --- Words ----------------------------------------------------------------
// The number of words an ayah is drawn with — i.e. how many word boxes its
// glyph pack carries, since both come from the same Uthmani text. This is the
// ONLY authoritative word index on the device.
//
// Beware: the .qtm timing tables use a different word split and disagree for
// 761 of the 6236 ayat (12.2%). Ayah 1 of every surah except 1 and 9 also
// carries a 4-word basmala prefix that the timings don't have. So anything
// mapping a timing segment onto a drawn word MUST check qdb_words_agree()
// first and fall back to an ayah-level treatment — silently min()-ing the two
// counts puts the marks under the wrong words.
int  qdb_word_count(int surah, int ayah);
bool qdb_words_agree(int surah, int ayah, int timing_word_count);
""")

    with open(OUT_C, "w") as f:
        f.write('// quran_db.c — GENERATED by tools/build_db.py. Do not edit by hand.\n')
        f.write('#include "quran_db.h"\n#include <stddef.h>\n\n')
        f.write("static const SurahInfo SURAHS[QDB_SURAH_COUNT] = {\n")
        for c in chapters:
            f.write("    { %s, %s, %d, %d },\n" % (
                c_str(c["name_simple"]),
                c_str(c["name_arabic"]),
                c["verses_count"],
                1 if c["revelation_place"] == "makkah" else 0,
            ))
        f.write("};\n\n")

        f.write("static const QRef JUZ_START[QDB_JUZ_COUNT] = {\n")
        for n in range(1, 31):
            s, a = juz_start[n]
            f.write("    { %d, %d },\n" % (s, a))
        f.write("};\n\n")

        # First global ayah index of each surah / page. Both bounds are < 65536,
        # so uint16 is enough: 228 B + 1208 B of flash. A per-ayah page table
        # would be 12.5 KB to save a 10-step binary search — not worth it.
        f.write("// First global ayah index (1..%d) of each surah.\n" % TOTAL_AYAT)
        f.write("static const uint16_t SURAH_FIRST[QDB_SURAH_COUNT] = {\n")
        for i in range(0, 114, 12):
            row = ", ".join("%d" % surah_first[s] for s in range(i + 1, min(i + 13, 115)))
            f.write("    %s,\n" % row)
        f.write("};\n\n")

        f.write("// First global ayah index of each Madani page. Monotone, so\n"
                "// page p covers [PAGE_FIRST[p-1], PAGE_FIRST[p] - 1].\n")
        f.write("static const uint16_t PAGE_FIRST[QDB_PAGE_COUNT] = {\n")
        for i in range(0, TOTAL_PAGES, 12):
            row = ", ".join("%d" % page_first[p]
                            for p in range(i + 1, min(i + 13, TOTAL_PAGES + 1)))
            f.write("    %s,\n" % row)
        f.write("};\n\n")

        # Words per ayah, indexed by global ordinal - 1. uint8 is enough: the
        # longest ayah (2:282) is 128 words.
        f.write("// Words per ayah, by global index. Same split as the glyph packs.\n")
        f.write("static const uint8_t AYAH_WORDS[QDB_AYAH_TOTAL] = {\n")
        flat = []
        for c in chapters:
            s = c["id"]
            for a in range(1, c["verses_count"] + 1):
                flat.append(words[(s, a)])
        for i in range(0, len(flat), 20):
            f.write("    %s,\n" % ", ".join("%d" % v for v in flat[i:i + 20]))
        f.write("};\n\n")

        f.write("""const SurahInfo *qdb_surah(int surah) {
    if (surah < 1 || surah > QDB_SURAH_COUNT) return NULL;
    return &SURAHS[surah - 1];
}
const char *qdb_surah_name(int surah) {
    const SurahInfo *s = qdb_surah(surah);
    return s ? s->name_en : "";
}
int qdb_ayah_count(int surah) {
    const SurahInfo *s = qdb_surah(surah);
    return s ? s->ayat : 0;
}
QRef qdb_juz_start(int juz) {
    if (juz < 1 || juz > QDB_JUZ_COUNT) { QRef z = {0, 0}; return z; }
    return JUZ_START[juz - 1];
}
int qdb_juz_of(int surah, int ayah) {
    int found = 1;
    for (int j = 0; j < QDB_JUZ_COUNT; j++) {
        int s = JUZ_START[j].surah, a = JUZ_START[j].ayah;
        if (surah > s || (surah == s && ayah >= a)) found = j + 1;
        else break;
    }
    return found;
}

int qdb_global_index(int surah, int ayah) {
    const SurahInfo *s = qdb_surah(surah);
    if (!s || ayah < 1 || ayah > (int)s->ayat) return 0;
    return (int)SURAH_FIRST[surah - 1] + ayah - 1;
}

QRef qdb_from_global(int gidx) {
    QRef z = {0, 0};
    if (gidx < 1 || gidx > QDB_AYAH_TOTAL) return z;
    // Last surah whose first index is <= gidx.
    int lo = 0, hi = QDB_SURAH_COUNT - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if ((int)SURAH_FIRST[mid] <= gidx) lo = mid; else hi = mid - 1;
    }
    z.surah = lo + 1;
    z.ayah  = gidx - (int)SURAH_FIRST[lo] + 1;
    return z;
}

int qdb_page_of_global(int gidx) {
    if (gidx < 1 || gidx > QDB_AYAH_TOTAL) return 0;
    int lo = 0, hi = QDB_PAGE_COUNT - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if ((int)PAGE_FIRST[mid] <= gidx) lo = mid; else hi = mid - 1;
    }
    return lo + 1;
}

int qdb_page_of(int surah, int ayah) {
    return qdb_page_of_global(qdb_global_index(surah, ayah));
}

int qdb_page_first_global(int page) {
    if (page < 1 || page > QDB_PAGE_COUNT) return 0;
    return (int)PAGE_FIRST[page - 1];
}

int qdb_page_ayah_count(int page) {
    if (page < 1 || page > QDB_PAGE_COUNT) return 0;
    int next = (page == QDB_PAGE_COUNT) ? QDB_AYAH_TOTAL + 1
                                        : (int)PAGE_FIRST[page];
    return next - (int)PAGE_FIRST[page - 1];
}

QRef qdb_page_start(int page) {
    return qdb_from_global(qdb_page_first_global(page));
}

QRef qdb_page_end(int page) {
    int first = qdb_page_first_global(page);
    if (!first) { QRef z = {0, 0}; return z; }
    return qdb_from_global(first + qdb_page_ayah_count(page) - 1);
}

int qdb_juz_page(int juz) {
    QRef r = qdb_juz_start(juz);
    return r.surah ? qdb_page_of(r.surah, r.ayah) : 0;
}

int qdb_word_count(int surah, int ayah) {
    int g = qdb_global_index(surah, ayah);
    return g ? (int)AYAH_WORDS[g - 1] : 0;
}

bool qdb_words_agree(int surah, int ayah, int timing_word_count) {
    int n = qdb_word_count(surah, ayah);
    return n > 0 && n == timing_word_count;
}
""")
    print("wrote", OUT_C, "and", OUT_H)
    print("surahs:", len(chapters), "juz starts:", len(juz_start))
    print("ayat:", len(verses), "pages:", len(page_first),
          "ayat/page: min %d max %d" % (
              min(qdb_page_n(page_first, p) for p in range(1, TOTAL_PAGES + 1)),
              max(qdb_page_n(page_first, p) for p in range(1, TOTAL_PAGES + 1))))


if __name__ == "__main__":
    main()
