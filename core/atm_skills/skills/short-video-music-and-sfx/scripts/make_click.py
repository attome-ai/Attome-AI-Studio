import wave, numpy as np
sr = 48000
n = int(0.14 * sr)
t = np.arange(n) / sr
rng = np.random.default_rng(5)
# a crisp UI click: a very short noise tick, a 2.4 kHz body that falls quickly, and a low thump under it
tick = rng.standard_normal(n) * np.exp(-t * 900) * 0.5
body = np.sin(2 * np.pi * (2400 - 6000 * t) * t) * np.exp(-t * 55) * 0.7
thump = np.sin(2 * np.pi * 140 * t) * np.exp(-t * 60) * 0.5
y = tick + body + thump
y[:48] *= np.linspace(0, 1, 48)  # no click from the edge itself
y = y / np.abs(y).max() * 0.8
pcm = (np.stack([y, y], axis=1) * 32767).astype('<i2')
with wave.open(r'C:\Users\Computia.me\Documents\Attome\sfx_click.wav', 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(pcm.tobytes())
print('ok')
