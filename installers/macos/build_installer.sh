#!/bin/zsh
# Builds the macOS installer package for SPAStrip (AU + VST3, no Standalone).
#
#   ./installers/macos/build_installer.sh <build dir> <output dir>
#
# <build dir> is a CMake build tree containing SPAStrip_artefacts/Release
# (configure with -DCMAKE_BUILD_TYPE=Release -DSPASTRIP_UNIVERSAL_BINARY=ON and
# build SPAStrip_AU and SPAStrip_VST3; scripts/build_release.sh does all of it).
#
# Signing is opt-in via environment (leave unset for an unsigned pkg: fine for
# local testing, required unset on CI):
#   SPASTRIP_CODESIGN_IDENTITY   "Developer ID Application: Kenzora Games (7K9WY5T49S)"
#                                deep-signs the bundles with the hardened runtime
#                                and a secure timestamp (notarization requires both)
#   SPASTRIP_INSTALLER_IDENTITY  "Developer ID Installer: Kenzora Games (7K9WY5T49S)"
#                                signs the pkg with productsign
# Unset, the bundles are ad-hoc signed (enough to load locally) and the pkg is
# unsigned. Notarization is a separate step: scripts/notarize.sh <pkg>.
#
# Unlike SPASynth's script, this one never signs inside the build tree: it signs
# copies, so the build tree stays pristine and re-running is idempotent.

set -e -u -o pipefail

BUILD_DIR="$1"
OUT_DIR="$2"

REPO_ROOT="${0:A:h:h:h}"
VERSION=$(sed -n 's/^project(SPAStrip VERSION \([0-9.]*\).*/\1/p' "$REPO_ROOT/CMakeLists.txt")
[[ -n "$VERSION" ]] || { echo "error: could not read the version from CMakeLists.txt" >&2; exit 1; }
ARTEFACTS="$BUILD_DIR/SPAStrip_artefacts/Release"

[[ -d "$ARTEFACTS/VST3/SPAStrip.vst3" && -d "$ARTEFACTS/AU/SPAStrip.component" ]] \
    || { echo "error: no Release AU/VST3 artefacts in $BUILD_DIR (build SPAStrip_AU SPAStrip_VST3)" >&2; exit 1; }
mkdir -p "$OUT_DIR"
OUT_DIR="${OUT_DIR:A}"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/spastrip-installer.XXXXXX")
trap "rm -rf '$WORK'" EXIT
mkdir -p "$WORK/packages" "$WORK/resources" "$WORK/docs"

# --- Component packages -------------------------------------------------------
# Two identifier traps here (the same two SPASynth hit), both caused by every
# JUCE format sharing one CFBundleIdentifier unless it is split (SPAStrip's
# CMakeLists gives the AU and VST3 their own: ...spastrip.au / ...spastrip.vst3):
#  1. Each component MUST get a unique pkgbuild --identifier: if omitted it is
#     derived from the bundle id, and payloads that collide on one receipt make
#     Installer lay down only one of them.
#  2. Bundles MUST be marked non-relocatable: pkgbuild components are
#     relocatable by default, so if LaunchServices already knows the bundle id
#     anywhere else (a previous install, a moved copy, a dev build tree),
#     Installer "atomically shoves" the payload at that bundle instead of the
#     install-location. The receipt says installed but the plugin never lands.
#     Fixed by analyzing a component plist and forcing BundleIsRelocatable=false.
build_component_pkg() {
    local bundle="$1" identifier="$2" location="$3" out="$4"
    local stage="$WORK/stage-$identifier"
    mkdir -p "$stage"
    # ditto preserves signatures; --norsrc/--noextattr keep Finder and cloud
    # metadata out of the payload (it would break the code signature seal).
    ditto --norsrc --noextattr "$bundle" "$stage/${bundle:t}"

    if [[ -n "${SPASTRIP_CODESIGN_IDENTITY:-}" ]]; then
        echo "codesign: ${bundle:t}"
        codesign --force --deep --options runtime --timestamp \
                 --sign "$SPASTRIP_CODESIGN_IDENTITY" "$stage/${bundle:t}"
    else
        codesign --force --deep --sign - "$stage/${bundle:t}"
    fi
    codesign --verify --deep --strict "$stage/${bundle:t}"

    pkgbuild --analyze --root "$stage" "$WORK/$identifier.plist" > /dev/null
    # --analyze sometimes omits the key (implicit default: true) and sometimes
    # emits it; force it false on every bundle entry either way.
    python3 -c '
import plistlib, sys
path = sys.argv[1]
with open(path, "rb") as f: entries = plistlib.load(f)
assert entries, "pkgbuild --analyze found no bundles in " + path
for e in entries: e["BundleIsRelocatable"] = False
with open(path, "wb") as f: plistlib.dump(entries, f)
' "$WORK/$identifier.plist"
    pkgbuild --quiet --identifier "$identifier" --version "$VERSION" \
             --root "$stage" --component-plist "$WORK/$identifier.plist" \
             --install-location "$location" --ownership recommended \
             "$out"
}
build_component_pkg "$ARTEFACTS/VST3/SPAStrip.vst3" \
    "com.silverplatteraudio.spastrip.vst3" "/Library/Audio/Plug-Ins/VST3" \
    "$WORK/packages/SPAStripVST3.pkg"
build_component_pkg "$ARTEFACTS/AU/SPAStrip.component" \
    "com.silverplatteraudio.spastrip.au" "/Library/Audio/Plug-Ins/Components" \
    "$WORK/packages/SPAStripAU.pkg"

