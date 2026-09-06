# Quran Teacher V2 — Phoneme-Level Scoring (Spec)

## Why (the V1 verdict)

Two labeled dataset sessions (2026-09-06, ~14 device takes; see
`training_takes*/`, gitignored) proved the V1 ceiling by measurement:
MFCC-DTW acoustic similarity cannot grade recitation *content*. Even
self-reference comparisons (user take vs user take — same voice, mic, room,
deliberate pace, 20dB SNR) score correct repetitions in the same 2.4–3.1
band as deliberately-bad takes and wrong-ayah comparisons. Within-class
spread (~0.7) exceeds class margins (~0.1–0.3). Threshold tuning is
exhausted.

What V1 does reliably — and keeps doing as the offline fallback:
omission/coverage detection (skipped words → MISSING, truncation, grunts
refused), alignment, endpointing, and the whole practice UX.

V2 replaces the *scoring source* with speech recognition: transcribe what
the user actually said, compare **words to words** instead of spectra to
spectra. Voice, style, and pace drop out of the comparison entirely.

## Architecture

```
DEVICE (unchanged UX)                    SCORING SERVER (LAN Mac first)
┌─────────────────────────┐              ┌──────────────────────────────┐
│ teacher scene           │  HTTP POST   │ FastAPI                      │
│  record take (16k WAV)  │ ───────────► │  /score?surah=S&ayah=A       │
│  "ANALYZING..."         │              │   1. ASR: whisper-base-ar-   │
│                         │  CSV reply   │      quran (transcribe +     │
│  per-word marks, review │ ◄─────────── │      word timestamps)        │
│  replay teacher / self  │              │   2. align transcript to     │
│                         │              │      canonical ayah text     │
│ offline → local V1 DTW  │              │   3. per-word verdicts       │
└─────────────────────────┘              └──────────────────────────────┘
```

- **Server location**: the user's Mac on the LAN (zero cost, private, always
  nearby during practice). The device already reaches the Mac's subnet (the
  `/takes` download proved the path in both directions). Deployable to a
  cloud box later without device changes — it's just a URL.
- **Fallback**: no server / no Wi-Fi / timeout (>8s) → the local V1 engine
  scores as today. The review panel tags the source: `scored online` vs
  `offline estimate`.

## Model

Primary: **`tarteel-ai/whisper-base-ar-quran`** (Hugging Face) — Whisper
fine-tuned on the Tarteel Everyayah dataset, 5.75% WER on Quranic
recitation. Tarteel's production app does live recitation correction with
this approach, so the architecture is field-proven.

- Run via `faster-whisper` (CTranslate2, int8) for CPU speed: a 3–5s take
  transcribes in well under 2s on an M-series Mac.
