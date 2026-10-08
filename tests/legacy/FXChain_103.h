#pragma once

#include <atomic>
#include <cmath>
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "ModEffect_103.h"
#include "TremVib_103.h"
#include "Limiter_103.h"
#include "PlateReverb_103.h"
#include "StereoChorus_103.h"
#include "ParametricEQ_103.h"
#include "Multiband_103.h"
#include "GrainFX_103.h"
#include "MultiModeFilter_103.h"
#include "../../source/dsp/Telemetry.h"
#include "../../source/params/LfoDivisions.h"

// FROZEN copy of SPAStrip 1.0.3 FXChain (header + implementation), the bit-identity
// reference for the spa-fx adoption. Do not edit.
namespace spa::dsp::legacy103
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
    // SPAStripAdded: filter is appended after them (numModules 11 -> 12); the
    // DEFAULT order puts it FIRST in the chain, but its id stays 11.
    enum class Module { distortion, chorus, delay, reverb, eq, mod, tremVib, limiter, convolve,
                        comp, grain, filter };
    static constexpr int numModules = 12;
    // Modules in saved orders written before 1.0.29.
    static constexpr int legacyNumModules = 9;
    // SPAStripAdded: modules in saved orders written before the FILTER module.
    static constexpr int preFilterNumModules = 11;

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
        Module def[numModules] { Module::filter,   // SPAStripAdded: first in the default chain
                                 Module::distortion, Module::chorus, Module::mod,
                                 Module::tremVib, Module::grain, Module::delay,
                                 Module::reverb, Module::convolve, Module::eq,
                                 Module::comp, Module::limiter };
        return packOrder (def);
    }

    // Unpack validates the value is a permutation. Four outcomes:
    //  1. A full 12-module permutation: used as is.
    //  2. SPAStripAdded: a PRE-FILTER eleven-module value (every session and
    //     preset saved before the FILTER module existed; nibble 11 is zero).
    //     The user's order is kept and FILTER is inserted at slot 0, the
    //     default position. It defaults OFF, so this changes no sound. (Reading
    //     eleven nibbles as twelve would see a second zero, call the value a
    //     duplicate and reset every custom order to the default.)
    //  3. A LEGACY nine-module value (before 1.0.29; nibbles 9..15 are zero).
    //     The user's order is kept and the two modules added then are inserted
    //     by a fixed rule: GRAIN immediately BEFORE DELAY, COMP immediately
    //     BEFORE LIMITER, wherever those two sit in the user's order. Then
    //     FILTER goes to slot 0 as in 2. All default OFF: no sound changes.
    //  4. Anything else (corrupt): the natural enum order.
    static void unpackOrder (juce::uint64 packed, Module* order)
    {
        if (readPermutation (packed, numModules, order))
            return;

        Module preFilter[preFilterNumModules];
        bool havePreFilter = false;

        if ((packed >> (preFilterNumModules * 4)) == 0
            && readPermutation (packed, preFilterNumModules, preFilter))
        {
            havePreFilter = true;
        }
        else
        {
            Module legacy[legacyNumModules];
            if ((packed >> (legacyNumModules * 4)) == 0
                && readPermutation (packed, legacyNumModules, legacy))
            {
                int n = 0;
                for (int i = 0; i < legacyNumModules; ++i)
                {
                    if (legacy[i] == Module::delay)   preFilter[n++] = Module::grain;
                    if (legacy[i] == Module::limiter) preFilter[n++] = Module::comp;
                    preFilter[n++] = legacy[i];
                }
                havePreFilter = true;
            }
        }

        if (havePreFilter)
        {
            order[0] = Module::filter;
            for (int i = 0; i < preFilterNumModules; ++i)
                order[i + 1] = preFilter[i];
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
        int chorusMode = 1;         // 0 Vintage (Juno-ish) 1 Modern (clean) 2 VHS
        float chorusRate = 0.8f;
        float chorusDepth = 0.3f;
        float chorusFeedback = 0.0f;
        float chorusWidth = 0.5f;   // 0..1 L/R LFO phase offset (see StereoChorus)
        float chorusMix = 0.5f;
        // VHS mode (0..1; see StereoChorus::Params)
        float chorusVhsWow = 0.4f, chorusVhsFlutter = 0.25f, chorusVhsTone = 0.45f,
              chorusVhsSat = 0.3f, chorusVhsHiss = 0.15f, chorusVhsDropouts = 0.1f;

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
        float grainReleaseSec = 0.0f;     // GLITTER RELEASE: 0 off, >= GrainFX::infiniteRelease holds

        // SPAStripAdded: FILTER, two SVF filters (Series / Parallel). filterEnable
        // is FILTER 1's switch (the module's own enable); filter 2 has its own.
        // Cutoff in Hz, resonance / drive / mix 0..1, type = params::FilterType.
        bool filterEnable = false;
        int filterRouting = 0;             // 0 Series, 1 Parallel
        int filter1Type = 0;
        float filter1Cutoff = 20000.0f;
        float filter1Resonance = 0.0f;
        float filter1Drive = 0.0f;
        float filter1Mix = 1.0f;
        bool filter2Enable = false;
        int filter2Type = 0;
        float filter2Cutoff = 20000.0f;
        float filter2Resonance = 0.0f;
        float filter2Drive = 0.0f;
        float filter2Mix = 1.0f;

        // Runtime FX processing order (drag-reorderable, saved per preset).
        Module order[numModules] {
            Module::filter,   // SPAStripAdded
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
        {
            t.compBandDb[(size_t) b].store (compEffect.bandGainDb (b), std::memory_order_relaxed);
            t.compBandLevelDb[(size_t) b].store (compInPeak > 0.0f ? compEffect.bandLevelDb (b) : -100.0f,
                                                 std::memory_order_relaxed);
        }
        // Peaks accumulate over every chunk of the host block (the modulated
        // path splits it) and restart here, once per block.
        t.compInPeak.store (compInPeak, std::memory_order_relaxed);
        t.compOutPeak.store (compOutPeak, std::memory_order_relaxed);
        compInPeak = compOutPeak = 0.0f;
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
   #ifdef SPASTRIP_MOD_AUDIT
    // SPAStripAdded, test target only: processFilter's coefficient glide can be
    // switched off, so the zipper audit can report the numbers with and without it.
    static std::atomic<bool>& filterGlideDisabledForAudit();
   #endif
    // SPAStripAdded: partition sizes the convolution was prepared with (diagnostics / tests).
    int convolutionHeadSamples() const { return convHeadSize; }
    int convolutionChunkSamples() const { return convChunk; }
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
    void processFilter (juce::AudioBuffer<float>&, const Params&);   // SPAStripAdded

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
    // SPAStripAdded: held by pointer so prepare() can re-create it with a
    // partition size suited to the engine rate (see FXChain::prepare).
    // One background queue (thread) shared by every instance this chain ever
    // creates: destroying a Convolution that owns its queue joins that thread,
    // which could stall the callback lock behind an in-flight IR build. Declared
    // before `convolution` so it outlives it.
    juce::dsp::ConvolutionMessageQueue convQueue;
    std::unique_ptr<juce::dsp::Convolution> convolution
        = std::make_unique<juce::dsp::Convolution> (juce::dsp::Convolution::NonUniform { 256 }, convQueue);
    int convHeadSize = 256;   // SPAStripAdded: head/tail partition size the instance above was built with
    int convChunk = 0;        // SPAStripAdded: max samples handed to one Convolution::process call
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
    mutable float compInPeak = 0.0f, compOutPeak = 0.0f;   // see publishTelemetry
    GrainFX grainEffect;

    // SPAStripAdded: FILTER module. Two TPT state-variable filters (the synth's
    // MultiModeFilter, ported verbatim) plus the glue state that lives here
    // rather than in the filter: the enable edges (state is cleared when a
    // filter is switched on) and the cutoff / resonance / drive the filter was
    // last run with, so the next call can glide to its new target instead of
    // stepping coefficients (see processFilter).
    struct FilterSlot
    {
        MultiModeFilter filter;
        bool wasOn = false;
        bool haveApplied = false;       // false: next call snaps instead of gliding
        float appliedCutoff = 20000.0f, appliedResonance = 0.0f, appliedDrive = 0.0f;
    };
    std::array<FilterSlot, 2> filterSlots;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FXChain)
};

} // namespace spa::dsp::legacy103


