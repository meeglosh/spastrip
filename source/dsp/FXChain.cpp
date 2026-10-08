#include "FXChain.h"

namespace spa::dsp
{

void FXChain::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;

    distortion.prepare (sampleRate);

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

    delay.prepare (sampleRate);

    reverb.prepare (sampleRate, maxBlockSize);

    eq.prepare (sampleRate, maxBlockSize);
    compEffect.prepare (sampleRate, maxBlockSize);
    grainEffect.prepare (sampleRate, maxBlockSize);
    filterModule.prepare (sampleRate);

    reset();
}

void FXChain::reset()
{
    distortion.reset();
    chorusEffect.reset();
    modEffect.reset();
    tremVibEffect.reset();
    limiterEffect.reset();
    convolution->reset();
    delay.reset();
    reverb.reset();
    eq.reset();
    compEffect.reset();
    grainEffect.reset();
    filterModule.reset();
}

double FXChain::tailSeconds (const Params& p) const
{
    double tail = 0.0;

    if (p.delayEnable)
    {
        const auto time = p.delaySync
                        ? params::lfoDivisionBeats (p.delayDivision) * 60.0 / p.bpm
                        : (double) p.delayTimeMs * 0.001;
        // Real -60 dB feedback ring-out (spa-fx), ceiling 300 s.
        tail = spa::fx::Delay::tailSeconds (time, (double) p.delayFeedback);
    }

    if (p.reverbEnable)
        tail = juce::jmax (tail, 0.5 + (double) p.reverbDecay);

    if (p.convEnable)
        // Pre-delay gap + the (reshaped) IR's own length; hosts truncate the
        // tail on bounce/freeze otherwise, clipping the reverb-style ring-out.
        tail = juce::jmax (tail, spa::fx::Convolver::tailSeconds (
                                     p.convPreDelay, irLengthSeconds.load (std::memory_order_relaxed)));

    if (p.grainEnable)
    {
        // Shared rule (spa-fx): feedback ring-out or RELEASE seconds; RELEASE at
        // its top holds forever and reports the 300 s ceiling.
        GrainFX::Params gp;
        gp.positionMs = p.grainPositionMs; gp.sizeMs = p.grainSizeMs;
        gp.feedback = p.grainFeedback; gp.releaseSec = p.grainReleaseSec;
        tail = juce::jmax (tail, GrainFX::tailSeconds (gp));
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
            case Module::eq:         tapEq (buffer, true);
                                     if (params.eqEnable)     processEQ (buffer, params);
                                     tapEq (buffer, false);
                                     break;
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
    spa::fx::Distortion::Params dp;
    dp.type = p.distType;
    dp.drive = p.distDrive;
    dp.toneHz = p.distToneHz;
    dp.mix = p.distMix;
    distortion.process (buffer.getWritePointer (0),
                        buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr,
                        buffer.getNumSamples(), dp);
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
    spa::fx::Delay::Params dp;
    dp.timeSeconds = p.delaySync
                   ? params::lfoDivisionBeats (p.delayDivision) * 60.0 / p.bpm
                   : (double) p.delayTimeMs * 0.001;
    dp.feedback = p.delayFeedback;
    dp.pingPong = p.delayPingPong;
    dp.width = p.delayWidth;
    dp.mix = p.delayMix;
    delay.process (buffer.getWritePointer (0),
                   buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr,
                   buffer.getNumSamples(), dp);
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
    reverb.process (buffer.getWritePointer (0),
                    buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr,
                    buffer.getNumSamples(), rp);
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
    {
        float inPk = 0.0f;
        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
            inPk = juce::jmax (inPk, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
        limInPeak = juce::jmax (limInPeak, inPk);
    }
    limiterEffect.process (buffer, lp);
    limOutPeak = juce::jmax (limOutPeak, limiterEffect.outputPeak());
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

// SPAStripAdded: FILTER. The module (two SVF filters, Series / Parallel, sub-block
// coefficient glide, enable-edge reset) lives in spa-fx (Filter.h), moved unchanged.
// Always invoked: it tracks each filter's own enable edge.
void FXChain::processFilter (juce::AudioBuffer<float>& buffer, const Params& p)
{
    spa::fx::Filter::Params fp;
    fp.filterEnable = p.filterEnable;      fp.filterRouting = p.filterRouting;
    fp.filter1Type = p.filter1Type;        fp.filter1Cutoff = p.filter1Cutoff;
    fp.filter1Resonance = p.filter1Resonance; fp.filter1Drive = p.filter1Drive; fp.filter1Mix = p.filter1Mix;
    fp.filter2Enable = p.filter2Enable;    fp.filter2Type = p.filter2Type;
    fp.filter2Cutoff = p.filter2Cutoff;    fp.filter2Resonance = p.filter2Resonance;
    fp.filter2Drive = p.filter2Drive;      fp.filter2Mix = p.filter2Mix;
   #ifdef SPASTRIP_MOD_AUDIT
    filterModule.glideEnabled = ! filterGlideDisabledForAudit().load (std::memory_order_relaxed);
   #endif
    filterModule.process (buffer, fp);
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

    // IR shaping (start trim, decay, damping, display envelope) is the shared
    // spa-fx code; the engine, its partitioning and queue stay SPAStrip's own.
    spa::fx::ConvolveShape shape;
    shape.decay = convDecayApplied; shape.damping = convDampingApplied; shape.start = convStartApplied;
    auto shapedIR = spa::fx::shapeImpulseResponse (rawIR, rawIRSampleRate, sampleRate, shape);

    convStartTrimApplied = shapedIR.startTrim;
    irLengthSeconds.store (shapedIR.lengthSeconds, std::memory_order_relaxed);
    irEnvelope = shapedIR.envelope;
    const double sr = shapedIR.sampleRate;
    auto shaped = std::move (shapedIR.ir);

    convolution->loadImpulseResponse (std::move (shaped), sr,
                                     juce::dsp::Convolution::Stereo::yes,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::yes);
    convIrLoaded.store (true, std::memory_order_relaxed);
}

void FXChain::tapEq (const juce::AudioBuffer<float>& buffer, bool pre)
{
    if (eqTap == nullptr) return;
    tapInto (buffer, pre ? eqTap->preScope : eqTap->scope, pre ? eqTap->preScopeWrite : eqTap->scopeWrite);
}

void FXChain::tapInto (const juce::AudioBuffer<float>& buffer,
                       std::array<std::atomic<float>, Telemetry::scopeSize>& ring, std::atomic<int>& writeIdx)
{
    const int n = buffer.getNumSamples();
    const auto* l = buffer.getReadPointer (0);
    const auto* r = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : l;
    const int d = eqTapDecim;
    int w = writeIdx.load (std::memory_order_relaxed);
    for (int i = 0; i + d <= n; i += d)
    {
        float acc = 0.0f;
        for (int k = 0; k < d; ++k)
            acc += l[i + k] + r[i + k];
        ring[(size_t) w].store (0.5f * acc / (float) d, std::memory_order_relaxed);
        w = (w + 1) & (Telemetry::scopeSize - 1);
    }
    writeIdx.store (w, std::memory_order_release);
}

void FXChain::processEQ (juce::AudioBuffer<float>& buffer, const Params& p)
{
    eq.setCharacter (p.eqCharacter);
    eq.updateBands (p.eqBands);
    eq.process (buffer);
}

} // namespace spa::dsp
