#include "SPAStripProcessor.h"
#include "ui/SPAStripEditor.h"

#include "presets/PresetManager.h"

#include <algorithm>
#include <cmath>

namespace spa
{

namespace
{
    constexpr const char* kIRChildType = "IR";
    constexpr const char* kIRSourceNone = "none";
    constexpr const char* kIRSourceEmbedded = "embedded";

    // Fills any registry parameter missing from an incoming state with its
    // default, so a state saved by an older/other build never leaves a
    // parameter at whatever value the live processor happened to hold.
    // (Same shape as SPASynth's fillMissingParamsWithDefaults.)
    void fillMissingParamsWithDefaults (juce::ValueTree& state)
    {
        juce::StringArray present;
        for (int i = 0; i < state.getNumChildren(); ++i)
        {
            const auto child = state.getChild (i);
            if (child.hasType ("PARAM"))
                present.add (child.getProperty ("id").toString());
        }

        for (const auto& def : params::all())
        {
            if (present.contains (def.id))
                continue;

            juce::ValueTree missing ("PARAM");
            missing.setProperty ("id", def.id, nullptr);
            missing.setProperty ("value", (double) def.defaultValue, nullptr);
            state.appendChild (missing, nullptr);
        }
    }

    // GLITTER's FREEZE button became RELEASE (infinite = hold), as in SPASynth
    // 1.0.31. A state that carries fxGrain.freeze = on but no fxGrain.release
    // comes from an older build: it loads as RELEASE infinite, so the knob
    // shows what holds. Runs before fillMissingParamsWithDefaults.
    void migrateGrainFreeze (juce::ValueTree& state)
    {
        const juce::String freezeId = params::id::fx::grainFreeze, releaseId = params::id::fx::grainRelease;
        bool freezeOn = false;
        for (int i = 0; i < state.getNumChildren(); ++i)
        {
            const auto child = state.getChild (i);
            if (! child.hasType ("PARAM"))
                continue;
            const auto cid = child.getProperty ("id").toString();
            if (cid == releaseId)
                return;
            if (cid == freezeId)
                freezeOn = (double) child.getProperty ("value") >= 0.5;
        }
        if (! freezeOn)
            return;
        juce::ValueTree migrated ("PARAM");
        migrated.setProperty ("id", releaseId, nullptr);
        migrated.setProperty ("value", (double) params::grainReleaseInfinite, nullptr);
        state.appendChild (migrated, nullptr);
    }

    juce::String modSlotStateKey (int slot)
    {
        return "modSlot" + juce::String (slot + 1) + "Target";
    }

    // Below this (about -80 dB) the envelope counts as silence for the purpose of
    // choosing between the modulated (piecewise) and the plain chain path.
    constexpr float kModEnvelopeEpsilon = 1.0e-4f;

