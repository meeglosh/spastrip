#pragma once

#include <spa_fx/Multiband.h>

namespace spa::dsp
{

// COMP: the DSP lives in the shared spa-fx module (spa::fx::Multiband, the
// canonical SPAGlitch / SPAStrip rebuild with knee, solo, bypass and meters).
// SPASynth's 1.0.32 port is frozen in tests/legacy/Multiband_1_0_32.h.
using Multiband = spa::fx::Multiband;

} // namespace spa::dsp
