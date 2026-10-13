"""The test scene of the film lab ("Newsroom": six shots, two people, eleven lines), as one function with every choice a parameter,
so an experiment changes one of them. Each take is checked (a cut inside the shot, the words against the script).

  python newsroom.py <name> [key=value ...]
     seed=4000        the seed base (shot n gets seed + n * 17)
     takes=1          takes for each shot: 1 = one take and its verdict (to measure how often shots fail); more = retake until one passes
     room=short       short | medium | long
     shots=1,2,3,4,5,6
     pixels=450000
     tail=0           seconds at the end of each shot in which nobody speaks (asked for in the prompt)
     grid=0           1 = each clip is 226 frames long (9.42 s), exactly what the model makes for 9 s
     angles=1         1 = one face picture a person, 2 = front and three-quarter (results\\proof4 portraits)
"""
import os, sys, shutil, subprocess, time, json, re, difflib
sys.path.insert(0, os.path.dirname(__file__))
from film import Film, BENCH, BIN, sheet

J = os.path.join
RES = J(BENCH, 'results')
ROOMS = {
    'short': 'a large open newsroom in the middle of the night, rows of dark desks, a few glowing monitors, rain on tall black windows, one warm desk lamp; it is dark outside',
    'medium': ('a large open newsroom in the middle of the night: a row of tall black windows along the left wall with rain on the glass, rows of dark desks with a few glowing '
               'monitors, grey carpet, one brass desk lamp with a warm glow on the desk nearest the windows; it is dark outside'),
    'dark': ('a large open newsroom in the middle of the night: a row of tall black windows along the left wall with rain on the glass, rows of dark desks with a few glowing '
             'monitors, grey carpet, one brass desk lamp with a warm glow on the desk nearest the windows; the ceiling lights are off, the only light is the lamp and the monitors; it is dark outside'),
    'long': ('the fourth-floor newsroom of a city newspaper in the middle of the night: one long open room with a low white grid ceiling, a row of six tall sash windows along the '
             'left wall that are completely black with night, rain running down the dark glass, four rows of black desks with dark monitors, grey carpet, and a single brass desk '
             'lamp with a warm orange glow on the desk nearest the windows; the room is dark, lit only by that lamp and the cold blue of two monitors; it is night'),
}
MCU = 'a medium close-up at eye level, filmed from across the desk on a long lens by someone else: head and shoulders, the whole head in frame with space above it; not a selfie'
S, L = '@Sam', '@Lena'
SHOTS = {
    1: ([L], 'a slow push-in from a wide shot', '<Subject 1> sits alone at a desk under the lamp, typing, then rubs her eyes', [(1, 'Three a.m., and still no lead.', 'tired, dry')]),
    2: ([S, L], 'a medium two-shot from the side', '<Subject 1> hurries in carrying a laptop and stops at the desk of <Subject 2>, who does not look up',
        [(1, 'Lena. I found the footage.', 'breathless, excited'), (2, 'You said that yesterday.', 'flat, dry')]),
    3: ([S], MCU, '<Subject 1> sits at a desk with both hands on a laptop, opens it and turns it around, his face lit by the screen',
        [(1, 'This time it is real. Look at the timestamp.', 'urgent, sure')]),
    4: ([L], MCU, '<Subject 1> sits at her desk and leans a little toward a screen out of frame; her expression changes from bored to alarmed',
        [(1, 'That is the harbour. That is tonight.', 'quiet, shaken')]),
    5: ([S, L], 'a steady two-shot from behind the laptop, both faces lit by the screen', '<Subject 1> and <Subject 2> stare at the screen side by side',
        [(1, 'So do we run it?', 'hopeful'), (2, 'We verify it first.', 'firm')]),
    6: ([L, S], 'a tracking shot following them toward the door', '<Subject 1> grabs her coat from the chair and walks toward the exit; <Subject 2> hurries after her',
        [(1, 'Get your coat.', 'brisk'), (2, 'It is three in the morning.', 'protesting'), (1, 'Then we are late.', 'dry')]),
}