    int factorForChoice (int choice)
    {
        return 1 << juce::jlimit (0, 2, choice);
    }
}

//==============================================================================
SPAStripProcessor::SPAStripProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "PARAMS", params::createLayout())
{
    namespace fx = params::id::fx;

    const auto rp = [this] (const char* pid)
    {
        auto* p = apvts.getRawParameterValue (pid);
        jassert (p != nullptr);
        return p;
    };
    // Binds an FxFloat to its registry parameter and records it by ID, so the
    // modulation matrix can address every float FX parameter through its
    // normOffset hook (see the target table built below).
    const auto bind = [this] (FxFloat& f, const juce::String& pid)
    {
        f.raw = apvts.getRawParameterValue (pid);
        const auto* def = params::find (pid);
        jassert (f.raw != nullptr && def != nullptr);
        f.range = &def->range;
        fxBindings.push_back ({ pid, &f });
    };

    raw.inputGain = rp (params::id::inputGain);
    raw.outputGain = rp (params::id::outputGain);
    raw.mix = rp (params::id::mix);
    raw.oversampling = rp (params::id::oversampling);
    raw.scSource  = rp (params::id::sc::source);
    raw.scGain    = rp (params::id::sc::gain);
    raw.scAttack  = rp (params::id::sc::attack);
    raw.scRelease = rp (params::id::sc::release);
    raw.scHpf     = rp (params::id::sc::hpf);
    raw.scListen  = rp (params::id::sc::listen);
    for (int i = 0; i < numModSlots; ++i)
        raw.modDepth[(size_t) i] = rp (params::id::modSlotDepth (i).toRawUTF8());

    auto& r = raw.fx;
    r.distEnable = rp (fx::distEnable);
    r.distType = rp (fx::distType);
    bind (r.distDrive, fx::distDrive);
    bind (r.distTone, fx::distTone);
    bind (r.distMix, fx::distMix);

    r.chorusEnable = rp (fx::chorusEnable);
    bind (r.chorusRate, fx::chorusRate);
    bind (r.chorusDepth, fx::chorusDepth);
    bind (r.chorusFeedback, fx::chorusFeedback);
    bind (r.chorusWidth, fx::chorusWidth);
    r.chorusMode = rp (fx::chorusMode);
    bind (r.chorusMix, fx::chorusMix);
    r.chorusVhsWow      = rp (fx::chorusVhsWow);
    r.chorusVhsFlutter  = rp (fx::chorusVhsFlutter);
    r.chorusVhsTone     = rp (fx::chorusVhsTone);
    r.chorusVhsSat      = rp (fx::chorusVhsSat);
    r.chorusVhsHiss     = rp (fx::chorusVhsHiss);
    r.chorusVhsDropouts = rp (fx::chorusVhsDropouts);

    r.delayEnable = rp (fx::delayEnable);
    r.delaySync = rp (fx::delaySync);
    bind (r.delayTime, fx::delayTime);
    r.delayDivision = rp (fx::delayDivision);
    bind (r.delayFeedback, fx::delayFeedback);
    r.delayPingPong = rp (fx::delayPingPong);
    bind (r.delayWidth, fx::delayWidth);
    bind (r.delayMix, fx::delayMix);

    r.reverbEnable = rp (fx::reverbEnable);
    r.reverbMode = rp (fx::reverbMode);
    bind (r.reverbPreDelay, fx::reverbPreDelay);
    bind (r.reverbSize, fx::reverbSize);
    bind (r.reverbDecay, fx::reverbDecay);
    bind (r.reverbDamping, fx::reverbDamping);
    bind (r.reverbModDepth, fx::reverbModDepth);
    bind (r.reverbLowCut, fx::reverbLowCut);
    bind (r.reverbHighCut, fx::reverbHighCut);
    bind (r.reverbWidth, fx::reverbWidth);
    bind (r.reverbMix, fx::reverbMix);

    r.eqEnable = rp (fx::eqEnable);
    r.eqCharacter = rp (fx::eqCharacter);
    for (int b = 0; b < 8; ++b)
    {
        auto& bp = r.eqBands[(size_t) b];
        bp.enable = rp (params::id::eqBand (b, fx::eqband::enable).toRawUTF8());
        bp.type   = rp (params::id::eqBand (b, fx::eqband::type).toRawUTF8());
        bp.slope  = rp (params::id::eqBand (b, fx::eqband::slope).toRawUTF8());
        bind (bp.freq, params::id::eqBand (b, fx::eqband::freq));
        bind (bp.gain, params::id::eqBand (b, fx::eqband::gain));
        bind (bp.q, params::id::eqBand (b, fx::eqband::q));
    }

    r.modEnable = rp (fx::modEnable);
    r.modType = rp (fx::modType);
    bind (r.modRate, fx::modRate);
    r.modSync = rp (fx::modSync);
    r.modDivision = rp (fx::modDivision);
    bind (r.modDepth, fx::modDepth);
    bind (r.modFeedback, fx::modFeedback);
    r.modStages = rp (fx::modStages);
    bind (r.modCentre, fx::modCentre);
    bind (r.modManual, fx::modManual);
    bind (r.modWidth, fx::modWidth);
    bind (r.modMix, fx::modMix);

    r.tremEnable = rp (fx::tremEnable);
    bind (r.tremRate, fx::tremRate);
    r.tremSync = rp (fx::tremSync);
    r.tremDivision = rp (fx::tremDivision);
    bind (r.tremDepth, fx::tremDepth);
    r.tremShape = rp (fx::tremShape);
    bind (r.tremStereo, fx::tremStereo);
    bind (r.tremMix, fx::tremMix);
    r.vibEnable = rp (fx::vibEnable);
    bind (r.vibRate, fx::vibRate);
    r.vibSync = rp (fx::vibSync);
    r.vibDivision = rp (fx::vibDivision);
    bind (r.vibDepth, fx::vibDepth);
    bind (r.vibMix, fx::vibMix);

    r.limEnable = rp (fx::limEnable);
    bind (r.limDrive, fx::limDrive);
    bind (r.limCeiling, fx::limCeiling);
    bind (r.limRelease, fx::limRelease);
    r.limAutoRelease = rp (fx::limAutoRelease);
    r.limCharacter = rp (fx::limCharacter);
    bind (r.limStereoLink, fx::limStereoLink);
    r.limTruePeak = rp (fx::limTruePeak);
    r.limLookahead = rp (fx::limLookahead);
    r.limAutoGain = rp (fx::limAutoGain);

    r.convEnable = rp (fx::convEnable);
    bind (r.convMix, fx::convMix);
    bind (r.convWidth, fx::convWidth);
    bind (r.convPreDelay, fx::convPreDelay);
    bind (r.convDecay, fx::convDecay);
    bind (r.convDamping, fx::convDamping);
    bind (r.convStart, fx::convStart);

    r.compEnable = rp (fx::compEnable);
    bind (r.compMix, fx::compMix);
    bind (r.compXoverLow, fx::compXoverLow);
    bind (r.compXoverHigh, fx::compXoverHigh);
    for (int b = 0; b < 3; ++b)
    {
        auto& cb = r.compBands[(size_t) b];
        bind (cb.thresh, params::id::compBand (b, fx::compband::threshold));
        bind (cb.ratio, params::id::compBand (b, fx::compband::ratio));
        bind (cb.upRatio, params::id::compBand (b, fx::compband::upRatio));
        bind (cb.attack, params::id::compBand (b, fx::compband::attack));
        bind (cb.release, params::id::compBand (b, fx::compband::release));
        bind (cb.gain, params::id::compBand (b, fx::compband::gain));
        bind (cb.knee, params::id::compBand (b, fx::compband::knee));
        cb.solo   = rp (params::id::compBand (b, fx::compband::solo).toRawUTF8());
        cb.bypass = rp (params::id::compBand (b, fx::compband::bypass).toRawUTF8());
    }

    r.grainEnable = rp (fx::grainEnable);
    bind (r.grainSize, fx::grainSize);
    bind (r.grainDensity, fx::grainDensity);
    r.grainSync = rp (fx::grainSync);
    r.grainDivision = rp (fx::grainDivision);
    bind (r.grainPitch, fx::grainPitch);
    bind (r.grainSpread, fx::grainSpread);
    bind (r.grainSpreadPitch, fx::grainSpreadPitch);
    bind (r.grainPosition, fx::grainPosition);
    bind (r.grainReverse, fx::grainReverse);
    bind (r.grainFeedback, fx::grainFeedback);
    bind (r.grainMix, fx::grainMix);
    r.grainFreeze = rp (fx::grainFreeze);
    r.grainRelease = rp (fx::grainRelease);

    // SPAStripAdded: FILTER.
    r.filterEnable = rp (fx::filterEnable);
    r.filterRouting = rp (fx::filterRouting);
    r.filter1Type = rp (fx::filter1Type);
    bind (r.filter1Cutoff, fx::filter1Cutoff);
    bind (r.filter1Res, fx::filter1Res);
    bind (r.filter1Drive, fx::filter1Drive);
    bind (r.filter1Mix, fx::filter1Mix);
    r.filter2Enable = rp (fx::filter2Enable);
    r.filter2Type = rp (fx::filter2Type);
    bind (r.filter2Cutoff, fx::filter2Cutoff);
    bind (r.filter2Res, fx::filter2Res);
    bind (r.filter2Drive, fx::filter2Drive);
    bind (r.filter2Mix, fx::filter2Mix);

    // Modulation target table: index i of mod::targets() -> its FxFloat. (Every
    // float FX parameter is bound above; excluded ones simply have no entry.)
    targetFx.assign (mod::targets().size(), nullptr);
    for (size_t t = 0; t < targetFx.size(); ++t)
        for (const auto& b : fxBindings)
            if (b.id == mod::targets()[t].id)
                targetFx[t] = b.fx;
    jassert (std::none_of (targetFx.begin(), targetFx.end(), [] (const FxFloat* f) { return f == nullptr; }));
    offsetAccum.assign (targetFx.size(), 0.0f);
    for (auto& st : slotTarget)
        st.store (-1);

    apvts.state.setProperty ("fxOrder", (juce::int64) dsp::FXChain::defaultOrderPacked(), nullptr);

    irFormats = std::make_unique<juce::AudioFormatManager>();
    irFormats->registerBasicFormats();

    // Undo: every registry parameter, in registry order (a snapshot is one
    // normalised float per entry), each with the gesture / value listener.
    for (const auto& def : params::all())
    {
        auto* p = apvts.getParameter (def.id);
        jassert (p != nullptr);
        undoParams.push_back (p);
        p->addListener (&undoGestureListener);
    }

    // Constructed last: it captures the pristine state as the "Init" baseline.
    presetManager = std::make_unique<preset::PresetManager> (*this);
    midiLearn = std::make_unique<MidiLearnManager> (apvts);

#if SPASTRIP_HAS_SPA_LICENSING
    // Licence state: file reads only, on the constructing (message) thread.
    // Never any network here -- requests only follow a click in the licence
    // panel (LicenseController).
    {
        // The 14-day trial starts the first time the editor is really in a
        // window (noteEditorOpened), never here: hosts construct processors
        // while scanning. Until then: full functionality, no demo.
        auto config = spa::lic::LicenseConfig::forProduct ("spastrip");
        config.trialStart = spa::lic::LicenseConfig::TrialStart::onEditorOpen;
        licenceState = std::make_unique<spa::lic::LicenseState> (std::move (config));
    }
    licenceState->refresh();
    {
        spa::lic::ProductInfo info;
        info.productId = "spastrip";
        info.productName = "SPAStrip";
        info.serialPrefix = "STR";
        info.appVersion = SPASTRIP_VERSION;
        info.apiBaseUrl = SPA_LICENSING_API_BASE_URL;
        info.buyUrl = "https://silverplatteraudio.com";       // placeholder until the store link is final
        info.offlineUrl = juce::String (SPA_LICENSING_API_BASE_URL) + "/offline";
        // "Open my account": SPAStation Licences view if installed, else info.accountFallbackUrl.
        licenceController = std::make_unique<spa::lic::LicenseController> (*licenceState, std::move (info));
        licenceController->onLicenceChanged = [this] { licenceBroadcaster.sendChangeMessage(); };
    }
    // Demo mode blocks saving/exporting presets; loading presets and host
    // session save/restore are never affected.
    presetManager->isSaveBlocked = [this] { return isDemoActive(); };
#endif

    // Message-thread housekeeping (oversampling rebuild, latency publish,
    // non-finite flush, IR reshape), same 150 ms cadence as SPASynth.
    startTimer (150);
}

SPAStripProcessor::~SPAStripProcessor()
{
    stopTimer();
    for (auto* p : undoParams)
        p->removeListener (&undoGestureListener);
}

//==============================================================================
bool SPAStripProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();

    const bool mainOk = (in == mono && (out == mono || out == stereo))
                     || (in == stereo && out == stereo);
    if (! mainOk)
        return false;

    if (layouts.outputBuses.size() != 1)
        return false;

    // Optional sidechain: disabled, mono or stereo.
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! (sc.isDisabled() || sc == mono || sc == stereo))
            return false;
    }
    return layouts.inputBuses.size() <= 2;
}

//==============================================================================
void SPAStripProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hostSampleRate = sampleRate;
    hostBlockSize = juce::jmax (1, samplesPerBlock);
#if SPASTRIP_HAS_SPA_LICENSING
    demoGate.prepare (sampleRate);   // host domain: applied to the final output
#endif

    work.setSize (2, hostBlockSize, false, false, false);
    dryOut.setSize (2, hostBlockSize, false, false, false);
    work.clear();
    dryOut.clear();
    dryOsWork.setSize (2, hostBlockSize, false, false, false);
    dryOsWork.clear();
    scWork.setSize (2, hostBlockSize, false, false, false);
    scWork.clear();
    listenBuf.setSize (2, hostBlockSize, false, false, false);
    listenBuf.clear();
    envBuf.assign ((size_t) hostBlockSize, 0.0f);
    detector.prepare (sampleRate);
    listenSmoothed.reset (sampleRate, 0.01);
    listenSmoothed.setCurrentAndTargetValue (0.0f);
    clearModOffsets();
    slotSmoothed.fill (0.0f);

    // Dry ring: must hold the largest latency (limiter lookahead 1.5 ms plus
    // the oversampler's own few samples) with headroom.
    const int ringSize = juce::nextPowerOfTwo (juce::jmax (1024, juce::roundToInt (sampleRate * 0.01) + 512));
    dryRing.setSize (2, ringSize, false, true, false);
    dryRing.clear();
    dryRingMask = ringSize - 1;
    dryWritePos = 0;
    dryOsRing.setSize (2, ringSize, false, true, false);
    dryOsRing.clear();
    dryOsWritePos = 0;

    // Pre-allocate both oversamplers (2x, 4x) so a factor change never has to
    // allocate. Same engine settings as SPASynth: half-band polyphase IIR,
    // non-integer latency, not max quality.
    for (size_t i = 0; i < oversamplers.size(); ++i)
    {
        oversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
            2, (size_t) (i + 1),
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, false, false);
        oversamplers[i]->initProcessing ((size_t) hostBlockSize);
        oversamplers[i]->reset();

        // The dry path's twin: same filter type, same settings, same block size.
        dryOversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
            2, (size_t) (i + 1),
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, false, false);
        dryOversamplers[i]->initProcessing ((size_t) hostBlockSize);
        dryOversamplers[i]->reset();
    }

    const int factor = factorForChoice ((int) raw.oversampling->load());
    pendingOsFactor.store (factor, std::memory_order_relaxed);
    rebuildOversampling (factor);

    const auto toGain = [] (float db) { return juce::Decibels::decibelsToGain (db); };
    inputGainSmoothed.reset (sampleRate, 0.02);
    outputGainSmoothed.reset (sampleRate, 0.02);
    mixSmoothed.reset (sampleRate, 0.02);
    inputGainSmoothed.setCurrentAndTargetValue (toGain (raw.inputGain->load()));
    outputGainSmoothed.setCurrentAndTargetValue (toGain (raw.outputGain->load()));
    mixSmoothed.setCurrentAndTargetValue (juce::jlimit (0.0f, 1.0f, raw.mix->load()));

    updateFXParams();
    applyFactorLatency();
}

