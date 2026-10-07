---
name: short-video-concept
description: Turn a topic into a Short's brief - hook, timed script with word counts, shot list, per-scene AI prompts with a shared {character} and {style}, a ~100-character description and hashtags. Use before building any Short.
---

> An Attome skill: built into Attome, and a project can keep its own (`skill.save`). Find them with `skill.list`, read one with
> `skill.get {id}`, and its files with `skill.get {id, file: "scripts/x.ps1"}`. The scripts are recipes (PowerShell or Python):
> fetch one, adapt the block at the top, run it from the repository root. They are being replaced by Tools
> (`script.plan`, `short.build`, `voice.make`, ... see docs/plan/SHORT_TOOLS.md); use a Tool when the list has it.


# Concept and script for a Short

Inputs: topic. Output: `spec.md` (see `short-video`) plus the script and prompts below. Do not build anything yet.

## Structure that retained attention (30-40 s)

- **0-3 s hook**: a question or a claim the viewer must see answered. On screen as big text; spoken in the first second.
- **3-31 s beats**: 7-8 beats of ~4 s. Each beat = one *place*, one *action*, one *joke or surprise*. Label them ("HOUR 3", "STEP 2", "DAY 5") so the viewer sees progress.
- **31-36 s twist** that reverses the premise, then a **call to follow** ("Follow for more What Ifs!").
- Narration: ~2.4-2.8 words/second. 9 lines, each 8-13 words, ends on a hard stop (`.`, `!`, `?`) - captions and pauses use the punctuation.
- Total narration 80-95 words for 36 s. Count them.

## Scene prompts (one per beat, for the video model)

```
<place>. {character}, <what they do>, while <other people react>. <camera/motion hint>. {style}
```
- Keep `{character}` and `{style}` as variables (Attome Variables) so every scene looks like the same film.
- One motion hint per scene ("Quick pop-in motion", "Light camera shake, fast zoom-in", "Slow sad zoom").
- Model: `minimax-h3.fl2va.turbo8-int8` (or `fasth3.8step-v2.int8`) via the user's ComfyUI; 4-5 s per scene.

## Also deliver

- **Shot list**: table of time | scene | on-screen label | narration | motion/effect | SFX.
- **Description** (~100 characters) + 3-5 hashtags, topic keyword first.
- Caption emphasis words (15-ish keywords shown in yellow).
- Sound-word pops (POOF!, BOING!...) - one per beat, chosen to match the action.

Ask the user only what changes the result: the topic, the character, a reference voice or song. Decide the rest.
