#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

#include "LfoDivisions.h"

// Standalone FX parameter registry for SPAStrip.
//
// Every effect ParamDef here is lifted from SPASynth's ParameterRegistry with
// IDENTICAL parameter IDs, display names, ranges, intervals, skews, defaults,
// units, choice lists (and their order), percentDisplay flags and RandomSpec
// values, so SPASynth FX state can later be imported by ID. Synth-only
// concepts (mod-destination budget, synth sections, lock groups, the matrix)
// are dropped. The four `global.*` parameters are new to SPAStrip.

namespace spa::params
{

// Sections drive host parameter groups (one group per effect). Order matches
// SPASynth's allSections order for the FX sections.
enum class Section
{
    global,
    fxDist,
    fxChorus,
    fxDelay,
    fxReverb,
    fxEQ,
    fxMod,
    fxTremVib,
    fxLimiter,
    fxConvolve,
    fxComp,
    fxGrain,
    // SPAStrip phase 2: sidechain detector + modulation matrix (not effects).
    sidechain,
    modMatrix,
    // SPAStrip: FILTER (two SVF filters, ported from SPASynth's filter section).
    // Its parameters sit right after GRAIN in the registry (so the Randomize
    // draw order of every existing parameter is unchanged), but its host
    // parameter GROUP is last in allSections, so no existing parameter's host
    // index moves.
    fxFilter,
    // Comp rebuild: per-band knee / solo / bypass. Its own host group, last,
    // so no existing parameter's host index moves.
    fxCompBands,
};

inline constexpr Section allSections[] = {
    Section::global, Section::fxDist, Section::fxChorus,
    Section::fxDelay, Section::fxReverb, Section::fxEQ, Section::fxMod,
    Section::fxTremVib, Section::fxLimiter, Section::fxConvolve, Section::fxComp,
    Section::fxGrain,
    Section::sidechain, Section::modMatrix,
    Section::fxFilter,
    Section::fxCompBands,
};

juce::String sectionName (Section);

// Randomization metadata (kept for the later UI's RANDOMIZE). Ranges are in
// normalized (0..1) parameter space. biasStrength 0 = uniform across
// [minNorm, maxNorm]; 1 = tightly clustered around biasCentre.
struct RandomSpec
{
    bool enabled = true;
    float minNorm = 0.0f;
    float maxNorm = 1.0f;
    float biasCentre = 0.5f;
    float biasStrength = 0.0f;
};

enum class ParamKind { floatParam, intParam, boolParam, choiceParam };

struct ParamDef
{
    juce::String id;
    juce::String name;
    Section section;
    ParamKind kind = ParamKind::floatParam;
    juce::NormalisableRange<float> range;    // floatParam only
    float defaultValue = 0.0f;               // also holds int/bool/choice defaults
    juce::String unit;
    RandomSpec random {};
    juce::StringArray choices {};            // choiceParam only
    // 0..1-range floatParams that read best as a percentage in the UI/host
    // automation lane (the FX MIX knobs) -- display-only, the stored/
    // automated range is unchanged (still 0..1).
    bool percentDisplay = false;
};

// Stable parameter IDs. Everything refers to parameters through these, never
// through ad hoc string literals.
namespace id
{
    // SPAStrip globals (new; randomization disabled).
    inline constexpr const char* inputGain    = "global.inputGain";     // -24..+24 dB
    inline constexpr const char* outputGain   = "global.outputGain";    // -24..+24 dB
    inline constexpr const char* mix          = "global.mix";           // 0..1, whole-chain dry/wet
    inline constexpr const char* oversampling = "global.oversampling";  // 1x/2x/4x

    // Sidechain envelope follower (phase 2; randomization disabled).
    namespace sc
    {
        inline constexpr const char* source  = "sc.source";   // External / Input
        inline constexpr const char* gain    = "sc.gain";     // -24..+24 dB detector sensitivity
        inline constexpr const char* attack  = "sc.attack";   // ms
        inline constexpr const char* release = "sc.release";  // ms
        inline constexpr const char* hpf     = "sc.hpf";      // Hz, detector high-pass
        inline constexpr const char* listen  = "sc.listen";   // output the detector signal
    }

