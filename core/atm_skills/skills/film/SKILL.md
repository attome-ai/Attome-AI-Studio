---
name: film
description: Make a film with Attome - scenes of people who speak, the same faces, voices and places from shot to shot. START HERE for any story with characters (a short film, a scene, an episode). Rules learned from experiments, the order of the Tools, and the checks every shot must pass.
---

# Film with Attome

The goal: a person describes the characters and the idea; an agent makes the film. This skill is the rules that have been
**measured** to work. Follow them as written; where a rule says "not known yet", do not guess: make two takes and look.

When you learn something new (a failure that repeats, a rule that fixes it), add it here with `skill.save` so the next film
starts from it.

## The order of work

1. `character.list` - use the cast that is there. Add who is missing with `character.create`.
2. Write the scene as shots: one continuous shot each, 4-10 s, with the lines spoken in it.
3. For each shot: `gen.dialogue_prompt` (subjects `{character: name}`) -> `gen.create_clip` (pass its `characters` and
   `voices`) -> `gen.run`.
4. **Check every take** (below). A take that fails is made again: `gen.run --clip <id> --new-take`. Keep the best.
5. `render.sequence`.

Model: `minimax-h3.singularity-ref2va.int8` (the only one that takes reference pictures), `steps: 8`.

## The cast

- **A person's reference picture is a tight crop of the whole face**: hairline to chin, ear to ear, almost no background.
  Give it as `{path, crop: [x, y, w, h], at: seconds}`; a frame of a take you like is the usual source.
  - A whole frame as the reference: the model **cuts to a copy of that frame** in the middle of a shot, wall and all.
  - A crop that cuts off the chin or keeps the corners of a wall: the shot lands in a room that looks like that wall.
- **One face picture per person.** A second angle (front + three-quarter) made the face *less* recognisable in the one film
  it was tried on. Not proven either way: do not add it without comparing.
- **Do not make a "reference sheet" by generating the person in a studio.** The model copies the old background into the new
  pictures. Crop the face instead.
- **A voice sample** is 3-5 s of that person speaking alone. Each speaker of a shot gets their own (`voices`); two people
  in one shot keep two voices.
- **A prop** (a product, an object) is the opposite of a person: give the whole picture, it must look exactly like it.
- **A place has no reference picture.** Named as a subject with a picture, shots open on that exact picture (the empty
  room) and then cut to the action. Describe the place in words (next section).
- In a prompt, a cast entry is `{Name}` (its name and look) or `{Name.attribute}`: the clip then follows every change to it.

## The place and the time of day

- One short description of the place, used word for word in every shot of the scene (keep it in a `location` entry and
  write `{Name.description}` as the setting): what kind of room, the few things that are always visible, the light.
- **Say the time of day in the description itself, and say what the windows look like** ("tall black windows, it is
  dark outside"). "Three in the morning" alone gave daylight.
- **Keep the description short: one sentence** ("a large open newsroom in the middle of the night, rows of dark desks, a
  few glowing monitors, rain on tall black windows, one warm desk lamp; it is dark outside"). This is the version the
  owner judged best.
- A longer description (where the windows are, the carpet, "the ceiling lights are off") makes the room more alike from
  shot to shot, but the film looked **worse**: brighter and flatter, daylight in a window, ceiling lights on, and shots
  that open on the empty room before the people appear. Likeness of the room is not worth the look.

## A scene: which shots are separate, which carry on

Decide shot by shot, as a film does. Most shots are separate (a cut); a few carry on.

- **Separate shots made on their own do not share a room.** Each one invents the room, the light and where people sit
  again: he is in the middle of the room, then she sits in the dark, then they are together somewhere else. Words do
  not fix this (a longer description of the place made the film look worse).
- **So give a scene a master first:** one wide or two-shot take of the scene's set-up (who sits where, the camera in
  one place). Then every closer shot of that scene **starts on a part of a frame of the master**:
  `gen.create_clip {start_from: <the master's clip>, start_at: <seconds into it>, start_crop: [x, y, w, h]}` (fractions of
  the frame, the part the same shape as the canvas, around the person). Camera: "a medium close-up, the camera does not
  move; the shot begins exactly on the first frame given and carries on from it". The close-up then has the master's
  windows, lamp and light behind the face.
  Measured on four close-ups in two films: three stayed in the master's room; **one left it after a second** (the
  camera pulled back into another room) and no check caught that: look at a late frame of every close-up, and retake.
- **Carry on (`continue_from`) only where one action runs through the cut** with the camera in one place: someone
  walks in and sits down, a long exchange in one two-shot. It works with the cast (four clips in a row kept the
  room, the seats and both faces), but a film made only of this has no cuts and no close-ups.
- When checking a clip that carries on, leave out its first 22 frames (0.92 s): they repeat the end of the clip
  before, and the listener hears that clip's last word again.

## The shot

