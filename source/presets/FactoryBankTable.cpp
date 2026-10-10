#include "FactoryBank.h"

// The factory table. Grammar and rules: FactoryBank.cpp. Bump kFactoryRecipeVersion on any change.
// Order inside a type: the six presets as the brief lists them. `out=` trims are calibrated
// with `SPAStripTests --render-factory-presets` (docs/factory-presets-1.0.6.md has the numbers).

namespace spa::preset
{

const std::vector<FactoryEntry>& factoryEntries()
{
    static const std::vector<FactoryEntry> entries = {

    // ======================= VOCALS =======================
    { "Vocal Clean Lead", "Vocals", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=90 eq.b1.slope=12_dB
eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=280 eq.b2.gain=-2 eq.b2.q=1.2
eq.b3.on=1 eq.b3.type=Bell eq.b3.freq=3500 eq.b3.gain=2 eq.b3.q=0.9
eq.b4.on=1 eq.b4.type=High_Shelf eq.b4.freq=11000 eq.b4.gain=2.5
comp.on=1 comp.mix=1 comp.lo.thresh=-26 comp.lo.ratio=2.5 comp.mid.thresh=-26 comp.mid.ratio=2.5 comp.hi.thresh=-28 comp.hi.ratio=2
rev.on=1 rev.mode=Plate rev.predelay=20 rev.size=0.5 rev.decay=1.2 rev.mix=0.12
lim.on=1 lim.ceiling=-1 lim.character=Clean

 lim.lookahead=1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=4)" },

    { "Vocal Radio Lead", "Vocals", R"(order=eq,dist
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=300 eq.b1.slope=24_dB
eq.b2.on=1 eq.b2.type=High_Cut eq.b2.freq=5200 eq.b2.slope=24_dB
eq.b3.on=1 eq.b3.type=Bell eq.b3.freq=1600 eq.b3.gain=4 eq.b3.q=0.8
dist.on=1 dist.type=Soft dist.drive=0.3 dist.tone=6000 dist.mix=0.5
comp.on=1 comp.mix=1 comp.lo.thresh=-24 comp.lo.ratio=3 comp.mid.thresh=-24 comp.mid.ratio=3 comp.hi.thresh=-24 comp.hi.ratio=3
rev.on=1 rev.mode=Room rev.size=0.3 rev.decay=0.4 rev.mix=0.1

lim.on=1 lim.ceiling=-1
 lim.lookahead=1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=5.5
in=2)" },

    { "Vocal Intimate Close", "Vocals", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB
eq.b2.on=1 eq.b2.type=Low_Shelf eq.b2.freq=200 eq.b2.gain=2.5
eq.b3.on=1 eq.b3.type=Bell eq.b3.freq=2800 eq.b3.gain=-1.5 eq.b3.q=1
eq.b4.on=1 eq.b4.type=High_Shelf eq.b4.freq=9000 eq.b4.gain=3
comp.on=1 comp.mix=1 comp.lo.thresh=-28 comp.lo.ratio=3.5 comp.mid.thresh=-28 comp.mid.ratio=3.5 comp.hi.thresh=-28 comp.hi.ratio=3
rev.on=1 rev.mode=Room rev.size=0.25 rev.decay=0.35 rev.damping=0.6 rev.mix=0.08

lim.on=1 lim.ceiling=-1
 lim.lookahead=1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=4.9)" },

    { "Vocal Plate Smooth", "Vocals", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB
eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=4000 eq.b2.gain=1.5 eq.b2.q=0.8
comp.on=1 comp.mix=1 comp.lo.thresh=-26 comp.lo.ratio=2.5 comp.mid.thresh=-26 comp.mid.ratio=2.5 comp.hi.thresh=-26 comp.hi.ratio=2.5
rev.on=1 rev.mode=Plate rev.predelay=35 rev.size=0.65 rev.decay=2.4 rev.damping=0.55 rev.lowcut=250 rev.highcut=9000 rev.mix=0.3
dly.on=1 dly.sync=1 dly.division=1/8 dly.feedback=0.25 dly.mix=0.12

lim.on=1 lim.ceiling=-1
 lim.lookahead=1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=4.8)" },

    { "Vocal Slap Wide", "Vocals", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB
comp.on=1 comp.mix=1 comp.lo.thresh=-26 comp.lo.ratio=2.5 comp.mid.thresh=-26 comp.mid.ratio=2.5 comp.hi.thresh=-26 comp.hi.ratio=2.5
dly.on=1 dly.sync=0 dly.time=95 dly.feedback=0.12 dly.pingpong=1 dly.width=100 dly.mix=0.28
cho.on=1 cho.mode=Modern cho.rate=0.4 cho.depth=0.25 cho.width=100 cho.mix=0.25
rev.on=1 rev.mode=Room rev.size=0.35 rev.decay=0.5 rev.mix=0.1

lim.on=1 lim.ceiling=-1
 lim.lookahead=1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=5.8)" },

    { "Vocal Telephone Lo-Fi", "Vocals", R"(order=eq,dist
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=400 eq.b1.slope=24_dB
eq.b2.on=1 eq.b2.type=High_Cut eq.b2.freq=3200 eq.b2.slope=36_dB
eq.b3.on=1 eq.b3.type=Bell eq.b3.freq=1100 eq.b3.gain=4 eq.b3.q=1.5
dist.on=1 dist.type=Crush dist.drive=0.3 dist.tone=4500 dist.mix=0.35
comp.on=1 comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=4 comp.mid.thresh=-22 comp.mid.ratio=4 comp.hi.thresh=-22 comp.hi.ratio=4

lim.on=1 lim.ceiling=-1
 lim.lookahead=1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=7.5
in=6)" },

    // ======================= DRUMS =======================
    { "Drums Punch Bus", "Drums", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Shelf eq.b1.freq=80 eq.b1.gain=2
eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=400 eq.b2.gain=-3 eq.b2.q=1
eq.b3.on=1 eq.b3.type=High_Shelf eq.b3.freq=6000 eq.b3.gain=2
comp.on=1 comp.mix=1 comp.lo.thresh=-20 comp.lo.ratio=3 comp.lo.attack=30 comp.mid.thresh=-20 comp.mid.ratio=3 comp.mid.attack=20 comp.hi.thresh=-22 comp.hi.ratio=2.5
dist.on=1 dist.type=Soft dist.drive=0.2 dist.mix=0.4
lim.on=1 lim.ceiling=-1

out=-0.1)" },

    { "Drums Room Smash", "Drums", R"(order=comp,dist
comp.on=1 comp.mix=1 comp.lo.thresh=-30 comp.lo.ratio=8 comp.mid.thresh=-30 comp.mid.ratio=8 comp.hi.thresh=-30 comp.hi.ratio=8 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
dist.on=1 dist.type=Soft dist.drive=0.35 dist.mix=0.5
rev.on=1 rev.mode=Room rev.size=0.4 rev.decay=0.6 rev.predelay=0 rev.mix=0.3
lim.on=1 lim.drive=3 lim.ceiling=-1
)" },

    { "Drums Parallel Crush", "Drums", R"(order=comp,dist
comp.on=1 comp.mix=1 comp.lo.thresh=-30 comp.lo.ratio=6 comp.lo.upratio=3 comp.mid.thresh=-30 comp.mid.ratio=6 comp.mid.upratio=3 comp.hi.thresh=-30 comp.hi.ratio=6 comp.hi.upratio=3
dist.on=1 dist.type=Hard dist.drive=0.5 dist.tone=8000 dist.mix=0.3
eq.on=1 eq.b1.on=1 eq.b1.type=High_Shelf eq.b1.freq=8000 eq.b1.gain=3
lim.on=1 lim.ceiling=-1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=2.7)" },

    { "Drums Tape Glue", "Drums", R"(order=dist,comp
dist.on=1 dist.type=Soft dist.drive=0.3 dist.tone=7000 dist.mix=0.8
vib.on=1 vib.rate=0.5 vib.depth=0.05 vib.mix=1
comp.on=1 comp.mix=1 comp.lo.thresh=-18 comp.lo.ratio=2 comp.mid.thresh=-18 comp.mid.ratio=2 comp.hi.thresh=-18 comp.hi.ratio=2
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=30 eq.b1.slope=12_dB eq.b2.on=1 eq.b2.type=High_Shelf eq.b2.freq=10000 eq.b2.gain=-2

lim.on=1
lim.ceiling=-1
out=-1.2)" },

    { "Drums Lo-Fi Breaks", "Drums", R"(order=eq,dist
dist.on=1 dist.type=Crush dist.drive=0.5 dist.tone=5000 dist.mix=0.5
flt.on=1 flt.type=LP_12 flt.cutoff=7500 flt.resonance=0.15
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=50 eq.b1.slope=12_dB
comp.on=1 comp.mix=1 comp.lo.thresh=-26 comp.lo.ratio=6 comp.mid.thresh=-26 comp.mid.ratio=6 comp.hi.thresh=-26 comp.hi.ratio=6
lim.on=1 lim.ceiling=-1
 lim.lookahead=1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=2.6)" },

    { "Drums Gated Space", "Drums", R"(comp.on=1 comp.mix=1 comp.lo.thresh=-24 comp.lo.ratio=4 comp.mid.thresh=-24 comp.mid.ratio=4 comp.hi.thresh=-24 comp.hi.ratio=4
rev.on=1 rev.mode=Chamber rev.size=0.6 rev.decay=0.7 rev.predelay=0 rev.damping=0.4 rev.lowcut=200 rev.highcut=9000 rev.mix=0.45
lim.on=1 lim.ceiling=-1

comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=3)" },

    // ======================= BASS =======================
    { "Bass DI Tighten", "Bass", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=30 eq.b1.slope=24_dB
eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=250 eq.b2.gain=-3 eq.b2.q=1
eq.b3.on=1 eq.b3.type=Bell eq.b3.freq=800 eq.b3.gain=2 eq.b3.q=1
comp.on=1 comp.mix=1 comp.xoverLow=150 comp.xoverHigh=1500 comp.lo.thresh=-24 comp.lo.ratio=3 comp.mid.thresh=-24 comp.mid.ratio=3 comp.hi.thresh=-24 comp.hi.ratio=3
lim.on=1 lim.ceiling=-1
 lim.lookahead=1

comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=2.1)" },

    { "Bass Amp Grit", "Bass", R"(order=eq,dist
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=40 eq.b1.slope=12_dB
eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=100 eq.b2.gain=2 eq.b2.q=1
eq.b3.on=1 eq.b3.type=High_Cut eq.b3.freq=6000 eq.b3.slope=12_dB
dist.on=1 dist.type=Hard dist.drive=0.4 dist.tone=3500 dist.mix=0.5
comp.on=1 comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=3 comp.mid.thresh=-22 comp.mid.ratio=3 comp.hi.thresh=-22 comp.hi.ratio=3

lim.drive=2
lim.on=1
lim.ceiling=-1)" },

    { "Bass Sub Focus", "Bass", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=25 eq.b1.slope=24_dB
eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=60 eq.b2.gain=3 eq.b2.q=1
eq.b3.on=1 eq.b3.type=Bell eq.b3.freq=300 eq.b3.gain=-4 eq.b3.q=1
eq.b4.on=1 eq.b4.type=High_Cut eq.b4.freq=3000 eq.b4.slope=24_dB
comp.on=1 comp.mix=1 comp.xoverLow=120 comp.lo.thresh=-22 comp.lo.ratio=4 comp.lo.attack=40 comp.mid.thresh=-24 comp.mid.ratio=3 comp.hi.thresh=-24 comp.hi.ratio=3
lim.on=1 lim.ceiling=-1

 lim.lookahead=1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=1.1)" },

    { "Bass Fuzz Drive", "Bass", R"(order=dist,flt
dist.on=1 dist.type=Fold dist.drive=0.55 dist.tone=2500 dist.mix=0.6
flt.on=1 flt.type=LP_24 flt.cutoff=1800 flt.resonance=0.15
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=35 eq.b1.slope=12_dB
lim.on=1 lim.ceiling=-1
out=-0.8)" },

    { "Bass Glue Comp", "Bass", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=30 eq.b1.slope=12_dB
comp.on=1 comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=3 comp.lo.knee=6 comp.mid.thresh=-22 comp.mid.ratio=3 comp.mid.knee=6 comp.hi.thresh=-22 comp.hi.ratio=3 comp.hi.knee=6
lim.on=1 lim.drive=3.6 lim.ceiling=-1
 lim.lookahead=1)" },

    { "Bass Chorus Thick", "Bass", R"(order=eq,cho
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=30 eq.b1.slope=12_dB
cho.on=1 cho.mode=Vintage cho.rate=0.5 cho.depth=0.25 cho.width=60 cho.mix=0.28
comp.on=1 comp.mix=1 comp.lo.thresh=-24 comp.lo.ratio=3 comp.mid.thresh=-24 comp.mid.ratio=3 comp.hi.thresh=-24 comp.hi.ratio=3

lim.on=1 lim.ceiling=-1 lim.lookahead=1

comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=3.4)" },

    // ======================= GUITAR =======================
    { "Guitar Clean Shimmer", "Guitar", R"(order=eq,cho
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=80 eq.b1.slope=12_dB
eq.b2.on=1 eq.b2.type=High_Shelf eq.b2.freq=9000 eq.b2.gain=2
cho.on=1 cho.mode=Modern cho.rate=0.5 cho.depth=0.3 cho.width=70 cho.mix=0.35
rev.on=1 rev.mode=Plate rev.predelay=25 rev.size=0.6 rev.decay=2.2 rev.mix=0.22
dly.on=1 dly.sync=1 dly.division=1/8. dly.feedback=0.2 dly.mix=0.1

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=5.7)" },

    { "Guitar Crunch Edge", "Guitar", R"(order=dist,eq
dist.on=1 dist.type=Hard dist.drive=0.45 dist.tone=5500 dist.mix=0.6
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=90 eq.b1.slope=12_dB
eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=800 eq.b2.gain=2 eq.b2.q=1
eq.b3.on=1 eq.b3.type=Bell eq.b3.freq=3000 eq.b3.gain=2 eq.b3.q=1
rev.on=1 rev.mode=Room rev.size=0.3 rev.decay=0.5 rev.mix=0.08
out=-2.1)" },

    { "Guitar Ambient Swell", "Guitar", R"(order=eq,dly
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB
dly.on=1 dly.sync=1 dly.division=1/4 dly.feedback=0.5 dly.pingpong=1 dly.width=100 dly.mix=0.3
rev.on=1 rev.mode=Hall rev.predelay=40 rev.size=0.8 rev.decay=4 rev.mix=0.3
cho.on=1 cho.mode=Modern cho.rate=0.25 cho.depth=0.3 cho.width=100 cho.mix=0.2

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=3.5)" },

    { "Guitar Slapback Twang", "Guitar", R"(order=eq,dly
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=90 eq.b1.slope=12_dB
eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=3500 eq.b2.gain=2 eq.b2.q=1
dly.on=1 dly.sync=0 dly.time=110 dly.feedback=0.15 dly.pingpong=0 dly.mix=0.3
rev.on=1 rev.mode=Spring rev.size=0.4 rev.decay=1 rev.mix=0.1

lim.drive=1.2
lim.on=1
lim.ceiling=-1)" },

    { "Guitar Chorus Twin", "Guitar", R"(order=eq,cho
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=80 eq.b1.slope=12_dB
cho.on=1 cho.mode=Vintage cho.rate=0.6 cho.depth=0.45 cho.width=100 cho.mix=0.5
rev.on=1 rev.mode=Spring rev.size=0.45 rev.decay=1.4 rev.mix=0.15

lim.drive=5
lim.on=1
lim.ceiling=-1)" },

    { "Guitar Surf Tremolo", "Guitar", R"(order=eq,trem
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB
eq.b2.on=1 eq.b2.type=High_Shelf eq.b2.freq=6000 eq.b2.gain=2
trem.on=1 trem.rate=5.5 trem.depth=0.65 trem.shape=Sine trem.stereo=0.3 trem.mix=1
rev.on=1 rev.mode=Spring rev.size=0.6 rev.decay=1.8 rev.damping=0.4 rev.mix=0.28

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=6.2)" },

    // ======================= KEYS =======================
    { "Keys Rhodes Chorus", "Keys", R"(order=cho,trem
cho.on=1 cho.mode=Vintage cho.rate=0.7 cho.depth=0.4 cho.width=100 cho.mix=0.5
trem.on=1 trem.rate=4.5 trem.depth=0.25 trem.shape=Sine trem.stereo=1 trem.mix=1
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=60 eq.b1.slope=12_dB eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=300 eq.b2.gain=1.5
rev.on=1 rev.mode=Room rev.size=0.4 rev.decay=0.8 rev.mix=0.1

lim.drive=4.5
lim.on=1
lim.ceiling=-1)" },

    { "Keys Piano Air", "Keys", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=High_Shelf eq.b1.freq=12000 eq.b1.gain=3
eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=300 eq.b2.gain=-2 eq.b2.q=1
comp.on=1 comp.mix=1 comp.lo.thresh=-24 comp.lo.ratio=2 comp.mid.thresh=-24 comp.mid.ratio=2 comp.hi.thresh=-24 comp.hi.ratio=2
rev.on=1 rev.mode=Hall rev.predelay=25 rev.size=0.65 rev.decay=2.4 rev.mix=0.2


lim.on=1
lim.ceiling=-1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=2.4)" },

    { "Keys Lo-Fi Tape", "Keys", R"(order=dist,vib
dist.on=1 dist.type=Soft dist.drive=0.25 dist.tone=6000 dist.mix=0.6
vib.on=1 vib.rate=0.4 vib.depth=0.15 vib.mix=1
cho.on=1 cho.mode=VHS cho.mix=0.4
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=60 eq.b1.slope=12_dB eq.b2.on=1 eq.b2.type=High_Cut eq.b2.freq=7500 eq.b2.slope=12_dB
lim.drive=0.5
lim.on=1
lim.ceiling=-1)" },

    { "Keys Wide Stereo", "Keys", R"(order=cho,dly
cho.on=1 cho.mode=Modern cho.rate=0.35 cho.depth=0.3 cho.width=100 cho.mix=0.35
dly.on=1 dly.sync=1 dly.division=1/8 dly.feedback=0.3 dly.pingpong=1 dly.width=100 dly.mix=0.12
rev.on=1 rev.mode=Plate rev.size=0.5 rev.decay=1.6 rev.mix=0.15
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=60 eq.b1.slope=12_dB

lim.drive=4.7
lim.on=1
lim.ceiling=-1)" },

    { "Keys Organ Swirl", "Keys", R"(order=dist,phs
dist.on=1 dist.type=Soft dist.drive=0.2 dist.tone=7000 dist.mix=0.5
phs.on=1 phs.type=Phaser phs.rate=1.2 phs.depth=0.6 phs.feedback=0.3 phs.stages=4 phs.mix=0.4
trem.on=1 trem.rate=6 trem.depth=0.2 trem.shape=Sine trem.stereo=0.5 trem.mix=1
rev.on=1 rev.mode=Spring rev.size=0.4 rev.decay=1 rev.mix=0.1

lim.drive=2.7
lim.on=1
lim.ceiling=-1)" },

    { "Keys Tape Warm", "Keys", R"(order=dist,eq
dist.on=1 dist.type=Soft dist.drive=0.2 dist.tone=8000 dist.mix=0.6
vib.on=1 vib.rate=0.3 vib.depth=0.06 vib.mix=1
cho.on=1 cho.mode=Vintage cho.rate=0.3 cho.depth=0.15 cho.width=60 cho.mix=0.18
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Shelf eq.b1.freq=150 eq.b1.gain=1.5 eq.b2.on=1 eq.b2.type=High_Shelf eq.b2.freq=9000 eq.b2.gain=-2
comp.on=1 comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=2 comp.mid.thresh=-22 comp.mid.ratio=2 comp.hi.thresh=-22 comp.hi.ratio=2

lim.drive=2.1
lim.on=1
lim.ceiling=-1)" },

    // ======================= SYNTH =======================
    { "Synth Pad Widener", "Synth", R"(order=cho,eq
cho.on=1 cho.mode=Modern cho.rate=0.3 cho.depth=0.35 cho.width=100 cho.mix=0.4
rev.on=1 rev.mode=Hall rev.predelay=30 rev.size=0.75 rev.decay=3.2 rev.mix=0.2
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=80 eq.b1.slope=12_dB

lim.drive=5.5
lim.on=1
lim.ceiling=-1 lim.lookahead=1)" },

    { "Synth Lead Presence", "Synth", R"(order=eq,dist
eq.on=1 eq.b1.on=1 eq.b1.type=Bell eq.b1.freq=2500 eq.b1.gain=3 eq.b1.q=0.9
eq.b2.on=1 eq.b2.type=High_Shelf eq.b2.freq=9000 eq.b2.gain=1.5
dist.on=1 dist.type=Soft dist.drive=0.15 dist.mix=0.5
comp.on=1 comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=2.5 comp.mid.thresh=-22 comp.mid.ratio=2.5 comp.hi.thresh=-22 comp.hi.ratio=2.5
dly.on=1 dly.sync=1 dly.division=1/8 dly.feedback=0.3 dly.mix=0.15

lim.drive=1.9
lim.on=1
lim.ceiling=-1)" },

    { "Synth Arp Delay", "Synth", R"(order=eq,dly
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB
dly.on=1 dly.sync=1 dly.division=1/8. dly.feedback=0.45 dly.pingpong=1 dly.width=100 dly.mix=0.3
rev.on=1 rev.mode=Plate rev.size=0.5 rev.decay=1.4 rev.mix=0.1

lim.drive=1.8
lim.on=1
lim.ceiling=-1 lim.lookahead=1)" },

    { "Synth Pluck Space", "Synth", R"(order=eq,dly
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB
rev.on=1 rev.mode=Chamber rev.predelay=10 rev.size=0.5 rev.decay=1.2 rev.mix=0.22
dly.on=1 dly.sync=1 dly.division=1/16 dly.feedback=0.25 dly.mix=0.12

lim.drive=3
lim.on=1
lim.ceiling=-1)" },

    { "Synth Tape Warp", "Synth", R"(order=vib,dist
vib.on=1 vib.rate=0.3 vib.depth=0.25 vib.mix=1
cho.on=1 cho.mode=VHS cho.vhsWow=55 cho.vhsFlutter=20 cho.vhsTone=40 cho.vhsSat=30 cho.vhsHiss=5 cho.vhsDropouts=0 cho.mix=0.5
dist.on=1 dist.type=Soft dist.drive=0.2 dist.mix=0.5
eq.on=1 eq.b1.on=1 eq.b1.type=High_Cut eq.b1.freq=9000 eq.b1.slope=12_dB)" },

    { "Synth Stereo Glue", "Synth", R"(order=comp,cho
comp.on=1 comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=2.5 comp.lo.knee=6 comp.mid.thresh=-22 comp.mid.ratio=2.5 comp.mid.knee=6 comp.hi.thresh=-22 comp.hi.ratio=2.5 comp.hi.knee=6
cho.on=1 cho.mode=Modern cho.rate=0.4 cho.depth=0.2 cho.width=80 cho.mix=0.2
eq.on=1 eq.b1.on=1 eq.b1.type=Tilt_Shelf eq.b1.freq=1000 eq.b1.gain=1
lim.on=1 lim.ceiling=-1
 lim.lookahead=1
comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=2.8)" },

    // ======================= FX (sound design) =======================
    { "FX Radio Sweep", "FX", R"(order=flt,eq
sc.source=Input sc.attack=5 sc.release=350 sc.gain=0
flt.on=1 flt.type=LP_12 flt.cutoff=1400 flt.resonance=0.45 flt.mix=1
m1=flt.cutoff:0.4
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=200 eq.b1.slope=12_dB
dist.on=1 dist.type=Soft dist.drive=0.3 dist.mix=0.4
lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=3.2)" },

    { "FX Robot Voice", "FX", R"(order=phs,dist
phs.on=1 phs.type=Flanger phs.rate=0.05 phs.depth=0.1 phs.feedback=0.75 phs.manual=4 phs.mix=0.6
dist.on=1 dist.type=Crush dist.drive=0.3 dist.tone=6000 dist.mix=0.4
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=150 eq.b1.slope=12_dB

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=2.6)" },

    { "FX Underwater", "FX", R"(order=flt,cho
flt.on=1 flt.type=LP_24 flt.cutoff=700 flt.resonance=0.3
cho.on=1 cho.mode=Modern cho.rate=0.8 cho.depth=0.7 cho.width=100 cho.mix=0.6
vib.on=1 vib.rate=1.2 vib.depth=0.3 vib.mix=1
rev.on=1 rev.mode=Hall rev.size=0.7 rev.decay=3 rev.damping=0.8 rev.mix=0.3

lim.on=1 lim.ceiling=-1
lim.drive=4 lim.lookahead=1
in=3)" },

    { "FX Megaphone", "FX", R"(order=eq,dist
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=600 eq.b1.slope=36_dB
eq.b2.on=1 eq.b2.type=High_Cut eq.b2.freq=3500 eq.b2.slope=24_dB
eq.b3.on=1 eq.b3.type=Bell eq.b3.freq=1800 eq.b3.gain=3 eq.b3.q=1.5
dist.on=1 dist.type=Hard dist.drive=0.5 dist.tone=4500 dist.mix=0.7

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=5.4
in=6)" },

    { "FX Alien Whistle", "FX", R"(order=glt,phs
glt.on=1 glt.size=60 glt.density=40 glt.pitch=7 glt.spread=0.3 glt.spreadPitch=6 glt.position=150 glt.feedback=0.2 glt.release=0.5 glt.mix=0.4
phs.on=1 phs.type=Phaser phs.rate=0.3 phs.depth=0.7 phs.feedback=0.6 phs.stages=8 phs.mix=0.35
rev.on=1 rev.mode=Plate rev.size=0.6 rev.decay=2 rev.mix=0.2

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=5.6
in=2)" },

    { "FX Glitch Stutter", "FX", R"(order=glt,dly
glt.on=1 glt.sync=1 glt.division=1/16 glt.size=40 glt.density=30 glt.spread=0.3 glt.position=100 glt.reverse=0.3 glt.release=0.3 glt.mix=0.5
dly.on=1 dly.sync=1 dly.division=1/16 dly.feedback=0.3 dly.mix=0.15

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=5.4)" },

    // ======================= MIXBUS =======================
    { "Mixbus Glue", "Mixbus", R"(comp.on=1 comp.mix=1 comp.lo.thresh=-20 comp.lo.ratio=2 comp.lo.knee=6 comp.mid.thresh=-20 comp.mid.ratio=2 comp.mid.knee=6 comp.hi.thresh=-20 comp.hi.ratio=2 comp.hi.knee=6
lim.on=1 lim.drive=1.9 lim.ceiling=-1
)" },

    { "Mixbus Punch", "Mixbus", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=High_Shelf eq.b1.freq=9000 eq.b1.gain=1.5
comp.on=1 comp.mix=1 comp.lo.thresh=-18 comp.lo.ratio=3 comp.lo.attack=35 comp.mid.thresh=-18 comp.mid.ratio=3 comp.mid.attack=22 comp.hi.thresh=-20 comp.hi.ratio=2.5 comp.hi.attack=8
dist.on=1 dist.type=Soft dist.drive=0.1 dist.mix=0.4
lim.on=1 lim.ceiling=-1)" },

    { "Mixbus Warm Tape", "Mixbus", R"(order=dist,eq
dist.on=1 dist.type=Soft dist.drive=0.2 dist.tone=9000 dist.mix=0.7
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Shelf eq.b1.freq=100 eq.b1.gain=1 eq.b2.on=1 eq.b2.type=High_Shelf eq.b2.freq=10000 eq.b2.gain=-1.5
vib.on=1 vib.rate=0.3 vib.depth=0.04 vib.mix=1
comp.on=1 comp.mix=1 comp.lo.thresh=-18 comp.lo.ratio=1.8 comp.mid.thresh=-18 comp.mid.ratio=1.8 comp.hi.thresh=-18 comp.hi.ratio=1.8


lim.on=1
lim.ceiling=-1
out=-1)" },

    { "Mixbus Wide Polish", "Mixbus", R"(order=eq,cho
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=25 eq.b1.slope=12_dB eq.b2.on=1 eq.b2.type=High_Shelf eq.b2.freq=12000 eq.b2.gain=2
cho.on=1 cho.mode=Modern cho.rate=0.3 cho.depth=0.15 cho.width=100 cho.mix=0.12
rev.on=1 rev.mode=Plate rev.size=0.5 rev.decay=1.2 rev.mix=0.05
lim.on=1 lim.ceiling=-1

lim.drive=1.7)" },

    { "Mixbus Parallel Comp", "Mixbus", R"(comp.on=1 comp.mix=1 comp.lo.thresh=-24 comp.lo.ratio=3 comp.lo.upratio=2 comp.mid.thresh=-24 comp.mid.ratio=3 comp.mid.upratio=2 comp.hi.thresh=-24 comp.hi.ratio=3 comp.hi.upratio=2
lim.on=1 lim.ceiling=-1

comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3
lim.drive=1.2)" },

    { "Mixbus Gentle Polish", "Mixbus", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=Tilt_Shelf eq.b1.freq=1000 eq.b1.gain=0.8
comp.on=1 comp.mix=1 comp.lo.thresh=-16 comp.lo.ratio=1.5 comp.mid.thresh=-16 comp.mid.ratio=1.5 comp.hi.thresh=-16 comp.hi.ratio=1.5 comp.lo.knee=8 comp.mid.knee=8 comp.hi.knee=8
lim.on=1 lim.ceiling=-1

lim.drive=0.8)" },

    // ======================= MASTERING (may add level; never past the ceiling) =======================
    { "Master Transparent Loud", "Mastering", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=25 eq.b1.slope=12_dB
comp.on=1 comp.mix=1 comp.lo.thresh=-14 comp.lo.ratio=2 comp.lo.knee=8 comp.mid.thresh=-14 comp.mid.ratio=2 comp.mid.knee=8 comp.hi.thresh=-14 comp.hi.ratio=2 comp.hi.knee=8
lim.on=1 lim.drive=5 lim.ceiling=-0.3 lim.character=Clean lim.truePeak=1 lim.lookahead=1)" },

    { "Master Warm Glue", "Mastering", R"(order=dist,eq
dist.on=1 dist.type=Soft dist.drive=0.12 dist.tone=9000 dist.mix=0.5
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Shelf eq.b1.freq=100 eq.b1.gain=1 eq.b2.on=1 eq.b2.type=High_Shelf eq.b2.freq=10000 eq.b2.gain=-1
comp.on=1 comp.mix=1 comp.lo.thresh=-14 comp.lo.ratio=1.8 comp.mid.thresh=-14 comp.mid.ratio=1.8 comp.hi.thresh=-14 comp.hi.ratio=1.8
lim.on=1 lim.drive=3 lim.ceiling=-0.5 lim.truePeak=1 lim.lookahead=1)" },

    { "Master Bright Air", "Mastering", R"(order=eq,comp
eq.on=1 eq.b1.on=1 eq.b1.type=High_Shelf eq.b1.freq=10000 eq.b1.gain=2 eq.b2.on=1 eq.b2.type=Bell eq.b2.freq=3000 eq.b2.gain=1 eq.b2.q=0.8
comp.on=1 comp.mix=1 comp.lo.thresh=-14 comp.lo.ratio=2 comp.mid.thresh=-14 comp.mid.ratio=2 comp.hi.thresh=-14 comp.hi.ratio=2
lim.on=1 lim.drive=3 lim.ceiling=-0.5 lim.truePeak=1 lim.lookahead=1)" },

    { "Master Safe Limiter", "Mastering", R"(lim.on=1 lim.drive=0 lim.ceiling=-1 lim.character=Clean lim.truePeak=1 lim.lookahead=1)" },

    { "Master Streaming Level", "Mastering", R"(order=comp
comp.on=1 comp.mix=1 comp.lo.thresh=-16 comp.lo.ratio=1.8 comp.mid.thresh=-16 comp.mid.ratio=1.8 comp.hi.thresh=-16 comp.hi.ratio=1.8
lim.on=1 lim.drive=2.5 lim.ceiling=-1 lim.character=Clean lim.truePeak=1 lim.lookahead=1)" },

    { "Master Dynamic Open", "Mastering", R"(order=comp
comp.on=1 comp.mix=1 comp.lo.thresh=-12 comp.lo.ratio=1.5 comp.mid.thresh=-12 comp.mid.ratio=1.5 comp.hi.thresh=-12 comp.hi.ratio=1.5 comp.lo.knee=10 comp.mid.knee=10 comp.hi.knee=10
lim.on=1 lim.drive=1 lim.ceiling=-0.5 lim.truePeak=1 lim.lookahead=1)" },

    // ======================= CREATIVE =======================
    { "Creative Pump Duck", "Creative", R"(order=trem,rev
sc.source=Input sc.attack=2 sc.release=250
trem.on=1 trem.sync=1 trem.division=1/4 trem.depth=0.55 trem.shape=Saw trem.mix=1
rev.on=1 rev.mode=Hall rev.size=0.7 rev.decay=3 rev.mix=0.4
m1=rev.mix:-0.3

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=4.4)" },

    { "Creative Filter Sweep", "Creative", R"(order=flt,phs
flt.on=1 flt.type=LP_12 flt.cutoff=1200 flt.resonance=0.5
sc.source=Input sc.attack=8 sc.release=500
m1=flt.cutoff:0.5
phs.on=1 phs.type=Phaser phs.sync=1 phs.division=1/1 phs.depth=0.8 phs.feedback=0.4 phs.stages=6 phs.mix=0.4

lim.drive=2.7
lim.on=1
lim.ceiling=-1)" },

    { "Creative Rhythmic Tremolo", "Creative", R"(trem.on=1 trem.sync=1 trem.division=1/8 trem.depth=0.8 trem.shape=Square trem.stereo=0.5 trem.mix=1
rev.on=1 rev.mode=Plate rev.size=0.5 rev.decay=1.6 rev.mix=0.15

lim.drive=3.2
lim.on=1
lim.ceiling=-1)" },

    { "Creative Ping-Pong Bounce", "Creative", R"(dly.on=1 dly.sync=1 dly.division=1/8 dly.feedback=0.5 dly.pingpong=1 dly.width=100 dly.mix=0.35
rev.on=1 rev.mode=Plate rev.size=0.5 rev.decay=1.4 rev.mix=0.1)" },

    { "Creative Glitter Freeze", "Creative", R"(order=glt,rev
glt.on=1 glt.size=220 glt.density=18 glt.pitch=0 glt.spread=0.4 glt.spreadPitch=2 glt.position=500 glt.feedback=0.3 glt.release=3 glt.mix=0.45
rev.on=1 rev.mode=Hall rev.size=0.7 rev.decay=3 rev.mix=0.25

lim.drive=3.8
lim.on=1
lim.ceiling=-1)" },

    { "Creative Wobble Warp", "Creative", R"(order=vib,flt
vib.on=1 vib.rate=2.5 vib.depth=0.45 vib.mix=1
trem.on=1 trem.sync=1 trem.division=1/8T trem.depth=0.4 trem.shape=Triangle trem.mix=1
flt.on=1 flt.type=LP_12 flt.cutoff=3000 flt.resonance=0.3

lim.drive=2.1
lim.on=1
lim.ceiling=-1)" },

    // ======================= AMBIENT =======================
    { "Ambient Huge Hall", "Ambient", R"(order=eq,rev
rev.on=1 rev.mode=Hall rev.predelay=40 rev.size=1 rev.decay=6.5 rev.damping=0.55 rev.moddepth=0.4 rev.lowcut=150 rev.highcut=9000 rev.mix=0.5

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=1)" },

    { "Ambient Glitter Cloud", "Ambient", R"(order=glt,rev
glt.on=1 glt.size=250 glt.density=30 glt.pitch=0 glt.spread=0.5 glt.spreadPitch=4 glt.position=800 glt.feedback=0.3 glt.release=2.5 glt.mix=0.5
rev.on=1 rev.mode=Hall rev.predelay=30 rev.size=0.8 rev.decay=4 rev.mix=0.3

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=4.4)" },

    { "Ambient Dub Delay Space", "Ambient", R"(order=dly,flt,rev
dly.on=1 dly.sync=1 dly.division=1/4. dly.feedback=0.6 dly.pingpong=1 dly.width=100 dly.mix=0.35
flt.on=1 flt.type=LP_12 flt.cutoff=4500 flt.resonance=0.1
rev.on=1 rev.mode=Chamber rev.size=0.6 rev.decay=2.5 rev.mix=0.2
lim.drive=0.3
lim.on=1
lim.ceiling=-1)" },

    { "Ambient Endless Wash", "Ambient", R"(order=glt,cho,rev
glt.on=1 glt.size=300 glt.density=20 glt.spread=0.5 glt.position=1000 glt.feedback=0.35 glt.release=5 glt.mix=0.3
cho.on=1 cho.mode=Modern cho.rate=0.2 cho.depth=0.4 cho.width=100 cho.mix=0.25
rev.on=1 rev.mode=Hall rev.predelay=50 rev.size=1 rev.decay=8 rev.damping=0.5 rev.moddepth=0.5 rev.mix=0.55

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=4)" },

    { "Ambient Shimmer Rise", "Ambient", R"(order=glt,rev
glt.on=1 glt.size=180 glt.density=25 glt.pitch=12 glt.spread=0.3 glt.spreadPitch=1 glt.position=600 glt.feedback=0.45 glt.release=3 glt.mix=0.4
rev.on=1 rev.mode=Hall rev.predelay=35 rev.size=0.85 rev.decay=5 rev.mix=0.35

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=3.2)" },

    { "Ambient Distant Memory", "Ambient", R"(order=flt,vib,rev
flt.on=1 flt.type=LP_24 flt.cutoff=2500 flt.resonance=0.1
vib.on=1 vib.rate=0.3 vib.depth=0.15 vib.mix=1
rev.on=1 rev.mode=Hall rev.predelay=80 rev.size=0.9 rev.decay=5 rev.damping=0.7 rev.mix=0.5
out=-0.8)" },

    // ======================= LO-FI =======================
    { "Lo-Fi Cassette Tape", "Lo-Fi", R"(order=dist,vib
dist.on=1 dist.type=Soft dist.drive=0.25 dist.tone=6000 dist.mix=0.6
vib.on=1 vib.rate=0.35 vib.depth=0.1 vib.mix=1
cho.on=1 cho.mode=Vintage cho.rate=0.3 cho.depth=0.1 cho.width=40 cho.mix=0.25
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=50 eq.b1.slope=12_dB eq.b2.on=1 eq.b2.type=High_Cut eq.b2.freq=8000 eq.b2.slope=12_dB
lim.drive=0.3
lim.on=1
lim.ceiling=-1)" },

    { "Lo-Fi Vinyl Dust", "Lo-Fi", R"(order=eq,cho
cho.on=1 cho.mode=VHS cho.vhsWow=15 cho.vhsFlutter=10 cho.vhsTone=55 cho.vhsSat=20 cho.vhsHiss=20 cho.vhsDropouts=15 cho.mix=0.6
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=60 eq.b1.slope=12_dB eq.b2.on=1 eq.b2.type=High_Cut eq.b2.freq=9000 eq.b2.slope=12_dB
dist.on=1 dist.type=Soft dist.drive=0.15 dist.mix=0.5

lim.drive=0.9
lim.on=1
lim.ceiling=-1)" },

    { "Lo-Fi Bitcrushed", "Lo-Fi", R"(order=dist,flt
dist.on=1 dist.type=Crush dist.drive=0.6 dist.tone=6000 dist.mix=0.7
flt.on=1 flt.type=LP_12 flt.cutoff=9000 flt.resonance=0.1
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=40 eq.b1.slope=12_dB

lim.drive=0.5
lim.on=1
lim.ceiling=-1)" },

    { "Lo-Fi Broken Speaker", "Lo-Fi", R"(order=eq,dist
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=500 eq.b1.slope=36_dB
eq.b2.on=1 eq.b2.type=High_Cut eq.b2.freq=4000 eq.b2.slope=24_dB
eq.b3.on=1 eq.b3.type=Bell eq.b3.freq=1200 eq.b3.gain=3 eq.b3.q=2
dist.on=1 dist.type=Hard dist.drive=0.55 dist.tone=4000 dist.mix=0.8

lim.drive=4.7
lim.on=1
lim.ceiling=-1
in=4)" },

    { "Lo-Fi VHS Chorus", "Lo-Fi", R"(cho.on=1 cho.mode=VHS cho.vhsWow=45 cho.vhsFlutter=30 cho.vhsTone=40 cho.vhsSat=35 cho.vhsHiss=18 cho.vhsDropouts=10 cho.width=60 cho.mix=0.8
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=50 eq.b1.slope=12_dB

lim.drive=1.2
lim.on=1
lim.ceiling=-1)" },

    { "Lo-Fi AM Radio", "Lo-Fi", R"(order=eq,dist
eq.on=1 eq.b1.on=1 eq.b1.type=Low_Cut eq.b1.freq=250 eq.b1.slope=24_dB
eq.b2.on=1 eq.b2.type=High_Cut eq.b2.freq=3500 eq.b2.slope=36_dB
eq.b3.on=1 eq.b3.type=Bell eq.b3.freq=1000 eq.b3.gain=3 eq.b3.q=1
dist.on=1 dist.type=Soft dist.drive=0.35 dist.tone=5000 dist.mix=0.6
cho.on=1 cho.mode=VHS cho.vhsWow=5 cho.vhsFlutter=5 cho.vhsTone=50 cho.vhsSat=0 cho.vhsHiss=25 cho.vhsDropouts=0 cho.mix=0.3

lim.on=1 lim.ceiling=-1 lim.lookahead=1
lim.drive=4.9
in=2)" },

    };
    return entries;
}

} // namespace spa::preset
