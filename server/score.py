"""Quran Teacher V2 scoring core (see docs/TEACHER_V2.md).

Transcribe a recitation take with tarteel-ai/whisper-base-ar-quran, align the
transcript words against the canonical ayah text, and emit per-word verdicts:
GOOD / UNSURE / MISMATCH / MISSING / UNCLEAR.

Voice, style, and pace drop out entirely — the comparison is words-to-words,
which is what the V1 acoustic engine could not do (docs/TEACHER_V2.md "Why").
"""
import json
import re
import unicodedata
import wave
from dataclasses import dataclass
from pathlib import Path

import os
# Benchmarked 2026-09-06 on labeled device takes: large-v3-turbo reads the
# user's natural fast recitation at 75-100% words correct where the
# murattal-tuned tarteel-ai/whisper-base-ar-quran managed ~15% (it expects
# slow tajwid delivery). ~4s/take on an M-series CPU. Base remains the
# low-power fallback via QN_ASR_MODEL.
MODEL_ID = os.environ.get("QN_ASR_MODEL", "openai/whisper-large-v3-turbo")
HERE = Path(__file__).parent

# --- Arabic normalization ---------------------------------------------------
# Learner ASR output is loosely vocalized; grade content diacritic-insensitively.
_DIACRITICS = re.compile(r"[ً-ٰٟـۖ-ۭ]")

def normalize(word: str) -> str:
    w = unicodedata.normalize("NFC", word)
    w = _DIACRITICS.sub("", w)
    w = (w.replace("أ", "ا").replace("إ", "ا").replace("آ", "ا")
          .replace("ٱ", "ا").replace("ى", "ي").replace("ة", "ه"))
    return "".join(ch for ch in w if not ch.isspace())

def char_similarity(a: str, b: str) -> float:
    """1 - normalized Levenshtein distance."""
    if not a and not b:
        return 1.0
    la, lb = len(a), len(b)
    prev = list(range(lb + 1))
    for i in range(1, la + 1):
        cur = [i] + [0] * lb
        for j in range(1, lb + 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1,
                         prev[j - 1] + (a[i - 1] != b[j - 1]))
        prev = cur
    return 1.0 - prev[lb] / max(la, lb)

# --- Canonical text ---------------------------------------------------------
_TEXT = json.loads((HERE / "quran_text.json").read_text())

def canonical_words(surah: int, ayah: int) -> list[str]:
    return _TEXT[str(surah)][str(ayah)]

# --- ASR (loaded once) ------------------------------------------------------
_model = None
_processor = None

def _load():
    global _model, _processor
    if _model is None:
        import torch  # noqa: F401  (import check before transformers)
        from transformers import WhisperForConditionalGeneration, WhisperProcessor
        _processor = WhisperProcessor.from_pretrained(MODEL_ID)
        _model = WhisperForConditionalGeneration.from_pretrained(
            MODEL_ID, torch_dtype=torch.float32)   # some ship fp16; CPU wants f32
        _model.eval()
    return _model, _processor

def load_wav_mono16k(path: str):
    import numpy as np
    w = wave.open(path, "rb")
    assert w.getsampwidth() == 2
    hz = w.getframerate()
    pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    if w.getnchannels() > 1:
        pcm = pcm.reshape(-1, w.getnchannels()).mean(axis=1)
    w.close()
    x = pcm.astype("float32") / 32768.0
    if hz != 16000:  # naive resample is fine for 44.1k->16k benchmark refs
        idx = (np.arange(int(len(x) * 16000 / hz)) * hz / 16000).astype(int)
        x = x[np.clip(idx, 0, len(x) - 1)]
    # Device-capture cleanup (measured on INMP441 field takes): the 0-100Hz
    # rumble band is as LOUD as the voice and wrecks the mel features — high-
    # pass at 90Hz, then peak-normalize. Harmless on clean audio.
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / 16000)
    X[f < 90] = 0
    edge = (f >= 90) & (f < 120)
    X[edge] *= (f[edge] - 90) / 30
    x = np.fft.irfft(X, len(x))
    peak = np.abs(x).max()
    if peak > 1e-4:
        x = x * min(8.0, 0.7 / peak)
    return x.astype("float32")

