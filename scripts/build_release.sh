#!/bin/zsh
# One-shot release build for SPAStrip: universal Release plugin, tests, macOS
# installer, notarization, the assembled Shopify download folder, and a
# verification report. Single edition, so one folder:
#
#   dist/shopify/SPAStrip-<version>/
#       EULA.txt  README.txt  QUICKSTART.txt
#       SPAStrip-<version>-macOS.pkg
#       SPAStrip-<version>-Windows.exe          (from CI, see --with-windows)
#
#   ./scripts/build_release.sh [--with-windows | --windows-exe <path>]
#   ./scripts/build_release.sh --stage-only <version> [--with-windows | --windows-exe <path>]
#
# --with-windows       fetch the Windows installer CI built for the CURRENT
#                      COMMIT from its draft release (scripts/fetch_windows_build.sh)
#                      and put it in the folder. Push the commit and let the
#                      Windows CI job finish first.
# --windows-exe <path> use an installer you already downloaded instead.
# --stage-only <v>     re-run only staging + verification against an already
#                      built dist/installers/SPAStrip-<v>-macOS.pkg (use after a
#                      notarization failure is fixed with scripts/notarize.sh,
#                      or to add the Windows exe later) without rebuilding.
#
# SIGNING / NOTARIZING (macOS), all opt-in through the environment:
#   SPASTRIP_CODESIGN_IDENTITY   "Developer ID Application: Kenzora Games (7K9WY5T49S)"
#   SPASTRIP_INSTALLER_IDENTITY  "Developer ID Installer: Kenzora Games (7K9WY5T49S)"
# Notarization (scripts/notarize.sh) runs when SPASTRIP_INSTALLER_IDENTITY is
# set, using SPASynth's credentials: ~/.config/spasynth/notary.env first, then
# the keychain profile in SPASYNTH_NOTARIZE_PROFILE.
#
# OTHER ENVIRONMENT
#   SPASTRIP_DIST_DIR     output root (default: <repo>/dist)
#   SPASTRIP_BUILD_DIR    CMake build tree (default: <repo>/build-release)
#   SPASTRIP_DRY_RUN=1    unsigned rehearsal: only PRINTS the ~/Library dev
#                         copies it would remove, never signs or notarizes
#                         (refused if a signing identity is set)
#   SPASTRIP_ALLOW_DIRTY=1  allow an uncommitted tree (rehearsal only; the run
#                         is refused when a signing identity is also set)
#   SPASTRIP_ALLOW_TODO=1 allow "TODO" markers in the docs (rehearsal only)
#   CMAKE_BUILD_PARALLEL_LEVEL=2   advisable on this Mac (4 jobs gets OOM-killed)

set -e -u -o pipefail

REPO_ROOT="${0:A:h:h}"
cd "$REPO_ROOT"

DIST="${SPASTRIP_DIST_DIR:-$REPO_ROOT/dist}"
BUILD="${SPASTRIP_BUILD_DIR:-$REPO_ROOT/build-release}"
mkdir -p "$DIST" "$BUILD"
DIST="${DIST:A}"; BUILD="${BUILD:A}"
DRY_RUN="${SPASTRIP_DRY_RUN:-0}"

die() { echo "error: $*" >&2; exit 1; }

# --- args -----------------------------------------------------------------------
STAGE_ONLY=""
WINDOWS_MODE=""      # "", "fetch", "file"
WINDOWS_FILE=""
while (( $# )); do
    case "$1" in
        --stage-only)   [[ -n "${2:-}" ]] || die "usage: $0 --stage-only <version>"; STAGE_ONLY="$2"; shift 2 ;;
        --with-windows) WINDOWS_MODE="fetch"; shift ;;
        --windows-exe)  [[ -f "${2:-}" ]] || die "--windows-exe needs an existing file"; WINDOWS_MODE="file"; WINDOWS_FILE="${2:A}"; shift 2 ;;
        -h|--help)      sed -n '2,45p' "$0"; exit 0 ;;
        *)              die "unknown argument: $1 (try --help)" ;;
    esac
done