// Message thread (prepareToPlay, or the timer with the callback lock held).
// Prepares the chain at the oversampled rate/block size exactly like SPASynth's
// prepareEngine(hostRate * factor, hostBlock * factor).
void SPAStripProcessor::rebuildOversampling (int factor)
{
    factor = factorForChoice ((int) std::lround (std::log2 ((double) juce::jmax (1, factor))));
    currentOsFactor = factor;
    fxChain.prepare (hostSampleRate * factor, hostBlockSize * factor);
    // The limiter's lookahead is measured in engine samples, so the latency
    // inputs must be refreshed for the new rate straight away (the callers
    // hold the callback lock or run before processing starts); otherwise the
    // reported latency would lag until the next processBlock.
    updateFXParams();

    if (factor > 1 && oversamplers[(size_t) (factor == 2 ? 0 : 1)] != nullptr)
    {
        auto& os = *oversamplers[(size_t) (factor == 2 ? 0 : 1)];
        os.reset();
        if (auto* dos = dryOversamplers[(size_t) (factor == 2 ? 0 : 1)].get())
            dos->reset();
        osLatencyHost = (int) std::ceil (os.getLatencyInSamples());
    }
    else
    {
        osLatencyHost = 0;
    }
}

int SPAStripProcessor::getCurrentLatencySamples() const
{
    // Limiter lookahead is measured in engine samples; convert to host rate.
    return lookaheadLatencyHost() + osLatencyHost;
}

void SPAStripProcessor::applyFactorLatency()
{
    const int lat = getCurrentLatencySamples();
    if (lat != getLatencySamples())
        setLatencySamples (lat);
}

void SPAStripProcessor::serviceMessageThread()
{
    // Apply a pending oversampling-factor change: re-prepare the chain at the
    // new rate under the callback lock so no processBlock touches it mid-way.
    if (const int pf = pendingOsFactor.load (std::memory_order_relaxed);
        pf != currentOsFactor && work.getNumSamples() > 0)
    {
        const juce::ScopedLock sl (getCallbackLock());
        rebuildOversampling (pf);
        std::fill (dryRing.getWritePointer (0), dryRing.getWritePointer (0) + dryRing.getNumSamples(), 0.0f);
        std::fill (dryRing.getWritePointer (1), dryRing.getWritePointer (1) + dryRing.getNumSamples(), 0.0f);
        dryOsRing.clear();
    }

    // Non-finite flush request (processBlock already silenced the block):
    // reset the chain's stateful DSP so a poisoned feedback ring cannot keep
    // re-emitting garbage.
    if (fxStateFlushPending.exchange (false, std::memory_order_relaxed))
    {
        const juce::ScopedLock sl (getCallbackLock());
        fxChain.reset();
        for (auto& os : oversamplers)
            if (os != nullptr)
                os->reset();
        for (auto& os : dryOversamplers)
            if (os != nullptr)
                os->reset();
        dryRing.clear();
        dryOsRing.clear();
        detector.reset();
    }

    // Report latency: limiter lookahead (engine -> host samples) plus the
    // oversampler's own latency.
    applyFactorLatency();

    // A control destroyed mid-drag never sends its gesture end; close the step.
    watchdogCloseStaleUndoStep();
    // Parameter edits (including host automation) may have moved the patch away
    // from, or back to, the loaded preset: re-evaluate the edited flag.
    if (editedCheckPending.exchange (false, std::memory_order_relaxed))
        presetManager->refreshEditedState();

    // Convolution IR shaping (decay/damping/start) reshapes + reloads the IR;
    // done here (message thread), only when the values actually move.
    fxChain.setConvolutionShaping (raw.fx.convDecay.raw->load(), raw.fx.convDamping.raw->load(),
                                   raw.fx.convStart.raw->load());
}

//==============================================================================
void SPAStripProcessor::resolveTempo()
{
    double bpm = 120.0;
    if (auto* host = getPlayHead())
        if (const auto position = host->getPosition())
            if (const auto hostBpm = position->getBpm())
                if (std::isfinite (*hostBpm) && *hostBpm > 0.0)
                    bpm = *hostBpm;
    blockBpm = bpm;
    telemetry.bpm.store ((float) bpm, std::memory_order_relaxed);
}

void SPAStripProcessor::delayThroughRing (juce::AudioBuffer<float>& ring, int& writePos,
                                          const juce::AudioBuffer<float>& src,
                                          juce::AudioBuffer<float>& dst,
                                          int numSamples, int delaySamples)
{
    delaySamples = juce::jlimit (0, dryRingMask, delaySamples);
    const int startPos = writePos;
    for (int ch = 0; ch < 2; ++ch)
    {
        float* r = ring.getWritePointer (ch);
        const float* in = src.getReadPointer (ch);
        float* out = dst.getWritePointer (ch);
        int w = startPos;
        for (int i = 0; i < numSamples; ++i)
        {
            r[w] = in[i];
            out[i] = r[(w - delaySamples) & dryRingMask];
            w = (w + 1) & dryRingMask;
        }
    }
    writePos = (startPos + numSamples) & dryRingMask;
}

void SPAStripProcessor::pushAndReadDry (int numSamples, int delaySamples)
{
    delayThroughRing (dryRing, dryWritePos, work, dryOut, numSamples, delaySamples);
}

bool SPAStripProcessor::supportsMidiLearn() const
{
   #if SPASTRIP_AU_MIDI
    return true;   // this target builds only the MusicEffect AU, which Logic feeds MIDI
   #else
    return midiLearnForcedForTest || wrapperType == wrapperType_VST3;
   #endif
}

void SPAStripProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // MIDI Learn first, so a CC lands in the parameters this block reads.
    // Lock-free (see MidiLearnManager). An effect produces no MIDI.
    if (! midi.isEmpty())
    {
        if (supportsMidiLearn())
            midiLearn->processMidi (midi);
        midi.clear();
    }

    // NOTE: unlike the synth, the buffer is NOT cleared -- this is an effect.
    if (work.getNumSamples() == 0)   // processBlock without prepareToPlay
        return;

    resolveTempo();
    pendingOsFactor.store (factorForChoice ((int) raw.oversampling->load()), std::memory_order_relaxed);

    const int total = buffer.getNumSamples();
    for (int pos = 0; pos < total; pos += hostBlockSize)
        processChunk (buffer, pos, juce::jmin (hostBlockSize, total - pos), false);

#if SPASTRIP_HAS_SPA_LICENSING
    // Demo mode (trial over, no licence): ~1.5 s faded silence about every
    // 60 s, the very last thing before the host gets the buffer (after every
    // per-chunk safety stage). Not active -> returns immediately, output
    // bit-identical. Host bypass (processBlockBypassed) is never gated: it is
    // the dry signal anyway.
    demoGate.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(),
                      licenceState->demoActive());
#endif
}

void SPAStripProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (work.getNumSamples() == 0)
        return;

    resolveTempo();
    pendingOsFactor.store (factorForChoice ((int) raw.oversampling->load()), std::memory_order_relaxed);

    const int total = buffer.getNumSamples();
    for (int pos = 0; pos < total; pos += hostBlockSize)
        processChunk (buffer, pos, juce::jmin (hostBlockSize, total - pos), true);
}

//==============================================================================
// Modulation matrix.
void SPAStripProcessor::clearModOffsets()
{
    for (auto* fx : targetFx)
        fx->normOffset = 0.0f;
    modOffsetsApplied = false;
}