- One shot = one `gen.create_clip`, `continuous: true`. Do not ask for cuts inside a clip.
- Say **where the camera is and how wide the frame is**: "a medium close-up at eye level, filmed from across the desk on
  a long lens, head and shoulders, the whole head in frame". "A close-up on his face" gives a selfie angle or a frame
  that crops the forehead.
- **A person who talks to someone keeps that someone at the edge of the frame** (a film calls it a dirty single). Take a
  wider part of the master, so the head or shoulder of the other person is still at the edge (`start_crop` about 0.6 of
  the frame, not 0.45), name both as subjects, and write: "a medium shot of <Subject 1> from the chest up, with the head
  and shoulder of <Subject 2> at the lower right edge of the frame, out of focus ... <Subject 1> looks down at
  <Subject 2> and speaks to her; his eyes stay on her; <Subject 2> does not speak". He then looks at her.
  - Alone in a tight close-up he looks into the camera. Telling him to look "at a woman out of frame to the right" made
    it better but not right (the owner: "better but not perfect", and the face changed).
  - The wider part is also enlarged less, so more of the real face is in the start picture and the face holds better.
- Name the people by `<Subject N>` in the action, and say what each one **does** from the first second, so the shot
  starts on them.
- Lines: short, one or two sentences a speaker; at most three lines in a 9 s shot. Give each a tone.
- **Do not shorten a generated clip to a round length.** The model paces the lines to the end of what it makes, so the last
  word ends 0.2-0.6 s before the end of the take. `gen.create_clip` gives the clip the length the model really makes (9 s asked
  is 9.42 s) and says so in `note`: leave it. Telling the model "finish the lines two seconds early" does nothing.
- `music: 'none'` and add music in the edit: the model's music changes from clip to clip.

## Action (a fight, a chase)

- **An anime fight is its shot language, cut fast.** A fight told only in side-view two-shots is clear but reads as a stage
  play (the owner: "still kind of bad, not like real anime"). Mix these, 1.3-2.5 s each on the timeline (ask for 2-3 s and
  cut the first 0.45 s off: a shot made on its own opens on a few frames of a posed portrait):
  - eyes only, in an extreme close-up, one fighter then the other;
  - feet digging in and launching; a dash straight at the camera; a blade swung down at the lens;
  - blades only, a flurry with afterimages; a clash seen from a low angle between them;
  - both faces either side of crossed blades; a kick with a black-and-white impact frame; a skid backward in dust;
  - two silhouettes crossing in one slash against the sky.
  For the close action shots give no place at all: "the background is abstract anime speed lines in red and black, not a
  place". Anime does this, and it removes the problem of the place changing between shots.
  **The moment one fighter strikes the other is taken from a side-view exchange** (below), cut to the second it lands:
  written as a close two-person shot ("he kicks him across the jaw", "his blade cuts her bow") the two just stand.
  The close shots carry everything around the hit: the eyes before, the weapon at the lens, the floor breaking, the
  one who was hit sliding or kneeling after.
  The side-view exchange below is one of these shots, not the whole fight. Not drawn well: a held pose with a delayed
  fall (two people just stand).
- **A fight is two fighters, whole in one frame, seen from the side, in one exchange.** "They trade fast blows, a burst of
  sparks at every clash, while they circle each other"; "their blades lock, they push face to face"; "she slashes at his
  head, he ducks under the blade and drives his shoulder into her". The model draws these well.
  - **Not one person in a shot.** Fighters shown one at a time never touch: they pose, and it is not a fight (the owner:
    "the scenes themselves are bad, the way they fight").
  - **Not several different actions, and not three attackers at once**, in a shot: the second half is dropped and the
    fighters scatter.
- **Write the look as fight animation:** "hand-drawn 2D anime fight animation of the highest quality (sakuga): fast fluid
  motion, smear frames, impact frames that flash white and black, speed lines, sparks, a dynamic camera that follows the
  action". The impact frames are wanted, so the cut check is off for these shots: look at them.
- **A duel is a chain of clips that carry on** (`continue_from`), 4-5 s each, one exchange in each: trade blows / the
  blades lock / the counter that ends it. It stays one fight in one place.
  - **At most four clips in a chain.** A chain of nine went dark from the fifth clip on and the fighters shrank to small
    dark figures. Start a new chain (a new duel) after three or four.
  - Keep the two close enough to fill the frame. Written "far apart" (an archer and a swordsman across the courtyard) both
    are tiny and the exchange is lost: bring the swordsman to her first, or show the arrows in a shot of him alone.
  - Use the plain anime look, not the fight wording, for the quiet shots after the fight: the fight wording puts an impact
    frame into them.
  - A new fighter is not brought into a clip that carries on (he is drawn as a dark silhouette): begin a new chain with him.
- **One exact place for several duels: a stage.** Make one stage shot first (plain look, 3 s): the hero alone, seen from
  the side at eye level, whole, in the left third of the frame, "the right half of the frame is empty courtyard". The
  first clip of EVERY duel starts on the same frame of it (`start_from` the stage, `start_crop: [0, 0, 1, 1]`) and says
  "<the opponent> comes in from the right edge of the frame and ..."; the duel then carries on for two or three clips.
  Three duels made this way have the same pillars, sky and torches (measured on two films).
