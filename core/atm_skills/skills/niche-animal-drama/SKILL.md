---
name: niche-animal-drama
description: Niche - a short emotional film whose characters are animals that act like people (stand, hold things, cry, hug) - photoreal fur, big readable eyes, no speech, a score that carries it. Learned on one wordless film; edit or delete any line.
---

> Read `niche-rules` first, then `film` (the cast, states, chains, the cut). This adds only what is special to animals as
> characters. Learned on one film of about 35 s (two animals, a night scene and a day scene): every line is a starting point.

KIND: film

# The animals

- **Breed words decide the face more than mood words.** "Chubby", "fluffy", "round face" and "Persian" gave flat-faced, grumpy
  faces whatever mood was written; "blinks slowly" gave a squint. A sweet face that can cry came from "a normal muzzle, huge
  wide-open round glossy eyes like a kitten, a sweet innocent face" and a breed with a normal muzzle (3 probe rounds, 2 seeds each,
  2 animals of one kind; for another kind of animal, probe its own breed words the same way).
- **Probe the cast first**: a 3 s portrait of each animal from the waist up, room around the head, 2 seeds; then the face crop
  (`film`, the cast). Cheap (about 25 s a take at draft size) and it decides the whole film.
- **An animal alone in a close-up stares into the camera**, as a person does. "It looks at <Subject 2>, never at the camera"
  only half works: keep the other animal at the edge of the frame when the look matters.
- Standing on hind legs and holding a thing in both paws works when the description says so ("it stands upright on its hind
  legs like a person and holds ... in both front paws"); say in every shot where the thing is (`film`).
- Tears, wounds and wet fur are states of the cast entry and stay in every shot of the scene (`film`, the cast); write crying
  as wet fur, not as "tears".

# Sound

- **The takes' own sound has the animals voicing in almost every take** (the animal's calls), also with "no animal makes a sound,
  mouths closed" in the prompt, and the owner heard it as noise. Use the picture only (`add_clip {with_audio: false}`); the score carries the
  film, the effects go on tracks of their own (`film`, the cut and the sound).
- `sfx.from_video` with the animal's own calls named in `negative` gave none in 17 of 17 results.
- A wordless film wants one score that builds into its turn (`film`, music). How loud and how many effects a viewer wants is not
  known yet (`film`, not known yet).

# Pace

- About a shot every 1.6-2 s; the turning point is a fast run of short shots (0.3-0.9 s) and a match cut.
- The emotional beat of one animal is one chain cut into a medium shot and punch-ins (`film`, how shots join).

# CHECKS

- Every shot of a scene shows the same state of each animal (tears, wounds, wet fur).
- No animal voice anywhere: `asr.transcribe` on the export hears none (noise words like "Thank you." do not count).
- The face is in the centre of every punch-in (a frame of the export).
