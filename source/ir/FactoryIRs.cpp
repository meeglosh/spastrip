#include "FactoryIRs.h"

#include <SPAStripFactoryData.h>

#include <algorithm>

namespace spa::factory
{

namespace
{
    // One generated row per embedded IR: { id, display name, category, file }.
    // Written by CMakeLists.txt from assets/irs/manifest.json (see there).
    struct Row { const char* id; const char* name; const char* category; const char* file; };
    constexpr Row kRows[] = {
       #include "FactoryIRTable.inc"
    };

    const char* findResource (const char* originalFile, int& size)
    {
        namespace data = spa_factory_data;
        for (int i = 0; i < data::namedResourceListSize; ++i)
        {
            const char* name = data::namedResourceList[i];
            const char* orig = data::getNamedResourceOriginalFilename (name);
            if (orig != nullptr && juce::String (orig) == originalFile)
                return data::getNamedResource (name, size);
        }
        size = 0;
        return nullptr;
    }
}

const std::vector<IR>& list()
{
    static const std::vector<IR> grouped = []
    {
        std::vector<juce::String> categories;
        for (const auto& r : kRows)
            if (std::find (categories.begin(), categories.end(), juce::String (r.category)) == categories.end())
                categories.emplace_back (r.category);

        std::vector<IR> out;
        for (const auto& c : categories)
            for (const auto& r : kRows)
                if (c == r.category)
                    out.push_back ({ r.id, r.name, r.category });
        return out;
    }();
    return grouped;
}

const IR* find (const juce::String& id)
{
    for (const auto& ir : list())
        if (ir.id == id)
            return &ir;
    return nullptr;
}

bool decode (const juce::String& id, juce::AudioBuffer<float>& out, double& sampleRate)
{
    const Row* row = nullptr;
    for (const auto& r : kRows)
        if (id == r.id)
            row = &r;
    if (row == nullptr)
        return false;

    int size = 0;
    const char* bytes = findResource (row->file, size);
    if (bytes == nullptr || size <= 0)
        return false;

    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (
        flac.createReaderFor (new juce::MemoryInputStream (bytes, (size_t) size, false), true));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return false;

    const int n = (int) reader->lengthInSamples;
    const int numCh = (int) juce::jmin ((juce::uint32) 2, reader->numChannels);
    juce::AudioBuffer<float> buf (numCh, n);
    if (! reader->read (&buf, 0, n, 0, true, true))
        return false;

    out = std::move (buf);
    sampleRate = reader->sampleRate;
    return true;
}

juce::String credits()
{
    int size = 0;
    if (const char* bytes = findResource ("CREDITS.md", size))
        return juce::String::fromUTF8 (bytes, size);
    return {};
}

} // namespace spa::factory
