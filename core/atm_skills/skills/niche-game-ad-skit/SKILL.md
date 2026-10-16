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
- Or the stylised look (the owner liked both): "a polished high-end mobile game cinematic in a stylised 3D animated style:
  caricature proportions with realistic detailed textures, big comic expressions", same light and settings. It holds the cast
  more easily (no drift to fight), so leave out the "never a cartoon" words. Ask which look before generating.
- A held product vanishes when the action needs both hands (putting cream on the face: 2 of 2). Say where it goes and that it
  stays seen: "sets the jar down on the edge of the sink in front of her, where it stays in view in the lower part of the frame
  the whole time" (fixed 2 of 2).
- A reveal: write every person's face in it, the one revealed too ("her eyes pop wide, her jaw drops, both hands fly to her
  cheeks, then a huge open-mouthed grin"); "beaming" gave a frozen smile the owner called weird.
- Make 2 takes of each shot and let the owner pick (the ComfyUI output names); the owner's pick was often not the take the check chose.
- Cast every person who appears twice, side characters too. Two references each, from your own best frames: full body with
  empty hands, and the face; a small or busy reference loses the person.

SPEECH
- One line per shot, said at its start: the model speaks at once. A shot longer than its line fills itself with made-up words,
  so make the shot as short as the line (about 3 s), and put the action that follows into a shot that carries on, with nobody speaking.
- But a line squeezed into its shot is said in a rush, and 1.75x on top of that sounded robotic to the owner (3 s shots with
  lines of 6 to 9 words). Better: the shot 1.5 s longer, the tone "speaking slowly and clearly, drawing out every word, with
  pauses between the phrases", then start the clip just before the line and cut soon after it (owner preferred it, 1 ad).
- Write lines in normal case and put the emphasis in the tone ("overjoyed, every word pronounced clearly"). A word in capitals
  with "drawing out every word" came out bent ("GLOWING" heard as "glawing", 2 of 2 takes, owner's ear; speech recognition still
  wrote "glowing"). Retaken in lowercase with "every word pronounced clearly and naturally" and no drawl, the owner heard it
  right (fixed 1 of 1); a simpler line without the word is the fallback. Keep "drawing out every word" off lines with long vowels.
- A voice sample cut from a scream or a gasp gives a flat, unreal copy. For a voice the owner likes, make 2-3 short clips of the
  person saying a calm, chatty line ("like chatting with a friend"), one per seed, no voice sample, and let the owner choose (1 of 1).
- A line that answers an action (a reveal) goes in a shot that carries on from the action, not in the action's shot.
- Every speaking character has a voice sample in the cast. A shot that carries on from another keeps that shot's speaker's voice:
  a new speaker's first line starts with a cut, not a carry-on.

- The skit works for any product, not only games (a service, a job ad): the problem as a comic disaster, the product as the hero
  who fixes it in one tap, a happy twist, then the pitch and contact details on the end card. Clothes, rooms and lines in the
  culture and language of the audience (see `film` for lines in another language); the captions in that language too.
  Done once for a service in Gulf Arabic, everyone in local dress, photoreal: the owner liked it (1 of 1).

EDIT
- Every spoken shot sped up 1.75x with its pitch kept (slower reads as slow); the pitch shots as fast as their narration allows
  (1.3x kept a narrator clear, 1 film); the end card at 1x.
- Trim each line's shot soon after its last word; never mute a mouth that moves (cut the shot or remake it).
- Joins of shots that carry on: skip the repeated frames (see `film`); a line that starts inside them keeps them.

SOUND
- Voices loud: `audio.match {lufs: -12, limit: true}`; music (a playful, comic piece) about 7 dB under them, ducked
  only under the speech (a reaction shot with no line keeps the music up, or its sound seems to cut out).
- A reaction shot (a facepalm, a crowd laughing) is never wordless: with nobody told to speak its sound is thin and has gaps,
  heard as the sound cutting out, and laughter from `sfx.from_video` over it sounded fake (owner's verdict, 1 film). Give the
  reacting person a one-word sound as the shot's line ("Ugh!", "a long, exasperated groan"), the shot about 2 s, the laughter in
  the action and the sound: the model makes the groan in the cast voice and real laughter after it (1 of 1).
- A reaction can be its own shot (a cut to a close face) or the same moment going on, without a cut; choose per shot, or ask the
  owner. For the second: start it on the last frame used of the line's shot (`start_from`, `start_at`,
  `start_crop: [0, 0, 1, 1]`, "the shot begins exactly on the first frame given and carries on from it"), both people as
  subjects; not `continue_from`, which would give the reaction the speaker's voice. No jump at the join (1 of 1).
- A big reaction is written big, step by step: "throws her head back, eyes squeezed shut, mouth stretched wide open in a huge,
  over-the-top cartoonish groan, then slaps her palm over her face and drags it down". "Facepalms and groans" gave a small,
  flat reaction the owner called bad.
- The same for a reveal with people in it (the hero product appears, everyone stares): muted, it was heard as "no sound" even
  with the music up (owner, 1 film), and foley on it gave words. Give one person a one-word gasp as the shot's line
  ("Wow!" in the audience's language), face and body huge, seen from the front: the sound filled and the reveal read (1 of 1).
- Real sound effects on the actions, bigger than life: `sfx.from_video` on the shots where nobody speaks (a smash, a reveal, a ship,
  machines); on shots with talking it invents mumbling, so those keep their own sound. No synthesised tones over the actions.

TEXT
- A caption per line, whole line at once, bold italic, bright yellow with a dark outline, lower third.
- Item tags as holograms (translucent, glowing, with a level) above the item, popping in with the reveal; a backlit sign for the place.
- End: a big download button with the game's name, pulsing, and "download now" in the captions. For a service: the logo, the
  slogan, the contact details from its own site, and a pulsing "contact us" button.
- Screens in the picture (phones, chat bubbles, dashboards) come out with made-up letters: write "glowing dots only, no letters
  or words anywhere" and lay the real words over them as tags (fixed 1 of 1).
- Captions in a right-to-left script: `direction: rtl`, a heavy font that has the script (Segoe UI 900 for Arabic), digits in
  that script; a long narrator line split at its commas, one part after another.

CHECKS
- `asr.transcribe` on the export: every line once, in order; no echo (a line heard twice), no made-up words, no words inside effects.
- Frame-to-frame change across the film: the big jumps are the planned cuts only (a jump inside a scene is a bad take or a bad join).
- Contact sheet of each scene: the same people in the same places; the item of each tag visible under it.
- `audio.analyze`: about -13 LUFS overall, peak under -1 dB.
