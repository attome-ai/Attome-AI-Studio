<div align="center">

# Attome

**An AI-first video editor with a node-based generation engine.**
Local-first. Scriptable. Open source.

[Status](#status) · [Download](#download) · [Build from source](#build-from-source) · [Updating](#updating) · [What it is](#what-is-attome) · [Contributing](#contributing) · [License](#license)

</div>

---

## Status

**Pre-alpha. Not usable for real projects yet.**

The design is settled and the engine is being built in phases. Watch the repository (Releases only) to hear about the first build.

What exists today:

- the project document with Stable IDs and the git-friendly file format
- exact rational time, with every time spelling accepted at the boundary (`12.5s`, `375@30`, `00:00:12;15`)
- ID-addressed patches that apply all-or-nothing, with dry run, undo and redo
- a crash-safe journal: an edit that was acknowledged survives a kill
- the `attomed` daemon (JSON-RPC over a per-user pipe or socket) and the `attome` command line
- a zone profiler built into the engine, see [Profiling](#profiling)
- a first editor, `attome-editor` (Windows only for now): import video and sound files, arrange them on a multi-track timeline (move, trim, split, opacity, volume), preview, and export an H.264 + AAC `.mp4`
- the same export for scripts and agents: `attome render Demo.attome -o out.mp4`
- text clips (titles, lower thirds, captions) with a size, colour and position, with Arabic and other right-to-left text shaped correctly in the preview and the export (DirectWrite on Windows; FreeType, HarfBuzz and SheenBidi with the bundled Noto fonts elsewhere)
- wipes, pushes and zooms between clips (wipes and pushes from the left, right, top or bottom), next to the dissolve (the Inspector's Transition card): a wipe has a soft edge crossing the picture, a push slides the old clip away and the new one in behind it; a zoom grows the old picture while the new one settles into place (you set how much); all use media beyond the cut like a dissolve does
- dissolves between clips, with the sound cross-faded at equal power; a dissolve uses media beyond the cut, and an edit that leaves too little is refused with the largest length that fits, or you can have the room made: Make room in the Transition card (the `make_room` op, or `make_room` on `add_transition`, for scripts and agents) trims what is missing off the end of one clip and the start of the next, moves the later clips on the track up so no gap opens, and keeps linked sound in step
- picture clips from PNG, JPEG, BMP, GIF and TGA files, with their transparency (a logo over the video), 5 s long unless given a length
- rotation around an anchor point and cropping of each side, for clips and titles (a quarter turn stands a sideways phone video up)
- keyframes on opacity, position, scale, rotation and anchor, and on the parameters of effects (a vignette that closes in, a blur that clears; a diamond on each effect slider adds, updates or removes a key at the playhead, with Previous and Next key) (linear, hold and easing presets); the Inspector's Fade card fades a clip in and out, and the timeline draws the opacity curve on the clip
- sound: music and other sound files on audio tracks, clip gain in dB, pan and fades (equal power or linear), track volume and pan, and mute; the Inspector's Audio card sets them
- colour grade (brightness, contrast, saturation) and vignette, on one clip or on an adjustment layer, from the Effects panel and the Inspector's effect cards
- a Gaussian blur on one clip (its edges soften into what is below it) or on an adjustment layer, which changes everything on the tracks below it; the Effects panel and each clip's Blur card add them
- linked picture and sound: a video with sound comes in as two linked clips, the picture and its sound on an audio track, so the sound can be cut, faded and mixed on its own while moving, trimming, splitting and deleting keep them together; Unlink separates them
- an MCP server, `attome mcp --stdio`: an AI agent such as Claude can build a project, look at frames and a contact sheet of it, and render it, see [Connect an AI agent](#connect-an-ai-agent)

Not built yet: AI generation, effects other than the blur, colour grade and vignette, transitions other than the dissolve, the wipe, the push and the zoom, the GPU compositor, JSON Schema validation, Suggested Edits and per-task undo. Decode and encode use the Windows media stack today, so import and export work on Windows only; the macOS and Linux code paths of the rest are written but have only been built and tested on Windows.

### The editor

```bash
attome-editor                              # asks for a project folder, then: File > Import media, or drop files on the window
attome-editor Demo.attome a.mp4 b.mp4      # open (or create) a project and import two files
```

Drag a clip on the timeline to move it (also between tracks), drag its edges to trim, drag the picture in the Monitor to reposition a clip (position, scale, rotation and crop are in the Inspector's Transform card), add a dissolve into the next clip from the Inspector's Transition card, `S` splits at the playhead, `Space` plays, `Ctrl+Z` / `Ctrl+Y` undo and redo, `Ctrl+E` exports. The editor is a client of the daemon: an edit made by `attome patch` or an agent while it is open shows up by itself. View > Profiler shows the daemon's zones live.

### Try it

```bash
attome new Demo.attome --rate 30000/1001
attome inspect Demo.attome --json          # shows the sequence ID to use below
attome patch Demo.attome edits.json        # {"ops": [{"op": "add", "path": "<seq id>/tracks/$new:v1", "value": {"kind": "video", "name": "V1"}}]}
attome undo Demo.attome
attome import Demo.attome a.mp4 music.wav  # media.import: files become assets
attome timeline Demo.attome ops.json       # timeline.edit: [{"op": "add_clip", "path": "C:/media/a.mp4"}, {"op": "add_text", "text": "Hi"}]
attome tools                               # every command is also a Tool of the daemon
attome daemon start                        # optional: keeps projects open between commands
```

### Connect an AI agent

`attome mcp --stdio` is an MCP server. Its Tools run in the daemon, which starts by itself, so a project open in `attome-editor` is the same live project the agent edits. For Claude Desktop, add this to `claude_desktop_config.json` (Settings > Developer > Edit Config) and restart it:

```json
{ "mcpServers": { "attome": { "command": "C:\\path\\to\\build\\win-msvc-release\\bin\\attome.exe", "args": ["mcp", "--stdio"] } } }
```

For Claude Code: `claude mcp add attome -- C:\path\to\attome.exe mcp --stdio`. The agent gets `guide_get` (how to edit, with examples), `media_import`, `timeline_edit` (the whole cut in one call: clips, text, dissolves, blur, music, and edits such as split, trim and move), `project_create`, `project_inspect`, `project_patch`, `project_undo`, `media_probe`, `see_frames` and `see_contact_sheet` (rendered frames attached as images, so it can check its edit by eye), `render_sequence` and `jobs_wait`. Names, descriptions and parameter schemas come from the engine (`attome tools --json`), so they match the CLI. Try:

> Using the Attome tools, create a 1280x720 project at C:\demo\summer.attome. Put C:\demo\a.mp4 and C:\demo\b.mp4 one after the other, 3 seconds each, muted. Add the title "Summer in the City" over the first 2 seconds in the lower third. Show me a contact sheet, then render to C:\demo\summer.mp4.

## What is Attome?

Attome is a video editor built around three ideas:

- **A clip is a recipe, not just a file.** A generated clip keeps its prompt, seed, model and inputs, so you can change one thing and regenerate it in place, keeping its timing and your edits.
- **An agent can drive it.** Everything the editor does is also a command line tool and an MCP server, so an AI agent (or a script) can edit a project the same way you do.
- **It runs on your machine.** Local models and your own API keys. No account is needed to edit, and your footage stays on your disk unless you choose a cloud model.

## What you will be able to do

**Video**
- Multi-track timeline with word-timed captions and beat markers
- Generated clips from text, images or the previous clip's last frame
- Change existing footage by prompt: replace an object or background, relight, restyle, extend a clip
- Draw on a frame to say what to change
- Keyframes and curves on any value, drawn like an automation lane
- One project, many outputs: 16:9, 9:16, 1:1 with platform presets

**Image**
- Layers, selections, masks and AI fill, on the same engine

**Music**
- Piano roll, step sequencer, mixer, and scoring to picture

**Under the hood**
- A text-based, git-friendly project format
- Workflows compatible with ComfyUI nodes
- Multi-step pipelines (for example: write lyrics, make music, detect beats, generate a scene per beat)
- Headless rendering for servers and batch jobs

## Download

**There is no release yet.** When the first one is published it will be easy to install and signed, so your system does not warn you:

| System | How |
|---|---|
| Windows | `winget install Attome.Attome`, or the signed installer / portable ZIP from [Releases](https://github.com/attome-ai/Attome-AI-Studio/releases) |
| macOS (Apple Silicon) | `brew install attome`, which builds on your machine, so there is no Gatekeeper prompt |
| Linux | Flatpak from Flathub, or the AppImage from Releases |

Every release ships with checksums, build provenance and a signature, and is built in the open by the workflow in this repository. See [Verify your download](#verify-your-download).

If you want to build it yourself, or you are a developer, use [Build from source](#build-from-source).

## Build from source

### Quick start

For developers and early testers. You need [Git](https://git-scm.com). Everything else is installed for you.

**Windows**

```powershell
git clone https://github.com/attome-ai/Attome-AI-Studio.git
cd Attome-AI-Studio
.\setup.cmd
```

**macOS (Apple Silicon) and Ubuntu / Debian**

```bash
git clone https://github.com/attome-ai/Attome-AI-Studio.git
cd Attome-AI-Studio
./setup.sh
```

That one command checks your machine, asks before installing anything, then builds the project.

### What setup does

1. Checks for Git, CMake, Ninja, Python, a C++ compiler and the Vulkan SDK.
2. Installs whatever is missing (winget on Windows, Homebrew on macOS, apt on Ubuntu/Debian). It asks first.
3. Downloads the C++ libraries the project needs into a local `.deps` folder. Nothing is installed system-wide.
4. Sets up the Python environment for the AI host.
5. Builds the project.

Safe to run again at any time. It skips what is already done.

| Option (Windows / macOS, Linux) | Meaning |
|---|---|
| `-CheckOnly` / `--check` | Only report what is installed. Changes nothing. |
| `-Yes` / `--yes` | Do not ask before installing tools. |
| `-DepsOnly` / `--deps-only` | Install tools and libraries, skip the build. |
| `-Config debug` / `--debug` | Build the debug configuration. |

### Requirements

| | |
|---|---|
| System | Windows 10 or 11 (x64), macOS 14 or newer on Apple Silicon, Ubuntu 24.04 or Debian-based |
| Graphics | A GPU with Vulkan 1.3 support |
| Memory | 16 GB recommended |
| Disk | Several GB for tools and libraries; AI models need more |

## Verify your download

Before you run a release file, check that it is the one we built:

```bash
# 1. Checksum (macOS / Linux)
sha256sum -c SHA256SUMS --ignore-missing

# 2. Build provenance: proves the file came from this repository's public CI run
gh attestation verify <downloaded-file> --repo attome-ai/Attome-AI-Studio
```

On Windows PowerShell: `Get-FileHash <file> -Algorithm SHA256` and compare it with the line in `SHA256SUMS`.

Releases never ask you to turn off antivirus, never download programs when they first start without asking you, and never run hidden scripts. If your antivirus flags a signed release, please [open an issue](https://github.com/attome-ai/Attome-AI-Studio/issues) so we can report the false positive.

## Updating

One command brings you to the newest version, refreshes dependencies and rebuilds:

```powershell
.\update.cmd
```

```bash
./update.sh
```

| Option | Meaning |
|---|---|
| `-Channel dev` / `--channel dev` | Follow the latest development code instead of tagged releases. |
| `-Check` / `--check` | Only say whether an update is available. |
| `-Force` / `--force` | Update even with local changes. They are saved in a git stash, not lost. |

By default you follow the newest tagged release. Until the first release is tagged, you follow `main`. Once the app exists it will also offer **Check for updates** in its own menu.

## Building by hand

If you prefer to run the steps yourself, install the tools listed above, then:

```bash
cmake --preset <preset>            # win-msvc-release | win-portable-release | mac-clang-release | linux-clang-release
cmake --build --preset <preset>
```

`win-portable-release` builds on Windows with the code the Linux and macOS builds use (FFmpeg for media, FreeType for text, stb for still images), so the portable path can be tested without those systems.
The editor uses Segoe UI and its icons on Windows and the bundled Noto fonts with [Lucide](https://lucide.dev) icons elsewhere; set `ATTOME_UI_FONTS=bundled` to see that look on Windows.

Libraries come from [vcpkg](https://github.com/microsoft/vcpkg) in manifest mode. Set `VCPKG_ROOT` to the vcpkg folder (setup uses `.deps/vcpkg`).

## Profiling

Speed is a design goal, so the profiler is part of the engine from the first module. Code marks its work with zones:

```cpp
ATM_PROFILE_SCOPE("patch.apply");   // times the enclosing block
```

Zones nest into a tree per thread. Each zone keeps its calls, total, mean, minimum and maximum, plus a smoothed per-request average and a one-second peak. A zone takes no lock and allocates nothing, and uses the CPU timestamp counter as its clock. `atm_bench` measures the cost: about 14 ns per zone on the development machine, and under 1 ns when the profiler is switched off at run time.

| What | How |
|---|---|
| See where the running daemon spends its time | `attome profile` (add `--watch` to refresh every second, `--reset` to zero the numbers) |
| The same data for a script or an agent | `attome profile --json`, or the Tool `profile.get` |
| Profile one command | `attome --profile patch Demo.attome edits.json` |
| Print a report every 10 s on the daemon's own output | `attomed --profile-interval 10` |
| Switch it off or on while running | `attome profile --off` / `--on` |
| Compile it out completely | `cmake --preset <preset> -DATTOME_PROFILING=OFF` |
| Check the engine against its speed targets | `build/<preset>/bin/atm_bench` |
| Check export speed (the plan's F1 scenes) | `build/<preset>/bin/atm_bench --export` |
| Check the editor with real mouse input (Windows) | `powershell -File tools/uitest/drag_clip.ps1` |

`atm_bench` prints each measured number next to its target from the plan, then the zone profile of the run, so a slow number comes with the place the time went. New engine code should add zones around its stages and a case to the benchmark.

## AI models

Attome does not ship models. You choose them:

- **Local:** run models on your own GPU, for example through [ComfyUI](https://github.com/comfyanonymous/ComfyUI).
- **Your own API keys:** use a cloud provider you already pay for.

Models have their own licenses, and some forbid commercial use. Attome records each model's license on every generated clip and warns you before you export something that used a non-commercial model. Check a model's current terms before you rely on it.

## Project layout

Planned layout; folders appear as each part lands.

```
core/        engine in C++, one folder per module (exists: atm_base, atm_storage, atm_doc, atm_patch, atm_media,
             atm_render, atm_api, daemon)
cli/         the attome command line (exists)
tests/       benchmarks and cross-module tests (exists: bench)
ai-host/     Python process that runs models and agents
app/         the editor (SDL3 + Dear ImGui) (exists: app/editor)
schema/      the project file schema
scripts/     setup and update scripts (exists)
```

## Contributing

Contributions are welcome once the first code lands. Read [CONTRIBUTING.md](CONTRIBUTING.md) first: contributions need a one-time CLA so the project can keep offering a commercial license. Until then, issues with ideas and bug reports about the setup scripts are the most useful.

- Open an issue before a large change so we can agree on the approach.
- Sign off your commits with `git commit -s` (the [Developer Certificate of Origin](https://developercertificate.org)).
- Keep changes small and focused.

## Security

Please report vulnerabilities privately through GitHub's **Security → Report a vulnerability** on this repository, not in a public issue.

## License

Attome is dual licensed.

- **Everyone: [GNU AGPL-3.0-or-later](LICENSE).** Free forever for individuals, students, creators, teams and companies that use Attome to make videos. Your videos and projects are yours; the license covers the software, not what you make with it. If you modify Attome and distribute it, or offer it as a network service, you must share your changes under the same license.
- **Companies that cannot or do not want to follow the AGPL** (embedding Attome in a closed product, keeping modifications private) can buy a commercial license. See [COMMERCIAL.md](COMMERCIAL.md).

See also [NOTICE](NOTICE). Third-party libraries keep their own licenses. The core only links permissive libraries, and LGPL libraries only through dynamic linking.

The name **Attome** and its logo are not covered by the AGPL. You may fork and build on the code, but please do not present a modified version as the official Attome.