- **Music** (`gen` has no Tool for it yet: ACE-Step 1.5 XL turbo through ComfyUI, attome-bench `scripts/music.py`): about
  10 s to make a minute. Give tags (genre, instruments, mood), a BPM and a key, lyrics "[instrumental]". The owner called an
  orchestral battle score "meh"; a track comes out at one level from start to end, it does not build by itself: make a
  track for each part of a scene and cut between them.
- **Sound of a fight:** an impact frame is a flash, so find them in the picture (a scene-change score above 0.5 between two
  frames, at most one every 0.6 s) and put an impact (`sfx.make`) on each, with a whoosh ending on it; keep the model's own
  fight sound about 5 dB under the voices; one bed of the place's air under everything (a 12 s take of the empty place,
  played forward then backward so no loop is heard).
- **The hit is made by the cut, not inside a shot.** The one who strikes, then what is struck, then the reaction: the
  hammer lifted / the stone floor cracking (no people in the picture) / him in the air above the dust. A shot of only the
  thing (the arrow flying, the wave of light crossing the courtyard) is said to have "no people in the picture".
- **One place for a fight: two eye-level masters that face each other** (one looks at the one, one at the others, each
  seen from the knees up; make both with one seed and at 0.9 megapixels). Every shot starts inside the master that
  faces its fighter (`start_from`, `start_crop` about half the frame around them, or the whole frame). The fight then has
  two backgrounds, as a filmed fight has. Without this every shot invents its own courtyard (the owner: "the scene is
  all over the place").
  - **Not a high wide of everyone.** A part of a view from above is a person on bare floor: the model leaves it within
    half a second for an eye-level view with a background of its own (0 of 16 shots held).
- A big change of state (three people going down) does not happen in a shot that starts on them standing. Make the
  reaction first (they throw their arms up and stagger), then a clip that **carries on** from it (`continue_from`) in
  which they fall and stay down.
- Shots of 3-4 seconds (3 s is the model's 73 frames). 21 such shots made a fight of 74 s in 13 minutes.
- Give each shot its own sound (a clash of steel, a stone impact, an arrow's whistle), not one ambience for the scene.
- A model step takes at most **3 voice samples and 9 reference pictures**: `gen.dialogue_prompt` gives a voice only to who
  has a line in the shot. Four people can be in one shot; at most three of them can speak in it with their own voices.
- Keep the cut check on, with its line at **0.7** for action (fast action measures 0.1-0.45; a cut to a reference picture
  0.9-1.0). A shot of someone being hit tends to open on them posing and then cut to the hit: it failed three takes in a row.

## The checks every take must pass

Do these after every `gen.run`; do not show a film whose shots were not checked.

| Check | How | Fails when |
|---|---|---|
| No cut inside the shot | the largest scene-change score between two frames of the take (ffmpeg `select='gte(scene,0)',metadata=print`) | above 0.15. Clean shots measure under 0.07; a cut to a reference picture measures 0.3-0.6 |
| The line is over before the cut | the level of the sound in the last 0.15 s of the clip | above -34 dB: a voice is still sounding (speech is about -20 dB, a quiet room about -45). Do not use the word times of `asr.transcribe` for this: the last word is always stretched to the end of the file |
| A shot that should hold its place does | 1 - the correlation of a small grey copy (48 x 27) of the first frame with each later frame; the largest value | above 0.85, for a shot whose camera is told not to move (a close-up started inside a master). Shots that held measure 0.06-0.71; the one that left for another room measured 1.05. **One bad example only: a thin margin.** Not for shots where the camera moves (a push-in measures 0.8, a tracking shot 1.2) |
| The words are the script | `asr.transcribe` on the clip, compared word by word with the lines | less than 90 % alike, or more words than the script (the model adds "I..." or repeats a name) |

Not caught by these, look at a frame of each take: a shot that starts on the empty room and drifts to the people; a face
that is a little off; daylight in a night scene.

## How to experiment (so a lesson is a lesson)

- **Change one thing at a time** against the last version that was good. Five changes at once hid the one that worked.
- **One run proves nothing.** The same reference and seed gave a clean shot in one film and a broken one in the next
  because other words of the prompt changed. Judge a change on at least three shots, better on two seeds each.
- Write down what was tried and what happened, also when it failed.

## Known to work (measured)

- A whole scene of a minute by these rules, in a place the people had never been in: a four-part master and two dirty
  singles, every take passed every check on the first try, about 7 minutes to make at 0.45 megapixels. The same two
  people were recognisable in a second scene with another place and light.

## Not known yet

- How to keep a room *identical* (not just alike) between shots without a reference picture.
- Whether two or three face angles help when a face is seen from the side.
- Faces at full size (0.9 megapixels) and in wide shots where the face is small.
- More than two people in a shot.
