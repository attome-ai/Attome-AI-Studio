#!/usr/bin/env bash
# Bring this checkout up to date: fetch the newest version, refresh
# dependencies, and rebuild.
#
#   ./update.sh [--channel stable|dev] [--force] [--check]
#
#   --channel   stable = newest tagged release (default), dev = latest main
#   --force     update even with local changes (they are kept in a git stash)
#   --check     only say whether an update is available (exit 10 = yes)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
CHANNEL=stable; FORCE=0; CHECK=0

while [ $# -gt 0 ]; do
  case "$1" in
    --channel) CHANNEL="${2:-}"; shift ;;
    --force) FORCE=1 ;;
    --check) CHECK=1 ;;
    -h|--help) sed -n '2,9p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
case "$CHANNEL" in stable|dev) ;; *) echo "--channel must be stable or dev" >&2; exit 2 ;; esac

die() { echo "error: $*" >&2; exit 1; }
[ -d .git ] || die "This folder is not a git checkout, so it cannot update itself. Clone the repository again."

echo "==> Looking for updates"
git fetch --tags --prune origin

TARGET="origin/main"
if [ "$CHANNEL" = stable ]; then
  TAG="$(git tag --list 'v*' --sort=-v:refname | head -n1)"
  if [ -n "$TAG" ]; then TARGET="$TAG"; else echo "No tagged release yet; following main."; fi
fi

CURRENT="$(git rev-parse HEAD)"
NEW="$(git rev-parse "$TARGET^{commit}")"

if [ "$CURRENT" = "$NEW" ]; then
  echo "Already up to date ($TARGET)."
  [ "$CHECK" = 1 ] && exit 0
else
  echo "Update available: $TARGET"
  echo "What changed:"
  git log --oneline --no-decorate -n 20 "$CURRENT..$NEW"
  [ "$CHECK" = 1 ] && exit 10
fi

if [ -n "$(git status --porcelain)" ]; then
  [ "$FORCE" = 1 ] || die "You have local changes. Commit or stash them, or run ./update.sh --force (they will be stashed, not lost)."
  git stash push -u -m "attome-update $(date +%Y-%m-%dT%H:%M:%S)"
fi

if [ "$CURRENT" != "$NEW" ]; then
  if [ "$CHANNEL" = stable ] && [ "$TARGET" != "origin/main" ]; then
    git checkout --detach "$TARGET"
  else
    git checkout main
    git merge --ff-only origin/main
  fi
fi

echo "==> Refreshing dependencies and rebuilding"
"$ROOT/scripts/setup.sh" --yes
echo "Updated."