namespace spa::dsp::legacy103
{

void FXChain::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 2 };
    juce::ignoreUnused (spec);   // (only edit to the frozen copy: silences a warning)

    for (auto& f : toneFilters)
    {
        f.prepare ({ sampleRate, (juce::uint32) maxBlockSize, 1 });
        f.setType (juce::dsp::FirstOrderTPTFilterType::lowpass);
    }

    chorusEffect.prepare (sampleRate, maxBlockSize);
    modEffect.prepare (sampleRate, maxBlockSize);
    tremVibEffect.prepare (sampleRate, maxBlockSize);
    limiterEffect.prepare (sampleRate, maxBlockSize);
    // SPAStripAdded: size the convolution partition and bound its per-call block.
    //
    // JUCE's NonUniform head size doubles as the block size of the uniform-
    // partition TAIL, whose work per second goes as (IR length x rate) / that
    // size. At the fixed 256 this was (measured on an M5, 5 s IR, one block per
    // budget period): 1x 35-40 % of the block budget, 2x 41-63 %, 4x 77-90 %
    // with misses -- the cost grows with the square of the oversampling factor
    // because both the IR and the block rate scale with it. The partition costs
    // nothing in latency (the head engine is zero-latency), so make it large:
    // 4096 gives 1x ~3 %, 2x ~8 %, 4x ~28 % at 512-1024 sample host buffers.
    //
    // The one trade-off: a bigger partition means fewer but taller tail
    // "fills" (the whole IR is multiplied in one call, ~2 ms warm / ~7 ms cold
    // for 5 s at 4x). At 4x with 128-256 sample host buffers one fill exceeds a
    // block budget, and a fill only every 8th-16th block (4096) misses ~12 % of
    // blocks where the old 256 spread the work flat at ~87 % load and missed
    // ~3-4 %. There the partition is matched to the engine block (one fill per
    // block keeps the IR hot in the system cache): 3 % / 1 % misses. Spreading a
    // fill over several blocks would need a time-distributed engine, which JUCE's
    // does not offer.
    //
    // Separately, cap what one Convolution::process call may be handed
    // (convChunk): the head engine does two FFTs of 2x its PREPARED block size
    // on EVERY call however few samples it is given, so preparing it for a
    // 4x-oversampled 1024-sample host block (4096) made each 32-sample
    // modulation piece cost ~100 us. The output is the same stream either way
    // (see processConvolve).
    {
        int head = 4096;
        if (sampleRate >= 128000.0 && maxBlockSize >= 512 && maxBlockSize < 2048)
            head = juce::nextPowerOfTwo (maxBlockSize);
        if (head != convHeadSize)
        {
            convolution = std::make_unique<juce::dsp::Convolution> (juce::dsp::Convolution::NonUniform { head }, convQueue);
            convHeadSize = head;
        }
        convChunk = juce::jmax (64, juce::jmin (maxBlockSize, 256));
    }
    convolution->prepare ({ sampleRate, (juce::uint32) convChunk, 2 });
    convScratch.setSize (2, maxBlockSize, false, false, true);
    for (auto& b : convPreBuf) { b.setSize (1, (int) (0.2 * sampleRate) + 8); b.clear(); }
    convPreWrite = 0;
    // Re-shape/reload the IR at the new rate so it survives sample-rate and
    // oversampling changes (prepare resets the convolution engine).
    if (haveRawIR) reshapeConvolutionIR();

    delayBuffer.setSize (2, (int) (sampleRate * 4.0) + 8);
    delayBuffer.clear();
    delayWritePos = 0;
    delaySamplesSmoothed.reset (sampleRate, 0.1);
    delayWidthSmoothed.reset (sampleRate, 0.05);

    reverb.prepare (sampleRate, maxBlockSize);

    eq.prepare (sampleRate, maxBlockSize);
    compEffect.prepare (sampleRate, maxBlockSize);
    grainEffect.prepare (sampleRate, maxBlockSize);
    for (auto& slot : filterSlots)    // SPAStripAdded
        slot.filter.prepare (sampleRate);

    reset();
}