def norm(text):
    t = text.lower().replace('3:00 am', 'three am').replace('3 am', 'three am').replace('a.m.', 'am').replace('harbour', 'harbor').replace('time stamp', 'timestamp')
    t = re.sub(r'\*[^*]*\*', ' ', t)  # *door opens*: a sound the listener named, not a word
    return re.sub(r'[^a-z ]', ' ', t).split()


def scene_max(video):
    r = subprocess.run(['ffmpeg', '-hide_banner', '-i', video, '-vf', "select='gte(scene,0)',metadata=print:file=-", '-an', '-f', 'null', '-'], capture_output=True)
    vals = [float(l.split('=')[-1]) for l in r.stdout.decode('utf8', 'replace').splitlines() if 'scene_score' in l]
    return max(vals) if vals else 0.0


def check(video, want, clip_seconds=9.0):
    j = video + '.words.json'
    subprocess.run([sys.executable, J(BENCH, 'scripts', 'words.py'), video, j, 'en'], capture_output=True)
    words = [x for x in json.load(open(j, encoding='utf8')) if not x['text'].startswith('*') and not x['text'].endswith('*')]
    heard = ' '.join(x['text'] for x in words)
    a, b = norm(want), norm(heard)
    ratio = difflib.SequenceMatcher(None, a, b).ratio()
    cut = scene_max(video)
    problems = []
    if cut > 0.15:
        problems.append(f'cut {cut:.2f}')
    if ratio < 0.9:
        problems.append(f'words {ratio:.2f}: "{heard}"')
    elif len(b) > len(a):
        problems.append(f'extra words: "{heard}"')
    # the line must be over before the clip ends: the sound of the last 0.15 s is the room, not a voice (speech measures about
    # -20 dB here, the room about -45). A listener's word times are no use for this: it stretches the last word to the end of the file.
    late = 0.0
    import numpy as np
    x = np.frombuffer(subprocess.run(['ffmpeg', '-v', 'error', '-i', video, '-vn', '-ac', '1', '-ar', '16000', '-f', 'f32le', '-'], capture_output=True).stdout, dtype='<f4')
    n = int(min(len(x) / 16000.0, clip_seconds) * 16000)
    tail = x[max(0, n - 2400):n]
    level = 20 * np.log10(np.sqrt((tail ** 2).mean()) + 1e-9) if len(tail) else -90.0
    if level > -34.0:
        late = 1.0
        problems.append(f'still speaking at the cut ({level:.0f} dB in the last 0.15 s)')
    return problems, (0 if cut > 0.15 else 1) + ratio - 0.05 * max(0, len(b) - len(a)) - max(0.0, late), cut


def cast(f, angles=1):
    v21, cd, p4 = J(RES, 'trailer_v21'), J(RES, 'cast_demo'), J(RES, 'proof4')
    if angles == 2:
        sam = [{'path': J(p4, 'Sam_portrait.mp4'), 'at': 0.33, 'crop': [0.35, 0.0, 0.34, 0.80]}, {'path': J(p4, 'Sam_portrait.mp4'), 'at': 3.83, 'crop': [0.38, 0.0, 0.38, 0.82]}]
        lena = [{'path': J(p4, 'Lena_portrait.mp4'), 'at': 0.33, 'crop': [0.28, 0.02, 0.44, 0.80]}, {'path': J(p4, 'Lena_portrait.mp4'), 'at': 3.83, 'crop': [0.28, 0.02, 0.44, 0.80]}]
    else:
        sam = [{'path': J(v21, 'face.png'), 'label': 'face', 'crop': [0.19, 0.10, 0.62, 0.50]}]
        lena = [{'path': J(cd, 'lena.mp4'), 'label': 'face', 'at': 2.0, 'crop': [0.34, 0.0, 0.32, 0.74]}]
    f.call('character.create', {'name': 'Sam', 'description': 'a man in his late twenties with short dark hair, a light beard and a plain white t-shirt',
                                'references': sam, 'voice': J(v21, 'voice_creator.wav')})
    f.call('character.create', {'name': 'Lena', 'description': 'a woman in her thirties with shoulder-length auburn hair, green eyes and a navy blazer',
                                'references': lena, 'voice': J(cd, 'lena_voice.wav')})


