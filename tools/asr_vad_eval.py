"""What voice activity detection does on speech with silence and music around it: words invented, words missed, and word timing.

usage: powershell -File tools/asr_timing_truth.ps1 -Out <folder>   (the voice recordings with each word's true start)
       python -I tools/asr_vad_eval.py <folder> <models folder> [model file ...]

Each recording is placed after 8 s of silence and before 8 s of "music" (a chord with a beat and a little noise) and 4 s of silence; the true word
times move with it. Whisper is known to invent words over silence and music; VAD decodes the speech only.

Result of 2026-10-09 (large-v3 turbo, 192 words): the same with VAD and without: 12 words "missed" and 6 or 7 "invented", all of them numbers
written in digits ("twenty three" heard as "23"); nothing was invented over the silence or the music, and word starts stay 96-97 % within 0.2 s
with VAD (its timeline is mapped back). So attome-whisper has --vad, and the engine does not turn it on: no gain was measured.
"""
import difflib, json, math, random, re, statistics, struct, subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
exe = root / "build/win-msvc-release/bin/attome-whisper.exe"
data, models = Path(sys.argv[1]), Path(sys.argv[2])
model_files = sys.argv[3:] or ["ggml-large-v3-turbo-q5_0.bin"]
truth = json.loads((data / "truth.json").read_text(encoding="utf-8-sig"))
for rec in truth:  # the voice reports its words through events that can arrive slightly out of order
    rec["words"] = sorted(rec["words"], key=lambda w: w["s"])
vad_model = models / "ggml-silero-v6.2.0.bin"
RATE = 16000

def pcm16(path):
    b = path.read_bytes()
    pos, pcm = 12, None
    while pos + 8 <= len(b):
        tag, size = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        if tag == b"data":
            pcm = b[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    return [v / 32768.0 for v in struct.unpack("<%dh" % (len(pcm) // 2), pcm[: len(pcm) // 2 * 2])]

def music(seconds, seed):
    rnd = random.Random(seed)
    out = []
    for i in range(int(seconds * RATE)):
        t = i / RATE
        beat = 1.0 if (t % 0.5) < 0.05 else 0.0
        chord = sum(math.sin(2 * math.pi * f * t) for f in (220.0, 277.2, 329.6)) / 3
        out.append(0.25 * chord + 0.2 * beat * math.sin(2 * math.pi * 60 * t) + 0.02 * (rnd.random() * 2 - 1))
    return out

norm = lambda w: re.sub(r"[^a-z0-9']", "", w.lower())
LEAD = 8.0

for model in model_files:
    for use_vad in (False, True):
        invented = missed = said_total = 0
        errs = []
        for n, rec in enumerate(truth):
            speech = pcm16(data / rec["file"])
            audio = [0.0] * int(LEAD * RATE) + speech + music(8.0, n) + [0.0] * int(4 * RATE)
            payload = struct.pack("<I", len(audio)) + struct.pack("<%df" % len(audio), *audio)
            args = [str(exe), "--model", str(models / model), "--language", "en", "--gpu", "1"]
            if use_vad:
                args += ["--vad", str(vad_model)]
            p = subprocess.run(args, input=payload, capture_output=True)
            heard = [json.loads(l) for l in p.stdout.decode().splitlines() if '"words"' in l][0]["words"]
            ref = [(norm(w["t"]), w["s"] + LEAD) for w in rec["words"]]
            hyp = [(norm(w["t"]), w["s"]) for w in heard if norm(w["t"])]
            sm = difflib.SequenceMatcher(None, [r[0] for r in ref], [h[0] for h in hyp], autojunk=False)
            matched = 0
            for b in sm.get_matching_blocks():
                for k in range(b.size):
                    errs.append(hyp[b.b + k][1] - ref[b.a + k][1])
                matched += b.size
            said_total += len(ref)
            missed += len(ref) - matched
            invented += len(hyp) - matched
        a = [abs(e) for e in errs]
        print(f"{model:30s} vad {'on ' if use_vad else 'off'} | words {said_total} | missed {missed:3d} | invented {invented:3d}"
              f" | start within 0.2 s {100 * sum(x <= 0.2 for x in a) / len(a):3.0f}% | median error {1000 * statistics.median(a):4.0f} ms")
