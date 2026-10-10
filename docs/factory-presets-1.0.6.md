# SPAStrip 1.0.6 factory presets

72 presets, 12 types x 6, in the browser under Factory. Recipes are in `source/presets/FactoryBankTable.cpp`; the grammar is at the top of `source/presets/FactoryBank.cpp`. Every preset starts from the plug-in defaults (only the modules named below are on), never uses CONV, and never holds GLITTER forever.

## How the numbers were measured

`SPAStripTests --render-factory-presets` loads each preset through the browser path and renders four synthetic sources (48 kHz, 120 bpm, all peaking at -6 dBFS): **drums** (kick / snare / hats loop), **vocal** (formant-filtered glottal source with breaths), **bass** (55 Hz saw + sine notes), **pad** (four detuned saws, stereo, slow swell). Level change is the RMS of the first 4 s of output minus the RMS of the source over the same 4 s, so a reverb or delay tail that rings into the window counts as added level. Then 20 s of silence is fed in and the last second is measured (tail). Peak is the highest sample over the whole render.

Pass rules: non-Mastering presets average within +-3 dB of the input over the four sources and within +-3 dB on their own type's source (Drums on drums, Vocals on vocal, Bass on bass, Guitar / Keys / Synth / Ambient on pad); Mastering may add level but its sample peak stays at or under its limiter ceiling. All renders: no NaN or inf, no sample past 0 dBFS, not silent, tail under -60 dBFS after 20 s.

## Things to know before auditioning

- **Level trims.** Make-up gain is the LIMIT module's DRIVE (ceiling -1 dB, lookahead on where the peaks needed it) or, when a preset is too loud, OUTPUT gain; INPUT gain is used a few times for band-limited sounds. Every non-Mastering preset ends in a limiter or has at least 5 dB of headroom on a -6 dBFS source, so none reaches 0 dBFS.
- **COMP MIX is always 100 %.** Below 100 % the compressor's dry path is not phase-aligned with its crossover-split wet path, so the blend combs (a vocal at 200 Hz measured -11 dB with the ratio at 1:1 and MIX at 55 %). Parallel-style presets use COMP's UP RATIO instead. This is in the shared spa-fx Multiband, not in this bank; it is worth a look separately.
- **Band-limited sounds** (Telephone, Radio, Megaphone, Broken Speaker, AM Radio, Radio Sweep) are trimmed to the mean of the four sources, so a bass-heavy source reads a few dB quieter and a mid-heavy one a few dB louder than the average. The tables show it.
- **Tempo-synced settings** follow the host; the numbers below were measured at 120 bpm.
- **Mastering** presets add level on purpose (LIMIT drive) and never pass their ceiling; they use true-peak and lookahead, which reports latency to the host.

Level change in dB (positive = louder than the input); the source a type is judged on is marked with an asterisk.

## Vocals

### Vocal Clean Lead

- Chain: EQ > Comp > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=90 eq.b1.slope=12_dB eq.b2.type=Bell eq.b2.freq=280 eq.b2.gain=-2 eq.b2.q=1.2 eq.b3.type=Bell eq.b3.freq=3500 eq.b3.gain=2 eq.b3.q=0.9 eq.b4.type=High_Shelf eq.b4.freq=11000 eq.b4.gain=2.5 comp.mix=1 comp.lo.thresh=-26 comp.lo.ratio=2.5 comp.mid.thresh=-26 comp.mid.ratio=2.5 comp.hi.thresh=-28 comp.hi.ratio=2 rev.mode=Plate rev.predelay=20 rev.size=0.5 rev.decay=1.2 rev.mix=0.12 lim.ceiling=-1 lim.character=Clean lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=4`
- Level change (dB): drums +1.2, vocal -0.1*, bass -0.8, pad +0.1; mean +0.1
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Vocal Radio Lead

- Chain: EQ > Dist > Reverb > Comp > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=300 eq.b1.slope=24_dB eq.b2.type=High_Cut eq.b2.freq=5200 eq.b2.slope=24_dB eq.b3.type=Bell eq.b3.freq=1600 eq.b3.gain=4 eq.b3.q=0.8 dist.type=Soft dist.drive=0.3 dist.tone=6000 dist.mix=0.5 comp.mix=1 comp.lo.thresh=-24 comp.lo.ratio=3 comp.mid.thresh=-24 comp.mid.ratio=3 comp.hi.thresh=-24 comp.hi.ratio=3 rev.mode=Room rev.size=0.3 rev.decay=0.4 rev.mix=0.1 lim.ceiling=-1 lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=5.5 in=2`
- Level change (dB): drums -0.9, vocal -0.3*, bass -1.0, pad +3.7; mean +0.4
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Vocal Intimate Close

- Chain: EQ > Comp > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB eq.b2.type=Low_Shelf eq.b2.freq=200 eq.b2.gain=2.5 eq.b3.type=Bell eq.b3.freq=2800 eq.b3.gain=-1.5 eq.b3.q=1 eq.b4.type=High_Shelf eq.b4.freq=9000 eq.b4.gain=3 comp.mix=1 comp.lo.thresh=-28 comp.lo.ratio=3.5 comp.mid.thresh=-28 comp.mid.ratio=3.5 comp.hi.thresh=-28 comp.hi.ratio=3 rev.mode=Room rev.size=0.25 rev.decay=0.35 rev.damping=0.6 rev.mix=0.08 lim.ceiling=-1 lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=4.9`
- Level change (dB): drums +2.5, vocal -0.3*, bass -0.7, pad -0.1; mean +0.3
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Vocal Plate Smooth

- Chain: EQ > Comp > Delay > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB eq.b2.type=Bell eq.b2.freq=4000 eq.b2.gain=1.5 eq.b2.q=0.8 comp.mix=1 comp.lo.thresh=-26 comp.lo.ratio=2.5 comp.mid.thresh=-26 comp.mid.ratio=2.5 comp.hi.thresh=-26 comp.hi.ratio=2.5 rev.mode=Plate rev.predelay=35 rev.size=0.65 rev.decay=2.4 rev.damping=0.55 rev.lowcut=250 rev.highcut=9000 rev.mix=0.3 dly.sync=1 dly.division=1/8 dly.feedback=0.25 dly.mix=0.12 lim.ceiling=-1 lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=4.8`
- Level change (dB): drums +0.4, vocal +0.2*, bass -2.0, pad +0.7; mean -0.2
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Vocal Slap Wide

