import os, sys, time, torch, numpy as np, soundfile as sf
from kokoro import KPipeline, KModel
d = os.environ.get('KOKORO_DIR', r'C:\Users\Computia.me\Downloads\everything\attome-bench\ComfyUI_windows_portable\ComfyUI\models\Kokorotts\Kokoro-82M')
dev = 'cuda' if torch.cuda.is_available() else 'cpu'
print('device', dev)
model = KModel(config=os.path.join(d, 'config.json'), model=os.path.join(d, 'kokoro-v1_0.pth')).to(dev).eval()
text = "Hour three: school pranks. The teacher's pencil is haunted. Hour six: skip every line at the theme park!"
out = r'C:\Users\Computia.me\Documents\Attome\voice_samples3'
os.makedirs(out, exist_ok=True)
for v in sys.argv[1:]:
    pipe = KPipeline(lang_code=v[0], model=model)
    vt = torch.load(os.path.join(d, 'voices', v + '.pt'), weights_only=True)
    t = time.time()
    parts = [a for _, _, a in pipe(text, voice=vt, speed=1.0)]
    audio = np.concatenate([p.cpu().numpy() if hasattr(p, 'cpu') else p for p in parts])
    sf.write(os.path.join(out, v + '.wav'), audio, 24000)
    print(v, round(len(audio) / 24000, 2), 's in', round(time.time() - t, 2), 's')