    // Modulation matrix: 8 slots, bipolar depth. The slot TARGETS are plugin
    // state, not host parameters (see source/mod/ModTargets.h).
    inline constexpr int numModSlots = 8;
    juce::String modSlotDepth (int slot);   // slot 0..7 -> "mod.slot1.depth" ...

    // Parametric-EQ band IDs: eqBand(0, "freq") -> "fxEQ.band0.freq"
    juce::String eqBand (int band, const char* key);

    // COMP band IDs: compBand(0, "thresh") -> "fxComp.band0.thresh" (0 low, 1 mid, 2 high)
    juce::String compBand (int band, const char* key);

    // FX chain (identical IDs to SPASynth).
    namespace fx
    {
        inline constexpr const char* distEnable = "fxDist.enable";
        inline constexpr const char* distType   = "fxDist.type";
        inline constexpr const char* distDrive  = "fxDist.drive";
        inline constexpr const char* distTone   = "fxDist.tone";
        inline constexpr const char* distMix    = "fxDist.mix";

        inline constexpr const char* chorusEnable   = "fxChorus.enable";
        inline constexpr const char* chorusRate     = "fxChorus.rate";
        inline constexpr const char* chorusDepth    = "fxChorus.depth";
        inline constexpr const char* chorusFeedback = "fxChorus.feedback";
        inline constexpr const char* chorusWidth    = "fxChorus.width";
        inline constexpr const char* chorusMode     = "fxChorus.mode";   // Vintage/Modern/VHS
        inline constexpr const char* chorusMix      = "fxChorus.mix";
        // VHS mode only (SPASynth 1.0.32), percentages.
        inline constexpr const char* chorusVhsWow      = "fxChorus.vhsWow";
        inline constexpr const char* chorusVhsFlutter  = "fxChorus.vhsFlutter";
        inline constexpr const char* chorusVhsTone     = "fxChorus.vhsTone";
        inline constexpr const char* chorusVhsSat      = "fxChorus.vhsSat";
        inline constexpr const char* chorusVhsHiss     = "fxChorus.vhsHiss";
        inline constexpr const char* chorusVhsDropouts = "fxChorus.vhsDropouts";

        inline constexpr const char* delayEnable   = "fxDelay.enable";
        inline constexpr const char* delaySync     = "fxDelay.sync";
        inline constexpr const char* delayTime     = "fxDelay.time";
        inline constexpr const char* delayDivision = "fxDelay.division";
        inline constexpr const char* delayFeedback = "fxDelay.feedback";
        inline constexpr const char* delayPingPong = "fxDelay.pingpong";
        inline constexpr const char* delayWidth    = "fxDelay.width";
        inline constexpr const char* delayMix      = "fxDelay.mix";

        inline constexpr const char* reverbEnable   = "fxReverb.enable";
        inline constexpr const char* reverbMode     = "fxReverb.mode";
        inline constexpr const char* reverbPreDelay = "fxReverb.predelay";
        inline constexpr const char* reverbSize     = "fxReverb.size";
        inline constexpr const char* reverbDecay    = "fxReverb.decay";
        inline constexpr const char* reverbDamping  = "fxReverb.damping";
        inline constexpr const char* reverbModDepth = "fxReverb.moddepth";
        inline constexpr const char* reverbLowCut   = "fxReverb.lowcut";
        inline constexpr const char* reverbHighCut  = "fxReverb.highcut";
        inline constexpr const char* reverbWidth    = "fxReverb.width";
        inline constexpr const char* reverbMix      = "fxReverb.mix";

        inline constexpr const char* eqEnable    = "fxEQ.enable";
        inline constexpr const char* eqCharacter = "fxEQ.character";

