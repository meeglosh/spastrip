# SPAStrip handoff

**2026-10-03: 1.0.1 WITH LICENSING SENT to Paul and Phil** (main 333e4f9; dist/shopify/SPAStrip-1.0.1/ staged mac+win; NFR serials issued to Mike/Paul/Phil). Bump to 1.0.2 for anything after this. Licensing pinned in libs/spa-licensing.pin; CI needs secret SPA_LICENSING_TOKEN. Cross-product state: ~/spasynth/handoff.md top section.

State as of 2026-10-02. Read this before starting work in a new session.

## What SPAStrip is

SPAStrip is Silverplatter Audio's channel-strip effect plugin: SPASynth's effects
chain lifted into a standalone AU (macOS) and VST3 (macOS + Windows) effect, so any
audio source can run through it. JUCE 8.0.14 (submodule `libs/JUCE`), CMake, C++20.
No Standalone, no AAX.

- Plugin codes: manufacturer `SpAu`, plugin `SpSt`, AU type `aufx`.
- Bundle ids: `com.silverplatteraudio.spastrip.au` / `.vst3`.
- Version: 1.0.0 (single source: `project(SPAStrip VERSION …)` in `CMakeLists.txt`).

## Current state

- **1.0.0 is cut and staged** in `dist/shopify/SPAStrip-1.0.0/` (not in git): EULA.txt,
  README.txt, QUICKSTART.txt, `SPAStrip-1.0.0-macOS.pkg` (signed, notarized, stapled),
  `SPAStrip-1.0.0-Windows.exe` (unsigned). Both built from `main` at `b901b76`.
  Checksums (md5): pkg `9c3e1864251163266f9a692850e0d66e`, exe `dbaa54d26b9aa99cbaed101356aca327`.
- 1.0.0 was **sent to the testers (Paul, Phil) on 2026-10-02**, so it is frozen. The
  version is now **1.0.1** in `CMakeLists.txt`; the next batch of work ships as 1.0.1.
  Version bump rule (from SPASynth): once a build has been sent to anyone, the next build
  gets a new version; an unsent build may be overwritten in place at the same version.
  The Inno script's `AppVersion` default ("1.0.0") is only a fallback; CI passes the
  CMake version.
- **Branches:** `main` and `engine` contain identical files. GitHub repo
  `meeglosh/spastrip` is PRIVATE. A local-only branch `backup/engine-pre-purge` holds the
  pre-purge history (it contains an accidentally committed 18 MB `ui-snapshots/` folder);
  delete it when no longer wanted. `main` was joined to the rewritten `engine` with
  `--allow-unrelated-histories` after the purge rewrote the root commit.

## Open items

1. **EULA legal review** before sale (owner arranging). Especially clause 3 / 13: six of
   the 19 factory impulse responses are CC BY 4.0, so the "no redistribution of factory
   content" limit is written not to restrict them beyond their own licences.
2. **Licensing**: merged to `main` (2026-10-03), not in any build yet (1.0.0 has none).
   14-day trial then demo (silence ~every 60 s, no preset save/export), 3 machines,
   module `spa-licensing` pinned in `libs/spa-licensing.pin` (gitignored checkout via
   `scripts/fetch_spa_licensing.sh`). `build_release.sh` and CI configure with
   `SPASTRIP_REQUIRE_LICENSING=ON` and fail without the module; CI needs the repo secret
   `SPA_LICENSING_TOKEN` (read-only PAT for meeglosh/spa-licensing). EULA clause 6 and
   README.txt describe it.
3. **Windows code signing**: certificate coming. The Inno script keeps a `/DSignToolCmd`
   hook (see header of `installers/windows/SPAStrip.iss`). Until signed, users see a
   SmartScreen warning.
4. **Never run on a real Windows machine yet** (CI builds and tests only).
5. **Factory presets**: none ship. The preset manager has a read-only Factory seam ready
   for content; types are Drums, Bass, Vocals, Guitar, Keys, Synth, FX, Mixbus, Mastering,
   Creative.