// Per slot: offset = depth x envelope, slewed (modSmoothingMs). Summed per
// target and handed to each target's FxFloat::normOffset (audio thread; the
// caller then runs updateFXParams()). FxFloat::get() adds it to the base
// normalised value, clamps to 0..1 and converts through the parameter's own
// range. Host-visible values are never touched.
void SPAStripProcessor::applyModOffsets (float envelope, int pieceHostSamples)
{
    const float tauMs = modSmoothingMs.load (std::memory_order_relaxed);
    const float alpha = tauMs > 0.0f
        ? 1.0f - std::exp (-(float) pieceHostSamples / (tauMs * 0.001f * (float) hostSampleRate))
        : 1.0f;

    std::fill (offsetAccum.begin(), offsetAccum.end(), 0.0f);
    float slotOffsets[numModSlots] = {};
    for (int i = 0; i < numActiveSlots; ++i)
    {
        const auto& a = activeSlots[(size_t) i];
        float& sm = slotSmoothed[(size_t) a.slot];
        sm += (a.depth * envelope - sm) * alpha;
        slotOffsets[a.slot] = sm;
        offsetAccum[(size_t) a.target] += sm;
    }
    for (size_t t = 0; t < targetFx.size(); ++t)
        targetFx[t]->normOffset = offsetAccum[t];
    for (int s = 0; s < numModSlots; ++s)
        telemetry.modSlotOffset[(size_t) s].store (slotOffsets[s], std::memory_order_relaxed);
    modOffsetsApplied = true;
}

void SPAStripProcessor::processChain (float* const* chans, int engineSamples, int factor,
                                      int hostSamples, bool modActive)
{
    if (! modActive)
    {
        chainInvocations.fetch_add (1, std::memory_order_relaxed);
        juce::AudioBuffer<float> buf (chans, 2, engineSamples);
        fxChain.process (buf, fxParams);
        return;
    }

    modulatedChunks.fetch_add (1, std::memory_order_relaxed);

    const int step = juce::jlimit (1, 32, modUpdateInterval.load (std::memory_order_relaxed));
    for (int a = 0; a < hostSamples; a += step)
    {
        const int b = juce::jmin (hostSamples, a + step);
        // The envelope at the middle of the piece.
        applyModOffsets (envBuf[(size_t) ((a + b - 1) / 2)], b - a);
        updateFXParams();

        chainInvocations.fetch_add (1, std::memory_order_relaxed);
        float* pc[2] = { chans[0] + a * factor, chans[1] + a * factor };
        juce::AudioBuffer<float> piece (pc, 2, (b - a) * factor);
        fxChain.process (piece, fxParams);
    }
}

