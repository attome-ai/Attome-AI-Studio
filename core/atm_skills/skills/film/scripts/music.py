"""Music from words with ACE-Step 1.5 (XL turbo, the 4B language model) through ComfyUI.
  python music.py out.wav seconds bpm "key" "tags" [seed]"""
import json, os, sys, time, urllib.request, shutil
HOST = 'http://127.0.0.1:8188'
out, seconds, bpm, key, tags = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5]
seed = int(sys.argv[6]) if len(sys.argv) > 6 else 1
g = {
    'unet': {'class_type': 'UNETLoader', 'inputs': {'unet_name': 'acestep_v1.5_xl_turbo_bf16.safetensors', 'weight_dtype': 'default'}},
    'shift': {'class_type': 'ModelSamplingAuraFlow', 'inputs': {'model': ['unet', 0], 'shift': 3.0}},
    'clip': {'class_type': 'DualCLIPLoader', 'inputs': {'clip_name1': 'qwen_0.6b_ace15.safetensors', 'clip_name2': 'qwen_4b_ace15.safetensors', 'type': 'ace'}},
    'vae': {'class_type': 'VAELoader', 'inputs': {'vae_name': 'ace_1.5_vae.safetensors'}},
    'pos': {'class_type': 'TextEncodeAceStepAudio1.5', 'inputs': {'clip': ['clip', 0], 'tags': tags, 'lyrics': '[instrumental]', 'seed': seed, 'bpm': bpm, 'duration': seconds,
                                                                  'timesignature': '4', 'language': 'en', 'keyscale': key, 'generate_audio_codes': True, 'cfg_scale': 2.0,
                                                                  'temperature': 0.85, 'top_p': 0.9, 'top_k': 0, 'min_p': 0.0}},
    'neg': {'class_type': 'ConditioningZeroOut', 'inputs': {'conditioning': ['pos', 0]}},
    'lat': {'class_type': 'EmptyAceStep1.5LatentAudio', 'inputs': {'seconds': seconds, 'batch_size': 1}},
    'ks': {'class_type': 'KSampler', 'inputs': {'model': ['shift', 0], 'positive': ['pos', 0], 'negative': ['neg', 0], 'latent_image': ['lat', 0], 'seed': seed, 'steps': 8, 'cfg': 1.0,
                                                 'sampler_name': 'euler', 'scheduler': 'simple', 'denoise': 1.0}},
    'dec': {'class_type': 'VAEDecodeAudio', 'inputs': {'samples': ['ks', 0], 'vae': ['vae', 0]}},
    'save': {'class_type': 'SaveAudio', 'inputs': {'audio': ['dec', 0], 'filename_prefix': 'attome_music'}},
}
t0 = time.time()
req = urllib.request.Request(HOST + '/prompt', data=json.dumps({'prompt': g}).encode(), headers={'Content-Type': 'application/json'})
try:
    pid = json.load(urllib.request.urlopen(req))['prompt_id']
except urllib.error.HTTPError as e:
    raise SystemExit('refused: ' + e.read().decode()[:1500])
while True:
    h = json.load(urllib.request.urlopen(HOST + '/history/' + pid))
    if pid in h:
        break
    time.sleep(1)
h = h[pid]
if h.get('status', {}).get('status_str') != 'success':
    raise SystemExit('failed: ' + json.dumps(h.get('status'))[:1500])
a = h['outputs']['save']['audio'][0]
url = f"{HOST}/view?filename={a['filename']}&subfolder={a.get('subfolder', '')}&type={a.get('type', 'output')}"
tmp = out + '.' + a['filename'].rsplit('.', 1)[-1]
with urllib.request.urlopen(url) as r, open(tmp, 'wb') as f:
    shutil.copyfileobj(r, f)
os.system(f'ffmpeg -v error -y -i "{tmp}" -ar 48000 -ac 2 "{out}"')
print(f'made {out} in {time.time() - t0:.0f} s')
