"""Usage: python words.py video.mp4 out.json [lang] - the words of a clip's sound as {text,start,end} (whisper turbo)."""
import json, struct, subprocess, sys
src, out = sys.argv[1], sys.argv[2]
lang = sys.argv[3] if len(sys.argv) > 3 else 'en'
pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', src, '-vn', '-ac', '1', '-ar', '16000', '-f', 'f32le', '-'], capture_output=True).stdout
W = r'C:\Users\Computia.me\Downloads\everything\attome-wt-h3pro\build\win-msvc-release\bin\attome-whisper.exe'
M = r'C:\Users\Computia.me\Downloads\everything\attome-bench\models\ggml-large-v3-turbo-q5_0.bin'
p = subprocess.run([W, '--model', M, '--language', lang], input=struct.pack('<I', len(pcm) // 4) + pcm, capture_output=True)
last = json.loads(p.stdout.decode('utf8', 'replace').splitlines()[-1])
words = [{'text': w['t'], 'start': round(w['s'], 2), 'end': round(w['e'], 2)} for w in last.get('words', [])]
json.dump(words, open(out, 'w', encoding='utf8'), ensure_ascii=False)
print(len(words), 'words')
