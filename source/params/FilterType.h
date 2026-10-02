#pragma once

namespace spa::params
{

// The FILTER module's filter modes. Same enumerators, same ORDER as SPASynth's
// params::FilterType (spasynth source/params/ParameterRegistry.h): the order is
// load-bearing, it is the choice-parameter index that is stored in presets and
// automation. Kept in its own tiny header so dsp/MultiModeFilter.h (a verbatim
// port of the synth's file) does not need the whole parameter registry.
enum class FilterType { lp12, lp24, hp12, hp24, bp12, bp24, notch12, notch24 };

} // namespace spa::params