- Chain: EQ > Comp > Chorus > Delay > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB comp.mix=1 comp.lo.thresh=-26 comp.lo.ratio=2.5 comp.mid.thresh=-26 comp.mid.ratio=2.5 comp.hi.thresh=-26 comp.hi.ratio=2.5 dly.sync=0 dly.time=95 dly.feedback=0.12 dly.pingpong=1 dly.width=100 dly.mix=0.28 cho.mode=Modern cho.rate=0.4 cho.depth=0.25 cho.width=100 cho.mix=0.25 rev.mode=Room rev.size=0.35 rev.decay=0.5 rev.mix=0.1 lim.ceiling=-1 lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=5.8`
- Level change (dB): drums +0.7, vocal -0.1*, bass -0.8, pad +0.6; mean +0.1
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Vocal Telephone Lo-Fi

- Chain: EQ > Dist > Comp > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=400 eq.b1.slope=24_dB eq.b2.type=High_Cut eq.b2.freq=3200 eq.b2.slope=36_dB eq.b3.type=Bell eq.b3.freq=1100 eq.b3.gain=4 eq.b3.q=1.5 dist.type=Crush dist.drive=0.3 dist.tone=4500 dist.mix=0.35 comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=4 comp.mid.thresh=-22 comp.mid.ratio=4 comp.hi.thresh=-22 comp.hi.ratio=4 lim.ceiling=-1 lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=7.5 in=6`
- Level change (dB): drums -0.7, vocal -1.1*, bass +1.0, pad +5.5; mean +1.2
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

## Drums

### Drums Punch Bus

- Chain: EQ > Comp > Dist > Limiter
- Settings: `eq.b1.type=Low_Shelf eq.b1.freq=80 eq.b1.gain=2 eq.b2.type=Bell eq.b2.freq=400 eq.b2.gain=-3 eq.b2.q=1 eq.b3.type=High_Shelf eq.b3.freq=6000 eq.b3.gain=2 comp.mix=1 comp.lo.thresh=-20 comp.lo.ratio=3 comp.lo.attack=30 comp.mid.thresh=-20 comp.mid.ratio=3 comp.mid.attack=20 comp.hi.thresh=-22 comp.hi.ratio=2.5 dist.type=Soft dist.drive=0.2 dist.mix=0.4 lim.ceiling=-1 out=-0.1`
- Level change (dB): drums +0.7*, vocal -1.3, bass -0.4, pad -1.4; mean -0.6
- Peak -5.4 dBFS; tail after 20 s of silence below -120 dBFS

### Drums Room Smash

- Chain: Comp > Dist > Reverb > Limiter
- Settings: `comp.mix=1 comp.lo.thresh=-30 comp.lo.ratio=8 comp.mid.thresh=-30 comp.mid.ratio=8 comp.hi.thresh=-30 comp.hi.ratio=8 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 dist.type=Soft dist.drive=0.35 dist.mix=0.5 rev.mode=Room rev.size=0.4 rev.decay=0.6 rev.predelay=0 rev.mix=0.3 lim.drive=3 lim.ceiling=-1`
- Level change (dB): drums +1.0*, vocal -2.5, bass -1.6, pad -1.1; mean -1.1
- Peak -5.1 dBFS; tail after 20 s of silence below -120 dBFS

### Drums Parallel Crush

- Chain: Comp > Dist > EQ > Limiter
- Settings: `comp.mix=1 comp.lo.thresh=-30 comp.lo.ratio=6 comp.lo.upratio=3 comp.mid.thresh=-30 comp.mid.ratio=6 comp.mid.upratio=3 comp.hi.thresh=-30 comp.hi.ratio=6 comp.hi.upratio=3 dist.type=Hard dist.drive=0.5 dist.tone=8000 dist.mix=0.3 eq.b1.type=High_Shelf eq.b1.freq=8000 eq.b1.gain=3 lim.ceiling=-1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=2.7`
- Level change (dB): drums +0.3*, vocal -2.0, bass -1.1, pad -0.2; mean -0.8
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Drums Tape Glue

- Chain: Dist > Comp > Vib > EQ > Limiter
- Settings: `dist.type=Soft dist.drive=0.3 dist.tone=7000 dist.mix=0.8 vib.rate=0.5 vib.depth=0.05 vib.mix=1 comp.mix=1 comp.lo.thresh=-18 comp.lo.ratio=2 comp.mid.thresh=-18 comp.mid.ratio=2 comp.hi.thresh=-18 comp.hi.ratio=2 eq.b1.type=Low_Cut eq.b1.freq=30 eq.b1.slope=12_dB eq.b2.type=High_Shelf eq.b2.freq=10000 eq.b2.gain=-2 lim.ceiling=-1 out=-1.2`
- Level change (dB): drums +0.5*, vocal -1.1, bass -0.3, pad -0.3; mean -0.3
- Peak -3.6 dBFS; tail after 20 s of silence below -120 dBFS

### Drums Lo-Fi Breaks

- Chain: EQ > Dist > Filter > Comp > Limiter
- Settings: `dist.type=Crush dist.drive=0.5 dist.tone=5000 dist.mix=0.5 flt.type=LP_12 flt.cutoff=7500 flt.resonance=0.15 eq.b1.type=Low_Cut eq.b1.freq=50 eq.b1.slope=12_dB comp.mix=1 comp.lo.thresh=-26 comp.lo.ratio=6 comp.mid.thresh=-26 comp.mid.ratio=6 comp.hi.thresh=-26 comp.hi.ratio=6 lim.ceiling=-1 lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=2.6`
- Level change (dB): drums +1.0*, vocal -1.8, bass -2.0, pad -1.4; mean -1.1
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Drums Gated Space

- Chain: Reverb > Comp > Limiter
- Settings: `comp.mix=1 comp.lo.thresh=-24 comp.lo.ratio=4 comp.mid.thresh=-24 comp.mid.ratio=4 comp.hi.thresh=-24 comp.hi.ratio=4 rev.mode=Chamber rev.size=0.6 rev.decay=0.7 rev.predelay=0 rev.damping=0.4 rev.lowcut=200 rev.highcut=9000 rev.mix=0.45 lim.ceiling=-1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=3`
- Level change (dB): drums +0.8*, vocal -1.2, bass -1.0, pad -1.6; mean -0.8
- Peak -4.5 dBFS; tail after 20 s of silence below -120 dBFS

## Bass

### Bass DI Tighten

- Chain: EQ > Comp > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=30 eq.b1.slope=24_dB eq.b2.type=Bell eq.b2.freq=250 eq.b2.gain=-3 eq.b2.q=1 eq.b3.type=Bell eq.b3.freq=800 eq.b3.gain=2 eq.b3.q=1 comp.mix=1 comp.xoverLow=150 comp.xoverHigh=1500 comp.lo.thresh=-24 comp.lo.ratio=3 comp.mid.thresh=-24 comp.mid.ratio=3 comp.hi.thresh=-24 comp.hi.ratio=3 lim.ceiling=-1 lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=2.1`
- Level change (dB): drums +2.3, vocal -1.7, bass +0.1*, pad -1.5; mean -0.2
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Bass Amp Grit

- Chain: EQ > Dist > Comp > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=40 eq.b1.slope=12_dB eq.b2.type=Bell eq.b2.freq=100 eq.b2.gain=2 eq.b2.q=1 eq.b3.type=High_Cut eq.b3.freq=6000 eq.b3.slope=12_dB dist.type=Hard dist.drive=0.4 dist.tone=3500 dist.mix=0.5 comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=3 comp.mid.thresh=-22 comp.mid.ratio=3 comp.hi.thresh=-22 comp.hi.ratio=3 lim.drive=2 lim.ceiling=-1`
- Level change (dB): drums +2.0, vocal -0.6, bass -0.3*, pad +0.1; mean +0.3
- Peak -2.4 dBFS; tail after 20 s of silence below -120 dBFS

