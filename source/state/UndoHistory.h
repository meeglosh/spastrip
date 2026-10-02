#pragma once

#include <array>
#include <deque>
#include <memory>
#include <set>
#include <vector>

#include <juce_core/juce_core.h>

#include "../ir/StoredIR.h"
#include "../params/ParameterRegistry.h"

// Undo / redo history for "everything done to the sound" (ported from
// SPASynth's UndoHistory.h and trimmed to what SPAStrip has).
//
// A PatchSnapshot is a COMPACT record of the patch: one normalised float per
// registry parameter, the FX chain order, the eight mod-slot target IDs and the
// convolution IR source. The IR is held by shared pointer (StoredIRPtr): the
// FLAC blob of an embedded user IR is NEVER copied into a step, only
// ref-counted, so a 100-step history that spans one IR holds that blob once.
//
// Deliberately NOT in a snapshot (never undone, never makes two snapshots
// "different"): the FX lock mask, WILD (both workflow settings), UI state.
//
// Message-thread only (the processor owns the one instance).
namespace spa
{

struct PatchSnapshot
{
    static constexpr int numSlots = params::id::numModSlots;

    std::vector<float> params;                    // registry order, normalised 0..1
    juce::uint64 fxOrder = 0;                     // packed chain order
    std::array<juce::String, numSlots> slotTargets;   // parameter IDs, "" = unassigned
    StoredIRPtr ir;                               // never null in a captured snapshot

    // Parameters compare with a tiny tolerance: a knob dragged away and back
    // lands within float rounding of where it started, and that is "no change".
    static constexpr float paramTolerance = 1.0e-6f;

    // Two IRs are the same when they are the same blob, or both name the same
    // factory IR, or both are "no IR". An embedded IR loaded twice from the
    // same file is two blobs and counts as a change (a reload is an edit).
    static bool sameIR (const StoredIRPtr& a, const StoredIRPtr& b)
    {
        if (a == b)
            return true;
        if (a == nullptr || b == nullptr)
            return (a == nullptr ? b : a)->source == "none";
        if (a->source != b->source)
            return false;
        return a->source == "none" || a->isFactory();
    }

    bool operator== (const PatchSnapshot& o) const { return firstDifference (o).isEmpty(); }
    bool operator!= (const PatchSnapshot& o) const { return ! (*this == o); }

    // Empty when equal, else a short description of the first difference (also
    // used by the tests to explain a failure). `ignoreParam` (a registry index,
    // -1 = none) is skipped: the preset "edited" flag ignores global.oversampling,
    // which presets neither carry nor change.
    juce::String firstDifference (const PatchSnapshot& o, int ignoreParam = -1) const
    {
        if (params.size() != o.params.size())
            return "parameter count";
        for (size_t i = 0; i < params.size(); ++i)
            if ((int) i != ignoreParam && std::abs (params[i] - o.params[i]) > paramTolerance)
                return "parameter #" + juce::String ((int) i) + " ("
                       + juce::String (params[i]) + " vs " + juce::String (o.params[i]) + ")";
        if (fxOrder != o.fxOrder) return "fx order";
        for (size_t i = 0; i < slotTargets.size(); ++i)
            if (slotTargets[i] != o.slotTargets[i])
                return "mod slot " + juce::String ((int) i + 1) + " target";
        if (! sameIR (ir, o.ir)) return "IR source";
        return {};
    }

    // Bytes this snapshot owns EXCLUDING the shared IR blob (see irBlobBytes).
    size_t approxBytes() const
    {
        size_t n = sizeof (*this) + params.capacity() * sizeof (float);
        for (const auto& s : slotTargets)
            n += s.getNumBytesAsUTF8();
        return n;
    }