void SPAStripProcessor::processChunk (juce::AudioBuffer<float>& hostBuffer, int startSample,
                                      int n, bool bypassed)
{
    float* wL = work.getWritePointer (0);
    float* wR = work.getWritePointer (1);

    // --- 1. Main input -> stereo working buffer (mono is duplicated), and the
    // sidechain bus -> its own stereo scratch. BOTH are read here, before any
    // output sample of this chunk is written: in layouts such as mono in ->
    // stereo out the sidechain's host-buffer channels are also main OUTPUT
    // channels.
    bool scBusActive = false;
    {
        const auto inBus = getBusBuffer (hostBuffer, true, 0);
        const int nIn = inBus.getNumChannels();
        if (nIn >= 1)
        {
            const float* in0 = inBus.getReadPointer (0) + startSample;
            const float* in1 = nIn > 1 ? inBus.getReadPointer (1) + startSample : in0;
            std::copy (in0, in0 + n, wL);
            std::copy (in1, in1 + n, wR);
        }
        else
        {
            std::fill (wL, wL + n, 0.0f);
            std::fill (wR, wR + n, 0.0f);
        }

        if (! bypassed && getBusCount (true) > 1)
        {
            const auto scBus = getBusBuffer (hostBuffer, true, 1);
            const int nSc = scBus.getNumChannels();
            if (nSc >= 1)
            {
                const float* s0 = scBus.getReadPointer (0) + startSample;
                const float* s1 = nSc > 1 ? scBus.getReadPointer (1) + startSample : s0;
                std::copy (s0, s0 + n, scWork.getWritePointer (0));
                std::copy (s1, s1 + n, scWork.getWritePointer (1));
                scBusActive = true;
            }
        }
    }

    // SPAStripAdded: pre-chain input peak for the editor's input meter.
    {
        float ipL = 0.0f, ipR = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            ipL = juce::jmax (ipL, std::abs (wL[i]));
            ipR = juce::jmax (ipR, std::abs (wR[i]));
        }
        telemetry.inPeakL.store (ipL, std::memory_order_relaxed);
        telemetry.inPeakR.store (ipR, std::memory_order_relaxed);
    }

    // --- 2. updateFXParams (also feeds the latency used by the dry path). Any
    // modulation offset left over from the previous chunk is dropped first, so
    // this chunk starts from the base (host-visible) values.
    if (modOffsetsApplied)
        clearModOffsets();
    updateFXParams();
    const int latency = getCurrentLatencySamples();

    // --- 3. Dry tap (pre input gain), delayed by the reported latency.
    pushAndReadDry (n, latency);

    const int factor = currentOsFactor;
    const bool useOs = factor > 1 && oversamplers[(size_t) (factor == 2 ? 0 : 1)] != nullptr
                       && dryOversamplers[(size_t) (factor == 2 ? 0 : 1)] != nullptr;

    // With oversampling the MIX's dry signal takes its own trip through an
    // identical oversampler (up, straight back down), so it shares the wet
    // path's phase response instead of only an integer delay. Kept running
    // while bypassed too, so its state is always current. In bypass the plain
    // integer-delayed input (dryOut from the raw ring) is what is output.
    if (useOs)
    {
        auto& dos = *dryOversamplers[(size_t) (factor == 2 ? 0 : 1)];
        std::copy (wL, wL + n, dryOsWork.getWritePointer (0));
        std::copy (wR, wR + n, dryOsWork.getWritePointer (1));
        float* dc[2] = { dryOsWork.getWritePointer (0), dryOsWork.getWritePointer (1) };
        juce::dsp::AudioBlock<float> dryBlock (dc, 2, (size_t) n);
        dos.processSamplesUp (dryBlock);
        dos.processSamplesDown (dryBlock);
        // dryOsWork is now the dry tap delayed by the oversampler's own latency;
        // add the limiter-lookahead part (integer host samples).
        delayThroughRing (dryOsRing, dryOsWritePos, dryOsWork, bypassed ? dryOsWork : dryOut,
                          n, lookaheadLatencyHost());
    }

    const float mixTarget = juce::jlimit (0.0f, 1.0f, raw.mix->load());

    bool listening = false;

    if (bypassed)
    {
        // Bypass: the dry signal, delayed by the reported latency. No gains,
        // no mix, no chain. Track the smoothers so there is no ramp on exit.
        inputGainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (raw.inputGain->load()));
        outputGainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (raw.outputGain->load()));
        mixSmoothed.setCurrentAndTargetValue (mixTarget);
        listenSmoothed.setCurrentAndTargetValue (0.0f);
        std::copy (dryOut.getReadPointer (0), dryOut.getReadPointer (0) + n, wL);
        std::copy (dryOut.getReadPointer (1), dryOut.getReadPointer (1) + n, wR);
        telemetry.scEnvelope.store (0.0f, std::memory_order_relaxed);
        for (auto& o : telemetry.modSlotOffset)
            o.store (0.0f, std::memory_order_relaxed);
    }
    else
    {
        // --- 4. Input gain.
        inputGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (raw.inputGain->load()));
        if (inputGainSmoothed.isSmoothing() || inputGainSmoothed.getTargetValue() != 1.0f)
        {
            for (int i = 0; i < n; ++i)
            {
                const float g = inputGainSmoothed.getNextValue();
                wL[i] *= g;
                wR[i] *= g;
            }
        }

        // EQ analyzer PRE tap: the signal entering the chain.
        {
            int w = telemetry.preScopeWrite.load (std::memory_order_relaxed);
            for (int i = 0; i < n; ++i)
            {
                telemetry.preScope[(size_t) w].store (0.5f * (wL[i] + wR[i]), std::memory_order_relaxed);
                w = (w + 1) & (dsp::Telemetry::scopeSize - 1);
            }
            telemetry.preScopeWrite.store (w, std::memory_order_release);
        }

        // --- 4b. Sidechain detector. Source "Input" is the main input AFTER the
        // input gain (and before the chain); "External" is the sidechain bus,
        // which is simply absent (envelope 0) when the host has it disabled.
        // Always run -- it feeds the envelope meter and sc.listen -- but it only
        // changes the audio when a slot is active or listen is on.
        {
            const bool useInput = (int) raw.scSource->load() == 1;
            listening = raw.scListen->load() >= 0.5f;
            const float* dL = nullptr;
            const float* dR = nullptr;
            if (useInput)      { dL = wL; dR = wR; }
            else if (scBusActive) { dL = scWork.getReadPointer (0); dR = scWork.getReadPointer (1); }

            float* lL = listenBuf.getWritePointer (0);
            float* lR = listenBuf.getWritePointer (1);
            telemetry.scPresent.store (dL != nullptr, std::memory_order_relaxed);
            if (dL != nullptr)
            {
                detector.setSettings ({ raw.scGain->load(), raw.scAttack->load(),
                                        raw.scRelease->load(), raw.scHpf->load() });
                detector.process (dL, dR, n, envBuf.data(), listening ? lL : nullptr, listening ? lR : nullptr);
            }
            else
            {
                detector.reset();
                std::fill (envBuf.begin(), envBuf.begin() + n, 0.0f);
                std::fill (lL, lL + n, 0.0f);
                std::fill (lR, lR + n, 0.0f);
            }
            telemetry.scEnvelope.store (envBuf[(size_t) (n - 1)], std::memory_order_relaxed);
        }

        // Active slots: an assigned target and a non-zero depth (or a slewed
        // offset that has not yet glided back to zero).
        numActiveSlots = 0;
        for (int s = 0; s < numModSlots; ++s)
        {
            const int t = slotTarget[(size_t) s].load (std::memory_order_relaxed);
            const float d = raw.modDepth[(size_t) s]->load();
            if (t >= 0 && t < (int) targetFx.size()
                && (d != 0.0f || std::abs (slotSmoothed[(size_t) s]) > 1.0e-4f))
                activeSlots[(size_t) numActiveSlots++] = { t, juce::jlimit (-1.0f, 1.0f, d), s };
            else
                slotSmoothed[(size_t) s] = 0.0f;
        }
        // With no envelope (e.g. External source and the host's sidechain bus
        // disabled) and nothing left to glide, every offset is zero: the plain,
        // phase-1 path then runs, so the output is bit-identical to a plugin
        // without the matrix.
        //
        // SPAStripAdded: the test is on the largest offset a slot can reach in this
        // chunk (|depth| x envelope peak), not on the envelope alone, so a slot with
        // a tiny depth (a knob just leaving 0) or a nearly-silent sidechain (host
        // noise floor) does not pay for the subdivided path to apply an offset of
        // < kModEnvelopeEpsilon (1e-4 of the parameter's range: inaudible). The
        // subdivided path costs one chain call per kDefaultModUpdateInterval host
        // samples, which at 4x with a long convolution is the expensive one.
        bool modActive = false;
        if (numActiveSlots > 0)
        {
            float envMax = 0.0f;
            for (int i = 0; i < n; ++i)
                envMax = juce::jmax (envMax, envBuf[(size_t) i]);
            for (int i = 0; i < numActiveSlots && ! modActive; ++i)
            {
                const auto& a = activeSlots[(size_t) i];
                modActive = std::abs (a.depth) * envMax > kModEnvelopeEpsilon
                            || std::abs (slotSmoothed[(size_t) a.slot]) > kModEnvelopeEpsilon;
            }
        }
        if (! modActive)
        {
            slotSmoothed.fill (0.0f);
            for (auto& o : telemetry.modSlotOffset)
                o.store (0.0f, std::memory_order_relaxed);
        }

        // --- 5. (oversample up) -> chain -> telemetry -> (oversample down).
        // The chain runs at the (possibly oversampled) engine rate, so its
        // CPU cost scales with the factor, as in SPASynth. While a modulation
        // slot is active it runs in pieces of <= modUpdateInterval host samples.
        if (useOs)
        {
            auto& os = *oversamplers[(size_t) (factor == 2 ? 0 : 1)];
            float* hostChans[2] = { wL, wR };
            juce::dsp::AudioBlock<float> hostBlock (hostChans, 2, (size_t) n);
            auto osBlock = os.processSamplesUp (hostBlock);
            float* osChans[2] = { osBlock.getChannelPointer (0), osBlock.getChannelPointer (1) };
            processChain (osChans, (int) osBlock.getNumSamples(), factor, n, modActive);
            fxChain.publishTelemetry (telemetry);
            os.processSamplesDown (hostBlock);
        }
        else
        {
            float* chans[2] = { wL, wR };
            processChain (chans, n, 1, n, modActive);
            fxChain.publishTelemetry (telemetry);
        }

        // --- 6. Global mix against the latency-compensated dry copy.
        mixSmoothed.setTargetValue (mixTarget);
        if (mixSmoothed.isSmoothing() || mixSmoothed.getTargetValue() < 1.0f)
        {
            const float* dL = dryOut.getReadPointer (0);
            const float* dR = dryOut.getReadPointer (1);
            for (int i = 0; i < n; ++i)
            {
                const float m = mixSmoothed.getNextValue();
                wL[i] = wL[i] * m + dL[i] * (1.0f - m);
                wR[i] = wR[i] * m + dR[i] * (1.0f - m);
            }
        }

        // --- 7. Output gain.
        outputGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (raw.outputGain->load()));
        if (outputGainSmoothed.isSmoothing() || outputGainSmoothed.getTargetValue() != 1.0f)
        {
            for (int i = 0; i < n; ++i)
            {
                const float g = outputGainSmoothed.getNextValue();
                wL[i] *= g;
                wR[i] *= g;
            }
        }

        // --- 7b. sc.listen: audition the detector signal (post HPF and gain)
        // in place of the processed signal. The chain keeps running underneath
        // so nothing glitches when listen is switched off; the swap is
        // cross-faded over 10 ms.
        listenSmoothed.setTargetValue (listening ? 1.0f : 0.0f);
        if (listenSmoothed.isSmoothing() || listenSmoothed.getTargetValue() > 0.0f)
        {
            const float* lL = listenBuf.getReadPointer (0);
            const float* lR = listenBuf.getReadPointer (1);
            for (int i = 0; i < n; ++i)
            {
                const float k = listenSmoothed.getNextValue();
                wL[i] += (lL[i] - wL[i]) * k;
                wR[i] += (lR[i] - wR[i]) * k;
            }
        }
    }

    // --- 8. Non-finite flush: a NaN/Inf landing in a feedback structure never
    // decays on its own, so silence this block outright and ask the timer to
    // reset the chain (off the audio thread). Same policy as SPASynth.
    {
        bool bad = false;
        for (int i = 0; i < n; ++i)
            if (! std::isfinite (wL[i]) || ! std::isfinite (wR[i])) { bad = true; break; }
        if (bad)
        {
            std::fill (wL, wL + n, 0.0f);
            std::fill (wR, wR + n, 0.0f);
            fxStateFlushPending.store (true, std::memory_order_relaxed);
        }
    }

    // --- 9. Telemetry (post everything): peaks, analyzer scope, limiter history.
    {
        float pkL = 0.0f, pkR = 0.0f;
        int w = telemetry.scopeWrite.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            pkL = juce::jmax (pkL, std::abs (wL[i]));
            pkR = juce::jmax (pkR, std::abs (wR[i]));
            telemetry.scope[(size_t) w].store (0.5f * (wL[i] + wR[i]), std::memory_order_relaxed);
            w = (w + 1) & (dsp::Telemetry::scopeSize - 1);
        }
        telemetry.scopeWrite.store (w, std::memory_order_release);
        telemetry.peakL.store (pkL, std::memory_order_relaxed);
        telemetry.peakR.store (pkR, std::memory_order_relaxed);

        // When the limiter is off the master level still scrolls (zero
        // reduction) so the display lives -- as in SPASynth.
        const bool limOn = ! bypassed && fxParams.limEnable;
        const float outLvl = limOn ? fxChain.limiterOutputPeak() : juce::jmax (pkL, pkR);
        const float grDb = limOn ? fxChain.limiterGainReductionDb() : 0.0f;
        const int lw = telemetry.limWrite.load (std::memory_order_relaxed);
        telemetry.limOut[(size_t) lw].store (outLvl, std::memory_order_relaxed);
        telemetry.limGrDb[(size_t) lw].store (grDb, std::memory_order_relaxed);
        telemetry.limWrite.store ((lw + 1) % dsp::Telemetry::limiterHistory, std::memory_order_release);
    }

    // --- 10. Stereo working buffer -> main output (mono out is the L/R
    // average). Inputs were fully consumed in step 1, so writing the output
    // channels in place is safe even though the sidechain input shares the
    // host buffer's channel space.
    {
        auto outBus = getBusBuffer (hostBuffer, false, 0);
        const int nOut = outBus.getNumChannels();
        if (nOut == 1)
        {
            float* o0 = outBus.getWritePointer (0) + startSample;
            for (int i = 0; i < n; ++i)
                o0[i] = 0.5f * (wL[i] + wR[i]);
        }
        else if (nOut >= 2)
        {
            std::copy (wL, wL + n, outBus.getWritePointer (0) + startSample);
            std::copy (wR, wR + n, outBus.getWritePointer (1) + startSample);
            for (int ch = 2; ch < nOut; ++ch)
                std::fill (outBus.getWritePointer (ch) + startSample,
                           outBus.getWritePointer (ch) + startSample + n, 0.0f);
        }
    }
}

