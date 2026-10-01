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
    convolution.prepare (spec);
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
    convolution.reset();
    delayBuffer.clear();
    reverb.reset();
    eq.reset();
    compEffect.reset();
    grainEffect.reset();
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
    compEffect.process (buffer, mp);
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
    grainEffect.process (buffer, gp);
}

void FXChain::processConvolve (juce::AudioBuffer<float>& buffer, const Params& p)
{
    const int n = buffer.getNumSamples();
    const int numCh = juce::jmin (2, buffer.getNumChannels());

    // Wet copy through the convolution, then blend with the dry (mix + width).
    for (int ch = 0; ch < 2; ++ch)
        convScratch.copyFrom (ch, 0, buffer, juce::jmin (ch, numCh - 1), 0, n);
    auto block = juce::dsp::AudioBlock<float> (convScratch).getSubBlock (0, (size_t) n);
    convolution.process (juce::dsp::ProcessContextReplacing<float> (block));

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

    convolution.loadImpulseResponse (std::move (shaped), sr,
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
