"""Does Whisper hold up on a long recording? Long recordings are said to make it loop, drop stretches of speech, and drift in its times.

usage: python -I tools/long_asr_eval.py <ggml model> <language> <minutes> <out.json> [gpu] [control] [norm=leaderboard]

No download: the recording is built from the MediaSpeech clips that tools/wer_eval.py already uses (.deps/mediaspeech/AR, 2505 clips, about 10 hours,
each with a transcript). Clips spread evenly over the set (so the channels and speakers change) are laid one after another with half a second of
silence between them until `minutes` is full; every clip's place in the recording is known, so the words that come back can be sorted back
into their clips. The recording goes through attome-whisper in one piece, as the engine hands it a clip of that length.

Reported, for the whole recording and for each 5-minute window of it:
  wer       words wrong against the clips' transcripts, with each heard word given to the clip it started in (the same rules as wer_eval.py)
  control   with `control`: the same clips one by one, as wer_eval.py runs them. A long recording that gets the same number is not worse for being long.
  loops     runs of one to six words repeated four times or more in a row (the loop Whisper falls into), and how many words are in them
  lost      clips that no word was heard in, and the seconds they cover
  start     the median distance, in seconds, between where a clip starts and where its first word is said to start (drift shows as it growing
            window by window; a clip's first word is a little after its start, so look at the change, not the size)

This is a stand-in for a lecture or a meeting: the speech is real, the join between clips is not (a different programme every 8 seconds, where a long
recording goes on with one voice). If it loops here, it will loop there; if it does not, the long recording of one voice is the case still to see.
"""
import json, re, statistics, struct, subprocess, sys, time
from pathlib import Path

root = Path(r"C:\Users\Computia.me\Downloads\everything\attome-wt-uifix")
exe = root / "build/win-msvc-release/bin/attome-whisper.exe"
media = root / ".deps/mediaspeech/AR"
model, language, minutes, out = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
flags = sys.argv[5:]
gpu_args = ["--gpu", "1"] if "gpu" in flags else []
RATE, GAP, WINDOW = 16000, 0.5, 300.0

DIACRITICS = re.compile(r"[\u0610-\u061A\u064B-\u065F\u0670\u06D6-\u06ED\u0640]")
PUNCT = re.compile(r"[\u060C\u061B\u061F\u066A-\u066D\.,;:!?\"'()\[\]{}«»\-–—…،؛؟]")
INDIC = str.maketrans("٠١٢٣٤٥٦٧٨٩", "0123456789")


def normal(text):
    text = DIACRITICS.sub("", text).translate(INDIC).lower()
    text = PUNCT.sub(" ", text)
    text = text.replace("أ", "ا").replace("إ", "ا").replace("آ", "ا").replace("ى", "ي").replace("ة", "ه")
    return text.split()


def leaderboard(text):
    text = re.sub(r'[!"#$%&\'()*+,\-./:;<=>?@\[\\\]^_`{|}~،؛؟]', "", text)
    text = re.sub(r"[\u064B-\u0652]", "", text)
    text = text.replace("پ", "ب").replace("ڤ", "ف")
    text = re.sub(r"[آأإ]", "ا", text)
    text = text.replace("ؤ", "و").replace("ئ", "ي").replace("ء", "").translate(INDIC)
    text = re.sub(r"(^|\s)و\s+", r"\1و", text)
    return re.sub(r"\s+", " ", text).strip().split()


if "norm=leaderboard" in flags:
    normal = leaderboard


def edits(ref, hyp):
    d = list(range(len(hyp) + 1))
    for i in range(1, len(ref) + 1):
        prev, d[0] = d[0], i
        for j in range(1, len(hyp) + 1):
            cur = min(d[j] + 1, d[j - 1] + 1, prev + (ref[i - 1] != hyp[j - 1]))
            prev, d[j] = d[j], cur
    return d[len(hyp)]


