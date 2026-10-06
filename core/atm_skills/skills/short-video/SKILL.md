---
name: short-video
description: Make a 30-40 s vertical (9:16) YouTube Short with Attome from a topic - script, AI scenes, word-by-word captions, voiceover, beat-locked music, SFX, fast cuts, MP4 export. Start here; it names the other short-video-* skills in order.
---

> An Attome skill: built into Attome, and a project can keep its own (`skill.save`). Find them with `skill.list`, read one with
> `skill.get {id}`, and its files with `skill.get {id, file: "scripts/x.ps1"}`. The scripts are recipes (PowerShell or Python):
> fetch one, adapt the block at the top, run it from the repository root. They are being replaced by Tools
> (`short.plan`, `short.build`, `short.voice`, ... see docs/plan/SHORT_TOOLS.md); use a Tool when the list has it.


# Short video with Attome (the "What If You...?" format and its relatives)

Use this when the user gives a **topic** ("What if you were invisible for 24 hours?") and wants a finished Short.
It works for any topic with a hook, 6-9 numbered/ordered beats, a twist and a call to follow.

Run the steps in order. Each is its own skill; read it when you reach it.

| # | Skill | What it makes |
|---|-------|---------------|
| 1 | `short-video-concept` | brief: hook, script (word counts), shot list, scene prompts, description, hashtags |
| 2 | `short-video-scenes-and-text` | the Attome project: AI scene clips, titles, word captions, pop words, motion, mood, transitions |
| 3 | `short-video-voice` | voiceover (Kokoro or OmniVoice on the user's ComfyUI), speeds fitted to the scenes, captions timed from the voice |
| 4 | `short-video-music-and-sfx` | song cut on the beat, ducked under the voice, whooshes on the cuts, scene sound muted |
| 5 | `short-video-edit-shots` | a new shot every 1-1.5 s inside each scene (the retention trick), render, check, export |

## Ground rules learned the hard way

- **Never call the finished thing good without looking.** `see.contact_sheet` / `see.frames` give image paths; Read them. You cannot hear audio: say so, and ask the user what sounds wrong.
- **Never download models or files without a clear yes.** List the files and approximate sizes first. A scope answer ("go with kokoro") is not approval of the file list.
- Use the **GPU**. ComfyUI runs on 127.0.0.1:8188; check `attome gen engines`.
- Do not commit or push unless asked. Never add backward-compatibility for old projects.
- Use Attome's own terms: Clip Workflow, Take, Exposed Input, Preset.
- Windows + PowerShell 5.1: write JSON with `[IO.File]::WriteAllText(..., UTF8Encoding($false))`; no `&&`; heredocs with quotes break in Bash - write scripts to files instead.
- `attome call <tool> params.json` runs any Tool (`attome tools` lists them). `project.patch` takes `{ops:[{op:add|replace|remove,path,value}]}`; `$new:name` placeholders link ops in one patch; times are rationals (`"36"`, `"7/50"`, `"90@30"`).
- Tracks cannot overlap: a patch that makes two clips on one track overlap is rejected as a whole. Set lengths first, positions second.
- Each render: `attome render <project> -o out.mp4`, then probe it. Overwrite the "latest" copy only after the render says `done`.

## Spec to fill in at the start (keep it in the project folder as `spec.md`)

```
topic:        <one line>
format:       9:16 1080x1920, 30 fps, 30-40 s
hook (0-3 s): <question that creates a gap>
beats:        <6-9 x  label | scene idea | one-line narration>
twist:        <last 4-5 s, reverses the premise>
cta:          <follow line>
character:    <one recurring character, described once, reused as {character}>
style:        <one style sentence, reused as {style}>
voice:        <kokoro am_michael | omnivoice description>
music:        <track + start second, tempo>
```
