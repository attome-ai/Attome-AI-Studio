---
name: short-video-music-and-sfx
description: Music for a Short - cut a user-chosen song so its beat lands on the scene cuts (tempo detection, stretch to a round BPM), bake ducking under the voice into the file, mute the scene sound, add synthesized whooshes/clicks at cuts. No downloads, no licence trouble for the SFX.
---

> An Attome skill: built into Attome, and a project can keep its own (`skill.save`). Find them with `skill.list`, read one with
> `skill.get {id}`, and its files with `skill.get {id, file: "scripts/x.ps1"}`. The scripts are recipes (PowerShell or Python):
> fetch one, adapt the block at the top, run it from the repository root. They are being replaced by Tools
> (`script.plan`, `short.build`, `voice.make`, ... see docs/plan/SHORT_TOOLS.md); use a Tool when the list has it.


# Music and sound effects

**Tools first.** `audio.analyze {path | project + clip}` finds the tempo and where the beats fall (`bpm`, `first_beat`, `beats`, `has_beat`, in seconds of the file) and measures peak and RMS in dB, so you can lock cuts or a clip's speed to a song and check levels without a script. `sfx.make {kind (whoosh | click | pop | riser | impact), project}` writes the effect, imports it, and returns an `asset_id` for `timeline.edit add_clip`. `music.fit {project, clip, bpm, at, until?, from?}` sets the clip's speed to a target tempo (tape-style, 0.5x-2x) and puts the first beat at or after `from` (seconds of the file) on `at`. The scripts below remain for the pitch-kept stretch and the ducking recipe, which have no Tool yet.

Needs: `%TEMP%\voices.json` from `short-video-voice` (voice start/end seconds) and the ComfyUI python (`python_embeded\python.exe` has numpy, av, soundfile).

## Music on the beat
`scripts/beat_locked_music.py voices.json [song] [out.wav]` (edit the constants at the top: `SRC` song, `OUT` wav, analysis window, `TARGET_BPM`):
- decodes with PyAV (**a file named .mp3 can be an MP4/AAC container**: soundfile fails, `av` works; `attome probe` works too),
- finds tempo + phase with a comb over an onset curve (search 125-150 BPM; check that the answer is not a harmonic; ask where the user wants the song to start - they said "from 12 s of the song"),
- resamples to 150 BPM so a 4 s scene = 10 beats and puts a beat on each scene start (scene starts at 3.1 + 4k -> grid offset 0.3 s mod 0.4),
- **ducks**: 0.24 gain where nobody speaks, 0.10 under the voice, smoothed 0.12 s (Attome has no ducking control; bake it),
- writes `skeleton_song150.wav`. `attome import` it, then replace the A1 clip: `{audio:{fade_in:"1/4",fade_out:"2"}, timing:{record_in:"0",duration:"36",source_in:"0"}, media_ref:{type:"file",asset,path,duration,has_audio:true}}`.
- The generated scene clips carry their own sound: mute/remove it (`scripts/mute_scene_sound_and_add_music.ps1` shows how).
- Copyright: a commercial track will likely get a Content ID claim on YouTube. Say so once.

## SFX
- `scripts/make_whoosh.py` - 0.7 s band-pass noise sweep landing at 0.5 s with a low thump, panned left to right. Place one **0.5 s before each cut** on an `SFX` audio track, `audio.gain_db` -5.
- `scripts/make_click.py` - UI tick. The user found the synthesized click "bad"; prefer whoosh, pop, riser+impact on the twist. Real recordings (Pixabay, Freesound CC0, Mixkit, YouTube Audio Library) sound better: the user must supply or approve them.
- Sound-word pops (POOF!) and inner-scene cuts can get quieter whooshes (-12 dB).

Loudness order: voice (peak ~0.9) > whoosh (-5 dB) > music (~24%, 10% under speech). If the user says "voice too low", raise voice gain first, then lower music.