### Bass Sub Focus

- Chain: EQ > Comp > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=25 eq.b1.slope=24_dB eq.b2.type=Bell eq.b2.freq=60 eq.b2.gain=3 eq.b2.q=1 eq.b3.type=Bell eq.b3.freq=300 eq.b3.gain=-4 eq.b3.q=1 eq.b4.type=High_Cut eq.b4.freq=3000 eq.b4.slope=24_dB comp.mix=1 comp.xoverLow=120 comp.lo.thresh=-22 comp.lo.ratio=4 comp.lo.attack=40 comp.mid.thresh=-24 comp.mid.ratio=3 comp.hi.thresh=-24 comp.hi.ratio=3 lim.ceiling=-1 lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=1.1`
- Level change (dB): drums +3.1, vocal -3.9, bass +0.9*, pad -3.9; mean -1.0
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Bass Fuzz Drive

- Chain: Dist > Filter > EQ > Limiter
- Settings: `dist.type=Fold dist.drive=0.55 dist.tone=2500 dist.mix=0.6 flt.type=LP_24 flt.cutoff=1800 flt.resonance=0.15 eq.b1.type=Low_Cut eq.b1.freq=35 eq.b1.slope=12_dB lim.ceiling=-1 out=-0.8`
- Level change (dB): drums +0.1, vocal -3.7, bass +0.6*, pad +0.3; mean -0.7
- Peak -8.8 dBFS; tail after 20 s of silence below -120 dBFS

### Bass Glue Comp

- Chain: EQ > Comp > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=30 eq.b1.slope=12_dB comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=3 comp.lo.knee=6 comp.mid.thresh=-22 comp.mid.ratio=3 comp.mid.knee=6 comp.hi.thresh=-22 comp.hi.ratio=3 comp.hi.knee=6 lim.drive=3.6 lim.ceiling=-1 lim.lookahead=1`
- Level change (dB): drums +1.5, vocal -0.3, bass -0.2*, pad -0.5; mean +0.1
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Bass Chorus Thick

- Chain: EQ > Chorus > Comp > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=30 eq.b1.slope=12_dB cho.mode=Vintage cho.rate=0.5 cho.depth=0.25 cho.width=60 cho.mix=0.28 comp.mix=1 comp.lo.thresh=-24 comp.lo.ratio=3 comp.mid.thresh=-24 comp.mid.ratio=3 comp.hi.thresh=-24 comp.hi.ratio=3 lim.ceiling=-1 lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=3.4`
- Level change (dB): drums +1.7, vocal -0.7, bass -0.3*, pad +0.2; mean +0.2
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

## Guitar

### Guitar Clean Shimmer

- Chain: EQ > Chorus > Delay > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=80 eq.b1.slope=12_dB eq.b2.type=High_Shelf eq.b2.freq=9000 eq.b2.gain=2 cho.mode=Modern cho.rate=0.5 cho.depth=0.3 cho.width=70 cho.mix=0.35 rev.mode=Plate rev.predelay=25 rev.size=0.6 rev.decay=2.2 rev.mix=0.22 dly.sync=1 dly.division=1/8. dly.feedback=0.2 dly.mix=0.1 lim.ceiling=-1 lim.lookahead=1 lim.drive=5.7`
- Level change (dB): drums -1.5, vocal +0.2, bass -2.2, pad +0.7*; mean -0.7
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Guitar Crunch Edge

- Chain: Dist > EQ > Reverb
- Settings: `dist.type=Hard dist.drive=0.45 dist.tone=5500 dist.mix=0.6 eq.b1.type=Low_Cut eq.b1.freq=90 eq.b1.slope=12_dB eq.b2.type=Bell eq.b2.freq=800 eq.b2.gain=2 eq.b2.q=1 eq.b3.type=Bell eq.b3.freq=3000 eq.b3.gain=2 eq.b3.q=1 rev.mode=Room rev.size=0.3 rev.decay=0.5 rev.mix=0.08 out=-2.1`
- Level change (dB): drums -3.6, vocal -1.4, bass -2.3, pad +1.4*; mean -1.5
- Peak -5.5 dBFS; tail after 20 s of silence below -120 dBFS

### Guitar Ambient Swell

- Chain: EQ > Delay > Chorus > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB dly.sync=1 dly.division=1/4 dly.feedback=0.5 dly.pingpong=1 dly.width=100 dly.mix=0.3 rev.mode=Hall rev.predelay=40 rev.size=0.8 rev.decay=4 rev.mix=0.3 cho.mode=Modern cho.rate=0.25 cho.depth=0.3 cho.width=100 cho.mix=0.2 lim.ceiling=-1 lim.lookahead=1 lim.drive=3.5`
- Level change (dB): drums -2.0, vocal +1.7, bass -1.9, pad +0.5*; mean -0.4
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Guitar Slapback Twang

- Chain: EQ > Delay > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=90 eq.b1.slope=12_dB eq.b2.type=Bell eq.b2.freq=3500 eq.b2.gain=2 eq.b2.q=1 dly.sync=0 dly.time=110 dly.feedback=0.15 dly.pingpong=0 dly.mix=0.3 rev.mode=Spring rev.size=0.4 rev.decay=1 rev.mix=0.1 lim.drive=1.2 lim.ceiling=-1`
- Level change (dB): drums -2.4, vocal +0.9, bass -1.9, pad +0.7*; mean -0.7
- Peak -2.2 dBFS; tail after 20 s of silence below -120 dBFS

### Guitar Chorus Twin

- Chain: EQ > Chorus > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=80 eq.b1.slope=12_dB cho.mode=Vintage cho.rate=0.6 cho.depth=0.45 cho.width=100 cho.mix=0.5 rev.mode=Spring rev.size=0.45 rev.decay=1.4 rev.mix=0.15 lim.drive=5 lim.ceiling=-1`
- Level change (dB): drums -2.4, vocal +0.4, bass -2.2, pad +0.8*; mean -0.8
- Peak -1.3 dBFS; tail after 20 s of silence below -120 dBFS

### Guitar Surf Tremolo

- Chain: EQ > Trem > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB eq.b2.type=High_Shelf eq.b2.freq=6000 eq.b2.gain=2 trem.rate=5.5 trem.depth=0.65 trem.shape=Sine trem.stereo=0.3 trem.mix=1 rev.mode=Spring rev.size=0.6 rev.decay=1.8 rev.damping=0.4 rev.mix=0.28 lim.ceiling=-1 lim.lookahead=1 lim.drive=6.2`
- Level change (dB): drums -2.0, vocal +1.3, bass -2.7, pad +0.7*; mean -0.7
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

