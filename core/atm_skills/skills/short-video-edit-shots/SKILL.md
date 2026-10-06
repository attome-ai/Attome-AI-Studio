---
name: short-video-edit-shots
description: The retention pass for a Short - cut every scene into 2-4 shots (wide, close-up on faces, side medium, tight) with scale/position keyframes so something changes every 1-1.5 s, then render, check frames and export the MP4. Use after scenes, voice and music are in.
---

> An Attome skill: built into Attome, and a project can keep its own (`skill.save`). Find them with `skill.list`, read one with
> `skill.get {id}`, and its files with `skill.get {id, file: "scripts/x.ps1"}`. The scripts are recipes (PowerShell or Python):
> fetch one, adapt the block at the top, run it from the repository root. They are being replaced by Tools
> (`short.plan`, `short.build`, `short.voice`, ... see docs/plan/SHORT_TOOLS.md); use a Tool when the list has it.


# Shots every 1-1.5 seconds, render, export

The user's feedback: scenes of 4 s on one image feel "boring"; they want **action every 1-3 seconds**.

`scripts/cut_into_shots.ps1 -Project X.attome` (run from the repo root):
- per V1 clip: 3 shots if <= 3 s (the hook), else `round(dur/40 frames)` shots;
- shot plan: wide (1.00 -> 1.08) / close on the upper middle (1.50 -> 1.64, focus y 0.38) / medium to a side (1.30 -> 1.40) / tight (1.78 -> 1.92); alternate scenes mirror the side;
- each cut lands with a 1.07x punch that settles in 6 frames; position keyed so the focus point stays centred (`pos = 0.5 + m*(0.5-focus)*0.9`; the image must keep covering the frame);
- it removes the clip's old `scale` and `position` keyframes first (child ids, not the collection).
- Focus points are fixed guesses (faces are usually upper-middle). **Look at frames** (`see.frames` at cut+0.3 s) and edit the `$plan` / per-scene focus where the close-up misses the action.
- Ideas not yet done: whoosh/tick on inner cuts, sticker/emoji pops, shake on punchlines, speed ramps.

## Render and check
```
attome validate <p>                      # ok, 0 warnings
attome render <p> -o out.mp4 --bitrate 6   # state: done. The default is 12.4 Mbit/s for a Short (54 MB); 6 Mbit/s gives 27 MB and looks the same on a phone
attome probe out.mp4                     # 1080x1920 30fps 36 s audio=True
see.contact_sheet / see.frames           # Read the image paths
```
Name exports `..._vNN_<what changed>.mp4` and copy the latest over the plain name. Then open it for the user (`Start-Process`). Report what you did *not* verify (audio by ear, sync by ear).

## Final package for the user
Video file, the script (words), the shot list, the ~100-character description + hashtags, and what to try next (a cloned reference voice, real SFX files, beat-snapped pop words).