SIGNING=0
[[ -n "${SPASTRIP_CODESIGN_IDENTITY:-}" || -n "${SPASTRIP_INSTALLER_IDENTITY:-}" ]] && SIGNING=1
if [[ "$DRY_RUN" == 1 && "$SIGNING" == 1 ]]; then
    die "SPASTRIP_DRY_RUN=1 is an unsigned rehearsal; unset the signing identities"
fi
if [[ "$SIGNING" == 1 && ( -n "${SPASTRIP_ALLOW_DIRTY:-}" || -n "${SPASTRIP_ALLOW_TODO:-}" ) ]]; then
    die "SPASTRIP_ALLOW_DIRTY / SPASTRIP_ALLOW_TODO are not allowed for a signed release"
fi

read_version() { sed -n 's/^project(SPAStrip VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt; }

# --- Windows installer: fetch / copy, and check its embedded version -----------------
# An Inno Setup exe carries its version in the PE version resource as UTF-16LE
# (SPAStrip.iss sets VersionInfoVersion from AppVersion). Reads FileVersion and
# ProductVersion and requires <version> or <version>.0. Pure python, so it runs
# on macOS. UNVERIFIED against a real Inno exe until CI has produced one.
check_exe_version() {
    local exe="$1" version="$2"
    python3 - "$exe" "$version" <<'PY'
import sys
path, version = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
def u16(s): return s.encode("utf-16-le")
found = {}
for key in ("FileVersion", "ProductVersion"):
    i = data.find(u16(key) + b"\0\0")
    if i < 0:
        continue
    j = i + len(u16(key)) + 2
    while data[j:j+2] == b"\0\0":   # alignment padding
        j += 2
    k = j
    while data[k:k+2] != b"\0\0":
        k += 2
    found[key] = data[j:k].decode("utf-16-le", "replace").strip(" \t\r\n\0")
if not found:
    sys.exit("no FileVersion/ProductVersion resource found in " + path)
print("exe version resource:", found)
if not all(v in (version, version + ".0") for v in found.values()):  # exact, after stripping
    sys.exit("exe version does not match " + version)
PY
}

stage_windows() {   # <version> <folder>
    local version="$1" folder="$2"
    local target="$folder/SPAStrip-$version-Windows.exe" src="" tmp=""
    case "$WINDOWS_MODE" in
        fetch)
            local sha7; sha7=$(git rev-parse --short=7 HEAD)
            tmp=$(mktemp -d "${TMPDIR:-/tmp}/spastrip-winfetch.XXXXXX")
            echo "fetching the Windows installer CI built for commit $sha7 ..."
            "$REPO_ROOT/scripts/fetch_windows_build.sh" "$sha7" "$tmp"
            src=$(print -l "$tmp"/*.exe(N) | head -1)
            [[ -n "$src" ]] || die "fetch produced no .exe"
            ;;
        file) src="$WINDOWS_FILE" ;;
    esac
    if [[ -n "$src" ]]; then
        [[ "${src:t}" == "SPAStrip-$version-Windows.exe" ]] \
            || echo "note: the installer is named ${src:t}; staging it as ${target:t}"
        cp "$src" "$target"
    fi
    [[ -n "$tmp" ]] && rm -rf "$tmp"
    if [[ -f "$target" ]]; then
        check_exe_version "$target" "$version" || die "Windows installer version check failed"
    else
        echo "PENDING: no Windows installer staged (run again with --with-windows once CI is green for this commit)"
    fi
}

# --- Staging (the Shopify folder) --------------------------------------------------
stage_shopify() {   # <version>
    local version="$1"
    local pkg="$DIST/installers/SPAStrip-$version-macOS.pkg"
    local folder="$DIST/shopify/SPAStrip-$version"
    [[ -f "$pkg" ]] || die "no $pkg (build first, or check the version)"
    mkdir -p "$folder"
    # Files are overwritten in place; a previously staged exe is kept.
    local docs; docs=$(mktemp -d "${TMPDIR:-/tmp}/spastrip-docs.XXXXXX")
    "$REPO_ROOT/scripts/prepare_docs.sh" "$docs" "$version"
    # CREDITS.txt is installed by the installers, not shipped loose.
    cp "$docs/README.txt" "$docs/QUICKSTART.txt" "$docs/EULA.txt" "$folder/"
    rm -rf "$docs"
    cp "$pkg" "$folder/"
    stage_windows "$version" "$folder"
    echo "staged: $folder"
}

# --- Verification -------------------------------------------------------------------
verify_release() {   # <version>
    local version="$1"
    local folder="$DIST/shopify/SPAStrip-$version"
    local pkg="$folder/SPAStrip-$version-macOS.pkg"
    local failures=0
    local x; x=$(mktemp -d "${TMPDIR:-/tmp}/spastrip-verify.XXXXXX")
    fail() { echo "  FAIL: $*"; failures=$((failures+1)); }

    echo ""
    echo "=== verification ==="

    # folder contents: exactly the five deliverables (the exe may still be pending)
    local expected=(EULA.txt README.txt QUICKSTART.txt "SPAStrip-$version-macOS.pkg" "SPAStrip-$version-Windows.exe")
    local f
    for f in "$folder"/*(N); do
        (( ${expected[(Ie)${f:t}]} )) || fail "unexpected file in the folder: ${f:t}"
    done
    for f in $expected; do [[ -e "$folder/$f" ]] || echo "  missing: $f"; done

    # signature + Gatekeeper
    if pkgutil --check-signature "$pkg" 2>&1 | grep -q 'Developer ID Installer'; then
        pkgutil --check-signature "$pkg" | sed -n '1,6p' | sed 's/^/  /'
        spctl -a -vv -t install "$pkg" > "$x/spctl" 2>&1 || true
        if grep -q '^source=Notarized Developer ID' "$x/spctl"; then
            echo "  spctl: accepted, $(grep -m1 '^source=' "$x/spctl")"
        else
            sed 's/^/  /' "$x/spctl"; fail "spctl -a -t install did not report 'Notarized Developer ID'"
        fi
        xcrun stapler validate "$pkg" >/dev/null 2>&1 && echo "  stapler: ticket valid" || fail "no valid stapled ticket"
    else
        echo "  UNSIGNED pkg: signature, spctl and staple checks skipped"
        [[ "$DRY_RUN" == 1 ]] || fail "the release pkg is not Developer ID signed"
    fi

    # payload checks on the expanded package
    pkgutil --expand-full "$pkg" "$x/pkg"
    local comp bundle fmt bin plist pdir archs minos sv bv id
    local ids=()
    for comp in "AU:SPAStrip.component" "VST3:SPAStrip.vst3" "AUMIDI:SPAStrip MIDI.component"; do
        fmt="${comp%%:*}"; bundle="${comp#*:}"
        pdir="$x/pkg/SPAStrip${fmt}.pkg/Payload"
        bin="$pdir/$bundle/Contents/MacOS/${bundle%.*}"
        plist="$pdir/$bundle/Contents/Info.plist"
        [[ -f "$bin" ]] || { fail "$fmt binary missing from the payload"; continue; }
        archs=$(lipo -archs "$bin")
        echo "  $fmt: archs $archs"
        [[ "$archs" == *arm64* && "$archs" == *x86_64* ]] || fail "$fmt is not universal ($archs)"
        # LC_BUILD_VERSION prints "minos"; the older LC_VERSION_MIN_MACOSX prints "version".
        minos=$(otool -l "$bin" | awk '/LC_BUILD_VERSION/{s=1} /LC_VERSION_MIN_MACOSX/{s=2} s==1&&$1=="minos"{print $2; s=0} s==2&&$1=="version"{print $2; s=0}' | sort | tr '\n' ' ')
        echo "  $fmt: minos per slice: ${minos}"
        [[ "$minos" == "11.0 11.0 " || "$minos" == "11.0 " ]] || fail "$fmt deployment target is not 11.0 on every slice (${minos})"
        sv=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist")
        bv=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$plist")
        id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$plist")
        echo "  $fmt: bundle id $id, version $sv ($bv)"
        [[ "$sv" == "$version" && "$bv" == "$version" ]] || fail "$fmt bundle version is $sv/$bv, expected $version"
        ids+=("$id")
        codesign --verify --deep --strict "$pdir/$bundle" && echo "  $fmt: codesign seal valid" || fail "$fmt codesign verification failed"
        if [[ "$SIGNING" == 1 ]]; then
            codesign -dvv "$pdir/$bundle" 2>&1 | grep -E 'Authority=Developer ID Application|flags=.*runtime|TeamIdentifier|Timestamp' | sed 's/^/  /' \
                || fail "$fmt is not Developer ID signed with the hardened runtime"
        fi
    done
    (( ${#ids} == 3 )) && [[ "${ids[1]}" != "${ids[2]}" && "${ids[1]}" != "${ids[3]}" && "${ids[2]}" != "${ids[3]}" ]] \
        || fail "AU, VST3 and MIDI AU bundle identifiers are not distinct"
    local d="$x/pkg/SPAStripDocs.pkg/Payload/Library/Application Support/Silverplatter Audio/SPAStrip"
    echo "  docs payload: $(ls "$d" | tr '\n' ' ')"
    grep -q 'CC BY 4.0' "$d/CREDITS.txt" || fail "CREDITS.txt in the docs payload has no CC BY attribution"
    rm -rf "$x"

    echo ""
    echo "=== checksums ==="
    for f in "$DIST/installers/SPAStrip-$version-macOS.pkg" "$folder"/*(N); do
        printf '%s\n  md5    %s\n  sha256 %s\n  size   %s bytes\n' "$f" "$(md5 -q "$f")" "$(shasum -a 256 "$f" | awk '{print $1}')" "$(stat -f %z "$f")"
    done
    (( failures == 0 )) || die "$failures verification check(s) failed (see above)"
    echo ""
    echo "verification passed"
}

# --- --stage-only -----------------------------------------------------------------------
if [[ -n "$STAGE_ONLY" ]]; then
    stage_shopify "$STAGE_ONLY"
    verify_release "$STAGE_ONLY"
    exit 0
fi

# --- 0. Preconditions ----------------------------------------------------------------
VERSION=$(read_version)
[[ -n "$VERSION" ]] || die "could not read the project version from CMakeLists.txt"
MODE=unsigned; [[ $SIGNING == 1 ]] && MODE=signed; [[ $DRY_RUN == 1 ]] && MODE="DRY RUN, unsigned"
echo "=== SPAStrip $VERSION release build ($MODE) ==="
echo "dist:  $DIST"
echo "build: $BUILD"

if [[ -n "$(git status --porcelain)" ]]; then
    if [[ -n "${SPASTRIP_ALLOW_DIRTY:-}" ]]; then
        echo "WARNING: the git tree is dirty; continuing only because SPASTRIP_ALLOW_DIRTY is set (rehearsal)"
    else
        git status --short >&2
        die "the git tree is dirty. Commit or stash first: a release must be built from a clean tree."
    fi
fi
echo "commit: $(git rev-parse HEAD)  branch: $(git branch --show-current)"

# --- 1. Clear dev-build shadow copies ---------------------------------------------------
# Dev/auval builds (SPASTRIP_COPY_PLUGIN=ON by default) copy plugins into the
# user's ~/Library/Audio/Plug-Ins. macOS's AudioComponent lookup prefers the
# user domain over the system domain (/Library, where the installer puts the
# release), so a leftover dev copy silently shadows every signed install in
# every DAW. SPASynth hit this repeatedly; clearing is automatic here.
SHADOWS=("$HOME/Library/Audio/Plug-Ins/Components/SPAStrip.component" "$HOME/Library/Audio/Plug-Ins/Components/SPAStrip MIDI.component" "$HOME/Library/Audio/Plug-Ins/VST3/SPAStrip.vst3")
for s in $SHADOWS; do
    if [[ "$DRY_RUN" == 1 ]]; then
        [[ -e "$s" ]] && echo "DRY RUN: would remove dev shadow copy $s" || echo "DRY RUN: no dev shadow copy at $s"
    else
        rm -rf "$s"
    fi
done
[[ "$DRY_RUN" == 1 ]] || echo "cleared any ~/Library dev-build shadow copy"

# --- 2. Plugin (universal Release) + tests -------------------------------------------------
# The targets are named explicitly: SPASynth's notes record a trap where only the
# test binary got built and the installer step then found nothing to package.
# The artefacts are asserted below.
# Licensing is mandatory for release builds: the private module is checked
# out at the pinned commit and the configure FAILS if it is missing, so an
# unlicensed build can never be packaged by accident. The URL is the
# module's built-in production default (the configure refuses an override).
"$REPO_ROOT/scripts/fetch_spa_licensing.sh"
# Shared FX module (spa-fx) at its pinned commit; CMake uses libs/spa-fx unless
# SPA_FX_DIR is set in the environment.
"$REPO_ROOT/scripts/fetch_spa_fx.sh"
# Shared preset browser/manager (spa-presets) at its pinned commit; CMake uses
# libs/spa-presets unless SPA_PRESETS_DIR is set in the environment.
"$REPO_ROOT/scripts/fetch_spa_presets.sh"
cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSPASTRIP_UNIVERSAL_BINARY=ON -DSPASTRIP_COPY_PLUGIN=OFF -DSPASTRIP_BUILD_SNAPSHOTS=OFF \
      -DSPASTRIP_REQUIRE_LICENSING=ON \
      -DSPASTRIP_SPA_LICENSING_DIR="$REPO_ROOT/libs/spa-licensing" \
      -DSPASTRIP_LICENSING_API_URL=
cmake --build "$BUILD" --target SPAStrip_AU SPAStrip_VST3 SPAStripMIDI_AU SPAStripTests

ART="$BUILD/SPAStrip_artefacts/Release"
[[ -d "$ART/AU/SPAStrip.component" ]] || die "SPAStrip.component was not built"
[[ -d "$ART/VST3/SPAStrip.vst3" ]]    || die "SPAStrip.vst3 was not built"
[[ -d "$BUILD/SPAStripMIDI_artefacts/Release/AU/SPAStrip MIDI.component" ]] || die "SPAStrip MIDI.component was not built"
TESTS="$BUILD/SPAStripTests_artefacts/Release/SPAStripTests"
[[ -x "$TESTS" ]] || die "SPAStripTests was not built"

echo "=== running tests ==="
"$TESTS" || die "tests failed; not releasing"

# --- 3. macOS installer (signs here; notarization is the next step) --------------------------
mkdir -p "$DIST/installers"
"$REPO_ROOT/installers/macos/build_installer.sh" "$BUILD" "$DIST/installers"
PKG="$DIST/installers/SPAStrip-$VERSION-macOS.pkg"

# --- 4. Notarize ------------------------------------------------------------------------------
if [[ -n "${SPASTRIP_INSTALLER_IDENTITY:-}" ]]; then
    if "$REPO_ROOT/scripts/notarize.sh" "$PKG"; then
        echo "notarized: $PKG"
    else
        status=$?
        echo ""
        echo "WARNING: notarization failed (exit $status): $PKG is signed but NOT notarized/stapled."
        echo "Fix credentials (see scripts/notarize.sh), then:"
        echo "  scripts/notarize.sh '$PKG'"
        echo "  scripts/build_release.sh --stage-only $VERSION"
        echo "Skipping staging for now."
        exit 69
    fi
else
    echo "note: unsigned pkg, skipping notarization (set SPASTRIP_INSTALLER_IDENTITY to sign)"
fi

# --- 5. Stage + verify --------------------------------------------------------------------------
stage_shopify "$VERSION"
verify_release "$VERSION"

echo ""
echo "=== done ==="
echo "Folder: $DIST/shopify/SPAStrip-$VERSION"
if [[ ! -f "$DIST/shopify/SPAStrip-$VERSION/SPAStrip-$VERSION-Windows.exe" ]]; then
    echo "Next: push the commit, wait for the Windows CI job, then:"
    echo "  scripts/build_release.sh --stage-only $VERSION --with-windows"
fi
