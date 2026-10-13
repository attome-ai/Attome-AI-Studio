"""'Why things fall': the cut. Clips that carry on lose their first 22 frames (they repeat the clip before); one level for all speech;
one quiet score under everything, pulled down under the voices."""
import subprocess, os
J = os.path.join
W = r'C:\Users\Computia.me\Downloads\everything\attome-bench\results\gravity'
S = J(W, 'shots')
CARRY = 0.9167
# key, carries on from the clip before, spoken, extra video filter
cut = [('open', 0, 0, ''), ('a1', 0, 1, ''), ('a2', 1, 1, ''), ('a3', 1, 1, ''), ('c_earth', 0, 1, ''), ('s_mass_b', 0, 1, ''), ('throw_2', 0, 1, ''),
       ('s_why', 0, 1, ''), ('s_does_b', 0, 1, ''), ('orbit_1', 0, 1, ''), ('c_astro', 0, 1, ''), ('c_moonwalk', 0, 1, ''), ('s_go', 0, 1, ''),
       ('tide_2', 0, 1, ''),
       ('d1', 0, 1, ''), ('d2', 1, 1, ''), ('c_cloth2', 0, 1, ''), ('c_planets', 0, 1, ''), ('c_hole', 0, 1, ''), ('c_stars', 0, 1, ''),
       ('d3', 1, 1, ''), ('d4', 1, 1, ''), ('end', 0, 0, '')]


def dur(p):
    return float(subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', p], capture_output=True, text=True).stdout)


args, fc, t = [], '', 0.0
for i, (key, carry, spoken, vf) in enumerate(cut):
    p = J(S, key + '.mp4')
    a = CARRY if carry else 0.0
    if key == 'd3':
        a = CARRY          # d3 follows cutaways, but its first frames still repeat d2
    d = dur(p) - a - 0.02
    args += ['-i', p]
    lvl = 'loudnorm=I=-17:TP=-2:LRA=9' if spoken else 'loudnorm=I=-27:TP=-4:LRA=9'
    fc += (f'[{i}:v]trim=start={a}:duration={d:.3f},setpts=PTS-STARTPTS,{vf}scale=1920:1080:flags=lanczos,fps=24,setsar=1[v{i}];'
           f'[{0 if key == "end" else i}:a]atrim=start={a}:duration={d:.3f},asetpts=PTS-STARTPTS,aresample=48000,aformat=channel_layouts=stereo,highpass=f=70,{lvl},afade=t=in:d=0.04,afade=t=out:st={d - 0.06:.3f}:d=0.06[a{i}];')
    print(f'{t:6.1f}  {key}')
    t += d
n = len(cut)
fc += ''.join(f'[v{i}][a{i}]' for i in range(n)) + f'concat=n={n}:v=1:a=1[v0][fx];[v0]fade=t=in:d=0.8,fade=t=out:st={t - 1.2:.2f}:d=1.2[v];[fx]asplit=2[fx1][key];'
args += ['-i', J(W, 'music.wav')]
fc += (f'[{n}:a]atrim=duration={t:.2f},loudnorm=I=-25:TP=-3:LRA=7,afade=t=in:d=1.5,afade=t=out:st={t - 3.0:.2f}:d=3.0[m];'
       f'[m][key]sidechaincompress=threshold=0.05:ratio=4:attack=30:release=500[md];'
       f'[fx1][md]amix=inputs=2:duration=first:normalize=0,alimiter=limit=0.85:level=disabled[a]')
out = J(W, 'why_things_fall_v2.mp4')
r = subprocess.run(['ffmpeg', '-v', 'error', '-y'] + args + ['-filter_complex', fc, '-map', '[v]', '-map', '[a]', '-c:v', 'libx264', '-crf', '17', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '192k', out],
                   capture_output=True, text=True)
print(r.stderr[-600:] if r.returncode else f'{t:.1f} s, {n} shots')
