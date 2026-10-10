cmake_minimum_required(VERSION 3.24)

# Minimum macOS we support. MUST be set before project() so it is baked into
# every binary's LC_BUILD_VERSION (otherwise builds inherit the BUILD MACHINE's
# OS). 11.0 is also the floor for arm64. Ignored on non-Apple platforms.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)

project(SPAStrip VERSION 1.0.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_XCODE_GENERATE_SCHEME OFF)

# Short git commit + build date, resolved at CONFIGURE time (a CI checkout with
# no .git / no git binary still configures cleanly and just shows "unknown").
find_package(Git QUIET)
set(SPASTRIP_GIT_COMMIT "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE SPASTRIP_GIT_COMMIT_OUT
        RESULT_VARIABLE SPASTRIP_GIT_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(SPASTRIP_GIT_RESULT EQUAL 0 AND NOT SPASTRIP_GIT_COMMIT_OUT STREQUAL "")
        set(SPASTRIP_GIT_COMMIT "${SPASTRIP_GIT_COMMIT_OUT}")
    endif()
endif()
string(TIMESTAMP SPASTRIP_BUILD_DATE "%Y-%m-%d" UTC)

# Universal binary is required for release; keep it opt-in for day-to-day
# development because it doubles compile time.
option(SPASTRIP_UNIVERSAL_BINARY "Build macOS universal binary (arm64 + x86_64)" OFF)

# Copy built plugins into the system plugin folders (handy locally, breaks on
# CI runners without admin rights -- CI passes OFF).
option(SPASTRIP_COPY_PLUGIN "Copy plugins to system folders after build" ON)
if(APPLE AND SPASTRIP_UNIVERSAL_BINARY)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

add_subdirectory(libs/JUCE)

# AU + VST3 on macOS, VST3 on Windows. No Standalone, no AAX.
if(APPLE)
    set(SPASTRIP_FORMATS AU VST3)
else()
    set(SPASTRIP_FORMATS VST3)
endif()

juce_add_plugin(SPAStrip
    COMPANY_NAME "Silverplatter Audio"
    COMPANY_WEBSITE "https://www.silverplatteraudio.com"
    BUNDLE_ID "com.silverplatteraudio.spastrip"
    PLUGIN_MANUFACTURER_CODE SpAu
    PLUGIN_CODE SpSt
    FORMATS ${SPASTRIP_FORMATS}
    PRODUCT_NAME "SPAStrip"
    IS_SYNTH FALSE
    # MIDI in for MIDI Learn. Only the VST3 ever gets MIDI from this target: Logic
    # never sends MIDI to a kAudioUnitType_Effect AU (and the editor offers no
    # MIDI Learn there, see SPAStripProcessor::supportsMidiLearn).
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${SPASTRIP_COPY_PLUGIN}
    VST3_CATEGORIES Fx
    AU_MAIN_TYPE kAudioUnitType_Effect)

# "SPAStrip MIDI": the same plugin as a second AU of type kAudioUnitType_MusicEffect
# ('aumf'), which Logic CAN send MIDI to (choose it under Audio Units > MIDI-controlled
# Effects, then pick a MIDI source in the plugin header's side-chain MIDI menu).
# Its own subtype, so it never replaces the plain AU in existing sessions. macOS only.
if(APPLE)
    juce_add_plugin(SPAStripMIDI
        COMPANY_NAME "Silverplatter Audio"
        COMPANY_WEBSITE "https://www.silverplatteraudio.com"
        BUNDLE_ID "com.silverplatteraudio.spastripmidi"
        PLUGIN_MANUFACTURER_CODE SpAu
        PLUGIN_CODE SpSm
        FORMATS AU
        PRODUCT_NAME "SPAStrip MIDI"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        COPY_PLUGIN_AFTER_BUILD ${SPASTRIP_COPY_PLUGIN}
        AU_MAIN_TYPE kAudioUnitType_MusicEffect)
endif()

# Give the AU and VST3 bundles their own CFBundleIdentifier. JUCE stamps one
# shared BUNDLE_ID into every format, but macOS Installer tracks pkg payload
# bundles BY bundle id: payloads sharing one id collide in PackageKit's upgrade
# bookkeeping and an upgrade install can silently delete the AU right after
# laying it down. Hosts identify the AU by type/subtype/manufacturer and the
# VST3 by class UUID, so existing sessions are unaffected. JUCE generates each
# format's Info.plist at configure time (juceaide, inside juce_add_plugin), so
# patching here is deterministic.
if(APPLE)
    get_target_property(_spastrip_gen_dir SPAStrip JUCE_GENERATED_SOURCES_DIRECTORY)
    foreach(_fmt_suffix "AU;au" "VST3;vst3")
        list(GET _fmt_suffix 0 _fmt)
        list(GET _fmt_suffix 1 _suffix)
        execute_process(
            COMMAND /usr/libexec/PlistBuddy
                    -c "Set :CFBundleIdentifier com.silverplatteraudio.spastrip.${_suffix}"
                    "${_spastrip_gen_dir}/SPAStrip_${_fmt}/Info.plist"
            RESULT_VARIABLE _plist_result)
        if(NOT _plist_result EQUAL 0)
            message(FATAL_ERROR "Failed to set per-format bundle id for SPAStrip_${_fmt}")
        endif()
    endforeach()
    get_target_property(_spastrip_midi_gen_dir SPAStripMIDI JUCE_GENERATED_SOURCES_DIRECTORY)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy
                -c "Set :CFBundleIdentifier com.silverplatteraudio.spastripmidi.au"
                "${_spastrip_midi_gen_dir}/SPAStripMIDI_AU/Info.plist"
        RESULT_VARIABLE _plist_result)
    if(NOT _plist_result EQUAL 0)
        message(FATAL_ERROR "Failed to set the bundle id for SPAStripMIDI_AU")
    endif()
