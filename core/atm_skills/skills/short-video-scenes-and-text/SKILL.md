---
name: short-video-scenes-and-text
description: Build the Attome project for a Short - create the AI scene clips from prompts, then titles, shadowed word-by-word captions, pop words, per-cut flash, a beat push, mood grades and transitions - with ready PowerShell scripts to adapt (scenes array, script lines).
---

> An Attome skill: built into Attome, and a project can keep its own (`skill.save`). Find them with `skill.list`, read one with
> `skill.get {id}`, and its files with `skill.get {id, file: "scripts/x.ps1"}`. The scripts are recipes (PowerShell or Python):
> fetch one, adapt the block at the top, run it from the repository root. They are being replaced by Tools
> (`short.plan`, `short.build`, `short.voice`, ... see docs/plan/SHORT_TOOLS.md); use a Tool when the list has it.


# Scenes and on-screen text

Prerequisites: `short-video-concept` done; ComfyUI up (`attome gen engines`); the repo built (`build\win-msvc-release\bin\attome.exe`).

```
attome new Name.attome --rate 30 --canvas 1080x1920
```

All scripts here take `-Project <path>.attome`, run from the repo root, and have a block at the top you **edit** (scenes, narration, key words). They stop on the first error.

1. `scripts/create_scenes.ps1` - `gen.create_clip` per scene (`model`, `seconds`, `name`, `prompt`). Then `attome gen run <project>` generates them (watch `gen status`). Look at a contact sheet before going on.
2. `scripts/titles_and_captions.ps1` - tracks `Caption shadow`, `Captions`, `Title shadow`, `Titles`; one text clip per word (black copy offset 0.0035 under the white/yellow one), hook title, labels, CTA. Edit `$scenes` (start/end seconds, label, narration) and `$key` (yellow words). Caption timing is only a guess here: **`short-video-voice` retimes it from the real voice**.
3. `scripts/motion_and_flash.ps1` - word pops, label slides, a flash adjustment layer on every cut, one look over the film.
4. `scripts/beat_mood_pop_words.ps1` - `Mood` adjustment clips (grade/vignette/grain per scene), whip blur on the flashes, the sound-word pops (edit `$bursts`: time, word, colour, y).
5. `scripts/transitions.ps1` - one transition per cut, all different (zoom, push, wipe, slide, iris).

Gotchas
- Text clips: `media_ref.type = "text"`, `content{text,size,color,bold}`, `transform.position` as `[[x,y]]` normalised. Shadow = same text, `#000000`, offset +0.0035.
- Tracks are added with `anchor:{before: <track id>}` to control stacking (first added sits nearest the pictures).
- Keyframes: `transform/keyframes/<prop>/$new:k` with `{t:"12@30", v:..., interp:"easing", ease:"ease_out_back"}`; **two keyframes at one time are rejected**; collections cannot be removed - remove child keyframe ids.
- **Check the track order** (`get` the sequence: `track_order` lists bottom to top). V1 must be first, then Grade/Flash/Mood, then Caption shadow, Captions, Title shadow, Titles, Pop shadow, Pop words. If the text tracks sit below V1 they are hidden (it happened on the Zahraa Short): fix with `{"op":"move","path":<track>,"to":"<seq>/tracks","anchor":{"before":<track id>}}`.`n- `attome validate <project>` after each script (expect `ok`, 0 warnings).
- Frame check: `see.contact_sheet` {project,count,columns,width} and `see.frames` {project,times:["96@30"],width}.

