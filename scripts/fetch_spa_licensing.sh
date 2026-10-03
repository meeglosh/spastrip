#!/usr/bin/env bash
# Check out the PRIVATE licensing module at libs/spa-licensing, at exactly the
# commit pinned in libs/spa-licensing.pin. scripts/build_release.sh calls this; CI does the
# same with actions/checkout. Clones from a local ~/spa-licensing if present
# (fast, no credentials), otherwise from GitHub (needs access to the repo).
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$REPO_ROOT/libs/spa-licensing"
PIN="$(tr -d '[:space:]' < "$REPO_ROOT/libs/spa-licensing.pin")"
SRC="${SPA_LICENSING_SRC:-}"
if [[ -z "$SRC" ]]; then
    if [[ -d "$HOME/spa-licensing/.git" ]]; then SRC="$HOME/spa-licensing"
    else SRC="https://github.com/meeglosh/spa-licensing.git"; fi
fi
if [[ ! -d "$DEST/.git" ]]; then
    git clone --quiet "$SRC" "$DEST"
fi
if ! git -C "$DEST" cat-file -e "$PIN^{commit}" 2>/dev/null; then
    git -C "$DEST" fetch --quiet "$SRC" || true
    git -C "$DEST" fetch --quiet origin || true
fi
if [[ -n "$(git -C "$DEST" status --porcelain)" ]]; then
    echo "error: $DEST has local changes; refusing to build a release from it" >&2
    exit 1
fi
git -C "$DEST" -c advice.detachedHead=false checkout --quiet "$PIN"
echo "spa-licensing at $(git -C "$DEST" rev-parse --short HEAD) (pinned $PIN)"
