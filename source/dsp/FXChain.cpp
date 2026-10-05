#include "FXChain.h"

namespace spa::dsp
{

void FXChain::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 2 };

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
        // Feedback ring-out to roughly -60 dB.
        const auto repeats = p.delayFeedback > 0.01f
                           ? std::log (0.001) / std::log ((double) p.delayFeedback)
                           : 1.0;
        tail = juce::jlimit (0.0, 12.0, time * repeats);
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
        // ends, so it is capped at the ring length like the delay's 12 s.
        const double reach = (double) p.grainPositionMs * 0.001 + 0.25 + (double) p.grainSizeMs * 0.001;
        const auto repeats = p.grainFeedback > 0.01f
                           ? std::log (0.001) / std::log ((double) p.grainFeedback)
                           : 1.0;
        tail = juce::jmax (tail, juce::jlimit (0.0, 12.0, reach * repeats));
        // RELEASE rings out for its own time (infinite = held = the same cap).
        if (p.grainReleaseSec >= GrainFX::minRelease)
            tail = juce::jmax (tail, juce::jlimit (0.0, 12.0, reach + (double) p.grainReleaseSec));
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
    cp.mode = p.chorusMode == 0 ? StereoChorus::Mode::vintage : StereoChorus::Mode::modern;
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

} // namespace spa::dsp
