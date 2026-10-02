#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace spa::mod
{

// Sidechain envelope follower (audio thread, real-time safe: no allocation, no
// locks, everything is preallocated / plain members).
//
//   stereo in -> detector high-pass (2nd-order Butterworth, per channel)
//             -> sensitivity gain (smoothed, so turning the knob while
//                auditioning does not click)
//             -> peak = max(|L|, |R|)
//             -> one-pole attack / release follower
//             -> envelope, clamped 0..1 (the follower STATE is clamped too)
//
// The follower state is capped at 1.0 (the full-scale envelope the modulation
// matrix can ever see). It used to be left unclamped, so a very hot signal
// (+24 dB sensitivity on a loud source) parked the state far above 1 while the
// output stayed pinned at 1, and the release then spent ln(peak) time
// constants decaying invisibly before the output moved at all. Only the state
// is capped, not the input: the attack still aims at the true rectified level,
// so a hot signal saturates the envelope exactly as quickly as before, and the
// release starts from 1.0 the instant the signal falls away.
class SidechainDetector
{
public:
    struct Settings
    {
        float gainDb = 0.0f;
        float attackMs = 10.0f;
        float releaseMs = 150.0f;
        float hpfHz = 20.0f;
    };

    void prepare (double sr)
    {
        sampleRate = sr;
        gain.reset (sr, 0.02);
        gain.setCurrentAndTargetValue (1.0f);
        lastHpf = -1.0f;
        reset();
    }

    void reset()
    {
        for (auto& c : hpf)
            c = {};
        env = 0.0f;
    }

    // Per chunk. Coefficients are plain arithmetic (no allocation).
    void setSettings (const Settings& s)
    {
        gain.setTargetValue (juce::Decibels::decibelsToGain (s.gainDb));
        atkCoef = coefFor (s.attackMs);
        relCoef = coefFor (s.releaseMs);
        if (! juce::approximatelyEqual (s.hpfHz, lastHpf))
        {
            lastHpf = s.hpfHz;
            designHighpass (s.hpfHz);
        }
    }

    // Processes n samples. `inL`/`inR` are read only. `envOut` (host-rate
    // envelope, 0..1, n samples) is always written. `listenL`/`listenR`
    // (may be null) receive the detector signal after HPF and gain.
    void process (const float* inL, const float* inR, int n, float* envOut,
                  float* listenL, float* listenR)
    {
        for (int i = 0; i < n; ++i)
        {
            float l = inL[i], r = inR[i];
            if (! (std::abs (l) < 1.0e6f)) l = 0.0f;   // NaN / Inf / absurd: never poison the follower
            if (! (std::abs (r) < 1.0e6f)) r = 0.0f;

            l = runHpf (0, l);
            r = runHpf (1, r);
            const float g = gain.getNextValue();
            l *= g;
            r *= g;
            if (listenL != nullptr) listenL[i] = l;
            if (listenR != nullptr) listenR[i] = r;

            const float x = juce::jmax (std::abs (l), std::abs (r));
            const float coef = x > env ? atkCoef : relCoef;
            env = x + coef * (env - x);
            if (env > 1.0f) env = 1.0f;       // release always starts from full scale
            if (env < 1.0e-12f) env = 0.0f;   // keep the state out of denormals
            envOut[i] = env;
        }
    }

    float currentEnvelope() const { return env; }

private:
    struct Biquad
    {
        float z1 = 0.0f, z2 = 0.0f;
    };

    float coefFor (float ms) const
    {
        const double seconds = juce::jmax (1.0e-5, (double) ms * 0.001);
        return (float) std::exp (-1.0 / (sampleRate * seconds));
    }

    void designHighpass (float hz)
    {
        const double f = juce::jlimit (20.0, sampleRate * 0.45, (double) hz);
        const double w0 = juce::MathConstants<double>::twoPi * f / sampleRate;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * 0.70710678118654752);
        const double a0 = 1.0 + alpha;
        b0 = (float) (((1.0 + cw) * 0.5) / a0);
        b1 = (float) (-(1.0 + cw) / a0);
        b2 = b0;
        a1 = (float) ((-2.0 * cw) / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }

    float runHpf (int ch, float x)
    {
        auto& s = hpf[(size_t) ch];
        const float y = b0 * x + s.z1;
        s.z1 = b1 * x - a1 * y + s.z2;
        s.z2 = b2 * x - a2 * y;
        if (std::abs (s.z1) < 1.0e-20f) s.z1 = 0.0f;
        if (std::abs (s.z2) < 1.0e-20f) s.z2 = 0.0f;
        return y;
    }

    double sampleRate = 48000.0;
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float lastHpf = -1.0f;
    Biquad hpf[2];
    float atkCoef = 0.0f, relCoef = 0.0f;
    float env = 0.0f;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gain;
};

} // namespace spa::mod