void FXChain::reset()
{
    for (auto& f : toneFilters)
        f.reset();
    crushHold.fill (0.0f);
    crushPhase.fill (0.0f);
    chorusEffect.reset();
    modEffect.reset();
    tremVibEffect.reset();
    limiterEffect.reset();
    convolution->reset();
    delayBuffer.clear();
    reverb.reset();
    eq.reset();
    compEffect.reset();
    grainEffect.reset();
    for (auto& slot : filterSlots)    // SPAStripAdded
    {
        slot.filter.reset();
        slot.wasOn = false;
        slot.haveApplied = false;
    }
}

double FXChain::tailSeconds (const Params& p) const
{
    double tail = 0.0;

    if (p.delayEnable)
    {
        const auto time = p.delaySync
                        ? params::lfoDivisionBeats (p.delayDivision) * 60.0 / p.bpm
                        : (double) p.delayTimeMs * 0.001;
        // Real -60 dB feedback ring-out; ceiling 300 s (was 12 s, which hosts
        // used as the stop point and so cut long delay decays off).
        const auto repeats = p.delayFeedback > 0.01f
                           ? std::log (0.001) / std::log ((double) p.delayFeedback)
                           : 1.0;
        tail = juce::jlimit (0.0, 300.0, time * repeats);
    }

    if (p.reverbEnable)
        tail = juce::jmax (tail, 0.5 + (double) p.reverbDecay);

    if (p.convEnable)
        // Pre-delay gap + the (reshaped) IR's own length; hosts truncate the
        // tail on bounce/freeze otherwise, clipping the reverb-style ring-out.
        tail = juce::jmax (tail, (double) p.convPreDelay * 0.001
                                + irLengthSeconds.load (std::memory_order_relaxed));

    if (p.grainEnable)
    {
        // The cloud keeps reading what is already in the ring: up to POSITION
        // (+ the spread jitter) behind, plus one grain, then the feedback
        // repeats, same ring-out rule as the delay. A held (frozen) ring never
        // ends, so it is capped at 300 s like the delay.
        const double reach = (double) p.grainPositionMs * 0.001 + 0.25 + (double) p.grainSizeMs * 0.001;
        const auto repeats = p.grainFeedback > 0.01f
                           ? std::log (0.001) / std::log ((double) p.grainFeedback)
                           : 1.0;
        tail = juce::jmax (tail, juce::jlimit (0.0, 300.0, reach * repeats));
        // RELEASE rings out for its own time (infinite = held = the same cap).
        if (p.grainReleaseSec >= GrainFX::minRelease)
            tail = juce::jmax (tail, juce::jlimit (0.0, 300.0, reach + (double) p.grainReleaseSec));
    }

    return tail;
}

void FXChain::process (juce::AudioBuffer<float>& buffer, const Params& params)
{
    for (const auto module : params.order)
    {
        switch (module)
        {
            case Module::distortion: if (params.distEnable)   processDistortion (buffer, params); break;
            // Always invoked, for the same reason as Module::mod below:
            // StereoChorus tracks its own enable edge so it can clear a hot
            // delay line + feedback state on re-enable rather than ringing
            // it back out. The disabled path is a cheap early-out.
            case Module::chorus:     processChorus (buffer, params); break;
            case Module::delay:      if (params.delayEnable)  processDelay (buffer, params); break;
            case Module::reverb:     if (params.reverbEnable) processReverb (buffer, params); break;
            case Module::eq:         if (params.eqEnable)     processEQ (buffer, params); break;
            // Always invoke (rather than gating on modEnable like the other
            // modules) so ModEffect's own enable-edge tracking sees every
            // disable; that's what lets it clear its trapped allpass/
            // feedback/delay state on re-enable instead of ringing it back
            // out (see ModEffect::process). The disabled path is a cheap
            // early-out, not a real per-sample cost.
            case Module::mod:        processMod (buffer, params); break;
            case Module::tremVib:    if (params.tremEnable || params.vibEnable)
                                                            { processTremVib (buffer, params); } break;
            case Module::limiter:    if (params.limEnable)    processLimiter (buffer, params); break;
            case Module::convolve:   if (params.convEnable && convIrLoaded.load (std::memory_order_relaxed))
                                                            { processConvolve (buffer, params); } break;
            // COMP: the Multiband holds only envelope followers and filter
            // state, so a disabled module is simply skipped. Re-enabling
            // resumes from state that decayed while it was off, as the other
            // modules here do.
            case Module::comp:       if (params.compEnable)   processComp (buffer, params); break;
            // Always invoked: GrainFX tracks its own enable edge so it can
            // forget its ring when switched off (see GrainFX::process); the
            // disabled path is a one-branch early-out.
            case Module::grain:      processGrain (buffer, params); break;
            // SPAStripAdded. Always invoked, like chorus / mod / grain: it tracks
            // each filter's own enable edge so a filter switched back on starts
            // from cleared state rather than whatever it held when it was last
            // running. The all-off path is two compares and a return.
            case Module::filter:     processFilter (buffer, params); break;
        }
    }
}