## Keys

### Keys Rhodes Chorus

- Chain: Chorus > Trem > Reverb > EQ > Limiter
- Settings: `cho.mode=Vintage cho.rate=0.7 cho.depth=0.4 cho.width=100 cho.mix=0.5 trem.rate=4.5 trem.depth=0.25 trem.shape=Sine trem.stereo=1 trem.mix=1 eq.b1.type=Low_Cut eq.b1.freq=60 eq.b1.slope=12_dB eq.b2.type=Bell eq.b2.freq=300 eq.b2.gain=1.5 rev.mode=Room rev.size=0.4 rev.decay=0.8 rev.mix=0.1 lim.drive=4.5 lim.ceiling=-1`
- Level change (dB): drums -2.1, vocal +0.2, bass -2.0, pad +0.7*; mean -0.8
- Peak -1.3 dBFS; tail after 20 s of silence below -120 dBFS

### Keys Piano Air

- Chain: EQ > Comp > Reverb > Limiter
- Settings: `eq.b1.type=High_Shelf eq.b1.freq=12000 eq.b1.gain=3 eq.b2.type=Bell eq.b2.freq=300 eq.b2.gain=-2 eq.b2.q=1 comp.mix=1 comp.lo.thresh=-24 comp.lo.ratio=2 comp.mid.thresh=-24 comp.mid.ratio=2 comp.hi.thresh=-24 comp.hi.ratio=2 rev.mode=Hall rev.predelay=25 rev.size=0.65 rev.decay=2.4 rev.mix=0.2 lim.ceiling=-1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=2.4`
- Level change (dB): drums +2.5, vocal +0.3, bass +0.0, pad -0.6*; mean +0.5
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Keys Lo-Fi Tape

- Chain: Dist > Vib > Chorus > EQ > Limiter
- Settings: `dist.type=Soft dist.drive=0.25 dist.tone=6000 dist.mix=0.6 vib.rate=0.4 vib.depth=0.15 vib.mix=1 cho.mode=VHS cho.mix=0.4 eq.b1.type=Low_Cut eq.b1.freq=60 eq.b1.slope=12_dB eq.b2.type=High_Cut eq.b2.freq=7500 eq.b2.slope=12_dB lim.drive=0.5 lim.ceiling=-1`
- Level change (dB): drums -1.1, vocal -0.8, bass -0.9, pad +0.6*; mean -0.6
- Peak -4.6 dBFS; tail after 20 s of silence below -120 dBFS

### Keys Wide Stereo

- Chain: Chorus > Delay > Reverb > EQ > Limiter
- Settings: `cho.mode=Modern cho.rate=0.35 cho.depth=0.3 cho.width=100 cho.mix=0.35 dly.sync=1 dly.division=1/8 dly.feedback=0.3 dly.pingpong=1 dly.width=100 dly.mix=0.12 rev.mode=Plate rev.size=0.5 rev.decay=1.6 rev.mix=0.15 eq.b1.type=Low_Cut eq.b1.freq=60 eq.b1.slope=12_dB lim.drive=4.7 lim.ceiling=-1`
- Level change (dB): drums -1.6, vocal +0.2, bass -1.8, pad +0.6*; mean -0.7
- Peak -2.8 dBFS; tail after 20 s of silence below -120 dBFS

### Keys Organ Swirl

- Chain: Dist > Mod > Trem > Reverb > Limiter
- Settings: `dist.type=Soft dist.drive=0.2 dist.tone=7000 dist.mix=0.5 phs.type=Phaser phs.rate=1.2 phs.depth=0.6 phs.feedback=0.3 phs.stages=4 phs.mix=0.4 trem.rate=6 trem.depth=0.2 trem.shape=Sine trem.stereo=0.5 trem.mix=1 rev.mode=Spring rev.size=0.4 rev.decay=1 rev.mix=0.1 lim.drive=2.7 lim.ceiling=-1`
- Level change (dB): drums +2.8, vocal -1.8, bass +2.7, pad -0.8*; mean +0.7
- Peak -4.4 dBFS; tail after 20 s of silence below -120 dBFS

### Keys Tape Warm

- Chain: Dist > EQ > Chorus > Vib > Comp > Limiter
- Settings: `dist.type=Soft dist.drive=0.2 dist.tone=8000 dist.mix=0.6 vib.rate=0.3 vib.depth=0.06 vib.mix=1 cho.mode=Vintage cho.rate=0.3 cho.depth=0.15 cho.width=60 cho.mix=0.18 eq.b1.type=Low_Shelf eq.b1.freq=150 eq.b1.gain=1.5 eq.b2.type=High_Shelf eq.b2.freq=9000 eq.b2.gain=-2 comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=2 comp.mid.thresh=-22 comp.mid.ratio=2 comp.hi.thresh=-22 comp.hi.ratio=2 lim.drive=2.1 lim.ceiling=-1`
- Level change (dB): drums +1.4, vocal -0.9, bass +0.1, pad -0.1*; mean +0.1
- Peak -3.3 dBFS; tail after 20 s of silence below -120 dBFS

## Synth

### Synth Pad Widener

- Chain: Chorus > EQ > Reverb > Limiter
- Settings: `cho.mode=Modern cho.rate=0.3 cho.depth=0.35 cho.width=100 cho.mix=0.4 rev.mode=Hall rev.predelay=30 rev.size=0.75 rev.decay=3.2 rev.mix=0.2 eq.b1.type=Low_Cut eq.b1.freq=80 eq.b1.slope=12_dB lim.drive=5.5 lim.ceiling=-1 lim.lookahead=1`
- Level change (dB): drums -2.1, vocal +0.2, bass -2.6, pad +0.9*; mean -0.9
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Synth Lead Presence

- Chain: EQ > Dist > Delay > Comp > Limiter
- Settings: `eq.b1.type=Bell eq.b1.freq=2500 eq.b1.gain=3 eq.b1.q=0.9 eq.b2.type=High_Shelf eq.b2.freq=9000 eq.b2.gain=1.5 dist.type=Soft dist.drive=0.15 dist.mix=0.5 comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=2.5 comp.mid.thresh=-22 comp.mid.ratio=2.5 comp.hi.thresh=-22 comp.hi.ratio=2.5 dly.sync=1 dly.division=1/8 dly.feedback=0.3 dly.mix=0.15 lim.drive=1.9 lim.ceiling=-1`
- Level change (dB): drums +1.4, vocal -0.7, bass -0.4, pad -0.1*; mean +0.0
- Peak -2.8 dBFS; tail after 20 s of silence below -120 dBFS

