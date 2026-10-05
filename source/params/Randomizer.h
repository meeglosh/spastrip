#pragma once

#include <array>
#include <optional>

#include "ParameterRegistry.h"
#include "../dsp/FXChain.h"

namespace spa::params
{

// RANDOMIZE ALL for SPAStrip (ported from SPASynth's params/Randomizer.*, FX
// parts only).
//
// Locks: one per chain module (the 12 dsp::FXChain::Module values; the TREM/VIB
// module covers both the fxTrem.* and fxVib.* parameters). A lock mask is a
// bitmask keyed by the Module enum value: bit i = Module i is locked. A locked
// module keeps every one of its parameters (its enable toggle included) AND its
// slot in the chain order.
using Module = dsp::FXChain::Module;

inline constexpr int numLockModules = dsp::FXChain::numModules;
inline constexpr juce::uint32 allLocksMask = (1u << numLockModules) - 1u;
inline juce::uint32 lockBit (Module m) { return 1u << (int) m; }

// The chain module a parameter section belongs to; nullopt for global /
// sidechain / mod-matrix sections (never randomized).
std::optional<Module> moduleForSection (Section);
// Short UI label for a module's lock ("DIST", "TREM/VIB", ...).
juce::String moduleName (Module);

// FILTER roll guards. The thresholds are tuned against the pink-noise
// measurement in filterRollGuardTest (tests/Phase2bTests.inc).
namespace filterguard
{
    inline constexpr float lowpassMinHz = 300.0f;            // LP cutoff below this is folded up
    inline constexpr float highpassMaxHz = 1500.0f;          // HP cutoff above this is folded down
    inline constexpr float bandMinHz = 150.0f;               // band-pass / notch centre range
    inline constexpr float bandMaxHz = 6000.0f;
    inline constexpr float bandpassMaxMix = 0.7f;            // a band-pass always leaves 30% dry
    inline constexpr float driveResonanceThreshold = 0.4f;   // above this drive ...
    inline constexpr float resonanceCapAtHighDrive = 0.5f;   // ... resonance is capped here
    inline constexpr float seriesOverlapOctaves = 1.5f;      // series passbands must overlap this much
    inline constexpr float bandpassHalfWidthOctaves = 1.0f;  // a band-pass is treated as centre /2 .. x2
}

// The filter module's rolled settings in real units, for the guard rules.
struct FilterSettings
{
    struct One { bool enabled = false; int type = 0; float cutoffHz = 20000.0f;
                 float resonance = 0.0f, drive = 0.0f, mix = 1.0f; };
    One f1, f2;
    bool parallel = false;
};
// Pure: bends `s` to satisfy the rules above. Draws no random numbers; idempotent.
void guardFilterSettings (FilterSettings& s);
// Reads the FILTER parameters, applies guardFilterSettings and writes back what changed.
void applyFilterGuards (juce::AudioProcessorValueTreeState&);

// Samples a normalized value from a RandomSpec. wildness 0 = tight around the
// musical centre, 0.5 = the spec's constrained window with its bias, 1 =
// full-range uniform. IDENTICAL maths to SPASynth's sampleRandomValue.
float sampleRandomValue (const RandomSpec&, float wildness, juce::Random&);

// The FX chain order after a Randomize All: Fisher-Yates over the modules that
// are neither locked nor the limiter, written back into the slots those modules
// occupied. Locked modules and the limiter keep their exact slot index. With no
// locks this is bit-for-bit SPASynth's "shuffle everything but the limiter".
// `order` holds Module enum values per slot; anything that is not a valid
// permutation is returned unchanged.
juce::Array<int> shuffleFxOrder (const juce::Array<int>& order, juce::uint32 lockMask, juce::Random&);

// Rolls every unlocked module (message thread). Every parameter write is a
// begin-gesture / setValueNotifyingHost / end-gesture, so hosts record it, and
// the caller wraps the whole thing in one undo step.
//
//  1. Each parameter whose RandomSpec is enabled, in registry order, one
//     rng.nextFloat() each (locked modules draw nothing).
//  2. The chain order (shuffleFxOrder), returned through `fxOrder`; the caller
//     stores it.
//  3. Limiter safety (only when the limiter is unlocked): forced ON with every
//     limiter parameter at its default. The limiter's rolled values are drawn
//     (so the RNG sequence matches the synth) but never written, since step 3
//     overwrites all ten; the end state is identical to rolling then resetting.
//  4. Convolve start hard-capped at 0.5 (only when convolve is unlocked).
//  5. Grain FREEZE (never rolled) forced off (only when grain is unlocked).
//  7. Reverb lo-cut guard (SPASynth 1.0.32; only when the reverb is unlocked):
//     reverb on, LoCut above 250 Hz and MIX above 50 % -> MIX 50 %.
//  6. FILTER guards (applyFilterGuards; only when the filter is unlocked and
//     `guardFilter` is true -- the flag exists so a test can measure the
//     unguarded roll): deterministic, they consume no random numbers.
//
// Never touched: global.*, sc.*, mod.slotN.depth, mod slot targets, the IR
// (the mod slots are rolled separately, by rollModSlots).
void randomizeAll (juce::AudioProcessorValueTreeState&, float wildness, juce::uint32 lockMask,
                   juce::Array<int>& fxOrder, juce::Random&, bool guardFilter = true);

// RANDOMIZE ALL's modulation roll (SPASynth rolls its matrix routes the same
// way; here the source is always the sidechain envelope). Call AFTER
// randomizeAll so the effects' rolled state is what decides the candidates,
// and so every draw randomizeAll makes stays where it was.
//
//  * How many slots: lo..lo+2 (at most 8), lo = 1 + round(5 x wildness), so
//    1-3 at WILD 0 and 6-8 at full WILD; the rest are cleared.
//  * Targets: modulation targets of effects that are ON after the roll and
//    not locked. Skipped as inaudible or unsafe: the limiter (the safety
//    ceiling), an EQ band that is off, filter 2 while it is off, and a rate /
//    time / density knob while its effect is synced to tempo. Distinct per slot.
//  * Depth: |depth| in 0.15 .. 0.4 + 0.5 x wildness, random sign; cleared
//    slots get depth 0. If nothing is eligible, nothing is written or drawn and
//    slot 0 holds keepSlotsMarker.
//
// Writes the depth parameters (gesture-wrapped) and returns the target per
// slot ("" = cleared) for the caller to apply with setModSlotTarget.
// Returned in slot 0 when nothing is eligible: the caller keeps every slot as is.
inline const juce::String keepSlotsMarker { "<keep>" };
std::array<juce::String, id::numModSlots> rollModSlots (juce::AudioProcessorValueTreeState&, float wildness,
                                                       juce::uint32 lockMask, juce::Random&);

// True when modulating `targetId` would currently be heard (the candidate rule above).
bool isAudibleModTarget (juce::AudioProcessorValueTreeState&, const juce::String& targetId, juce::uint32 lockMask);

} // namespace spa::params