        // Per-band parametric-EQ keys, combined via eqBand(band, key) ->
        // "fxEQ.band0.freq" etc.
        namespace eqband
        {
            inline constexpr const char* enable = "enable";
            inline constexpr const char* type   = "type";   // Bell/LoShelf/HiShelf/LoCut/HiCut/Notch/BandPass/Tilt
            inline constexpr const char* slope  = "slope";  // Low Cut / High Cut only: 6-48 dB/oct
            inline constexpr const char* freq   = "freq";
            inline constexpr const char* gain   = "gain";
            inline constexpr const char* q      = "q";
        }

        inline constexpr const char* modEnable   = "fxMod.enable";
        inline constexpr const char* modType     = "fxMod.type";
        inline constexpr const char* modRate     = "fxMod.rate";
        inline constexpr const char* modSync     = "fxMod.sync";
        inline constexpr const char* modDivision = "fxMod.division";
        inline constexpr const char* modDepth    = "fxMod.depth";
        inline constexpr const char* modFeedback = "fxMod.feedback";
        inline constexpr const char* modStages   = "fxMod.stages";
        inline constexpr const char* modCentre   = "fxMod.centre";
        inline constexpr const char* modManual   = "fxMod.manual";
        inline constexpr const char* modWidth    = "fxMod.width";
        inline constexpr const char* modMix      = "fxMod.mix";

        inline constexpr const char* tremEnable   = "fxTrem.enable";
        inline constexpr const char* tremRate     = "fxTrem.rate";
        inline constexpr const char* tremSync     = "fxTrem.sync";
        inline constexpr const char* tremDivision = "fxTrem.division";
        inline constexpr const char* tremDepth    = "fxTrem.depth";
        inline constexpr const char* tremShape    = "fxTrem.shape";
        inline constexpr const char* tremStereo   = "fxTrem.stereo";
        inline constexpr const char* tremMix      = "fxTrem.mix";
        inline constexpr const char* vibEnable    = "fxVib.enable";
        inline constexpr const char* vibRate      = "fxVib.rate";
        inline constexpr const char* vibSync      = "fxVib.sync";
        inline constexpr const char* vibDivision  = "fxVib.division";
        inline constexpr const char* vibDepth     = "fxVib.depth";
        inline constexpr const char* vibMix       = "fxVib.mix";

        inline constexpr const char* limEnable      = "fxLim.enable";
        inline constexpr const char* limDrive       = "fxLim.drive";
        inline constexpr const char* limCeiling     = "fxLim.ceiling";
        inline constexpr const char* limRelease     = "fxLim.release";
        inline constexpr const char* limAutoRelease = "fxLim.autoRelease";
        inline constexpr const char* limCharacter   = "fxLim.character";
        inline constexpr const char* limStereoLink  = "fxLim.stereoLink";
        inline constexpr const char* limTruePeak    = "fxLim.truePeak";
        inline constexpr const char* limLookahead   = "fxLim.lookahead";
        inline constexpr const char* limAutoGain     = "fxLim.autoGain";

        inline constexpr const char* convEnable   = "fxConv.enable";
        inline constexpr const char* convMix      = "fxConv.mix";
        inline constexpr const char* convWidth    = "fxConv.width";
        inline constexpr const char* convPreDelay = "fxConv.predelay";
        inline constexpr const char* convDecay    = "fxConv.decay";
        inline constexpr const char* convDamping  = "fxConv.damping";
        // Appended at the end of the registry (see ParameterRegistry.cpp) --
        // not a mod destination, so this doesn't disturb dest-index order.
        inline constexpr const char* convStart    = "fxConv.start";

