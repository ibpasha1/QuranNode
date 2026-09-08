#!/usr/bin/env python3
"""build_waqf_text.py — add waqf (pause) marks to the Uthmani text + remap tajweed.

The glyph packs are shaped from tools/quran-tajweed/quran-uthmani.txt, the exact
Tanzil Uthmani text the cpfair tajweed annotations index into (codepoint offsets
must line up). That text carries NO waqf/pause stop-signs, so the reader shows
none. This tool pulls the pause marks from quran.com's Uthmani script, injects
ONLY those marks into our text (letters untouched), and rewrites the tajweed
annotation offsets so the colours still land on the right letters.

Two invariants make this safe:
  * We insert only waqf-sign codepoints, never spaces — so the word count (words
    split on ASCII space) is identical, and the per-word recitation timing/
    highlight stays aligned.
  * Marks cling to the preceding word (inserted before any run of spaces), which
    is where a waqf sign belongs.

Outputs (consumed by shape_quran.py --text-file/--annot-file):
  tools/quran-tajweed/quran-uthmani-waqf.txt          surah|ayah|augmented-text
  tools/quran-tajweed/output/tajweed.hafs.waqf.json   annotations in augmented coords
"""
import argparse
import difflib
import json
import os
import urllib.request

TAJWEED_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "quran-tajweed")
OURS = os.path.join(TAJWEED_DIR, "quran-uthmani.txt")
ANNOT = os.path.join(TAJWEED_DIR, "output", "tajweed.hafs.uthmani-pause-sajdah.json")
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".cache")
UA = {"User-Agent": "Mozilla/5.0 QuranNode/0.1"}

# Waqf / pause stop-signs (ARABIC SMALL HIGH ...): sad-lam-alef, qala, meem,
# lam-alef, jeem, three-dots, seen — plus the sajdah sign. NOT the end-of-ayah
# rosette (06DD), rub-el-hizb (06DE), or tatweel (0640).
WAQF = set(range(0x06D6, 0x06DD)) | {0x06E9}


def load_ours():
    out = {}
    for line in open(OURS, encoding="utf-8"):
        line = line.strip()
        if "|" in line:
            s, a, t = line.split("|", 2)
            out[(int(s), int(a))] = t
    return out


def fetch_qcom(surah):
    """{ayah: uthmani-text-with-waqf} for a surah, cached under tools/.cache."""
    os.makedirs(CACHE, exist_ok=True)
    cp = os.path.join(CACHE, f"qcom_uthmani_{surah}.json")
    if os.path.exists(cp) and os.path.getsize(cp) > 0:
        d = json.load(open(cp, encoding="utf-8"))
    else:
        url = f"https://api.quran.com/api/v4/quran/verses/uthmani?chapter_number={surah}"
        with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
            d = json.load(r)
        json.dump(d, open(cp, "w", encoding="utf-8"), ensure_ascii=False)
    return {int(v["verse_key"].split(":")[1]): v["text_uthmani"] for v in d["verses"]}


def augment(ours, qcom):
    """Inject waqf marks from `qcom` into `ours`. Returns (augmented_text,
    orig2aug) where orig2aug[k] is the augmented index of original char k."""
    sm = difflib.SequenceMatcher(a=list(ours), b=list(qcom), autojunk=False)
    inserts = {}   # original index -> [mark chars] to place before that index
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        marks = [c for c in qcom[j1:j2] if ord(c) in WAQF]
        if not marks:
            continue
        p = i1
        while p > 0 and ours[p - 1] == " ":   # cling to the preceding word
            p -= 1
        inserts.setdefault(p, []).extend(marks)

    aug, orig2aug = [], [0] * (len(ours) + 1)
    for k in range(len(ours) + 1):
        for m in inserts.get(k, []):
            aug.append(m)
        orig2aug[k] = len(aug)
        if k < len(ours):
            aug.append(ours[k])
    return "".join(aug), orig2aug


def main():
    ap = argparse.ArgumentParser(description="Inject waqf marks + remap tajweed offsets")
    ap.add_argument("--surahs", default="1-114", help="e.g. '2' or '1,78-114'")
    ap.add_argument("--text-out", default=os.path.join(TAJWEED_DIR, "quran-uthmani-waqf.txt"))
    ap.add_argument("--annot-out", default=os.path.join(TAJWEED_DIR, "output", "tajweed.hafs.waqf.json"))
    args = ap.parse_args()

    surahs = []
    for part in args.surahs.split(","):
        if "-" in part:
            a, b = part.split("-"); surahs += range(int(a), int(b) + 1)
        elif part.strip():
            surahs.append(int(part))

    ours = load_ours()
    annot = json.load(open(ANNOT, encoding="utf-8"))
    annot_by = {(e["surah"], e["ayah"]): e for e in annot}

    text_lines, new_annot = [], []
    total_marks = warnings = 0
    for surah in surahs:
        qcom = fetch_qcom(surah)
        n = max(a for (s, a) in ours if s == surah)
        for ayah in range(1, n + 1):
            o = ours[(surah, ayah)]
            aug, o2a = augment(o, qcom.get(ayah, o))
            # Invariant: word count (spaces) must be unchanged or timing desyncs.
            if aug.count(" ") != o.count(" "):
                warnings += 1
                print(f"  !! {surah}:{ayah} space count changed — skipping marks")
                aug, o2a = o, list(range(len(o) + 1))
            total_marks += len(aug) - len(o)
            text_lines.append(f"{surah}|{ayah}|{aug}")

            e = annot_by.get((surah, ayah))
            anns = []
            if e:
                for a in e["annotations"]:
                    s0, e0 = a["start"], a["end"]
                    if e0 <= 0 or s0 >= len(o):
                        continue
                    ns = o2a[min(s0, len(o))]
                    ne = o2a[min(e0, len(o)) - 1] + 1   # exclude a mark at the boundary
                    anns.append({"start": ns, "end": ne, "rule": a["rule"]})
            new_annot.append({"surah": surah, "ayah": ayah, "annotations": anns})

    with open(args.text_out, "w", encoding="utf-8") as f:
        f.write("\n".join(text_lines) + "\n")
    json.dump(new_annot, open(args.annot_out, "w", encoding="utf-8"), ensure_ascii=False)
    print(f"wrote {args.text_out}  ({len(text_lines)} ayat, +{total_marks} waqf marks, "
          f"{warnings} warnings)")
    print(f"wrote {args.annot_out}")


if __name__ == "__main__":
    main()
