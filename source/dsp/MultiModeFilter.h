#pragma once

// The FILTER module's state-variable filter lives in the shared spa-fx module
// (libs/spa-fx, pinned by libs/spa-fx.pin; see scripts/fetch_spa_fx.sh). This
// shim keeps the product's `dsp::MultiModeFilter` name. SPAStrip's 1.0.3 copy is
// frozen in tests/legacy/MultiModeFilter_103.h.
#include <spa_fx/Filter.h>

namespace spa::dsp
{
using MultiModeFilter = spa::fx::MultiModeFilter;
}