def run(name, seed=4000, takes=1, room='short', shots=(1, 2, 3, 4, 5, 6), pixels=450000, angles=1, tail=0, grid=0):
    t0 = time.time()
    w = J(RES, name)
    f = Film(w, 'Film', pixels=pixels)
    cast(f, angles)
    f.call('character.create', {'name': 'Newsroom', 'kind': 'location', 'description': ROOMS[room]})
    log, vids = [], []
    for n in shots:
        who, cam, act, lines = SHOTS[n]
        want = ' '.join(l[1] for l in lines)
        if tail:  # leave room after the last line: the model otherwise paces the lines to the very end of the clip
            act = act + f'. All the lines are spoken in the first {9 - tail} seconds; for the last {tail} seconds nobody speaks and they hold their look'
        secs = 226 / 24 if grid else 9  # grid: the clip is as long as what the model makes (17k + 5 frames), so no frame of the take is cut off
        clip = f.shot(f'Shot {n}', who, cam, act, lines, '{Newsroom.description}', seconds=secs, seed=seed + n * 17, sound='a quiet newsroom hum, rain on glass')
        best = None
        for take in range(1, takes + 1):
            subprocess.run([BIN, '--daemon', 'never', '--json', 'gen', 'run', f.proj, '--clip', clip] + (['--new-take'] if take > 1 else []), capture_output=True, env=f.env)
            m = f.call('project.get', {'id': clip})['object']['media_ref']
            tid = m['selected']
            video = J(f.proj, m['takes'][tid]['outputs']['video']['path'].replace('/', os.sep))
            shown = float(f.call('gen.status', {})['clips'][-1].get('seconds', 0) or 0) if False else secs
            problems, score, cut = check(video, want, 99.0 if not grid else secs)
            log.append({'shot': n, 'take': take, 'ok': not problems, 'problems': problems, 'cut': round(cut, 3)})
            print(f'{name} shot {n} take {take}: ' + ('OK' if not problems else '; '.join(problems)), flush=True)
            if best is None or score > best[0]:
                best = (score, tid, video)
            if not problems:
                break
        if m['selected'] != best[1]:
            f.call('project.patch', {'patch': {'ops': [{'op': 'replace', 'path': clip + '/media_ref/selected', 'value': best[1]}]}})
        out = J(w, f'shot{n}.mp4')
        shutil.copy(best[2], out)
        vids.append(out)
    subprocess.run([BIN, '--daemon', 'never', 'render', f.proj, '-o', J(w, name + '.mp4')], env=f.env, capture_output=True)
    sheet(vids, J(w, 'sheet.png'), frames=(4, 60, 120, 190), width=300)
    first = [e for e in log if e['take'] == 1]
    summary = {'name': name, 'seed': seed, 'room': room, 'angles': angles, 'pixels': pixels, 'first_take_ok': sum(e['ok'] for e in first), 'shots': len(first),
               'takes': len(log), 'seconds': int(time.time() - t0), 'log': log}
    json.dump(summary, open(J(w, 'summary.json'), 'w'), indent=1)
    print(f"{name}: {summary['first_take_ok']} of {summary['shots']} shots passed on the first take; {summary['takes']} takes; {summary['seconds']} s", flush=True)
    return summary


if __name__ == '__main__':
    kw = {}
    for a in sys.argv[2:]:
        k, v = a.split('=', 1)
        kw[k] = tuple(int(x) for x in v.split(',')) if k == 'shots' else v if k == 'room' else int(v)
    run(sys.argv[1], **kw)
