#pragma once

#include <spa_fx/Filter.h>

namespace spa::params
{

// The FILTER module's filter modes: the shared spa-fx enum (same enumerators,
// same ORDER as SPASynth's params::FilterType; the order is load-bearing, it is
// the choice-parameter index stored in presets and automation).
using FilterType = spa::fx::FilterType;

} // namespace spa::params