# --- Docs package -------------------------------------------------------------
# README, QUICKSTART, EULA and CREDITS.txt (the factory impulse response
# attributions; CC BY 4.0 items require credit in the distributed product).
# Not a bundle, so no component plist and nothing to mark non-relocatable.
DOCS_DIR="/Library/Application Support/Silverplatter Audio/SPAStrip"
"$REPO_ROOT/scripts/prepare_docs.sh" "$WORK/docs" "$VERSION"
mkdir -p "$WORK/stage-docs$DOCS_DIR"
for f in README.txt QUICKSTART.txt EULA.txt CREDITS.txt; do
    ditto --norsrc --noextattr "$WORK/docs/$f" "$WORK/stage-docs$DOCS_DIR/$f"
done
pkgbuild --quiet --identifier "com.silverplatteraudio.spastrip.docs" --version "$VERSION" \
         --root "$WORK/stage-docs" --install-location / --ownership recommended \
         "$WORK/packages/SPAStripDocs.pkg"

# --- Distribution (choices, licence pane, OS check) ---------------------------
cp "$WORK/docs/EULA.txt" "$WORK/resources/License.txt"

cat > "$WORK/resources/welcome.html" <<HTML
<html><head><meta charset="utf-8"></head>
<body style="font-family: -apple-system;">
<h2>SPAStrip $VERSION</h2>
<p>Silverplatter Audio</p>
<p>This installs the SPAStrip effect plug-in as an Audio Unit and a VST3.
Use Customize to choose formats.</p>
<p>Quit your DAW before continuing, then start it again afterwards.</p>
</body></html>
HTML

# Conclusion page: newsletter line.
cat > "$WORK/resources/conclusion.html" <<'HTML'
<html><head><meta charset="utf-8"></head>
<body style="font-family: -apple-system;">
<h2>SPAStrip is installed</h2>
<p>Quit and reopen your DAW to pick it up.</p>
<p>Stay in the loop: get the Silverplatter Audio newsletter at <a href="https://silverplatteraudio.com/pages/newsletter">silverplatteraudio.com/pages/newsletter</a></p>
</body></html>
HTML

cat > "$WORK/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>SPAStrip $VERSION</title>
    <welcome file="welcome.html"/>
    <license file="License.txt"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <options customize="always" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <installation-check script="hostsClosed()"/>
    <script><![CDATA[
    // A DAW that is open while the plug-in is replaced keeps (and re-caches) the
    // old component and the plug-in disappears from it until a manual rescan.
    function hostsClosed() {
        var hosts = [['com.apple.logic10', 'Logic Pro'], ['com.apple.mainstage3', 'MainStage'], ['com.apple.garageband10', 'GarageBand']];
        for (var i = 0; i < hosts.length; ++i) {
            if (system.applications.fromIdentifier(hosts[i][0])) {
                my.result.type = 'Fatal';
                my.result.title = 'Please quit ' + hosts[i][1];
                my.result.message = 'Quit ' + hosts[i][1] + ' before installing, then run this installer again.';
                return false;
            }
        }
        return true;
    }
    ]]></script>
    <volume-check>
        <allowed-os-versions><os-version min="11.0"/></allowed-os-versions>
    </volume-check>
    <domains enable_localSystem="true" enable_currentUserHome="false" enable_anywhere="false"/>
    <choices-outline>
        <line choice="au"/>
        <line choice="vst3"/>
        <line choice="docs"/>
    </choices-outline>
    <choice id="au" title="Audio Unit" description="For Logic Pro and other AU hosts."
            start_selected="true">
        <pkg-ref id="com.silverplatteraudio.spastrip.au"/>
    </choice>
    <choice id="vst3" title="VST3" description="For Ableton Live, Cubase, Reaper, FL Studio and other VST3 hosts."
            start_selected="true">
        <pkg-ref id="com.silverplatteraudio.spastrip.vst3"/>
    </choice>
    <choice id="docs" title="Documentation and credits (required)"
            description="README, quickstart, licence and the factory impulse response credits."
            start_selected="true" start_enabled="false">
        <pkg-ref id="com.silverplatteraudio.spastrip.docs"/>
    </choice>
    <pkg-ref id="com.silverplatteraudio.spastrip.au" version="$VERSION" onConclusion="none">SPAStripAU.pkg</pkg-ref>
    <pkg-ref id="com.silverplatteraudio.spastrip.vst3" version="$VERSION" onConclusion="none">SPAStripVST3.pkg</pkg-ref>
    <pkg-ref id="com.silverplatteraudio.spastrip.docs" version="$VERSION" onConclusion="none">SPAStripDocs.pkg</pkg-ref>
</installer-gui-script>
XML

UNSIGNED="$WORK/SPAStrip-$VERSION-macOS.pkg"
FINAL="$OUT_DIR/SPAStrip-$VERSION-macOS.pkg"

productbuild --quiet --distribution "$WORK/distribution.xml" \
             --package-path "$WORK/packages" --resources "$WORK/resources" \
             "$UNSIGNED"

# --- Optional installer signing (notarization is scripts/notarize.sh) ---------
rm -f "$FINAL"
if [[ -n "${SPASTRIP_INSTALLER_IDENTITY:-}" ]]; then
    productsign --sign "$SPASTRIP_INSTALLER_IDENTITY" "$UNSIGNED" "$FINAL"
    pkgutil --check-signature "$FINAL" | sed -n '1,4p'
else
    cp "$UNSIGNED" "$FINAL"
    echo "note: unsigned pkg (set SPASTRIP_INSTALLER_IDENTITY to sign)"
fi

echo "installer: $FINAL"
