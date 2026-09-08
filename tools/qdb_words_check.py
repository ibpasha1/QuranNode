#!/usr/bin/env python3
"""qdb_words_check.py — verify qdb_word_count() matches the real glyph packs.

The whole point of AYAH_WORDS in quran_db.c is that it equals the number of word
boxes each ayah's .qgp carries, so a word index means the same thing to the veil
masks, the word-by-word meanings, and the per-word verdict marks. Both are built
from tools/quran-tajweed/quran-uthmani.txt, but they're built by *different*
scripts (build_db.py vs shape_quran.py) — so re-shaping the packs with a
different text would silently desync them. This catches that.

It also reports how far the .qtm timing split diverges, which is the reason
qdb_words_agree() exists at all.

    python3 tools/qdb_words_check.py [--packs reader_sm]
"""
import argparse
import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def table_from_c():
    """Parse AYAH_WORDS[] out of the generated quran_db.c."""
    src = open(os.path.join(ROOT, "core", "quran", "quran_db.c"), encoding="utf-8").read()
    m = re.search(r"AYAH_WORDS\[QDB_AYAH_TOTAL\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit("AYAH_WORDS not found in quran_db.c — run tools/build_db.py")
    return [int(v) for v in m.group(1).replace("\n", "").split(",") if v.strip()]


def surah_first():
    """Global index of each surah's first ayah, from SURAH_FIRST[] in the C."""
    src = open(os.path.join(ROOT, "core", "quran", "quran_db.c"), encoding="utf-8").read()
    m = re.search(r"SURAH_FIRST\[QDB_SURAH_COUNT\]\s*=\s*\{(.*?)\};", src, re.S)
    return [int(v) for v in m.group(1).replace("\n", "").split(",") if v.strip()]


def pack_word_counts(pack, surah):
    """(ayah -> n_words) from a .qgp index, or None if the pack is absent."""
    p = os.path.join(ROOT, "sdcard", "packs", pack, "%d.qgp" % surah)
    if not os.path.exists(p):
        return None
    d = open(p, "rb").read()
    if d[:4] != b"QNGP":
        return None
    n, ioff = struct.unpack_from("<II", d, 12)
    out = {}
    for i in range(n):
        s, a, _bo, _w, _h, nw, _pad = struct.unpack_from("<HHIHHHH", d, ioff + i * 16)
        out[a] = nw
    return out


def timing_word_counts(surah):
    p = os.path.join(ROOT, "sdcard", "quran", "timings", "%d.qtm" % surah)
    if not os.path.exists(p):
        return None
    d = open(p, "rb").read()
    if d[:4] != b"QNTM":
        return None
    _ver, _s, nay = struct.unpack_from("<HHH", d, 4)
    out, off = {}, 12
    for _ in range(nay):
        ayah, nw = struct.unpack_from("<HH", d, off)
        off += 4 + nw * 8
        out[ayah] = nw
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--packs", default="reader_sm")
    args = ap.parse_args()

    words, first = table_from_c(), surah_first()
    print("AYAH_WORDS: %d entries, %d words total" % (len(words), sum(words)))

    checked = bad = 0
    missing_packs = 0
    for s in range(1, 115):
        pk = pack_word_counts(args.packs, s)
        if pk is None:
            missing_packs += 1
            continue
        for ayah, nw in pk.items():
            g = first[s - 1] + ayah - 1
            if g - 1 >= len(words):
                continue
            checked += 1
            if words[g - 1] != nw:
                bad += 1
                if bad <= 10:
                    print("  MISMATCH %d:%d  table=%d  pack=%d"
                          % (s, ayah, words[g - 1], nw))

    # Informational: how far the timing split diverges (why qdb_words_agree exists).
    t_checked = t_bad = 0
    for s in range(1, 115):
        tm = timing_word_counts(s)
        if tm is None:
            continue
        for ayah, nw in tm.items():
            g = first[s - 1] + ayah - 1
            if g - 1 >= len(words):
                continue
            t_checked += 1
            if words[g - 1] != nw:
                t_bad += 1

    if missing_packs:
        print("note: %d surahs had no '%s' pack on disk (skipped)"
              % (missing_packs, args.packs))
    if t_checked:
        print("timings vs table: %d/%d ayat disagree (%.1f%%) — this is expected "
              "and is why qdb_words_agree() exists"
              % (t_bad, t_checked, 100.0 * t_bad / t_checked))

    if checked == 0:
        print("FAIL: no packs available to check against")
        return 1
    if bad:
        print("FAIL: %d/%d ayat disagree with the glyph packs" % (bad, checked))
        return 1
    print("OK: table matches all %d checked ayat in packs/%s" % (checked, args.packs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
