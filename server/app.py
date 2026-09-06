"""Quran Teacher V2 scoring server (docs/TEACHER_V2.md).

    server/.venv/bin/uvicorn app:app --host 0.0.0.0 --port 8090

POST /score?surah=1&ayah=2 with a 16k mono s16 WAV body returns CSV:
    word,verdict,score,start_ms,end_ms
plus '#'-prefixed comment lines (transcript). The device parses the rows;
comments are for humans in the serial log.
"""
import tempfile

from fastapi import FastAPI, Request, Response

import score as S

app = FastAPI()

# Warm the model at startup so the first take isn't slow.
@app.on_event("startup")
def _warm():
    S._load()

@app.get("/")
def root():
    return {"service": "qurannode-teacher", "model": S.MODEL_ID}

@app.post("/score")
async def score(request: Request, surah: int, ayah: int):
    body = await request.body()
    if len(body) < 100:
        return Response("error: empty body\n", status_code=400)
    with tempfile.NamedTemporaryFile(suffix=".wav") as f:
        f.write(body)
        f.flush()
        try:
            verdicts, text = S.score_take(f.name, surah, ayah)
        except KeyError:
            return Response("error: unknown surah/ayah\n", status_code=404)
    lines = ["word,verdict,score,start_ms,end_ms"]
    for i, v in enumerate(verdicts):
        # start/end word timestamps: M3 (device falls back to its DTW spans)
        lines.append(f"{i + 1},{v.verdict},{v.similarity:.2f},0,0")
    lines.append(f"#transcript={text}")
    return Response("\n".join(lines) + "\n", media_type="text/csv")
