#pragma once

#include <atomic>
#include <cmath>
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "ModEffect.h"
#include "TremVib.h"
#include "Limiter.h"
#include "PlateReverb.h"
#include "StereoChorus.h"
#include "ParametricEQ.h"
#include "Multiband.h"
#include "GrainFX.h"
#include "Telemetry.h"
#include "../params/LfoDivisions.h"

namespace spa::dsp
{

// Global stereo FX chain, processed after the synth mix and before master
// gain. Modules run in the order listed in `processOrder` — fixed for v1 but
// architected as an ordered list so reordering is a data change, not a
// rewrite.
class FXChain
{
public:
    FXChain() = default;

    // Append-only: module ids are serialized in the per-preset chain order.
    // comp and grain were appended in 1.0.29 (numModules 9 -> 11).
    enum class Module { distortion, chorus, delay, reverb, eq, mod, tremVib, limiter, convolve,
                        comp, grain };
    static constexpr int numModules = 11;
    // Modules in saved orders written before 1.0.29.
    static constexpr int legacyNumModules = 9;

    // Pack/unpack the chain order into a uint64 (4 bits/module): a single atomic
    // for the lock-free UI->audio hand-off and compact preset storage.
    static juce::uint64 packOrder (const Module* order)
    {
        juce::uint64 v = 0;
        for (int i = 0; i < numModules; ++i)
            v |= (juce::uint64) ((int) order[i] & 0xF) << (i * 4);
        return v;
    }
    static juce::uint64 defaultOrderPacked()
    {
        Module def[numModules] { Module::distortion, Module::chorus, Module::mod,
                                 Module::tremVib, Module::grain, Module::delay,
                                 Module::reverb, Module::convolve, Module::eq,
                                 Module::comp, Module::limiter };
        return packOrder (def);
    }

    // Unpack validates the value is a permutation. Three outcomes:
    //  1. A full 11-module permutation: used as is.
    //  2. A LEGACY nine-module value (every preset/session saved before
    //     1.0.29; nibbles 9..15 are zero). The user's order is kept and the
    //     two new modules are inserted by a fixed rule: GRAIN immediately
    //     BEFORE DELAY, COMP immediately BEFORE LIMITER, wherever those two
    //     sit in the user's order. Both default OFF, so this changes no sound.
    //     (Reading nine nibbles as eleven would see two zeros, call the value
    //     a duplicate and reset every custom order to the default.)
    //  3. Anything else (corrupt): the natural enum order.
    static void unpackOrder (juce::uint64 packed, Module* order)
    {
        if (readPermutation (packed, numModules, order))
            return;

        Module legacy[legacyNumModules];
        if ((packed >> (legacyNumModules * 4)) == 0
            && readPermutation (packed, legacyNumModules, legacy))
        {
            int n = 0;
            for (int i = 0; i < legacyNumModules; ++i)
            {
                if (legacy[i] == Module::delay)   order[n++] = Module::grain;
                if (legacy[i] == Module::limiter) order[n++] = Module::comp;
                order[n++] = legacy[i];
            }
            return;
        }

        for (int i = 0; i < numModules; ++i)
            order[i] = (Module) i;
    }

    struct Params
    {
        bool distEnable = false;
        int distType = 0;          // Soft/Hard/Fold
        float distDrive = 0.3f;
        float distToneHz = 8000.0f;
        float distMix = 1.0f;

        bool chorusEnable = false;
        int chorusMode = 1;         // 0 Vintage (Juno-ish) 1 Modern (clean)
        float chorusRate = 0.8f;
        float chorusDepth = 0.3f;
        float chorusFeedback = 0.0f;
        float chorusWidth = 0.5f;   // 0..1 L/R LFO phase offset (see StereoChorus)
        float chorusMix = 0.5f;

        bool delayEnable = false;
        bool delaySync = true;
        float delayTimeMs = 350.0f;
        int delayDivision = 6;
        float delayFeedback = 0.35f;
        bool delayPingPong = false;
        float delayWidth = 1.0f;   // 0..1, only acts when delayPingPong is on
        float delayMix = 0.35f;

