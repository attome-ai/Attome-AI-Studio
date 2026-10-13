"""Sound for the fight of four chains: hits that land ON the impact frames. An impact frame is a flash, so it is found in the picture
(a scene-change score above 0.5 between two frames); an impact is put on each one and a whoosh runs into it. Voices at one level,
the model's own fight sound under the effects, one bed of the place's air."""
import os, sys, json, subprocess
J = os.path.join
BENCH = r'C:\Users\Computia.me\Downloads\everything\attome-bench'
BIN = J(os.path.dirname(BENCH), 'attome-wt-h3pro', 'build', 'win-msvc-release', 'bin', 'attome.exe')
R = J(BENCH, 'results')
OUT = J(R, 'anime9')
tmp = J(OUT, 'sound')
os.makedirs(tmp, exist_ok=True)
order = [J(R, 'anime8', f'c0{i}.mp4') for i in (1, 2, 3, 4)] + [J(OUT, k + '.mp4') for k in ('g1', 'g2', 'g3', 'z1', 'z2', 'z3', 'f1', 'f2b')]
spoken = {'c01', 'g2', 'z1', 'f1', 'f2b'}
quiet = {'c01', 'f1', 'f2b'}


def dur(p):
    return float(subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', p], capture_output=True, text=True).stdout)


def flashes(p):
    r = subprocess.run(['ffmpeg', '-hide_banner', '-i', p, '-vf', "select='gte(scene,0)',metadata=print:file=-", '-an', '-f', 'null', '-'], capture_output=True).stdout.decode('utf8', 'replace')
    out, cur = [], 0.0
    for line in r.splitlines():
        if 'pts_time' in line:
            cur = float(line.split('pts_time:')[1].split()[0])
        elif 'scene_score' in line and float(line.split('=')[-1]) > 0.5 and cur > 0.2:
            if not out or cur - out[-1] > 0.6:      # a flash is two changes (in and out): one hit
                out.append(cur)
    return out


def sfx(kind, seed):
    path = J(tmp, f'{kind}_{seed}.wav')
    if not os.path.exists(path):
        f = J(tmp, 'sfx.json')
        json.dump({'kind': kind, 'output': path, 'seed': seed}, open(f, 'w'))
        subprocess.run([BIN, '--daemon', 'never', '--json', 'call', 'sfx.make', f], capture_output=True, env=dict(os.environ, ATTOME_ENDPOINT=r'\\.\pipe\attome-fightsound2'))
    return path


args, fc, mix, t, n, hits = [], '', [], 0.0, 0, 0
vl = []
for p in order:
    key = os.path.basename(p)[:-4]
    d = dur(p)
    args += ['-i', p]
    i = n
    n += 1
    fc += f'[{i}:v]scale=1920:1080:flags=lanczos,fps=24,setsar=1[v{i}];'
    vl.append(f'[v{i}]')
    ms = int(t * 1000)
    level = 'loudnorm=I=-17:TP=-2:LRA=9' if key in spoken else 'loudnorm=I=-22:TP=-3:LRA=11'
    fc += f'[{i}:a]aresample=48000,aformat=channel_layouts=stereo,highpass=f=70,{level},afade=t=in:d=0.03,afade=t=out:st={max(0.0, d - 0.05):.3f}:d=0.05,adelay={ms}|{ms}[s{i}];'
    mix.append(f'[s{i}]')
    if key not in quiet:
        for h in flashes(p):
            hits += 1
            for kind, at, vol in (('whoosh', t + h - 0.45, '-9dB'), ('impact', t + h, '-3dB')):
                w = sfx(kind, 10 + hits)
                args += ['-i', w]
                a = int(max(0.0, at) * 1000)
                fc += f'[{n}:a]aresample=48000,aformat=channel_layouts=stereo,volume={vol},adelay={a}|{a}[f{n}];'
                mix.append(f'[f{n}]')
                n += 1
            print(f'{key}: a hit at {h:.2f} s')
    t += d
fc += ''.join(vl) + f'concat=n={len(vl)}:v=1:a=0[v];'
args += ['-i', J(R, 'anime4', 'bed.wav')]
fc += (f'[{n}:a]aresample=48000,aformat=channel_layouts=stereo,atrim=start=0.3,asetpts=PTS-STARTPTS,asplit=2[bf][br0];[br0]areverse[br];[bf][br]concat=n=2:v=0:a=1[pair];'
       f'[pair]aloop=loop=8:size=2000000000,atrim=0:{t:.3f},loudnorm=I=-31:TP=-8:LRA=5[bed];')
mix.append('[bed]')
fc += ''.join(mix) + f'amix=inputs={len(mix)}:duration=longest:normalize=0,atrim=0:{t:.3f},volume=-3dB,alimiter=limit=0.8:level=disabled[a]'
out = J(OUT, 'three_against_one_v12_sound.mp4')
r = subprocess.run(['ffmpeg', '-v', 'error', '-y'] + args + ['-filter_complex', fc, '-map', '[v]', '-map', '[a]', '-c:v', 'libx264', '-crf', '17', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '192k', out],
                   capture_output=True, text=True)
print(r.stderr[-500:] if r.returncode else f'{t:.1f} s, {hits} hits')