- Word-level timestamps requested from the decoder; they map verdicts back
  to audio spans for per-word replay (fallback: V1's DTW spans).
- Alternatives if timestamps/accuracy disappoint: `whisper-small-ar-quran`
  (accuracy), wav2vec2-CTC Arabic phoneme models (harder timestamps, finer
  phoneme detail), `whisper-tiny-ar-quran` (speed).

## Server

`server/` in this repo. Python 3.11+, FastAPI + uvicorn + faster-whisper.

### API

```
POST /score?surah=1&ayah=2
  body: audio/wav (16k mono s16; the device's take, untrimmed)
  auth: none on LAN (v2.1: bearer token for hosted deployment)

200 text/csv:
  word,verdict,score,start_ms,end_ms
  1,GOOD,0.92,120,840
  2,MISSING,0.00,0,0
  3,UNSURE,0.55,1200,1650
  4,GOOD,0.88,1700,2400
  # trailing comment lines (ignored by device):
  #transcript=بسم الله الرحمن الرحيم
  #asr_conf=0.91

5xx / timeout → device falls back to local scoring.
```

CSV, not JSON: trivial to emit, trivial to parse on the ESP32 (no JSON
parser dependency), human-debuggable in the serial log.

### Scoring logic

1. **Canonical text**: `server/quran_text.json` — Uthmani text per
   (surah, ayah), word-split to MATCH the device's timing word counts
   (source both from the same Tanzil text `tools/fetch_sample.py` already
   uses; add a build check that counts agree).
2. **Normalize** both transcript and canonical words: strip diacritics,
   unify alef/hamza/ta-marbuta forms (learner ASR output is unvocalized-ish;
   first pass is diacritic-insensitive).
3. **Align** transcript words to canonical words (Levenshtein at word
   level, substitution cost = normalized character edit distance).
4. **Verdicts** per canonical word:
   - aligned, char-similarity ≥ 0.8 → `GOOD`
   - aligned, 0.5–0.8 → `UNSURE` ("a bit different — listen")
   - aligned, < 0.5 → `MISMATCH`
   - deleted (no transcript word) → `MISSING`
   - ASR confidence low / no-speech → `UNCLEAR` for affected words
   - insertions between words: reported in the `#` comments, not marked
5. `score` = the char similarity (0..1); the device displays verdicts only,
   scores go to the log for tuning.

Thresholds tuned against the labeled dataset (`training_takes*/` +
`quran-recite-eval`-style harness in `server/eval.py`) — that dataset is
now the permanent benchmark.

## Device changes (small by design)

1. **HAL**: `int hal_score_remote(const int16_t *pcm, uint32_t n, uint32_t hz,
   int surah, int ayah, RemoteWord *out, int max_words);`
   - esp32: bring Wi-Fi up (reuse OTA's `wifi_up`), HTTP POST the WAV
     (~100–300KB; the OTA path already moves MBs), parse CSV rows.
     Returns word count, 0 = no result (fall back), keeps Wi-Fi up for the
     session (subsequent takes score fast).
   - sim: same via libcurl-less plain sockets or just POSIX `connect()` —
     or simplest: sim also POSTs (SDL_net not needed; ~80 lines of plain
     HTTP over BSD sockets).
   - dump: stub returning 0 (headless flows keep exercising the local path).
2. **Scene**: `run_analysis()` tries `hal_score_remote` first; on result,
   fills `s_words[]` from it (verdict + spans) and tags the review panel
   "scored online". Local engine unchanged as fallback, still computes the
   user-span mapping when remote timestamps are absent.
3. **Config**: server URL in `firmware/secrets.ini` (`TEACHER_URL`), NVS-
   overridable later from Settings. No URL configured = pure offline device.

## Privacy

Recitation audio leaves the device **only** to the configured server —
the user's own Mac by default. Document in Settings copy. Hosted
deployment (if ever) needs explicit opt-in + auth. Voice recordings never
enter the git repo (`training_takes*/` gitignored).

## Milestones

- **M1 — server MVP + benchmark gate** (no device changes):
  FastAPI + model + alignment; `server/eval.py` runs the labeled takes.
  **Acceptance before M2 starts**: sincere takes ≥ 90% words GOOD;
  bad/gibberish takes ≥ 75% words flagged; wrong-ayah flagged; per-take
  latency < 3s on the Mac. If the model can't pass this on our own data,
  stop and re-evaluate model choice — no device work wasted.
- **M2 — device integration**: HAL POST + CSV parse + scene wiring +
  source tag + secrets URL. Ship as a normal vX release.
- **M3 — polish**: word-timestamp replay spans, confidence gating, latency
  (start upload while recording?), Settings toggle ("online scoring"),
  optional hosted deployment with token auth.
- **Later**: madd/tajweed duration checks from word timestamps (the ASR
  gives phone-level durations — rule-based tajweed hints become feasible),
  multi-reciter references, personal progress stats.

## Risks

| Risk | Mitigation |
|---|---|
| ASR accuracy on fast casual (non-tajwid) recitation | M1 benchmark on OUR takes is the gate; model swap is cheap (same API) |
| Word-count mismatch text vs device timings | build-time check; canonical JSON derived from the same Tanzil source |
| Whisper hallucination on silence/noise | no-speech prob + compression-ratio gates → UNCLEAR, never fake GOOD |
| LAN server not running | timeout → local fallback, UI says "offline estimate" |
| ESP32 POST reliability | OTA HTTP stack already proven for larger transfers |
