"""What makes a fight read as anime is its shot language, not one side view: eyes in extreme close-up, speed-line backgrounds, low angles,
a blade coming at the lens, a held pose after the strike. Twelve short shots (2-3 s) of the Kaito / Rin duel, one take each, to see which
of these the model can draw; then cut fast, as anime cuts."""
import os, sys, shutil, subprocess, time
sys.path.insert(0, os.path.dirname(__file__))
from film import Film, BENCH
from anime_d1 import cast
J = os.path.join
OUT = J(BENCH, 'results', 'anime_lang')
os.makedirs(OUT, exist_ok=True)
LOOK = ('a shot from a hand-drawn 2D anime action series, cel shading, bold ink lines, dramatic shadows, saturated colour, fast fluid sakuga animation with smear frames, '
        'not photorealistic; no subtitles, no captions, no written text; no background music')
SPEED = 'the background is abstract anime speed lines in red and black, not a place'
K, R = '@Kaito', '@Rin'
SET = '{Temple.description}'
shots = [  # key, who, camera, action, seconds, setting
    ('a01_eyes_k', [K], 'an extreme close-up of his eyes only, widescreen strip', '<Subject 1> narrows his grey eyes; a glint runs across them; ' + SPEED, 2, 'abstract'),
    ('a02_eyes_r', [R], 'an extreme close-up of her golden eyes only', '<Subject 1> smirks, her eyes flash; ' + SPEED, 2, 'abstract'),
    ('a03_feet', [R], 'a low close-up of her boots on the stone', 'her feet dig in and she launches forward out of the frame in a burst of dust', 2, SET),
    ('a04_dash', [R], 'she rushes straight at the camera, a blur', '<Subject 1> dashes at the camera with both daggers; ' + SPEED, 2, 'abstract'),
    ('a05_pov_blade', [K], 'a low angle from below his guard', '<Subject 1> swings his katana down at the camera; the blade fills the frame; ' + SPEED, 2, 'abstract'),
    ('a06_clash_low', [K, R], 'a low angle between them, the camera circling fast', 'the katana of <Subject 1> and the daggers of <Subject 2> clash above the camera in a shower of sparks, a white impact frame', 3, SET),
    ('a07_flurry', [K, R], 'a tight shot of the blades only, hands and steel', 'a flurry of blows: blades blur, afterimages of steel, sparks at every clash; ' + SPEED, 3, 'abstract'),
    ('a08_lock_faces', [K, R], 'a tight close-up of both faces either side of the crossed blades', '<Subject 1> and <Subject 2> strain against each other, teeth clenched, sparks drifting between their faces', 3, SET),
    ('a09_kick', [K, R], 'a dynamic low angle, the camera whips with the motion', '<Subject 1> spins and kicks <Subject 2> in the stomach; she folds and flies backward; a black-and-white impact frame', 3, SET),
    ('a10_slide', [R], 'a wide low shot', '<Subject 1> skids backward across the stone on one knee, a trail of dust, and looks up, breathing hard', 3, SET),
    ('a11_slash_past', [K, R], 'a wide shot in silhouette against the sunset', '<Subject 1> and <Subject 2> rush past each other in one crossing slash and both freeze back to back, weapons extended', 3, SET),
    ('a12_hold', [K, R], 'the same pose held, the camera slowly pushing in', '<Subject 1> and <Subject 2> stand frozen back to back; then her daggers fall from her hands and she drops to her knees', 4, SET),
]
if __name__ == '__main__':
    t0 = time.time()
    f = Film(OUT, 'Lang')
    cast(f)
    clips = []
    for n, (key, who, cam, act, secs, setting) in enumerate(shots):
        place = 'no place: an abstract anime action background of speed lines' if setting == 'abstract' else setting
        clips.append((key, f.shot(key, who, cam, act, [], place, seconds=secs, seed=1200 + n, style=LOOK, sound='steel, wind, effort, impacts')))
    f.run()
    from PIL import Image
    rows = []
    for key, c in clips:
        p = J(OUT, key + '.mp4')
        shutil.copy(f.take(c), p)
        png = J(OUT, key + '.png')
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', p, '-vf', "select='not(mod(n,10))',scale=220:-1,tile=6x1", '-fps_mode', 'passthrough', '-frames:v', '1', png], check=True)
        rows.append(Image.open(png))
    w = max(i.width for i in rows)
    sh = Image.new('RGB', (w * 2 + 6, sum(i.height for i in rows[::2])), 'black')
    y = 0
    for a in range(0, len(rows), 2):
        sh.paste(rows[a], (0, y))
        if a + 1 < len(rows):
            sh.paste(rows[a + 1], (w + 6, y))
        y += rows[a].height
    sh.save(J(OUT, 'lang.png'))
    print('made in', int(time.time() - t0), 's')
