---
name: niche-from-video
description: Learn the style of any video (a Short, a music video, a film clip) and write it as a draft niche - measured numbers from video.analyze, the words from asr.transcribe, the look and tone read from frames. Use when the user says "make it like this video" or "learn this style".
---

> Read `niche-rules` first. A niche learned from a video is a **draft**: it describes a style (pace, look, tone, structure), not
> scripts, footage or anyone's likeness. The user checks it before it is used.

# Steps

1. **Measure.** `video.analyze {path}` (at most 180 s; `from`/`to` for a part). It gives `cut_times`, `rhythm` (cuts per second,
   average, shortest and longest shot), `brightness`, `contrast`, `palette` (five colours with their share) and `sound`
   (LUFS, peak, bpm, `cuts_on_beat`: the share of cuts within 60 ms of a beat). The cut detector finds hard cuts and large jumps
   (a title popping on, a whip pan); it can name one or two extra cuts in a busy video, so look at the frames before trusting a count.
2. **Hear.** `asr.transcribe {path}` (a job: follow it with `jobs.get`). Words with times. Speech share = the time the words cover
   divided by the length. No words and a clear beat: it is a music video or a montage (`KIND: music video`).
3. **See.** `see.contact_sheet` works on a project's sequence, so put the video on a scratch project's timeline first
   (`project.create`, then `timeline.edit` `add_clip {path}`), then `see.contact_sheet {count: 12..24}` or `see.frames` at the cut
   times. Read the images. Look at: shot types (wide, close-up, action), what the camera does, how the first 3 seconds hook, text on
   screen (where, how big, what colour, word by word or not), the colours and grade, one character or many.
4. **Write the draft niche** in the headings of the default niche (`niche-funny-short`), first line `DRAFT: check before use`,
   second line `KIND: short | music video | film` (a short: vertical and 70 s or less):
   - FORMAT: shape, length, number of shots and their length, how it opens, how it ends.
   - SCRIPT VOICE: tone and wording in a few lines, with 2 or 3 short example lines taken from the transcript, **paraphrased**, not copied.
   - LOOK: `{style}` and `{character}` in words, camera, brightness and palette (numbers from step 1).
   - TEXT: caption position, size, colours, pop words.
   - PACE AND SOUND: a cut every N seconds (and every N beats at the bpm, if the cuts are on the beat), flash or zoom at cuts,
     voice, music kind, loudness (LUFS).
   - CHECKS: the numbers to hit again (length, cuts per second, loudness) and what to look at.
5. **Show it** to the user and say what is measured and what is your reading. Keep it as a niche only when they say so:
   `skill.save {project, title, description: "Niche: <title>", body}` (the editor lists project skills whose description starts
   with "Niche" in its Niches panel).

# Limits to say out loud

- It learns a style, not an exact copy; the likeness of a person in the video is never reused.
- The numbers are measured; tone, hook and structure are your reading and may be wrong.
- You cannot hear: loudness and tempo are measured, how it sounds is not.
- A commercial song in a reference video is a reference for tempo and pace only; the new video needs music the user has the right to use.
