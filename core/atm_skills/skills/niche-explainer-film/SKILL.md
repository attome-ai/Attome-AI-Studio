---
name: niche-explainer-film
description: Niche - a short film that explains one idea (science, history, how something works) through people talking and pictures of the thing itself. Read the skill film first; this adds what is special to explaining.
---

# Niche: a film that explains

KIND: film

One idea, 2-4 minutes, told by people: someone asks, someone who knows answers, and the thing itself is shown while they
talk. Read `film` first. This holds for any subject (found on one film of three minutes so far: treat it as a first version).

STRUCTURE
- **A question a child would ask opens it**, set off by something that happens in the picture (a thing falls, a light goes
  out). Each answer raises the next question. End on a line that says the idea in the asker's own words.
- **One fact a shot.** Write the facts yourself, then check each one before generating: the model draws and speaks what
  it is told and does not know what is true.
- Three kinds of shot, mixed so that no kind runs longer than about 30 s:
  - the two people in one framing (a chain, see `film`): the questions and the short answers;
  - closer shots of one of them (started inside the chain): a line that matters;
  - **the thing itself, with the explainer's voice from off screen** (see `film`: a voice from off screen): about half
    of the film.
- **A demonstration with things on a table** (a ball, a cloth, a glass of water) makes an idea visible and is drawn well:
  have one for the hardest idea. Start its scene on the last frame of the scene before so that the place is the same.

WORDS
- About 2 words a second: at most 20 words in a 9 s shot, 14 in 7 s, 9 in 5 s. Count them.
- The explainer: "warm, slow". The asker: short lines, a tone for each ("curious", "delighted").

PICTURES OF THE THING
- Say "the camera is locked off and does not move" for a close shot of a small thing: a camera that pulls back makes it
  grow to many times its size.
- Slow, simple motion of one thing is drawn: it falls, rises and comes back, rolls, turns, floats, flows. **Exact
  motion is not** (speeds, distances, a path that must close): let the words carry the exactness.
- A thing that moves along a path: film it from the side that shows the whole path (from above for a path on the
  ground or around something), camera locked off, and say the path as a picture a child knows ("all the way around,
  like the hand of a clock"), not as a term ("moves in a curve"): the term gave almost no motion, the picture gave it in
  3 of 3 takes.
- A demonstration by one of the people happens in the scene's own framing: start the shot on a frame of that scene.
  Made on its own it is in another corner of the place.
- No written text, labels or diagrams in the picture: they come out as nonsense letters. Titles belong in the edit.

SOUND
- All speech at one level (`audio.match`); one quiet score under the whole film (`music.make`: quiet instruments, a slow tempo, no drums), pulled down under the voices (`audio.duck`); no effects needed.

CHECKS (besides those of `film`)
- Hear the whole finished film (`asr.transcribe`, in parts of a minute: a longer piece made the listener repeat a
  sentence over and over) and read it as a text: does it explain the idea in order, is
  every fact right, is nothing said twice?
- The length: the lines add up before generating (the words / 2 = seconds of speech; add a quarter for pauses).


SECOND FILM (3 minutes, 20 shots; every shot passed `gen.check_take`, one still needed a retake for what it showed; first versions of rules)
- **A scene of the two people is a chain of at most three clips; the next scene in the same place starts on the last frame of the one before** (`start_from`, `start_crop: [0, 0, 1, 1]`), and may itself be continued. The same two faces, clothes and room held in all eight of them.
- **A cut to a shot of the thing, then back to the room, works**: the clip that carries on after a cutaway skips its first 22 frames, which is what the cut needs. Put the cutaways between scenes, not inside a chain.
- **Show one change in a shot of the thing, in a camera that does not move**: two objects that snap together or push apart, a heap of small objects lifted, particles that bend round a body, a pointer that turns. The change is drawn when it is one and when it is written as what ends up different ("turns through a quarter of a circle and settles"). A pointer or dial written as "swings" alone was not drawn (1 of 2): the thing was drawn as a gauge and did not move.
- **Look at the thing you ask for before the film is cut**: the Take checks pass a shot in which nothing happens. Read 5 frames of every shot of the thing.
- **Name the people so that the transcript spells them the same way** (a name with two usual spellings fails the words check while the shot is right).
- **Draft size (0.45 MP) was enough to judge the whole film.**

LIMITS (say them to the owner)
- The pictures show the idea; they are not a simulation, and numbers cannot be shown.
