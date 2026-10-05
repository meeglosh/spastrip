#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include <cstdint>

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
//   VHS     -- 80s synthwave tape (1.0.32): slow irregular WOW, faster
//     FLUTTER, tape-bandwidth TONE, soft saturation, signal-following hiss
//     and brief dropouts. Self-contained (processVhs); Vintage and Modern
//     never touch any of it and stay sample-identical to 1.0.31.
//   Modern  -- clean digital. Sine LFO, longer centre delay, wider sweep, two
//     taps per channel for a thicker bed, full bandwidth, no saturation.
//
// Real-time safe: the delay lines are allocated in prepare() and nowhere else;
// process() has no allocation, no locks and no I/O.
class StereoChorus
{
public:
    // Append-only: serialized as the fxChorus.mode choice parameter.
    enum class Mode { vintage, modern, vhs };

    struct Params
    {
        bool enable = true;         // edge-detected here (see process())
        Mode mode = Mode::modern;
        float rateHz = 0.8f;
        float depth = 0.3f;         // 0..1
        float feedback = 0.0f;      // -0.9..0.9, clamped below
        float width = 0.5f;         // 0..1 L/R LFO phase offset, 1 = 180 deg
        float mix = 0.5f;           // 0 = dry, 1 = fully wet
        // VHS mode only, all 0..1. RATE sets the wow speed (and nudges the
        // flutter), DEPTH scales both modulation amounts, FB/MIX/WIDTH as usual.
        float vhsWow = 0.4f;
        float vhsFlutter = 0.25f;
        float vhsTone = 0.45f;
        float vhsSat = 0.3f;
        float vhsHiss = 0.15f;
        float vhsDropouts = 0.1f;
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
        resetVhs();
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
            resetVhs();
            wasEnabled = true;
        }

        if (p.mode == Mode::vhs)
        {
            processVhs (buffer, p, n, numCh);
            return;
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


    // ---- VHS voicing ------------------------------------------------------
    // Everything below is sized in prepare() (the shared delay line) or is a
    // fixed-size member, so processVhs() allocates nothing. All time constants
    // derive from sampleRate -- the FX chain runs at the oversampled rate.
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1[2] { 0, 0 }, z2[2] { 0, 0 };

        void setLowpass (double sr, double fc, double q)
        {
            fc = juce::jmin (fc, sr * 0.45);
            const double w = 2.0 * juce::MathConstants<double>::pi * fc / sr;
            const double al = std::sin (w) / (2.0 * q), c = std::cos (w), a0 = 1.0 + al;
            b0 = (float) ((1.0 - c) * 0.5 / a0);
            b1 = (float) ((1.0 - c) / a0);
            b2 = b0;
            a1 = (float) (-2.0 * c / a0);
            a2 = (float) ((1.0 - al) / a0);
        }

        void setPeak (double sr, double fc, double q, double gainDb)
        {
            const double A = std::pow (10.0, gainDb / 40.0);
            const double w = 2.0 * juce::MathConstants<double>::pi * fc / sr;
            const double al = std::sin (w) / (2.0 * q), c = std::cos (w), a0 = 1.0 + al / A;
            b0 = (float) ((1.0 + al * A) / a0);
            b1 = (float) (-2.0 * c / a0);
            b2 = (float) ((1.0 - al * A) / a0);
            a1 = b1;
            a2 = (float) ((1.0 - al / A) / a0);
        }

        float run (int ch, float x)
        {
            const float y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            // Flush: a decaying filter tail otherwise lingers in denormals for
            // seconds, and "silent in, exactly silent out" is a promise (HISS).
            if (std::abs (z1[ch]) < 1.0e-20f) z1[ch] = 0.0f;
            if (std::abs (z2[ch]) < 1.0e-20f) z2[ch] = 0.0f;
            return y;
        }
    };

    struct Walker   // smoothed random walk in about [-1, 1]
    {
        float cur = 0.0f, target = 0.0f;
        int countdown = 0;
    };

    struct VhsState
    {
        Biquad warmth, lp;
        float hissHp[2] { 0, 0 }, hissLp[2] { 0, 0 }, dropLp[2] { 0, 0 };
        float fb[2] { 0, 0 };
        Walker wowCommon, wowOwn[2], flutOwn[2];
        float phW1 = 0, phW2 = 0, phF1 = 0, phF2 = 0;
        float env = 0.0f;
        float dropEnv = 0.0f, dropEnv1 = 0.0f, dropTarget = 0.0f;
        int dropRemaining = 0;
        float lastTone = -1.0f;
        std::uint32_t rng = 0x1badf00du;
        std::uint32_t hissRng = 0x2545f491u;   // own stream: hiss never shifts wow/dropout draws
    };
    VhsState vhs;

    void resetVhs() { vhs = VhsState {}; }

