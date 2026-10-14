"""A small driver for making a film with Attome's Tools, for an agent to import or adapt. It holds no knowledge of its own: every step
is one Tool call, and the rules are in the skill `film`.

    from attome_film import Film
    f = Film('<folder>/MyFilm')                                 # makes <folder>/MyFilm/MyFilm.attome
    f.call('character.create', name='A', description='...', references=[{'path': 'a_portrait.mp4', 'at': 0.4, 'crop': [x, y, w, h]}], voice='a.wav')
    a = f.shot('s1', ['@A', '@B'], camera='...', action='...', lines=[(1, '<a line>', '<its tone>')], setting='{Place.description}')
    b = f.shot('s2', ..., continue_from=a)                      # or start_from=a, start_at=6.0, start_crop=[...]
    f.cut([a, b], '<folder>/MyFilm/film.mp4', music={'tags': '<genre, instruments, mood>', 'bpm': 90, 'key': 'C major'})

The engine is found in ATTOME_BIN (or `attome` on the PATH). It needs to know where the models are and where the generator runs:
ATTOME_MODELS_DIR and ATTOME_COMFYUI in the environment, or once `attome call models.set_folder` / `gen.set_comfyui` (remembered)."""
import json, os, shutil, subprocess
from fractions import Fraction

BIN = os.environ.get('ATTOME_BIN') or shutil.which('attome') or 'attome'
MODEL = 'minimax-h3.singularity-ref2va.int8'   # the one that takes reference pictures (gen.models lists them)
STYLE = ('a cinematic drama, filmed on a real camera, natural light, realistic skin, shallow depth of field, widescreen; '
         'no subtitles, no captions, no written text; no background music')


