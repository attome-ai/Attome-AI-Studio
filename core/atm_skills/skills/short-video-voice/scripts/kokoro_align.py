import json, os, sys, torch
from kokoro import KPipeline, KModel

d = os.environ.get('KOKORO_DIR', r'C:\Users\Computia.me\Downloads\everything\attome-bench\ComfyUI_windows_portable\ComfyUI\models\Kokorotts\Kokoro-82M')
lines = json.load(open(sys.argv[1], encoding='utf-8'))
dev = 'cuda' if torch.cuda.is_available() else 'cpu'
model = KModel(config=os.path.join(d, 'config.json'), model=os.path.join(d, 'kokoro-v1_0.pth')).to(dev).eval()
pipes = {}
result = []
for ln in lines:
    v = ln['voice']
    if v[0] not in pipes:
        pipes[v[0]] = KPipeline(lang_code=v[0], model=model)
    vt = torch.load(os.path.join(d, 'voices', v + '.pt'), weights_only=True)
    offset = 0.0
    words = []
    cur = None
    for r in pipes[v[0]](ln['text'], voice=vt, speed=ln['speed'], split_pattern=r'\n+'):
        for t in (r.tokens or []):
            s, e = t.start_ts, t.end_ts
            if cur is None:
                cur = {'text': '', 'start': None, 'end': None}
            cur['text'] += t.text
            if s is not None:
                cur['start'] = offset + s if cur['start'] is None else cur['start']
            if e is not None:
                cur['end'] = offset + e
            if t.whitespace:
                words.append(cur)
                cur = None
        offset += len(r.audio) / 24000.0 if r.audio is not None else 0.0
    if cur is not None:
        words.append(cur)
    result.append({'words': words, 'length': offset})
json.dump(result, open(sys.argv[2], 'w', encoding='utf-8'))
for r in result:
    print(len(r['words']), 'words', round(r['length'], 2), 's')