    float vhsRand01()
    {
        auto& s = vhs.rng;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float) (s >> 8) * (1.0f / 16777216.0f);
    }

    float hissWhite()
    {
        auto& s = vhs.hissRng;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return ((float) (s >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f) * 1.732f;   // ~unit RMS
    }

    float stepWalker (Walker& w, float meanSamples, float smooth)
    {
        if (--w.countdown <= 0)
        {
            w.target = vhsRand01() * 2.0f - 1.0f;
            w.countdown = juce::jmax (1, (int) (meanSamples * (0.5f + vhsRand01())));
        }
        w.cur += smooth * (w.target - w.cur);
        return w.cur;
    }

    void processVhs (juce::AudioBuffer<float>& buffer, const Params& p, int n, int numCh)
    {
        const float sr = (float) sampleRate;
        const float wow = juce::jlimit (0.0f, 1.0f, p.vhsWow);
        const float flutter = juce::jlimit (0.0f, 1.0f, p.vhsFlutter);
        const float tone = juce::jlimit (0.0f, 1.0f, p.vhsTone);
        const float sat = juce::jlimit (0.0f, 1.0f, p.vhsSat);
        const float hiss = juce::jlimit (0.0f, 1.0f, p.vhsHiss);
        const float drop = juce::jlimit (0.0f, 1.0f, p.vhsDropouts);
        const float mix = juce::jlimit (0.0f, 1.0f, p.mix);
        const float fbAmt = juce::jlimit (-0.85f, 0.85f, p.feedback);
        const float width = juce::jlimit (0.0f, 1.0f, p.width);
        // DEPTH is the master modulation scale; 0.3 (the knob's default) = 1.
        const float modScale = juce::jlimit (0.0f, 2.5f, juce::jlimit (0.0f, 1.0f, p.depth) / 0.3f);

        // TONE: 2-pole low-pass, 5.5 kHz (0) .. 16 kHz (1), exponential, plus a
        // gentle low-mid warmth bump that fades as the tape brightens.
        if (tone != vhs.lastTone)
        {
            vhs.lastTone = tone;
            const double fc = 5500.0 * std::pow (16000.0 / 5500.0, (double) tone);
            vhs.lp.setLowpass (sampleRate, fc, 0.7071);
            vhs.warmth.setPeak (sampleRate, 250.0, 0.8, 1.0 + 2.0 * (1.0 - (double) tone));
        }

        // WOW: delay modulation, amplitude 7 ms at full (pitch deviation is
        // 2*pi*f*A, so ~24 cents peak at the default 0.8 Hz / 40 %). FLUTTER:
        // 0.32 ms at full around 9 Hz (~8 cents at the default 25 %).
        const float wowHz = juce::jlimit (0.25f, 1.5f, p.rateHz);
        const float flutHz = 9.0f * (0.75f + 0.5f * juce::jlimit (0.0f, 1.0f, wowHz / 1.5f));
        const float wowAmpMs = juce::jmin (16.0f, 7.0f * wow * modScale);
        const float flutAmpMs = 0.32f * flutter * modScale;
        const float incW1 = wowHz / sr, incW2 = wowHz * 0.37f / sr;
        const float incF1 = flutHz / sr, incF2 = flutHz * 1.618f / sr;
        const float wowWalkLen = sr / wowHz, wowSmooth = 1.0f - std::exp (-6.2831853f * wowHz * 0.7f / sr);
        const float flutWalkLen = sr / flutHz, flutSmooth = 1.0f - std::exp (-6.2831853f * flutHz / sr);
        const float centreMs = 20.0f;

        // SATURATION: tanh drive, normalised at a 0.3 peak so a moderate signal
        // keeps its level (the knob changes colour, not loudness).
        const float satDrive = 1.0f + 4.0f * sat;
        // The 0.1 trim keeps full drive within 1 dB (a squashed sine gains RMS).
        const float satNorm = (0.3f / std::tanh (satDrive * 0.3f)) * (1.0f - 0.1f * sat);
        const bool satOn = sat > 1.0e-4f;

        // HISS follows the input: ~10 ms attack, ~400 ms release on |dry|.
        const float hissAmp = 0.08f * hiss * hiss;
        const float envA = 1.0f - std::exp (-1.0f / (0.010f * sr));
        const float envR = 1.0f - std::exp (-1.0f / (0.400f * sr));
        const float hissHpC = 1.0f - std::exp (-6.2831853f * 2000.0f / sr);
        const float hissLpC = 1.0f - std::exp (-6.2831853f * 9000.0f / sr);

        // DROPOUTS: 0.05 + 1.5*knob events/s, 10-80 ms, smoothed edges.
        const float dropRate = drop > 0.0f ? (0.05f + 1.5f * drop) / sr : 0.0f;
        const float dropAtk = 1.0f - std::exp (-1.0f / (0.002f * sr));
        const float dropRel = 1.0f - std::exp (-1.0f / (0.006f * sr));
        const float dropLpC = 1.0f - std::exp (-6.2831853f * 1500.0f / sr);

        float* chData[2] { buffer.getWritePointer (0),
                           numCh > 1 ? buffer.getWritePointer (1) : nullptr };
        float* lines[2] { delayBuf.getWritePointer (0), delayBuf.getWritePointer (1) };

        for (int i = 0; i < n; ++i)
        {
            // Shared (both channels) once per sample.
            const float common = stepWalker (vhs.wowCommon, wowWalkLen, wowSmooth);
            float walkOwnW[2], walkOwnF[2];
            for (int ch = 0; ch < numCh; ++ch)
            {
                walkOwnW[ch] = stepWalker (vhs.wowOwn[ch], wowWalkLen, wowSmooth);
                walkOwnF[ch] = stepWalker (vhs.flutOwn[ch], flutWalkLen, flutSmooth);
            }

            // Dropout event state machine (common to both channels).
            if (vhs.dropRemaining > 0)
            {
                if (--vhs.dropRemaining == 0)
                    vhs.dropTarget = 0.0f;
            }
            else if (dropRate > 0.0f && vhsRand01() < dropRate)
            {
                vhs.dropRemaining = (int) (sr * (0.010f + 0.070f * vhsRand01()));
                vhs.dropTarget = (0.5f + 0.5f * vhsRand01()) * (0.25f + 0.75f * drop);
            }
            // Two cascaded one-poles: the level starts moving with zero slope,
            // so an event's edge has no kink (a single pole steps on sample 1).
            vhs.dropEnv1 += (vhs.dropTarget > vhs.dropEnv1 ? dropAtk : dropRel) * (vhs.dropTarget - vhs.dropEnv1);
            vhs.dropEnv += (vhs.dropTarget > vhs.dropEnv ? dropAtk : dropRel) * (vhs.dropEnv1 - vhs.dropEnv);
            if (vhs.dropRemaining == 0 && vhs.dropEnv1 < 1.0e-6f)
                vhs.dropEnv1 = vhs.dropEnv = 0.0f;

            float inPeak = 0.0f;
            for (int ch = 0; ch < numCh; ++ch)
                inPeak = juce::jmax (inPeak, std::abs (chData[ch][i]));
            vhs.env += (inPeak > vhs.env ? envA : envR) * (inPeak - vhs.env);
            if (vhs.env < 1.0e-4f)
                vhs.env = 0.0f;   // -80 dB: exact silence once the release has run out

            for (int ch = 0; ch < numCh; ++ch)
            {
                float* line = lines[ch];
                const float chOff = ch == 1 ? 0.5f * width * 0.3f : 0.0f;

                auto sinp = [] (float ph) { return std::sin (ph * 6.2831853f); };
                const float wowSig = (0.5f * sinp (vhs.phW1 + chOff) + 0.3f * sinp (vhs.phW2 + 2.0f * chOff)
                                      + 0.4f * ((1.0f - width) * common + width * walkOwnW[ch])) / 1.2f;
                const float flutSig = 0.7f * sinp (vhs.phF1 + 3.0f * chOff) + 0.3f * sinp (vhs.phF2)
                                      + 0.25f * walkOwnF[ch];

                const float dry = chData[ch][i];
                line[writePos] = dry + fbAmt * juce::jlimit (-2.0f, 2.0f, vhs.fb[ch]);

                float wet = readLine (line, delayLen, writePos,
                                      msToSamples (centreMs + wowAmpMs * wowSig + flutAmpMs * flutSig));

                if (satOn)
                    wet = satNorm * std::tanh (satDrive * wet);

                wet = vhs.lp.run (ch, vhs.warmth.run (ch, wet));

                // Dropout: level dip + treble loss.
                if (vhs.dropEnv > 0.0f)
                {
                    vhs.dropLp[ch] += dropLpC * (wet - vhs.dropLp[ch]);
                    const float e = vhs.dropEnv;
                    wet = (wet + e * (vhs.dropLp[ch] - wet)) * (1.0f - 0.6f * e);
                }
                else
                    vhs.dropLp[ch] = wet;

                vhs.fb[ch] = wet;

                if (vhs.env > 0.0f && hissAmp > 0.0f)
                {
                    const float white = hissWhite();
                    vhs.hissHp[ch] += hissHpC * (white - vhs.hissHp[ch]);
                    vhs.hissLp[ch] += hissLpC * ((white - vhs.hissHp[ch]) - vhs.hissLp[ch]);
                    wet += vhs.hissLp[ch] * hissAmp * juce::jmin (1.0f, vhs.env * 4.0f) ;
                }
                else
                {
                    vhs.hissHp[ch] = 0.0f;
                    vhs.hissLp[ch] = 0.0f;
                }

                chData[ch][i] = dry + (wet - dry) * mix;
            }

            auto wrap = [] (float& ph, float inc) { ph += inc; if (ph >= 1.0f) ph -= 1.0f; };
            wrap (vhs.phW1, incW1); wrap (vhs.phW2, incW2);
            wrap (vhs.phF1, incF1); wrap (vhs.phF2, incF2);
            if (++writePos >= delayLen)
                writePos = 0;
        }
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