class Film:
    def __init__(self, folder, name=None, canvas='1920x1080', megapixels=0.45, new=True):
        """megapixels 0.45 is a draft (work at it until the content is right); 0.9 is the engine's default."""
        self.folder = folder
        name = name or os.path.basename(os.path.normpath(folder))
        self.project = os.path.join(folder, name + '.attome')
        self.megapixels = megapixels
        self.env = dict(os.environ, ATTOME_ENDPOINT=r'\\.\pipe\attome-film-' + name)
        os.makedirs(folder, exist_ok=True)
        if new or not os.path.exists(self.project):
            if os.path.exists(self.project):
                shutil.rmtree(self.project)
            self._cli('new', self.project, '--name', name, '--rate', '24', '--canvas', canvas)

    def _cli(self, *args):
        return subprocess.run([BIN, '--daemon', 'never', *args], capture_output=True, env=self.env)

    def call(self, tool, **params):
        """Any Tool (`attome tools` lists them). The project is added to the parameters."""
        params.setdefault('project', self.project)
        r = subprocess.run([BIN, '--daemon', 'never', '--json', 'call', tool, '-'], input=json.dumps(params).encode('utf8'), capture_output=True, env=self.env)
        out = json.loads(r.stdout.decode('utf8', 'replace') or '{"ok": false, "error": {"message": "no answer from attome"}}')
        if not out.get('ok'):
            raise RuntimeError(tool + ': ' + json.dumps(out.get('error'), ensure_ascii=False)[:900])
        return out['result']

    def shot(self, name, subjects, camera, action, lines=(), setting='', seconds=5, seed=1, style=STYLE, sound='', takes=3, hold=False, cut_limit=0.15,
             continue_from=None, start_from=None, start_at=None, start_crop=None, make=True):
        """One shot = one generative clip. subjects: '@Name' for an entry of the cast, or a plain description. lines: [(subject number, text, tone)].
        The clip is made and CHECKED (gen.check_take); a Take with problems is made again, up to `takes` times, and the best one is selected.
        Returns the clip ID; self.report[clip] has the check of the Take that plays."""
        subs = [{'character': s[1:]} if s.startswith('@') else s for s in subjects]
        d = self.call('gen.dialogue_prompt', style=style, setting=setting, subjects=subs, continuous=True, soundscape=sound, music='none',
                      shots=[{'camera': camera, 'action': action, 'lines': [{'subject': a, 'text': b, 'tone': c} for a, b, c in lines]}])
        p = dict(prompt=d['prompt'], model=MODEL, seconds=seconds, seed=seed, name=name, settings={'steps': 8}, with_audio=True, megapixels=self.megapixels)
        if d.get('characters'):
            p['characters'], p['voices'] = d['characters'], d.get('voices', [])
        for key, value in (('continue_from', continue_from), ('start_from', start_from), ('start_at', start_at), ('start_crop', start_crop)):
            if value is not None:
                p[key] = value
        clip = self.call('gen.create_clip', **p)['clip']
        if make:
            self.make(clip, takes=takes, hold=hold or bool(start_crop and start_crop != [0, 0, 1, 1]), cut_limit=cut_limit)
        return clip

    report = {}

    def make(self, clip, takes=3, hold=False, cut_limit=0.15):
        best = None
        for n in range(takes):
            self._cli('gen', 'run', self.project, '--clip', clip, *(['--new-take'] if n else []))
            r = self.call('gen.check_take', clip=clip, hold=hold, cut_limit=cut_limit)
            if best is None or len(r['problems']) < len(best['problems']):
                best = r
            if r['ok']:
                break
        if best['take'] != r['take']:
            self.call('gen.select_take', clip=clip, take=best['take'])
        self.report[clip] = best
        return best

    def file(self, clip):
        """The video file of the Take that plays (to look at its frames)."""
        m = self.call('project.get', id=clip)['object']['media_ref']
        return os.path.join(self.project, m['takes'][m['selected']]['outputs']['video']['path'].replace('/', os.sep))

    def cut(self, order, output, music=None, speech_lufs=-17.0, quiet=(), quiet_lufs=-30.0, music_lufs=-25.0, duck_db=8.0):
        """The film: the clips of `order` one after another on a new track (every other clip of the first track is deleted), their sound at one
        level (`quiet`: clips with only the place's air), a score under them (music: the parameters of music.make), and the export."""
        seq = self.call('project.inspect', level='tracks')['data']['sequences'][0]
        old = [t for t in seq['tracks'] if any(c['id'] in order for c in t.get('clip_list', []))]
        ops, t = [{'op': 'add_track', 'id': '$new:film', 'kind': 'video', 'name': 'Film', 'position': 'top'}], Fraction(0)
        for clip in order:
            ops.append({'op': 'move', 'clip': clip, 'to': f'{t.numerator}/{t.denominator}', 'track': '$new:film'})
            t += Fraction(self.call('project.get', id=clip)['object']['timing']['duration'])
        ops += [{'op': 'delete_track', 'track': tr['id']} for tr in old]
        film = self.call('timeline.edit', ops=ops, label='The cut')['id_map']['$new:film']
        spoken = [c for c in order if c not in quiet]
        if spoken:
            self.call('audio.match', clips=spoken, lufs=speech_lufs)
        if quiet:
            self.call('audio.match', clips=list(quiet), lufs=quiet_lufs)
        if music:
            made = self.call('music.make', seconds=min(240.0, float(t) + 2.0), **music)
            added = self.call('timeline.edit', label='Score', ops=[{'op': 'add_clip', 'id': '$new:music', 'asset': made['asset_id'], 'at': '0s', 'duration': f'{float(t):.3f}s',
                                                                   'gain_db': round(music_lufs - made['lufs'], 1), 'fade_in': '1.5s', 'fade_out': '3s'}])
            self.call('audio.duck', clip=added['id_map']['$new:music'], over=[film], db=duck_db, ramp=0.3)
        self.call('project.save')
        r = self._cli('render', self.project, '-o', output)
        if r.returncode or not os.path.exists(output):
            raise RuntimeError('render: ' + (r.stdout + r.stderr).decode('utf8', 'replace')[-600:])
        return {'output': output, 'seconds': float(t), 'sound': self.call('audio.analyze', path=output)}
