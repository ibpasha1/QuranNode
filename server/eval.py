"""M1 benchmark: run the labeled take dataset through the V2 scorer.

Acceptance gate (docs/TEACHER_V2.md):
  - sincere takes: >= 90% of words GOOD
  - bad/gibberish takes: >= 75% of words flagged (not GOOD)
  - wrong-ayah scoring: flagged
  - latency < 3s/take
Labels supplied inline below (from the user's session notes).
"""
import glob
import os
import time

import score as S

os.chdir(os.path.join(os.path.dirname(__file__), ".."))   # repo root

# session 2 (training_takes2): user note — takes 2 and (5 or 6) were BAD.
SESSION2 = {
    1: "sincere", 2: "bad", 3: "sincere", 4: "sincere",
    5: "bad?", 6: "bad?", 7: "sincere",
}
# session 1 (training_takes): all correct content (sincere/fast labels).
SESSION1 = {i: "sincere" for i in range(1, 8)}

def run(folder, labels, surah, ayah, title):
    print(f"\n=== {title} (vs {surah}:{ayah}) ===")
    stats = {}
    for i in sorted(labels):
        paths = glob.glob(f"{folder}/train_{i:02d}_*.wav")
        if not paths:
            continue
        t0 = time.time()
        verdicts, text = S.score_take(paths[0], surah, ayah)
        dt = time.time() - t0
        good = sum(v.verdict == "GOOD" for v in verdicts)
        marks = " ".join(f"{v.verdict[0]}{v.similarity:.2f}" for v in verdicts)
        print(f"take {i} [{labels[i]:8s}] {good}/{len(verdicts)} good "
              f"({dt:.1f}s)  {marks}  '{text}'")
        stats.setdefault(labels[i], []).append(good / len(verdicts))
    return stats

if __name__ == "__main__":
    all_stats = {}
    for st in (
        run("training_takes2", SESSION2, 1, 2, "session 2, correct reference"),
        run("training_takes", SESSION1, 1, 2, "session 1, correct reference"),
        run("training_takes2", {i: "wrong-ayah" for i in (1, 3, 4)}, 1, 3,
            "session 2 sincere takes vs WRONG ayah text"),
    ):
        for k, v in st.items():
            all_stats.setdefault(k, []).extend(v)

    print("\n=== ACCEPTANCE ===")
    for label, fracs in sorted(all_stats.items()):
        avg = sum(fracs) / len(fracs)
        print(f"{label:10s}: mean {avg*100:.0f}% words GOOD over {len(fracs)} takes")
