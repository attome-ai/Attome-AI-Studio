"""How far a shot's picture moves away from its first frame: 1 - the correlation of small grey copies of the first frame and of later
frames (the lowest of them). A close-up that holds its place stays low; one where the camera leaves for another room goes high."""
import subprocess, sys, numpy as np
def frames(video, w=48, h=27, every=12):
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', video, '-vf', f"select='not(mod(n,{every}))',scale={w}:{h},format=gray", '-fps_mode', 'passthrough', '-f', 'rawvideo', '-'], capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.uint8).reshape(-1, h * w).astype(float)
def drift(video):
    f = frames(video)
    if len(f) < 2: return 0.0
    a = f[0] - f[0].mean()
    worst = 1.0
    for b in f[1:]:
        b = b - b.mean()
        worst = min(worst, float((a * b).sum() / (np.sqrt((a * a).sum() * (b * b).sum()) + 1e-9)))
    return 1.0 - worst
if __name__ == '__main__':
    for v in sys.argv[1:]:
        print('%.2f  %s' % (drift(v), v))