endif()

# ---- Factory impulse responses ------------------------------------------------
# Embedded with juce_add_binary_data, DRIVEN BY assets/irs/manifest.json at
# CONFIGURE time: every entry with role "representative" (and only those -- the
# "alternate" entries are not shipped) contributes its processed FLAC, and a
# row of the generated table below. Generating the table here (rather than
# checking in a hand-kept copy) means the shipped list cannot drift from the
# manifest; a re-configure is triggered whenever the manifest changes, and
# tests/SPAStripTests.cpp re-reads the manifest at run time and asserts the
# embedded list matches it (so a stale build dir is caught too).
set(SPASTRIP_IR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/irs)
set(SPASTRIP_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SPASTRIP_IR_DIR}/manifest.json)
file(READ ${SPASTRIP_IR_DIR}/manifest.json SPASTRIP_IR_MANIFEST)
string(JSON SPASTRIP_IR_COUNT LENGTH "${SPASTRIP_IR_MANIFEST}" irs)
set(SPASTRIP_IR_ROWS "")
set(SPASTRIP_IR_FILES "")
math(EXPR SPASTRIP_IR_LAST "${SPASTRIP_IR_COUNT} - 1")
foreach(_i RANGE ${SPASTRIP_IR_LAST})
    string(JSON _role GET "${SPASTRIP_IR_MANIFEST}" irs ${_i} role)
    if(NOT _role STREQUAL "representative")
        continue()
    endif()
    string(JSON _id   GET "${SPASTRIP_IR_MANIFEST}" irs ${_i} id)
    string(JSON _name GET "${SPASTRIP_IR_MANIFEST}" irs ${_i} display_name)
    string(JSON _cat  GET "${SPASTRIP_IR_MANIFEST}" irs ${_i} category)
    string(JSON _file GET "${SPASTRIP_IR_MANIFEST}" irs ${_i} processed file)
    if(NOT EXISTS ${SPASTRIP_IR_DIR}/${_file})
        message(FATAL_ERROR "Factory IR '${_id}': ${SPASTRIP_IR_DIR}/${_file} is missing")
    endif()
    get_filename_component(_base ${_file} NAME)
    foreach(_v _id _name _cat _base)
        string(REPLACE "\\" "\\\\" ${_v} "${${_v}}")
        string(REPLACE "\"" "\\\"" ${_v} "${${_v}}")
    endforeach()
    string(APPEND SPASTRIP_IR_ROWS "{ \"${_id}\", \"${_name}\", \"${_cat}\", \"${_base}\" },\n")
    list(APPEND SPASTRIP_IR_FILES ${SPASTRIP_IR_DIR}/${_file})
endforeach()
file(WRITE ${SPASTRIP_GENERATED_DIR}/FactoryIRTable.inc.tmp "${SPASTRIP_IR_ROWS}")
configure_file(${SPASTRIP_GENERATED_DIR}/FactoryIRTable.inc.tmp ${SPASTRIP_GENERATED_DIR}/FactoryIRTable.inc COPYONLY)