### Synth Arp Delay

- Chain: EQ > Delay > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB dly.sync=1 dly.division=1/8. dly.feedback=0.45 dly.pingpong=1 dly.width=100 dly.mix=0.3 rev.mode=Plate rev.size=0.5 rev.decay=1.4 rev.mix=0.1 lim.drive=1.8 lim.ceiling=-1 lim.lookahead=1`
- Level change (dB): drums -2.2, vocal +1.4, bass -2.7, pad +0.7*; mean -0.7
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Synth Pluck Space

- Chain: EQ > Delay > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=100 eq.b1.slope=12_dB rev.mode=Chamber rev.predelay=10 rev.size=0.5 rev.decay=1.2 rev.mix=0.22 dly.sync=1 dly.division=1/16 dly.feedback=0.25 dly.mix=0.12 lim.drive=3 lim.ceiling=-1`
- Level change (dB): drums -2.2, vocal +1.7, bass -2.8, pad +0.7*; mean -0.7
- Peak -1.6 dBFS; tail after 20 s of silence below -120 dBFS

### Synth Tape Warp

- Chain: Vib > Dist > Chorus > EQ
- Settings: `vib.rate=0.3 vib.depth=0.25 vib.mix=1 cho.mode=VHS cho.vhsWow=55 cho.vhsFlutter=20 cho.vhsTone=40 cho.vhsSat=30 cho.vhsHiss=5 cho.vhsDropouts=0 cho.mix=0.5 dist.type=Soft dist.drive=0.2 dist.mix=0.5 eq.b1.type=High_Cut eq.b1.freq=9000 eq.b1.slope=12_dB`
- Level change (dB): drums +0.0, vocal -1.7, bass +0.4, pad -0.1*; mean -0.3
- Peak -5.9 dBFS; tail after 20 s of silence below -120 dBFS

### Synth Stereo Glue

- Chain: Comp > Chorus > EQ > Limiter
- Settings: `comp.mix=1 comp.lo.thresh=-22 comp.lo.ratio=2.5 comp.lo.knee=6 comp.mid.thresh=-22 comp.mid.ratio=2.5 comp.mid.knee=6 comp.hi.thresh=-22 comp.hi.ratio=2.5 comp.hi.knee=6 cho.mode=Modern cho.rate=0.4 cho.depth=0.2 cho.width=80 cho.mix=0.2 eq.b1.type=Tilt_Shelf eq.b1.freq=1000 eq.b1.gain=1 lim.ceiling=-1 lim.lookahead=1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=2.8`
- Level change (dB): drums +1.5, vocal -0.8, bass -0.5, pad -0.1*; mean +0.0
- Peak -1.2 dBFS; tail after 20 s of silence below -120 dBFS

## FX

### FX Radio Sweep

- Chain: Filter > EQ > Dist > Limiter
- Settings: `sc.source=Input sc.attack=5 sc.release=350 sc.gain=0 flt.type=LP_12 flt.cutoff=1400 flt.resonance=0.45 flt.mix=1 m1=flt.cutoff:0.4 eq.b1.type=Low_Cut eq.b1.freq=200 eq.b1.slope=12_dB dist.type=Soft dist.drive=0.3 dist.mix=0.4 lim.ceiling=-1 lim.lookahead=1 lim.drive=3.2`
- Level change (dB): drums -4.2, vocal +2.2, bass -2.6, pad +4.6; mean +0.0
- Peak -2.3 dBFS; tail after 20 s of silence below -120 dBFS

### FX Robot Voice

- Chain: Mod > Dist > EQ > Limiter
- Settings: `phs.type=Flanger phs.rate=0.05 phs.depth=0.1 phs.feedback=0.75 phs.manual=4 phs.mix=0.6 dist.type=Crush dist.drive=0.3 dist.tone=6000 dist.mix=0.4 eq.b1.type=Low_Cut eq.b1.freq=150 eq.b1.slope=12_dB lim.ceiling=-1 lim.lookahead=1 lim.drive=2.6`
- Level change (dB): drums -1.7, vocal +3.5, bass -5.0, pad +2.1; mean -0.3
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### FX Underwater

- Chain: Filter > Chorus > Vib > Reverb > Limiter
- Settings: `flt.type=LP_24 flt.cutoff=700 flt.resonance=0.3 cho.mode=Modern cho.rate=0.8 cho.depth=0.7 cho.width=100 cho.mix=0.6 vib.rate=1.2 vib.depth=0.3 vib.mix=1 rev.mode=Hall rev.size=0.7 rev.decay=3 rev.damping=0.8 rev.mix=0.3 lim.ceiling=-1 lim.drive=4 lim.lookahead=1 in=3`
- Level change (dB): drums -0.5, vocal +2.1, bass -0.7, pad -0.0; mean +0.2
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### FX Megaphone