//==============================================================================
// Converts the raw parameter atomics into FXChain::Params once per block. Every
// float goes through FxFloat::get(), so a later per-block normalized offset
// (set on FxFloat::normOffset before this runs) is applied to ANY float FX
// parameter before it is converted and written into FXChain::Params.
void SPAStripProcessor::updateFXParams()
{
    const auto& rf = raw.fx;
    auto& p = fxParams;

    dsp::FXChain::unpackOrder (fxOrderPacked.load (std::memory_order_relaxed), p.order);

    p.distEnable     = rf.distEnable->load() >= 0.5f;
    p.distType       = (int) rf.distType->load();
    p.distDrive      = rf.distDrive.get();
    p.distToneHz     = rf.distTone.get();
    p.distMix        = rf.distMix.get();
    p.chorusEnable   = rf.chorusEnable->load() >= 0.5f;
    p.chorusRate     = rf.chorusRate.get();
    p.chorusDepth    = rf.chorusDepth.get();
    p.chorusFeedback = rf.chorusFeedback.get();
    // The only percent -> 0..1 conversion for chorus width: the registry
    // carries it as 0..100 % (that is how the knob reads), the DSP wants 0..1.
    p.chorusWidth    = rf.chorusWidth.get() * 0.01f;
    p.chorusMode     = (int) rf.chorusMode->load();
    p.chorusMix      = rf.chorusMix.get();
    p.chorusVhsWow      = rf.chorusVhsWow->load() * 0.01f;      // registry %, DSP 0..1
    p.chorusVhsFlutter  = rf.chorusVhsFlutter->load() * 0.01f;
    p.chorusVhsTone     = rf.chorusVhsTone->load() * 0.01f;
    p.chorusVhsSat      = rf.chorusVhsSat->load() * 0.01f;
    p.chorusVhsHiss     = rf.chorusVhsHiss->load() * 0.01f;
    p.chorusVhsDropouts = rf.chorusVhsDropouts->load() * 0.01f;
    p.delayEnable    = rf.delayEnable->load() >= 0.5f;
    p.delaySync      = rf.delaySync->load() >= 0.5f;
    p.delayTimeMs    = rf.delayTime.get();
    p.delayDivision  = (int) rf.delayDivision->load();
    p.delayFeedback  = rf.delayFeedback.get();
    p.delayPingPong  = rf.delayPingPong->load() >= 0.5f;
    // Same percent -> 0..1 conversion as chorusWidth.
    p.delayWidth     = rf.delayWidth.get() * 0.01f;
    p.delayMix       = rf.delayMix.get();
    p.reverbEnable   = rf.reverbEnable->load() >= 0.5f;
    p.reverbMode     = (int) rf.reverbMode->load();
    p.reverbPreDelay = rf.reverbPreDelay.get();
    p.reverbSize     = rf.reverbSize.get();
    p.reverbDecay    = rf.reverbDecay.get();
    p.reverbDamping  = rf.reverbDamping.get();
    p.reverbModDepth = rf.reverbModDepth.get();
    p.reverbLowCut   = rf.reverbLowCut.get();
    p.reverbHighCut  = rf.reverbHighCut.get();
    p.reverbWidth    = rf.reverbWidth.get();
    p.reverbMix      = rf.reverbMix.get();
    p.eqEnable       = rf.eqEnable->load() >= 0.5f;
    p.eqCharacter    = (int) rf.eqCharacter->load();
    for (int b = 0; b < 8; ++b)
    {
        const auto& bp = rf.eqBands[(size_t) b];
        auto& band = p.eqBands[(size_t) b];
        band.enabled = bp.enable->load() >= 0.5f;
        band.type    = (int) bp.type->load();
        band.slope   = (int) bp.slope->load();
        band.freq    = bp.freq.get();
        band.gainDb  = bp.gain.get();
        band.q       = bp.q.get();
    }

    p.modEnable   = rf.modEnable->load() >= 0.5f;
    p.modType     = (int) rf.modType->load();
    p.modRate     = rf.modRate.get();
    p.modSync     = rf.modSync->load() >= 0.5f;
    p.modDivision = (int) rf.modDivision->load();
    p.modDepth    = rf.modDepth.get();
    p.modFeedback = rf.modFeedback.get();
    {
        static constexpr int stageCounts[] = { 2, 4, 6, 8, 12 };
        p.modStages = stageCounts[juce::jlimit (0, 4, (int) rf.modStages->load())];
    }
    p.modCentreHz = rf.modCentre.get();
    p.modManualMs = rf.modManual.get();
    p.modWidth    = rf.modWidth.get();
    p.modMix      = rf.modMix.get();

    p.tremEnable   = rf.tremEnable->load() >= 0.5f;
    p.tremRate     = rf.tremRate.get();
    p.tremSync     = rf.tremSync->load() >= 0.5f;
    p.tremDivision = (int) rf.tremDivision->load();
    p.tremDepth    = rf.tremDepth.get();
    p.tremShape    = (int) rf.tremShape->load();
    p.tremStereo   = rf.tremStereo.get();
    p.tremMix      = rf.tremMix.get();
    p.vibEnable    = rf.vibEnable->load() >= 0.5f;
    p.vibRate      = rf.vibRate.get();
    p.vibSync      = rf.vibSync->load() >= 0.5f;
    p.vibDivision  = (int) rf.vibDivision->load();
    p.vibDepth     = rf.vibDepth.get();
    p.vibMix       = rf.vibMix.get();

    p.limEnable      = rf.limEnable->load() >= 0.5f;
    p.limDrive       = rf.limDrive.get();
    p.limCeiling     = rf.limCeiling.get();
    p.limRelease     = rf.limRelease.get();
    p.limAutoRelease = rf.limAutoRelease->load() >= 0.5f;
    p.limCharacter   = (int) rf.limCharacter->load();
    p.limStereoLink  = rf.limStereoLink.get();
    p.limTruePeak    = rf.limTruePeak->load() >= 0.5f;
    p.limLookahead   = rf.limLookahead->load() >= 0.5f;
    p.limAutoGain    = rf.limAutoGain->load() >= 0.5f;

    p.convEnable   = rf.convEnable->load() >= 0.5f;
    p.convMix      = rf.convMix.get();
    p.convWidth    = rf.convWidth.get();
    p.convPreDelay = rf.convPreDelay.get();
    p.convDecay    = rf.convDecay.get();
    p.convDamping  = rf.convDamping.get();
    p.convStart    = rf.convStart.get();

    p.compEnable        = rf.compEnable->load() >= 0.5f;
    p.compMix           = rf.compMix.get();
    p.compCrossoverLow  = rf.compXoverLow.get();
    p.compCrossoverHigh = rf.compXoverHigh.get();
    for (int b = 0; b < 3; ++b)
    {
        const auto& cb = rf.compBands[(size_t) b];
        auto& band = p.compBands[(size_t) b];
        band.thresholdDb = cb.thresh.get();
        band.ratio       = cb.ratio.get();
        band.upRatio     = cb.upRatio.get();
        band.attackMs    = cb.attack.get();
        band.releaseMs   = cb.release.get();
        band.gainDb      = cb.gain.get();
        band.kneeDb      = cb.knee.get();
        band.solo        = cb.solo->load() >= 0.5f;
        band.bypass      = cb.bypass->load() >= 0.5f;
    }

    p.grainEnable      = rf.grainEnable->load() >= 0.5f;
    p.grainSizeMs      = rf.grainSize.get();
    p.grainDensityHz   = rf.grainDensity.get();
    p.grainSync        = rf.grainSync->load() >= 0.5f;
    p.grainDivision    = (int) rf.grainDivision->load();
    p.grainPitch       = rf.grainPitch.get();
    p.grainSpread      = rf.grainSpread.get();
    p.grainSpreadPitch = rf.grainSpreadPitch.get();
    p.grainPositionMs  = rf.grainPosition.get();
    p.grainReverse     = rf.grainReverse.get();
    p.grainFeedback    = rf.grainFeedback.get();
    p.grainMix         = rf.grainMix.get();
    p.grainFreeze      = rf.grainFreeze->load() >= 0.5f;
    p.grainReleaseSec  = rf.grainRelease->load();

    // SPAStripAdded: FILTER.
    p.filterEnable     = rf.filterEnable->load() >= 0.5f;
    p.filterRouting    = (int) rf.filterRouting->load();
    p.filter1Type      = (int) rf.filter1Type->load();
    p.filter1Cutoff    = rf.filter1Cutoff.get();
    p.filter1Resonance = rf.filter1Res.get();
    p.filter1Drive     = rf.filter1Drive.get();
    p.filter1Mix       = rf.filter1Mix.get();
    p.filter2Enable    = rf.filter2Enable->load() >= 0.5f;
    p.filter2Type      = (int) rf.filter2Type->load();
    p.filter2Cutoff    = rf.filter2Cutoff.get();
    p.filter2Resonance = rf.filter2Res.get();
    p.filter2Drive     = rf.filter2Drive.get();
    p.filter2Mix       = rf.filter2Mix.get();

    desiredLatency.store (fxChain.limiterLatencySamples (p), std::memory_order_relaxed);
    p.bpm = blockBpm;
}