juce_add_binary_data(SPAStripFactoryData
    HEADER_NAME SPAStripFactoryData.h
    NAMESPACE spa_factory_data
    SOURCES ${SPASTRIP_IR_FILES} ${SPASTRIP_IR_DIR}/CREDITS.md)
# Position-independent so the static data library links into the plugin bundles.
set_target_properties(SPAStripFactoryData PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Brand logo (the SPAudio ring-and-disc mark, white), embedded for the editor.
juce_add_binary_data(SPAStripBranding
    HEADER_NAME SPAStripBrandingData.h
    NAMESPACE spa_brand
    SOURCES assets/branding/SPAudio_logo_white.svg)
set_target_properties(SPAStripBranding PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Shared FX module (spa-fx, namespace spa::fx). Checked out (gitignored) at
# libs/spa-fx at the commit pinned in libs/spa-fx.pin by scripts/fetch_spa_fx.sh
# (locally by build_release.sh, in CI by actions/checkout). To build against a
# local checkout such as ~/spa-fx instead, set SPA_FX_DIR in the environment or
# -DSPASTRIP_SPA_FX_DIR=... .
set(_spa_fx_default "${CMAKE_CURRENT_SOURCE_DIR}/libs/spa-fx")
if(DEFINED ENV{SPA_FX_DIR} AND NOT "$ENV{SPA_FX_DIR}" STREQUAL "")
    set(_spa_fx_default "$ENV{SPA_FX_DIR}")
endif()
set(SPASTRIP_SPA_FX_DIR "${_spa_fx_default}" CACHE PATH "Path to a spa-fx checkout")
if(NOT EXISTS "${SPASTRIP_SPA_FX_DIR}/spa_fx/PlateReverb.h")
    message(FATAL_ERROR "spa-fx module not found at '${SPASTRIP_SPA_FX_DIR}'. Run scripts/fetch_spa_fx.sh "
        "or set SPA_FX_DIR / -DSPASTRIP_SPA_FX_DIR to a spa-fx checkout (e.g. ~/spa-fx).")
endif()
message(STATUS "SPAStrip spa-fx: ${SPASTRIP_SPA_FX_DIR}")

# Shared preset browser + manager (spa-presets, namespace spa::presets): checked out
# (gitignored) at libs/spa-presets at the commit pinned in libs/spa-presets.pin by
# scripts/fetch_spa_presets.sh (locally by build_release.sh, in CI by actions/checkout).
# Unlike spa-fx it has two .cpp files, built into every target below. To build against a
# local checkout such as ~/spa-presets, set SPA_PRESETS_DIR in the environment or
# -DSPASTRIP_SPA_PRESETS_DIR=... .
set(_spa_presets_default "${CMAKE_CURRENT_SOURCE_DIR}/libs/spa-presets")
if(DEFINED ENV{SPA_PRESETS_DIR} AND NOT "$ENV{SPA_PRESETS_DIR}" STREQUAL "")
    set(_spa_presets_default "$ENV{SPA_PRESETS_DIR}")
endif()
set(SPASTRIP_SPA_PRESETS_DIR "${_spa_presets_default}" CACHE PATH "Path to a spa-presets checkout")
if(NOT EXISTS "${SPASTRIP_SPA_PRESETS_DIR}/spa_presets/PresetManager.h")
    message(FATAL_ERROR "spa-presets module not found at '${SPASTRIP_SPA_PRESETS_DIR}'. Run scripts/fetch_spa_presets.sh "
        "or set SPA_PRESETS_DIR / -DSPASTRIP_SPA_PRESETS_DIR to a spa-presets checkout (e.g. ~/spa-presets).")
endif()
message(STATUS "SPAStrip spa-presets: ${SPASTRIP_SPA_PRESETS_DIR}")

set(SPASTRIP_SOURCES
    ${SPASTRIP_SPA_PRESETS_DIR}/spa_presets/PresetManager.cpp
    ${SPASTRIP_SPA_PRESETS_DIR}/spa_presets/PresetBrowser.cpp
    source/SPAStripProcessor.cpp
    source/SPAStripProcessorEdit.cpp
    source/MidiLearn.cpp
    source/params/Randomizer.cpp
    source/presets/PresetManager.cpp
    source/params/ParameterRegistry.cpp
    source/mod/ModTargets.cpp
    source/ir/FactoryIRs.cpp
    source/dsp/FXChain.cpp
    source/ui/ConvolvePanel.cpp
    source/ui/Dialogs.cpp
    source/ui/Displays.cpp
    source/ui/FxPanel.cpp
    source/ui/ModViz.cpp
    source/ui/PresetBrowser.cpp
    source/ui/SectionPanel.cpp
    source/ui/SPAStripEditor.cpp
    source/ui/SPAStripLookAndFeel.cpp
    source/ui/StripPanels.cpp
    source/ui/UiSettings.cpp)

set(SPASTRIP_PLUGIN_TARGETS SPAStrip)
if(APPLE)
    list(APPEND SPASTRIP_PLUGIN_TARGETS SPAStripMIDI)
endif()

foreach(_plugin ${SPASTRIP_PLUGIN_TARGETS})
target_sources(${_plugin} PRIVATE ${SPASTRIP_SOURCES})

target_include_directories(${_plugin} PRIVATE source ${SPASTRIP_GENERATED_DIR} "${SPASTRIP_SPA_FX_DIR}" "${SPASTRIP_SPA_PRESETS_DIR}")

target_compile_definitions(${_plugin} PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    SPASTRIP_VERSION="${PROJECT_VERSION}"
    SPASTRIP_GIT_COMMIT="${SPASTRIP_GIT_COMMIT}"
    SPASTRIP_BUILD_DATE="${SPASTRIP_BUILD_DATE}")

target_link_libraries(${_plugin}
    PRIVATE
        SPAStripFactoryData
        SPAStripBranding
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
endforeach()
target_compile_definitions(SPAStrip PRIVATE SPASTRIP_AU_MIDI=0)
if(APPLE)
    target_compile_definitions(SPAStripMIDI PRIVATE SPASTRIP_AU_MIDI=1)
endif()

# Licensing. The module lives in the PRIVATE repo meeglosh/spa-licensing,
# checked out (gitignored) at libs/spa-licensing at the commit pinned in
# libs/spa-licensing.pin -- by scripts/build_release.sh locally and by
# actions/checkout in CI, so both build the same module. It is NOT a git
# submodule. Release builds pass -DSPASTRIP_REQUIRE_LICENSING=ON and fail if
# the module is missing (never ship unlicensed by accident); dev builds without
# the checkout build exactly as before, with no licence checks.
# SPAStripSnapshots is deliberately left without it (it would start a real
# trial on the machine that renders screenshots).
set(_spa_lic_default "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/spa-licensing/spa_licensing/spa_licensing.h")
    set(_spa_lic_default "${CMAKE_CURRENT_SOURCE_DIR}/libs/spa-licensing")
endif()
set(SPASTRIP_SPA_LICENSING_DIR "${_spa_lic_default}" CACHE PATH "Path to a spa-licensing checkout (empty = unlicensed dev build)")
option(SPASTRIP_REQUIRE_LICENSING "Fail the configure if the licensing module is missing (release builds)" OFF)
# Licensing API base URL: empty = the module's built-in production default.
# (Deliberately not spelled out here: a test forbids the SPASynth preset
# extension anywhere in this file, and the production host name contains it.)
set(SPASTRIP_LICENSING_API_URL "" CACHE STRING "Licensing API base URL (empty = module's production default)")
if(SPASTRIP_REQUIRE_LICENSING)
    if(NOT SPASTRIP_SPA_LICENSING_DIR OR NOT EXISTS "${SPASTRIP_SPA_LICENSING_DIR}/spa_licensing/spa_licensing.h")
        message(FATAL_ERROR "SPASTRIP_REQUIRE_LICENSING=ON but the spa-licensing module was not found "
            "(SPASTRIP_SPA_LICENSING_DIR='${SPASTRIP_SPA_LICENSING_DIR}'). Check it out at libs/spa-licensing "
            "at the commit in libs/spa-licensing.pin.")
    endif()
    if(SPASTRIP_LICENSING_API_URL)
        message(FATAL_ERROR "Release builds must use the module's built-in production licensing URL: "
            "leave SPASTRIP_LICENSING_API_URL empty (it is '${SPASTRIP_LICENSING_API_URL}')")
    endif()
endif()
set(_spastrip_lic_url_def "")
if(SPASTRIP_LICENSING_API_URL)
    set(_spastrip_lic_url_def SPA_LICENSING_API_BASE_URL="${SPASTRIP_LICENSING_API_URL}")
endif()
if(SPASTRIP_SPA_LICENSING_DIR)
    if(SPASTRIP_LICENSING_API_URL)
        message(STATUS "SPAStrip licensing: ON (${SPASTRIP_SPA_LICENSING_DIR}, ${SPASTRIP_LICENSING_API_URL})")
    else()
        message(STATUS "SPAStrip licensing: ON (${SPASTRIP_SPA_LICENSING_DIR}, module default production URL)")
    endif()
else()
    message(STATUS "SPAStrip licensing: OFF (dev build, no licence checks)")
endif()
if(SPASTRIP_SPA_LICENSING_DIR)
    juce_add_module(${SPASTRIP_SPA_LICENSING_DIR}/spa_licensing)
    foreach(_plugin ${SPASTRIP_PLUGIN_TARGETS})
        target_link_libraries(${_plugin} PRIVATE spa_licensing)
        target_compile_definitions(${_plugin} PRIVATE SPASTRIP_HAS_SPA_LICENSING=1 ${_spastrip_lic_url_def})
    endforeach()
endif()

# Headless test target: compiles the same sources as the plugin (JUCE modules
# are INTERFACE targets, so a console app is the supported way to reuse them).
option(SPASTRIP_BUILD_TESTS "Build headless DSP/processor tests" ON)
if(SPASTRIP_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(SPAStripTests PRODUCT_NAME "SPAStripTests")
    target_sources(SPAStripTests PRIVATE tests/SPAStripTests.cpp ${SPASTRIP_SOURCES})
    target_include_directories(SPAStripTests PRIVATE source ${SPASTRIP_GENERATED_DIR} "${SPASTRIP_SPA_FX_DIR}" "${SPASTRIP_SPA_PRESETS_DIR}")
    target_compile_definitions(SPAStripTests PRIVATE
        SPASTRIP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SPASTRIP_MOD_AUDIT=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        SPASTRIP_VERSION="${PROJECT_VERSION}"
        SPASTRIP_GIT_COMMIT="${SPASTRIP_GIT_COMMIT}"
        SPASTRIP_BUILD_DATE="${SPASTRIP_BUILD_DATE}")
    target_link_libraries(SPAStripTests
        PRIVATE
            SPAStripFactoryData
            SPAStripBranding
            juce::juce_audio_utils
            juce::juce_cryptography   # tests only: SHA-256 of the embedded FLACs vs the manifest
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    if(SPASTRIP_SPA_LICENSING_DIR)
        # SPA_LICENSING_TESTS: this target (only) trusts the committed TEST keys.
        # The suite installs a temp licensing root, a fake machine hash and a
        # counting fake transport at start-up: nothing real is read/written/sent.
        target_link_libraries(SPAStripTests PRIVATE spa_licensing)
        target_compile_definitions(SPAStripTests PRIVATE SPASTRIP_HAS_SPA_LICENSING=1 SPA_LICENSING_TESTS=1
            ${_spastrip_lic_url_def})
    endif()

    add_test(NAME SPAStripTests COMMAND SPAStripTests)
endif()

# Headless screenshot tool: constructs the processor + editor off-screen, feeds test
# audio and writes PNGs of the editor (see tools/SPAStripSnapshots.cpp).
option(SPASTRIP_BUILD_SNAPSHOTS "Build the editor screenshot tool" ON)
if(SPASTRIP_BUILD_SNAPSHOTS)
    juce_add_console_app(SPAStripSnapshots PRODUCT_NAME "SPAStripSnapshots")
    target_sources(SPAStripSnapshots PRIVATE tools/SPAStripSnapshots.cpp ${SPASTRIP_SOURCES})
    target_include_directories(SPAStripSnapshots PRIVATE source ${SPASTRIP_GENERATED_DIR} "${SPASTRIP_SPA_FX_DIR}" "${SPASTRIP_SPA_PRESETS_DIR}")
    target_compile_definitions(SPAStripSnapshots PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        SPASTRIP_VERSION="${PROJECT_VERSION}"
        SPASTRIP_GIT_COMMIT="${SPASTRIP_GIT_COMMIT}"
        SPASTRIP_BUILD_DATE="${SPASTRIP_BUILD_DATE}")
    target_link_libraries(SPAStripSnapshots
        PRIVATE
            SPAStripFactoryData
            SPAStripBranding
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