        bool reverbEnable = false;
        int reverbMode = 0;         // 0 Hall 1 Plate 2 Chamber 3 Room 4 Spring
        float reverbPreDelay = 20.0f;
        float reverbSize = 0.5f;
        float reverbDecay = 2.0f;   // RT60 seconds
        float reverbDamping = 0.5f; // HF damp
        float reverbModDepth = 0.2f;
        float reverbLowCut = 20.0f;
        float reverbHighCut = 12000.0f;
        float reverbWidth = 1.0f;
        float reverbMix = 0.3f;

        bool eqEnable = false;
        int eqCharacter = 0;   // 0 Clean 1 Modern 2 Vintage 3 Tube
        std::array<ParametricEQ::Band, ParametricEQ::numBands> eqBands {};

        double bpm = 120.0;

        bool modEnable = false;
        int modType = 0;           // 0 = Phaser, 1 = Flanger
        float modRate = 0.5f;
        bool modSync = false;
        int modDivision = 6;
        float modDepth = 0.5f;
        float modFeedback = 0.3f;
        int modStages = 6;
        float modCentreHz = 800.0f;
        float modManualMs = 3.0f;
        float modWidth = 0.5f;
        float modMix = 0.5f;

        bool tremEnable = false;
        float tremRate = 5.0f;
        bool tremSync = false;
        int tremDivision = 6;
        float tremDepth = 0.5f;
        int tremShape = 0;
        float tremStereo = 0.0f;
        float tremMix = 1.0f;

        bool vibEnable = false;
        float vibRate = 5.0f;
        bool vibSync = false;
        int vibDivision = 6;
        float vibDepth = 0.5f;
        float vibMix = 1.0f;

        bool limEnable = false;
        float limDrive = 0.0f;
        float limCeiling = -0.3f;
        float limRelease = 120.0f;
        bool limAutoRelease = false;
        int limCharacter = 0;
        float limStereoLink = 1.0f;
        bool limTruePeak = false;
        bool limLookahead = false;
        bool limAutoGain = false;

        bool convEnable = false;
        float convMix = 0.3f;
        float convWidth = 1.0f;
        float convPreDelay = 0.0f;   // ms, wet pre-delay
        float convDecay = 1.0f;      // 0..1 IR tail length (shorter = tighter)
        float convDamping = 0.0f;    // 0..1 HF damping of the IR
        float convStart = 0.0f;      // 0..1 proportion of the raw IR trimmed off the front

        // COMP: SPAGlitch's three-band OTT-style compressor (ported exactly).
        bool compEnable = false;
        float compMix = 1.0f;
        float compCrossoverLow = 200.0f;
        float compCrossoverHigh = 2000.0f;
        std::array<Multiband::Band, Multiband::numBands> compBands {};

        // GRAIN: granular delay / texture.
        bool grainEnable = false;
        float grainSizeMs = 120.0f;
        float grainDensityHz = 14.0f;
        bool grainSync = false;
        int grainDivision = 9;
        float grainPitch = 0.0f;
        float grainSpread = 0.25f;        // SPREAD TIME 0..1
        float grainSpreadPitch = 3.0f;    // SPREAD PITCH 0..12 st
        float grainPositionMs = 300.0f;
        float grainReverse = 0.0f;
        float grainFeedback = 0.0f;
        float grainMix = 0.35f;
        bool grainFreeze = false;

        // Runtime FX processing order (drag-reorderable, saved per preset).
        Module order[numModules] {
            Module::distortion, Module::chorus, Module::mod, Module::tremVib,
            Module::grain, Module::delay, Module::reverb, Module::convolve,
            Module::eq, Module::comp, Module::limiter
        };
    };

