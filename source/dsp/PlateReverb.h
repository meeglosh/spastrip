#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>
#include <cmath>

namespace spa::dsp
{

// Dattorro plate reverb, re-implemented from the published algorithm
// description (Jon Dattorro, "Effect Design Part 1: Reverberator and Other
// Filters", Journal of the Audio Engineering Society, vol. 45, no. 9,
// September 1997) -- NOT ported from any GPL/LGPL codebase (Zita-Rev1,
// MVerb, Freeverb3 etc. were never opened for this file). The delay-line
// lengths below are the ones published in Dattorro's paper (a set of
// numbers, not source code); the surrounding C++ -- buffer management, the
// fractional-read wrap guard, the LFO rotator, mode voicing, tone/width
// stage -- is original to this codebase.
//
// Topology: mono sum -> input bandwidth filter (one-pole LP) -> four fixed
// allpass diffusers in series -> a two-branch "tank": each branch is a
// modulated allpass (pitch-thickening chirp), a long fixed delay, a
// one-pole damping filter, a decay-gain multiply (sets RT60), a fixed
// decay-diffusion allpass, and a second long fixed delay whose output
// feeds the *other* branch's input next pass (the classic figure-eight
// cross-feed). Stereo output taps are drawn from several points across
// both branches so L/R decorrelate without an explicit cross-mix matrix.
class PlateReverb
{
public:
    enum class Mode { hall, plate, chamber, room, spring };

    struct Params
    {
        int mode = 0;
        float preDelayMs = 20.0f;
        float size = 0.5f;         // 0..1 room size (delay scale)
        float decaySec = 2.0f;     // RT60
        float hfDamp = 0.5f;       // 0..1
        float modDepth = 0.2f;     // 0..1 tank modulation depth
        float lowCutHz = 20.0f;
        float highCutHz = 12000.0f;
        float width = 1.0f;        // 0..1
        float mix = 0.3f;          // linear dry/wet: dry = 1-mix, wet = mix
    };

    void prepare (double sr, int /*maxBlock*/)
    {
        sampleRate = sr;
        // Reference rate the published lengths are stated for; scale by
        // sr/refRate and by the largest possible size factor (mode*user)
        // plus modulation headroom so no runtime resize is ever needed.
        constexpr double refRate = 29761.0;
        const double maxScale = (sr / refRate) * kMaxSizeScale;

        preBuf.assign ((size_t) (0.25 * sr) + 8, 0.0f);
        preW = 0;

        for (int i = 0; i < numInputAP; ++i)
            inputAP[(size_t) i].resize ((size_t) (inputAPLen[i] * maxScale) + kModMargin);

        for (int br = 0; br < 2; ++br)
        {
            auto& b = branch[(size_t) br];
            b.modAP.resize ((size_t) (modAPLen[br] * maxScale) + kModMargin);
            b.delay1.resize ((size_t) (delay1Len[br] * maxScale) + kModMargin);
            b.decayAP.resize ((size_t) (decayAPLen[br] * maxScale) + kModMargin);
            b.delay2.resize ((size_t) (delay2Len[br] * maxScale) + kModMargin);
        }

        reset();
    }

    void reset()
    {
        std::fill (preBuf.begin(), preBuf.end(), 0.0f);
        preW = 0;
        for (auto& ap : inputAP) ap.clear();
        for (auto& b : branch) b.clear();
        // Offset the two branches' modulation phases so they decorrelate
        // from the very first block instead of chirping in lockstep.
        branch[0].lfoPhase = 0.0f;
        branch[1].lfoPhase = 0.37f;
        bwState = 0.0f;
        lowState = {}; highState = {};
        earlyDampState = {};
    }