//==============================================================================
void SPAStripProcessor::setFxOrder (const juce::Array<int>& moduleIds)
{
    if (moduleIds.size() != dsp::FXChain::numModules)
        return;
    UndoStep undoScope (*this, "FX ORDER");
    dsp::FXChain::Module ord[dsp::FXChain::numModules];
    for (int i = 0; i < dsp::FXChain::numModules; ++i)
        ord[i] = (dsp::FXChain::Module) moduleIds[i];
    const auto packed = dsp::FXChain::packOrder (ord);
    fxOrderPacked.store (packed, std::memory_order_relaxed);
    apvts.state.setProperty ("fxOrder", (juce::int64) packed, nullptr);
}

juce::Array<int> SPAStripProcessor::getFxOrder() const
{
    dsp::FXChain::Module ord[dsp::FXChain::numModules];
    dsp::FXChain::unpackOrder (fxOrderPacked.load (std::memory_order_relaxed), ord);
    juce::Array<int> ids;
    for (auto m : ord)
        ids.add ((int) m);
    return ids;
}

//==============================================================================
// Convolve IR.
bool SPAStripProcessor::loadConvolutionIR (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;

    UndoStep undoScope (*this, "LOAD IR");
    std::unique_ptr<juce::AudioFormatReader> reader (irFormats->createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return false;

    // Same cap and channel handling as SPASynth's FXChain::loadConvolutionIR.
    const int maxSamples = (int) (reader->sampleRate * kMaxIRSeconds);
    const int n = (int) juce::jmin ((juce::int64) maxSamples, reader->lengthInSamples);
    const int numCh = (int) juce::jmin ((juce::uint32) 2, reader->numChannels);
    juce::AudioBuffer<float> ir (numCh, n);
    reader->read (&ir, 0, n, 0, true, true);

    // The convolution engine normalises the IR and the shaping (decay /
    // damping / start) is linear, so absolute level is irrelevant: peak-
    // normalise to just under full scale so the 24-bit FLAC keeps maximum
    // resolution (and a float IR that exceeds +-1 cannot clip).
    const float peak = ir.getMagnitude (0, n);
    if (! (peak > 0.0f) || ! std::isfinite (peak))
        return false;
    ir.applyGain (0.98f / peak);

    StoredIR s;
    s.source = kIRSourceEmbedded;
    s.name = file.getFileNameWithoutExtension();
    s.sampleRate = reader->sampleRate;
    s.numChannels = numCh;
    s.numSamples = n;
    {
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::OutputStream> mos = std::make_unique<juce::MemoryOutputStream> (s.flac, false);
        auto writer = flac.createWriterFor (mos, juce::AudioFormatWriterOptions()
                                                     .withSampleRate (reader->sampleRate)
                                                     .withNumChannels (numCh)
                                                     .withBitsPerSample (24)
                                                     .withQualityOptionIndex (5));
        if (writer == nullptr)
            return false;
        if (! writer->writeFromAudioSampleBuffer (ir, 0, n))
            return false;
        writer.reset();   // flushes into s.flac (the writer owns and deletes the stream)
    }
    if (s.flac.getSize() == 0)
        return false;

    {
        auto built = std::make_shared<const StoredIR> (std::move (s));
        const juce::ScopedLock sl (irLock);
        storedIR = std::move (built);
    }
    loadStoredIRIntoChain();
    return true;
}

bool SPAStripProcessor::loadFactoryIR (const juce::String& id)
{
    const auto* info = factory::find (id);
    if (info == nullptr)
        return false;

    UndoStep undoScope (*this, "FACTORY IR");
    juce::AudioBuffer<float> ir;
    double irRate = 0.0;
    if (! factory::decode (id, ir, irRate))
        return false;

    StoredIR s;
    s.source = juce::String (factory::sourcePrefix) + info->id;
    s.name = info->name;
    s.sampleRate = irRate;
    s.numChannels = ir.getNumChannels();
    s.numSamples = ir.getNumSamples();
    {
        auto built = std::make_shared<const StoredIR> (std::move (s));
        const juce::ScopedLock sl (irLock);
        storedIR = std::move (built);
    }
    loadStoredIRIntoChain();
    return true;
}

void SPAStripProcessor::clearConvolutionIR()
{
    UndoStep undoScope (*this, "CLEAR IR");
    {
        auto empty = std::make_shared<const StoredIR>();
        const juce::ScopedLock sl (irLock);
        storedIR = std::move (empty);
    }
    loadStoredIRIntoChain();
}

juce::String SPAStripProcessor::getConvolutionIRName() const
{
    const juce::ScopedLock sl (irLock);
    return storedIR->name;
}

juce::String SPAStripProcessor::getConvolutionIRSource() const
{
    const juce::ScopedLock sl (irLock);
    return storedIR->source;
}

// Decodes the stored IR (the FLAC in the state, so a fresh load and a restore
// feed the engine bit-identical audio) and hands it to the chain. Message
// thread.
void SPAStripProcessor::loadStoredIRIntoChain()
{
    StoredIRPtr sp;
    {
        const juce::ScopedLock sl (irLock);
        sp = storedIR;
    }
    const StoredIR& s = *sp;

    // "factory:<id>": the state holds only the id; the audio is decoded from the
    // FLAC embedded in the plugin. An id this build does not ship loads no
    // audio (the source string is kept in the state, so a session from a build
    // with more factory IRs round-trips untouched).
    if (s.source.startsWith (factory::sourcePrefix))
    {
        juce::AudioBuffer<float> ir;
        double irRate = 0.0;
        if (factory::decode (s.source.fromFirstOccurrenceOf (factory::sourcePrefix, false, false), ir, irRate))
            fxChain.loadConvolutionIRFromBuffer (ir, irRate);
        else
            fxChain.clearConvolutionIR();
        return;
    }

    if (s.source != kIRSourceEmbedded || s.flac.getSize() == 0)
    {
        fxChain.clearConvolutionIR();
        return;
    }

    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (
        flac.createReaderFor (new juce::MemoryInputStream (s.flac.getData(), s.flac.getSize(), false), true));
    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        fxChain.clearConvolutionIR();
        return;
    }

    const int n = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> ir ((int) juce::jmin ((juce::uint32) 2, reader->numChannels), n);
    reader->read (&ir, 0, n, 0, true, true);
    fxChain.loadConvolutionIRFromBuffer (ir, s.sampleRate > 0.0 ? s.sampleRate : reader->sampleRate);
}

void SPAStripProcessor::scheduleStoredIRLoad()
{
    // setStateInformation may arrive on a non-message thread; the chain's IR
    // handling is message-thread only.
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        loadStoredIRIntoChain();
        return;
    }

    juce::MessageManager::callAsync ([weak = juce::WeakReference<SPAStripProcessor> (this)]
    {
        if (weak != nullptr)
            weak->loadStoredIRIntoChain();
    });
}

//==============================================================================
// State. XML via copyXmlToBinary; root = the APVTS tree (PARAM children) plus
// properties: fxOrder (int64), stateVersion, irSource, convIRName, and an "IR"
// child carrying the embedded impulse response.
juce::ValueTree SPAStripProcessor::buildStateTree()
{
    apvts.state.setProperty ("fxOrder", (juce::int64) fxOrderPacked.load (std::memory_order_relaxed), nullptr);

    auto state = apvts.copyState();
    state.setProperty ("stateVersion", kStateVersion, nullptr);

    StoredIRPtr sp;
    {
        const juce::ScopedLock sl (irLock);
        sp = storedIR;
    }
    const StoredIR& s = *sp;

    // Never leave a stale IR child from the live tree.
    for (auto stale = state.getChildWithName (kIRChildType); stale.isValid(); stale = state.getChildWithName (kIRChildType))
        state.removeChild (stale, nullptr);

    state.setProperty ("irSource", s.source, nullptr);
    state.setProperty ("convIRName", s.name, nullptr);

    // Modulation slot targets (parameter-ID strings; "" = unassigned).
    for (int i = 0; i < numModSlots; ++i)
        state.setProperty (modSlotStateKey (i), getModSlotTarget (i), nullptr);

    // MIDI Learn map (session state; capturePresetState strips it).
    for (auto stale = state.getChildWithName (MidiLearnManager::mapTreeType); stale.isValid();
         stale = state.getChildWithName (MidiLearnManager::mapTreeType))
        state.removeChild (stale, nullptr);
    auto map = midiLearn->toValueTree();
    if (map.getNumChildren() > 0)
        state.appendChild (map, nullptr);

    // Which preset the header shows (host state only; presets never carry it).
    // (getStateInformation may be called by a host off the message thread, where only
    // the cached edited flag and the lock-guarded name are safe to read.)
    state.setProperty (kPresetNameProperty, presetManager->getCurrentName(), nullptr);
    const bool onMessageThread = juce::MessageManager::existsAndIsCurrentThread();
    if (onMessageThread ? presetManager->isEdited() : presetManager->isEditedCached())
        state.setProperty (kPresetEditedProperty, true, nullptr);

    if (s.source == kIRSourceEmbedded && s.flac.getSize() > 0)
    {
        juce::ValueTree ir (kIRChildType);
        ir.setProperty ("format", "flac24", nullptr);
        ir.setProperty ("name", s.name, nullptr);
        ir.setProperty ("sampleRate", s.sampleRate, nullptr);
        ir.setProperty ("numChannels", s.numChannels, nullptr);
        ir.setProperty ("numSamples", s.numSamples, nullptr);
        ir.setProperty ("data", juce::Base64::toBase64 (s.flac.getData(), s.flac.getSize()), nullptr);
        state.appendChild (ir, nullptr);
    }
    return state;
}

