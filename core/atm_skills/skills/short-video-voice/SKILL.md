---
name: short-video-voice
description: Add a voiceover to a Short with Attome Generate speech - Kokoro (clean preset voices, per-word timestamps) or OmniVoice (voice by description) on the user's ComfyUI GPU; make samples to choose from, fit speeds to the scenes, time the captions from the voice, set loudness.
---

> An Attome skill: built into Attome, and a project can keep its own (`skill.save`). Find them with `skill.list`, read one with
> `skill.get {id}`, and its files with `skill.get {id, file: "scripts/x.ps1"}`. The scripts are recipes (PowerShell or Python):
> fetch one, adapt the block at the top, run it from the repository root. They are being replaced by Tools
> (`script.plan`, `short.build`, `voice.make`, ... see docs/plan/SHORT_TOOLS.md); use a Tool when the list has it.


# Voiceover

**Tools first.** `voice.make {project, model, lines: [{text, at}], voice?, speed?}` makes one voice clip for each line of the script at its scene start, on the first free audio track. Run `gen.run`, then `voice.fit {project}`: it fits each voice to the time it has (up to the next voice) by its speed, listing in `rerun` any voice whose speed changed; run `gen.run` again and call it again until `next` says done. It also keeps voices from overlapping and sets each one's gain from its loudest sample. The scripts below do the same by hand, for other recipes.

Speech is a Clip Workflow (`voice:<model>`): clip on an **audio** track, Exposed Inputs `text` (required), `voice` (Kokoro: a voice name like `am_michael`; OmniVoice: a description), `seed`; Output `audio` + `length` (the clip becomes exactly as long as the speech).

## Models (ComfyUI at 127.0.0.1:8188; both run on the GPU)

| id | notes |
|----|-------|
| `kokoro.82m` | 82M, clean, no energy control, English voices `am_michael am_adam am_eric am_fenrir am_puck bm_george bm_daniel af_heart ...`. ComfyUI node `AttomeKokoroTTS` (custom_nodes/attome_kokoro), weights `ComfyUI/models/Kokorotts/Kokoro-82M` (`kokoro-v1_0.pth` 312 MB, `config.json`, `voices/*.pt`). Setting: `speed`. |
| `omnivoice.bf16` | 0.6B, voice designed from words ("male, young adult, moderate pitch, american accent"), 646 languages, cloning with a reference clip. Settings: `speed`, `steps` (64 sounded best), `cfg` (2.0), `language`. Licence of the weights is unclear (Apache vs CC-BY-NC): tell the user before commercial use. |

User verdicts on this project: OmniVoice "so bad / not that good"; Kokoro accepted. A cloned reference voice (OmniVoice or Chatterbox) is the next step up - needs a 5-10 s clip from the user.

Installing Kokoro needed `pip install --no-deps kokoro` + `--only-binary=:all: loguru spacy "misaki[en]"` (numpy stays at 2.x; Python 3.13 has no source builds). **Ask before any download.**

## Procedure

1. **Samples first.** `scripts/kokoro_samples.py <voice...>` (or `omnivoice_samples.ps1`) writes WAVs of the same two lines into `Documents\Attome\voice_samples*`. Open the folder; let the user pick. You cannot hear them.
2. `scripts/create_voice_clips.ps1` - one `gen.create_clip` per narration line at the scene starts (edit `$lines`). Saves clip ids to `%TEMP%\voice_ids.txt` (the later scripts read it).
3. `scripts/switch_to_kokoro.ps1 -Voice am_michael` sets model/settings/voice on every line and runs `attome gen run`; it pushes later lines so none overlap.
4. `scripts/fit_kokoro_speeds.ps1` - Kokoro at speed 1.0 is slower than a Short wants. Sets each line's speed = natural length / 3.75 s (1.0-1.4), re-runs, places lines at their scene starts (never overlapping) and writes `%TEMP%\voices.json` (start/end per line, used by the music ducking).
5. **Captions timed from the voice:** `timeline.edit` op `add_captions {clip: <voice clip>}` makes one word-by-word caption clip per sentence on a Captions track, timed from the words the voice model reported (Kokoro returns them as the Take's `words` output; OmniVoice does not, so those are timed by word length). After a voice is made again (another line, speed or voice) run `sync_captions {clip: <voice clip>}`; if the text changed it says so and you make the captions again. The older scripts `sync_captions_to_voice.ps1` and `kokoro_align.py` are the manual route for projects whose captions are separate word clips.
6. `scripts/voice_louder.ps1` - Kokoro files peak ~0.35: gives each voice clip `audio.gain_db` so its peak is ~0.9 (max +10 dB). On an **audio-track clip** the top-level `volume` field is ignored by the renderer; use `audio.gain_db` (and `fade_in`/`fade_out`, `pan`). `volume` does work on a picture clip (volume 0 mutes the sound it came with).

Verify: `attome validate`, contact sheet, then `attome render`. Say plainly that you did not hear it.

