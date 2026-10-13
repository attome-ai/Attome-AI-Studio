"""'Why things fall' (3 minutes, explains gravity). Stage 1: the two masters (the apple; the cloth) as chains, and every cutaway with the
grandfather's voice over it. Each take is checked (cut, words, a voice at the cut) and made again up to three times; the best is kept.
An off-screen voice is a cast entry of its own with a voice sample and no picture."""
import os, sys, shutil, subprocess, time, json
sys.path.insert(0, os.path.dirname(__file__))
from film import Film, BENCH, BIN
import newsroom as N
from gravity_a import W
from gravity_probe import cast
J = os.path.join
OUT = J(W, 'shots')
SET = '{Garden.description}'
M, E, V = '@Mira', '@Elias', '@Narrator'
NIGHT = 'a quiet night garden, crickets'
TWO = ('one continuous steady medium two-shot at eye level from in front of the wooden table, the camera does not move and does not cut: <Subject 1> and <Subject 2> sit side by side '
       'on a bench behind the table, <Subject 1> at the left of the frame, <Subject 2> at the right; the brass telescope stands at the left edge, the lantern on the table')
VO = '; nobody is in the picture; the voice of <Subject 1> is heard from off screen'


def setup(name='Gravity'):
    f = Film(OUT, name)
    cast(f)
    f.call('character.create', {'name': 'Narrator', 'description': 'the warm slow voice of an old man, heard from off screen, never seen', 'voice': J(W, 'Elias_voice.wav')})
    return f


def make(f, key, who, cam, act, lines, place, secs, seed, sound=NIGHT, extra=None, takes=3, calm=0.15):
    """One shot, checked; up to `takes` takes; the best is copied to <key>.mp4. Returns the clip."""
    c = f.shot(key, who, cam, act, lines, place, seconds=secs, seed=seed, sound=sound, extra=extra)
    want = ' '.join(l[1] for l in lines)
    best = None
    for t in range(1, takes + 1):
        subprocess.run([BIN, '--daemon', 'never', '--json', 'gen', 'run', f.proj, '--clip', c] + (['--new-take'] if t > 1 else []), capture_output=True, env=f.env)
        v = f.take(c)
        if lines:
            problems, score, cut = N.check(v, want, 99.0)
            problems = [p for p in problems if not p.startswith('cut ')]
        else:
            problems, score, cut = [], 1.0, N.scene_max(v)
        if cut > calm:
            problems.append(f'cut {cut:.2f}')
            score -= 1
        print(key, 'take', t, 'OK' if not problems else '; '.join(problems), flush=True)
        if best is None or score > best[0]:
            best = (score, t, problems)
            shutil.copy(v, J(OUT, key + '.mp4'))
        if not problems:
            break
    if best[1] != t:      # select the kept take again so that a clip that carries on starts from it
        m = f.call('project.get', {'id': c})['object']['media_ref']
        f.call('project.patch', {'patch': {'ops': [{'op': 'replace', 'path': f'{c}/media_ref/selected', 'value': list(m['takes'])[best[1] - 1] if isinstance(m['takes'], dict) else best[1] - 1}]}})
    REPORT[key] = {'clip': c, 'take': best[1], 'problems': best[2]}
    json.dump(REPORT, open(J(OUT, 'report.json'), 'w'), indent=1)
    return c


