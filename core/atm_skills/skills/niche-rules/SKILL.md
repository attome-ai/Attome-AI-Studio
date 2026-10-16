---
name: niche-rules
description: How to follow a niche - a free-text playbook for a kind of video (a Short, a music video, a film). Read this first whenever the user names a niche, pastes a style, or asks for "the same style as" something.
---

# Following a niche

A **niche** is free text that says how a kind of video is made. It may be one sentence or ten pages, it may have the headings
of the default niche (`niche-funny-short`) or none, and the user may have deleted any part of it or all of it. Find the
built-in ones with `skill.list` (`niche-funny-short`, `niche-music-video`, `niche-skeleton`); the user's own are skills of the
project whose description starts with "Niche" (the editor's Niches panel keeps them with `skill.save`). When the user names a niche
by its title, find it there first.

## Four rules

1. **The user's text is the brief.** Follow whatever is there, in its own words. Do not "correct" a niche toward the default.
2. **What is missing falls back to a default.** Take it from `niche-funny-short` for a Short (or from the kind's own default when
   the niche says `KIND: music video` or `KIND: film`). The user's text always wins over a default. Never invent a rule the
   niche does not give when a default exists.
3. **Say it back before spending time.** Before any generation, write at most five lines: format and length, hook, look, voice,
   music, pace; mark which of them came from the niche and which from a default. Wait for a yes. A wrong reading costs nothing
   now; nine generated scenes cost about 7 minutes of GPU.
4. **Check before "done".** Run the niche's CHECKS if it has any. Look at frames (`see.contact_sheet`, `see.frames`) and read them.
   You cannot hear: report audio as "not heard" and say what you measured (`audio.analyze`: loudness, peak).

## Pictures of a niche

A niche of the user's may come with pictures: `skill.get {"id": <the niche>, "project": ...}` lists them as `images` (full paths, each with
an optional label). They show what the words cannot: a look, a colour mood, a kind of place, a frame to match. Open
and look at them (they are ordinary picture files) before you say the niche back, and treat them as part of the brief: the same rule 1 applies.
Give them to an image or video model as references only when the niche's words say that is what they are for.

## When the niche is empty or only a topic

Use the default niche whole, still say it back (rule 3).

## Kinds

- `KIND: short` - the `short-video` skills; the pipeline below the niche is the same, the niche changes the choices.
- `KIND: music video` - no voiceover; cut on the beat of the song (`audio.analyze`), lyrics as captions if the user gives them
  or `asr.transcribe` finds them; the visuals follow the niche's look.
- `KIND: film` - dialogue, music cues and shot scale lead; long; build in sequences and say each one back. Read the skill
  `film` first (the general rules for people, objects and places that stay the same); a niche of this kind adds only what is
  special to it (`niche-anime-action`: fights; `niche-explainer-film`: a film that explains an idea; `niche-game-ad-skit`: a game advert told as a skit; `niche-animal-drama`: animals that act like people in an emotional film).

If the kind is not written, assume `short`.

## Learning a niche from a video

Read `niche-from-video`: `video.analyze` measures the cuts, look and sound, `asr.transcribe` gives the words, frames give the
tone and the look; the result is a **draft niche** the user checks. It learns a style (pace, look, tone, structure), not scripts
or footage. The editor's Niches panel has "Learn from a video", which writes the measured half.
