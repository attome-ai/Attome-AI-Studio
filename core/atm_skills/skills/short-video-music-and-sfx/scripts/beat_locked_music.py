import json, sys, wave
import numpy as np, av

SRC = sys.argv[2] if len(sys.argv) > 2 else r'C:\Users\Computia.me\Downloads\Crystal Castles - Transgender.mp3'  # usage: beat_locked_music.py voices.json [song] [out.wav]
OUT = sys.argv[3] if len(sys.argv) > 3 else r'C:\Users\Computia.me\Documents\Attome\skeleton_song150.wav'
voices = json.load(open(sys.argv[1]))  # [[start, end], ...] in timeline seconds
TARGET_BPM = 150.0
SEC_FROM = 62.0   # analyse and cut from the loud section (it starts at 70 s)
c = av.open(SRC)
st = c.streams.audio[0]
sr = st.rate
ch = []
for f in c.decode(st):
    a = f.to_ndarray().astype(np.float32)
    ch.append(a if f.format.is_planar else a.reshape(-1, len(f.layout.channels)).T)
x = np.concatenate(ch, axis=1)  # (2, n)
mono = x.mean(axis=0)

# tempo and phase: score a comb of beats against the onset curve at 5 ms
hop = 220
n = len(mono) // hop
env = np.sqrt((mono[:n * hop].reshape(n, hop) ** 2).mean(axis=1))
flux = np.maximum(0, np.diff(env, prepend=env[0]))
a, b = int(12 / 0.005), int(48 / 0.005)
seg = flux[a:b]
best = (0, 0, 0)
for bpm in np.arange(125.0, 150.0, 0.02):
    p = 60.0 / bpm / 0.005
    k = np.arange(0, int(len(seg) / p) - 1)
    for ph in np.arange(0, p, 1.0):
        idx = np.round(ph + k * p).astype(int)
        s = seg[idx].sum()
        if s > best[0]:
            best = (s, bpm, ph)
_, bpm, ph = best
t0 = 12 + ph * 0.005  # a beat at this second of the song
print('bpm', round(bpm, 3), 'beat at', round(t0, 3))
period = 60.0 / bpm
# move back to a bar line a little before 72 s: the beat nearest to SEC_FROM... keep it on the grid
beat = t0 - period * np.floor((t0 - SEC_FROM) / period)
beat = t0 - period * np.floor((t0 - 12.0) / period)  # last beat at or after 70 s
print('cut from', round(beat, 3))
ratio = TARGET_BPM / bpm  # playing faster
dur = 40.0
need = int((dur * ratio + 1) * sr)
i0 = int(beat * sr)
seg2 = x[:, i0:i0 + need]
# resample by `ratio`: the output sample j reads the input at j * ratio
osr = 48000
nout = int(dur * osr)
pos = np.arange(nout) * ratio * sr / osr
left = np.interp(pos, np.arange(seg2.shape[1]), seg2[0])
right = np.interp(pos, np.arange(seg2.shape[1]), seg2[1])
y = np.stack([left, right])
# the first beat now sits at t = 0; a beat is 0.4 s: put the grid on the scene starts (3.1 + 4k), i.e. 0.3 s mod 0.4
shift = int(0.3 * osr)
y = np.concatenate([np.zeros((2, shift), np.float32), y], axis=1)[:, :nout]
# ducking baked in: the music sits at 0.30 where nobody speaks, 0.16 under the voice (smoothed)
gain = np.full(nout, 0.24, np.float32)
for s, e in voices:
    gain[int(s * osr):int(e * osr)] = 0.10
k = int(0.12 * osr)
gain = np.convolve(gain, np.ones(k) / k, mode='same')
y *= gain
peak = float(np.abs(y).max())
y = np.clip(y, -0.98, 0.98)
pcm = (np.clip(y.T, -1, 1) * 32767).astype('<i2')
with wave.open(OUT, 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(osr); w.writeframes(pcm.tobytes())
print('wrote', OUT, 'peak', round(float(np.abs(y).max()), 3))


