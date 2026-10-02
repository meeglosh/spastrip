#pragma once

#include <algorithm>
#include <cmath>
#include <complex>

#include "../params/FilterType.h"

namespace spa::dsp::filterresponse
{

// SPAStrip: the EXACT frequency response of the FILTER module, computed from
// the transfer function of MultiModeFilter's TPT state-variable filter rather
// than from an analogue approximation. Used by the FILTER tab's display and by
// the tests, which check the real DSP against it.
//
// One stage, with g = tan(pi fc / fs), k = 2 - 1.9 res, and the bilinear
// s = j tan(pi f / fs) / g (the TPT structure realises the bilinear transform of
// the analogue prototype exactly):
//     LP = 1 / (s^2 + k s + 1)        BP = s / (s^2 + k s + 1)    (NOT peak-normalised)
//     HP = s^2 / (s^2 + k s + 1)      Notch = LP + HP = (1 + s^2) / (s^2 + k s + 1)
// The 24 dB modes cascade two identical stages (the square). MIX blends the
// filtered signal with the dry one: 1 + (H - 1) mix. DRIVE is a memoryless
// nonlinearity in front of the filter and is not part of a linear response.
//
// Module: Series multiplies the two blended responses; Parallel averages them
// (0.5 x sum), a disabled filter 1 being a plain wire (1) in either. Filter 2 off
// leaves filter 1 alone whatever the routing. This is the synth's routing.

struct Filter
{
    bool enabled = false;
    params::FilterType type = params::FilterType::lp12;
    double cutoffHz = 20000.0;
    double resonance = 0.0;     // 0..1
    double mix = 1.0;           // 0..1
};

inline bool is24dB (params::FilterType t)
{
    return t == params::FilterType::lp24 || t == params::FilterType::hp24
        || t == params::FilterType::bp24 || t == params::FilterType::notch24;
}

// One filter's own response (no mix).
inline std::complex<double> response (params::FilterType type, double cutoffHz, double resonance,
                                      double sampleRate, double hz)
{
    constexpr double pi = 3.14159265358979323846;
    // The same clamp as MultiModeFilter::setParams.
    const double fc = std::clamp (cutoffHz, 20.0, sampleRate * 0.45);
    const double g = std::tan (pi * fc / sampleRate);
    const double k = 2.0 - 1.9 * std::clamp (resonance, 0.0, 1.0);

    const double f = std::min (hz, sampleRate * 0.5 - 1.0e-3);
    const std::complex<double> s (0.0, std::tan (pi * f / sampleRate) / g);
    const std::complex<double> den = s * s + k * s + 1.0;

    std::complex<double> h;
    switch (type)
    {
        case params::FilterType::lp12: case params::FilterType::lp24:       h = 1.0 / den; break;
        case params::FilterType::hp12: case params::FilterType::hp24:       h = (s * s) / den; break;
        case params::FilterType::bp12: case params::FilterType::bp24:       h = s / den; break;
        case params::FilterType::notch12: case params::FilterType::notch24: h = (1.0 + s * s) / den; break;
    }
    return is24dB (type) ? h * h : h;
}

// One filter as the module uses it: blended with the dry by MIX; 1 when off.
inline std::complex<double> blended (const Filter& f, double sampleRate, double hz)
{
    if (! f.enabled)
        return 1.0;
    const double m = std::clamp (f.mix, 0.0, 1.0);
    return 1.0 + (response (f.type, f.cutoffHz, f.resonance, sampleRate, hz) - 1.0) * m;
}

inline std::complex<double> module (const Filter& f1, const Filter& f2, bool parallel,
                                    double sampleRate, double hz)
{
    const auto a = blended (f1, sampleRate, hz);
    if (! f2.enabled)           // the synth only looks at routing when filter 2 is on
        return a;
    const auto b = blended (f2, sampleRate, hz);
    return parallel ? 0.5 * (a + b) : a * b;
}

inline double toDb (std::complex<double> h) { return 20.0 * std::log10 (std::max (std::abs (h), 1.0e-9)); }

} // namespace spa::dsp::filterresponse
