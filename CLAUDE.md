# Attome

Attome is a video engine with an editor. Everything the engine can do is a **Tool**; the command line is
`build/win-msvc-release/bin/attome.exe` (`attome tools` lists the Tools, `attome call <tool> params.json` calls one).

## When asked to MAKE a video (a film, a Short, an ad, a tutorial, a music video)

Do not start from your own ideas of how to prompt a video model. What works and what fails with this engine was found by
experiment and is kept **inside the engine as skills**:

1. `attome --json call skill.list -` (parameters `{}` on stdin) lists them. Read `niche-rules`, then the entry skill of
   the kind of video (`film` for anything with characters; `short-video` for a Short), then its niche
   (`niche-explainer-film`, `niche-anime-action`, ...). Read with `skill.get {"id": "film"}`; a skill's other files with
   `skill.get {"id": "film", "file": "LAB.md"}`.
2. Follow them, including their checks and their "Setting up" section. The work is done with the engine's Tools
   (`gen.create_clip`, `gen.check_take`, `timeline.edit`, `audio.match`, `music.make`, `render`), not with programs of your own
   around ffmpeg; `skill.get {"id": "film", "file": "scripts/attome_film.py"}` is a ready driver over them.
3. What you learn that they do not say goes back into them, in general terms (the skill `film` says how). The sources are
   in `core/atm_skills/skills/`; they are compiled into the engine, so rebuild after changing them.

If `skill.list` shows no `film`, the build is older than the sources: build first.

## Working on the engine itself

C++20, CMake presets, MSVC (`vcvars64.bat`, then `cmake --build build/win-msvc-release`). Tests:
`build/win-msvc-release/bin/atm_tests.exe -r compact`.