void FXChain::processDistortion (juce::AudioBuffer<float>& buffer, const Params& p)
{
    const auto driveGain = 1.0f + 15.0f * p.distDrive;

    for (auto& f : toneFilters)
        f.setCutoffFrequency (p.distToneHz);

    // Crush (bit-depth + sample-rate reduction) params, driven entirely by
    // DRIVE via the exponential mappings shared with the UI curve (see
    // crushBitsForDrive/crushHoldForDrive) so the knob stays useful across
    // its whole range instead of spending half its travel inaudible.
    const auto crushLevels = std::pow (2.0f, crushBitsForDrive (p.distDrive));
    const auto crushHoldLen = crushHoldForDrive (p.distDrive, sampleRate);

    for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        auto& tone = toneFilters[(size_t) ch];

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto dry = data[i];

            float wet;
            if (p.distType == 3)
            {
                // Bit-depth quantise the unscaled input (no drive boost --
                // that would just clip everything at high bit-crush amounts).
                const auto quantised = std::round (dry * crushLevels) / crushLevels;

                // Sample-and-hold decimation: advance the phase each sample;
                // only latch a new held value once the accumulated hold
                // length has been reached, else repeat the last one.
                auto& hold = crushHold[(size_t) ch];
                auto& phase = crushPhase[(size_t) ch];
                if (phase <= 0.0f)
                {
                    hold = quantised;
                    phase = crushHoldLen;
                }
                phase -= 1.0f;

                wet = tone.processSample (0, hold);
            }
            else
            {
                const auto x = dry * driveGain;
                switch (p.distType)
                {
                    case 1:  wet = juce::jlimit (-1.0f, 1.0f, x); break;              // Hard
                    case 2:  wet = std::sin (x * 1.2f); break;                        // Fold
                    default: wet = std::tanh (x); break;                              // Soft
                }
                wet = tone.processSample (0, wet / std::sqrt (driveGain));
            }

            data[i] = dry + (wet - dry) * p.distMix;
        }
    }
}

void FXChain::processChorus (juce::AudioBuffer<float>& buffer, const Params& p)
{
    StereoChorus::Params cp;
    cp.enable = p.chorusEnable;   // StereoChorus early-outs and tracks the edge
    cp.mode = p.chorusMode == 0 ? StereoChorus::Mode::vintage
            : p.chorusMode == 2 ? StereoChorus::Mode::vhs : StereoChorus::Mode::modern;
    cp.vhsWow = p.chorusVhsWow;
    cp.vhsFlutter = p.chorusVhsFlutter;
    cp.vhsTone = p.chorusVhsTone;
    cp.vhsSat = p.chorusVhsSat;
    cp.vhsHiss = p.chorusVhsHiss;
    cp.vhsDropouts = p.chorusVhsDropouts;
    cp.rateHz = p.chorusRate;
    cp.depth = p.chorusDepth;
    cp.feedback = p.chorusFeedback;
    cp.width = p.chorusWidth;
    cp.mix = p.chorusMix;
    chorusEffect.process (buffer, cp);
}