void SPAStripProcessor::restoreStateTree (const juce::ValueTree& incoming, bool isPresetLoad)
{
    if (! incoming.isValid() || ! incoming.hasType (apvts.state.getType()))
        return;

    // Pull the embedded IR out before the tree goes into the live APVTS (the
    // blob lives in storedIR, not in the live tree).
    StoredIR s;
    s.source = incoming.getProperty ("irSource", kIRSourceNone).toString();
    s.name = incoming.getProperty ("convIRName").toString();
    if (const auto ir = incoming.getChildWithName (kIRChildType); ir.isValid())
    {
        juce::MemoryOutputStream decoded;
        if (juce::Base64::convertFromBase64 (decoded, ir.getProperty ("data").toString())
            && decoded.getDataSize() > 0)
        {
            s.flac = decoded.getMemoryBlock();
            s.sampleRate = (double) ir.getProperty ("sampleRate", 0.0);
            s.numChannels = (int) ir.getProperty ("numChannels", 0);
            s.numSamples = (int) ir.getProperty ("numSamples", 0);
            if (ir.hasProperty ("name"))
                s.name = ir.getProperty ("name").toString();
        }
    }
    // A factory IR is referenced by id only; show the shipped display name when
    // the id is known (an unknown id keeps whatever name the state carried).
    if (s.source.startsWith (factory::sourcePrefix))
        if (const auto* info = factory::find (s.source.fromFirstOccurrenceOf (factory::sourcePrefix, false, false)))
            s.name = info->name;

    // An "embedded" source without usable audio is no source at all.
    if (s.source == kIRSourceEmbedded && s.flac.getSize() == 0)
        s = {};
    if (s.source.isEmpty())
        s.source = kIRSourceNone;

    // This function only ever sees SPAStrip-shaped state (a host session, a preset
    // file or an undo snapshot); the chain order saved by older builds is widened by
    // FXChain::unpackOrder below.
    auto state = incoming.createCopy();
    const auto savedPresetName = state.getProperty (kPresetNameProperty).toString();
    const bool savedPresetEdited = (bool) state.getProperty (kPresetEditedProperty, false);
    state.removeProperty (kPresetNameProperty, nullptr);   // host-state-only bookkeeping, not live state
    state.removeProperty (kPresetEditedProperty, nullptr);
    for (auto child = state.getChildWithName (kIRChildType); child.isValid(); child = state.getChildWithName (kIRChildType))
        state.removeChild (child, nullptr);

    // MIDI Learn map: a host session restore brings its own (or none = cleared);
    // a preset load keeps the session's (presets never carry one). Never left in
    // the parameter tree.
    {
        const auto map = state.getChildWithName (MidiLearnManager::mapTreeType);
        if (! isPresetLoad)
        {
            if (map.isValid()) midiLearn->restoreFromValueTree (map);
            else               midiLearn->clearAll();
        }
        for (auto c = state.getChildWithName (MidiLearnManager::mapTreeType); c.isValid();
             c = state.getChildWithName (MidiLearnManager::mapTreeType))
            state.removeChild (c, nullptr);
    }
    migrateGrainFreeze (state);
    fillMissingParamsWithDefaults (state);

    {
        // setStateInformation is called with NO lock while processBlock runs
        // under getCallbackLock(); replaceState updates parameters one at a
        // time, so hold the lock to avoid rendering against a half-old /
        // half-new parameter set.
        const juce::ScopedLock sl (getCallbackLock());
        apvts.replaceState (state);

        dsp::FXChain::Module restored[dsp::FXChain::numModules];
        dsp::FXChain::unpackOrder ((juce::uint64) (juce::int64) apvts.state.getProperty (
                                       "fxOrder", (juce::int64) dsp::FXChain::defaultOrderPacked()),
                                   restored);
        const auto packed = dsp::FXChain::packOrder (restored);
        fxOrderPacked.store (packed, std::memory_order_relaxed);
        apvts.state.setProperty ("fxOrder", (juce::int64) packed, nullptr);

        // A preset load replaces the whole sound: reset the FX chain's stateful
        // DSP (tails, feedback rings) so the first block after the swap runs the
        // new parameters over silent state, as SPASynth's preset load does. (A
        // host session restore, like the 150 ms flush, leaves it alone. Randomize
        // All does NOT reset either: it replaces no state wholesale, and cutting
        // reverb/delay tails on every roll would be a behaviour change nobody
        // asked for -- see randomizeAll.)
        if (isPresetLoad)
            fxChain.reset();
    }

    {
        auto built = std::make_shared<const StoredIR> (std::move (s));
        const juce::ScopedLock sl (irLock);
        storedIR = std::move (built);
    }

    // Slot targets: unknown / excluded IDs (or a state from before the matrix
    // existed) become unassigned.
    for (int i = 0; i < numModSlots; ++i)
        slotTarget[(size_t) i].store (mod::indexOf (incoming.getProperty (modSlotStateKey (i)).toString()),
                                      std::memory_order_relaxed);

    scheduleStoredIRLoad();

    // A host session restore is not an edit: it starts with an empty history and
    // brings the header's preset name back (edited only if it was when saved).
    // (The history and the preset identity are message-thread structures, so a host that
    // restores on another thread gets them updated there, like the IR load above.)
    if (! isPresetLoad)
    {
        const auto finish = [this, savedPresetName, savedPresetEdited]
        {
            clearUndoHistory();
            presetManager->sessionRestored (savedPresetName, savedPresetEdited);
        };
        if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
            || juce::MessageManager::getInstance()->isThisTheMessageThread())
            finish();
        else
            juce::MessageManager::callAsync ([weak = juce::WeakReference<SPAStripProcessor> (this), finish]
            {
                if (weak != nullptr)
                    finish();
            });
    }
}

//==============================================================================
bool SPAStripProcessor::setModSlotTarget (int slot, const juce::String& parameterID)
{
    if (! juce::isPositiveAndBelow (slot, numModSlots))
        return false;
    UndoStep undoScope (*this, "MOD TARGET " + juce::String (slot + 1));
    if (parameterID.isEmpty())
    {
        slotTarget[(size_t) slot].store (-1, std::memory_order_relaxed);
        return true;
    }
    const int index = mod::indexOf (parameterID);
    if (index < 0)
        return false;
    slotTarget[(size_t) slot].store (index, std::memory_order_relaxed);
    return true;
}

juce::String SPAStripProcessor::getModSlotTarget (int slot) const
{
    if (! juce::isPositiveAndBelow (slot, numModSlots))
        return {};
    const int index = slotTarget[(size_t) slot].load (std::memory_order_relaxed);
    const auto& t = mod::targets();
    return juce::isPositiveAndBelow (index, (int) t.size()) ? t[(size_t) index].id : juce::String();
}

void SPAStripProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = buildStateTree().createXml())
        copyXmlToBinary (*xml, destData);
}

void SPAStripProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        restoreStateTree (juce::ValueTree::fromXml (*xml), false);
}

juce::AudioProcessorEditor* SPAStripProcessor::createEditor()
{
    return new SPAStripEditor (*this);
}

} // namespace spa

// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new spa::SPAStripProcessor();
}

#if SPASTRIP_HAS_SPA_LICENSING
void spa::SPAStripProcessor::refreshLicence()
{
    licenceController->refresh();
    licenceBroadcaster.sendChangeMessage();
}

void spa::SPAStripProcessor::noteEditorOpened()
{
    if (licenceState == nullptr)
        return;
    licenceState->noteEditorOpened();   // idempotent; writes the trial stores once
    licenceBroadcaster.sendChangeMessage();
}
#endif