- Chain: EQ > Dist > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=600 eq.b1.slope=36_dB eq.b2.type=High_Cut eq.b2.freq=3500 eq.b2.slope=24_dB eq.b3.type=Bell eq.b3.freq=1800 eq.b3.gain=3 eq.b3.q=1.5 dist.type=Hard dist.drive=0.5 dist.tone=4500 dist.mix=0.7 lim.ceiling=-1 lim.lookahead=1 lim.drive=5.4 in=6`
- Level change (dB): drums +0.3, vocal -7.6, bass -1.1, pad +8.5; mean +0.0
- Peak -2.5 dBFS; tail after 20 s of silence below -120 dBFS

### FX Alien Whistle

- Chain: Grain > Mod > Reverb > Limiter
- Settings: `glt.size=60 glt.density=40 glt.pitch=7 glt.spread=0.3 glt.spreadPitch=6 glt.position=150 glt.feedback=0.2 glt.release=0.5 glt.mix=0.4 phs.type=Phaser phs.rate=0.3 phs.depth=0.7 phs.feedback=0.6 phs.stages=8 phs.mix=0.35 rev.mode=Plate rev.size=0.6 rev.decay=2 rev.mix=0.2 lim.ceiling=-1 lim.lookahead=1 lim.drive=5.6 in=2`
- Level change (dB): drums +1.2, vocal -1.3, bass +1.0, pad -0.1; mean +0.2
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### FX Glitch Stutter

- Chain: Grain > Delay > Limiter
- Settings: `glt.sync=1 glt.division=1/16 glt.size=40 glt.density=30 glt.spread=0.3 glt.position=100 glt.reverse=0.3 glt.release=0.3 glt.mix=0.5 dly.sync=1 dly.division=1/16 dly.feedback=0.3 dly.mix=0.15 lim.ceiling=-1 lim.lookahead=1 lim.drive=5.4`
- Level change (dB): drums +0.2, vocal +0.3, bass +0.1, pad -0.4; mean +0.0
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

## Mixbus

### Mixbus Glue

- Chain: Comp > Limiter
- Settings: `comp.mix=1 comp.lo.thresh=-20 comp.lo.ratio=2 comp.lo.knee=6 comp.mid.thresh=-20 comp.mid.ratio=2 comp.mid.knee=6 comp.hi.thresh=-20 comp.hi.ratio=2 comp.hi.knee=6 lim.drive=1.9 lim.ceiling=-1`
- Level change (dB): drums +0.9, vocal -0.3, bass -0.2, pad -0.5; mean -0.0
- Peak -3.5 dBFS; tail after 20 s of silence below -120 dBFS

### Mixbus Punch

- Chain: EQ > Comp > Dist > Limiter
- Settings: `eq.b1.type=High_Shelf eq.b1.freq=9000 eq.b1.gain=1.5 comp.mix=1 comp.lo.thresh=-18 comp.lo.ratio=3 comp.lo.attack=35 comp.mid.thresh=-18 comp.mid.ratio=3 comp.mid.attack=22 comp.hi.thresh=-20 comp.hi.ratio=2.5 comp.hi.attack=8 dist.type=Soft dist.drive=0.1 dist.mix=0.4 lim.ceiling=-1`
- Level change (dB): drums +0.6, vocal -0.3, bass -0.1, pad -0.3; mean -0.0
- Peak -5.4 dBFS; tail after 20 s of silence below -120 dBFS

### Mixbus Warm Tape

- Chain: Dist > EQ > Vib > Comp > Limiter
- Settings: `dist.type=Soft dist.drive=0.2 dist.tone=9000 dist.mix=0.7 eq.b1.type=Low_Shelf eq.b1.freq=100 eq.b1.gain=1 eq.b2.type=High_Shelf eq.b2.freq=10000 eq.b2.gain=-1.5 vib.rate=0.3 vib.depth=0.04 vib.mix=1 comp.mix=1 comp.lo.thresh=-18 comp.lo.ratio=1.8 comp.mid.thresh=-18 comp.mid.ratio=1.8 comp.hi.thresh=-18 comp.hi.ratio=1.8 lim.ceiling=-1 out=-1`
- Level change (dB): drums +1.0, vocal -0.8, bass +0.2, pad -0.3; mean +0.0
- Peak -5.5 dBFS; tail after 20 s of silence below -120 dBFS

### Mixbus Wide Polish

- Chain: EQ > Chorus > Reverb > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=25 eq.b1.slope=12_dB eq.b2.type=High_Shelf eq.b2.freq=12000 eq.b2.gain=2 cho.mode=Modern cho.rate=0.3 cho.depth=0.15 cho.width=100 cho.mix=0.12 rev.mode=Plate rev.size=0.5 rev.decay=1.2 rev.mix=0.05 lim.ceiling=-1 lim.drive=1.7`
- Level change (dB): drums +0.0, vocal -0.2, bass -0.1, pad +0.4; mean +0.0
- Peak -4.9 dBFS; tail after 20 s of silence below -120 dBFS

### Mixbus Parallel Comp

- Chain: Comp > Limiter
- Settings: `comp.mix=1 comp.lo.thresh=-24 comp.lo.ratio=3 comp.lo.upratio=2 comp.mid.thresh=-24 comp.mid.ratio=3 comp.mid.upratio=2 comp.hi.thresh=-24 comp.hi.ratio=3 comp.hi.upratio=2 lim.ceiling=-1 comp.lo.gain=3 comp.mid.gain=3 comp.hi.gain=3 lim.drive=1.2`
- Level change (dB): drums +0.7, vocal -1.1, bass -0.5, pad -0.1; mean -0.2
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Mixbus Gentle Polish

- Chain: EQ > Comp > Limiter
- Settings: `eq.b1.type=Tilt_Shelf eq.b1.freq=1000 eq.b1.gain=0.8 comp.mix=1 comp.lo.thresh=-16 comp.lo.ratio=1.5 comp.mid.thresh=-16 comp.mid.ratio=1.5 comp.hi.thresh=-16 comp.hi.ratio=1.5 comp.lo.knee=8 comp.mid.knee=8 comp.hi.knee=8 lim.ceiling=-1 lim.drive=0.8`
- Level change (dB): drums +0.4, vocal -0.1, bass -0.1, pad -0.1; mean +0.0
- Peak -4.2 dBFS; tail after 20 s of silence below -120 dBFS

## Mastering

### Master Transparent Loud

