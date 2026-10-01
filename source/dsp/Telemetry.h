#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>

namespace spa::dsp
{

// Lock-free audio -> UI channel for the effect. Effect-only slimming of
// SPASynth's Telemetry: it holds exactly what FXChain, GrainFX, the EQ
// analyzer, the limiter history and the COMP meters write or read, and has no
// dependency on the synth parameter registry. Everything is written by the
// audio thread with relaxed atomics and read by UI timers -- no locks, no
// allocation, no waiting on either side.
struct Telemetry
{
    // Resolved live tempo (host playhead BPM, or the 120 fallback), published
    // once per block from the exact same value the FX chain's synced modules
    // (delay/mod/trem/vib/grain) read. 120 is only ever a pre-first-block
    // default.
    std::atomic<float> bpm { 120.0f };

    // Block peaks of the final (post-chain, post-mix, post-output-gain)
    // output, host domain.
    std::atomic<float> peakL { 0.0f };
    std::atomic<float> peakR { 0.0f };

    // Post-chain mono scope ring for the EQ spectrum analyzer. The audio
    // thread pushes the output sample-by-sample; the UI reads the latest
    // window ending at scopeWrite and runs its own FFT. Cosmetic, so relaxed
    // atomics.
    static constexpr int scopeSize = 4096;   // power of two for the FFT
    std::array<std::atomic<float>, scopeSize> scope {};
    std::atomic<int> scopeWrite { 0 };

    // PRE-FX tap for the EQ analyzer's PRE/POST overlay: the signal entering
    // the chain, same mono-sum shape as `scope`.
    std::array<std::atomic<float>, scopeSize> preScope {};
    std::atomic<int> preScopeWrite { 0 };

    // Scrolling limiter history: per-block output peak (0..~1) and gain
    // reduction (dB, <= 0). The UI reads the window ending at limWrite.
    static constexpr int limiterHistory = 512;
    std::array<std::atomic<float>, limiterHistory> limOut {};
    std::array<std::atomic<float>, limiterHistory> limGrDb {};
    std::atomic<int> limWrite { 0 };

    // Sidechain / modulation matrix (SPAStrip phase 2), published once per
    // chunk. scEnvelope: the detector output, 0..1. scPresent: a signal source
    // exists for the detector (sc.source = Input, or External with the host's
    // sidechain bus enabled); false = the UI should show "no sidechain signal
    // routed" (the envelope is then 0). modSlotOffset: per slot, the normalised
    // offset it contributed at the last refresh (depth x envelope, before the
    // per-target sum and clamp).
    std::atomic<float> scEnvelope { 0.0f };
    std::atomic<bool> scPresent { false };
    static constexpr int numModSlots = 8;
    std::array<std::atomic<float>, numModSlots> modSlotOffset {};

    // COMP (FX module): signed per-band gain in dB (positive = upward lift,
    // negative = reduction), published once per block after the FX chain runs.
    std::array<std::atomic<float>, 3> compBandDb {};

    // GRAIN (FX module): the live grain cloud for the display, published once
    // per block. back = how far behind the write head the grain is reading
    // (seconds), age 0..1 through its window, span = how much audio it covers
    // (seconds), dir = +1 forward / -1 reversed, pan -1..1. fill = how much of
    // the ring has ever been written (0..1); frozen = FREEZE is holding it.
    // Cosmetic only, so relaxed atomics with count published last.
    static constexpr int maxGrainFxViz = 64;
    struct GrainFxViz
    {
        std::atomic<int> count { 0 };
        std::array<std::atomic<float>, maxGrainFxViz> back {}, age {}, span {}, dir {}, pan {};
        std::atomic<float> fill { 0.0f };
        std::atomic<bool> frozen { false };
        std::atomic<bool> capturing { false };   // FREEZE on, still filling the ring
        std::atomic<bool> active { false };
    };
    GrainFxViz grainFx;
};

} // namespace spa::dsp