void FXChain::processDelay (juce::AudioBuffer<float>& buffer, const Params& p)
{
    const auto timeSeconds = p.delaySync
                           ? params::lfoDivisionBeats (p.delayDivision) * 60.0 / p.bpm
                           : (double) p.delayTimeMs * 0.001;
    const auto targetSamples = (float) juce::jlimit (
        32.0, (double) delayBuffer.getNumSamples() - 8.0, timeSeconds * sampleRate);
    delaySamplesSmoothed.setTargetValue (targetSamples);
    delayWidthSmoothed.setTargetValue (juce::jlimit (0.0f, 1.0f, p.delayWidth));

    const auto bufLen = delayBuffer.getNumSamples();
    auto* bufL = delayBuffer.getWritePointer (0);
    auto* bufR = delayBuffer.getWritePointer (1);
    auto* left = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : left;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const auto delaySamples = delaySamplesSmoothed.getNextValue();

        auto readPos = (double) delayWritePos - (double) delaySamples;
        while (readPos < 0.0)
            readPos += (double) bufLen;

        auto r0 = (int) readPos;
        const auto frac = (float) (readPos - (double) r0);
        while (r0 >= bufLen) r0 -= bufLen;   // wrap can round to exactly bufLen -- see FDNReverb.h
        const auto r1 = (r0 + 1) % bufLen;

        const auto outL = bufL[r0] + frac * (bufL[r1] - bufL[r0]);
        const auto outR = bufR[r0] + frac * (bufR[r1] - bufR[r0]);

        // Ping-pong crosses the feedback paths. WIDTH (only meaningful with
        // ping-pong on) blends the INJECTION from today's behaviour (w=0:
        // left into the left line, right into the right line) to true
        // ping-pong (w=1: the mono sum injected into the left line only, so
        // a centred source actually bounces instead of arriving on both
        // sides at once). Equal-power (0.70710678 = 1/sqrt(2)) so a centred
        // source keeps roughly the same echo energy across the width range.
        // Width is a no-op when ping-pong is off -- inL/inR just equal
        // left[i]/right[i], bit-identical to the pre-1.0.25 algorithm.
        float inL = left[i];
        float inR = right[i];
        if (p.delayPingPong)
        {
            const auto w = delayWidthSmoothed.getNextValue();
            const auto monoSum = (left[i] + right[i]) * 0.70710678f;
            inL = (1.0f - w) * left[i] + w * monoSum;
            inR = (1.0f - w) * right[i];
        }

        bufL[delayWritePos] = inL + (p.delayPingPong ? outR : outL) * p.delayFeedback;
        bufR[delayWritePos] = inR + (p.delayPingPong ? outL : outR) * p.delayFeedback;

        left[i] += outL * p.delayMix;
        right[i] += outR * p.delayMix;

        delayWritePos = (delayWritePos + 1) % bufLen;
    }
}

void FXChain::processReverb (juce::AudioBuffer<float>& buffer, const Params& p)
{
    // Dattorro-plate-derived engine (PlateReverb) with mode voicings. MIX is
    // a LINEAR dry/wet dial handled inside the engine (dry = 1-mix, wet =
    // mix): 0% is untouched dry, 100% is pure wet, 50% is exactly half of
    // each. (1.0.14 and earlier used an equal-power sin/cos crossfade on the
    // old FDN engine, which made the knob feel oversensitive near 0 -- see
    // CHANGELOG 1.0.15.)
    PlateReverb::Params rp;
    rp.mode = p.reverbMode;
    rp.preDelayMs = p.reverbPreDelay;
    rp.size = p.reverbSize;
    rp.decaySec = p.reverbDecay;
    rp.hfDamp = p.reverbDamping;
    rp.modDepth = p.reverbModDepth;
    rp.lowCutHz = p.reverbLowCut;
    rp.highCutHz = p.reverbHighCut;
    rp.width = p.reverbWidth;
    rp.mix = p.reverbMix;
    reverb.process (buffer, rp);
}

void FXChain::processMod (juce::AudioBuffer<float>& buffer, const Params& p)
{
    ModEffect::Params mp;
    mp.enable  = p.modEnable;
    mp.type    = p.modType == 1 ? ModEffect::Type::flanger : ModEffect::Type::phaser;
    mp.rateHz  = p.modSync
               ? (float) (p.bpm / 60.0
                          / juce::jmax (0.01, (double) params::lfoDivisionBeats (p.modDivision)))
               : p.modRate;
    mp.depth    = p.modDepth;
    mp.feedback = p.modFeedback;
    mp.stages   = p.modStages;
    mp.centreHz = p.modCentreHz;
    mp.manualMs = p.modManualMs;
    mp.spread   = p.modWidth;
    mp.mix      = p.modMix;
    modEffect.process (buffer, mp);
}

void FXChain::processTremVib (juce::AudioBuffer<float>& buffer, const Params& p)
{
    const auto syncHz = [&] (int div)
    {
        return (float) (p.bpm / 60.0
                        / juce::jmax (0.01, (double) params::lfoDivisionBeats (div)));
    };

    TremVib::Params tp;
    tp.tremOn     = p.tremEnable;
    tp.tremRateHz = p.tremSync ? syncHz (p.tremDivision) : p.tremRate;
    tp.tremDepth  = p.tremDepth;
    tp.tremShape  = p.tremShape;
    tp.tremStereo = p.tremStereo;
    tp.tremMix    = p.tremMix;
    tp.vibOn      = p.vibEnable;
    tp.vibRateHz  = p.vibSync ? syncHz (p.vibDivision) : p.vibRate;
    tp.vibDepth   = p.vibDepth;
    tp.vibMix     = p.vibMix;
    tremVibEffect.process (buffer, tp);
}

void FXChain::processLimiter (juce::AudioBuffer<float>& buffer, const Params& p)
{
    Limiter::Params lp;
    lp.enable      = p.limEnable;
    lp.driveDb     = p.limDrive;
    lp.ceilingDb   = p.limCeiling;
    lp.releaseMs   = p.limRelease;
    lp.autoRelease = p.limAutoRelease;
    lp.character   = p.limCharacter;
    lp.stereoLink  = p.limStereoLink;
    lp.truePeak    = p.limTruePeak;
    lp.lookahead   = p.limLookahead;
    lp.autoGain    = p.limAutoGain;
    limiterEffect.process (buffer, lp);
}

int FXChain::limiterLatencySamples (const Params& p) const
{
    Limiter::Params lp;
    lp.enable    = p.limEnable;
    lp.lookahead = p.limLookahead;
    return limiterEffect.latencySamples (lp);
}

