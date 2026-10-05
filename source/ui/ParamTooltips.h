#pragma once

#include <juce_core/juce_core.h>
#include <utility>

namespace spa::ui
{

// One place for the hover text of every parameter-bound control. Knob, Choice
// and Toggle (Controls.h) look their parameter up here when they are built, so
// a new control gets its tooltip without any per-panel code; a panel that
// calls setTooltip itself afterwards still wins. Per-band families (EQ, COMP)
// and the mod slots are matched by the key after the band/slot part of the ID.
// Returns an empty string for an unknown ID (no tooltip rather than a wrong one).
inline juce::String tooltipFor (const juce::String& paramID)
{
    static const std::pair<const char*, const char*> exact[] {
        // Global
        { "global.inputGain",    "Input gain: level into the effect chain." },
        { "global.outputGain",   "Output gain: level after the effect chain." },
        { "global.mix",          "Mix: balance between the dry input and the whole processed chain." },
        { "global.oversampling", "Oversampling of the whole chain. Higher reduces aliasing from distortion and limiting, at more CPU." },

        // Sidechain detector
        { "sc.source",  "Detector source. External: the plugin's sidechain input. Input: the plugin's own main input." },
        { "sc.gain",    "Detector gain: how hard the sidechain signal drives the modulation envelope." },
        { "sc.attack",  "Detector attack: how fast the envelope rises when the sidechain gets louder." },
        { "sc.release", "Detector release: how fast the envelope falls when the sidechain gets quieter." },
        { "sc.hpf",     "Detector high-pass: ignore low end in the sidechain (e.g. follow the snare, not the kick)." },
        { "sc.listen",  "Listen: replace the output with the detector signal, to hear what drives the envelope." },

        // Distortion
        { "fxDist.enable", "Turn the distortion on or off." },
        { "fxDist.type",   "Distortion type. Soft: warm saturation. Hard: clipping. Fold: wavefolding. Crush: bit and rate reduction." },
        { "fxDist.drive",  "Drive: how hard the signal is pushed into the distortion." },
        { "fxDist.tone",   "Tone: darker to brighter distortion." },
        { "fxDist.mix",    "Mix: balance between the clean and distorted signal." },

        // Chorus
        { "fxChorus.enable",   "Turn the chorus on or off." },
        { "fxChorus.rate",     "Rate: speed of the chorus modulation." },
        { "fxChorus.depth",    "Depth: how far the chorus voices drift in pitch." },
        { "fxChorus.feedback", "Feedback: feeds the chorus back into itself for a more metallic sound." },
        { "fxChorus.width",    "Width: stereo spread of the chorus." },
        { "fxChorus.mode",     "Mode. Vintage: darker, BBD-style. Modern: clean and bright. VHS: a worn 80s tape, with wobble, hiss and a little grit." },
        { "fxChorus.mix",      "Mix: balance between the dry and chorused signal." },

        // Delay
        { "fxDelay.enable",   "Turn the delay on or off." },
        { "fxDelay.sync",     "Sync: set the delay time in note values locked to the host tempo." },
        { "fxDelay.time",     "Delay time in milliseconds (used when Sync is off)." },
        { "fxDelay.division", "Delay time as a note value (used when Sync is on)." },
        { "fxDelay.feedback", "Feedback: how many repeats. Higher values ring out longer." },
        { "fxDelay.pingpong", "Ping-pong: repeats bounce between left and right." },
        { "fxDelay.width",    "Width: stereo spread of the repeats." },
        { "fxDelay.mix",      "Mix: balance between the dry signal and the echoes." },

        // Reverb
        { "fxReverb.enable",   "Turn the reverb on or off." },
        { "fxReverb.mode",     "Reverb type: Hall, Plate, Chamber, Room or Spring." },
        { "fxReverb.predelay", "Pre-delay: gap before the reverb starts, keeps the dry signal clear." },
        { "fxReverb.size",     "Size: how big the space sounds." },
        { "fxReverb.decay",    "Decay: how long the reverb tail lasts." },
        { "fxReverb.damping",  "Damping: how quickly high frequencies die away in the tail." },
        { "fxReverb.moddepth", "Modulation: gentle movement in the tail to avoid metallic ringing." },
        { "fxReverb.lowcut",   "Low cut: removes low end from the reverb only." },
        { "fxReverb.highcut",  "High cut: removes top end from the reverb only." },
        { "fxReverb.width",    "Width: stereo spread of the reverb." },
        { "fxReverb.mix",      "Mix: balance between the dry signal and the reverb." },

        // EQ
        { "fxEQ.enable",    "Turn the EQ on or off." },
        { "fxEQ.character", "EQ character. Clean: transparent. Modern, Vintage, Tube: increasingly coloured analogue-style response." },

        // Phaser / Flanger
        { "fxMod.enable",   "Turn the phaser / flanger on or off." },
        { "fxMod.type",     "Phaser or Flanger." },
        { "fxMod.rate",     "Rate: speed of the sweep (used when Sync is off)." },
        { "fxMod.sync",     "Sync: lock the sweep to the host tempo." },
        { "fxMod.division", "Sweep speed as a note value (used when Sync is on)." },
        { "fxMod.depth",    "Depth: how wide the sweep moves." },
        { "fxMod.feedback", "Feedback: resonance of the sweep. Negative and positive values give different colours." },
        { "fxMod.stages",   "Stages: number of phaser stages. More stages give more notches." },
        { "fxMod.centre",   "Centre: the frequency the phaser sweeps around." },
        { "fxMod.manual",   "Delay: the base delay time the flanger sweeps around." },
        { "fxMod.width",    "Width: stereo offset between left and right sweeps." },
        { "fxMod.mix",      "Mix: balance between the dry and swept signal." },

        // Tremolo / Vibrato
        { "fxTrem.enable",   "Turn the tremolo on or off." },
        { "fxTrem.rate",     "Tremolo rate (used when Sync is off)." },
        { "fxTrem.sync",     "Sync: lock the tremolo to the host tempo." },
        { "fxTrem.division", "Tremolo rate as a note value (used when Sync is on)." },
        { "fxTrem.depth",    "Depth: how much the volume dips." },
        { "fxTrem.shape",    "Shape of the volume wobble: Sine, Triangle, Square or Saw." },
        { "fxTrem.stereo",   "Stereo: offsets left and right for an auto-pan effect." },
        { "fxTrem.mix",      "Mix: balance between the dry and tremolo signal." },
        { "fxVib.enable",    "Turn the vibrato on or off." },
        { "fxVib.rate",      "Vibrato rate (used when Sync is off)." },
        { "fxVib.sync",      "Sync: lock the vibrato to the host tempo." },
        { "fxVib.division",  "Vibrato rate as a note value (used when Sync is on)." },
        { "fxVib.depth",     "Depth: how far the pitch wobbles." },
        { "fxVib.mix",       "Mix: balance between the dry and vibrato signal." },

        // Limiter
        { "fxLim.enable",      "Turn the limiter on or off." },
        { "fxLim.drive",       "Drive: gain into the limiter. More drive means louder and more limited." },
        { "fxLim.ceiling",     "Ceiling: the output never goes above this level." },
        { "fxLim.release",     "Release: how fast the limiter lets go after a peak." },
        { "fxLim.autoRelease", "Auto release: adapts the release to the material." },
        { "fxLim.character",   "Character. Clean: transparent. Punchy: keeps transients. Aggressive: dense and loud." },
        { "fxLim.stereoLink",  "Stereo link: how much the left and right channels are limited together." },
        { "fxLim.truePeak",    "True peak: also catches peaks between samples, for streaming-safe output." },
        { "fxLim.lookahead",   "Lookahead: lets the limiter see peaks coming. Smoother, adds latency." },
        { "fxLim.autoGain",    "Auto gain: compensates the drive at the output, so you hear the limiting, not just the level change." },

        // Convolve
        { "fxConv.enable",   "Turn the convolution on or off." },
        { "fxConv.mix",      "Mix: balance between the dry signal and the impulse response." },
        { "fxConv.width",    "Width: stereo spread of the convolved signal." },
        { "fxConv.predelay", "Pre-delay: gap before the impulse response starts." },
        { "fxConv.decay",    "Decay: shortens the impulse response tail." },
        { "fxConv.damping",  "Damping: darkens the impulse response over time." },
        { "fxConv.start",    "Start: skips into the impulse response, trimming its beginning." },

        // Compressor (global)
        { "fxComp.enable",    "Turn the multiband compressor on or off." },
        { "fxComp.mix",       "Mix: blend of dry and compressed signal (parallel compression)." },
        { "fxComp.xoverLow",  "Low crossover: split frequency between the Low and Mid bands. Also draggable on the graph." },
        { "fxComp.xoverHigh", "High crossover: split frequency between the Mid and High bands. Also draggable on the graph." },

        // Glitter (granular)
        { "fxGrain.enable",      "Turn Glitter on or off." },
        { "fxGrain.size",        "Size: length of each grain." },
        { "fxGrain.density",     "Density: how many grains play per second (used when Sync is off)." },
        { "fxGrain.sync",        "Sync: trigger grains in time with the host tempo." },
        { "fxGrain.division",    "Grain rate as a note value (used when Sync is on)." },
        { "fxGrain.pitch",       "Pitch: transposes the grains in semitones." },
        { "fxGrain.spread",      "Spread time: randomises each grain's start, timing and stereo position." },
        { "fxGrain.spreadPitch", "Spread pitch: randomises each grain's pitch." },
        { "fxGrain.position",    "Position: how far back in the recent audio the grains read from." },
        { "fxGrain.reverse",     "Reverse: chance of a grain playing backwards." },
        { "fxGrain.feedback",    "Feedback: feeds the grains back in for evolving textures." },
        { "fxGrain.mix",         "Mix: balance between the dry signal and the grains." },
        { "fxGrain.release",     "Release: how long the cloud takes to fade away once the sound stops. Off leaves it to Feedback; all the way up holds what it has heard, for ever." },

        // Filter
        { "fxFilter.enable",       "Turn the filters on or off." },
        { "fxFilter.routing",      "Series: filter 2 filters the output of filter 1. Parallel: both filter the input and are summed." },
        { "fxFilter.f2.enable",    "Turn filter 2 on or off." },
    };

    for (const auto& [id, text] : exact)
        if (paramID == id)
            return text;

    // Filter 1 / 2 share their controls.
    if (paramID.startsWith ("fxFilter.f"))
    {
        const auto key = paramID.fromLastOccurrenceOf (".", false, false);
        if (key == "type")      return "Filter mode and slope: LP / HP / BP / Notch, 12 or 24 dB per octave.";
        if (key == "cutoff")    return "Cutoff or centre frequency.";
        if (key == "resonance") return "Resonance: emphasis at the cutoff.";
        if (key == "drive")     return "Drive: saturation in front of the filter.";
        if (key == "mix")       return "Mix: balance between the dry and filtered signal.";
    }

    if (paramID.startsWith ("fxEQ.band"))
    {
        const auto key = paramID.fromLastOccurrenceOf (".", false, false);
        if (key == "enable") return "Turn this EQ band on or off.";
        if (key == "type")   return "Band shape: Bell, Low/High Shelf, Low/High Cut, Notch, Band Pass or Tilt.";
        if (key == "slope")  return "Slope of a Low or High Cut, in dB per octave.";
        if (key == "freq")   return "Frequency of this band.";
        if (key == "gain")   return "Gain: boost or cut at this band.";
        if (key == "q")      return "Q: width of the band. Higher is narrower.";
    }

    if (paramID.startsWith ("fxComp.band"))
    {
        const auto key = paramID.fromLastOccurrenceOf (".", false, false);
        if (key == "thresh")  return "Threshold: level where compression starts. Signal above it is turned down, signal below it is lifted by the Up Ratio.";
        if (key == "ratio")   return "Down ratio: how strongly signal above the threshold is turned down. 4:1 means 4 dB over becomes 1 dB over.";
        if (key == "upratio") return "Upward ratio: how strongly quiet signal below the threshold is brought up. 1:1 is off.";
        if (key == "attack")  return "Attack: how fast the band reacts when the level rises.";
        if (key == "release") return "Release: how fast the band recovers when the level falls.";
        if (key == "gain")    return "Makeup gain: output level of this band after compression.";
    }

    if (paramID.startsWith ("mod.slot") && paramID.endsWith (".depth"))
        return "Depth: how far the sidechain envelope moves this slot's target. Negative values move it the other way.";

    return {};
}

} // namespace spa::ui
