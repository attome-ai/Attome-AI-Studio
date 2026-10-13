---
name: film
description: Make a film with Attome - any story with characters (a scene, a short film, an episode, an ad with people) where faces, voices, objects and places must stay the same from shot to shot. START HERE. General rules learned from experiments, the order of the Tools, and the checks every take must pass. For action and fights also read niche-anime-action.
---

# Film with Attome

The goal: a person describes the characters and the idea; an agent makes the film. These are **general rules**: each one is
about how the model behaves, not about one video. They were found by experiment and are written as: the rule, why (what the
model does otherwise), and the words that work. The films they were found on are only evidence (`LAB.md`).

A kind of video with rules of its own is a **niche** (a skill named `niche-...`, see `niche-rules`): for action and fights
read `niche-anime-action` after this.

## Keeping what is learned (part of the work, every time)

1. **Before a film:** read this skill whole, and the niche's. Do not repeat an experiment `LAB.md` already has
   (`skill.get {id: "film", file: "LAB.md"}`): every experiment, what changed, what happened, the owner's verdict, the mistakes.
2. **Before building a scene on an idea that is not a rule here: probe it.** A few 2-3 s shots, one take each, a few
   minutes; look; then build. A whole scene built on a guess costs ten times more and teaches less.
3. **After a person judges a film:** write their words into `LAB.md` as a row. Then turn it into a rule **in general
   terms**: not "the swordsman's sword changed" but "what a person carries changes from shot to shot unless ...". Ask of
   every lesson: *what would another video of this kind need to know?* Put a rule that holds for any film here, one that
   holds for a kind of video in its niche skill. Say how sure it is (how many shots, how many films).
4. A rule that turns out wrong is rewritten, not left beside its correction.
5. Notes with a time in the film ("at 0:23 the kick does not lead to her pose") are the best evidence: find the shot, find
   the cause, fix that shot, and write the cause as a rule.

Files of this skill: `LAB.md` (the evidence) and `scripts/` (the recipes the experiments ran with: `film.py` drives the
engine, `newsroom.py` holds the checks, `words.py` hears a clip, `drift.py` measures whether a shot holds its place,
`music.py` makes a score). They hold paths of the machine they were written on: adapt the top of each.

## The order of work

1. `character.list` - use the cast that is there. Add who and what is missing with `character.create`.
2. Write each scene as shots, with the lines spoken in each. Decide for each shot how it joins its neighbours (below).
3. For each shot: `gen.dialogue_prompt` (subjects `{character: name}`) -> `gen.create_clip` (pass its `characters` and
   `voices`) -> `gen.run`.
4. **Check every take** (below). A take that fails is made again: `gen.run --clip <id> --new-take`. Keep the best.
5. Sound and music in the edit; `render.sequence`.

Model: `minimax-h3.singularity-ref2va.int8` (the only one that takes reference pictures), `steps: 8`. Work at draft size
(0.45 megapixels) until the content is right: full size makes the picture sharper and changes nothing in what happens.

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
- **An object as a `prop` with its own picture helps a small, distinct object and harms a long thin one.** A product, a
  pair of small blades, a mask: name it as a subject of its own and it matches its picture. A long thin thing (a sword,
  a staff) is enlarged into a broad one: describe it in words instead.
  - A prop's picture: a 2 s shot of the object alone, "a locked-off product shot, the whole object in the centre of the
    frame, seen from the side, on a plain pale grey surface; no people, no hands"; the frame at 1 s.
  - Three subjects in a shot at most (two people and one object): with four, one of the people is lost.
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
- **A clip that carries on (`continue_from`)** where one motion runs through the join: someone walks in and sits, a long
  exchange in one framing, **an action and its result**. The place, the positions and the faces hold.
  - **At most four in a chain**; then start a new one.
  - A person who was not in the chain cannot enter it (drawn as a dark figure). A person CAN enter a shot that starts on
    a still frame ("<Subject 2> comes in from the right edge of the frame").
  - When checking such a clip leave out its first 22 frames (0.92 s): they repeat the clip before.
- **Several sequences in one exact place: a stage.** One shot with the constant person at one side and the rest of the
  frame empty; the first clip of every sequence starts on the same frame of it (`start_crop: [0, 0, 1, 1]`) and the
  other person comes in from the empty side.
- **A shot made on its own** only for what does not have to join anything: an eye, a hand, an object, an empty place,
  a reaction. It opens on about 0.45 s of a posed frame: cut that off.
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
- Lines: short; at most three in a 9 s shot; a tone for each.
- **Do not shorten a generated clip.** `gen.create_clip` gives the clip the length the model really makes (9 s asked is
  9.42 s) and says so in `note`. The model paces speech to the end of the take: a shorter clip cuts the last word, and
  "finish the lines early" in the prompt does nothing.
- A big change of state (people going down, a room emptying) does not happen in a shot that starts before it: make the
  reaction, then a clip that carries on in which it happens.
- `music: 'none'` in every shot: the model's music changes from clip to clip.

## Sound and music

- One level for all speech; the model's other sound a few dB under it; short fades at the cuts.
- **One bed of the place's air under a whole scene**: a 12 s take of the empty place, played forward then backward (no
  loop is heard).
- **Music** (no Tool yet: ACE-Step 1.5 XL turbo through ComfyUI, `scripts/music.py`): about 10 s to make a minute. Tags
  (genre, instruments, mood), a BPM, a key, lyrics "[instrumental]". A track stays at one level from start to end: make
  one track for each part of a scene (tension, action, after) and cut between them on the picture. Pull the music down
  under speech (side-chain).

## The checks every take must pass

Do these after every `gen.run`. **Judge a take first for what the shot is for** (is the action there?), then for the
thing you made it again to fix: a take with the right detail and no action made a film worse.

| Check | How | Fails when |
|---|---|---|
| The shot shows what it is for | look at 5-8 frames spread over the take | the action is missing, half done, or done by the wrong person |
| No cut inside the shot | the largest scene-change score between two frames (ffmpeg `select='gte(scene,0)',metadata=print`) | above 0.15 for a calm shot (clean: under 0.07; a cut to a reference picture: 0.3-1.0). Niches with flashes set their own line |
| The line is over before the cut | the level of the sound in the last 0.15 s of the clip | above -34 dB (speech about -20, a quiet place about -45). Not the word times of `asr.transcribe`: the last word is stretched to the end of the file |
| A shot told to hold its place does | 1 - the correlation of a 48 x 27 grey copy of the first frame with each later frame; the largest | above 0.85 (held: 0.06-0.71; left: 1.05; one bad example, a thin margin). Not for a moving camera |
| The words are the script | `asr.transcribe`, word by word against the lines | less than 90 % alike, or extra words (a sigh, a name said twice) |
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

## Not known yet

- A place *identical* (not just alike) between shots that do not start inside a master.
- Faces in wide shots where the face is small; more than four people.
- A theme of music that returns across scenes.
