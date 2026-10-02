#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>

namespace spa::dsp
{

// Stereo chorus: one modulated delay line per channel, each with its own LFO.
//
// Replaces juce::dsp::Chorus, which drives both channels from a SINGLE LFO --
// both sides sweep identically, so the effect images dead centre and reads as
// a phaser rather than a chorus (a tester's words on 1.0.22), and JUCE exposes
// no per-channel LFO phase to fix that from the outside.
//
// WIDTH is that missing control: the phase offset between the left and right
// LFOs. 0 = both in phase (narrow, the old mono-imaging behaviour), 1 = half a
// cycle apart, the two sides moving in opposite directions. That opposition is
// what removes the phaser character, because phasiness is exactly what you
// hear when both channels move together.
//
// Two voicings:
//   Vintage -- Juno 106 flavoured. Short (~5 ms) centre delay, triangle LFO,
//     a one-pole rolloff on the wet path standing in for the bucket-brigade
//     delay's limited bandwidth, gentle saturation, and the 106's signature
//     wet polarity inversion between left and right. Character, not a circuit
//     model.
//   Modern  -- clean digital. Sine LFO, longer centre delay, wider sweep, two
//     taps per channel for a thicker bed, full bandwidth, no saturation.
//
// Real-time safe: the delay lines are allocated in prepare() and nowhere else;
// process() has no allocation, no locks and no I/O.
class StereoChorus
{
public:
    // Append-only: serialized as the fxChorus.mode choice parameter.
    enum class Mode { vintage, modern };

    struct Params
    {
        bool enable = true;         // edge-detected here (see process())
        Mode mode = Mode::modern;
        float rateHz = 0.8f;
        float depth = 0.3f;         // 0..1
        float feedback = 0.0f;      // -0.9..0.9, clamped below
        float width = 0.5f;         // 0..1 L/R LFO phase offset, 1 = 180 deg
        float mix = 0.5f;           // 0 = dry, 1 = fully wet
    };

    void prepare (double sr, int /*maxBlockSize*/)
    {
        sampleRate = sr;
        // Sized in SECONDS rather than samples, so the line still covers the
        // longest delay the Modern voicing asks for at any host rate -- and
        // at any oversampling factor, since the whole FX chain runs at the
        // oversampled rate (up to 8x).
        delayLen = (int) (sr * maxDelaySeconds) + 4;
        delayBuf.setSize (2, delayLen, false, true, true);
        reset();
    }

    void reset()
    {
        delayBuf.clear();
        for (auto& st : channels)
            st = {};
        writePos = 0;
        lfoPhase = 0.0f;
        wasEnabled = false;
    }

