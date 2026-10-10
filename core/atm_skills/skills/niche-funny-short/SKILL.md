---
name: niche-funny-short
description: The default niche for a YouTube Short - funny story, 30-40 s, fast cuts on the beat of the user's own music, word-by-word captions. Prefilled values that worked; edit or delete any line, or start from nothing.
---

> Read `niche-rules` first. Everything below is a starting point: the user may replace any line or delete the lot, and the
> agent follows whatever text the user leaves.

KIND: short

NICHE: Funny story short (default)

FORMAT
- 9:16, 1080x1920, 30 fps, 30 to 40 s.
- 9 scenes of about 3.5 s: hook, 6 numbered steps, a reality check, a finale.
- Hook in the first 3 s: a dumb claim and a number, said straight.
- Each step ends on a joke. The last scene is a twist and a question to the viewer.

SCRIPT VOICE
- Funny and dumb, deadpan, short sentences (6 to 10 words a scene).
- Never explain how to make money. No "make $X". The joke is the point.
- Call to action is a question the viewer wants to answer in the comments ("Would you quit? Be honest.").
- Title: the claim and the number. Hashtags: #shorts plus the topic, three at most.

LOOK
- {style}: bright, saturated, cinematic, fast action; {character}: one recurring character, described once and reused.
- The camera always moves (whip pan, orbit, tracking). No static shots, no talking head.
- Avoid: text inside the generated picture, plain portraits, slow scenes.

TEXT
- Captions word by word in the lower middle (y 0.7), pop style, big, money and number words in yellow.
- A short pop word on the funniest word of a scene (`$8`, `QUIT?`), about 0.85 s.
- A small label per scene at the top ("YEAR 5").

PACE AND SOUND
- Music: the user's own file, never generated music; start where the beat is clear, first beat on the first frame.
- A cut every 4 beats; each shot used twice, the second punched in (scale 1.2). A scale and rotation punch on every beat.
- A brightness flash at each cut, no blur.
- One quiet whoosh per cut with its peak on the cut (the file `sfx_whoosh.wav` peaks 0.5 s in, so start it 0.5 s early).
- Voice: Kokoro `am_michael`, speed 1.3, +3 dB; music about -5 dB and ducked 9 dB under the voice. Target about -16 LUFS.

CHECKS
- Contact sheet of the finished cut: a face or action in every scene, captions readable, nothing cut off at the top or bottom.
- Whoosh peaks sit on the cuts (compare the cut times with the whoosh clip starts plus 0.5 s).
- `audio.analyze` of the render: loudness near -16 LUFS, peak below 0 dB. Say that the sound was not heard.
- Duration is 30 to 40 s.
