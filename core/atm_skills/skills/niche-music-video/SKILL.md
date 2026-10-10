---
name: niche-music-video
description: The default niche for a music video - the song leads, cuts and effects sit on its beat, no voiceover, lyrics as captions if wanted. Prefilled values; edit or delete any line, or start from nothing.
---

> Read `niche-rules` first. Everything below is a starting point: the user may replace any line or delete the lot, and the
> agent follows whatever text the user leaves. Missing lines fall back to this text.

KIND: music video

NICHE: Music video (default)

FORMAT
- The song is the length: the whole song, or the part the user names (a chorus clip of 30 to 60 s for a Short, vertical).
- Shape: 16:9 for a normal music video, 9:16 for a Short; ask when it is not said.
- Scenes follow the song: intro, verse, build, chorus, drop, outro. One look per part, the chorus the biggest.
- No voiceover. The first beat of the picture is the first beat of the song (or the first strong beat the user names).

SCRIPT VOICE
- No narration. Words on screen are the lyrics, and only if the user gives them or `asr.transcribe` hears them clearly.
- Title card: the song name and the artist, short, once, in the first seconds or the last.

LOOK
- One look for the whole video, chosen from the song's mood (dark and slow: low key, blue and teal; fast and loud: saturated, high contrast).
- {style}: one sentence, reused in every prompt; {character}: the performer or the one figure the story follows, described once.
- Camera: moves with the music (push in on the build, whip on the drop). Avoid static wide shots longer than 2 beats.
- Avoid: text inside the generated picture, lip-sync claims (generated mouths do not match the song).

TEXT
- Lyrics: word by word or line by line in the lower third, big, one colour, no boxes. Off when the song is instrumental.
- Nothing else on screen unless the user asks.

PACE AND SOUND
- `audio.analyze` the song: its bpm and beat grid are the clock. Cut every 4 beats in verses, every 2 in the build, every beat on the drop.
- A zoom or brightness punch on the downbeat (keyframes on scale, a brightness flash of 0.1 to 0.2); a bigger one on the drop.
- Only the song: no whooshes unless asked, no ducking. Level it to about -14 LUFS.
- Start the song where the beat is clear (`first_beat`), with the music's first beat on the first frame.

CHECKS
- Contact sheet of the finished cut: the look is the same in every part, nothing cut off, the chorus is the strongest picture.
- Cuts land on beats: compare the cut times with `beats` from `audio.analyze` (within 2 frames).
- `audio.analyze` of the render: loudness near -14 LUFS, peak below 0 dB. Say that the sound was not heard.
- Duration matches the song part chosen.
