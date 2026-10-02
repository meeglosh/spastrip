#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <cmath>

namespace spa::dsp
{

// Maximizer: input drive into a brickwall limiter at a ceiling. Optional
// lookahead (reports latency to the host when on); zero-latency otherwise.
// Character modes shape release + saturation. Stereo link + a coarse true-peak
// margin. Tracks gain reduction for a meter.
class Limiter
{
public:
    void prepare (double sr, int /*maxBlock*/)
    {
        sampleRate = sr;
        const int la = lookaheadSamples();
        delayL.assign ((size_t) juce::jmax (1, la), 0.0f);
        delayR.assign ((size_t) juce::jmax (1, la), 0.0f);
        reset();
    }

    void reset() { gr = 1.0f; grMeter = 1.0f; outPk = 0.0f; wr = 0; }

    struct Params
    {
        bool enable = false;
        float driveDb = 0.0f;       // input gain
        float ceilingDb = -0.3f;    // brickwall output
        float releaseMs = 120.0f;
        bool autoRelease = false;
        int character = 0;          // 0 clean, 1 punchy, 2 aggressive
        float stereoLink = 1.0f;    // 0..1
        bool truePeak = false;
        bool lookahead = false;
        bool autoGain = false;      // compensate the drive at the output
    };

    int lookaheadSamples() const { return (int) (0.0015 * sampleRate); }   // 1.5 ms
    int latencySamples (const Params& p) const
    {
        return (p.enable && p.lookahead) ? lookaheadSamples() : 0;
    }
    float gainReductionDb() const { return juce::Decibels::gainToDecibels (grMeter); }
    float outputPeak() const { return outPk; }   // block output peak, 0..~1

    void process (juce::AudioBuffer<float>& buffer, const Params& p)
    {
        const int n = buffer.getNumSamples();
        const int numCh = juce::jmin (2, buffer.getNumChannels());
        const float drive = juce::Decibels::decibelsToGain (p.driveDb);
        float ceiling = juce::Decibels::decibelsToGain (p.ceilingDb);
        if (p.truePeak) ceiling *= 0.89f;   // ~ -1 dB inter-sample margin (coarse)

        const int la = p.lookahead ? juce::jmin ((int) delayL.size(), lookaheadSamples()) : 0;
        const float relMs  = p.autoRelease ? juce::jlimit (40.0f, 400.0f, p.releaseMs)
                                           : juce::jmax (1.0f, p.releaseMs);
        const float relCoef = std::exp (-1.0f / (float) (sampleRate * relMs * 0.001f));
        const float atkCoef = la > 0 ? std::exp (-1.0f / (float) juce::jmax (1, la)) : 0.0f;
        const float link = juce::jlimit (0.0f, 1.0f, p.stereoLink);
        // Auto-gain undoes the input drive at the output, so the drive knob
        // controls how hard it limits without changing the overall level.
        const float outGain = p.autoGain && drive > 0.0f ? 1.0f / drive : 1.0f;

        float* L = buffer.getWritePointer (0);
        float* R = numCh > 1 ? buffer.getWritePointer (1) : L;
        float gmin = 1.0f, opk = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float xL = L[i] * drive;
            const float xR = R[i] * drive;
            const float aL = std::abs (xL), aR = std::abs (xR);
            const float linked = juce::jmax (aL, aR);
            const float peak = juce::jmax (link * linked + (1.0f - link) * aL,
                                           link * linked + (1.0f - link) * aR);

            const float target = peak > ceiling ? ceiling / peak : 1.0f;
            gr = target < gr ? target + (gr - target) * atkCoef    // pull down over lookahead
                             : target + (gr - target) * relCoef;   // recover

            float dL = xL, dR = xR;
            if (la > 0)
            {
                dL = delayL[(size_t) wr]; dR = delayR[(size_t) wr];
                delayL[(size_t) wr] = xL; delayR[(size_t) wr] = xR;
                wr = (wr + 1) % la;
            }

            float oL = dL * gr, oR = dR * gr;
            if (p.character == 2)   // aggressive: soft saturate into the ceiling
            {
                oL = std::tanh (oL / ceiling) * ceiling;
                oR = std::tanh (oR / ceiling) * ceiling;
            }
            L[i] = juce::jlimit (-ceiling, ceiling, oL) * outGain;
            if (numCh > 1) R[i] = juce::jlimit (-ceiling, ceiling, oR) * outGain;

            gmin = juce::jmin (gmin, gr);
            opk = juce::jmax (opk, std::abs (L[i]), numCh > 1 ? std::abs (R[i]) : 0.0f);
        }
        grMeter = gmin;
        outPk = opk;
    }

private:
    double sampleRate = 48000.0;
    std::vector<float> delayL, delayR;
    float gr = 1.0f, grMeter = 1.0f, outPk = 0.0f;
    int wr = 0;
};

} // namespace spa::dsp
