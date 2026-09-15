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
import urllib.error
import urllib.request

RECITER_ID = 2                      # AbdulBaset AbdulSamad, Murattal
RECITER_DIR = "abdulbasit"
QDC = "https://api.qurancdn.com/api/qdc/audio/reciters/{rid}/audio_files?chapter={ch}&segments=true"
# Same Abdul Basit Murattal recording, pre-cut per ayah from the master the qdc
# segment timeline was built against — the fallback when the CDN's full-surah
# file has been swapped out from under its own timings (see process_surah).
EVERYAYAH = "https://everyayah.com/data/Abdul_Basit_Murattal_192kbps/{s:03d}{a:03d}.mp3"

# Surahs to fetch audio + timings for. Al-Fatihah (1) stays for the no-SD fallback;
# Juz Amma (78-114) is the first real SD content. Each whole surah is fetched;
# the ayah count comes from the API, so just list surah numbers.
SAMPLE = list(range(1, 115))   # whole Quran

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


def _download_force(url, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    print(f"  download {url}")
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req) as r, open(path, "wb") as f:
        f.write(r.read())


def _duration_ms(path):
    out = subprocess.check_output(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "csv=p=0", path])
    return int(float(out) * 1000)


def _slice(full_mp3, start_ms, end_ms, out_mp3):
    os.makedirs(os.path.dirname(out_mp3), exist_ok=True)
    # Stream-copy (no re-encode): -ss/-to before -i, -c copy. Avoids an
    # ffmpeg 7.1 + libmp3lame padding bug and is fast; cuts at MP3 frame
    # boundaries (<26 ms), which is imperceptible for recitation + word sync.
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error",
         "-ss", f"{start_ms/1000:.3f}", "-to", f"{end_ms/1000:.3f}",
         "-i", full_mp3, "-c", "copy", out_mp3],
        check=True,
    )


def process_surah(surah):
    data = _get_json(QDC.format(rid=RECITER_ID, ch=surah))
    af = data["audio_files"][0]
    full_url = af["audio_url"]
    full_mp3 = os.path.join(CACHE, f"{RECITER_DIR}_{surah}.mp3")
    _download(full_url, full_mp3)

    # The CDN sometimes serves a DIFFERENT master than the one the API's
    # timings describe (surah 2: API says 8655s/232MB, CDN serves 9685s/137MB —
    # same recitation, ~3.6s more inter-ayah pause per ayah). Slicing that file
    # at the API's timestamps grabs audio from progressively wrong positions
    # (up to ~17 min off by 2:286). Detect it by duration and fall back to
    # everyayah's per-ayah clips, which are cut from the master the segment
    # timeline was built for (verified by cross-correlation + qtm span match).
    api_dur = af.get("duration")   # ms, describes the timeline's master
    swapped = bool(api_dur) and abs(_duration_ms(full_mp3) - api_dur) > 5000
    if swapped:
        print(f"  !! CDN file duration {_duration_ms(full_mp3)}ms != API "
              f"{api_dur}ms — timeline mismatch; using everyayah per-ayah clips")

    vt_by_key = {vt["verse_key"]: vt for vt in af["verse_timings"]}
    # Whole surah: ayah numbers from the API, in order.
    ayah_nums = sorted(int(k.split(":")[1]) for k in vt_by_key)

    ayat = []   # (ayah, [(start_rel, end_rel) per word])
    for ayah in ayah_nums:
        vt = vt_by_key[f"{surah}:{ayah}"]
        base = vt["timestamp_from"]
        end = vt["timestamp_to"]
        segs = vt.get("segments", [])
        # The qdc API sometimes returns a bogus timestamp_to (~10s past base) for
        # long ayat while the word segments still run to the true end — seen on ~41
        # of Al-Baqarah's ayat (e.g. 2:187 to-from=10.5s but last word ends at
        # 112.6s). Slicing on timestamp_to truncates the audio to ~10s while the
        # .qtm (built from the segments below) spans the whole ayah, so the word
        # highlight desyncs badly. Trust the segments: never cut before the last
        # word ends.
        if segs:
            last_end = max(int(s[-1]) for s in segs if len(s) >= 2)
            end = max(end, last_end)
        # Per-ayah audio: sliced from the full surah file, or fetched whole
        # from everyayah when the CDN's full file doesn't match the timeline.
        out_mp3 = os.path.join(AUDIO_ROOT, str(surah), f"{ayah}.mp3")
        try:
            if swapped:
                _download_force(EVERYAYAH.format(s=surah, a=ayah), out_mp3)
            else:
                _slice(full_mp3, base, end, out_mp3)
        except (subprocess.CalledProcessError, urllib.error.URLError) as e:
            print(f"  !! audio failed {surah}:{ayah} — skipping ({e})")
            continue   # keep the .qtm consistent with the mp3s that exist
        # Word timings, normalized to ayah start. Segments are usually
        # [word_no, start_ms, end_ms] but some come back [start_ms, end_ms] — take
        # the last two as start/end either way.
        words = [(int(seg[-2] - base), int(seg[-1] - base))
                 for seg in vt.get("segments", []) if len(seg) >= 2]
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
    import sys
    # Optional surah numbers on the CLI regenerate just those (e.g. re-slice a
    # single surah after the timestamp_to fix); no args = the whole SAMPLE set.
    surahs = [int(a) for a in sys.argv[1:]] or SAMPLE
    os.makedirs(CACHE, exist_ok=True)
    for surah in surahs:
        print(f"surah {surah} reciter={RECITER_DIR}")
        try:
            process_surah(surah)
        except Exception as e:
            print(f"  !! surah {surah} failed: {e} — skipping")


if __name__ == "__main__":
    main()