void FXChain::processComp (juce::AudioBuffer<float>& buffer, const Params& p)
{
    Multiband::Params mp;
    mp.enable          = p.compEnable;
    mp.mix             = p.compMix;
    mp.crossoverLowHz  = p.compCrossoverLow;
    mp.crossoverHighHz = p.compCrossoverHigh;
    mp.bands           = p.compBands;
    const auto peakOf = [&buffer]
    {
        float peak = 0.0f;
        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
            peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
        return peak;
    };
    compInPeak = juce::jmax (compInPeak, peakOf());
    compEffect.process (buffer, mp);
    compOutPeak = juce::jmax (compOutPeak, peakOf());
}

void FXChain::processGrain (juce::AudioBuffer<float>& buffer, const Params& p)
{
    GrainFX::Params gp;
    gp.enable     = p.grainEnable;
    gp.sizeMs     = p.grainSizeMs;
    gp.densityHz  = p.grainDensityHz;
    gp.sync       = p.grainSync;
    gp.division   = p.grainDivision;
    gp.bpm        = p.bpm;
    gp.pitchSt    = p.grainPitch;
    gp.spread     = p.grainSpread;
    gp.spreadPitchSt = p.grainSpreadPitch;
    gp.positionMs = p.grainPositionMs;
    gp.reverse    = p.grainReverse;
    gp.feedback   = p.grainFeedback;
    gp.mix        = p.grainMix;
    gp.freeze     = p.grainFreeze;
    gp.releaseSec = p.grainReleaseSec;
    grainEffect.process (buffer, gp);
}

#ifdef SPASTRIP_MOD_AUDIT
// Test-target-only seam (see ModTargets.h's SPASTRIP_MOD_AUDIT): turns the
// coefficient glide of processFilter off, to measure what it buys.
std::atomic<bool>& FXChain::filterGlideDisabledForAudit()
{
    static std::atomic<bool> disabled { false };
    return disabled;
}
#endif

// SPAStripAdded: FILTER. Port of SPASynth's per-voice filter section
// (SPASynthVoice.cpp) to an insert effect: filter 1 blends with the dry by its
// MIX; Series feeds that into filter 2 (blended by its own MIX), Parallel runs
// both on the raw input and averages them (0.5 x sum), exactly as the synth does.
// filterEnable is filter 1's switch, filter2Enable filter 2's; a disabled filter
// is a plain wire (in Parallel it still takes its half of the average, as in the
// synth). No latency, no tail.
//
// Coefficient glide. MultiModeFilter::setParams is a whole-block setting: the
// synth calls it per voice per block with a slow envelope behind it. Here the
// cutoff arrives from the modulation matrix in steps (a new value every 8 host
// samples, slewed over 4 ms) or from automation in steps of a whole host block,
// and a step of the SVF's coefficients is audible as zipper noise. So the
// filter is run in short sub-blocks, and between the cutoff / resonance / drive
// of the previous call and this call's targets the sub-block values move on a
// straight line (cutoff in LOG frequency, i.e. a constant number of octaves per
// sample). MultiModeFilter itself is untouched. A filter that was just switched
// on, or the first call after prepare / reset, snaps to its target.
void FXChain::processFilter (juce::AudioBuffer<float>& buffer, const Params& p)
{
    const bool on[2] { p.filterEnable, p.filter2Enable };

    for (int i = 0; i < 2; ++i)
    {
        auto& slot = filterSlots[(size_t) i];
        if (on[i] && ! slot.wasOn)
        {
            slot.filter.reset();
            slot.haveApplied = false;
        }
        slot.wasOn = on[i];
    }

    if (! on[0] && ! on[1])
        return;

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (2, buffer.getNumChannels());
    if (numSamples <= 0 || numCh <= 0)
        return;

    struct Target { params::FilterType type; float cutoff, resonance, drive, mix; };
    const auto toType = [] (int t) { return (params::FilterType) juce::jlimit (0, 7, t); };
    const Target targets[2] {
        { toType (p.filter1Type), juce::jmax (1.0f, p.filter1Cutoff), juce::jlimit (0.0f, 1.0f, p.filter1Resonance),
          juce::jlimit (0.0f, 1.0f, p.filter1Drive), juce::jlimit (0.0f, 1.0f, p.filter1Mix) },
        { toType (p.filter2Type), juce::jmax (1.0f, p.filter2Cutoff), juce::jlimit (0.0f, 1.0f, p.filter2Resonance),
          juce::jlimit (0.0f, 1.0f, p.filter2Drive), juce::jlimit (0.0f, 1.0f, p.filter2Mix) } };
    const bool parallel = p.filterRouting == 1;

   #ifdef SPASTRIP_MOD_AUDIT
    const bool glide = ! filterGlideDisabledForAudit().load (std::memory_order_relaxed);
   #else
    constexpr bool glide = true;
   #endif
    // One setParams per call (the plain port) when the glide is switched off for the audit.
    const int subBlock = glide ? 16 : numSamples;
    const int numSub = (numSamples + subBlock - 1) / subBlock;

    float logFrom[2], logTo[2];
    for (int i = 0; i < 2; ++i)
    {
        auto& slot = filterSlots[(size_t) i];
        if (! slot.haveApplied || ! glide)
        {
            slot.appliedCutoff = targets[i].cutoff;
            slot.appliedResonance = targets[i].resonance;
            slot.appliedDrive = targets[i].drive;
        }
        logFrom[i] = std::log (slot.appliedCutoff);
        logTo[i] = std::log (targets[i].cutoff);
    }

    for (int sb = 0; sb < numSub; ++sb)
    {
        const int start = sb * subBlock;
        const int len = juce::jmin (subBlock, numSamples - start);
        const float t = (float) (sb + 1) / (float) numSub;   // the last sub-block lands on the target

        for (int i = 0; i < 2; ++i)
        {
            if (! on[i])
                continue;
            auto& slot = filterSlots[(size_t) i];
            const auto& tg = targets[i];
            const float cutoff = juce::exactlyEqual (slot.appliedCutoff, tg.cutoff)
                               ? tg.cutoff
                               : std::exp (logFrom[i] + (logTo[i] - logFrom[i]) * t);
            slot.filter.setParams (tg.type, cutoff,
                                   slot.appliedResonance + (tg.resonance - slot.appliedResonance) * t,
                                   slot.appliedDrive + (tg.drive - slot.appliedDrive) * t);
        }

        for (int ch = 0; ch < numCh; ++ch)
        {
            float* data = buffer.getWritePointer (ch) + start;
            auto& f1 = filterSlots[0].filter;
            auto& f2 = filterSlots[1].filter;
            const float m1 = targets[0].mix, m2 = targets[1].mix;

            for (int i = 0; i < len; ++i)
            {
                const float x = data[i];

                // The filter always runs (its state must keep tracking the input);
                // MIX 0 then returns the input itself, so a fully dry filter is a
                // bit-exact wire rather than x + (y - x) * 0.
                float out1 = x;
                if (on[0])
                {
                    const float y = f1.processSample (ch, x);
                    out1 = m1 > 0.0f ? x + (y - x) * m1 : x;
                }

                float out = out1;
                if (on[1])
                {
                    if (parallel)
                    {
                        const float y = f2.processSample (ch, x);
                        const float pb = m2 > 0.0f ? x + (y - x) * m2 : x;
                        out = 0.5f * (out1 + pb);
                    }
                    else
                    {
                        const float y = f2.processSample (ch, out1);
                        out = m2 > 0.0f ? out1 + (y - out1) * m2 : out1;
                    }
                }
                data[i] = out;
            }
        }
    }

    for (int i = 0; i < 2; ++i)
    {
        auto& slot = filterSlots[(size_t) i];
        slot.appliedCutoff = targets[i].cutoff;
        slot.appliedResonance = targets[i].resonance;
        slot.appliedDrive = targets[i].drive;
        slot.haveApplied = true;
    }
}

