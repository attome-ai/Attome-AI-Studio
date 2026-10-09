"""Word error rate of attome-whisper on FLEURS Arabic (ar_eg test), the check behind the choice of speech models (F3 accuracy gate).

usage: python -I tools/wer_eval.py <ggml model> <language> <count> <out.json> [cli]

Reads .deps/fleurs_ar/test.tsv and audio/test/*.wav (google/fleurs, CC-BY-4.0, revision 70bb2e84b976b7e960aa89f1c648e09c59f894dd: data/ar_eg/test.tsv and
data/ar_eg/audio/test.tar.gz, unpacked there). Runs `count` recordings spread evenly over the test set through build/win-msvc-release/bin/attome-whisper
(or, with `cli`, whisper.cpp's own whisper-cli as a control) and prints the word error rate after stripping diacritics and punctuation and folding the
usual Arabic letter variants. Results of 2026-10-09 on 100 recordings (1941 words): ggml-small 25.8 %, ggml-large-v3-turbo-q5_0 8.6 % (no recording lost),
ggml-large-v3 (full, 3.1 GB) 8.0 %; on the GPU large-v3 runs 15 times real time against 80 for turbo. 22 words of the output are numbers in digits, counted
wrong against numbers in words.
A first sample of 30 (591 words) had given 32.0 % and 16.6 %: it happened to hold the one recording the model lost, so a sample that small misleads.

MediaSpeech (set=mediaspeech), 200 clips of real Arabic broadcast speech, 5594 words, transcripts written by people who listened: large-v3 turbo
20.9 % of words wrong, the full large-v3 20.4 %. About a fifth of that is not hearing: the references write the conjunction apart ("و كان"), numbers
in words, and the clips are cut in mid-sentence; with conjunctions joined and the two words at each end left out, 16.6 % and 16.0 %. So real speech,
much of it dialect, is about twice as hard as FLEURS's read speech, and a larger general model does not help.
"""
import json, re, struct, subprocess, sys, time, wave
from pathlib import Path

root = Path(r"C:\Users\Computia.me\Downloads\everything\attome-wt-uifix")
exe = root / "build/win-msvc-release/bin/attome-whisper.exe"
fleurs = root / ".deps/fleurs_ar"
model, language, count, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
use_cli = len(sys.argv) > 5 and sys.argv[5] == "cli"  # control: whisper.cpp's own whisper-cli with its defaults
gpu_args = ["--gpu", "1"] if "gpu" in sys.argv[5:] else []  # attome-whisper on the graphics card
for a in sys.argv[5:]:  # any other option of attome-whisper, as name=value: beam=5, prompt=..., timing=segments
    if "=" in a and not a.startswith("set="):
        gpu_args += ["--" + a.split("=", 1)[0], a.split("=", 1)[1]]
cli = root / ".deps/whisper.cpp/build/bin/whisper-cli.exe"

# Arabic diacritics (tashkeel), tatweel, punctuation
DIACRITICS = re.compile(r"[\u0610-\u061A\u064B-\u065F\u0670\u06D6-\u06ED\u0640]")
PUNCT = re.compile(r"[\u060C\u061B\u061F\u066A-\u066D\.,;:!?\"'()\[\]{}«»\-–—…،؛؟]")

INDIC = str.maketrans("٠١٢٣٤٥٦٧٨٩", "0123456789")

def normal(text: str) -> list[str]:
    text = DIACRITICS.sub("", text).translate(INDIC).lower()  # Arabic-Indic digits as 0-9, Latin words in lower case
    text = PUNCT.sub(" ", text)
    text = text.replace("أ", "ا").replace("إ", "ا").replace("آ", "ا").replace("ى", "ي").replace("ة", "ه")  # the usual letter variants
    return text.split()

def edits(ref: list[str], hyp: list[str]) -> int:
    d = list(range(len(hyp) + 1))
    for i in range(1, len(ref) + 1):
        prev, d[0] = d[0], i
        for j in range(1, len(hyp) + 1):
            cur = min(d[j] + 1, d[j - 1] + 1, prev + (ref[i - 1] != hyp[j - 1]))
            prev, d[j] = d[j], cur
    return d[len(hyp)]