    // Crush (bit-crusher distortion type) drive mappings, shared between the
    // DSP (processDistortion) and the UI (Displays.cpp's transfer-curve
    // staircase) so they can never drift apart. Both are exponential so the
    // DRIVE knob stays useful across its whole range: linear bit reduction
    // left most of the knob's travel inaudible (drive 0.5 -> ~9.5 bits).
    //   drive 0    -> 16 bits / no decimation (transparent)
    //   drive 0.5  -> ~6.9 bits / ~6.3-sample hold
    //   drive 0.8  -> ~4.2 bits / ~19-sample hold
    //   drive 1    -> 3 bits / a ~40-sample hold at 48 kHz
    static float crushBitsForDrive (float drive)
    {
        return 16.0f * std::pow (3.0f / 16.0f, drive);
    }
    static float crushHoldForDrive (float drive, double sampleRate)
    {
        return (float) (std::pow (40.0, (double) drive) * (sampleRate / 48000.0));
    }

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    void process (juce::AudioBuffer<float>& buffer, const Params& params);

    // Worst-case ring-out for AudioProcessor::getTailLengthSeconds().
    double tailSeconds (const Params& params) const;

    // Per-band COMP gain meter (signed dB) and the GRAIN cloud, copied into
    // Telemetry by the processor once per block (audio thread, lock-free).
    void publishTelemetry (Telemetry& t) const
    {
        for (int b = 0; b < Multiband::numBands; ++b)
            t.compBandDb[(size_t) b].store (compEffect.bandGainDb (b), std::memory_order_relaxed);
        grainEffect.publish (t.grainFx);
    }
    const GrainFX& grain() const { return grainEffect; }

    // Lookahead-limiter latency (reported to the host) + gain reduction meter.
    int limiterLatencySamples (const Params& p) const;
    float limiterGainReductionDb() const { return limiterEffect.gainReductionDb(); }
    float limiterOutputPeak() const { return limiterEffect.outputPeak(); }

    // Convolve (SFX / user WAV as impulse). The raw IR is read once and kept;
    // decay/damping reshape it and it is (re)loaded into juce::dsp::Convolution
    // on a background thread. All of these run on the message thread.
    void loadConvolutionIR (const juce::File& irFile);
    // SPAStripAdded: load the raw IR from already-decoded audio (the plugin
    // keeps the IR inside its state rather than pointing at a file), and
    // clear it. Same raw-IR bookkeeping as loadConvolutionIR above; message
    // thread only.
    void loadConvolutionIRFromBuffer (const juce::AudioBuffer<float>& ir, double irSampleRate);
    void clearConvolutionIR();
    // start = proportion (0..1) of the raw IR trimmed off the front, applied
    // before decay/damping. Reshapes (and reloads into the convolution
    // engine) only when any of the three actually changed.
    void setConvolutionShaping (float decay, float damping, float start);
    bool hasConvolutionIR() const { return convIrLoaded.load (std::memory_order_relaxed); }

    // Proportion of the raw IR actually trimmed by the last reshape (after
    // the minimum-tail clamp below), 0..1 -- lets the UI draw the trimmed
    // region even when it differs slightly from the raw parameter value
    // (e.g. a short IR near the min-tail floor).
    float convolutionStartTrim() const { return convStartTrimApplied; }

    // Downsampled magnitude envelope of the shaped IR for the UI waveform.
    static constexpr int convEnvPoints = 256;
    const std::array<float, convEnvPoints>& convolutionEnvelope() const { return irEnvelope; }
    double convolutionLengthSeconds() const { return irLengthSeconds.load (std::memory_order_relaxed); }

private:
    void processDistortion (juce::AudioBuffer<float>&, const Params&);
    void processChorus (juce::AudioBuffer<float>&, const Params&);
    void processDelay (juce::AudioBuffer<float>&, const Params&);
    void processReverb (juce::AudioBuffer<float>&, const Params&);
    void processEQ (juce::AudioBuffer<float>&, const Params&);
    void processMod (juce::AudioBuffer<float>&, const Params&);
    void processTremVib (juce::AudioBuffer<float>&, const Params&);
    void processLimiter (juce::AudioBuffer<float>&, const Params&);
    void processConvolve (juce::AudioBuffer<float>&, const Params&);
    void processComp (juce::AudioBuffer<float>&, const Params&);
    void processGrain (juce::AudioBuffer<float>&, const Params&);

