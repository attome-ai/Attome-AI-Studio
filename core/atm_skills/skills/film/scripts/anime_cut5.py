"""The anime cut again with the swordsman's shots that hold one slim katana (katana probe), the dagger dash, and a kick cut to its flash."""
import subprocess, os
J = os.path.join
R_ = r'C:\Users\Computia.me\Downloads\everything\attome-bench\results'
A, L, T, K, N12, M = (J(R_, x) for x in ('anime11', 'anime_lang', 'anime10', 'katana', 'anime12', 'music'))


def flash(p):
    r = subprocess.run(['ffmpeg', '-hide_banner', '-i', p, '-vf', "select='gte(scene,0)',metadata=print:file=-", '-an', '-f', 'null', '-'], capture_output=True).stdout.decode('utf8', 'replace')
    best, t, cur = -1, 1.0, 0.0
    for line in r.splitlines():
        if 'pts_time' in line:
            cur = float(line.split('pts_time:')[1].split()[0])
        elif 'scene_score' in line and cur > 0.6:
            v = float(line.split('=')[-1])
            if v > best:
                best, t = v, cur
    return t


kt = flash(J(N12, 'hit_kick.mp4'))
cut = [(J(A, 'o1_wide.mp4'), 0.45, 2.4, 1), (J(R_, 'fix6', 'o2_2.mp4'), 0.3, 2.75, 1), (J(A, 'o3_kaito.mp4'), 0.45, 2.5, 1)]
intro = sum(c[2] for c in cut)
src = {}
F5 = J(R_, 'fix5')
for k, d in [('a01_eyes_k', 1.5), ('a02_eyes_r', 1.5), ('a03_feet', 1.3)]:
    cut.append((J(L, k + '.mp4'), 0.45, d, 0))
cut.append((J(F5, 'dash1.mp4'), 0.45, 1.5, 0))        # note 1: her daggers held blades up
cut.append((J(L, 'a05_pov_blade.mp4'), 0.45, 1.5, 0))
cut.append((J(L, 'a07_flurry.mp4'), 0.45, 2.2, 0))
cut.append((J(L, 'a06_clash_low.mp4'), 0.45, 2.2, 0))
cut.append((J(F5, 'lock2.mp4'), 1.0, 2.0, 0))         # note 2: the sword's length written
cut.append((J(F5, 'kick1.mp4'), 0.7, 2.2, 0))         # note 3: the kick lands, and
cut.append((J(F5, 'slide1.mp4'), 0.0, 2.3, 0))        #         her slide carries on from it
for k, d, s in [('g01_eyes', 1.4, 0), ('g02_roar', 2.5, 1), ('g03_hammer_pov', 1.4, 0), ('g04_crater', 1.5, 0)]:
    cut.append((J(A, k + '.mp4'), 0.45, d, s))
cut.append((J(T, 'g3.mp4'), 0.2, 3.7, 0))             # note 4: one continuous shot: he throws the hammer off, jumps, kicks, the big man falls
YU = J(R_, 'yui')
bt = flash(J(YU, 'bowcut2.mp4'))
cut.append((J(A, 'y01_eyes.mp4'), 0.45, 2.3, 1))
cut.append((J(YU, 'volley2.mp4'), 0.45, 2.0, 0))
cut.append((J(A, 'y03_arrow.mp4'), 0.45, 1.2, 0))
cut.append((J(R_, 'deflect', 'side1.mp4'), 0.45, 2.2, 0))
cut.append((J(YU, 'rush2.mp4'), 0.45, 1.5, 0))
cut.append((J(YU, 'bowcut2.mp4'), max(0.0, bt - 1.0), 1.9, 0))
cut.append((J(YU, 'knees1.mp4'), 0.45, 2.2, 0))
end0 = sum(c[2] for c in cut)
cut += [(J(A, 'e1_sheath.mp4'), 0.45, 1.8, 0), (J(R_, 'fix6', 'e2_1.mp4'), 0.45, 2.5, 1), (J(A, 'e3_leave.mp4'), 0.45, 4.6, 1)]
args, fc, t = [], '', 0.0
for i, (p, a, d, spoken) in enumerate(cut):
    args += ['-i', p]
    lvl = 'loudnorm=I=-16:TP=-2:LRA=9' if spoken else 'loudnorm=I=-21:TP=-3:LRA=11'
    fc += (f'[{i}:v]trim=start={a}:duration={d},setpts=PTS-STARTPTS,scale=1920:1080:flags=lanczos,fps=24,setsar=1[v{i}];'
           f'[{i}:a]atrim=start={a}:duration={d},asetpts=PTS-STARTPTS,aresample=48000,aformat=channel_layouts=stereo,{lvl},afade=t=in:d=0.02,afade=t=out:st={d - 0.04:.2f}:d=0.04[a{i}];')
    t += d
n = len(cut)
fc += ''.join(f'[v{i}][a{i}]' for i in range(n)) + f'concat=n={n}:v=1:a=1[v][fx];[fx]asplit=2[fx1][key];'
args += ['-i', J(M, 'tension.wav'), '-i', J(M, 'fight_rock60.wav')]
ms = int(intro * 1000)
fight = end0 - intro + 1.5
fc += (f'[{n}:a]atrim=duration={intro + 0.3:.2f},loudnorm=I=-22:TP=-3,afade=t=in:d=0.5,afade=t=out:st={intro - 0.2:.2f}:d=0.5[m1];'
       f'[{n + 1}:a]atrim=start=1.0:duration={fight:.2f},asetpts=PTS-STARTPTS,loudnorm=I=-17:TP=-2:LRA=9,afade=t=in:d=0.05,afade=t=out:st={fight - 2.0:.2f}:d=2.0,adelay={ms}|{ms}[m2];'
       f'[m1][m2]amix=inputs=2:duration=longest:normalize=0[m];[m][key]sidechaincompress=threshold=0.08:ratio=3:attack=20:release=300[md];'
       f'[fx1][md]amix=inputs=2:duration=first:normalize=0,alimiter=limit=0.85:level=disabled[a]')
out = J(N12, 'three_against_one_anime_v5.mp4')
r = subprocess.run(['ffmpeg', '-v', 'error', '-y'] + args + ['-filter_complex', fc, '-map', '[v]', '-map', '[a]', '-c:v', 'libx264', '-crf', '17', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '192k', out],
                   capture_output=True, text=True)
print(r.stderr[-400:] if r.returncode else f'{t:.1f} s, {n} shots; kick flash at {kt:.2f}')