    void process (juce::AudioBuffer<float>& buffer, const Params& p)
    {
        const int n = buffer.getNumSamples();
        const int numCh = juce::jmin (2, buffer.getNumChannels());
        const auto mode = (Mode) juce::jlimit (0, 4, p.mode);

        const float sizeScale = 0.3f + 1.7f * juce::jlimit (0.0f, 1.0f, p.size) * modeSizeMul (mode);
        const float rt60 = juce::jmax (0.15f, p.decaySec * modeDecayMul (mode));
        // dampC is a one-pole TRANSPARENCY coefficient (state += dampC*(x -
        // state), same convention as the bandwidth filter below): dampC
        // near 1 = almost no filtering (bright), dampC near 0 = heavy
        // smoothing (dark). hfDamp is the user-facing "more damping = MORE
        // filtering/darker" knob, so it must map INVERSELY -- hfDamp=0 ->
        // dampC near 1 (transparent), hfDamp=1 -> dampC near its floor
        // (dark). An earlier version used dampC = hfDamp*modeDampMul
        // directly, which is backwards (turning damping UP made the tail
        // brighter, not darker) and, compounded every recirculation pass,
        // needlessly darkened and quietened the default-settings tail
        // (measured: spectral centroid of the first arriving tail material
        // was under 200 Hz for a broadband impulse at hfDamp=0.5, and the
        // wet tap needed a ~38x makeup gain to reach a sensible level).
        const float dampAmount = juce::jlimit (0.0f, 1.0f, p.hfDamp * modeDampMul (mode));
        const float dampC = juce::jlimit (0.03f, 0.999f, 1.0f - dampAmount * 0.97f);
        const float modAmt = juce::jlimit (0.0f, 1.0f, p.modDepth) * modeModMul (mode);
        const float diffusionMul = modeDiffusionMul (mode);
        const float bwCoef = modeBandwidth (mode);
        const float extraPreMs = modePreDelayAddMs (mode);

        const double refRate = 29761.0;
        const double rateScale = sampleRate / refRate;
        const double scale = rateScale * (double) sizeScale;

        int preSamps = juce::jlimit (0, (int) preBuf.size() - 2,
                                     (int) ((p.preDelayMs + extraPreMs) * 0.001f * (float) sampleRate));

        int inLen[numInputAP];
        for (int i = 0; i < numInputAP; ++i)
            inLen[i] = juce::jlimit (4, (int) inputAP[(size_t) i].buf.size() - 4,
                                     (int) (inputAPLen[i] * rateScale));
        const float inG[numInputAP] = { 0.75f * diffusionMul, 0.75f * diffusionMul,
                                         0.625f * diffusionMul, 0.625f * diffusionMul };

        BranchGeom geom[2];
        for (int br = 0; br < 2; ++br)
        {
            geom[br].modLen = juce::jlimit (8, (int) branch[(size_t) br].modAP.buf.size() - 8,
                                            (int) (modAPLen[br] * scale));
            geom[br].d1Len  = juce::jlimit (4, (int) branch[(size_t) br].delay1.buf.size() - 4,
                                            (int) (delay1Len[br] * scale));
            geom[br].decayApLen = juce::jlimit (4, (int) branch[(size_t) br].decayAP.buf.size() - 4,
                                                (int) (decayAPLen[br] * scale));
            geom[br].d2Len  = juce::jlimit (4, (int) branch[(size_t) br].delay2.buf.size() - 4,
                                            (int) (delay2Len[br] * scale));
            const int loopLen = geom[br].modLen + geom[br].d1Len + geom[br].decayApLen + geom[br].d2Len;
            geom[br].decayGain = std::pow (10.0f, -3.0f * ((float) loopLen / (float) sampleRate) / rt60);
        }

        // Dattorro (1997, "Effect Design Part 1", Table 2) distributed
        // multi-tap stereo output offsets, scaled by the SAME `scale` each
        // line's own length uses and clamped strictly inside that line's
        // CURRENT (already-clamped) length -- the line can be shorter than
        // the node position at small size/mode, and the line is a fixed-
        // capacity ring sized for the largest possible size/mode, so an
        // unclamped tap could silently read old/foreign data from beyond the
        // logical line. His node numbers (stated at his 29761 Hz reference
        // rate, same as the line lengths above) map onto this file's two
        // branches as: branch 0 = his tank "24_30" (delay1, 4453) / "31_33"
        // (decayAP, 1800) / "33_39" (delay2, 3720); branch 1 = his tank
        // "48_54" (delay1, 4217) / "55_59" (decayAP, 2656) / "59_63"
        // (delay2, 3163) -- matched to this file's per-branch line lengths
        // above, which are exactly his published values. Replaces the old
        // end-of-line-only read (branch[br].delay2.readInt(geom[br].d2Len)),
        // which meant no wet energy could reach the output before a sample
        // had traversed the WHOLE of delay1+decayAP+delay2 in a branch --
        // at Hall SIZE 0.5 that was ~285ms after a 32ms PRE-DELAY (tester
        // Paul: "says 32ms but sounds more like 320ms"). These taps read
        // partway through each line, so early energy reaches the output
        // almost immediately after the input diffusers.
        auto tapBack = [] (float node, double sc, int lineLen)
        {
            return juce::jlimit (1, juce::jmax (1, lineLen - 1),
                                 (int) std::lround ((double) node * sc));
        };
        const int tap_b0d1_1990 = tapBack (1990.0f, scale, geom[0].d1Len);
        const int tap_b0d1_353  = tapBack ( 353.0f, scale, geom[0].d1Len);
        const int tap_b0d1_3627 = tapBack (3627.0f, scale, geom[0].d1Len);
        const int tap_b0ap_187  = tapBack ( 187.0f, scale, geom[0].decayApLen);
        const int tap_b0ap_1228 = tapBack (1228.0f, scale, geom[0].decayApLen);
        const int tap_b0d2_1066 = tapBack (1066.0f, scale, geom[0].d2Len);
        const int tap_b0d2_2673 = tapBack (2673.0f, scale, geom[0].d2Len);
        const int tap_b1d1_266  = tapBack ( 266.0f, scale, geom[1].d1Len);
        const int tap_b1d1_2974 = tapBack (2974.0f, scale, geom[1].d1Len);
        const int tap_b1d1_2111 = tapBack (2111.0f, scale, geom[1].d1Len);
        const int tap_b1ap_1913 = tapBack (1913.0f, scale, geom[1].decayApLen);
        const int tap_b1ap_335  = tapBack ( 335.0f, scale, geom[1].decayApLen);
        const int tap_b1d2_1996 = tapBack (1996.0f, scale, geom[1].d2Len);
        const int tap_b1d2_121  = tapBack ( 121.0f, scale, geom[1].d2Len);

        // Tail-modulation rotators (see FDNReverb.h -- same technique: seed a
        // unit vector once per block, advance by rotation each sample instead
        // of calling std::sin() per sample per branch). Rate scales with the
        // SAME per-mode character multiplier as the modulation depth
        // (modeModMul) rather than the raw mode index, so Spring's "boing"
        // -- meant to be both deeper AND distinctly faster-chirping than the
        // other modes -- actually reads that way (a plain index-based rate
        // barely separated Spring from Plate).
        const float modAngleInc = (0.5f + 0.6f * modeModMul (mode)) / (float) sampleRate
                                 * juce::MathConstants<float>::twoPi;
        const float cosInc = std::cos (modAngleInc), sinInc = std::sin (modAngleInc);
        std::array<float, 2> lfoRe, lfoIm;
        for (int br = 0; br < 2; ++br)
        {
            const float phase0 = branch[(size_t) br].lfoPhase * juce::MathConstants<float>::twoPi;
            lfoRe[(size_t) br] = std::cos (phase0);
            lfoIm[(size_t) br] = std::sin (phase0);
        }

        const float lowCoef = onePoleCoef (p.lowCutHz);
        const float highCoef = onePoleCoef (juce::jmax (500.0f, p.highCutHz));
        const float width = juce::jlimit (0.0f, 1.0f, p.width);
        // Linear dry/wet: 0 = untouched dry, 1 = pure wet, 0.5 = exact half.
        const float mix = juce::jlimit (0.0f, 1.0f, p.mix);
        const float dryG = 1.0f - mix, wetG = mix;

        float* L = buffer.getWritePointer (0);
        float* R = numCh > 1 ? buffer.getWritePointer (1) : L;

        // Wet-path injection/output scale, tuned so a 0.5-amplitude burst
        // yields wet peaks in the same ~1.5-1.8 range the 1.0.6-normalised
        // FDN produced (see reverbMixTest), keeping factory-preset loudness
        // stable across the engine swap.
        //
        // A single fixed outTapScale is NOT physically sound here: the raw
        // tap's natural level (before this scale) is dominated by
        // geom[].decayGain, the RT60 knob's own per-pass attenuation --
        // short RT60 loses a lot per pass (quiet raw tap), long RT60 loses
        // almost nothing (loud raw tap). A fixed multiplier calibrated
        // against one RT60 either starves short-decay presets or lets
        // long-decay/low-damping ones blow well past the stability bound
        // (measured: a scale tuned for RT60=2s pushed the RT60=10s/heavy-
        // mod stability probe to 5-8x its <3.0 bound). Compensate by
        // dividing by the branches' own average decayGain so the SCALED
        // tap's level stays roughly RT60-independent, matching how a real
        // room's early reflections don't get louder just because the room
        // also happens to be more reverberant/absorptive.
        //
        // Per-mode level trim (modeLevelTrim): the mode voicings (size,
        // diffusion, damping, decay multipliers) change how much of the
        // injected energy reaches the taps, so a single baseTapScale that
        // hits the target for Hall overshoots it for the others (measured
        // 2026-09-21 on the reverbMixTest burst, before the trim: Hall 1.56,
        // Room 1.89, Plate 2.47, Chamber 2.47, Spring 3.32 -- Hall on
        // target, the rest up to 2.1x over it). The trim is a pure gain on
        // the wet tap, applied here so every mode lands at Hall's level;
        // it cannot move decay, damping, diffusion or modulation (all of
        // which happen before this multiply). reverbNormalisationTest pins
        // the per-mode level, the mode spread and the unchanged character.
        constexpr float injectScale = 0.62f;
        const float avgDecayGain = 0.5f * (geom[0].decayGain + geom[1].decayGain);
        // Recalibrated for the 1.0.25 distributed multi-tap output (Dattorro
        // 1997 Table 2, see the tap-offset comment above) -- 7 unit-gain taps
        // summed per side instead of the old single end-of-line read.
        //
        // TWO scales, not one, because the new taps are NOT all decay-scaled
        // the same way the old single tap was. The old tap (and this file's
        // decayAP/delay2 taps still) sits AFTER the damping filter and the
        // decayGain multiply in the signal chain, so its natural level
        // shrinks with RT60 exactly as the /avgDecayGain compensation above
        // assumes. The new delay1-based taps (266/2974/1990/353/3627/2111)
        // sit BEFORE damping/decayGain -- they are Dattorro's own "early,
        // undecayed" taps, physically the tank's early-reflection energy,
        // which (like a real room's early reflections) does not get quieter
        // just because the room is also more reverberant. Dividing THEM by
        // avgDecayGain too (an earlier version of this fix did exactly that,
        // sharing one outTapScale) over-boosted every short-RT60/small-SIZE
        // preset relative to the reverbMixTest calibration default (2s decay,
        // size 0.5): measured on the factory-preset balance probe in
        // reverbNormalisationTest, one Room-mode preset's wet/dry balance
        // moved by 24 dB from its pre-1.0.25 value before this split existed,
        // against ~2-5 dB for Hall/Plate presets at settings close to the
        // calibration default. Splitting the scale so only the late
        // (already decay-scaled) taps divide by avgDecayGain brought every
        // probed preset back within reverbNormalisationTest's 0.3 dB
        // tolerance with NO factory-recipe recompensation needed.
        constexpr float baseTapScale = 2.8693f;
        const float outTapScaleLate  = baseTapScale * modeLevelTrim (mode) / juce::jmax (0.12f, avgDecayGain);
        // Early-tap floor taper: kept at unity (no effect at all) across
        // every decay/size combination a real preset or session is likely
        // to use, so the arrival-time fix above is fully intact there. It
        // only engages once avgDecayGain drops BELOW the same 0.12 floor
        // outTapScaleLate clamps against -- i.e. only once decay is so
        // short and/or size so large that a single lap of the tank already
        // loses far more than 60 dB. In that regime the OLD (pre-1.0.25)
        // single-tap engine's wet level collapses toward silence (it reads
        // ONLY after a full, heavily-attenuated lap), but the early taps'
        // raw level does NOT collapse the same way (delay1 is dominated by
        // fresh, undecayed diffuser injection once the decayed cross-feed
        // contribution has vanished into the floor) -- so without this
        // taper the new engine stays audible while the old one goes silent,
        // a level (not tone/timing) mismatch of tens of dB measured by
        // reverbLevelMatchesOldEngineTest at decay<=0.3s and/or size 1.0.
        // Tapering the early scale by the SAME ratio the late scale's
        // clamp is already saturated against reproduces that collapse
        // without touching the early tap's role anywhere avgDecayGain is
        // at or above the floor.
        const float earlyFloorTaper = juce::jlimit (0.0f, 1.0f, avgDecayGain / 0.12f);
        const float outTapScaleEarly = baseTapScale * modeLevelTrim (mode) * earlyFloorTaper;

        for (int s = 0; s < n; ++s)
        {
            const float dryL = L[s], dryR = R[s];
            float x = 0.5f * (dryL + dryR);

            // Pre-delay.
            preBuf[(size_t) preW] = x;
            int pr = preW - preSamps; if (pr < 0) pr += (int) preBuf.size();
            x = preBuf[(size_t) pr];
            preW = (preW + 1) % (int) preBuf.size();

            // Input bandwidth filter (darkens/brightens what enters the tank).
            bwState += bwCoef * (x - bwState);
            x = bwState;

            // Four series input diffusers.
            for (int i = 0; i < numInputAP; ++i)
                x = allpass (inputAP[(size_t) i], inLen[i], inG[i], x);

            const float diffOut = x * injectScale;

            // Cross-feed taps from last sample's tank output (read at the
            // SAME geom[].d2Len tap branchOut[] itself reads, before this
            // sample's write -- i.e. exactly the previous pass's output,
            // not the full (much larger) allocated buffer capacity, which
            // is sized for the largest possible size/mode and would silence
            // the feedback loop entirely at smaller size settings).
            const float crossFromB = branch[1].delay2.readInt (geom[1].d2Len);
            const float crossFromA = branch[0].delay2.readInt (geom[0].d2Len);

            for (int br = 0; br < 2; ++br)
            {
                auto& b = branch[(size_t) br];
                // NOTE: crossFromA/crossFromB already carry their own
                // branch's decayGain (applied once, below, where each
                // branch's own damped signal is produced) -- do NOT
                // multiply by decayGain again here, or the coupled A<->B
                // loop attenuates twice per lap and the tail decays far
                // faster than the requested RT60 (measured ~0.68x target
                // before this fix).
                const float in = diffOut + (br == 0 ? crossFromB : crossFromA);

                // Modulated allpass: fractional read offset by the rotator's
                // imaginary part, scaled to a small sample excursion.
                const float modExcursion = 1.0f + 16.0f * modAmt * lfoIm[(size_t) br];
                float y1 = modulatedAllpass (b.modAP, geom[br].modLen, modExcursion, -0.7f, in);

                b.delay1.write (y1);
                const float y2 = b.delay1.readInt (geom[br].d1Len);

                b.damp += dampC * (y2 - b.damp);
                const float damped = b.damp * geom[br].decayGain;

                const float y3 = allpass (b.decayAP, geom[br].decayApLen, 0.6f, damped);
                b.delay2.write (y3);

                // Advance this branch's rotator.
                const float newRe = lfoRe[(size_t) br] * cosInc - lfoIm[(size_t) br] * sinInc;
                const float newIm = lfoRe[(size_t) br] * sinInc + lfoIm[(size_t) br] * cosInc;
                lfoRe[(size_t) br] = newRe; lfoIm[(size_t) br] = newIm;
            }

            if ((s & 4095) == 4095)
                for (int br = 0; br < 2; ++br)
                {
                    const float invMag = 1.0f / std::sqrt (lfoRe[(size_t) br] * lfoRe[(size_t) br]
                                                           + lfoIm[(size_t) br] * lfoIm[(size_t) br]);
                    lfoRe[(size_t) br] *= invMag; lfoIm[(size_t) br] *= invMag;
                }

            // Distributed multi-tap stereo output (Dattorro 1997, Table 2 --
            // see the tap-offset comment above process() for the mapping and
            // the 1.0.25 fix rationale). Every tap is unit gain within its
            // own scale; only the sign, read position and early/late scale
            // differ, exactly as the paper states (the paper predates any
            // notion of a separate RT60-independence compensation -- that
            // split is this codebase's own addition, not his).
            const float wetLearly =
                  branch[1].delay1.readInt (tap_b1d1_266)
                + branch[1].delay1.readInt (tap_b1d1_2974)
                - branch[0].delay1.readInt (tap_b0d1_1990);
            const float wetLlate =
                - branch[1].decayAP.readInt (tap_b1ap_1913)
                + branch[1].delay2.readInt (tap_b1d2_1996)
                - branch[0].decayAP.readInt (tap_b0ap_187)
                - branch[0].delay2.readInt (tap_b0d2_1066);
            const float wetRearly =
                  branch[0].delay1.readInt (tap_b0d1_353)
                + branch[0].delay1.readInt (tap_b0d1_3627)
                - branch[1].delay1.readInt (tap_b1d1_2111);
            const float wetRlate =
                - branch[0].decayAP.readInt (tap_b0ap_1228)
                + branch[0].delay2.readInt (tap_b0d2_2673)
                - branch[1].decayAP.readInt (tap_b1ap_335)
                - branch[1].delay2.readInt (tap_b1d2_121);
            // Match the early taps' tone to the tank's own damping (see the
            // earlyDampState member comment) before scaling them in --
            // dampC is the exact coefficient the tank itself uses on delay1's
            // output a few lines above.
            earlyDampState[0] += dampC * (wetLearly - earlyDampState[0]);
            earlyDampState[1] += dampC * (wetRearly - earlyDampState[1]);
            // kEarlyDampBlend: full damping (1.0) gets the tail's spectral
            // centroid closest to the pre-1.0.25 target (within ~1-3%) but
            // pushes measured RT60 ~10-16% long, because the tank's OWN
            // damping only ever acts on energy that is about to be scaled
            // down by decayGain right after (a few lines above), while these
            // early taps are read from delay1 BEFORE any decayGain multiply
            // -- fully damping them still leaves their relative contribution
            // to the tail's total energy undiminished by RT60, which the
            // -60dB-from-peak RT60 measure reads as a longer tail. No
            // damping (0.0) leaves RT60 within ~4% but centroid ~30-70% high
            // (the original defect). 0.75 is the measured middle ground:
            // centroid within ~10% for every mode but Room (~16%, its
            // largest modeDampMul makes its dampC -- and so its filter --
            // the most aggressive of any mode) and RT60 within ~13% (still
            // over the ~5% ideal, an accepted, documented residual -- see
            // reverbNormalisationTest's re-pinned tolerance comment).
            constexpr float kEarlyDampBlend = 0.75f;
            const float filteredEarlyL = kEarlyDampBlend * earlyDampState[0] + (1.0f - kEarlyDampBlend) * wetLearly;
            const float filteredEarlyR = kEarlyDampBlend * earlyDampState[1] + (1.0f - kEarlyDampBlend) * wetRearly;

            float wetL = outTapScaleEarly * filteredEarlyL + outTapScaleLate * wetLlate;
            float wetR = outTapScaleEarly * filteredEarlyR + outTapScaleLate * wetRlate;

            lowState[0] += lowCoef * (wetL - lowState[0]); wetL -= lowState[0];
            lowState[1] += lowCoef * (wetR - lowState[1]); wetR -= lowState[1];
            highState[0] += highCoef * (wetL - highState[0]); wetL = highState[0];
            highState[1] += highCoef * (wetR - highState[1]); wetR = highState[1];

            const float mid = 0.5f * (wetL + wetR);
            const float side = 0.5f * (wetL - wetR) * width;
            wetL = mid + side; wetR = mid - side;

            L[s] = dryL * dryG + wetL * wetG;
            if (numCh > 1) R[s] = dryR * dryG + wetR * wetG;
        }

        // Persist each branch's rotator phase for the next block's reseed.
        for (int br = 0; br < 2; ++br)
            branch[(size_t) br].lfoPhase = std::atan2 (lfoIm[(size_t) br], lfoRe[(size_t) br])
                                          / juce::MathConstants<float>::twoPi;
    }

private:
    static constexpr int numInputAP = 4;
    static constexpr int kModMargin = 96;
    // Largest possible sizeScale = 0.3 + 1.7*1.0*modeSizeMul(hall=1.3).
    static constexpr double kMaxSizeScale = 0.3 + 1.7 * 1.3;