    // Size of the IR blob this snapshot keeps alive (shared between snapshots).
    size_t irBlobBytes() const { return ir != nullptr ? ir->flac.getSize() : 0; }
};

// Which preset the header shows, and what "unedited" means for it. Carried by
// every entry but APPLIED only by preset load / INIT steps (resetsChain): those
// are the steps that change the loaded preset's identity. `baseline` is the
// patch as it was when that preset finished loading (or was last saved); the
// edited flag after any undo/redo is simply "patch != baseline", so undoing a
// knob back to the loaded state clears it.
struct PresetContext
{
    juce::File file;             // "" when the preset is not in the browser list (Init, import, loose file, factory)
    juce::String name { "Init" };
    juce::String bank;           // factory presets only: with `name`, their identity (they have no file)
    bool isFactory = false;
    std::shared_ptr<const PatchSnapshot> baseline;   // null = nothing to compare with (always "edited")
};

struct UndoEntry
{
    PatchSnapshot snapshot;      // the state to RETURN to when this entry is applied
    PresetContext context;       // preset identity/baseline to return to (preset load / INIT steps only)
    juce::String label;          // what the step did, e.g. "CHORUS RATE", "RANDOMIZE ALL"
    bool resetsChain = false;    // preset-style FX state reset when applied (preset load / INIT only)
};

// Two bounded stacks. Entries on `undo` are "the state before <label>";
// entries on `redo` are "the state after <label>", captured live at undo time.
class UndoStacks
{
public:
    // Bound on steps per stack. SPASynth uses the same 100.
    static constexpr int maxSteps = 100;
    // Bound on the bytes of DISTINCT shared IR blobs the two stacks may keep
    // alive. Snapshots share blobs, but 100 steps that each loaded a different
    // user IR would otherwise pin up to 100 FLACs (a 10 s stereo IR is a few
    // MB). Beyond this the OLDEST undo steps are dropped (never the newest).
    static constexpr size_t defaultMaxIRBytes = 64u * 1024u * 1024u;

    void setMaxIRBytes (size_t bytes) { maxIRBytes = bytes; trimToBudget(); }

    void pushUndo (UndoEntry e)
    {
        undo.push_back (std::move (e));
        redo.clear();   // a new edit forks history
        trim();
    }
    bool canUndo() const { return ! undo.empty(); }
    bool canRedo() const { return ! redo.empty(); }
    int undoCount() const { return (int) undo.size(); }
    int redoCount() const { return (int) redo.size(); }
    const juce::String& undoLabel() const { return undo.back().label; }
    const juce::String& redoLabel() const { return redo.back().label; }
    const std::deque<UndoEntry>& undoEntries() const { return undo; }
    const std::deque<UndoEntry>& redoEntries() const { return redo; }

    UndoEntry popUndo() { auto e = std::move (undo.back()); undo.pop_back(); return e; }
    UndoEntry popRedo() { auto e = std::move (redo.back()); redo.pop_back(); return e; }
    void pushRedo (UndoEntry e)
    {
        redo.push_back (std::move (e));
        while ((int) redo.size() > maxSteps)
            redo.pop_front();
    }
    // Used when redo re-pushes onto undo: must NOT clear the redo stack.
    void pushUndoKeepingRedo (UndoEntry e)
    {
        undo.push_back (std::move (e));
        trim();
    }
    void clear() { undo.clear(); redo.clear(); }

    // Distinct IR blobs held by both stacks (the memory the budget governs).
    size_t retainedIRBytes() const
    {
        std::set<const StoredIR*> seen;
        size_t total = 0;
        const auto add = [&] (const std::deque<UndoEntry>& d)
        {
            for (const auto& e : d)
                if (e.snapshot.ir != nullptr && seen.insert (e.snapshot.ir.get()).second)
                    total += e.snapshot.ir->flac.getSize();
        };
        add (undo);
        add (redo);
        return total;
    }

private:
    void trim()
    {
        while ((int) undo.size() > maxSteps)
            undo.pop_front();
        trimToBudget();
    }
    void trimToBudget()
    {
        while (undo.size() > 1 && retainedIRBytes() > maxIRBytes)
            undo.pop_front();
    }

    std::deque<UndoEntry> undo, redo;
    size_t maxIRBytes = defaultMaxIRBytes;
};

} // namespace spa