- Chain: EQ > Comp > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=25 eq.b1.slope=12_dB comp.mix=1 comp.lo.thresh=-14 comp.lo.ratio=2 comp.lo.knee=8 comp.mid.thresh=-14 comp.mid.ratio=2 comp.mid.knee=8 comp.hi.thresh=-14 comp.hi.ratio=2 comp.hi.knee=8 lim.drive=5 lim.ceiling=-0.3 lim.character=Clean lim.truePeak=1 lim.lookahead=1`
- Level change (dB): drums +4.8, vocal +4.5, bass +4.4, pad +4.5; mean +4.6
- Peak -1.3 dBFS; tail after 20 s of silence below -120 dBFS

### Master Warm Glue

- Chain: Dist > EQ > Comp > Limiter
- Settings: `dist.type=Soft dist.drive=0.12 dist.tone=9000 dist.mix=0.5 eq.b1.type=Low_Shelf eq.b1.freq=100 eq.b1.gain=1 eq.b2.type=High_Shelf eq.b2.freq=10000 eq.b2.gain=-1 comp.mix=1 comp.lo.thresh=-14 comp.lo.ratio=1.8 comp.mid.thresh=-14 comp.mid.ratio=1.8 comp.hi.thresh=-14 comp.hi.ratio=1.8 lim.drive=3 lim.ceiling=-0.5 lim.truePeak=1 lim.lookahead=1`
- Level change (dB): drums +5.0, vocal +3.9, bass +4.8, pad +4.2; mean +4.5
- Peak -1.5 dBFS; tail after 20 s of silence below -120 dBFS

### Master Bright Air

- Chain: EQ > Comp > Limiter
- Settings: `eq.b1.type=High_Shelf eq.b1.freq=10000 eq.b1.gain=2 eq.b2.type=Bell eq.b2.freq=3000 eq.b2.gain=1 eq.b2.q=0.8 comp.mix=1 comp.lo.thresh=-14 comp.lo.ratio=2 comp.mid.thresh=-14 comp.mid.ratio=2 comp.hi.thresh=-14 comp.hi.ratio=2 lim.drive=3 lim.ceiling=-0.5 lim.truePeak=1 lim.lookahead=1`
- Level change (dB): drums +3.2, vocal +2.8, bass +2.8, pad +2.9; mean +2.9
- Peak -1.5 dBFS; tail after 20 s of silence below -120 dBFS

### Master Safe Limiter

- Chain: Limiter
- Settings: `lim.drive=0 lim.ceiling=-1 lim.character=Clean lim.truePeak=1 lim.lookahead=1`
- Level change (dB): drums +0.0, vocal -0.0, bass -0.0, pad -0.0; mean -0.0
- Peak -6.0 dBFS; tail after 20 s of silence below -120 dBFS

### Master Streaming Level

- Chain: Comp > Limiter
- Settings: `comp.mix=1 comp.lo.thresh=-16 comp.lo.ratio=1.8 comp.mid.thresh=-16 comp.mid.ratio=1.8 comp.hi.thresh=-16 comp.hi.ratio=1.8 lim.drive=2.5 lim.ceiling=-1 lim.character=Clean lim.truePeak=1 lim.lookahead=1`
- Level change (dB): drums +2.4, vocal +1.9, bass +1.9, pad +1.7; mean +2.0
- Peak -2.8 dBFS; tail after 20 s of silence below -120 dBFS

### Master Dynamic Open

- Chain: Comp > Limiter
- Settings: `comp.mix=1 comp.lo.thresh=-12 comp.lo.ratio=1.5 comp.mid.thresh=-12 comp.mid.ratio=1.5 comp.hi.thresh=-12 comp.hi.ratio=1.5 comp.lo.knee=10 comp.mid.knee=10 comp.hi.knee=10 lim.drive=1 lim.ceiling=-0.5 lim.truePeak=1 lim.lookahead=1`
- Level change (dB): drums +1.0, vocal +0.9, bass +0.9, pad +0.8; mean +0.9
- Peak -3.8 dBFS; tail after 20 s of silence below -120 dBFS

## Creative

### Creative Pump Duck

- Chain: Trem > Reverb > Limiter
- Settings: `sc.source=Input sc.attack=2 sc.release=250 trem.sync=1 trem.division=1/4 trem.depth=0.55 trem.shape=Saw trem.mix=1 rev.mode=Hall rev.size=0.7 rev.decay=3 rev.mix=0.4 m1=rev.mix:-0.3 lim.ceiling=-1 lim.lookahead=1 lim.drive=4.4`
- Level change (dB): drums -1.5, vocal +1.5, bass +0.7, pad -0.6; mean +0.0
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Creative Filter Sweep

- Chain: Filter > Mod > Limiter
- Settings: `flt.type=LP_12 flt.cutoff=1200 flt.resonance=0.5 sc.source=Input sc.attack=8 sc.release=500 m1=flt.cutoff:0.5 phs.type=Phaser phs.sync=1 phs.division=1/1 phs.depth=0.8 phs.feedback=0.4 phs.stages=6 phs.mix=0.4 lim.drive=2.7 lim.ceiling=-1`
- Level change (dB): drums +1.1, vocal -1.3, bass +1.5, pad -1.2; mean +0.0
- Peak -1.5 dBFS; tail after 20 s of silence below -120 dBFS

### Creative Rhythmic Tremolo

- Chain: Trem > Reverb > Limiter
- Settings: `trem.sync=1 trem.division=1/8 trem.depth=0.8 trem.shape=Square trem.stereo=0.5 trem.mix=1 rev.mode=Plate rev.size=0.5 rev.decay=1.6 rev.mix=0.15 lim.drive=3.2 lim.ceiling=-1`
- Level change (dB): drums +1.3, vocal -0.5, bass -0.2, pad -0.6; mean -0.0
- Peak -2.8 dBFS; tail after 20 s of silence below -120 dBFS

### Creative Ping-Pong Bounce

- Chain: Delay > Reverb
- Settings: `dly.sync=1 dly.division=1/8 dly.feedback=0.5 dly.pingpong=1 dly.width=100 dly.mix=0.35 rev.mode=Plate rev.size=0.5 rev.decay=1.4 rev.mix=0.1`
- Level change (dB): drums +0.1, vocal +0.1, bass -0.4, pad -0.2; mean -0.1
- Peak -3.1 dBFS; tail after 20 s of silence below -120 dBFS

### Creative Glitter Freeze

- Chain: Grain > Reverb > Limiter
- Settings: `glt.size=220 glt.density=18 glt.pitch=0 glt.spread=0.4 glt.spreadPitch=2 glt.position=500 glt.feedback=0.3 glt.release=3 glt.mix=0.45 rev.mode=Hall rev.size=0.7 rev.decay=3 rev.mix=0.25 lim.drive=3.8 lim.ceiling=-1`
- Level change (dB): drums +0.2, vocal +0.3, bass +0.6, pad -1.3; mean -0.0
- Peak -2.2 dBFS; tail after 20 s of silence below -120 dBFS

### Creative Wobble Warp

- Chain: Trem > Vib > Filter > Limiter
- Settings: `vib.rate=2.5 vib.depth=0.45 vib.mix=1 trem.sync=1 trem.division=1/8T trem.depth=0.4 trem.shape=Triangle trem.mix=1 flt.type=LP_12 flt.cutoff=3000 flt.resonance=0.3 lim.drive=2.1 lim.ceiling=-1`
- Level change (dB): drums -0.7, vocal +0.3, bass +0.2, pad +0.2; mean +0.0
- Peak -4.1 dBFS; tail after 20 s of silence below -120 dBFS

## Ambient

### Ambient Huge Hall

- Chain: Reverb > Limiter
- Settings: `rev.mode=Hall rev.predelay=40 rev.size=1 rev.decay=6.5 rev.damping=0.55 rev.moddepth=0.4 rev.lowcut=150 rev.highcut=9000 rev.mix=0.5 lim.ceiling=-1 lim.lookahead=1 lim.drive=1`
- Level change (dB): drums -1.7, vocal +1.6, bass -1.2, pad +0.2*; mean -0.3
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Ambient Glitter Cloud

- Chain: Grain > Reverb > Limiter
- Settings: `glt.size=250 glt.density=30 glt.pitch=0 glt.spread=0.5 glt.spreadPitch=4 glt.position=800 glt.feedback=0.3 glt.release=2.5 glt.mix=0.5 rev.mode=Hall rev.predelay=30 rev.size=0.8 rev.decay=4 rev.mix=0.3 lim.ceiling=-1 lim.lookahead=1 lim.drive=4.4`
- Level change (dB): drums +0.9, vocal +0.4, bass +0.7, pad -0.4*; mean +0.4
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Ambient Dub Delay Space

- Chain: Delay > Filter > Reverb > Limiter
- Settings: `dly.sync=1 dly.division=1/4. dly.feedback=0.6 dly.pingpong=1 dly.width=100 dly.mix=0.35 flt.type=LP_12 flt.cutoff=4500 flt.resonance=0.1 rev.mode=Chamber rev.size=0.6 rev.decay=2.5 rev.mix=0.2 lim.drive=0.3 lim.ceiling=-1`
- Level change (dB): drums -0.6, vocal -0.2, bass -0.1, pad +0.1*; mean -0.2
- Peak -3.4 dBFS; tail after 20 s of silence below -120 dBFS

### Ambient Endless Wash

- Chain: Grain > Chorus > Reverb > Limiter
- Settings: `glt.size=300 glt.density=20 glt.spread=0.5 glt.position=1000 glt.feedback=0.35 glt.release=5 glt.mix=0.3 cho.mode=Modern cho.rate=0.2 cho.depth=0.4 cho.width=100 cho.mix=0.25 rev.mode=Hall rev.predelay=50 rev.size=1 rev.decay=8 rev.damping=0.5 rev.moddepth=0.5 rev.mix=0.55 lim.ceiling=-1 lim.lookahead=1 lim.drive=4`
- Level change (dB): drums +0.0, vocal +2.0, bass +0.6, pad -0.5*; mean +0.5
- Peak -1.0 dBFS; tail after 20 s of silence -104.591 dBFS

### Ambient Shimmer Rise

- Chain: Grain > Reverb > Limiter
- Settings: `glt.size=180 glt.density=25 glt.pitch=12 glt.spread=0.3 glt.spreadPitch=1 glt.position=600 glt.feedback=0.45 glt.release=3 glt.mix=0.4 rev.mode=Hall rev.predelay=35 rev.size=0.85 rev.decay=5 rev.mix=0.35 lim.ceiling=-1 lim.lookahead=1 lim.drive=3.2`
- Level change (dB): drums +0.2, vocal +0.4, bass -0.1, pad -0.1*; mean +0.1
- Peak -1.0 dBFS; tail after 20 s of silence below -120 dBFS

### Ambient Distant Memory

- Chain: Filter > Vib > Reverb
- Settings: `flt.type=LP_24 flt.cutoff=2500 flt.resonance=0.1 vib.rate=0.3 vib.depth=0.15 vib.mix=1 rev.mode=Hall rev.predelay=80 rev.size=0.9 rev.decay=5 rev.damping=0.7 rev.mix=0.5 out=-0.8`
- Level change (dB): drums +0.7, vocal +0.8, bass +1.3, pad -0.5*; mean +0.6
- Peak -1.8 dBFS; tail after 20 s of silence below -120 dBFS

## Lo-Fi

### Lo-Fi Cassette Tape

- Chain: Dist > Vib > Chorus > EQ > Limiter
- Settings: `dist.type=Soft dist.drive=0.25 dist.tone=6000 dist.mix=0.6 vib.rate=0.35 vib.depth=0.1 vib.mix=1 cho.mode=Vintage cho.rate=0.3 cho.depth=0.1 cho.width=40 cho.mix=0.25 eq.b1.type=Low_Cut eq.b1.freq=50 eq.b1.slope=12_dB eq.b2.type=High_Cut eq.b2.freq=8000 eq.b2.slope=12_dB lim.drive=0.3 lim.ceiling=-1`
- Level change (dB): drums -0.8, vocal -0.3, bass -0.1, pad +1.0; mean -0.0
- Peak -5.3 dBFS; tail after 20 s of silence below -120 dBFS

### Lo-Fi Vinyl Dust

- Chain: EQ > Chorus > Dist > Limiter
- Settings: `cho.mode=VHS cho.vhsWow=15 cho.vhsFlutter=10 cho.vhsTone=55 cho.vhsSat=20 cho.vhsHiss=20 cho.vhsDropouts=15 cho.mix=0.6 eq.b1.type=Low_Cut eq.b1.freq=60 eq.b1.slope=12_dB eq.b2.type=High_Cut eq.b2.freq=9000 eq.b2.slope=12_dB dist.type=Soft dist.drive=0.15 dist.mix=0.5 lim.drive=0.9 lim.ceiling=-1`
- Level change (dB): drums -0.6, vocal +0.1, bass -0.1, pad +0.7; mean +0.0
- Peak -4.7 dBFS; tail after 20 s of silence below -120 dBFS

### Lo-Fi Bitcrushed

- Chain: Dist > Filter > EQ > Limiter
- Settings: `dist.type=Crush dist.drive=0.6 dist.tone=6000 dist.mix=0.7 flt.type=LP_12 flt.cutoff=9000 flt.resonance=0.1 eq.b1.type=Low_Cut eq.b1.freq=40 eq.b1.slope=12_dB lim.drive=0.5 lim.ceiling=-1`
- Level change (dB): drums -0.5, vocal +0.4, bass -0.1, pad +0.3; mean +0.0
- Peak -4.7 dBFS; tail after 20 s of silence below -120 dBFS

### Lo-Fi Broken Speaker

- Chain: EQ > Dist > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=500 eq.b1.slope=36_dB eq.b2.type=High_Cut eq.b2.freq=4000 eq.b2.slope=24_dB eq.b3.type=Bell eq.b3.freq=1200 eq.b3.gain=3 eq.b3.q=2 dist.type=Hard dist.drive=0.55 dist.tone=4000 dist.mix=0.8 lim.drive=4.7 lim.ceiling=-1 in=4`
- Level change (dB): drums -1.0, vocal -6.2, bass -0.9, pad +8.0; mean -0.0
- Peak -4.4 dBFS; tail after 20 s of silence below -120 dBFS

