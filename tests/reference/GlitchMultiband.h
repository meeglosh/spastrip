#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>

namespace glitch::fx
{

// Three-band compressor with ordinary compressor controls: per band a
// threshold, a ratio for what sits above it, a second ratio for what sits
// below it, attack, release and makeup gain.
//
// The downward half is the familiar one. The upward half is what makes a
// sparse loop sound dense -- it lifts material below the threshold instead of
// holding down material above it -- and it is also the dangerous half, because
// what sits below the threshold includes noise, room tone and the tail of a
// sound that was meant to end. Two guards, both deliberate and neither
// exposed as a control:
//
//   * the lift is capped at maxUpwardDb, because an upward ratio of 8:1 on a
//     band 60 dB down would otherwise ask for +52 dB of gain;
//   * it tapers to nothing below a noise floor, so a passage fading to silence
//     fades out rather than swelling into hiss.
//
// The crossover is a pair of 4th-order Linkwitz-Riley filters, with the low
// band passed through an allpass at the upper crossover so all three bands
// carry the same phase shift. The three then sum flat when nothing is
// compressing, which is an invariant the tests assert rather than a claim.
class Multiband
{
public:
    static constexpr int numBands = 3;

    struct Band
    {
        float thresholdDb = -24.0f;
        float ratio = 4.0f;        // above the threshold; 1 = off
        float upRatio = 1.0f;      // below the threshold; 1 = off
        float attackMs = 12.0f;
        float releaseMs = 120.0f;
        float gainDb = 0.0f;       // makeup
    };

    struct Params
    {
        bool enable = false;
        float mix = 1.0f;                 // dry/wet for the module as a whole
        float crossoverLowHz = 200.0f;    // low | mid
        float crossoverHighHz = 2000.0f;  // mid | high
        std::array<Band, numBands> bands {};
    };

    void prepare (double sr, int maxBlock)
    {
        sampleRate = juce::jmax (8000.0, sr);
        const juce::dsp::ProcessSpec spec { sampleRate,
                                            (juce::uint32) juce::jmax (1, maxBlock),
                                            2 };
        splitLow.prepare (spec);
        splitHigh.prepare (spec);
        alignLow.prepare (spec);
        alignLow.setType (juce::dsp::LinkwitzRileyFilterType::allpass);
        reset();
    }

    void reset()
    {
        splitLow.reset();
        splitHigh.reset();
        alignLow.reset();
        for (auto& b : bands)
            b = {};
    }

    // Signed, so the meter can show both directions: positive is upward gain,
    // negative is downward reduction.
    float bandGainDb (int band) const
    {
        return juce::isPositiveAndBelow (band, numBands) ? bands[(size_t) band].meterDb : 0.0f;
    }

