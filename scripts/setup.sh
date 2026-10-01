#!/usr/bin/env bash
# One-command setup for Attome on macOS and Ubuntu/Debian:
# installs missing tools, fetches dependencies, and builds the project.
#
#   ./setup.sh [--check] [--yes] [--deps-only] [--debug]
#
#   --check       only report what is installed; change nothing
#   --yes         do not ask before installing missing tools
#   --deps-only   install tools and dependencies, do not build
#   --debug       build the debug configuration (default: release)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/.deps"
VCPKG_DIR="$DEPS/vcpkg"
CHECK=0; YES=0; DEPS_ONLY=0; CONFIG=release

for a in "$@"; do
  case "$a" in
    --check) CHECK=1 ;;
    --yes|-y) YES=1 ;;
    --deps-only) DEPS_ONLY=1 ;;
    --debug) CONFIG=debug ;;
    -h|--help) sed -n '2,10p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

c_cyan=$'\033[36m'; c_green=$'\033[32m'; c_yellow=$'\033[33m'; c_red=$'\033[31m'; c_off=$'\033[0m'
step() { echo "${c_cyan}==> $*${c_off}"; }
ok()   { echo "  ${c_green}[ok]${c_off}      $*"; }
miss() { echo "  ${c_yellow}[missing]${c_off} $*"; }
die()  { echo "${c_red}error:${c_off} $*" >&2; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }
# version_ge A B  ->  true when A >= B
version_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]; }
ver_of() { "$@" 2>&1 | head -n1 | grep -Eo '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n1; }

# ---- which system -----------------------------------------------------------
OS="$(uname -s)"
case "$OS" in
  Darwin) PLATFORM=mac; PRESET_PREFIX=mac-clang ;;
  Linux)
    PLATFORM=linux; PRESET_PREFIX=linux-clang
    if [ -r /etc/os-release ]; then . /etc/os-release; fi
    case "${ID:-}${ID_LIKE:-}" in
      *ubuntu*|*debian*) ;;
      *) die "Only Ubuntu/Debian is automated. Install: git, cmake (3.28+), ninja, python (3.11+), clang 18+, pkg-config, curl, zip, unzip, tar, the Vulkan loader and headers. Then run: ./setup.sh --deps-only" ;;
    esac ;;
  *) die "On Windows use setup.cmd. Unsupported system: $OS" ;;
esac

# ---- what we need -------------------------------------------------------------
MISSING=()
check() { # name, command that succeeds when OK
  if eval "$2" >/dev/null 2>&1; then ok "$1"; else miss "$1"; MISSING+=("$1"); fi
}
step "Checking your machine ($PLATFORM)"
check "git"                      "have git"
check "cmake 3.28 or newer"      "have cmake && version_ge \"\$(ver_of cmake --version)\" 3.28"
check "ninja"                    "have ninja"
check "python 3.11 or newer"     "have python3 && version_ge \"\$(ver_of python3 --version)\" 3.11"
check "uv (Python packages)"     "have uv"
check "C++ compiler (clang)"     "have clang++ || xcrun --find clang++"
check "pkg-config"               "have pkg-config"
check "curl, zip, unzip, tar"    "have curl && have zip && have unzip && have tar"
if [ "$PLATFORM" = linux ]; then
  check "Vulkan loader + headers" "pkg-config --exists vulkan"
else
  check "Vulkan (MoltenVK)"       "have brew && brew list molten-vk"
fi
if [ -x "$VCPKG_DIR/vcpkg" ]; then ok "vcpkg (in .deps)"; else miss "vcpkg (will be cloned into .deps)"; fi

if [ "$CHECK" = 1 ]; then
  if [ "${#MISSING[@]}" -eq 0 ]; then echo "${c_green}Everything needed is installed.${c_off}"; exit 0; fi
  echo "${c_yellow}${#MISSING[@]} item(s) missing. Run ./setup.sh to install them.${c_off}"; exit 1
fi

# ---- install what is missing --------------------------------------------------
confirm() {
  [ "$YES" = 1 ] && return 0
  read -r -p "$1 [Y/n] " a || a=n
  case "$a" in n|N|no|NO) return 1 ;; *) return 0 ;; esac
}

if [ "${#MISSING[@]}" -gt 0 ]; then
  if [ "$PLATFORM" = mac ]; then
    have brew || die "Homebrew is needed to install tools. Get it from https://brew.sh then run ./setup.sh again."
    if ! xcode-select -p >/dev/null 2>&1; then
      echo "Installing the Xcode command line tools (a dialog will open)..."
      xcode-select --install || true
      die "Finish the Xcode tools install, then run ./setup.sh again."
    fi
    confirm "Install with Homebrew: cmake ninja python@3.12 git pkg-config uv molten-vk vulkan-loader vulkan-headers shaderc?" || die "Cancelled."
    brew install cmake ninja python@3.12 git pkg-config uv molten-vk vulkan-loader vulkan-headers shaderc
  else
    confirm "Install with apt (needs sudo): build tools, clang, cmake, ninja, python, git, Vulkan dev files?" || die "Cancelled."
    SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"
    $SUDO apt-get update
    $SUDO apt-get install -y build-essential clang cmake ninja-build python3 python3-venv python3-pip \
      git curl zip unzip tar pkg-config libvulkan-dev mesa-vulkan-drivers vulkan-tools glslang-tools
    have uv || { step "Installing uv"; curl -LsSf https://astral.sh/uv/install.sh | sh; export PATH="$HOME/.local/bin:$PATH"; }
  fi
fi

# ---- vcpkg, kept inside the project folder; nothing global ---------------------
mkdir -p "$DEPS"
if [ ! -d "$VCPKG_DIR/.git" ]; then
  step "Fetching vcpkg"
  git clone https://github.com/microsoft/vcpkg.git "$VCPKG_DIR"
fi
if [ ! -x "$VCPKG_DIR/vcpkg" ]; then
  step "Bootstrapping vcpkg"
  "$VCPKG_DIR/bootstrap-vcpkg.sh" -disableMetrics
fi
export VCPKG_ROOT="$VCPKG_DIR" VCPKG_DISABLE_METRICS=1

# ---- Python environment for the AI host ---------------------------------------
if [ -f "$ROOT/ai-host/pyproject.toml" ] && have uv; then
  step "Setting up the Python environment"
  (cd "$ROOT/ai-host" && uv sync)
fi

# ---- build --------------------------------------------------------------------
if [ "$DEPS_ONLY" = 1 ]; then echo "${c_green}Tools and dependencies are ready.${c_off}"; exit 0; fi
if [ ! -f "$ROOT/CMakeLists.txt" ]; then
  echo
  echo "${c_green}Your toolchain is ready. This checkout has no engine sources to build yet.${c_off}"
  echo "Run ./update.sh later to pull new code and build it."
  exit 0
fi

PRESET="$PRESET_PREFIX-$CONFIG"
step "Building ($PRESET)"
(cd "$ROOT" && cmake --preset "$PRESET" && cmake --build --preset "$PRESET") || die "The build failed. The messages above say why."

echo
echo "${c_green}Done. Attome is built.${c_off}"
echo "Try:  build/$PRESET/bin/attome --version"