void FXChain::processConvolve (juce::AudioBuffer<float>& buffer, const Params& p)
{
    const int n = buffer.getNumSamples();
    const int numCh = juce::jmin (2, buffer.getNumChannels());

    // Wet copy through the convolution, then blend with the dry (mix + width).
    for (int ch = 0; ch < 2; ++ch)
        convScratch.copyFrom (ch, 0, buffer, juce::jmin (ch, numCh - 1), 0, n);
    // SPAStripAdded: in pieces of at most convChunk samples (the size it was
    // prepared for); the result is the same stream, just bounded per-call cost.
    for (int off = 0; off < n; off += convChunk)
    {
        auto block = juce::dsp::AudioBlock<float> (convScratch).getSubBlock ((size_t) off, (size_t) juce::jmin (convChunk, n - off));
        convolution->process (juce::dsp::ProcessContextReplacing<float> (block));
    }

    // Wet pre-delay (gap before the reverb).
    const int rs = convPreBuf[0].getNumSamples();
    const int pd = juce::jlimit (0, rs - 1, (int) (p.convPreDelay * 0.001f * (float) sampleRate));
    if (pd > 0 && rs > 1)
        for (int i = 0; i < n; ++i)
        {
            const int w = convPreWrite;
            const int rd = (w - pd + rs) % rs;
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* ring = convPreBuf[(size_t) ch].getWritePointer (0);
                ring[w] = convScratch.getSample (ch, i);
                convScratch.setSample (ch, i, ring[rd]);
            }
            convPreWrite = (w + 1) % rs;
        }

    const float mix = juce::jlimit (0.0f, 1.0f, p.convMix);
    const float width = juce::jlimit (0.0f, 1.0f, p.convWidth);
    for (int i = 0; i < n; ++i)
    {
        float wL = convScratch.getSample (0, i);
        float wR = convScratch.getSample (1, i);
        if (numCh > 1)   // stereo width via mid/side on the wet
        {
            const float mid = 0.5f * (wL + wR);
            const float side = 0.5f * (wL - wR) * width;
            wL = mid + side; wR = mid - side;
        }
        buffer.setSample (0, i, buffer.getSample (0, i) * (1.0f - mix) + wL * mix);
        if (numCh > 1)
            buffer.setSample (1, i, buffer.getSample (1, i) * (1.0f - mix) + wR * mix);
    }
}

void FXChain::loadConvolutionIR (const juce::File& irFile)
{
    haveRawIR = false;
    convIrLoaded.store (false, std::memory_order_relaxed);
    if (! irFile.existsAsFile()) return;
    if (convFormats.getNumKnownFormats() == 0) convFormats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (convFormats.createReaderFor (irFile));
    if (reader == nullptr || reader->lengthInSamples <= 0) return;

    const int maxSamples = (int) (reader->sampleRate * 10.0);   // cap the IR at 10 s
    const int n = (int) juce::jmin ((juce::int64) maxSamples, reader->lengthInSamples);
    rawIR.setSize ((int) juce::jmin ((juce::uint32) 2, reader->numChannels), n);
    reader->read (&rawIR, 0, n, 0, true, true);
    rawIRSampleRate = reader->sampleRate;
    haveRawIR = true;
    reshapeConvolutionIR();
}