    // Self-contained enable-edge tracking, same idiom as ModEffect: the chain
    // may gate this module by not calling process() at all, or by calling it
    // with enable=false, and either way the false->true transition is caught
    // here. On that edge the delay line and feedback state are cleared, so a
    // chorus that was switched off mid-ring cannot dump trapped feedback into
    // the mix when it comes back. lfoPhase is deliberately left running.
    void process (juce::AudioBuffer<float>& buffer, const Params& p)
    {
        if (! p.enable)
        {
            wasEnabled = false;
            return;
        }

        const int n = buffer.getNumSamples();
        const int numCh = juce::jmin (2, buffer.getNumChannels());
        if (delayLen <= 4 || n <= 0 || numCh <= 0)
            return;

        if (! wasEnabled)
        {
            delayBuf.clear();
            for (auto& st : channels)
                st = {};
            wasEnabled = true;
        }

        const bool vintage = (p.mode == Mode::vintage);
        const float depth = juce::jlimit (0.0f, 1.0f, p.depth);
        const float mix   = juce::jlimit (0.0f, 1.0f, p.mix);
        // Clamped short of unity: this is a recirculating path and the knob's
        // own range (+/-0.9) already sits where a comb rings for a long time.
        const float fb    = juce::jlimit (-0.85f, 0.85f, p.feedback);
        // WIDTH -> L/R LFO phase offset. 0.5 of a cycle is 180 degrees, so
        // width=1 puts the two sides exactly in opposition.
        const float spread = 0.5f * juce::jlimit (0.0f, 1.0f, p.width);

        // Vintage sits short and shallow (BBD-length delays); Modern is
        // longer and sweeps further. Sweep is fully proportional to depth, so
        // depth=0 really is a static delay in both voicings.
        const float centreMs = vintage ? 5.0f : 12.0f;
        const float sweepMs  = (vintage ? 4.0f : 12.0f) * depth;
        // Modern only: a second, shorter tap per channel. Two chorus voices a
        // quarter cycle apart off one delay line -- lush, and nearly free.
        const float centre2Ms = 8.0f;
        const float sweep2Ms  = 0.6f * sweepMs;

        const float phaseInc = (float) (juce::jlimit (0.001f, 40.0f, p.rateHz) / sampleRate);
        // BBD bandwidth stand-in, wet path only. Vintage's default rate
        // (0.8 Hz) is already in Juno territory; the triangle shape is what
        // carries the rest of the character.
        const float lpCoeff = vintage ? onePoleCoeff (7000.0f) : 0.0f;

        float* chData[2] { buffer.getWritePointer (0),
                           numCh > 1 ? buffer.getWritePointer (1) : nullptr };
        float* lines[2] { delayBuf.getWritePointer (0), delayBuf.getWritePointer (1) };

        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto& st = channels[(size_t) ch];
                float* line = lines[ch];

                const float phase = lfoPhase + (ch == 1 ? spread : 0.0f);
                const float lfo = vintage ? triangleBipolar (phase) : sineBipolar (phase);

                const float dry = chData[ch][i];
                // The feedback term is the only unbounded path in here, so
                // the recirculated sample is clamped before it goes back in:
                // a runaway guard, transparent at any sane level.
                line[writePos] = dry + fb * juce::jlimit (-2.0f, 2.0f, st.fbLast);

                float wet = readLine (line, delayLen, writePos,
                                      msToSamples (centreMs + 0.5f * sweepMs * lfo));

                if (vintage)
                {
                    st.lp += lpCoeff * (wet - st.lp);
                    wet = saturate (st.lp);
                }
                else
                {
                    const float lfo2 = sineBipolar (phase + 0.25f);
                    wet = 0.5f * (wet + readLine (line, delayLen, writePos,
                                                  msToSamples (centre2Ms + 0.5f * sweep2Ms * lfo2)));
                }

                st.fbLast = wet;
                // The Juno inverts the wet signal on one side; it is a big
                // part of why that chorus sounds as wide as it does.
                const float wetOut = (vintage && ch == 1) ? -wet : wet;
                chData[ch][i] = dry + (wetOut - dry) * mix;
            }

            if (++writePos >= delayLen)
                writePos = 0;
            lfoPhase += phaseInc;
            if (lfoPhase >= 1.0f)
                lfoPhase -= 1.0f;
        }
    }

private:
    // Longest delay asked for is Modern's 12 ms centre + 6 ms of sweep; 40 ms
    // leaves plenty of room above that.
    static constexpr double maxDelaySeconds = 0.04;

    struct ChannelState
    {
        float fbLast = 0.0f;
        float lp = 0.0f;       // vintage wet-path one-pole state
    };

    static float sineBipolar (float phase)
    {
        return std::sin (phase * juce::MathConstants<float>::twoPi);
    }

    static float triangleBipolar (float phase)
    {
        phase -= std::floor (phase);
        return phase < 0.5f ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
    }

    // Bounded (+/-1) soft saturation with unity slope around zero: gentle a
    // little way past nominal level rather than a clipper, and it cannot run
    // away however hot the feedback path gets.
    static float saturate (float x)
    {
        return x / std::sqrt (1.0f + x * x);
    }

    float onePoleCoeff (float hz) const
    {
        return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hz / (float) sampleRate);
    }

    float msToSamples (float ms) const
    {
        return juce::jlimit (1.0f, (float) (delayLen - 2),
                             ms * 0.001f * (float) sampleRate);
    }

    // Linear-interpolated read, delaySamps behind the write head.
    //
    // The index is clamped, never assumed: a read position a hair below zero
    // is wrapped by `+= len`, and float precision can round that to exactly
    // (float) len, so truncation hands back i0 == len -- one past the end.
    // Two shipped bugs in this codebase came from exactly that pattern (the
    // FDN reverb's noise bursts and the delay line beside it), so this reads
    // through a single guarded accessor.
    static float readLine (const float* line, int len, int writePosition, float delaySamps)
    {
        float rp = (float) writePosition - delaySamps;
        while (rp < 0.0f)
            rp += (float) len;

        int i0 = (int) rp;
        const float frac = rp - (float) i0;
        while (i0 >= len)
            i0 -= len;
        if (i0 < 0)
            i0 = 0;

        int i1 = i0 + 1;
        if (i1 >= len)
            i1 -= len;

        return line[i0] + frac * (line[i1] - line[i0]);
    }

    double sampleRate = 48000.0;
    int delayLen = 0;
    int writePos = 0;
    float lfoPhase = 0.0f;
    bool wasEnabled = false;
    juce::AudioBuffer<float> delayBuf;
    ChannelState channels[2];
};

} // namespace spa::dsp