def transcribe(path: str) -> str:
    import torch
    model, processor = _load()
    audio = load_wav_mono16k(path)
    feats = processor(audio, sampling_rate=16000, return_tensors="pt").input_features
    with torch.no_grad():
        # No language/task args: the fine-tune is Arabic-Quran-only and ships
        # a pre-transformers-4.32 generation config that rejects them.
        # Beam search: measurably better than greedy on marginal device audio.
        ids = model.generate(feats, num_beams=5)
    text = processor.batch_decode(ids, skip_special_tokens=True)[0]
    # The tarteel tokenizer predates special-token registration for the task
    # markers, so <|ar|> etc. survive decoding — strip them explicitly.
    return re.sub(r"<\|[^|]*\|>", " ", text).strip()

# --- Alignment + verdicts ---------------------------------------------------
@dataclass
class WordVerdict:
    verdict: str          # GOOD/UNSURE/MISMATCH/MISSING
    similarity: float
    heard: str            # the transcript word(s) this canonical word matched

TH_GOOD = 0.80
TH_UNSURE = 0.50

def align_words(canon: list[str], heard: list[str]):
    """Word-level Needleman-Wunsch; substitution cost = 1 - char similarity."""
    nc, nh = len(canon), len(heard)
    cn = [normalize(w) for w in canon]
    hn = [normalize(w) for w in heard]
    GAP = 0.72   # cheaper than a terrible substitution, dearer than a decent one
    D = [[0.0] * (nh + 1) for _ in range(nc + 1)]
    for i in range(1, nc + 1):
        D[i][0] = i * GAP
    for j in range(1, nh + 1):
        D[0][j] = j * GAP
    for i in range(1, nc + 1):
        for j in range(1, nh + 1):
            sub = D[i - 1][j - 1] + (1.0 - char_similarity(cn[i - 1], hn[j - 1]))
            D[i][j] = min(sub, D[i - 1][j] + GAP, D[i][j - 1] + GAP)
    # backtrack
    pairs: dict[int, list[int]] = {}
    i, j = nc, nh
    while i > 0 or j > 0:
        if i > 0 and j > 0 and abs(
            D[i][j] - (D[i - 1][j - 1] + (1.0 - char_similarity(cn[i - 1], hn[j - 1])))
        ) < 1e-9:
            pairs.setdefault(i - 1, []).append(j - 1)
            i, j = i - 1, j - 1
        elif i > 0 and abs(D[i][j] - (D[i - 1][j] + GAP)) < 1e-9:
            i -= 1                     # canonical word deleted (MISSING)
        else:
            j -= 1                     # inserted transcript word
    return pairs, cn, hn

def score_take(path: str, surah: int, ayah: int):
    canon = canonical_words(surah, ayah)
    text = transcribe(path)
    heard = text.split()
    pairs, cn, hn = align_words(canon, heard)
    out = []
    for i, w in enumerate(canon):
        js = sorted(pairs.get(i, []))
        if not js:
            out.append(WordVerdict("MISSING", 0.0, ""))
            continue
        heard_join = "".join(hn[j] for j in js)
        sim = max(char_similarity(cn[i], hn[j]) for j in js)
        sim = max(sim, char_similarity(cn[i], heard_join))
        v = ("GOOD" if sim >= TH_GOOD else
             "UNSURE" if sim >= TH_UNSURE else "MISMATCH")
        out.append(WordVerdict(v, sim, " ".join(heard[j] for j in js)))
    return out, text

if __name__ == "__main__":
    import sys
    surah, ayah = int(sys.argv[1]), int(sys.argv[2])
    for p in sys.argv[3:]:
        verdicts, text = score_take(p, surah, ayah)
        print(f"{p}: '{text}'")
        for k, v in enumerate(verdicts):
            print(f"  word {k+1}: {v.verdict:8s} sim={v.similarity:.2f} heard='{v.heard}'")
