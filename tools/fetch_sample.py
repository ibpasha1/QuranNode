#!/usr/bin/env python3
"""fetch_sample.py — build the bundled recitation + word-timing sample.

Audio and word timings MUST come from the same recording or the highlight drifts,
so we take both from Quran.com's `qdc` API for one reciter (Abdul Basit, Murattal):
download the full-surah mp3, slice it per-ayah at the verse boundaries with ffmpeg,
and store word timings normalized to each ayah's start. Output mirrors the
on-device SD layout:

  sdcard/audio/abdulbasit/<surah>/<ayah>.mp3
  sdcard/quran/timings/<surah>.qtm     (see format below / core/quran/timing.c)

Timing file (.qtm, little-endian):
  header { magic "QNTM", u16 version, u16 surah, u16 n_ayat, u16 reserved }
  per ayah { u16 ayah, u16 n_words, n_words x (u32 start_ms, u32 end_ms) }  # ayah-relative
"""
import json
import os
import struct
import subprocess
import urllib.request

RECITER_ID = 2                      # AbdulBaset AbdulSamad, Murattal
RECITER_DIR = "abdulbasit"
QDC = "https://api.qurancdn.com/api/qdc/audio/reciters/{rid}/audio_files?chapter={ch}&segments=true"

# (surah, first_ayah, last_ayah) — M1a proves the loop on Al-Fatiha; extend here
# as more shaped text lands (Al-Mulk, Al-Kahf 1-20).
SAMPLE = [(1, 1, 7)]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AUDIO_ROOT = os.path.join(ROOT, "sdcard", "audio", RECITER_DIR)
TIMING_ROOT = os.path.join(ROOT, "sdcard", "quran", "timings")
CACHE = os.path.join(ROOT, "tools", ".cache")


UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) QuranNode/0.1"


def _get_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req) as r:
        return json.load(r)


def _download(url, path):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    print(f"  download {url}")
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req) as r, open(path, "wb") as f:
        f.write(r.read())


def _slice(full_mp3, start_ms, end_ms, out_mp3):
    os.makedirs(os.path.dirname(out_mp3), exist_ok=True)
    # -ss/-to AFTER -i + re-encode => sample-accurate cut (keeps word sync tight).
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-i", full_mp3,
         "-ss", f"{start_ms/1000:.3f}", "-to", f"{end_ms/1000:.3f}",
         "-c:a", "libmp3lame", "-q:a", "4", out_mp3],
        check=True,
    )


def process_surah(surah, a0, a1):
    data = _get_json(QDC.format(rid=RECITER_ID, ch=surah))
    af = data["audio_files"][0]
    full_url = af["audio_url"]
    full_mp3 = os.path.join(CACHE, f"{RECITER_DIR}_{surah}.mp3")
    _download(full_url, full_mp3)

    vt_by_key = {vt["verse_key"]: vt for vt in af["verse_timings"]}

    ayat = []   # (ayah, [(start_rel, end_rel) per word])
    for ayah in range(a0, a1 + 1):
        vt = vt_by_key[f"{surah}:{ayah}"]
        base = vt["timestamp_from"]
        end = vt["timestamp_to"]
        # Per-ayah audio, sliced from the full surah file.
        out_mp3 = os.path.join(AUDIO_ROOT, str(surah), f"{ayah}.mp3")
        _slice(full_mp3, base, end, out_mp3)
        # Word timings, normalized to ayah start.
        words = [(int(s0 - base), int(s1 - base)) for (_wn, s0, s1) in vt["segments"]]
        ayat.append((ayah, words))
        print(f"  {surah}:{ayah}  {end-base}ms  {len(words)} words")

    # Write .qtm
    os.makedirs(TIMING_ROOT, exist_ok=True)
    out = os.path.join(TIMING_ROOT, f"{surah}.qtm")
    with open(out, "wb") as f:
        f.write(struct.pack("<4sHHHH", b"QNTM", 1, surah, len(ayat), 0))
        for ayah, words in ayat:
            f.write(struct.pack("<HH", ayah, len(words)))
            for (s0, s1) in words:
                f.write(struct.pack("<II", max(0, s0), max(0, s1)))
    print(f"wrote {out}  ({len(ayat)} ayat)")


def main():
    os.makedirs(CACHE, exist_ok=True)
    for (surah, a0, a1) in SAMPLE:
        print(f"surah {surah} ({a0}-{a1}) reciter={RECITER_DIR}")
        process_surah(surah, a0, a1)


if __name__ == "__main__":
    main()
