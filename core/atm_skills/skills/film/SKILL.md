---
name: film
description: Make a film with Attome - any story with characters (a scene, a short film, an episode, an ad with people) where faces, voices, objects and places must stay the same from shot to shot. START HERE. General rules learned from experiments, the order of the Tools, and the checks every take must pass. For action and fights also read niche-anime-action.
---

# Film with Attome

The goal: a person describes the characters and the idea; an agent makes the film. These are **general rules**: each one is
about how the model behaves, not about one video. They were found by experiment and are written as: the rule, why (what the
model does otherwise), and the words that work. Nothing here is about one video: the films the rules were found on are not part of this skill.

A kind of video with rules of its own is a **niche** (a skill named `niche-...`, see `niche-rules`): for action and fights
read `niche-anime-action` after this; for a film that explains something, `niche-explainer-film`. For real-looking people
(live action, ads, UGC) also read `photoreal-people`.

## Keeping what is learned (part of the work, every time)

1. **Before a film:** read this skill whole, and the niche's. Each rule says how sure it is (how many shots, how many films).
2. **Before building a scene on an idea that is not a rule here: probe it.** A few 2-3 s shots, one take each, a few
   minutes; look; then build. A whole scene built on a guess costs ten times more and teaches less.
3. **After a person judges a film:** turn what they said into a rule **in general terms**: not "this character's sword
   changed" but "what a person carries changes from shot to shot unless ...". Ask of every lesson: *what would another
   video of this kind need to know?* Put a rule that holds for any film here, one that holds for a kind of video in
   its niche skill (`skill.save` for a niche of the project; the built-in ones are in the engine's sources). Say how sure
   it is. **Never write a video's own story, names, lines or subject into a skill**: only what the next video can use.
4. A rule that turns out wrong is rewritten, not left beside its correction.
5. Notes with a time in the film ("at 0:23 the kick does not lead to her pose") are the best evidence: find the shot, find
   the cause, fix that shot, and write the cause as a rule.

One file comes with this skill: `scripts/attome_film.py`, a driver of a hundred lines over the Tools (a shot made and
checked, the cut, the score, the export), with no path of any machine and nothing of any one film in it.

## Setting up (once for a film)

Everything is a Tool of the engine; nothing here needs a program of your own beyond calling them.

- The engine: `attome` (`attome tools` lists the Tools with their parameters). Call one with
  `attome --daemon never --json call <tool> -` and the parameters as JSON on standard input; every Tool takes `project`
  (the path of the `.attome` folder).
- The generator (ComfyUI) and the models: `comfyui.status` says whether ComfyUI answers and has the nodes the engine uses;
  `models.list` says which models are on the computer. What is missing the engine fetches itself: `comfyui.install`
  (ComfyUI with its node packs, 2 GB, then started), `comfyui.start`, `models.fetch {id}` (20-47 GB each).
  **Ask the owner before any of these downloads, with the sizes, and wait for a clear yes.** A ComfyUI or a models
  folder the owner already has: `gen.set_comfyui`, `models.set_folder` (or `ATTOME_COMFYUI`, `ATTOME_MODELS_DIR`).
- A project: `attome new <folder>/<Name>.attome --name <Name> --rate 24 --canvas 1920x1080`.
- A shot: `gen.dialogue_prompt` -> `gen.create_clip` (**`megapixels: 0.45`** for a draft; returns `clip`) ->
  `attome gen run <project> --clip <clip>` (`--new-take` for another take) -> **`gen.check_take {clip}`**.
- A take's file, to look at its frames: `project.get {id: <clip>}`, `object.media_ref.takes[selected].outputs.video.path`,
  relative to the project folder. Look at 5-8 frames of every take (ffmpeg, or `see.frames` once it is on the timeline).
- The film: `timeline.edit` (`move` the clips in order onto a new track, `delete_track` the old one), `audio.match`,
  `music.make`, `audio.duck`, `attome render <project> -o film.mp4`. See "The cut" below.
- `scripts/attome_film.py` does all of this (`Film.shot`, `Film.make`, `Film.cut`): get it with
  `skill.get {id: "film", file: "scripts/attome_film.py"}`, save it beside your work and import it.

## The order of work

1. `character.list` - use the cast that is there. Add who and what is missing with `character.create`.
2. Write each scene as shots, with the lines spoken in each. Decide for each shot how it joins its neighbours (below).
3. For each shot: `gen.dialogue_prompt` (subjects `{character: name}`) -> `gen.create_clip` (pass its `characters` and
   `voices`) -> `gen.run`.
4. **Check every take**: `gen.check_take`, and your own eyes on its frames (below). A take that fails is made again
   (`--new-take`); keep the best (`gen.select_take`).
5. The cut, the sound and the music (below); the export.

Model: `minimax-h3.singularity-ref2va.int8` (the only one that takes reference pictures), `steps: 8`. Work at draft size
(`megapixels: 0.45`) until the content is right: full size makes the picture sharper and changes nothing in what happens.

## What the model does by itself (why the rules exist)

- **It copies a reference picture whole**: its background, light, framing and pose, not only who is in it.
- **It invents again, in every shot made on its own**, everything that is not pinned: the room, the light, where people
  are, what they hold and how, what is on their back.
- **It draws one clear action in a shot** and drops or muddles a second.
- **It fills a clip to its end** with speech and motion, and it makes only some lengths.
- **A person alone in a frame looks into the camera.**
- **It enlarges and exaggerates** what it is told is important ("fills the frame"), and a pictured object.
- **A chain of clips drifts**: after about four clips that carry on, the picture darkens and the people shrink.

## The cast: who and what must stay the same

- **A person's reference is a tight crop of the whole face**: hairline to chin, ear to ear, almost no background. Give it
  as `{path, crop: [x, y, w, h], at: seconds}`; a frame of a take you like is the usual source. **Look at the crop.**
  - A whole frame as the reference: shots cut to a copy of that frame, wall and all.
  - A crop that cuts off part of the face, or keeps background at its edges: the shot lands in a place like that background.
  - Anything else in the crop comes along in every shot (a strap, a hilt behind the shoulder, a collar): crop it out.
  - **Film the portrait with room around the head** (from the waist up, empty wall above and beside). A face that fills
    the portrait cannot be cropped tight: the crop is the whole frame, and shots cut to a copy of it (2 of 3).
- **One face picture per person.** A second angle made no visible difference. A "reference sheet" made by generating
  the person on a plain background does not work (the old background is copied into it).
- **A voice sample** is 3-5 s of that person speaking alone. Each speaker of a shot gets their own (`voices`); at most
  **three voices and nine pictures** in one shot. Four people can be in a shot; three can speak in it.
- **What a person carries or wears is written into their description, exactly and with its limits**: what it is, its
  size against the body ("a blade as long as his arm"), which hand, which way it points, and what they do NOT have ("he
  carries no other ...", "nothing on her back"). Without the limits the thing changes shape, multiplies, or moves.
  - **In every shot say where the thing is**: "she holds it up beside her cheek in her right hand"; "they lie on the
    ground in front of her, out of her hands". In a close-up the model otherwise puts it on the back or drops it.
  - Ask the owner how a thing is held before writing it: a grip written by guess was the wrong one.
  - Do not write that a thing "fills the frame": it is drawn huge. Write that it "crosses the frame".
  - A size written against the body stops a thing growing wildly; the exact size is not obeyed. When the model draws a
    thing the same way in most shots, change the description to what it draws: the same in every shot matters more
    than the size you had in mind.
- **A person whose state changes is a second cast entry**: disarmed, wounded, in other clothes. Same face picture and
  voice, a description of the new state ("her hands are empty; she has no sword"). The description is stronger than the
  shot: with "she holds a sword" in it, a shot that says the sword is gone still shows it (0 of 3); with the second
  entry it is gone (2 of 2).
- **A shot of things only has no cast in it**: name the people as subjects and a person is drawn. Describe what is seen
  as a plain subject ("two pairs of hands, a staff and a sword") and write "no faces and no bodies in the picture".
- **An object as a `prop` with its own picture helps a small, distinct object and harms a long thin one.** A product, a
  pair of small blades, a mask: name it as a subject of its own and it matches its picture. A long thin thing (a sword,
  a staff) is enlarged into a broad one: describe it in words instead.
  - A prop's picture: a 2 s shot of the object alone, "a locked-off product shot, the whole object in the centre of the
    frame, seen from the side, on a plain pale grey surface; no people, no hands"; the frame at 1 s.
  - **A real product's photo** (a shop's packshot on white) works as the start of that: give it as the reference of a first
    prop entry, make the 2 s shot on grey from it, and crop the jar out of its frame at 1 s (a little margin, no wall) for the
    entry the film uses. The printed label then came out readable in every shot where the product was more than a tenth of
    the frame tall, held in a hand or standing (9 of 9, one film). Say in the description which way the label faces ("its label facing the camera") in every product shot.
  - **A product shot is close only when the setting is short.** With the room's full sentence as the setting, "a close
    product shot" gave a wide view of the room with the product small in it (3 of 3). A setting of one surface and a blur
    ("a pale stone window sill in warm low evening sunlight; behind it everything is a soft warm blur") and its size in the
    frame ("as tall as half of the frame") gave a close, warm product shot (3 of 3). "Fills the frame" still must not be written.
  - Three subjects in a shot at most (two people and one object): with four, one of the people is lost.
- **A voice from off screen is a cast entry with a voice sample and no picture** ("the voice of ..., heard from off
  screen, never seen"). In the shot: "nobody is in the picture; the voice of <Subject 1> is heard from off
  screen". The speaker was never drawn (10 of 10 cutaways). Someone else may be in the picture and stays silent if told so.
- **A place has no reference picture.** Named as a subject with a picture, shots open on that exact picture, empty, and
  then cut to the action. A place is words (next section) and a master (the section after).
- In a prompt a cast entry is `{Name}` (its name and look) or `{Name.attribute}`: the clip follows every change to it.

## The place and the time of day

- **One sentence, used word for word in every shot of the scene** (keep it in a `location` entry, write
  `{Name.description}` as the setting): what kind of place, the few things always visible, the light.
- **Say the time of day in it, and say what shows it** ("tall black windows, it is dark outside"). A clock time alone
  ("three in the morning") gave daylight.
- **Not longer.** A detailed description makes the place more alike between shots but the film worse: brighter and
  flatter, lights on that should be off, and shots that open on the empty place before anyone appears.
- **Whatever the sentence names is drawn in every shot that uses it**, even a macro: a building named in it stood behind a
  close-up of a leaf and of water (3 of 3), and the shot lost its closeness. Give close-ups of things (a leaf, a flower,
  water, skin, a product) a sentence of their own with nothing named in it: "a meadow in warm low evening sun; beyond the
  subject everything is a soft blur of green and gold light, no buildings" (close and soft, 3 of 3). The light words stay
  the same, so the shots still cut together.

## How shots join (the most important choice in a scene)

Shots made on their own share nothing. Decide for each shot which of these it is:

- **A master, then closer shots that start inside it.** Make one take of the scene's set-up at eye level (who is where,
  the camera in one place). Every closer shot of that scene starts on a part of a frame of the master:
  `gen.create_clip {start_from: <the master's clip>, start_at: <seconds>, start_crop: [x, y, w, h]}` (fractions of the
  frame, the part the shape of the canvas, around the person). Camera: "the camera does not move; the shot begins exactly
  on the first frame given and carries on from it". The closer shot then has the master's place and light.
  - **The master must be filmed from the angle the closer shots have.** A part of a view from above is left within half
    a second (0 of 16 held). Eye-level masters held (7 of 8).
  - About one closer shot in four leaves its place after a second: measure it (drift, below) and retake.
  - **The part a closer shot starts on holds one person and at most a sliver of the other** (a shoulder at the edge).
    With half of the other's head in it the shot left its place (4 of 4); with a sliver it held (4 of 4).
- **A clip that carries on (`continue_from`)** where one motion runs through the join: someone walks in and sits, a long
  exchange in one framing, **an action and its result**. The place, the positions and the faces hold.
  - **At most four in a chain**; then start a new one.
  - A person who was not in the chain cannot enter it (drawn as a dark figure). A person CAN enter a shot that starts on
    a still frame ("<Subject 2> comes in from the right edge of the frame").
  - When checking such a clip leave out its first 22 frames (0.92 s): they repeat the clip before, its last word too
    (heard as an "extra word").
  - **Who leaves the frame is gone**: the next clip of the chain does not bring them back. Write the end of each clip so
    that everyone needed next is still in the picture ("drops to one knee in front of her", not "falls").
- **The next scene in the same place starts on the last frame of the scene before** (`start_from` that clip,
  `start_at` its last second, `start_crop: [0, 0, 1, 1]`): the place is the same one, and a new object can be brought
  into it ("he lifts ... from under the table and lays it on the table").
- **Several sequences in one exact place: a stage.** One shot with the constant person at one side and the rest of the
  frame empty; the first clip of every sequence starts on the same frame of it (`start_crop: [0, 0, 1, 1]`) and the
  other person comes in from the empty side.
- **A shot made on its own** only for what does not have to join anything: an eye, a hand, an object, an empty place,
  a reaction. It opens on a posed frame, 0.45 s as a rule and up to 0.9 s: cut the shot in after it (the largest
  scene-change score in the first second shows where).
- **Cause and effect are never two shots made on their own.** A blow and where the struck person ends up, a jump and
  where it lands, a throw and the catch: the poses and the places do not join and the viewer sees it. One shot holds
  both, or the result carries on from the cause.

## The shot

- One shot = one `gen.create_clip`, `continuous: true`. **One action in it.**
- Say **where the camera is and how wide the frame is**: "a medium close-up at eye level, filmed from across the table on
  a long lens, head and shoulders, the whole head in frame". "A close-up on his face" gives a selfie angle or a cropped head.
- **A person who talks to someone keeps that someone at the edge of the frame** (a dirty single): a wider part of the
  master (about 0.6 of the frame), both named as subjects, "... with the head and shoulder of <Subject 2> at the lower
  right edge of the frame, out of focus; <Subject 1> looks at <Subject 2> and speaks to her; <Subject 2> does not
  speak". Alone in a tight close-up the speaker looks into the camera, and "look off to the right" only half works.
- Name people by `<Subject N>` in the action, and say what each **does** from the first second.
- Lines: short; at most three in a 9 s shot; a tone for each. Ask for a second more than the line needs: a line that
  just fits ends on the cut.
- **Do not shorten a generated clip.** `gen.create_clip` gives the clip the length the model really makes (9 s asked is
  9.42 s) and says so in `note`. The model paces speech to the end of the take: a shorter clip cuts the last word, and
  "finish the lines early" in the prompt does nothing.
- A big change of state (people going down, a room emptying) does not happen in a shot that starts before it: make the
  reaction, then a clip that carries on in which it happens.
- `music: 'none'` in every shot: the model's music changes from clip to clip.

## The cut, the sound and the music

- **The cut**: one `timeline.edit` with `add_track {id: "$new:film", kind: video, position: top}`, then for each clip of
  the film in order `move {clip, to: "<seconds>s", track: "$new:film"}` (the time is the sum of the lengths before it:
  `project.get` -> `timing.duration`), then `delete_track` of the track the clips were made on (the takes not used go
  with it). A clip that carries on already skips the 22 frames it repeats: do not trim it. If you set its `source_in` yourself (to trim or
  re-time it), set it to those 22 frames (at 24 fps, 0.917 s), never 0: from 0 the end of the shot before plays twice. When its
  first line starts inside those 22 frames (the model often speaks at once, over the last words of the shot before), keep from
  just before the word and end the shot before as many frames earlier (the join stays one take), and give the earlier shot's
  sound its last words on a sound track of its own, the next shot's sound that much later (`video.extract_audio` of each take).
- Changing a cast entry (`character.update`) marks every clip that shows it out of date; `gen.run` on the whole project, or a
  shot that carries on from one of them, then makes those clips again and the film changes under you. Make clips one by one
  (`gen.run --clip`), and look at a clip's newest take before you cut it.
- **One level for all speech**: `audio.match {clips: [the spoken clips], lufs: -17}`; the shots with only the place's
  air: `lufs: -30`. Generated takes come out at different levels. A take with a shout or a hit in it (a hammer, a
  crash) is only raised as far as that instant allows and stays quiet: add `limit: true` and the voice reaches the level while a
  limiter holds the peaks. A fast ad wants its voices loud: about -12 LUFS each, the mix about -13.
- **Music**: `music.make {project, tags, seconds, bpm, key}` (genre, instruments and mood in words; about 10 s for a
  minute; it needs ACE-Step 1.5 in the owner's ComfyUI and says so when it is missing). Put it on the timeline with
  `add_clip {asset, at: "0s", duration, gain_db: -25 - its lufs, fade_in, fade_out}`, then `audio.duck {clip: <music>,
  over: [<film track>], db: 8}`. A piece stays at one level from start to end: for a scene that changes make one piece
  for each part (tension, action, after) and cut between them on the picture.
- A shot that should be silent but murmurs: retake it, or set its `audio.gain_db` to -60 and let the music carry it.
- **The export**: `attome render <project> -o film.mp4` (a 3-minute film: seconds). Then check the film itself:
  `audio.analyze {path}` (peak under -1 dB), frames (`see.contact_sheet`), and hear it in parts of a minute
  (`asr.transcribe {path, from, duration: 60}`): every line there, in order, nothing extra.
- Under a fight: an impact (`sfx.make`) on every flash (see `niche-anime-action`).
- **Sound effects follow the actions**, not the cuts: each thing that happens on screen (a smash, a reveal, a laugh, a failure, an
  exit) gets a real recorded sound that fits it and exaggerates it, on the frame where it lands. `sfx.search` and `sfx.fetch` get them
  (Freesound, CC0); `short-video-music-and-sfx` says how. Whooshes on every cut alone sound random.

## The checks every take must pass

**`gen.check_take {project, clip}`** measures the rows marked (T) and answers `{ok, problems: [{check, message, hint}]}`:
call it after every `gen.run`, with `hold: true` for a shot told to keep its place and `cut_limit: 0.7` for fast action.
It was calibrated on 22 takes of two films. The other rows are yours: the Tool cannot see what a shot is *for*.
**Judge a take first for what the shot is for** (is the action there?), then for the
thing you made it again to fix: a take with the right detail and no action made a film worse.

| Check | How | Fails when |
|---|---|---|
| The shot shows what it is for | look at 5-8 frames spread over the take | the action is missing, half done, or done by the wrong person |
| (T) No cut inside the shot | the largest scene-change score between two frames (ffmpeg `select='gte(scene,0)',metadata=print`) | above 0.15 for a calm shot (clean: under 0.07; a cut to a reference picture: 0.3-1.0). Niches with flashes set their own line |
| (T) The line is over before the cut | the level of the sound in the last 0.15 s of the clip | above -34 dB (speech about -20, a quiet place about -45). Not the word times of `asr.transcribe`: the last word is stretched to the end of the file |
| (T) A shot told to hold its place does | 1 - the correlation of a 48 x 27 grey copy of the first frame with each later frame; the largest | above 0.85 (held: 0.06-0.71; left: 1.05; one bad example, a thin margin). Not for a moving camera |
| (T) The words are the script | `asr.transcribe`, word by word against the lines | less than 90 % alike, or extra words (a sigh, a name said twice) |
| (T) A shot without lines is silent | `asr.transcribe` on it too | words are heard: people who are told nothing murmur (1 of 3). Retake with "their mouths are closed, nobody speaks, no voice is heard", or give the shot the sound of an empty shot of the place |
| Nothing foreign in the picture | look at the corners of the frames | a broadcaster's mark, a caption, letters (1 of 10 cutaways): retake with "no logo, no watermark and no letters anywhere in the picture" (clean 2 of 2) |
| Continuity | compare with the shot before and after | a thing held differently, on the back, a different length; a person somewhere they could not have got to |

## How to experiment (so a lesson is a lesson)

- **Probe before you build** (above).
- **Change one thing at a time** against the last version that was good.
- **One run proves nothing**: judge a change on three shots or more, better on two seeds each.
- **A measure says where to look; a person's eye decides.** A place measured "more alike" in a film that looked worse.
- **Calibrate a check** on takes known good and known bad before trusting it.
- Do not fix in the prompt what is a fault of the engine, and do not turn a check off for a whole scene.
- Show a person only what you have looked at yourself, and say what you could not judge (sound, motion).

## Known to work (measured)

- A scene of a minute with two people in a place they had never been in: a four-part master and two dirty singles, every
  take passed on the first try, about 7 minutes at draft size. The same people were recognisable in a second scene.
- Four drawn characters, each recognisable in every shot, also four in one frame.
- A film of three minutes (two people at a table, 23 shots, 10 of them with a voice from off screen): 19 of 23 first
  takes passed, about 35 minutes at draft size.
- **The rules carry to a new video**: a second drawn fight with other people, place and weapons, made from this skill
  and `niche-anime-action` alone: 17 of 24 first takes usable, the rest fixed in one round of retakes.

- **A wordless product film of 30 s in the style of a brand spot** (18 shots of 1.4-2.6 s: nature close-ups, one person in
  a place, hands with the product, packshot; captions and an end card from `web-motion`, a score from `music.make`): 24
  takes in all, 12 of the 18 first takes kept; the 6 remade were the two rules above (3 each), and all 6 remakes held. About 15 minutes of generation at 0.9
  megapixels. Silent shots are cut freely: each starts after its posed first half second.

## Live-action people (photoreal skin and faces)

Read the skill `photoreal-people` for any shot of real-looking people: which add-ons and settings give real skin on H3, what
does not, and how to light and frame people so they read as filmed. Drawn, anime and stylised films do not need it.

## Not known yet

- A place *identical* (not just alike) between shots that do not start inside a master.
- Faces in wide shots where the face is small; more than four people.
- A theme of music that returns across scenes.
