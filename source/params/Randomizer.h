#pragma once

#include <optional>

#include "ParameterRegistry.h"
#include "../dsp/FXChain.h"

namespace spa::params
{

// RANDOMIZE ALL for SPAStrip (ported from SPASynth's params/Randomizer.*, FX
// parts only).
//
// Locks: one per chain module (the 11 dsp::FXChain::Module values; the TREM/VIB
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
//
// Never touched: global.*, sc.*, mod.slotN.depth, mod slot targets, the IR.
void randomizeAll (juce::AudioProcessorValueTreeState&, float wildness, juce::uint32 lockMask,
                   juce::Array<int>& fxOrder, juce::Random&);

} // namespace spa::params