6. **Known limits**:
   - 4x oversampling with long IRs and host buffers of 128 samples or less: about 2-6% of
     blocks can still run late (JUCE's convolution does the whole tail in one call). Fine
     at 1x/2x and at normal buffer sizes. Real fix = a time-distributed convolver.
   - Three Hopkins IRs (Dark Plate, Studio B, Concrete Tunnel) end on a steady noise floor
     rather than decaying; owner to judge by ear.
   - A hot FILTER (res 1, drive 1, 24 dB) can peak around +36 dBFS by design.
   - Filter response display shows knob values, not the live modulated cutoff.
   - EQ band and Comp crossover handles show no on-knob modulation arc.
   - Popup menus don't scale with the editor (same as SPASynth).
   - Cosmetic: Comp tab control cluster floats in empty space; Grain "Spread" labels sit close.
7. **Possible future IRs** (permission needed before use): Greg Hopkins' full EMT 140 set
   (unclear licence on mirrors; message drafted, owner to send) and EchoThief (needs Chris
   Warren's written permission, chris@superhoax.com). OpenAIR sites were down. Store any
   written permission under `assets/irs/licences/`.

## Architecture map

- `source/dsp/` — engines copied **verbatim** from SPASynth (StereoChorus, ModEffect,
  TremVib, GrainFX, PlateReverb, ParametricEQ, Multiband, Limiter, MultiModeFilter) plus
  `FXChain.{h,cpp}`. Rule: do not change their maths. Additions are marked
  `// SPAStripAdded`. Verify with `diff` against `/Users/mikejerugim/spasynth/source/dsp/`.
- Chain: 12 modules, append-only `Module` enum, order packed 4 bits/module in a uint64
  (`fxOrder` state property). Default order: filter, dist, chorus, mod, trem/vib, grain,
  delay, reverb, conv, eq, comp, limiter. `unpackOrder` migrates 9- and 11-module saves.
- `source/params/` — registry (IDs/ranges/defaults/RandomSpecs identical to SPASynth for
  the ported effects; filter uses new `fxFilter.*` ids), randomizer (ported; per-module
  locks in `fxLockMask`, filter silence guards).
- `source/mod/` — sidechain detector + 8-slot mod matrix (93+ targets; six excluded for
  clicks/reloads, reasons in `ModTargets.cpp`). Refresh every 8 host samples, 4 ms slew;
  plain path when depth x envelope is negligible.
- `source/ir/` — 19 factory IRs embedded from `assets/irs/processed/` via
  `assets/irs/manifest.json` (CMake configure-time table). `assets/irs/original/` is
  gitignored (source downloads, sha256 in the manifest). Credits: `assets/irs/CREDITS.md`.
- `source/presets/` — `.spastrip` presets in
  `<userAppData>/Silverplatter Audio/SPAStrip/Presets/User/…`, typed, SPASynth-style
  browser. No SPASynth import (removed by owner's decision).
- `source/ui/` — editor ported from SPASynth's look (Theme, LookAndFeel, panels).
  Base 1180x740, 50-200% scaling.
- `tests/` — headless `SPAStripTests` (114 test functions). Extras:
  `--bench-cpu`, `--audit-mod`, `--ui-only`, `--filter-only`.
- `tools/SPAStripSnapshots.cpp` — renders UI PNGs. Always pass an output dir outside the
  repo (`ui-snapshots/` is gitignored but don't rely on it).

## Build, test, release

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/SPAStripTests_artefacts/Release/SPAStripTests
auval -v aufx SpSt SpAu
/Applications/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 build/SPAStrip_artefacts/Release/VST3/SPAStrip.vst3
```

Release (from a clean, committed tree; the script refuses otherwise and refuses to sign
while any `TODO-REVIEW` remains in `packaging/docs/`):

```
export CMAKE_BUILD_PARALLEL_LEVEL=2
export SPASTRIP_CODESIGN_IDENTITY="Developer ID Application: Kenzora Games (7K9WY5T49S)"
export SPASTRIP_INSTALLER_IDENTITY="Developer ID Installer: Kenzora Games (7K9WY5T49S)"
export SPASYNTH_NOTARIZE_PROFILE="SPASYNTH_NOTARY"   # fallback; ~/.config/spasynth/notary.env is used first
./scripts/build_release.sh
# push, wait for the Windows CI run on that commit, then:
./scripts/build_release.sh --stage-only <version> --with-windows
```

- Notarization reuses SPASynth's credentials file (owner's decision). macOS minimum 11.0.
- The release build deletes dev copies in `~/Library/Audio/Plug-Ins` so they can't shadow
  the installed release in Logic.
- CI (`.github/workflows/build.yml`): Windows job on every push (build, tests, Inno Setup,
  installer published to a draft release `ci-windows-<sha7>`, last 5 kept). macOS job is
  manual-only (10x minutes on a private repo). Don't push again before fetching the exe
  for the commit being released: a new push cancels the in-progress run.

## Working notes

- The owner wants Claude to do all git/GitHub work (commit, push, CI). Auto mode's
  classifier has blocked some pushes and history rewrites; when blocked, give the owner
  the exact `!` command instead of working around it.
- Do not start a git history rewrite while an agent has uncommitted work in the tree
  (a `filter-repo` reset wiped uncommitted UI edits once).
- Release order of the folder: EULA.txt, README.txt, QUICKSTART.txt, pkg, exe. No Library
  folder (owner's decision).

## Launch checklist additions

- Newsletter page: when this product is announced, add it to an "Our tools" list on silverplatteraudio.com/pages/newsletter (Shopify → Online Store → Pages → "Silverplatter Audio newsletter"; the list was removed 2026-10-04 so the page didn't preview unreleased products, so recreate the heading if it's the first). Approved one-liner — SPAStrip: The Silverplatter Audio effects chain, in a single plugin.

## 2026-10-04: Phil's feedback round -> 1.0.2 (sent for testing)

- EQ: curve and nodes now drawn from modulated values (`EqEditor::liveBand`).
- Tooltips on every parameter control from one table: `source/ui/ParamTooltips.h`
  (Knob / Choice / Toggle / SectionPanel pick it up automatically).
- COMP rebuilt (`source/ui/CompPanel.h`): spectrum behind bands, drag band = threshold,
  drag line = crossover, wheel = down ratio, transfer curve, IN/GR/OUT meters, per-band
  Knee / Solo / Bypass (new params in their own host group "FX Comp Bands", appended
  last so no host index moves; knee 0 = original hard knee, still bit-identical to
  SPAGlitch). Display names: Threshold, Down Ratio, Up Ratio, Makeup, Low/High Crossover.
- GLITTER: RELEASE knob replaces FREEZE (SPASynth 1.0.31's GrainFX, verbatim). FREEZE stays
  registered but hidden; freeze=on states load as RELEASE infinite. RELEASE is excluded
  from mod targets. `tests/reference/GrainFX_1_0_2.h` is the pre-RELEASE GrainFX for the
  RELEASE-off identity test.
- 1.0.2 was rebuilt with RELEASE before going out (commit 10e122a). Tester note:
  `docs/tester-note-1.0.2.txt`.
- Pending: Phil's sign-off on the new COMP, then port DSP (`Multiband.h`) + panel to
  SPASynth and SPAGlitch (owner's decision: finish here first, then copy).