def read_wav(path):
    """Mono 16 kHz samples as floats from a RIFF WAV holding 16-bit integers (format 1) or 32-bit floats (format 3)."""
    data = Path(path).read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", path
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    tag, channels, rate, _, _, bits = fmt
    assert rate == 16000 and channels == 1, (rate, channels)
    if tag == 3 and bits == 32:
        return list(struct.unpack("<%df" % (len(pcm) // 4), pcm[: len(pcm) // 4 * 4]))
    assert tag == 1 and bits == 16, (tag, bits)
    return [v / 32768.0 for v in struct.unpack("<%dh" % (len(pcm) // 2), pcm[: len(pcm) // 2 * 2])]

# The recordings: rows of (-, wav path, -, what is said). FLEURS by default; "set=mediaspeech" reads .deps/mediaspeech/AR (NTRLab/MediaSpeech release
# 1.1, AR.zip: 2505 clips from Arabic media channels, each with a transcript written by a person who listened; CC BY 4.0).
dataset = next((a.split("=", 1)[1] for a in sys.argv[5:] if a.startswith("set=")), "fleurs")
if dataset == "mediaspeech":
    media = root / ".deps/mediaspeech/AR"
    rows = [["", str(w), "", w.with_suffix(".txt").read_text(encoding="utf-8").strip()] for w in sorted(media.glob("*.wav"))]
elif dataset == "fleurs":
    rows = [line.rstrip("\n").split("\t") for line in (fleurs / "test.tsv").read_text(encoding="utf-8").splitlines()]
    rows = [[r[0], str(fleurs / "audio/test" / r[1]), r[2], r[3]] for r in rows if len(r) >= 4 and (fleurs / "audio/test" / r[1]).exists()]
else:
    raise SystemExit("unknown set: " + dataset)
step = max(1, len(rows) // count)
picked = rows[::step][:count]

total_ref = total_err = 0
audio_seconds = run_seconds = 0.0
per_file = []
devices = set()
for r in picked:
    wav = Path(r[1])
    samples = read_wav(wav)
    started = time.time()
    if use_cli:
        p = subprocess.run([str(cli), "-m", model, "-l", language, "-nt", "-np", "-f", str(wav)], capture_output=True)
        result = {"words": [{"t": w} for w in p.stdout.decode("utf-8", "replace").split()]}
    else:
        payload = struct.pack("<I", len(samples)) + struct.pack("<%df" % len(samples), *samples)
        p = subprocess.run([str(exe), "--model", model, "--language", language] + gpu_args, input=payload, capture_output=True)
        result = None
        for line in p.stdout.decode("utf-8", "replace").splitlines():
            j = json.loads(line)
            if "words" in j:
                result = j
                devices.add(j.get("device", "?"))
            elif "error" in j:
                raise SystemExit("program error: " + j["error"])
    run_seconds += time.time() - started
    audio_seconds += len(samples) / 16000.0
    hyp = normal(" ".join(w["t"] for w in result["words"]))
    ref = normal(r[3])
    e = edits(ref, hyp)
    total_ref += len(ref)
    total_err += e
    per_file.append({"file": Path(r[1]).name, "ref_words": len(ref), "errors": e, "wer": round(e / max(1, len(ref)), 3), "heard": " ".join(hyp), "said": " ".join(ref)})

summary = {"model": Path(model).name, "language": language, "files": len(picked), "ref_words": total_ref, "errors": total_err,
           "wer": round(total_err / total_ref, 4), "audio_seconds": round(audio_seconds, 1), "run_seconds": round(run_seconds, 1),
           "times_real_time": round(audio_seconds / run_seconds, 2), "device": sorted(devices), "set": dataset}
Path(out).write_text(json.dumps({"summary": summary, "files": per_file}, ensure_ascii=False, indent=1), encoding="utf-8")
print(json.dumps(summary))