        // COMP (1.0.29): SPAGlitch's three-band OTT-style compressor, same
        // parameter set, ranges and defaults. Per-band keys combine via
        // compBand(band, key) -> "fxComp.band0.thresh".
        inline constexpr const char* compEnable    = "fxComp.enable";
        inline constexpr const char* compMix       = "fxComp.mix";
        inline constexpr const char* compXoverLow  = "fxComp.xoverLow";
        inline constexpr const char* compXoverHigh = "fxComp.xoverHigh";
        namespace compband
        {
            inline constexpr const char* threshold = "thresh";
            inline constexpr const char* ratio     = "ratio";
            inline constexpr const char* upRatio   = "upratio";
            inline constexpr const char* attack    = "attack";
            inline constexpr const char* release   = "release";
            inline constexpr const char* gain      = "gain";
            inline constexpr const char* knee      = "knee";     // comp rebuild
            inline constexpr const char* solo      = "solo";     // comp rebuild
            inline constexpr const char* bypass    = "bypass";   // comp rebuild
        }

        // GRAIN (1.0.29): granular delay / texture.
        inline constexpr const char* grainEnable   = "fxGrain.enable";
        inline constexpr const char* grainSize     = "fxGrain.size";
        inline constexpr const char* grainDensity  = "fxGrain.density";
        inline constexpr const char* grainSync     = "fxGrain.sync";
        inline constexpr const char* grainDivision = "fxGrain.division";
        inline constexpr const char* grainPitch    = "fxGrain.pitch";
        inline constexpr const char* grainSpread   = "fxGrain.spread";        // SPREAD TIME
        inline constexpr const char* grainSpreadPitch = "fxGrain.spreadPitch"; // 1.0.30
        inline constexpr const char* grainPosition = "fxGrain.position";
        inline constexpr const char* grainReverse  = "fxGrain.reverse";
        inline constexpr const char* grainFeedback = "fxGrain.feedback";
        inline constexpr const char* grainMix      = "fxGrain.mix";
        inline constexpr const char* grainFreeze   = "fxGrain.freeze";
        inline constexpr const char* grainRelease  = "fxGrain.release";   // SPASynth 1.0.31, replaces FREEZE in the UI

        // FILTER (SPAStrip): two filters, Series / Parallel. fxFilter.enable is
        // FILTER 1's own on/off (the module header's first toggle, default OFF
        // here although the synth's FILTER 1 defaults on); filter 2 has its own.
        // The synth's keytrack / env-amount are synth-only and not ported.
        // New IDs on purpose: nothing maps across from a SPASynth preset.
        inline constexpr const char* filterEnable   = "fxFilter.enable";
        inline constexpr const char* filterRouting  = "fxFilter.routing";   // Series / Parallel
        inline constexpr const char* filter1Type    = "fxFilter.f1.type";
        inline constexpr const char* filter1Cutoff  = "fxFilter.f1.cutoff";
        inline constexpr const char* filter1Res     = "fxFilter.f1.resonance";
        inline constexpr const char* filter1Drive   = "fxFilter.f1.drive";
        inline constexpr const char* filter1Mix     = "fxFilter.f1.mix";
        inline constexpr const char* filter2Enable  = "fxFilter.f2.enable";
        inline constexpr const char* filter2Type    = "fxFilter.f2.type";
        inline constexpr const char* filter2Cutoff  = "fxFilter.f2.cutoff";
        inline constexpr const char* filter2Res     = "fxFilter.f2.resonance";
        inline constexpr const char* filter2Drive   = "fxFilter.f2.drive";
        inline constexpr const char* filter2Mix     = "fxFilter.f2.mix";
    }
}

// GLITTER RELEASE (ported from SPASynth 1.0.31): stored value is seconds.
// 0 = off, 0.1..30 s on a log taper, and the top of travel
// (grainReleaseInfinite) = hold forever. A custom range so the knob has a
// sticky OFF at the bottom and a sticky infinity at the top.
inline constexpr float grainReleaseInfinite = 31.0f;
juce::NormalisableRange<float> grainReleaseRange();

const std::vector<ParamDef>& all();
const ParamDef* find (const juce::String& paramID);

// Number of effect parameters (the fx* sections only: not global, not sidechain/mod). SPASynth's registry holds the same
// number of fx*-section parameters (checked by the parity test).
int numFxParams();

juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

} // namespace spa::params