### Lo-Fi VHS Chorus

- Chain: Chorus > EQ > Limiter
- Settings: `cho.mode=VHS cho.vhsWow=45 cho.vhsFlutter=30 cho.vhsTone=40 cho.vhsSat=35 cho.vhsHiss=18 cho.vhsDropouts=10 cho.width=60 cho.mix=0.8 eq.b1.type=Low_Cut eq.b1.freq=50 eq.b1.slope=12_dB lim.drive=1.2 lim.ceiling=-1`
- Level change (dB): drums -0.9, vocal +0.6, bass -0.6, pad +1.1; mean +0.1
- Peak -4.0 dBFS; tail after 20 s of silence below -120 dBFS

### Lo-Fi AM Radio

- Chain: EQ > Dist > Chorus > Limiter
- Settings: `eq.b1.type=Low_Cut eq.b1.freq=250 eq.b1.slope=24_dB eq.b2.type=High_Cut eq.b2.freq=3500 eq.b2.slope=36_dB eq.b3.type=Bell eq.b3.freq=1000 eq.b3.gain=3 eq.b3.q=1 dist.type=Soft dist.drive=0.35 dist.tone=5000 dist.mix=0.6 cho.mode=VHS cho.vhsWow=5 cho.vhsFlutter=5 cho.vhsTone=50 cho.vhsSat=0 cho.vhsHiss=25 cho.vhsDropouts=0 cho.mix=0.3 lim.ceiling=-1 lim.lookahead=1 lim.drive=4.9 in=2`
- Level change (dB): drums -4.9, vocal +0.8, bass -2.1, pad +6.2; mean -0.0
- Peak -1.1 dBFS; tail after 20 s of silence below -120 dBFS

---
Largest absolute mean level change over the four sources: 1.5 dB (Mastering excluded from the limit).