def read_wav(path):
    """Mono 16 kHz 16-bit samples of a RIFF WAV, as floats."""
    data = Path(path).read_bytes()
    pos, pcm = 12, b""
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        if tag == b"data":
            pcm = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    return [v / 32768.0 for v in struct.unpack("<%dh" % (len(pcm) // 2), pcm[: len(pcm) // 2 * 2])]


def run(samples):
    """attome-whisper on the samples: (words as {t, s, e}, seconds it took)."""
    payload = struct.pack("<I", len(samples)) + struct.pack("<%df" % len(samples), *samples)
    started = time.time()
    p = subprocess.run([str(exe), "--model", model, "--language", language] + gpu_args, input=payload, capture_output=True)
    took = time.time() - started
    for line in p.stdout.decode("utf-8", "replace").splitlines():
        j = json.loads(line)
        if "words" in j:
            return j["words"], took
        if "error" in j:
            raise SystemExit("program error: " + j["error"])
    raise SystemExit("no answer from the program")


def loops(words, longest=6, times=4):
    """Runs of one to `longest` words said `times` or more in a row: (how many runs, how many words they take up, the worst run)."""
    n, runs, covered, worst, i = len(words), 0, 0, "", 0
    while i < n:
        hit = 0
        for g in range(1, longest + 1):
            if i + g * times > n:
                break
            gram = words[i:i + g]
            reps = 1
            while words[i + reps * g:i + (reps + 1) * g] == gram:
                reps += 1
            if reps >= times:
                hit = g * reps
                runs += 1
                covered += hit
                if len(" ".join(gram)) * reps > len(worst):
                    worst = " ".join(gram) + " x" + str(reps)
                break
        i += hit or 1
    return runs, covered, worst


# The clips: spread evenly over the whole set, as many as `minutes` takes.
wavs = sorted(media.glob("*.wav"))
step = max(1, len(wavs) // 400)
clips, samples, cursor = [], [], 0.0
silence = [0.0] * int(GAP * RATE)
for w in wavs[::step]:
    s = read_wav(w)
    if (cursor + len(s) / RATE) / 60.0 > minutes:
        break
    clips.append({"file": w.name, "at": cursor, "len": len(s) / RATE, "ref": normal(w.with_suffix(".txt").read_text(encoding="utf-8")), "pcm": s})
    samples += s + silence
    cursor += len(s) / RATE + GAP
total_seconds = len(samples) / RATE
print(f"{len(clips)} clips, {total_seconds / 60:.1f} minutes", flush=True)

words, long_took = run(samples)
print(f"heard {len(words)} words in {long_took:.0f} s ({total_seconds / long_took:.1f} times real time)", flush=True)

# Each heard word goes to the clip it started in (a word between two clips, in the gap, goes to the clip before it).
starts = [c["at"] for c in clips]
heard = [[] for _ in clips]
first = [None] * len(clips)
j = 0
for w in words:
    while j + 1 < len(clips) and w["s"] >= starts[j + 1]:
        j += 1
    heard[j] += normal(w["t"])
    if first[j] is None:
        first[j] = w["s"]

windows = {}
for i, c in enumerate(clips):
    k = int(c["at"] // WINDOW)
    wnd = windows.setdefault(k, {"ref": 0, "err": 0, "lost": 0, "lost_s": 0.0, "off": []})
    e = edits(c["ref"], heard[i])
    wnd["ref"] += len(c["ref"])
    wnd["err"] += e
    c["errors"] = e
    if not heard[i]:
        wnd["lost"] += 1
        wnd["lost_s"] += c["len"]
    if first[i] is not None:
        wnd["off"].append(first[i] - c["at"])

control = {}
if "control" in flags:
    errs, ref, took, per = 0, 0, 0.0, {}
    for c in clips:
        got, t = run(c["pcm"])
        took += t
        e = edits(c["ref"], normal(" ".join(w["t"] for w in got)))
        errs += e
        ref += len(c["ref"])
        k = int(c["at"] // WINDOW)
        per.setdefault(k, [0, 0])
        per[k][0] += e
        per[k][1] += len(c["ref"])
    control = {"wer": round(errs / max(1, ref), 4), "errors": errs, "run_seconds": round(took, 1),
               "windows": {str(k): round(v[0] / max(1, v[1]), 4) for k, v in sorted(per.items())}}

all_heard = [w for h in heard for w in h]
n_loops, in_loops, worst = loops(all_heard)
ref_total = sum(len(c["ref"]) for c in clips)
err_total = sum(c["errors"] for c in clips)
summary = {
    "model": Path(model).name, "language": language, "clips": len(clips), "minutes": round(total_seconds / 60, 1),
    "ref_words": ref_total, "heard_words": len(all_heard), "errors": err_total, "wer": round(err_total / ref_total, 4),
    "run_seconds": round(long_took, 1), "times_real_time": round(total_seconds / long_took, 2),
    "loops": {"runs": n_loops, "words": in_loops, "worst": worst},
    "lost_clips": sum(w["lost"] for w in windows.values()), "lost_seconds": round(sum(w["lost_s"] for w in windows.values()), 1),
    "windows": {f"{int(k * WINDOW / 60)}-{int((k + 1) * WINDOW / 60)} min": {
        "wer": round(v["err"] / max(1, v["ref"]), 4), "lost_clips": v["lost"],
        "start_median_s": round(statistics.median(v["off"]), 2) if v["off"] else None} for k, v in sorted(windows.items())},
    "control": control,
}
for c in clips:
    c.pop("pcm")
Path(out).write_text(json.dumps({"summary": summary, "clips": clips}, ensure_ascii=False, indent=1), encoding="utf-8")
print(json.dumps(summary, ensure_ascii=False, indent=1))
