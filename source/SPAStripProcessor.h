#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <set>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/FXChain.h"
#include "dsp/Telemetry.h"
#include "ir/FactoryIRs.h"
#include "ir/StoredIR.h"
#include "mod/ModTargets.h"
#include "mod/SidechainDetector.h"
#include "params/ParameterRegistry.h"
#include "params/Randomizer.h"
#include "state/UndoHistory.h"

namespace spa
{

namespace preset { class PresetManager; }

// SPAStrip: the SPASynth effects chain as a standalone audio effect.
//
// Signal flow per block (always on a stereo working buffer; mono in is
// duplicated, mono out is the L/R average):
//
//   host in -> [dry tap, pre input gain] -> input gain
//           -> sidechain detector (External bus or the post-gain input)
//              -> envelope -> modulation matrix (8 slots, normalised offsets)
//           -> (oversample up) -> updateFXParams -> FXChain::process
//              (in <= 32-host-sample pieces while any slot is active)
//           -> publishTelemetry -> (oversample down)
//           -> global mix against the latency-compensated dry tap
//           -> output gain -> [sc.listen: detector signal] -> host out
//
// Dry tap: at 1x it is the input delayed by the reported latency (an integer
// number of samples). With oversampling it is run through a second, identical
// Oversampling instance (up then straight down) so the dry and wet paths share
// the oversampler's phase response and global.mix stays flat; bypass keeps the
// plain integer-delayed input.
//
// The optional "Sidechain" input bus feeds the detector when sc.source is
// "External". Its channels share host-buffer indices with main OUTPUT channels
// in some layouts (mono in -> stereo out), so it is copied out at the start of
// each chunk, before any output is written.
class SPAStripProcessor : public juce::AudioProcessor,
                          private juce::Timer
{
public:
    SPAStripProcessor();
    ~SPAStripProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    // Bypass passes the dry signal delayed by the reported latency; the chain
    // is not run.
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "SPAStrip"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return fxChain.tailSeconds (fxParams); }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }
    dsp::Telemetry& getTelemetry() { return telemetry; }

    // Chain order (packed 4 bits/module in a uint64, lock-free hand-off to the
    // audio thread; persisted as the "fxOrder" int64 state property).
    void setFxOrder (const juce::Array<int>& moduleIds);
    juce::Array<int> getFxOrder() const;

    //==========================================================================
    // Randomize All, WILD and per-effect locks (message thread).
    //
    // Locks: one per chain module (dsp::FXChain::Module; TREM/VIB covers both
    // fxTrem.* and fxVib.*). A locked module keeps every parameter (its enable
    // toggle included) AND its slot in the chain order through Randomize All.
    // The lock mask is a bitmask keyed by the Module enum value, stored in the
    // "fxLockMask" state property: saved with the host state, NOT part of
    // presets and NOT undoable (a workflow setting). Same for WILD
    // ("randomWildness", 0..1, default 0.5).
    //
    // randomizeAll() is ONE undo step; every parameter write is a
    // begin-gesture / setValueNotifyingHost / end-gesture so hosts record it.
    // The Random& overload is the deterministic test seam.
    void randomizeAll();
    void randomizeAll (juce::Random& rng);
    float getRandomWildness() const;
    void setRandomWildness (float wildness);
    bool isLocked (dsp::FXChain::Module module) const;
    void setLocked (dsp::FXChain::Module module, bool locked);
    juce::uint32 getFxLockMask() const;

    //==========================================================================
    // Undo / redo (message thread). History of everything done to the SOUND:
    // all parameter values, the FX order, the mod slot targets and the IR
    // source (an embedded IR blob is shared between steps, never copied).
    // Not undoable: the lock mask, WILD, UI state.
    //
    // What opens a step: (a) a parameter GESTURE -- every editor control
    // brackets its drag/click with begin/endChangeGesture, which the
    // per-parameter listener turns into ONE step per whole drag; (b) an
    // explicit UndoStep scope around a discrete action (the action methods
    // below open their own). Plain setValueNotifyingHost() calls with no gesture
    // and no scope -- host automation playback -- are never recorded, so
    // automation cannot flood the history. Steps nest: while one is open every
    // inner gesture/scope folds into it (Randomize All writes each parameter
    // inside its own gesture and still lands as ONE step). A step that ends in
    // the state it started in (a click on the same value, a drag back to the
    // start) records nothing. Returns false (and records nothing) while an
    // undo/redo is itself being applied, or off the message thread.
    static constexpr int kMaxUndoSteps = UndoStacks::maxSteps;   // per stack
    bool beginUndoStep (const juce::String& label, bool resetsChain = false, bool forceRecord = false);
    void endUndoStep();
    struct UndoStep
    {
        UndoStep (SPAStripProcessor& p, const juce::String& label,
                  bool resetsChain = false, bool forceRecord = false)
            : proc (p), active (p.beginUndoStep (label, resetsChain, forceRecord)) {}
        ~UndoStep() { if (active) proc.endUndoStep(); }
        UndoStep (const UndoStep&) = delete;
        UndoStep& operator= (const UndoStep&) = delete;
        SPAStripProcessor& proc;
        bool active;
    };
    bool canUndo() const { return undoStacks.canUndo(); }
    bool canRedo() const { return undoStacks.canRedo(); }
    juce::String getUndoLabel() const { return undoStacks.canUndo() ? undoStacks.undoLabel() : juce::String(); }
    juce::String getRedoLabel() const { return undoStacks.canRedo() ? undoStacks.redoLabel() : juce::String(); }
    int getUndoStepCount() const { return undoStacks.undoCount(); }
    int getRedoStepCount() const { return undoStacks.redoCount(); }
    bool undo();
    bool redo();
    void clearUndoHistory();
    // The editor's undo/redo buttons listen here (fires when a step is recorded,
    // on undo/redo and when the history is cleared).
    juce::ChangeBroadcaster& getUndoBroadcaster() { return undoBroadcaster; }
    PatchSnapshot capturePatchSnapshot() const;
    const UndoStacks& getUndoStacksForTest() const { return undoStacks; }
    UndoStacks& getUndoStacksForTest() { return undoStacks; }

    //==========================================================================
    // Presets (see presets/PresetManager.h). The manager is owned here; these two
    // are what it needs from the processor.
    preset::PresetManager& getPresetManager() { return *presetManager; }
    // The current state as a preset: parameters, FX order, mod slot targets and
    // IR source (an embedded user IR travels inside it). Excludes -- a preset
    // neither carries nor changes them -- the lock mask, WILD,
    // global.oversampling, UI properties and the preset identity.
    juce::ValueTree capturePresetState();
    // Loads a preset tree as ONE undo step: replaces everything a preset owns,
    // keeps the session's lock mask / WILD / oversampling / UI properties, and
    // resets the FX chain state (under the callback lock, like SPASynth's
    // preset load). False (nothing changed) for a tree that is not a state.
    bool applyPresetState (const juce::ValueTree& presetTree, const juce::String& undoLabel);

    // Property names kept in the live state tree.
    static constexpr const char* kLockMaskProperty = "fxLockMask";
    static constexpr const char* kWildnessProperty = "randomWildness";
    // Host-state-only (never in a preset): the loaded preset's name / edited flag.
    static constexpr const char* kPresetNameProperty = "presetName";
    static constexpr const char* kPresetEditedProperty = "presetEdited";

    //==========================================================================
    // Convolve impulse response. The decoded audio is stored INSIDE the plugin
    // state (FLAC + base64) so sessions survive the source file moving.
    // Message thread only.
    bool loadConvolutionIR (const juce::File& file);
    void clearConvolutionIR();
    bool hasConvolutionIR() const { return fxChain.hasConvolutionIR(); }
    // Display name of the current IR (original file name without extension).
    juce::String getConvolutionIRName() const;
    // "none", "embedded" (a user file, audio stored in the state), or
    // "factory:<id>" (an embedded factory IR; the state stores only the id).
    juce::String getConvolutionIRSource() const;

    // Factory IRs (message thread). The list is in manifest order grouped by
    // category. loadFactoryIR returns false (and changes nothing) for an
    // unknown id.
    static const std::vector<factory::IR>& getFactoryIRs() { return factory::list(); }
    bool loadFactoryIR (const juce::String& id);
    static juce::String getFactoryIRCredits() { return factory::credits(); }

    // The UI-facing views of the convolution engine (envelope for a waveform).
    const std::array<float, dsp::FXChain::convEnvPoints>& getConvolutionEnvelope() const
    {
        return fxChain.convolutionEnvelope();
    }
    float getConvolutionStartTrim() const { return fxChain.convolutionStartTrim(); }
    double getConvolutionLengthSeconds() const { return fxChain.convolutionLengthSeconds(); }

    //==========================================================================
    // Modulation matrix (message thread). Slot targets are plugin state, not
    // host parameters; the depth of each slot IS a host parameter
    // (mod.slotN.depth). A slot modulates its target by depth x envelope in the
    // target's normalised (0..1) space, summed per target across slots,
    // clamped, and never written back to the host-visible parameter.
    static constexpr int numModSlots = params::id::numModSlots;
    static const std::vector<mod::ModTarget>& getModTargets() { return mod::targets(); }
    // slot 0..numModSlots-1. "" unassigns. False (nothing changed) for a bad
    // slot or an unknown / excluded parameter ID.
    bool setModSlotTarget (int slot, const juce::String& parameterID);
    juce::String getModSlotTarget (int slot) const;   // "" = unassigned

    // How often (host samples, 1..32; default kDefaultModUpdateInterval = 8) the
    // modulation is refreshed while at least one slot is active. The product
    // requirement is "at least every 32 host samples"; 8 is what the zipper
    // measurements chose (see tests/Phase2Tests.inc, --audit-mod). A
    // diagnostics / test knob; the defaults are what ships.
    void setModUpdateInterval (int hostSamples) { modUpdateInterval.store (juce::jlimit (1, 32, hostSamples)); }
    int getModUpdateInterval() const { return modUpdateInterval.load(); }
    // One-pole slew (ms; 0 = off) applied to each slot's offset (depth x envelope)
    // at the refresh rate: turns an abrupt envelope edge (or a depth change) into
    // a short glide so the stepped refresh does not click. Diagnostics / test knob.
    void setModSmoothingMs (float ms) { modSmoothingMs.store (juce::jmax (0.0f, ms)); }
    float getModSmoothingMs() const { return modSmoothingMs.load(); }

    //==========================================================================
    // Message-thread housekeeping, run from the 150 ms timer (and directly by
    // the headless tests, which have no message loop): applies a pending
    // oversampling-factor change, services a non-finite flush request,
    // publishes the reported latency and reshapes the convolution IR.
    void serviceMessageThread();

    // SPAStripAdded diagnostics: how many times the effect chain has been run
    // (one per modulation sub-chunk, or one per host chunk on the plain path)
    // and how many host chunks took the modulated (subdividing) path. Relaxed
    // counters, read by the tests / bench; never used by the DSP.
    int getConvolutionHeadSamples() const { return fxChain.convolutionHeadSamples(); }
    int getConvolutionChunkSamples() const { return fxChain.convolutionChunkSamples(); }
    uint64_t getChainInvocationCount() const { return chainInvocations.load (std::memory_order_relaxed); }
    uint64_t getModulatedChunkCount() const { return modulatedChunks.load (std::memory_order_relaxed); }

    // Total latency (host-rate samples) that the processor reports / the dry
    // path is delayed by: oversampler latency + limiter lookahead.
    int getCurrentLatencySamples() const;

    static constexpr int kStateVersion = 1;
    static constexpr int kDefaultModUpdateInterval = 8;
    static constexpr float kDefaultModSmoothingMs = 4.0f;
    static constexpr double kMaxIRSeconds = 10.0;   // same cap as SPASynth

