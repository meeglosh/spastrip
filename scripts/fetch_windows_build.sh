#!/usr/bin/env bash
# Downloads the Windows installer .exe that CI published as an asset on a DRAFT
# GitHub Release (tag ci-windows-<short sha>); see .github/workflows/build.yml.
# Release assets are used instead of actions/upload-artifact because the
# account-wide Actions storage quota is too small for the exe.
#
# Draft releases are NOT resolvable via `gh release download <tag>` (that uses
# the /releases/tags/<tag> endpoint, which does not return drafts), so this
# lists releases via the API and downloads the asset by numeric id. The repo is
# private: `gh` must be authenticated with access to meeglosh/spastrip.
#
# Usage:
#   scripts/fetch_windows_build.sh [<sha-or-'head'-or-'latest'>] [<outdir>]
#
#   <sha-or-...>  A commit SHA (full or short) to fetch the matching
#                 ci-windows-<sha7> draft; "head" (default) = the current git
#                 HEAD of this checkout; "latest" = the newest ci-windows-*
#                 draft regardless of commit.
#   <outdir>      Where to save the exe. Default: dist/installers/
#
# Prints the saved path on the last line of stdout ("Saved to: <path>").

set -euo pipefail

SHA_ARG="${1:-head}"
OUT_DIR="${2:-dist/installers}"

for tool in gh jq; do
  command -v "$tool" >/dev/null 2>&1 || { echo "error: '$tool' is required but not found on PATH." >&2; exit 1; }
done

if [ "$SHA_ARG" = "head" ]; then
  SHA_ARG="$(git rev-parse HEAD)"
fi

# owner/repo: prefer `gh repo view`, fall back to parsing the git remote.
REPO=""
if REPO="$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)"; then
  :
else
  REMOTE_URL="$(git remote get-url origin 2>/dev/null || true)"
  REPO="$(echo "$REMOTE_URL" | sed -E 's#^(https://github\.com/|git@github\.com:)##; s#\.git$##')"
fi
[ -n "$REPO" ] || { echo "error: could not determine the GitHub owner/repo." >&2; exit 1; }

if [ "$SHA_ARG" = "latest" ]; then
  TAG_FILTER='startswith("ci-windows-")'
else
  TAG_FILTER=". == \"ci-windows-${SHA_ARG:0:7}\""
fi

# Draft releases only show up via the list endpoint.
RELEASE_JSON="$(gh api --paginate "repos/${REPO}/releases" \
  --jq "[.[] | select(.draft == true and (.tag_name | ${TAG_FILTER}))] | sort_by(.created_at) | reverse | .[0]")"

if [ -z "$RELEASE_JSON" ] || [ "$RELEASE_JSON" = "null" ]; then
  echo "error: no matching ci-windows-* draft release found for '${SHA_ARG}' in ${REPO}." >&2
  echo "Has this commit been pushed, and has CI finished? Check: gh run list --limit 3" >&2
  exit 1
fi

FOUND_TAG="$(echo "$RELEASE_JSON" | jq -r '.tag_name')"
ASSET_ID="$(echo "$RELEASE_JSON" | jq -r '[.assets[] | select(.name | endswith(".exe"))][0].id // empty')"
ASSET_NAME="$(echo "$RELEASE_JSON" | jq -r '[.assets[] | select(.name | endswith(".exe"))][0].name // empty')"

if [ -z "$ASSET_ID" ] || [ -z "$ASSET_NAME" ]; then
  echo "error: draft release '${FOUND_TAG}' has no .exe asset attached (CI may still be running)." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
OUT_PATH="${OUT_DIR%/}/${ASSET_NAME}"
gh api -H "Accept: application/octet-stream" "repos/${REPO}/releases/assets/${ASSET_ID}" > "$OUT_PATH"

if command -v md5 >/dev/null 2>&1; then MD5="$(md5 -q "$OUT_PATH")"
elif command -v md5sum >/dev/null 2>&1; then MD5="$(md5sum "$OUT_PATH" | awk '{print $1}')"
else MD5="(no md5 tool)"; fi

echo "Fetched draft release: ${FOUND_TAG}"
echo "md5: ${MD5}"
echo "Saved to: ${OUT_PATH}"
