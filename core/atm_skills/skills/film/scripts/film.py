"""Helpers for film experiments with the cast: call the engine, build a cast, make shots, pull frames."""
import json, os, subprocess

BENCH = r'C:\Users\Computia.me\Downloads\everything\attome-bench'
BIN = r'C:\Users\Computia.me\Downloads\everything\attome-wt-h3pro\build\win-msvc-release\bin\attome.exe'
MODEL = 'minimax-h3.singularity-ref2va.int8'
STYLE = ('a cinematic drama, filmed on a real camera, natural low light, realistic skin, shallow depth of field, widescreen; '
         'no subtitles, no captions, no written text; no background music')


class Film:
    def __init__(self, work, name, canvas='1920x1080', pixels=450000):
        self.work = work
        os.makedirs(work, exist_ok=True)
        self.proj = os.path.join(work, name + '.attome')
        self.pixels = pixels
        self.env = dict(os.environ, ATTOME_ENDPOINT=r'\\.\pipe\attome-film-' + name, ATTOME_MODELS_DIR=BENCH + r'\models',
                        ATTOME_COMFYUI='http://127.0.0.1:8188', ATTOME_SETTINGS=os.path.join(work, 'settings.json'))
        open(self.env['ATTOME_SETTINGS'], 'w').write('{"comfyui": "http://127.0.0.1:8188"}')
        if os.path.exists(self.proj):
            import shutil
            shutil.rmtree(self.proj)
        subprocess.run([BIN, '--daemon', 'never', 'new', self.proj, '--name', name, '--rate', '24', '--canvas', canvas], env=self.env, capture_output=True)

    def call(self, tool, params):
        params = dict(params, project=self.proj)
        f = os.path.join(self.work, 'q_' + tool.replace('.', '_') + '.json')
        json.dump(params, open(f, 'w', encoding='utf8'), ensure_ascii=False)
        r = subprocess.run([BIN, '--daemon', 'never', '--json', 'call', tool, f], capture_output=True, env=self.env)
        out = json.loads(r.stdout.decode('utf8', 'replace'))
        if not out.get('ok'):
            raise SystemExit(tool + ' failed: ' + json.dumps(out.get('error'), ensure_ascii=False)[:700])
        return out['result']

    def shot(self, name, subjects, camera, action, lines, setting, seconds=9, seed=1, fix=None, style=STYLE, sound='a quiet room', extra=None):
        """subjects: names of the cast or plain descriptions; lines: [(subject number, text, tone)]. fix: a function on the prompt."""
        subs = [{'character': s[1:]} if s.startswith('@') else s for s in subjects]
        d = self.call('gen.dialogue_prompt', {'style': style, 'setting': setting, 'subjects': subs, 'continuous': True, 'soundscape': sound, 'music': 'none',
                                              'shots': [{'camera': camera, 'action': action,
                                                         'lines': [{'subject': a, 'text': b, 'tone': c, 'language': 'English'} for a, b, c in lines]}]})
        prompt = fix(d['prompt']) if fix else d['prompt']
        p = {'prompt': prompt, 'model': MODEL, 'seconds': seconds, 'seed': seed, 'name': name, 'settings': {'steps': 8}, 'with_audio': True}
        if d.get('characters'):
            p['characters'] = d['characters']
            p['voices'] = d.get('voices', [])
        p.update(extra or {})
        clip = self.call('gen.create_clip', p)['clip']
        obj = self.call('project.get', {'id': clip})['object']
        for nid, node in obj['media_ref']['workflow']['nodes'].items():
            if node['kind'] == 'attome.project':
                self.call('project.patch', {'patch': {'ops': [{'op': 'replace', 'path': f'{clip}/media_ref/workflow/nodes/{nid}/settings/pixels', 'value': self.pixels}]}})
        return clip

    def run(self):
        r = subprocess.run([BIN, '--daemon', 'never', '--json', 'gen', 'run', self.proj], capture_output=True, env=self.env)
        out = json.loads(r.stdout.decode('utf8', 'replace'))
        if not out.get('ok'):
            raise SystemExit('gen run failed: ' + json.dumps(out.get('error'))[:700])

    def take(self, clip):
        m = self.call('project.get', {'id': clip})['object']['media_ref']
        return os.path.join(self.proj, m['takes'][m['selected']]['outputs']['video']['path'].replace('/', os.sep))


def frame(video, at, out, crop=None):
    vf = ['-vf', crop] if crop else []
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', str(at), '-i', video] + vf + ['-frames:v', '1', out], check=True)
    return out


def sheet(videos, out, frames=(40, 110, 180), width=320):
    args, fc = [], ''
    for i, v in enumerate(videos):
        args += ['-i', v]
        sel = '+'.join(f'eq(n\\,{n})' for n in frames)
        fc += f"[{i}:v]select='{sel}',scale={width}:-1,tile={len(frames)}x1[t{i}];"
    fc += ''.join(f'[t{i}]' for i in range(len(videos))) + f'vstack=inputs={len(videos)}' if len(videos) > 1 else fc[:-1].rsplit('[t0]', 1)[0]
    if len(videos) == 1:
        sel = '+'.join(f'eq(n\\,{n})' for n in frames)
        fc = f"[0:v]select='{sel}',scale={width}:-1,tile={len(frames)}x1"
    subprocess.run(['ffmpeg', '-v', 'error', '-y'] + args + ['-filter_complex', fc, '-fps_mode', 'passthrough', '-frames:v', '1', out], check=True)
