<div align="center">

# Attome

**An AI-first video editor with a node-based generation engine.**
Local-first. Scriptable. Open source.

[Status](#status) · [Download](#download) · [Build from source](#build-from-source) · [Updating](#updating) · [What it is](#what-is-attome) · [Contributing](#contributing) · [License](#license)

</div>

---

## Status

**Pre-alpha. Not usable for real projects yet.**

The design is settled and the engine is being built in phases. This repository already contains the one-command setup and update scripts, so the toolchain is ready the day code lands. Watch the repository (Releases only) to hear about the first build.

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
cmake --preset <preset>            # win-msvc-release | mac-clang-release | linux-clang-release
cmake --build --preset <preset>
```

Libraries come from [vcpkg](https://github.com/microsoft/vcpkg) in manifest mode. Set `VCPKG_ROOT` to the vcpkg folder (setup uses `.deps/vcpkg`).

## AI models

Attome does not ship models. You choose them:

- **Local:** run models on your own GPU, for example through [ComfyUI](https://github.com/comfyanonymous/ComfyUI).
- **Your own API keys:** use a cloud provider you already pay for.

Models have their own licenses, and some forbid commercial use. Attome records each model's license on every generated clip and warns you before you export something that used a non-commercial model. Check a model's current terms before you rely on it.

## Project layout

Planned layout; folders appear as each part lands.

```
core/        engine in C++ (document model, render graph, GPU compositor)
ai-host/     Python process that runs models and agents
app/         the editor (Qt 6)
schema/      the project file schema
scripts/     setup and update scripts
```

## Contributing

Contributions are welcome once the first code lands. Until then, issues with ideas and bug reports about the setup scripts are the most useful.

- Open an issue before a large change so we can agree on the approach.
- Sign off your commits with `git commit -s` (the [Developer Certificate of Origin](https://developercertificate.org)).
- Keep changes small and focused.

## Security

Please report vulnerabilities privately through GitHub's **Security → Report a vulnerability** on this repository, not in a public issue.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Third-party libraries keep their own licenses. The core only links permissive libraries, and LGPL libraries only through dynamic linking.

The name **Attome** and its logo are not covered by the Apache license. You may fork and build on the code, but please do not present a modified version as the official Attome.
