import wave, numpy as np
sr = 48000
dur = 0.7          # the whoosh builds for 0.45 s and lands at 0.5 s (the cut), then tails off
n = int(dur * sr)
t = np.arange(n) / sr
rng = np.random.default_rng(11)
noise = rng.standard_normal(n)

# state-variable band-pass whose centre sweeps up (a fast "swoosh")
f0, f1 = 250.0, 5200.0
land = 0.5
centre = np.where(t < land, f0 * (f1 / f0) ** (t / land), f1 * np.exp(-(t - land) * 9))
q = 1.6
y = np.zeros(n)
low = band = 0.0
for i in range(n):
    f = 2 * np.sin(np.pi * min(centre[i], 9000.0) / sr)
    low += f * band
    high = noise[i] - low - band / q
    band += f * high
    y[i] = band
env = np.where(t < land, (t / land) ** 2.2, np.exp(-(t - land) * 11))
y *= env
y = y / np.abs(y).max()
# the impact on the cut: a soft low thump with a click of air
thump = np.sin(2 * np.pi * (95 - 55 * np.clip(t - land, 0, 1)) * t) * np.exp(-np.clip(t - land, 0, None) * 16) * (t >= land)
thump[: int(land * sr)] = 0
out = y * 0.8 + thump * 0.7
out[-2400:] *= np.linspace(1, 0, 2400)
out = out / np.abs(out).max() * 0.85
# a little width: the whoosh travels from left to right
pan = np.clip(t / land, 0, 1)
left = out * (1 - 0.35 * pan)
right = out * (0.65 + 0.35 * pan)
pcm = (np.stack([left, right], axis=1) * 32767).astype('<i2')
with wave.open(r'C:\Users\Computia.me\Documents\Attome\sfx_whoosh.wav', 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(pcm.tobytes())
print('ok', dur, 'landing at', land)