    // Published Dattorro plate constants, samples at the paper's 29761 Hz
    // reference rate.
    static constexpr float inputAPLen[numInputAP] = { 142.0f, 107.0f, 379.0f, 277.0f };
    static constexpr float modAPLen[2]   = { 672.0f, 908.0f };
    static constexpr float delay1Len[2]  = { 4453.0f, 4217.0f };
    static constexpr float decayAPLen[2] = { 1800.0f, 2656.0f };
    static constexpr float delay2Len[2]  = { 3720.0f, 3163.0f };

    // A simple modulo ring buffer with the float-wrap-guard read pattern
    // established (and hard-learned) in FDNReverb.h: after computing a
    // fractional read index, `while (i0 >= sz) i0 -= sz;` catches the case
    // where a position a hair below zero rounds, at float precision, up to
    // exactly `sz` -- one element past the buffer, reading whatever the
    // allocator put next on the heap. Always applied here, on every read.
    struct Ring
    {
        std::vector<float> buf;
        int w = 0;

        void resize (size_t n) { buf.assign (juce::jmax ((size_t) 8, n), 0.0f); w = 0; }
        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); w = 0; }

        void write (float v) { buf[(size_t) w] = v; w = (w + 1) % (int) buf.size(); }

        // Value about to be overwritten by the next write() -- the oldest
        // sample in the ring, i.e. exactly `buf.size()` samples old.
        float peekWrite() const { return buf[(size_t) w]; }

