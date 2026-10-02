#!/usr/bin/env bash
# Assembles the customer-facing text files for one version into <outdir>:
#
#   EULA.txt, README.txt, QUICKSTART.txt   from packaging/docs/ (README and
#                                          QUICKSTART have @VERSION@ and @DATE@
#                                          replaced)
#   CREDITS.txt                            generated from assets/irs/CREDITS.md,
#                                          the single source of the factory
#                                          impulse response attributions (the
#                                          same file the plugin embeds and shows
#                                          in its About dialog), with the
#                                          Markdown markers removed
#
# Used by the macOS installer build, the Windows CI job (Inno Setup installs
# these), and the Shopify staging step, so every copy of the docs comes from
# one place. Plain bash + sed so it runs the same on macOS and the Windows
# runner's Git Bash.
#
#   scripts/prepare_docs.sh <outdir> <version>
#
# The date is the date of the HEAD commit, not the wall clock, so the docs
# inside the installers (built in CI) and the loose copies in the download
# folder (built here) agree for the same commit. Override with
# SPASTRIP_DOC_DATE="2 October 2026".
#
# Any "TODO" left in a doc is a statement the author could not verify against
# the code. The script refuses to produce docs that still contain one unless
# SPASTRIP_ALLOW_TODO=1 (dry runs and CI set it; the real release does not).

set -euo pipefail

out="${1:?usage: prepare_docs.sh <outdir> <version>}"
version="${2:?usage: prepare_docs.sh <outdir> <version>}"

repo="$(cd "$(dirname "$0")/.." && pwd)"
docs="$repo/packaging/docs"

if [ -n "${SPASTRIP_DOC_DATE:-}" ]; then
    doc_date="$SPASTRIP_DOC_DATE"
else
    stamp="$(git -C "$repo" log -1 --format=%ct 2>/dev/null || true)"
    if [ -n "$stamp" ]; then
        doc_date="$(date -r "$stamp" '+%-d %B %Y' 2>/dev/null || date -d "@$stamp" '+%-d %B %Y')"
    else
        doc_date="$(date '+%-d %B %Y')"
    fi
fi

mkdir -p "$out"

for doc in README.txt QUICKSTART.txt; do
    sed -e "s/@VERSION@/$version/g" -e "s/@DATE@/$doc_date/g" "$docs/$doc" > "$out/$doc"
done
cp "$docs/EULA.txt" "$out/EULA.txt"

# Markdown to plain text: the same transformation the About dialog applies
# (drop heading hashes and bold markers).
sed -e 's/\*\*//g' -e 's/^#\{1,\} *//' "$repo/assets/irs/CREDITS.md" > "$out/CREDITS.txt"

if [ "${SPASTRIP_ALLOW_TODO:-0}" != "1" ] && grep -n 'TODO' "$out"/*.txt; then
    echo "error: unresolved TODO(s) in the docs above. Resolve them in packaging/docs/," >&2
    echo "       or set SPASTRIP_ALLOW_TODO=1 for a dry run." >&2
    exit 1
fi