REPORT = {}
if __name__ == '__main__':
    t0 = time.time()
    f = setup()
    # scene 1: the apple (a chain: one framing, the lines run through)
    a1 = make(f, 'a1', [M, E], TWO, '<Subject 1> looks through the telescope; <Subject 2> reads a book; a red apple falls from above and lands on the table in front of <Subject 1> with a thud; she jumps and looks up',
              [(1, 'Grandpa! The tree is throwing apples at me.', 'startled, bright'), (2, 'The tree did nothing. The Earth did that.', 'warm, slow, amused')], SET, 9, 2001)
    a2 = make(f, 'a2', [M, E], TWO, '<Subject 1> picks the apple up from the table and turns it in her hands; <Subject 2> closes his book and turns to her',
              [(1, 'The Earth? How?', 'curious'), (2, 'The Earth pulls on everything near it. The apple, you, me. That pull is called gravity.', 'warm, slow')], SET, 9, 2002, extra={'continue_from': a1})
    a3 = make(f, 'a3', [M, E], TWO, '<Subject 1> holds the apple out and lets it drop into the open hand of <Subject 2>; he catches it and smiles',
              [(1, 'So that is why everything falls down?', 'curious, bright'), (2, 'Yes. And down just means toward the middle of the Earth.', 'warm, slow')], SET, 9, 2003, extra={'continue_from': a2})
    # scene 4: the cloth. It starts on the last picture of the apple scene, so the place is the same one
    d1 = make(f, 'd1', [M, E], TWO.replace('one continuous steady', 'the shot begins exactly on the first frame given and carries on from it: a steady'),
              '<Subject 2> lifts a wide wooden hoop with a dark blue cloth stretched tight over it from under the table and lays it flat on the table between them; '
              'he puts a heavy steel ball in the middle of the cloth and the cloth dips under it',
              [(2, 'A man called Einstein saw it another way. Imagine space is like this cloth.', 'warm, slow'), (1, 'And the ball is the Earth?', 'curious')], SET, 9, 2011,
              extra={'start_from': a3, 'start_at': 8.9, 'start_crop': [0.0, 0.0, 1.0, 1.0]})
    d2 = make(f, 'd2', [M, E], TWO, '<Subject 1> rolls a small glass marble onto the cloth; it circles the heavy ball; she points at it, delighted',
              [(2, 'Or the Sun. Anything heavy bends the space around it.', 'warm, slow'), (1, 'Look! It goes round and round!', 'delighted')], SET, 9, 2012, extra={'continue_from': d1})
    d3 = make(f, 'd3', [M, E], TWO, '<Subject 1> turns to <Subject 2> and looks up at him; then she throws her arms around him and hugs him; he laughs',
              [(1, 'So I am pulling on you right now?', 'curious'), (2, 'A tiny bit. And I am pulling on you.', 'warm, amused'), (1, 'Then gravity is like a hug.', 'happy')], SET, 9, 2013, extra={'continue_from': d2})
    d4 = make(f, 'd4', [M, E], TWO, '<Subject 2> holds <Subject 1> with one arm; both look up at the sky above the camera',
              [(2, 'The whole universe, holding itself together.', 'quiet, warm')], SET, 7, 2014, extra={'continue_from': d3})
    # shots made on their own: they join nothing
    make(f, 'open', ['an old apple tree'], 'a slow wide shot from the far end of the garden, the camera drifts forward; no people in the picture', 'the apple tree, the table with the telescope and the lit lantern; leaves move in a light wind',
         [], SET, 5, 2020)
    make(f, 'throw', [M], 'a medium-wide shot at eye level, the camera does not move, she is seen whole', '<Subject 1> throws a red rubber ball straight up above her head; it rises, slows, stops and falls back; '
         'she catches it with both hands and speaks', [(1, 'It always comes back!', 'delighted')], SET, 7, 2021)
    make(f, 'c_earth', [V], 'a wide shot from space of the whole blue planet Earth turning slowly in the centre of the frame, stars behind', 'the Earth turns slowly' + VO,
         [(1, 'On the other side of the world, down points the other way.', 'warm, slow')], 'outer space', 5, 2031, sound='silence, a low hum')
    make(f, 'c_orbit', [V], 'a wide shot from space', 'the blue Earth turns slowly at the left of the frame; the small grey Moon moves slowly in a curve around it; stars behind' + VO,
         [(1, 'It moves sideways so fast that it keeps missing the Earth. Around and around. That is an orbit.', 'warm, slow')], 'outer space', 9, 2032, sound='silence, a low hum')
    make(f, 'c_astro', [V, 'an astronaut in a blue flight suit'], 'a medium shot inside a space station, a round window with the blue Earth outside',
         '<Subject 2> floats weightless in the middle of the module, turning slowly, and does not speak; a pen and a drop of water float beside her; the voice of <Subject 1> is heard from off screen, he is not in the picture',
         [(1, 'Astronauts float for the same reason. Gravity still holds them. They are falling around the Earth with their ship.', 'warm, slow')],
         'the inside of a space station, white walls, cables, equipment', 9, 2033, sound='the hum of fans')
    make(f, 'c_moonwalk', [V, 'an astronaut in a white space suit'], 'a wide shot on the grey surface of the Moon, black sky, the Earth small above the horizon',
         '<Subject 2> hops forward in long slow bounces, dust rising slowly at every step; the voice of <Subject 1> is heard from off screen, he is not in the picture',
         [(1, 'The Moon has less mass, so it pulls less. Up there you could jump over this table.', 'warm, slow, amused')], 'the surface of the Moon', 9, 2034, sound='silence, a low hum')
    make(f, 'c_tide', [V], 'a wide shot of a sea shore at night under a full moon, the camera does not move', 'waves roll slowly up a sandy beach under the full moon' + VO,
         [(1, 'And the Moon pulls back on us. It lifts the sea twice a day. We call that the tide.', 'warm, slow')], 'a sea shore at night', 9, 2035, sound='slow waves')
    make(f, 'c_cloth', [V], 'a close shot from above and to the side of a dark blue stretchy cloth pulled tight over a wooden hoop on a table, lit by a lantern; no faces',
         'a heavy steel ball sits in the middle of the cloth in a deep dip; a small glass marble rolls around it in circles' + VO,
         [(1, 'No rope pulls the marble. It simply follows the curve.', 'warm, slow')], SET, 7, 2036, sound='a marble rolling on cloth, crickets')
    make(f, 'c_planets', [V], 'a wide shot from space, seen from above at an angle', 'the bright Sun in the centre; several small planets move slowly around it on wide circles; stars behind' + VO,
         [(1, 'Planets circle the Sun the same way. The bigger the mass, the deeper the bend.', 'warm, slow')], 'outer space', 9, 2037, sound='silence, a low hum')
    make(f, 'c_hole', [V], 'a slow push in from space', 'a black hole: a black disc ringed by glowing orange gas that swirls around it, stars bent around its edge' + VO,
         [(1, 'A black hole bends space so deeply that not even light can climb out.', 'slow, quiet, full of wonder')], 'outer space', 7, 2038, sound='a deep low hum')
    make(f, 'c_stars', [V], 'a wide shot from space', 'a great cloud of glowing dust and gas slowly draws together into a bright young star' + VO,
         [(1, 'Gravity pulled dust together to make the stars, the Sun and the Earth. Without it, nothing would hold together.', 'warm, slow')], 'outer space', 9, 2039, sound='silence, a low hum')
    make(f, 'end', [M, E], 'a wide shot from behind them at eye level, the camera does not move: they sit side by side on the bench with their backs to the camera, small in the frame',
         '<Subject 1> leans her head on the shoulder of <Subject 2>; both look up at the full moon and the stars; nobody speaks', [], SET, 5, 2040)
    from PIL import Image
    rows = []
    for key in REPORT:
        p = J(OUT, key + '.mp4')
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', p, '-vf', "select='not(mod(n,28))',scale=220:-1,tile=8x1", '-fps_mode', 'passthrough', '-frames:v', '1', p[:-4] + '.png'], check=True)
        rows.append(Image.open(p[:-4] + '.png'))
    for part in range(0, len(rows), 9):
        grp = rows[part:part + 9]
        sh = Image.new('RGB', (max(i.width for i in grp), sum(i.height for i in grp)), 'black')
        y = 0
        for i in grp:
            sh.paste(i, (0, y)); y += i.height
        sh.save(J(W, f'gravity_sheet{part // 9 + 1}.png'))
    print('made in', int(time.time() - t0), 's')