// SPAStripAdded (not in SPASynth): see FXChain.h.
void FXChain::loadConvolutionIRFromBuffer (const juce::AudioBuffer<float>& ir, double irSampleRate)
{
    haveRawIR = false;
    convIrLoaded.store (false, std::memory_order_relaxed);
    if (ir.getNumSamples() <= 0 || ir.getNumChannels() <= 0 || irSampleRate <= 0.0) return;

    const int n = ir.getNumSamples();
    rawIR.setSize (juce::jmin (2, ir.getNumChannels()), n);
    for (int c = 0; c < rawIR.getNumChannels(); ++c)
        rawIR.copyFrom (c, 0, ir, c, 0, n);
    rawIRSampleRate = irSampleRate;
    haveRawIR = true;
    reshapeConvolutionIR();
}

// SPAStripAdded (not in SPASynth): see FXChain.h.
void FXChain::clearConvolutionIR()
{
    haveRawIR = false;
    convIrLoaded.store (false, std::memory_order_relaxed);
    irLengthSeconds.store (0.0, std::memory_order_relaxed);
    for (auto& e : irEnvelope) e = 0.0f;
}

void FXChain::setConvolutionShaping (float decay, float damping, float start)
{
    if (juce::approximatelyEqual (decay, convDecayApplied)
        && juce::approximatelyEqual (damping, convDampingApplied)
        && juce::approximatelyEqual (start, convStartApplied))
        return;
    convDecayApplied = decay;
    convDampingApplied = damping;
    convStartApplied = start;
    if (haveRawIR) reshapeConvolutionIR();
}

// Builds the shaped IR (decay envelope + HF damping) from the raw IR and loads
// it; also refreshes the display envelope. Message thread only.
void FXChain::reshapeConvolutionIR()
{
    if (! haveRawIR || rawIR.getNumSamples() == 0)
    {
        convIrLoaded.store (false, std::memory_order_relaxed);
        return;
    }

    const int rawN = rawIR.getNumSamples();
    const int ch = rawIR.getNumChannels();
    const double sr = rawIRSampleRate > 0.0 ? rawIRSampleRate : sampleRate;

    // Start position trims from the FRONT of the raw impulse -- removing the
    // direct hit and early reflections to leave only the diffuse tail -- and
    // runs before decay/damping, which then reshape whatever remains. This is
    // independent of pre-delay (which inserts silence before the wet signal
    // rather than removing anything from the impulse itself).
    //
    // Never let the effect go silent: however far start is dragged, at least
    // kConvStartMinTailSeconds of the raw IR survives the trim. A start of 0
    // always trims nothing, matching pre-start-position behaviour exactly.
    const int minTailSamples = juce::jmax (1, (int) (kConvStartMinTailSeconds * sr));
    const int maxTrim = juce::jmax (0, rawN - minTailSamples);
    const int trimSamples = juce::jlimit (0, maxTrim,
                                          (int) (juce::jlimit (0.0f, 1.0f, convStartApplied) * (float) rawN));
    convStartTrimApplied = rawN > 0 ? (float) trimSamples / (float) rawN : 0.0f;

    const int n = juce::jmax (1, rawN - trimSamples);
    irLengthSeconds.store ((double) n / sr, std::memory_order_relaxed);

    const float decay = juce::jlimit (0.05f, 1.0f, convDecayApplied);
    const float damp  = juce::jlimit (0.0f, 1.0f, convDampingApplied);
    const float kDecay = 6.9f / (decay * (float) juce::jmax (1, n));   // -60 dB at decay*len
    const float cutoff = juce::jmap (damp, 0.0f, 1.0f, 20000.0f, 800.0f);
    const float lpCoef = damp > 0.001f
        ? 1.0f - std::exp (-juce::MathConstants<float>::twoPi * cutoff / (float) sr)
        : 1.0f;

    juce::AudioBuffer<float> shaped (ch, n);
    for (auto& e : irEnvelope) e = 0.0f;

    for (int c = 0; c < ch; ++c)
    {
        const float* src = rawIR.getReadPointer (c) + trimSamples;
        float* dst = shaped.getWritePointer (c);
        float lp = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            float v = src[i];
            if (damp > 0.001f) { lp += lpCoef * (v - lp); v = lp; }
            dst[i] = v * std::exp (-kDecay * (float) i);
        }
    }

    for (int i = 0; i < n; ++i)
    {
        const int b = juce::jlimit (0, convEnvPoints - 1, i * convEnvPoints / juce::jmax (1, n));
        float a = std::abs (shaped.getSample (0, i));
        if (ch > 1) a = juce::jmax (a, std::abs (shaped.getSample (1, i)));
        irEnvelope[(size_t) b] = juce::jmax (irEnvelope[(size_t) b], a);
    }

    convolution->loadImpulseResponse (std::move (shaped), sr,
                                     juce::dsp::Convolution::Stereo::yes,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::yes);
    convIrLoaded.store (true, std::memory_order_relaxed);
}

void FXChain::processEQ (juce::AudioBuffer<float>& buffer, const Params& p)
{
    eq.setCharacter (p.eqCharacter);
    eq.updateBands (p.eqBands);
    eq.process (buffer);
}

} // namespace spa::dsp::legacy103
