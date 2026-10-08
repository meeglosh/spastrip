#pragma once

// The plate reverb engine now lives in the shared spa-fx module
// (libs/spa-fx, pinned by libs/spa-fx.pin; see scripts/fetch_spa_fx.sh).
// This shim keeps the product's `dsp::PlateReverb` name.
#include <spa_fx/PlateReverb.h>

namespace spa::dsp
{
using PlateReverb = spa::fx::PlateReverb;
}
