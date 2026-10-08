#!/usr/bin/env bash
# Check out the shared spa-fx module at libs/spa-fx, at exactly the commit
# pinned in libs/spa-fx.pin (modelled on fetch_spa_licensing.sh).
#
# SPA_FX_DIR=~/spa-fx scripts/fetch_spa_fx.sh   # use a local checkout as-is
# prints the checkout's commit and warns if it differs from the pin, but does
# not touch it (CMake reads SPA_FX_DIR directly). Without SPA_FX_DIR the script
# clones ~/spa-fx (or SPA_FX_SRC / the private GitHub repo) into libs/spa-fx
# and checks out the pin. CI uses actions/checkout with secret SPA_LICENSING_TOKEN.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIN="$(tr -d '[:space:]' < "$REPO_ROOT/libs/spa-fx.pin")"
if [[ -n "${SPA_FX_DIR:-}" ]]; then
    HEAD_SHA="$(git -C "$SPA_FX_DIR" rev-parse HEAD)"
    echo "spa-fx (local override) at ${HEAD_SHA:0:7} (pinned ${PIN:0:7})"
    [[ "$HEAD_SHA" == "$PIN" ]] || echo "warning: SPA_FX_DIR is not at the pinned commit" >&2
    [[ -z "$(git -C "$SPA_FX_DIR" status --porcelain)" ]] || echo "warning: SPA_FX_DIR has local changes" >&2
    exit 0
fi
DEST="$REPO_ROOT/libs/spa-fx"
SRC="${SPA_FX_SRC:-}"
if [[ -z "$SRC" ]]; then
    if [[ -d "$HOME/spa-fx/.git" ]]; then SRC="$HOME/spa-fx"
    else SRC="https://github.com/meeglosh/spa-fx.git"; fi
fi
[[ -d "$DEST/.git" ]] || git clone --quiet "$SRC" "$DEST"
if ! git -C "$DEST" cat-file -e "$PIN^{commit}" 2>/dev/null; then
    git -C "$DEST" fetch --quiet "$SRC" || true
    git -C "$DEST" fetch --quiet origin || true
fi
if [[ -n "$(git -C "$DEST" status --porcelain)" ]]; then
    echo "error: $DEST has local changes; refusing to build a release from it" >&2
    exit 1
fi
git -C "$DEST" -c advice.detachedHead=false checkout --quiet "$PIN"
echo "spa-fx at $(git -C "$DEST" rev-parse --short HEAD) (pinned $PIN)"