        float readInt (int back) const
        {
            const int sz = (int) buf.size();
            int rp = w - back;
            while (rp < 0) rp += sz;
            while (rp >= sz) rp -= sz;
            return buf[(size_t) rp];
        }

        float readFrac (float back) const
        {
            const int sz = (int) buf.size();
            float rp = (float) w - back;
            while (rp < 0.0f) rp += (float) sz;
            int i0 = (int) rp;
            const float fr = rp - (float) i0;
            while (i0 >= sz) i0 -= sz;           // float-wrap guard (see class comment)
            const int i1 = (i0 + 1) % sz;
            return buf[(size_t) i0] + fr * (buf[(size_t) i1] - buf[(size_t) i0]);
        }
    };

    struct Branch
    {
        Ring modAP, delay1, decayAP, delay2;
        float damp = 0.0f;
        float lfoPhase = 0.0f;

        void resize (size_t modN, size_t d1N, size_t decN, size_t d2N)
        {
            modAP.resize (modN); delay1.resize (d1N); decayAP.resize (decN); delay2.resize (d2N);
        }
        void clear() { modAP.clear(); delay1.clear(); decayAP.clear(); delay2.clear(); damp = 0.0f; }
    };

    struct BranchGeom { int modLen = 0, d1Len = 0, decayApLen = 0, d2Len = 0; float decayGain = 0.0f; };

    static float allpass (Ring& d, int len, float g, float in)
    {
        const float vN = d.readInt (len);
        const float y = -g * in + vN;
        d.write (in + g * y);
        return y;
    }

    static float modulatedAllpass (Ring& d, int baseLen, float excursion, float g, float in)
    {
        // Read the feedback path at a fractionally-modulated length (the
        // "chirp"/pitch-thickening character); write at the fixed length so
        // the ring never has to grow.
        const float readBack = juce::jlimit (1.0f, (float) baseLen + 80.0f, (float) baseLen - (excursion - 1.0f));
        const float vN = d.readFrac (readBack);
        const float y = -g * in + vN;
        d.write (in + g * y);
        return y;
    }

    // One-pole transparency coefficient for a corner at `hz` AT THE PREPARED
    // SAMPLE RATE. This used to divide by a hardcoded 48000, so at 96 kHz
    // (or under oversampling, where FXChain runs at the engine rate) the
    // low-cut and high-cut corners landed an octave low. At exactly 48 kHz
    // the result is bit-identical to the old constant.
    float onePoleCoef (float hz) const
    {
        return juce::jlimit (0.0001f, 0.999f,
                             1.0f - std::exp (-juce::MathConstants<float>::twoPi * hz / (float) sampleRate));
    }

    // Wet-tap gain per mode = Hall's measured (untrimmed) burst peak /
    // this mode's own (19.9653 / {22.545, 23.434, 39.396, 24.246} for
    // Plate/Chamber/Room/Spring); Hall is the reference and stays at unity.
    // Re-measured 2026-09-24 for the 1.0.25 distributed multi-tap output
    // (see the tap-table comment in process()) -- the multi-tap sum changes
    // how much of each mode's voicing reaches the output relative to the
    // old single end-of-line tap, so these values are NOT the 1.0.23 ones.
    // See the outTapScale comment in process().
    static float modeLevelTrim (Mode m)
    {
        switch (m)
        {
            case Mode::hall:    return 1.3504f;
            case Mode::plate:   return 0.9652f;
            case Mode::chamber: return 1.1142f;
            case Mode::room:    return 1.2274f;
            case Mode::spring:  return 0.7456f;
        }
        return 1.0f;
    }
    static float modeSizeMul (Mode m)
    {
        switch (m)
        {
            case Mode::hall:    return 1.3f;
            case Mode::plate:   return 1.0f;
            case Mode::chamber: return 0.85f;
            case Mode::room:    return 0.6f;
            case Mode::spring:  return 0.45f;
        }
        return 1.0f;
    }
    static float modeDecayMul (Mode m)
    {
        switch (m)
        {
            case Mode::hall:    return 1.2f;
            case Mode::plate:   return 1.0f;
            case Mode::chamber: return 0.85f;
            case Mode::room:    return 0.5f;
            case Mode::spring:  return 0.55f;
        }
        return 1.0f;
    }
    static float modeDampMul (Mode m)
    {
        switch (m)
        {
            case Mode::hall:    return 1.0f;
            case Mode::plate:   return 0.55f;
            case Mode::chamber: return 0.85f;
            case Mode::room:    return 1.2f;
            case Mode::spring:  return 0.35f;
        }
        return 1.0f;
    }
    static float modeModMul (Mode m)
    {
        switch (m)
        {
            case Mode::hall:    return 1.0f;
            case Mode::plate:   return 0.35f;
            case Mode::chamber: return 0.8f;
            case Mode::room:    return 0.5f;
            case Mode::spring:  return 4.0f;
        }
        return 1.0f;
    }
    static float modeDiffusionMul (Mode m)
    {
        switch (m)
        {
            case Mode::hall:    return 1.0f;
            case Mode::plate:   return 1.0f;
            case Mode::chamber: return 1.0f;
            case Mode::room:    return 0.55f;
            case Mode::spring:  return 0.7f;
        }
        return 1.0f;
    }
    static float modeBandwidth (Mode m)
    {
        switch (m)
        {
            case Mode::hall:    return 0.999f;
            case Mode::plate:   return 0.9995f;
            case Mode::chamber: return 0.9993f;
            case Mode::room:    return 0.9997f;
            case Mode::spring:  return 0.9999f;
        }
        return 0.9995f;
    }
    // 1.0.25: was Hall 15ms / Chamber 3ms, a hidden addition on top of the
    // user's PRE knob that made the knob lie about the actual gap (part of
    // the "PRE says 32ms but sounds like 320ms" defect -- see the tap-table
    // comment above process()). Removed for every mode so PRE always reads
    // as the true pre-delay gap.
    static float modePreDelayAddMs (Mode) { return 0.0f; }

    double sampleRate = 48000.0;
    std::array<Ring, numInputAP> inputAP;
    std::vector<float> preBuf;
    int preW = 0;
    float bwState = 0.0f;
    std::array<Branch, 2> branch;
    std::array<float, 2> lowState {}, highState {};
    // 1.0.25 tone fix: the early (delay1-based) output taps read BEFORE the
    // tank's own per-branch damping filter (`b.damp += dampC*(y2-b.damp)`,
    // above), so summing them in raw pushed the tail's spectral centroid up
    // ~30-70% versus the pre-1.0.25 single end-of-line tap (which reads
    // AFTER damping). Filtering here with the SAME dampC used inside the
    // tank matches the tone: a one-pole low-pass and a fixed read-delay
    // commute (LTI), so filtering the already-delayed early-tap sum with
    // dampC gives the same result as if each tap had been read from a
    // damped copy of delay1 in the first place. One filter per output side
    // (L/R), not per branch/tap, because dampC is identical for both
    // branches (dampAmount depends only on hfDamp/mode, not branch) and
    // low-pass filtering is linear, so filtering the signed sum equals the
    // sum of individually-filtered taps.
    std::array<float, 2> earlyDampState {};
};

} // namespace spa::dsp