    void process (juce::AudioBuffer<float>& buffer, const Params& p)
    {
        const int n = buffer.getNumSamples();
        const int numCh = juce::jmin (2, buffer.getNumChannels());
        if (n <= 0 || numCh <= 0)
            return;

        // The upper crossover has to stay above the lower one or the mid band
        // inverts. Clamping here rather than trusting the parameter ranges
        // keeps a preset written by another build from producing a broken
        // split.
        const float lowHz  = juce::jlimit (20.0f, 2000.0f, p.crossoverLowHz);
        const float highHz = juce::jlimit (lowHz * 1.25f, 18000.0f, p.crossoverHighHz);
        splitLow.setCutoffFrequency (lowHz);
        splitHigh.setCutoffFrequency (highHz);
        alignLow.setCutoffFrequency (highHz);

        const float mix = juce::jlimit (0.0f, 1.0f, p.mix);

        struct Settings { float attack, release, thresholdDb, downSlope, upSlope; };
        std::array<Settings, numBands> settings {};
        std::array<float, numBands> makeup {};

        for (int b = 0; b < numBands; ++b)
        {
            const auto& src = p.bands[(size_t) b];
            const float ratio   = juce::jmax (1.0f, src.ratio);
            const float upRatio = juce::jmax (1.0f, src.upRatio);

            settings[(size_t) b] = {
                coefficientFor (src.attackMs),
                coefficientFor (src.releaseMs),
                src.thresholdDb,
                // Ratio expressed as the slope of the dB-in/dB-out line, so
                // the per-sample maths is a multiply rather than a divide.
                1.0f - 1.0f / ratio,
                1.0f - 1.0f / upRatio
            };
            makeup[(size_t) b] = juce::Decibels::decibelsToGain (src.gainDb);
        }

        std::array<float, numBands> peakGainDb {};
        std::array<bool, numBands> sawPeak {};

        auto* left  = buffer.getWritePointer (0);
        auto* right = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

        for (int i = 0; i < n; ++i)
        {
            const float dryL = left[i];
            const float dryR = right != nullptr ? right[i] : dryL;

            // Split: low | rest, then rest into mid | high. The low band then
            // goes through the allpass so it picks up the same phase shift the
            // upper split imposed on the other two.
            float lowL, restL, lowR, restR;
            splitLow.processSample (0, dryL, lowL, restL);
            splitLow.processSample (1, dryR, lowR, restR);

            float midL, highL, midR, highR;
            splitHigh.processSample (0, restL, midL, highL);
            splitHigh.processSample (1, restR, midR, highR);

            lowL = alignLow.processSample (0, lowL);
            lowR = alignLow.processSample (1, lowR);

            const std::array<float, numBands> bandL { lowL, midL, highL };
            const std::array<float, numBands> bandR { lowR, midR, highR };

            float wetL = 0.0f, wetR = 0.0f;

            for (int b = 0; b < numBands; ++b)
            {
                auto& state = bands[(size_t) b];
                const auto& set = settings[(size_t) b];

                // One detector for both channels, so compression never pulls
                // the stereo image to one side.
                const float detector = juce::jmax (std::abs (bandL[(size_t) b]),
                                                   std::abs (bandR[(size_t) b]));
                const float coeff = detector > state.env ? set.attack : set.release;
                state.env = detector + coeff * (state.env - detector);

                const float levelDb = juce::Decibels::gainToDecibels (state.env, silenceDb);

                float gainDb = 0.0f;
                if (levelDb > set.thresholdDb)
                {
                    gainDb = -(levelDb - set.thresholdDb) * set.downSlope;
                }
                else if (set.upSlope > 0.0f)
                {
                    // Taper the lift away as the band approaches silence, so
                    // the noise floor is never what gets compressed upward.
                    const float taper = juce::jlimit (0.0f, 1.0f,
                                                      (levelDb - noiseFloorDb) / taperRangeDb + 1.0f);
                    gainDb = juce::jmin (maxUpwardDb,
                                         (set.thresholdDb - levelDb) * set.upSlope) * taper;
                }

                gainDb = juce::jlimit (-maxDownwardDb, maxUpwardDb, gainDb);
                state.meterDb = gainDb;

                if (! sawPeak[(size_t) b] || std::abs (gainDb) > std::abs (peakGainDb[(size_t) b]))
                {
                    peakGainDb[(size_t) b] = gainDb;
                    sawPeak[(size_t) b] = true;
                }

                const float gain = juce::Decibels::decibelsToGain (gainDb) * makeup[(size_t) b];
                wetL += bandL[(size_t) b] * gain;
                wetR += bandR[(size_t) b] * gain;
            }

            left[i] = dryL + mix * (wetL - dryL);
            if (right != nullptr)
                right[i] = dryR + mix * (wetR - dryR);
        }

        splitLow.snapToZero();
        splitHigh.snapToZero();
        alignLow.snapToZero();

        // The meter shows the block's extreme rather than its last sample,
        // which would otherwise be wherever the envelope happened to be at the
        // block boundary and would flicker.
        for (int b = 0; b < numBands; ++b)
            if (sawPeak[(size_t) b])
                bands[(size_t) b].meterDb = peakGainDb[(size_t) b];
    }

private:
    float coefficientFor (float milliseconds) const
    {
        const float seconds = juce::jmax (1.0e-5f, milliseconds * 0.001f);
        return std::exp (-1.0f / ((float) sampleRate * seconds));
    }

    struct BandState
    {
        float env = 0.0f;
        float meterDb = 0.0f;
    };

    static constexpr float maxUpwardDb   = 24.0f;
    static constexpr float maxDownwardDb = 48.0f;
    static constexpr float noiseFloorDb  = -60.0f;
    static constexpr float taperRangeDb  = 18.0f;
    static constexpr float silenceDb     = -100.0f;

    double sampleRate = 48000.0;
    juce::dsp::LinkwitzRileyFilter<float> splitLow, splitHigh, alignLow;
    std::array<BandState, numBands> bands {};

    JUCE_LEAK_DETECTOR (Multiband)
};

} // namespace glitch::fx
