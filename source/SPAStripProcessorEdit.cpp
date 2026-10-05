// SPAStripProcessor: Randomize All / WILD / per-effect locks, undo / redo and the
// preset hooks. (Split from SPAStripProcessor.cpp, which holds the audio path and
// host state.) Everything here is message-thread code.

#include "SPAStripProcessor.h"

#include "presets/PresetManager.h"

namespace spa
{

namespace
{
    // Properties a PRESET owns. Everything else on the incoming tree is
    // dropped on a preset load, and the live session's own properties (lock
    // mask, WILD, UI state, ...) are carried over instead.
    bool isPresetOwnedProperty (const juce::Identifier& name)
    {
        if (name == juce::Identifier ("fxOrder") || name == juce::Identifier ("stateVersion")
            || name == juce::Identifier ("irSource") || name == juce::Identifier ("convIRName"))
            return true;
        for (int i = 0; i < params::id::numModSlots; ++i)
            if (name == juce::Identifier ("modSlot" + juce::String (i + 1) + "Target"))
                return true;
        return false;
    }
}

//==============================================================================
// Randomize All, WILD, locks.
float SPAStripProcessor::getRandomWildness() const
{
    return juce::jlimit (0.0f, 1.0f, (float) (double) apvts.state.getProperty (kWildnessProperty, 0.5));
}

void SPAStripProcessor::setRandomWildness (float wildness)
{
    apvts.state.setProperty (kWildnessProperty, (double) juce::jlimit (0.0f, 1.0f, wildness), nullptr);
}

juce::uint32 SPAStripProcessor::getFxLockMask() const
{
    return (juce::uint32) (int) apvts.state.getProperty (kLockMaskProperty, 0) & params::allLocksMask;
}

bool SPAStripProcessor::isLocked (dsp::FXChain::Module module) const
{
    return (getFxLockMask() & params::lockBit (module)) != 0;
}

void SPAStripProcessor::setLocked (dsp::FXChain::Module module, bool locked)
{
    auto mask = getFxLockMask();
    mask = locked ? (mask | params::lockBit (module)) : (mask & ~params::lockBit (module));
    apvts.state.setProperty (kLockMaskProperty, (int) mask, nullptr);
}

bool SPAStripProcessor::isModLocked() const
{
    return (bool) apvts.state.getProperty (kModLockProperty, false);
}

void SPAStripProcessor::setModLocked (bool locked)
{
    apvts.state.setProperty (kModLockProperty, locked, nullptr);
}

void SPAStripProcessor::randomizeAll()
{
    randomizeAll (juce::Random::getSystemRandom());
}

void SPAStripProcessor::randomizeAll (juce::Random& rng)
{
    UndoStep undoScope (*this, "RANDOMIZE ALL");   // every write below folds into this one step

    // The FX chain is deliberately NOT reset (SPASynth does not either): unlike
    // a preset load this replaces no state wholesale, and killing reverb and
    // delay tails on every roll would be a behaviour change nobody asked for.
    // The parameter writes take no callback lock either (each is one atomic the
    // audio thread reads independently, as in the synth), so no host listener
    // ever runs underneath the lock.
    auto order = getFxOrder();
    params::randomizeAll (apvts, getRandomWildness(), getFxLockMask(), order, rng);
    setFxOrder (order);   // nested step: folds into the one above

    // Modulation last: its candidates depend on which effects the roll left on,
    // and drawing after everything else keeps every earlier draw where it was.
    if (! isModLocked())
    {
        const auto targets = params::rollModSlots (apvts, getRandomWildness(), getFxLockMask(), rng);
        if (targets[0] != params::keepSlotsMarker)
            for (int s = 0; s < numModSlots; ++s)
            setModSlotTarget (s, targets[(size_t) s]);   // nested steps fold in too
    }
}

//==============================================================================
// Undo / redo.
PatchSnapshot SPAStripProcessor::capturePatchSnapshot() const
{
    PatchSnapshot s;
    s.params.reserve (undoParams.size());
    for (auto* p : undoParams)
        s.params.push_back (p->getValue());
    s.fxOrder = fxOrderPacked.load (std::memory_order_relaxed);
    for (int i = 0; i < numModSlots; ++i)
        s.slotTargets[(size_t) i] = getModSlotTarget (i);
    {
        const juce::ScopedLock sl (irLock);
        s.ir = storedIR;
    }
    return s;
}

bool SPAStripProcessor::beginUndoStep (const juce::String& label, bool resetsChain, bool forceRecord)
{
    if (undoApplying || ! juce::MessageManager::existsAndIsCurrentThread())
        return false;

    if (undoDepth++ == 0)
    {
        pendingUndo = {};
        pendingUndo.snapshot = capturePatchSnapshot();
        if (presetManager != nullptr)
            pendingUndo.context = presetManager->captureContext();
        pendingUndo.label = label;
        pendingUndo.resetsChain = resetsChain;
        pendingForceRecord = forceRecord;
        undoOpenedMs = juce::Time::getMillisecondCounter();
    }
    else
    {
        // Nested: the outer step's label wins, the flags accumulate.
        pendingUndo.resetsChain = pendingUndo.resetsChain || resetsChain;
        pendingForceRecord = pendingForceRecord || forceRecord;
    }
    return true;
}

void SPAStripProcessor::endUndoStep()
{
    if (undoDepth <= 0)
        return;
    if (--undoDepth == 0)
        commitUndoStep();
}

void SPAStripProcessor::commitUndoStep()
{
    // Only a step that really changed the patch is recorded: a click that lands
    // on the same value, or a drag that returns to where it began, leaves no
    // entry. Preset loads / Init force-record (their preset identity changes even
    // when the sound happens to be identical).
    if (! pendingForceRecord && capturePatchSnapshot() == pendingUndo.snapshot)
        return;

    undoStacks.pushUndo (std::move (pendingUndo));
    pendingUndo = {};
    undoBroadcaster.sendChangeMessage();
    if (! pendingForceRecord && presetManager != nullptr)
        presetManager->refreshEditedState();
}

void SPAStripProcessor::onParameterGesture (int parameterIndex, bool starting)
{
    // Gestures only ever open an undo step from the message thread; bail out
    // before any String work if a host delivers one elsewhere.
    if (! juce::MessageManager::existsAndIsCurrentThread())
        return;

    if (starting)
    {
        juce::String label ("PARAMETER");
        const auto& all = getParameters();
        if (parameterIndex >= 0 && parameterIndex < all.size())
            label = all[parameterIndex]->getName (64).toUpperCase();
        if (beginUndoStep (label))
            acceptedGestures.insert (parameterIndex);
    }
    else
    {
        const auto it = acceptedGestures.find (parameterIndex);
        if (it != acceptedGestures.end())
        {
            acceptedGestures.erase (it);
            endUndoStep();
        }
    }
}

void SPAStripProcessor::watchdogCloseStaleUndoStep()
{
    // A control destroyed mid-drag never sends its gesture end; without this the
    // open step would swallow every later edit. Only while no mouse button is
    // down (a legitimately long drag keeps its step).
    if (undoDepth > 0 && ! undoApplying
        && (int) (juce::Time::getMillisecondCounter() - undoOpenedMs) > 4000
        && ! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
    {
        undoDepth = 0;
        acceptedGestures.clear();
        commitUndoStep();
    }
}

void SPAStripProcessor::clearUndoHistory()
{
    undoStacks.clear();
    undoBroadcaster.sendChangeMessage();
}

bool SPAStripProcessor::undo()
{
    if (! undoStacks.canUndo() || undoDepth > 0 || undoApplying)
        return false;

    auto entry = undoStacks.popUndo();
    UndoEntry redoEntry;
    redoEntry.snapshot = capturePatchSnapshot();   // the state being left, for redo
    redoEntry.context = presetManager->captureContext();
    redoEntry.label = entry.label;
    redoEntry.resetsChain = entry.resetsChain;
    undoStacks.pushRedo (std::move (redoEntry));

    applyPatchSnapshot (entry.snapshot, entry.resetsChain, entry.resetsChain ? &entry.context : nullptr);
    undoBroadcaster.sendChangeMessage();
    return true;
}

bool SPAStripProcessor::redo()
{
    if (! undoStacks.canRedo() || undoDepth > 0 || undoApplying)
        return false;

    auto entry = undoStacks.popRedo();
    UndoEntry undoEntry;
    undoEntry.snapshot = capturePatchSnapshot();
    undoEntry.context = presetManager->captureContext();
    undoEntry.label = entry.label;
    undoEntry.resetsChain = entry.resetsChain;
    undoStacks.pushUndoKeepingRedo (std::move (undoEntry));

    applyPatchSnapshot (entry.snapshot, entry.resetsChain, entry.resetsChain ? &entry.context : nullptr);
    undoBroadcaster.sendChangeMessage();
    return true;
}

void SPAStripProcessor::applyPatchSnapshot (const PatchSnapshot& snap, bool resetChain,
                                            const PresetContext* restoreContext)
{
    // How an undo is applied (and why it is not just restoreStateTree()):
    //  * Parameters are written under the callback lock -- the same race
    //    restoreStateTree() closes -- but WITHOUT the chain reset a preset load
    //    does: a hard cut on every undo would be jarring, and a parameter-only
    //    step needs none. Steps recorded from a preset load / Init (resetChain)
    //    DO get the reset, so undoing them behaves like loading the previous
    //    patch.
    //  * The IR is touched only where it differs from what is loaded; an
    //    embedded blob is shared from the snapshot, never decoded from a copy.
    undoApplying = true;
    bool irChanged = false;
    {
        const juce::ScopedLock sl (getCallbackLock());

        const auto n = juce::jmin (snap.params.size(), undoParams.size());
        for (size_t i = 0; i < n; ++i)
            if (std::abs (undoParams[i]->getValue() - snap.params[i]) > PatchSnapshot::paramTolerance)
                undoParams[i]->setValueNotifyingHost (snap.params[i]);

        if (resetChain)
            fxChain.reset();

        if (fxOrderPacked.load (std::memory_order_relaxed) != snap.fxOrder)
        {
            fxOrderPacked.store (snap.fxOrder, std::memory_order_relaxed);
            apvts.state.setProperty ("fxOrder", (juce::int64) snap.fxOrder, nullptr);
        }

        for (int i = 0; i < numModSlots; ++i)
            slotTarget[(size_t) i].store (mod::indexOf (snap.slotTargets[(size_t) i]), std::memory_order_relaxed);

        {
            const juce::ScopedLock irSl (irLock);
            if (! PatchSnapshot::sameIR (storedIR, snap.ir))
            {
                storedIR = snap.ir != nullptr ? snap.ir : std::make_shared<const StoredIR>();
                irChanged = true;
            }
        }
    }

    if (irChanged)
        loadStoredIRIntoChain();
    undoApplying = false;

    // A preset load / Init undo or redo puts the previous preset's name and
    // baseline back; every other step keeps the current preset. Either way
    // "edited" is exactly "this patch differs from the loaded/saved baseline",
    // so undoing a knob back to the loaded state clears it.
    if (restoreContext != nullptr)
        presetManager->restoreContext (*restoreContext);
    presetManager->refreshEditedState();
}

//==============================================================================
// Presets.
juce::ValueTree SPAStripProcessor::capturePresetState()
{
    auto state = buildStateTree();

    // Session-owned, never in a preset: lock mask, WILD, preset identity.
    for (const char* name : { kLockMaskProperty, kModLockProperty, kWildnessProperty, kPresetNameProperty, kPresetEditedProperty })
        state.removeProperty (name, nullptr);

    // UI state (uiScale, uiFxTab, drawer open, ...: every "ui*" property) lives in
    // host state only; a saved preset must not carry it.
    for (int i = state.getNumProperties(); --i >= 0;)
        if (state.getPropertyName (i).toString().startsWith ("ui"))
            state.removeProperty (state.getPropertyName (i), nullptr);

    // global.oversampling is a quality / CPU setting of THIS session, not part
    // of a sound: a preset neither carries nor changes it.
    for (int i = state.getNumChildren(); --i >= 0;)
    {
        const auto child = state.getChild (i);
        if (child.hasType ("PARAM") && child.getProperty ("id").toString() == params::id::oversampling)
            state.removeChild (i, nullptr);
    }
    return state;
}

bool SPAStripProcessor::applyPresetState (const juce::ValueTree& presetTree, const juce::String& undoLabel)
{
    if (! presetTree.isValid() || ! presetTree.hasType (apvts.state.getType()))
        return false;

    UndoStep undoScope (*this, undoLabel, /*resetsChain*/ true, /*forceRecord*/ true);

    auto incoming = presetTree.createCopy();

    // Keep only the properties a preset owns (a hand-edited file cannot smuggle
    // in a lock mask, WILD or UI state), then carry the live session's own.
    for (int i = incoming.getNumProperties(); --i >= 0;)
    {
        const auto name = incoming.getPropertyName (i);
        if (! isPresetOwnedProperty (name))
            incoming.removeProperty (name, nullptr);
    }
    for (int i = 0; i < apvts.state.getNumProperties(); ++i)
    {
        const auto name = apvts.state.getPropertyName (i);
        if (! isPresetOwnedProperty (name))
            incoming.setProperty (name, apvts.state.getProperty (name), nullptr);
    }

    // The session's oversampling factor survives the load.
    const float oversampling = raw.oversampling->load();
    bool stamped = false;
    for (int i = 0; i < incoming.getNumChildren() && ! stamped; ++i)
    {
        auto child = incoming.getChild (i);
        if (child.hasType ("PARAM") && child.getProperty ("id").toString() == params::id::oversampling)
        {
            child.setProperty ("value", (double) oversampling, nullptr);
            stamped = true;
        }
    }
    if (! stamped)
    {
        juce::ValueTree os ("PARAM");
        os.setProperty ("id", params::id::oversampling, nullptr);
        os.setProperty ("value", (double) oversampling, nullptr);
        incoming.appendChild (os, nullptr);
    }

    restoreStateTree (incoming, true);
    return true;
}

} // namespace spa
