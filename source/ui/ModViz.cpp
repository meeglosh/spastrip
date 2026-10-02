#include "ModViz.h"

#include "../mod/ModTargets.h"

namespace spa::ui
{

namespace
{
    void collect (juce::Component& c, std::vector<juce::Slider*>& out)
    {
        for (auto* child : c.getChildren())
        {
            if (auto* s = dynamic_cast<juce::Slider*> (child))
                if (s->getProperties().contains ("paramID"))
                    out.push_back (s);
            collect (*child, out);
        }
    }
}

ModVizDriver::ModVizDriver (SPAStripProcessor& p, juce::Component& root)
    : processor (p), rootComponent (root)
{
}

void ModVizDriver::setExtraRoots (std::vector<juce::Component*> roots) { extraRoots = std::move (roots); }

ModVizDriver::~ModVizDriver()
{
    stopTimer();
}

void ModVizDriver::rescan()
{
    std::vector<juce::Slider*> sliders;
    collect (rootComponent, sliders);
    for (auto* extra : extraRoots)   // e.g. the tab panels that are not parented right now
        if (extra != nullptr && extra->getParentComponent() == nullptr)
            collect (*extra, sliders);
    entries.clear();
    safe.clear();
    for (auto* s : sliders)
    {
        const auto id = s->getProperties()["paramID"].toString();
        if (mod::indexOf (id) < 0)
            continue;   // not a modulation target: never gets the overlay
        entries.push_back ({ s, id, processor.getAPVTS().getParameter (id) });
        safe.emplace_back (s);
    }
}

void ModVizDriver::setActive (bool on)
{
    if (on)
    {
        if (! isTimerRunning())
            startTimerHz (30);
    }
    else
        stopTimer();
}

ModVizDriver::Reach ModVizDriver::getReach (const juce::String& paramID) const
{
    Reach r;
    auto& tel = processor.getTelemetry();
    for (int s = 0; s < SPAStripProcessor::numModSlots; ++s)
    {
        if (processor.getModSlotTarget (s) != paramID)
            continue;
        r.assigned = true;
        const auto* depth = processor.getAPVTS().getRawParameterValue (params::id::modSlotDepth (s));
        const float d = depth != nullptr ? depth->load() : 0.0f;
        if (d > 0.0f) r.pos += d;
        else          r.neg += -d;
        r.offset += tel.modSlotOffset[(size_t) s].load (std::memory_order_relaxed);
    }
    return r;
}

void ModVizDriver::timerCallback()
{
    // slot -> target id, once per tick.
    std::array<juce::String, SPAStripProcessor::numModSlots> targets;
    std::array<float, SPAStripProcessor::numModSlots> depths {};
    bool any = false;
    for (int s = 0; s < SPAStripProcessor::numModSlots; ++s)
    {
        targets[(size_t) s] = processor.getModSlotTarget (s);
        any = any || targets[(size_t) s].isNotEmpty();
        if (const auto* d = processor.getAPVTS().getRawParameterValue (params::id::modSlotDepth (s)))
            depths[(size_t) s] = d->load();
    }

    auto& tel = processor.getTelemetry();
    constexpr float epsilon = 1.0f / 512.0f;

    for (size_t i = 0; i < entries.size(); ++i)
    {
        auto* slider = safe[i].getComponent();
        if (slider == nullptr)
            continue;
        const auto& e = entries[i];

        bool assigned = false;
        float neg = 0.0f, pos = 0.0f, offset = 0.0f;
        if (any)
            for (int s = 0; s < SPAStripProcessor::numModSlots; ++s)
                if (targets[(size_t) s] == e.paramID)
                {
                    assigned = true;
                    const float d = depths[(size_t) s];
                    if (d > 0.0f) pos += d; else neg += -d;
                    offset += tel.modSlotOffset[(size_t) s].load (std::memory_order_relaxed);
                }

        auto& props = slider->getProperties();
        const bool wasAssigned = (bool) props.getWithDefault ("modAssigned", false);
        const float wasNeg = (float) (double) props.getWithDefault ("modRangeNeg", 0.0);
        const float wasPos = (float) (double) props.getWithDefault ("modRangePos", 0.0);
        bool changed = false;
        if (assigned != wasAssigned || std::abs (neg - wasNeg) > epsilon || std::abs (pos - wasPos) > epsilon)
        {
            props.set ("modAssigned", assigned);
            props.set ("modRangeNeg", (double) neg);
            props.set ("modRangePos", (double) pos);
            changed = true;
        }

        // Live overlay: the knob's normalised base plus the summed offsets (the
        // engine's own clamp), shown while the offset is non-zero.
        const bool active = assigned && std::abs (offset) > 1.0e-4f && isLiveShowing (*slider);
        const bool wasActive = (bool) props.getWithDefault ("modActive", false);
        const float wasValue = (float) (double) props.getWithDefault ("modValue", 0.0);
        if (active)
        {
            const float base = e.parameter != nullptr ? e.parameter->getValue() : 0.0f;
            const float value = juce::jlimit (0.0f, 1.0f, base + offset);
            if (! wasActive || std::abs (value - wasValue) > (1.0f / 256.0f))
            {
                props.set ("modActive", true);
                props.set ("modValue", (double) value);
                changed = true;
            }
        }
        else if (wasActive)
        {
            props.set ("modActive", false);
            changed = true;
        }

        if (changed)
            slider->repaint();
    }
}

} // namespace spa::ui