    static bool readPermutation (juce::uint64 packed, int count, Module* out)
    {
        bool seen[16] = {};
        for (int i = 0; i < count; ++i)
        {
            const int id = (int) ((packed >> (i * 4)) & 0xF);
            if (id >= count || seen[id])
                return false;
            seen[id] = true;
            out[i] = (Module) id;
        }
        return true;
    }

    double sampleRate = 48000.0;
    ModEffect modEffect;
    TremVib tremVibEffect;
    Limiter limiterEffect;
    // Non-uniform partitioned convolution (256-sample head) rather than the
    // default uniform-block engine: IRs here run up to 10s (see
    // loadConvolutionIR's cap), and JUCE's own docs recommend NonUniform with
    // a >=256-sample head for reverberation-length IRs (>=~4096 samples) to
    // keep average CPU down on the long tail, at the cost of a little extra
    // latency at the head vs the zero-latency uniform default.
    juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 256 } };
    juce::AudioBuffer<float> convScratch;
    // Written on the message thread (load/reshape), read on the audio thread
    // (process()) and from hasConvolutionIR() — same relaxed-atomic pattern as
    // the rest of the codebase's cross-thread flags.
    std::atomic<bool> convIrLoaded { false };

    // Raw (unshaped) IR kept so decay/damping can reshape without re-reading the
    // file; the reshaped copy is what gets loaded into the convolution engine.
    void reshapeConvolutionIR();
    juce::AudioFormatManager convFormats;
    juce::AudioBuffer<float> rawIR;
    double rawIRSampleRate = 0.0;
    bool haveRawIR = false;
    float convDecayApplied = 1.0f, convDampingApplied = 0.0f, convStartApplied = 0.0f;
    // Actual trim fraction after the minimum-tail clamp (see reshapeConvolutionIR);
    // read by the UI via convolutionStartTrim(). Message-thread only, like the
    // rest of the convolve-reshape state above.
    float convStartTrimApplied = 0.0f;
    // A start position that trimmed away the whole IR would leave Convolve
    // silent -- which reads as a bug, not a creative extreme -- so the reshape
    // always keeps at least this much of the tail, however early the start
    // position is set.
    static constexpr float kConvStartMinTailSeconds = 0.15f;
    // Written on the message thread (reshapeConvolutionIR), read from
    // tailSeconds() on the audio thread (getTailLengthSeconds).
    std::atomic<double> irLengthSeconds { 0.0 };
    std::array<float, convEnvPoints> irEnvelope {};

    // Wet pre-delay ring (per channel), up to 200 ms.
    std::array<juce::AudioBuffer<float>, 2> convPreBuf;
    int convPreWrite = 0;

    // Distortion tone filter (post-shaper lowpass), one per channel.
    std::array<juce::dsp::FirstOrderTPTFilter<float>, 2> toneFilters;

    // Crush distortion (sample-and-hold decimation) state, one per channel:
    // the currently-held output sample and a fractional phase accumulator
    // counting down the hold length. Fixed-size, no allocation.
    std::array<float, 2> crushHold {};
    std::array<float, 2> crushPhase {};

    // Own engine rather than juce::dsp::Chorus: JUCE drives both channels
    // from one LFO, so it images mono and there is no way to bolt a width
    // control onto it from the outside. See StereoChorus.h.
    StereoChorus chorusEffect;

    // Delay: fixed max 4 s ring buffer per channel.
    juce::AudioBuffer<float> delayBuffer;
    int delayWritePos = 0;
    juce::SmoothedValue<float> delaySamplesSmoothed;
    juce::SmoothedValue<float> delayWidthSmoothed;

    PlateReverb reverb;

    // 8-band parametric EQ (hand-rolled biquads, character saturation).
    ParametricEQ eq;

    Multiband compEffect;
    GrainFX grainEffect;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FXChain)
};

} // namespace spa::dsp
