#include "MidiLearn.h"

namespace spa
{

MidiLearnManager::MidiLearnManager (juce::AudioProcessorValueTreeState& state)
    : apvts (state)
{
    for (auto& cc : ccToParam)
        cc.store (-1);

    // Stable index order for the atomics: the processor's parameter list.
    for (auto* param : apvts.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
        {
            parametersByIndex.push_back (ranged);
            rawByIndex.push_back (apvts.getRawParameterValue (ranged->paramID));
        }
}

MidiLearnManager::~MidiLearnManager()
{
    cancelPendingUpdate();
}

int MidiLearnManager::indexOfParam (const juce::String& paramID) const
{
    for (size_t i = 0; i < parametersByIndex.size(); ++i)
        if (parametersByIndex[i]->paramID == paramID)
            return (int) i;

    return -1;
}

void MidiLearnManager::armLearn (const juce::String& paramID)
{
    armedParamIndex.store (indexOfParam (paramID));
    sendChangeMessage();
}

void MidiLearnManager::cancelLearn()
{
    armedParamIndex.store (-1);
    sendChangeMessage();
}

juce::String MidiLearnManager::getArmedParamID() const
{
    const auto index = armedParamIndex.load();
    return index >= 0 ? parametersByIndex[(size_t) index]->paramID : juce::String();
}

int MidiLearnManager::getAssignedCC (const juce::String& paramID) const
{
    const auto index = indexOfParam (paramID);
    for (int cc = 0; cc < 128; ++cc)
        if (ccToParam[(size_t) cc].load() == index && index >= 0)
            return cc;

    return -1;
}

void MidiLearnManager::clearAssignment (const juce::String& paramID)
{
    const auto index = indexOfParam (paramID);
    for (auto& cc : ccToParam)
        if (cc.load() == index)
            cc.store (-1);

    sendChangeMessage();
}

void MidiLearnManager::clearAll()
{
    for (auto& cc : ccToParam)
        cc.store (-1);

    sendChangeMessage();
}

juce::ValueTree MidiLearnManager::toValueTree() const
{
    juce::ValueTree tree (mapTreeType);

    for (int cc = 0; cc < 128; ++cc)
    {
        const auto index = ccToParam[(size_t) cc].load();
        if (index < 0)
            continue;

        juce::ValueTree map ("MAP");
        map.setProperty ("cc", cc, nullptr);
        map.setProperty ("param", parametersByIndex[(size_t) index]->paramID, nullptr);
        tree.appendChild (map, nullptr);
    }

    return tree;
}

void MidiLearnManager::restoreFromValueTree (const juce::ValueTree& tree)
{
    if (! tree.hasType (mapTreeType))
        return;

    for (auto& cc : ccToParam)
        cc.store (-1);

    for (const auto& map : tree)
    {
        const auto cc = (int) map.getProperty ("cc", -1);
        const auto index = indexOfParam (map.getProperty ("param").toString());
        if (cc >= 0 && cc < 128 && index >= 0)
            ccToParam[(size_t) cc].store (index);
    }

    sendChangeMessage();
}

void MidiLearnManager::processMidi (const juce::MidiBuffer& midi)
{
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (! message.isController())
            continue;

        const auto cc = message.getControllerNumber();

        // Pending learn captures the first CC it hears.
        const auto armed = armedParamIndex.load();
        if (armed >= 0)
        {
            // One CC per parameter: release any previous binding.
            for (auto& entry : ccToParam)
                if (entry.load() == armed)
                    entry.store (-1);

            ccToParam[(size_t) cc].store (armed);
            armedParamIndex.store (-1);
            sendChangeMessage();   // async post — safe from the audio thread
        }

        const auto target = ccToParam[(size_t) cc].load();
        if (target >= 0)
        {
            const auto normValue = (float) message.getControllerValue() / 127.0f;
            auto* param = parametersByIndex[(size_t) target];

            // Both stores are plain lock-free writes. setValueNotifyingHost()
            // additionally walks the parameter's listener list under a
            // CriticalSection, which is not safe to take here, so that half is
            // deferred to the message thread -- but APVTS's denormalised cache
            // is only refreshed by that call, and the DSP reads the cache, so
            // it has to be written here too. See the class comment.
            param->setValue (normValue);
            if (auto* raw = rawByIndex[(size_t) target])
                raw->store (param->convertFrom0to1 (normValue), std::memory_order_relaxed);

            pendingApplyIndex.store (target, std::memory_order_relaxed);
            pendingApplyValue.store (normValue, std::memory_order_relaxed);
            triggerAsyncUpdate();   // safe from any thread
        }
    }
}

void MidiLearnManager::handleAsyncUpdate()
{
    const auto index = pendingApplyIndex.exchange (-1, std::memory_order_relaxed);
    if (index < 0)
        return;

    const auto value = pendingApplyValue.load (std::memory_order_relaxed);
    parametersByIndex[(size_t) index]->setValueNotifyingHost (value);
}

} // namespace spa