private:
    void timerCallback() override { serviceMessageThread(); }

    // One chunk (<= prepared block size) of the stereo working pipeline.
    void processChunk (juce::AudioBuffer<float>& hostBuffer, int startSample, int numSamples,
                       bool bypassed);
    // Runs the chain over `engineSamples` samples (hostSamples x factor). With
    // `modActive` the span is cut into pieces of at most modUpdateInterval host
    // samples and the slot offsets are refreshed before each one.
    void processChain (float* const* chans, int engineSamples, int factor, int hostSamples,
                       bool modActive);
    void applyModOffsets (float envelope, int pieceHostSamples);
    void clearModOffsets();
    void resolveTempo();
    void updateFXParams();
    void rebuildOversampling (int factor);   // message thread, callback lock held
    void applyFactorLatency();

    // Delays the stereo `src` (first numSamples) by delaySamples through `ring`
    // (per-channel, power-of-two length), writing the result to `dst`.
    void delayThroughRing (juce::AudioBuffer<float>& ring, int& writePos,
                           const juce::AudioBuffer<float>& src, juce::AudioBuffer<float>& dst,
                           int numSamples, int delaySamples);
    // Dry (pre-chain) tap, delayed by the reported latency.
    void pushAndReadDry (int numSamples, int delaySamples);
    int lookaheadLatencyHost() const
    {
        return desiredLatency.load (std::memory_order_relaxed) / juce::jmax (1, currentOsFactor);
    }

    juce::ValueTree buildStateTree();
    // isPresetLoad: a preset / Init load (resets the FX chain state under the
    // callback lock; keeps the undo history). Otherwise a host session restore
    // (no chain reset; clears the undo history, which is not part of a session).
    void restoreStateTree (const juce::ValueTree& incoming, bool isPresetLoad);
    void loadStoredIRIntoChain();            // message thread
    void scheduleStoredIRLoad();

    //==========================================================================
    juce::AudioProcessorValueTreeState apvts;
    dsp::Telemetry telemetry;
    dsp::FXChain fxChain;
    dsp::FXChain::Params fxParams;

    // A float FX parameter as the audio thread reads it. `normOffset` is the
    // hook for a later per-block normalized modulation offset (phase 2): it is
    // applied in the parameter's own normalized (0..1) space, so it behaves
    // identically for linear, skewed and stepped ranges, BEFORE the value is
    // converted (e.g. the chorus/delay width percent -> 0..1) and written into
    // FXChain::Params. Audio-thread-only; 0 means "no modulation" and returns
    // the raw value untouched.
    struct FxFloat
    {
        std::atomic<float>* raw = nullptr;
        const juce::NormalisableRange<float>* range = nullptr;
        float normOffset = 0.0f;

        float get() const
        {
            const float v = raw->load();
            if (normOffset == 0.0f)
                return v;
            // NOT snapped to the parameter's interval (0.1 dB, 0.01 st, ...):
            // that is the host/UI step of the stored value, and quantising a
            // modulated trajectory to it turns a smooth sweep into audible
            // gain/pitch stair-steps (measured: -38 dB steps on a +/-0.1 dB
            // grid at high makeup gain). Real-range clamping is the 0..1 clamp.
            const float n = juce::jlimit (0.0f, 1.0f, range->convertTo0to1 (v) + normOffset);
            return range->convertFrom0to1 (n);
        }
    };

    struct FxBinding { juce::String id; FxFloat* fx; };
    std::vector<FxBinding> fxBindings;            // constructor only
    std::vector<FxFloat*> targetFx;               // index = mod::targets() index

    struct RawFX
    {
        std::atomic<float>* distEnable = nullptr;
        std::atomic<float>* distType = nullptr;
        FxFloat distDrive, distTone, distMix;

        std::atomic<float>* chorusEnable = nullptr;
        FxFloat chorusRate, chorusDepth, chorusFeedback, chorusWidth;
        std::atomic<float>* chorusMode = nullptr;
        FxFloat chorusMix;

        std::atomic<float>* delayEnable = nullptr;
        std::atomic<float>* delaySync = nullptr;
        FxFloat delayTime;
        std::atomic<float>* delayDivision = nullptr;
        FxFloat delayFeedback;
        std::atomic<float>* delayPingPong = nullptr;
        FxFloat delayWidth, delayMix;

        std::atomic<float>* reverbEnable = nullptr;
        std::atomic<float>* reverbMode = nullptr;
        FxFloat reverbPreDelay, reverbSize, reverbDecay, reverbDamping, reverbModDepth,
                reverbLowCut, reverbHighCut, reverbWidth, reverbMix;

        std::atomic<float>* eqEnable = nullptr;
        std::atomic<float>* eqCharacter = nullptr;
        struct EqBandPtrs
        {
            std::atomic<float>* enable = nullptr;
            std::atomic<float>* type = nullptr;
            std::atomic<float>* slope = nullptr;
            FxFloat freq, gain, q;
        };
        std::array<EqBandPtrs, 8> eqBands {};

        std::atomic<float>* modEnable = nullptr;
        std::atomic<float>* modType = nullptr;
        FxFloat modRate;
        std::atomic<float>* modSync = nullptr;
        std::atomic<float>* modDivision = nullptr;
        FxFloat modDepth, modFeedback;
        std::atomic<float>* modStages = nullptr;
        FxFloat modCentre, modManual, modWidth, modMix;

        std::atomic<float>* tremEnable = nullptr;
        FxFloat tremRate;
        std::atomic<float>* tremSync = nullptr;
        std::atomic<float>* tremDivision = nullptr;
        FxFloat tremDepth;
        std::atomic<float>* tremShape = nullptr;
        FxFloat tremStereo, tremMix;
        std::atomic<float>* vibEnable = nullptr;
        FxFloat vibRate;
        std::atomic<float>* vibSync = nullptr;
        std::atomic<float>* vibDivision = nullptr;
        FxFloat vibDepth, vibMix;

        std::atomic<float>* limEnable = nullptr;
        FxFloat limDrive, limCeiling, limRelease;
        std::atomic<float>* limAutoRelease = nullptr;
        std::atomic<float>* limCharacter = nullptr;
        FxFloat limStereoLink;
        std::atomic<float>* limTruePeak = nullptr;
        std::atomic<float>* limLookahead = nullptr;
        std::atomic<float>* limAutoGain = nullptr;

        std::atomic<float>* convEnable = nullptr;
        FxFloat convMix, convWidth, convPreDelay, convDecay, convDamping, convStart;

        std::atomic<float>* compEnable = nullptr;
        FxFloat compMix, compXoverLow, compXoverHigh;
        struct CompBand { FxFloat thresh, ratio, upRatio, attack, release, gain; };
        std::array<CompBand, 3> compBands {};

        std::atomic<float>* grainEnable = nullptr;
        FxFloat grainSize, grainDensity;
        std::atomic<float>* grainSync = nullptr;
        std::atomic<float>* grainDivision = nullptr;
        FxFloat grainPitch, grainSpread, grainSpreadPitch, grainPosition, grainReverse,
                grainFeedback, grainMix;
        std::atomic<float>* grainFreeze = nullptr;
    };

    struct Raw
    {
        std::atomic<float>* inputGain = nullptr;
        std::atomic<float>* outputGain = nullptr;
        std::atomic<float>* mix = nullptr;
        std::atomic<float>* oversampling = nullptr;
        std::atomic<float>* scSource = nullptr;
        std::atomic<float>* scGain = nullptr;
        std::atomic<float>* scAttack = nullptr;
        std::atomic<float>* scRelease = nullptr;
        std::atomic<float>* scHpf = nullptr;
        std::atomic<float>* scListen = nullptr;
        std::array<std::atomic<float>*, numModSlots> modDepth {};
        RawFX fx;
    } raw;

    //==========================================================================
    // Oversampling. Both oversamplers are allocated in prepareToPlay, so a
    // factor change on the message thread never allocates a new one.
    // oversamplers[0] is the 2x engine, [1] is 4x.
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 2> oversamplers;
    // Identical second pair, used only for the dry/wet mix's dry signal (up then
    // straight down, no processing) so the dry path has the same phase response
    // as the wet one. Run whenever the factor is > 1, whatever the mix.
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 2> dryOversamplers;
    int currentOsFactor = 1;
    double hostSampleRate = 44100.0;
    int hostBlockSize = 512;
    int osLatencyHost = 0;
    std::atomic<int> pendingOsFactor { 1 };

    // Stereo working buffers (host rate), sized to the prepared block size.
    juce::AudioBuffer<float> work;
    juce::AudioBuffer<float> dryOut;       // delayed dry, current chunk
    // Dry ring (pre input gain), one per channel, power-of-two length.
    juce::AudioBuffer<float> dryRing;
    int dryRingMask = 0;
    int dryWritePos = 0;
    // Oversampled-dry path: the dry tap after its own up/down round trip, then
    // delayed by the limiter-lookahead part of the latency only.
    juce::AudioBuffer<float> dryOsWork;
    juce::AudioBuffer<float> dryOsRing;
    int dryOsWritePos = 0;

    // Sidechain / modulation.
    mod::SidechainDetector detector;
    juce::AudioBuffer<float> scWork;       // sidechain bus copy (2 ch), read before any output write
    juce::AudioBuffer<float> listenBuf;    // detector signal after HPF + gain (2 ch)
    std::vector<float> envBuf;             // per-sample envelope of the current chunk
    std::vector<float> offsetAccum;        // per target, summed normalised offset
    struct ActiveSlot { int target; float depth; int slot; };
    std::array<ActiveSlot, numModSlots> activeSlots {};
    int numActiveSlots = 0;
    std::array<std::atomic<int>, numModSlots> slotTarget;   // index into mod::targets(), -1 = none
    std::atomic<int> modUpdateInterval { kDefaultModUpdateInterval };
    std::atomic<uint64_t> chainInvocations { 0 };
    std::atomic<uint64_t> modulatedChunks { 0 };
    std::atomic<float> modSmoothingMs { kDefaultModSmoothingMs };
    std::array<float, numModSlots> slotSmoothed {};   // smoothed offset per slot
    bool modOffsetsApplied = false;        // some FxFloat::normOffset may be non-zero
    juce::SmoothedValue<float> listenSmoothed;

    juce::SmoothedValue<float> inputGainSmoothed, outputGainSmoothed, mixSmoothed;

    double blockBpm = 120.0;

    // Lookahead-limiter latency (engine samples): written on the audio thread
    // by updateFXParams, applied via setLatencySamples on the timer.
    std::atomic<int> desiredLatency { 0 };
    std::atomic<bool> fxStateFlushPending { false };
    std::atomic<juce::uint64> fxOrderPacked { dsp::FXChain::defaultOrderPacked() };

    //==========================================================================
    // Convolve IR as stored in the plugin state. Guarded by irLock (never
    // touched from the audio thread).
    // Held by shared pointer (immutable once built) so undo snapshots can share
    // the FLAC blob instead of copying it.
    mutable juce::CriticalSection irLock;
    StoredIRPtr storedIR = std::make_shared<const StoredIR>();
    std::unique_ptr<juce::AudioFormatManager> irFormats;

    //==========================================================================
    // Undo internals (see the public block above).
    UndoStacks undoStacks;
    juce::ChangeBroadcaster undoBroadcaster;
    int undoDepth = 0;                 // open gestures + scopes (message thread)
    bool undoApplying = false;         // true while undo()/redo() writes state
    juce::uint32 undoOpenedMs = 0;     // when depth went 0 -> 1 (watchdog)
    UndoEntry pendingUndo;             // the "before" state of the open step
    bool pendingForceRecord = false;
    std::vector<juce::RangedAudioParameter*> undoParams;   // registry order
    std::multiset<int> acceptedGestures;    // parameter indices whose gesture opened a step
    // Gesture start/end opens/closes undo steps; ANY value change (including
    // host automation, possibly on the audio thread) only raises an atomic flag
    // for the 150 ms timer to re-evaluate the preset "edited" state.
    struct UndoGestureListener : juce::AudioProcessorParameter::Listener
    {
        explicit UndoGestureListener (SPAStripProcessor& o) : owner (o) {}
        void parameterValueChanged (int, float) override { owner.editedCheckPending.store (true, std::memory_order_relaxed); }
        void parameterGestureChanged (int index, bool starting) override { owner.onParameterGesture (index, starting); }
        SPAStripProcessor& owner;
    };
    UndoGestureListener undoGestureListener { *this };
    std::atomic<bool> editedCheckPending { false };
    void onParameterGesture (int parameterIndex, bool starting);
    void commitUndoStep();
    void applyPatchSnapshot (const PatchSnapshot& snap, bool resetChain, const PresetContext* restoreContext);
    void watchdogCloseStaleUndoStep();

    std::unique_ptr<preset::PresetManager> presetManager;

    JUCE_DECLARE_WEAK_REFERENCEABLE (SPAStripProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SPAStripProcessor)
};

} // namespace spa
