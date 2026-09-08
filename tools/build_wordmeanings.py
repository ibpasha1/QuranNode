#!/usr/bin/env python3
# build_wordmeanings.py — word-by-word English glosses -> sdcard/quran/wbw/<s>.qwm
#
# Fetches the per-word English translation for every ayah from the quran.com v4
# API and reconciles it to the ONE authoritative word split this device draws
# and selects by: quran-uthmani.txt `.split(" ")`, the same split behind
# qdb_word_count() and the glyph packs. Anything keyed to a word on screen MUST
# use that split; the timing (.qtm) split disagrees for 12.2% of ayat, so we
# never key meanings to it.
#
# Two reconciliations matter:
#   * The Uthmani text bakes the basmala into ayah 1 of every surah except 1 and
#     9, as FOUR extra leading words. quran.com delivers ayah 1 without it, so we
#     prepend four fixed basmala glosses there.
#   * The ayah-end rosette is delivered as a pseudo-word (char_type_name ==
#     "end"); it is filtered out, or it shifts every index after it.
#
# If, after both, the gloss count still != the Uthmani count, we DO NOT shift to
# force a fit — a silently misaligned gloss is worse than none. We emit empty
# glosses for that ayah and log loudly, so the device shows blanks, not lies.
#
# Output format (little-endian), streamed like .qtm (see core/quran/wordmeaning.c):
#   header { "QNWM", u16 version=1, u16 surah, u16 n_ayat, u16 reserved }
#   index[n_ayat] { u16 ayah, u16 n_words, u32 blob_off, u32 blob_len }
#   blob (per ayah) { u16 off[n_words] (rel to blob start), packed NUL-term utf8 }
# Offsets are ayah-LOCAL u16: a surah-global pool would overflow u16 on
# Al-Baqarah (~106 KB of text). ~1.4 MB total across all 114 surahs.
import json
import os
import struct
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORDS_PATH = os.path.join(ROOT, "tools", "quran-tajweed", "quran-uthmani.txt")
OUT_DIR = os.path.join(ROOT, "sdcard", "quran", "wbw")
API = "https://api.quran.com/api/v4/verses/by_chapter/{s}?words=true" \
      "&word_translation_language=en&per_page=50&page={p}"

# The four basmala words, word-for-word, matching the four leading Uthmani words
# baked into ayah 1 of surahs 2-114 (except 9).
BASMALA = ["In (the) name", "(of) Allah", "the Most Gracious", "the Most Merciful"]

MAGIC = b"QNWM"
VERSION = 1


def load_word_counts():
    """(surah, ayah) -> authoritative word count, straight from the draw split."""
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


def fetch_surah(surah):
    """ayah -> [gloss, ...] from quran.com, rosette filtered, basmala NOT added."""
    verses = {}
    page = 1
    while True:
        url = API.format(s=surah, p=page)
        for attempt in range(5):
            try:
                with urllib.request.urlopen(url, timeout=30) as r:
                    data = json.load(r)
                break
            except Exception as e:  # noqa: BLE001 — retry any transient failure
                if attempt == 4:
                    raise
                print(f"  retry {surah} p{page}: {e}", file=sys.stderr)
                time.sleep(1.5 * (attempt + 1))
        for v in data["verses"]:
            _, ayah = (int(x) for x in v["verse_key"].split(":"))
            glosses = []
            for w in v["words"]:
                if w.get("char_type_name") != "word":
                    continue  # the ayah-end rosette is a pseudo-word — drop it
                tr = (w.get("translation") or {}).get("text") or ""
                glosses.append(tr.strip())
            verses[ayah] = glosses
        pg = data.get("pagination") or {}
        if not pg.get("next_page"):
            break
        page = pg["next_page"]
        time.sleep(0.2)
    return verses


def write_qwm(path, surah, ayat):
    """ayat: ordered list of (ayah_no, [gloss,...]). Writes the streamed .qwm."""
    n = len(ayat)
    header = MAGIC + struct.pack("<HHHH", VERSION, surah, n, 0)
    index = bytearray()
    blobs = bytearray()
    blob_base = len(header) + n * 12
    for ayah_no, glosses in ayat:
        nw = len(glosses)
        # per-ayah blob: u16 off[nw] then packed NUL-terminated utf8
        encoded = [g.encode("utf-8") + b"\x00" for g in glosses]
        off = []
        pos = nw * 2
        for e in encoded:
            off.append(pos)
            pos += len(e)
        blob = bytearray()
        for o in off:
            blob += struct.pack("<H", o)
        for e in encoded:
            blob += e
        index += struct.pack("<HHII", ayah_no, nw, blob_base + len(blobs), len(blob))
        blobs += blob
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header)
        f.write(index)
        f.write(blobs)


def main():
    counts = load_word_counts()
    only = [int(a) for a in sys.argv[1:]] or range(1, 115)
    mismatches = 0
    for surah in only:
        verses = fetch_surah(surah)
        n_ayat = max(a for (s, a) in counts if s == surah)
        ayat = []
        for ayah in range(1, n_ayat + 1):
            glosses = list(verses.get(ayah, []))
            # Prepend the basmala baked into ayah 1 of 2-114 (except 9).
            if ayah == 1 and surah not in (1, 9):
                glosses = BASMALA + glosses
            want = counts.get((surah, ayah), 0)
            if len(glosses) != want:
                print(f"MISMATCH {surah}:{ayah} got {len(glosses)} want {want} "
                      f"-> emitting {want} EMPTY glosses (never shift)",
                      file=sys.stderr)
                glosses = [""] * want
                mismatches += 1
            ayat.append((ayah, glosses))
        out = os.path.join(OUT_DIR, f"{surah}.qwm")
        write_qwm(out, surah, ayat)
        print(f"wrote {out}: {n_ayat} ayat")
        time.sleep(0.2)
    if mismatches:
        print(f"\n{mismatches} ayah(s) emitted empty (count mismatch) — see log",
              file=sys.stderr)


if __name__ == "__main__":
    main()
