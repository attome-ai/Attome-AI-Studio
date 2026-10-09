"""How close attome-whisper's word start times are to the true ones, for each model and timing method.

usage: powershell -File tools/asr_timing_truth.ps1 -Out <folder>   (8 recordings by the Windows voices, with each word's true start)
       python -I tools/asr_timing_eval.py <folder>

Results of 2026-10-09 (172 words, on the GPU): one-word segments put 52 % (small) and 39 % (large-v3 turbo) of the words within 0.1 s of the truth;
DTW, 0.2 s earlier as attome-whisper now gives it, 76 % and 64 %, and 98 % and 97 % within 0.2 s. A clean synthetic voice: real speech is harder.
"""
import json, re, struct, subprocess, sys, difflib, statistics
from pathlib import Path

root = Path(r"C:\Users\Computia.me\Downloads\everything\attome-wt-uifix")
exe = root / "build/win-msvc-release/bin/attome-whisper.exe"
data = Path(sys.argv[1])
truth = json.loads((data / "truth.json").read_text(encoding="utf-8-sig"))

def pcm16(path):
    b = path.read_bytes()
    pos, pcm = 12, None
    while pos + 8 <= len(b):
        tag, size = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        if tag == b"data":
            pcm = b[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    return [v / 32768.0 for v in struct.unpack("<%dh" % (len(pcm) // 2), pcm[: len(pcm) // 2 * 2])]

norm = lambda w: re.sub(r"[^a-z0-9']", "", w.lower())

for model in ("ggml-small.bin", "ggml-large-v3-turbo-q5_0.bin"):
    for timing in ("segments", "dtw"):
        errs = []
        for rec in truth:
            samples = pcm16(data / rec["file"])
            payload = struct.pack("<I", len(samples)) + struct.pack("<%df" % len(samples), *samples)
            p = subprocess.run([str(exe), "--model", str(root / "models" / model), "--language", "en", "--gpu", "1", "--timing", timing],
                               input=payload, capture_output=True)
            heard = [json.loads(l) for l in p.stdout.decode().splitlines() if '"words"' in l][0]["words"]
            ref = [(norm(w["t"]), w["s"]) for w in rec["words"]]
            hyp = [(norm(w["t"]), w["s"]) for w in heard]
            sm = difflib.SequenceMatcher(None, [r[0] for r in ref], [h[0] for h in hyp], autojunk=False)
            for block in sm.get_matching_blocks():
                for k in range(block.size):
                    errs.append(hyp[block.b + k][1] - ref[block.a + k][1])  # + = said late
        a = [abs(e) for e in errs]
        print(f"{model:30s} {timing:9s} words {len(errs):3d} | mean |err| {1000*statistics.mean(a):5.0f} ms | median {1000*statistics.median(a):4.0f} ms"
              f" | within 100 ms {100*sum(x <= 0.1 for x in a)/len(a):3.0f}% | within 200 ms {100*sum(x <= 0.2 for x in a)/len(a):3.0f}%"
              f" | bias {1000*statistics.mean(errs):+5.0f} ms")
