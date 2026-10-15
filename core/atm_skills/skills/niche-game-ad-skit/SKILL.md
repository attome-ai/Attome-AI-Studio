---
name: niche-game-ad-skit
description: Niche for a mobile-game advert told as a fast comic skit, then a short gameplay pitch and a download button - photoreal glossy game-trailer CGI, exaggerated acting, sped up 1.75x, level tags on every reveal, real sound effects on the actions. Learned on a made ad; edit or delete any line.
---

> Read `niche-rules` first, then `film` (people that stay the same, shots that carry on) and `photoreal-people`. Everything below is
> a starting point: the user may replace any line or delete the lot.

KIND: film

NICHE: Game ad skit (photoreal, fast)

FORMAT
- 16:9 (or 9:16 for Shorts), 45 to 60 s: a skit of 25 to 30 s, then a pitch of 20 to 25 s, then a download button.
- Skit: a hook line from the first frame (never a silent opening), a reward reveal, a loser who is mocked, a comeback, a twist.
- Pitch: one narrator over a picture that grows step by step (one step per shot), each step a short line; the last shot holds the
  button and "download now" in the captions.
- Every reveal gets an item tag with its level (`LV 200 Golden laser`); a sign names the place in the shots set there.

STORY LOGIC
- Show every cause before its effect: a reward must be seen being made or won before anyone reacts to it, and a request
  ("make one for me") must lead to a visible result. A viewer who has to guess why something happened stops watching.
- One fixed staging for a recurring place, written into every shot of it: who stands where (the host behind the counter on one side,
  customers coming in from the other), the same camera height. Without it the model puts people in each other's places.

LOOK
- Photoreal, glossy, hyper-detailed game-trailer CGI; warm golden light with a teal fill, evenly lit; MiniMax H3 with
  `speed_lora: turbo_v4`, `sampler: euler`, `scheduler: beta`, no `realism_lora` (it fights the glossy look).
- Big, comic faces and bodies (shock, rage, flirting, laughing); every person "a real photographed human, never a cartoon":
  without those words close shots and solo shots drift to a cartoon look.
- Cast every person who appears twice, side characters too. Two references each, from your own best frames: full body with
  empty hands, and the face; a small or busy reference loses the person.

SPEECH
- One line per shot, said at its start: the model speaks at once. A shot longer than its line fills itself with made-up words,
  so make the shot as short as the line (about 3 s), and put the action that follows into a shot that carries on, with nobody speaking.
- A line that answers an action (a reveal) goes in a shot that carries on from the action, not in the action's shot.
- Every speaking character has a voice sample in the cast. A shot that carries on from another keeps that shot's speaker's voice:
  a new speaker's first line starts with a cut, not a carry-on.

EDIT
- Every spoken shot sped up 1.75x with its pitch kept (slower reads as slow); the pitch shots as fast as their narration allows.
- Trim each line's shot soon after its last word; never mute a mouth that moves (cut the shot or remake it).
- Joins of shots that carry on: skip the repeated frames (see `film`); a line that starts inside them keeps them.

SOUND
- Voices loud: `audio.match {lufs: -12, limit: true}`; music (a playful, comic piece) about 7 dB under them, ducked 5 dB.
- Real sound effects on the actions, bigger than life: `sfx.from_video` on the shots where nobody speaks (a smash, a reveal, a ship,
  machines); on shots with talking it invents mumbling, so those keep their own sound. No synthesised tones over the actions.

TEXT
- A caption per line, whole line at once, bold italic, bright yellow with a dark outline, lower third.
- Item tags as holograms (translucent, glowing, with a level) above the item, popping in with the reveal; a backlit sign for the place.
- End: a big download button with the game's name, pulsing, and "download now" in the captions.

CHECKS
- `asr.transcribe` on the export: every line once, in order; no echo (a line heard twice), no made-up words, no words inside effects.
- Frame-to-frame change across the film: the big jumps are the planned cuts only (a jump inside a scene is a bad take or a bad join).
- Contact sheet of each scene: the same people in the same places; the item of each tag visible under it.
- `audio.analyze`: about -13 LUFS overall, peak under -1 dB.
