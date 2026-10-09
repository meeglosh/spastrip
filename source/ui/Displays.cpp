#include "Displays.h"
#include "../dsp/FXChain.h"
#include "../dsp/ParametricEQ.h"

namespace spa::ui
{

namespace
{
    // FXDisplay's animated kinds (chorus/delay/mod/tremVib) base their
    // scroll/playhead phase on wall-clock time, frame-rate independent by
    // design -- which makes a render non-deterministic. This freezes it for
    // the snapshot tool / tests: >=0 means "use this instead of the clock".
    std::atomic<double> fxDisplayFrozenMs { -1.0 };
}

void setFxDisplayFrozenMsForTest (double ms) { fxDisplayFrozenMs.store (ms, std::memory_order_relaxed); }

static double fxDisplayNowMs()
{
    const auto frozen = fxDisplayFrozenMs.load (std::memory_order_relaxed);
    return frozen >= 0.0 ? frozen : juce::Time::getMillisecondCounterHiRes();
}

// ========================== DisplayComponent ===============================

DisplayComponent::DisplayComponent (juce::AudioProcessorValueTreeState& state,
                                    juce::StringArray paramIDs,
                                    const dsp::Telemetry* tel)
    : apvts (state), telemetry (tel), watched (std::move (paramIDs))
{
    setInterceptsMouseClicks (false, false);
    for (const auto& id : watched)
        apvts.addParameterListener (id, this);
    startTimerHz (24);
}

DisplayComponent::~DisplayComponent()
{
    for (const auto& id : watched)
        apvts.removeParameterListener (id, this);
}

bool DisplayComponent::isLive() const
{
    // Audio is flowing through the plugin (the post-chain peak of the last block).
    return telemetry != nullptr
        && juce::jmax (telemetry->peakL.load (std::memory_order_relaxed),
                       telemetry->peakR.load (std::memory_order_relaxed)) > 1.0e-4f;
}

float DisplayComponent::value (const juce::String& paramID) const
{
    auto* param = apvts.getParameter (paramID);
    return param != nullptr ? param->convertFrom0to1 (param->getValue()) : 0.0f;
}

void DisplayComponent::timerCallback()
{
    if (! isLiveShowing (*this))   // hidden tab / closed editor: cost nothing
        return;
    if (dirty.exchange (false) || isLive() || wantsAnimation())
        repaint();
}

void DisplayComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    draw::displayWell (g, bounds);
    paintDisplay (g, bounds.reduced (3.0f));
}

// ============================= FXDisplay ===================================

juce::StringArray FXDisplay::watchedFor (Kind kind)
{
    namespace fx = params::id::fx;
    switch (kind)
    {
        case Kind::distortion: return { fx::distEnable, fx::distType, fx::distDrive, fx::distMix };
        case Kind::chorus:     return { fx::chorusEnable, fx::chorusRate, fx::chorusDepth,
                                        fx::chorusFeedback, fx::chorusWidth, fx::chorusMode,
                                        fx::chorusMix, fx::chorusVhsWow, fx::chorusVhsFlutter };
        case Kind::delay:      return { fx::delayEnable, fx::delaySync, fx::delayTime,
                                        fx::delayDivision, fx::delayFeedback, fx::delayPingPong,
                                        fx::delayWidth, fx::delayMix };
        case Kind::reverb:     return { fx::reverbEnable, fx::reverbSize, fx::reverbDamping,
                                        fx::reverbMix };
        case Kind::mod:        return { fx::modEnable, fx::modType, fx::modRate, fx::modSync,
                                        fx::modDivision, fx::modDepth, fx::modFeedback,
                                        fx::modStages, fx::modCentre, fx::modManual,
                                        fx::modWidth, fx::modMix };
        case Kind::tremVib:    return { fx::tremEnable, fx::tremRate, fx::tremSync,
                                        fx::tremDivision, fx::tremDepth, fx::tremShape,
                                        fx::tremStereo, fx::tremMix,
                                        fx::vibEnable, fx::vibRate, fx::vibSync,
                                        fx::vibDivision, fx::vibDepth, fx::vibMix };
        case Kind::grain:      return { fx::grainEnable, fx::grainSize, fx::grainDensity,
                                        fx::grainSync, fx::grainDivision, fx::grainPitch,
                                        fx::grainSpread, fx::grainSpreadPitch, fx::grainPosition, fx::grainReverse,
                                        fx::grainFeedback, fx::grainMix, fx::grainRelease };
        case Kind::eq:
        {
            juce::StringArray ids { fx::eqEnable, fx::eqCharacter };
            for (int b = 0; b < 8; ++b)
            {
                ids.add (params::id::eqBand (b, fx::eqband::enable));
                ids.add (params::id::eqBand (b, fx::eqband::type));
                ids.add (params::id::eqBand (b, fx::eqband::freq));
                ids.add (params::id::eqBand (b, fx::eqband::gain));
                ids.add (params::id::eqBand (b, fx::eqband::q));
            }
            return ids;
        }
    }
    return {};
}

FXDisplay::FXDisplay (juce::AudioProcessorValueTreeState& state, Kind k, const dsp::Telemetry* tel)
    : DisplayComponent (state, watchedFor (k), tel), kind (k)
{
}

bool FXDisplay::wantsAnimation() const
{
    namespace fx = params::id::fx;
    // A tempo change (host automation, tap tempo) doesn't touch any APVTS
    // parameter, so the normal listener->dirty path never fires for it;
    // this is the one case a synced-but-disabled display still needs to
    // notice on its own, without paying for a continuous 24Hz repaint the
    // rest of the time.
    const auto bpmChanged = [this]
    {
        if (telemetry == nullptr)
            return false;
        return std::abs (telemetry->bpm.load (std::memory_order_relaxed) - lastDrawnBpm) > 0.01f;
    };
    switch (kind)
    {
        case Kind::chorus:     return value (fx::chorusEnable) >= 0.5f;
        case Kind::delay:      return value (fx::delayEnable) >= 0.5f
                                    || (value (fx::delaySync) >= 0.5f && bpmChanged());
        case Kind::mod:        return value (fx::modEnable) >= 0.5f
                                    || (value (fx::modSync) >= 0.5f && bpmChanged());
        case Kind::tremVib:    return value (fx::tremEnable) >= 0.5f || value (fx::vibEnable) >= 0.5f
                                    || ((value (fx::tremSync) >= 0.5f || value (fx::vibSync) >= 0.5f)
                                        && bpmChanged());
        // The cloud moves on its own clock (grains are spawned whether or not
        // anything is playing), so it animates whenever the effect is on.
        case Kind::grain:      return value (fx::grainEnable) >= 0.5f;
        case Kind::distortion:
        case Kind::reverb:
        case Kind::eq:         return false;
    }
    return false;
}

namespace
{
    // Mirrors StereoChorus's own bipolar LFO shapes exactly (phase 0..1
    // cycles), so the display's Vintage/Modern shape choice matches what
    // the DSP actually sweeps with.
    float sineBipolarPhase (float phase)
    {
        return std::sin (phase * juce::MathConstants<float>::twoPi);
    }

    float triangleBipolarPhase (float phase)
    {
        phase -= std::floor (phase);
        return phase < 0.5f ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
    }
}

void FXDisplay::paintDisplay (juce::Graphics& g, juce::Rectangle<float> area)
{
    namespace fx = params::id::fx;
    const auto& t = currentTheme();

    const auto enabledID = kind == Kind::distortion ? fx::distEnable
                         : kind == Kind::chorus     ? fx::chorusEnable
                         : kind == Kind::delay      ? fx::delayEnable
                         : kind == Kind::reverb     ? fx::reverbEnable
                         : kind == Kind::mod        ? fx::modEnable
                         : kind == Kind::tremVib    ? fx::tremEnable
                         : kind == Kind::grain      ? fx::grainEnable : fx::eqEnable;
    const auto colour = value (enabledID) >= 0.5f ? t.accentMod
                                                  : t.textSecondary.withAlpha (0.45f);

    switch (kind)
    {
        case Kind::distortion:
        {
            // Transfer curve, input -1..1 -> output, with the dry diagonal.
            const auto drive = 1.0f + 15.0f * value (fx::distDrive);
            const auto type = (int) value (fx::distType);
            const auto mix = value (fx::distMix);

            g.setColour (t.outline);
            g.drawLine (area.getX(), area.getBottom(), area.getRight(), area.getY(), 1.0f);

            juce::Path curve;

            if (type == 3)
            {
                // Bit-crush staircase: fewer, wider steps as DRIVE (bit
                // depth reduction) increases, so the display reads as a
                // quantiser rather than a smooth shaper.
                const auto crushDrive = value (fx::distDrive);
                const auto crushLevels = std::pow (2.0f, dsp::FXChain::crushBitsForDrive (crushDrive));
                const auto numSteps = (float) juce::jlimit (3, 64, (int) crushLevels);

                for (int s = 0; s <= (int) numSteps; ++s)
                {
                    const auto in = -1.0f + 2.0f * (float) s / numSteps;
                    const auto quantised = std::round (in * crushLevels) / crushLevels;
                    const auto out = in + (quantised - in) * mix;

                    const auto px = area.getX() + area.getWidth() * (float) s / numSteps;
                    const auto py = area.getCentreY() - out * area.getHeight() * 0.46f;
                    if (s == 0)
                        curve.startNewSubPath (px, py);
                    else
                        curve.lineTo (px, py);

                    if ((float) s < numSteps)
                    {
                        const auto nextPx = area.getX() + area.getWidth() * (float) (s + 1) / numSteps;
                        curve.lineTo (nextPx, py);
                    }
                }
            }
            else
            {
                constexpr int steps = 96;
                for (int i = 0; i <= steps; ++i)
                {
                    const auto in = -1.0f + 2.0f * (float) i / steps;
                    const auto x = in * drive;
                    float wet;
                    switch (type)
                    {
                        case 1:  wet = juce::jlimit (-1.0f, 1.0f, x); break;
                        case 2:  wet = std::sin (x * 1.2f); break;
                        default: wet = std::tanh (x); break;
                    }
                    wet /= std::sqrt (drive);
                    const auto out = in + (wet - in) * mix;

                    const auto px = area.getX() + area.getWidth() * (float) i / steps;
                    const auto py = area.getCentreY() - out * area.getHeight() * 0.46f;
                    if (i == 0)
                        curve.startNewSubPath (px, py);
                    else
                        curve.lineTo (px, py);
                }
            }
            draw::glowStroke (g, curve, colour, 1.6f);
            break;
        }

        case Kind::chorus:
        {
            // Two voices weaving around a dry centre line, one per real
            // StereoChorus LFO: density from RATE (log-mapped over its real
            // 0.05-5Hz range so slow rates still read as motion), amplitude
            // from DEPTH, the L/R phase offset from WIDTH (0 = overlapping,
            // 100% = opposed, matching StereoChorus's own spread), shape
            // (triangle/sine) from MODE, a faint sharpened echo from
            // FEEDBACK, and wet/dry brightness balance from MIX.
            const auto rate = value (fx::chorusRate);
            const auto depth = value (fx::chorusDepth);
            const auto widthPct = value (fx::chorusWidth);
            const auto feedback = value (fx::chorusFeedback);
            const auto mode = (int) value (fx::chorusMode);   // 0 Vintage, 1 Modern, 2 VHS
            const auto vhsWow = value (fx::chorusVhsWow) * 0.01f;
            const auto vhsFlutter = value (fx::chorusVhsFlutter) * 0.01f;
            const auto mix = value (fx::chorusMix);
            const bool enabled = value (fx::chorusEnable) >= 0.5f;

            // Dry centre reference; fades out as MIX goes fully wet.
            g.setColour (t.textPrimary.withAlpha (juce::jmap (mix, 0.0f, 1.0f, 0.5f, 0.12f)));
            g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

            constexpr float rateLo = 0.05f, rateHi = 5.0f;
            const auto cycles = juce::jmap (std::log (juce::jlimit (rateLo, rateHi, rate)),
                                            std::log (rateLo), std::log (rateHi), 1.2f, 7.0f);
            const auto scrollCycles = (enabled && isLiveShowing (*this))
                                     ? (float) std::fmod (fxDisplayNowMs() * 0.001 * rate, 2000.0) : 0.0f;   // wrap in double: a float of uptime*rate loses the fraction and staircases the trace
            const auto offsetCycles = 0.5f * (widthPct / 100.0f);   // 0..0.5 cycle (0..180deg)

            // Thin the glow as density rises -- at max rate the crossings
            // are packed tightly enough that the mock's usual glowStroke
            // thickness turns the lattice into a smeared blur.
            const auto densityT = juce::jmap (cycles, 1.2f, 7.0f, 0.0f, 1.0f);
            const auto strokeThickness = juce::jmap (densityT, 1.4f, 0.85f);

            const auto wowAmp = vhsWow, flutAmp = vhsFlutter;
            // VHS is one tape, not two voices: a single smooth trace.
            for (int voice = 0; voice < (mode == 2 ? 1 : 2); ++voice)
            {
                juce::Path curve;
                const int steps = mode == 2 ? 360 : 140;
                for (int i = 0; i <= steps; ++i)
                {
                    const auto x01 = (float) i / steps;
                    const auto phase = x01 * cycles + scrollCycles
                                      + (voice == 1 ? offsetCycles : 0.0f);
                    // VHS: an irregular seasick wobble (slow incommensurate
                    // waves = WOW) with a fine ripple on top (FLUTTER).
                    const auto shape = mode == 2
                        ? 0.55f * wowAmp * (0.6f * sineBipolarPhase (phase * 0.45f)
                                            + 0.4f * sineBipolarPhase (phase * 0.166f + 0.3f))
                            + 0.25f * flutAmp * sineBipolarPhase (phase * 2.3f)
                        : (mode == 0 ? triangleBipolarPhase (phase)
                                     : sineBipolarPhase (phase));
                    const auto v = mode == 2 ? shape * (0.25f + 0.75f * mix) * 1.6f
                                             : shape * depth * (0.25f + 0.75f * mix);
                    const auto x = area.getX() + area.getWidth() * x01;
                    const auto y = area.getCentreY() - v * area.getHeight() * 0.42f;
                    if (i == 0)
                        curve.startNewSubPath (x, y);
                    else
                        curve.lineTo (x, y);
                }
                const auto voiceAlpha = 0.35f + 0.65f * mix;
                draw::glowStroke (g, curve,
                                  (voice == 0 ? colour : colour.withAlpha (0.55f))
                                      .withMultipliedAlpha (voiceAlpha), strokeThickness);
            }

            // FEEDBACK: an honest ghost -- a faint, slightly sharpened repeat
            // of voice 0's trace shifted forward, standing in for the
            // resonant echo a comb-like feedback path adds. Absent at 0.
            if (mode != 2 && std::abs (feedback) > 0.02f)
            {
                juce::Path ghost;
                constexpr int steps = 140;
                const auto sharpen = 1.0f + 2.0f * std::abs (feedback);
                const auto ghostShiftCycles = 0.1f;
                for (int i = 0; i <= steps; ++i)
                {
                    const auto x01 = (float) i / steps;
                    const auto phase = x01 * cycles + scrollCycles + ghostShiftCycles;
                    auto shape = mode == 0 ? triangleBipolarPhase (phase)
                                           : sineBipolarPhase (phase);
                    shape = std::copysign (std::pow (std::abs (shape), 1.0f / sharpen), shape);
                    const auto v = shape * depth * (0.25f + 0.75f * mix);
                    const auto x = area.getX() + area.getWidth() * x01;
                    const auto y = area.getCentreY() - v * area.getHeight() * 0.42f;
                    if (i == 0)
                        ghost.startNewSubPath (x, y);
                    else
                        ghost.lineTo (x, y);
                }
                draw::glowStroke (g, ghost,
                                  colour.withAlpha (0.28f * juce::jmin (1.0f, std::abs (feedback) / 0.9f)),
                                  strokeThickness * 0.7f);
            }
            break;
        }

        case Kind::delay:
        {
            // Real echo timing: tap spacing follows the actual delay time
            // (free ms, or division*beat at the real resolved tempo via
            // Telemetry::bpm when synced -- 120 only when this FXDisplay
            // was built without a telemetry pointer, e.g. an isolated test
            // render). Every tap's level comes from ONE
            // function, levelForTap(n) = mix * feedback^(n-1), and both the
            // envelope curve(s) and the tap bars are built from it, so they
            // can never disagree. In ping-pong, echo 1 is left and echo 2 is
            // right (see FXChain::processDelay's own comment on why), so
            // each lane's own envelope runs through every SECOND tap --
            // level halves twice as fast on the calendar but each hop still
            // costs exactly one FEEDBACK multiply, matching the DSP. A
            // playhead sweeps the window once per delay time and brightens
            // each tap as it passes.
            const auto enabled = value (fx::delayEnable) >= 0.5f;
            const auto sync = value (fx::delaySync) >= 0.5f;
            const auto timeMs = value (fx::delayTime);
            const auto divisionIdx = (int) value (fx::delayDivision);
            const auto feedback = juce::jlimit (0.0f, 0.97f, value (fx::delayFeedback));
            const auto pingpong = value (fx::delayPingPong) >= 0.5f;
            const auto widthPct = value (fx::delayWidth);
            const auto width01 = pingpong ? juce::jlimit (0.0f, 1.0f, widthPct / 100.0f) : 0.0f;
            const auto mix = value (fx::delayMix);

            // Real resolved tempo via Telemetry::bpm (mirrors the exact
            // value FXChain's synced modules use, published once per block
            // by SPASynthProcessor::processBlock); 120 only when no
            // telemetry was supplied (e.g. an isolated test render).
            const auto fallbackBpm = telemetry != nullptr
                                   ? (double) telemetry->bpm.load (std::memory_order_relaxed) : 120.0;
            lastDrawnBpm = (float) fallbackBpm;
            const auto timeSeconds = sync
                                    ? params::lfoDivisionBeats (divisionIdx) * 60.0 / fallbackBpm
                                    : (double) timeMs * 0.001;
            const auto timeSecF = (float) juce::jmax (0.001, timeSeconds);

            // Pick the smallest of a few fixed zoom windows that fits at
            // least ~3 taps, so turning TIME visibly moves the taps instead
            // of the view silently rescaling around them.
            constexpr float zoomSteps[] = { 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
            float window = zoomSteps[std::size (zoomSteps) - 1];
            for (auto z : zoomSteps)
                if (z / timeSecF >= 3.0f) { window = z; break; }

            const auto spacingPx = area.getWidth() * (timeSecF / window);
            const auto baseX = area.getX();
            const auto halfH = area.getHeight() * 0.46f;

            // How many echoes before the level drops below ~-48dB, capped so
            // a very short time / high feedback combination can't flood the
            // display, and capped by how many actually fit the window.
            constexpr float floorLinear = 0.00398f;   // -48dB
            const auto levelForTap = [&] (int n) -> float
            {
                return mix * std::pow (juce::jmax (0.0001f, feedback), (float) (n - 1));
            };
            int decayTaps = 1;
            while (levelForTap (decayTaps + 1) > floorLinear && decayTaps < 64)
                ++decayTaps;
            const auto tapsInWindow = juce::jmax (0, (int) (window / timeSecF));
            const auto numTaps = juce::jmin (16, decayTaps, tapsInWindow);

            // Faint time grid: one tick per beat when synced (the actual
            // musical grid), else 8 even divisions of the window.
            g.setColour (t.outline.withAlpha (0.24f));
            if (sync)
            {
                const auto beatSec = 60.0 / fallbackBpm;
                for (double bt = beatSec; bt < window; bt += beatSec)
                {
                    const auto x = area.getX() + area.getWidth() * (float) (bt / window);
                    g.drawVerticalLine ((int) x, area.getBottom() - 5.0f, area.getBottom());
                }
            }
            else
            {
                for (int i = 1; i < 8; ++i)
                {
                    const auto x = area.getX() + area.getWidth() * (float) i / 8.0f;
                    g.drawVerticalLine ((int) x, area.getBottom() - 5.0f, area.getBottom());
                }
            }

            // Lanes: OFF = a single centred lane, drawn mirrored top/bottom
            // like a waveform. ON = upper=L / lower=R, crossfading from
            // stacked-at-centre (width 0, i.e. the pre-1.0.25 ping-pong
            // look) to fully separated (width 100).
            const auto laneShift = area.getHeight() * 0.23f * width01;
            const auto laneCentre = [&] (bool leftLane)
            {
                return area.getCentreY() + (leftLane ? -laneShift : laneShift);
            };

            const auto phase01 = (enabled && isLiveShowing (*this))
                                ? (float) std::fmod (fxDisplayNowMs() * 0.001, (double) window) / window
                                : -1.0f;
            const auto playheadX = area.getX() + area.getWidth() * juce::jmax (0.0f, phase01);

            // One smooth, gently filled decay envelope from startX to the
            // right edge, gradient-filled toward its own baseline so it
            // reads as an energy trail rather than a flat wash. stepPx/
            // stepDecay let the SAME curve builder serve the single-lane
            // continuous decay (one hop per tap) and each ping-pong lane's
            // own decay (one hop every SECOND tap, i.e. half the taps, twice
            // the per-step distance) without duplicating the math.
            const auto drawEnvelope = [&] (float startX, float startLevel, float stepPx,
                                           float stepDecay, float baselineY, bool goingUp)
            {
                if (startX > area.getRight())
                    return;
                juce::Path env, fill;
                constexpr int steps = 80;
                bool first = true;
                for (int i = 0; i <= steps; ++i)
                {
                    const auto x = startX + (area.getRight() - startX) * (float) i / steps;
                    const auto k = (x - startX) / juce::jmax (1.0f, stepPx);
                    const auto lvl = startLevel * std::pow (juce::jmax (0.0001f, stepDecay), k);
                    const auto y = baselineY + (goingUp ? -1.0f : 1.0f) * lvl * halfH;
                    if (first) { env.startNewSubPath (x, y); fill.startNewSubPath (x, baselineY); fill.lineTo (x, y); first = false; }
                    else { env.lineTo (x, y); fill.lineTo (x, y); }
                }
                fill.lineTo (area.getRight(), baselineY);
                fill.closeSubPath();

                const auto topY = goingUp ? baselineY - startLevel * halfH : baselineY;
                const auto botY = goingUp ? baselineY : baselineY + startLevel * halfH;
                juce::ColourGradient grad (colour.withAlpha (0.16f), 0.0f, goingUp ? topY : botY,
                                           colour.withAlpha (0.0f), 0.0f, goingUp ? botY : topY, false);
                g.setGradientFill (grad);
                g.fillPath (fill);
                draw::glowStroke (g, env, colour.withAlpha (0.32f), 0.8f);
            };

            if (pingpong)
            {
                // Lane L runs through taps 1,3,5,... (one hop = 2 taps, so
                // level halves per PAIR of taps but only one FEEDBACK
                // multiply per hop -- stepDecay is feedback^2 accordingly).
                drawEnvelope (baseX + spacingPx, levelForTap (1), 2.0f * spacingPx,
                             feedback * feedback, laneCentre (true), true);
                // Lane R runs through taps 2,4,6,...
                drawEnvelope (baseX + spacingPx * 2.0f, levelForTap (2), 2.0f * spacingPx,
                             feedback * feedback, laneCentre (false), false);
            }
            else
            {
                // Single lane, mirrored top and bottom like a waveform.
                drawEnvelope (baseX + spacingPx, levelForTap (1), spacingPx, feedback,
                             area.getCentreY(), true);
                drawEnvelope (baseX + spacingPx, levelForTap (1), spacingPx, feedback,
                             area.getCentreY(), false);
            }

            // Dry impulse at t=0 -- full height, unmistakably distinct from
            // the wet taps (solid, textPrimary, no glow).
            g.setColour (t.textPrimary);
            g.fillRoundedRectangle (juce::Rectangle<float> (baseX - 1.5f, area.getY(), 3.0f,
                                                            area.getHeight()), 1.0f);

            for (int tap = 1; tap <= numTaps; ++tap)
            {
                const auto x = baseX + spacingPx * (float) tap;
                if (x > area.getRight() + 2.0f)
                    break;
                const auto level = levelForTap (tap);
                // A floor keeps even a near-decayed tap readable.
                const auto lvl = juce::jmax (0.05f, level);
                const auto h = lvl * halfH;

                // Playhead proximity flashes the tap as it sweeps past.
                const auto dist = phase01 >= 0.0f
                                 ? std::abs (phase01 * window - (float) tap * timeSecF) : window;
                const auto glow = juce::jmax (0.0f, 1.0f - dist / (window * 0.035f));
                const auto tapAlpha = juce::jlimit (0.0f, 1.0f, 0.35f + 0.55f * lvl + 0.4f * glow);

                const bool leftLane = pingpong ? (tap % 2 == 1) : true;
                const auto cy = pingpong ? laneCentre (leftLane) : area.getCentreY();
                const auto goingUp = pingpong ? leftLane : (tap % 2 == 1);   // off-mode alternates, mirrored
                const auto top = goingUp ? cy - h : cy;
                const auto bottom = goingUp ? cy : cy + h;

                // Soft glowing pill: a wide, faint underlay plus a narrow,
                // vertically graded core (bright at the baseline, fading
                // toward the tip) -- alpha overall falls with level, so a
                // near-decayed tap reads as a ghost rather than a hard edge.
                juce::Path pill;
                pill.startNewSubPath (x, top);
                pill.lineTo (x, bottom);
                draw::glowStroke (g, pill, colour.withAlpha (tapAlpha), 1.6f);

                juce::ColourGradient core (colour.withAlpha (tapAlpha), x, goingUp ? bottom : top,
                                          colour.withAlpha (tapAlpha * 0.35f), x, goingUp ? top : bottom, false);
                g.setGradientFill (core);
                g.fillRoundedRectangle (juce::Rectangle<float> (x - 1.2f, top, 2.4f, juce::jmax (2.0f, h)), 1.0f);
            }

            if (pingpong)
            {
                g.setColour (t.textSecondary.withAlpha (0.45f));
                g.setFont (metrics::smallFont());
                g.drawText ("L", juce::Rectangle<float> (area.getX(), laneCentre (true) - 14.0f, 16.0f, 12.0f),
                           juce::Justification::centredLeft);
                g.drawText ("R", juce::Rectangle<float> (area.getX(), laneCentre (false) + 2.0f, 16.0f, 12.0f),
                           juce::Justification::centredLeft);
            }
            else
            {
                g.setColour (t.outline.withAlpha (0.5f));
                g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());
            }

            // Travelling playhead: a soft glowing sweep (same glowStroke
            // language as the envelope/taps) with a short fading trail --
            // a few individually-faint dots along its base rather than
            // repeated full-height glow lines, which stacked into a solid
            // opaque block at this component's normal glow thickness. On
            // top of the taps so a flash reads clearly.
            if (phase01 >= 0.0f)
            {
                for (int trail = 3; trail >= 1; --trail)
                {
                    const auto trailX = playheadX - (float) trail * 5.0f;
                    if (trailX < area.getX())
                        continue;
                    const auto trailAlpha = 0.35f * (1.0f - (float) trail / 4.0f);
                    g.setColour (t.accentMod.withAlpha (trailAlpha));
                    g.fillEllipse (juce::Rectangle<float> (trailX - 1.5f, area.getBottom() - 4.5f, 3.0f, 3.0f));
                }
                juce::Path playheadLine;
                playheadLine.startNewSubPath (playheadX, area.getY());
                playheadLine.lineTo (playheadX, area.getBottom());
                draw::glowStroke (g, playheadLine, t.accentMod.withAlpha (0.8f), 1.1f);
            }

            // Time readout, corner label.
            const auto label = sync ? params::lfoDivisionNames()[divisionIdx]
                                    : (juce::String ((int) std::round (timeMs)) + " ms");
            g.setColour (t.textSecondary.withAlpha (0.7f));
            g.setFont (metrics::smallFont());
            g.drawText (label, area.removeFromTop (11.0f).removeFromRight (48.0f),
                       juce::Justification::centredRight);
            break;
        }

        case Kind::reverb:
        {
            // Decay envelope; size stretches it, damping bows it down.
            const auto size = value (fx::reverbSize);
            const auto damping = value (fx::reverbDamping);
            const auto mix = value (fx::reverbMix);

            juce::Path curve;
            constexpr int steps = 100;
            for (int i = 0; i <= steps; ++i)
            {
                const auto x01 = (float) i / steps;
                const auto decay = 1.2f + (1.0f - size) * 6.0f + damping * 2.0f;
                const auto v = (0.2f + 0.8f * mix) * std::exp (-decay * x01);
                const auto x = area.getX() + area.getWidth() * x01;
                const auto y = area.getBottom() - v * area.getHeight() * 0.92f;
                if (i == 0)
                    curve.startNewSubPath (x, y);
                else
                    curve.lineTo (x, y);
            }

            auto fill = curve;
            fill.lineTo (area.getRight(), area.getBottom());
            fill.lineTo (area.getX(), area.getBottom());
            fill.closeSubPath();
            g.setColour (colour.withAlpha (0.18f));
            g.fillPath (fill);
            draw::glowStroke (g, curve, colour, 1.6f);
            break;
        }

        case Kind::mod:
        {
            // Phaser: a frequency-response curve with notches swept between
            // fc*(1-0.9*depth) and fc*(1+2*depth) around CENTRE -- mirrors
            // ModEffect::processPhaser exactly. Flanger: comb teeth spaced by
            // the base DELAY (modManual) swept the same way, mirroring
            // processFlanger. STAGES sets notch count (phaser only);
            // FEEDBACK sharpens them; MIX sets notch depth; WIDTH draws a
            // second, fainter trace offset by the L/R sweep spread. Animates
            // at the real (or synced) rate while enabled and showing.
            const auto type = (int) value (fx::modType);   // 0 phaser, 1 flanger
            const auto rate = value (fx::modRate);
            const auto sync = value (fx::modSync) >= 0.5f;
            const auto divisionIdx = (int) value (fx::modDivision);
            const auto depth = value (fx::modDepth);
            const auto feedback = value (fx::modFeedback);
            // modStages is a choice param storing an INDEX into {2,4,6,8,12}
            // -- see SPASynthProcessor's identical stageCounts table.
            static constexpr int stageCounts[] = { 2, 4, 6, 8, 12 };
            const auto stages = stageCounts[juce::jlimit (0, 4, (int) value (fx::modStages))];
            const auto centreHz = value (fx::modCentre);
            const auto manualMs = value (fx::modManual);
            const auto widthAmt = value (fx::modWidth);
            const auto mix = value (fx::modMix);
            const auto enabled = value (fx::modEnable) >= 0.5f;

            const auto fallbackBpm = telemetry != nullptr
                                   ? (double) telemetry->bpm.load (std::memory_order_relaxed) : 120.0;
            lastDrawnBpm = (float) fallbackBpm;
            const auto effHz = sync
                              ? (float) (fallbackBpm / 60.0
                                         / juce::jmax (0.01, (double) params::lfoDivisionBeats (divisionIdx)))
                              : rate;
            const auto sweepPhase = (enabled && isLiveShowing (*this))
                                   ? std::fmod (fxDisplayNowMs() * 0.001 * effHz, 1.0) : 0.0;
            const auto lfo = 0.5f + 0.5f * std::sin ((float) sweepPhase * juce::MathConstants<float>::twoPi);

            g.setColour (t.outline.withAlpha (0.5f));
            g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

            const auto notchDepthPx = area.getHeight() * 0.44f * (0.2f + 0.8f * mix);
            const auto sharpness = 1.0f + 4.0f * juce::jmax (0.0f, feedback);

            const auto buildTrace = [&] (float spreadLfo) -> juce::Path
            {
                juce::Path curve;
                constexpr int steps = 160;
                if (type == 0)
                {
                    // Phaser: notches spaced across log-frequency, positions
                    // driven by the swept centre freq and its harmonics.
                    const auto minHz = juce::jmax (40.0f, centreHz * (1.0f - 0.9f * depth));
                    const auto maxHz = juce::jmin (18000.0f, centreHz * (1.0f + 2.0f * depth));
                    const auto fc = minHz + (maxHz - minHz) * spreadLfo;
                    for (int i = 0; i <= steps; ++i)
                    {
                        const auto x01 = (float) i / steps;
                        const auto freq = 40.0f * std::pow (500.0f, x01);   // 40Hz..20kHz log sweep
                        float resp = 0.0f;
                        for (int n = 1; n <= juce::jmax (1, stages / 2); ++n)
                        {
                            const auto notchF = fc * (float) n;
                            const auto d = std::log (freq / juce::jmax (1.0f, notchF));
                            resp -= std::exp (-sharpness * d * d * 6.0f);
                        }
                        const auto x = area.getX() + area.getWidth() * x01;
                        const auto y = area.getCentreY() - resp * notchDepthPx;
                        if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
                    }
                }
                else
                {
                    // Flanger: evenly spaced comb teeth, spacing set by the
                    // base delay (shorter delay = wider-spaced teeth),
                    // swept by depth the same way processFlanger sweeps it.
                    const auto sweepMs = 0.5f + 9.0f * depth;
                    const auto delayMs = juce::jmax (0.1f, manualMs + sweepMs * spreadLfo);
                    const auto combHz = 1000.0f / delayMs;   // first null spacing
                    for (int i = 0; i <= steps; ++i)
                    {
                        const auto x01 = (float) i / steps;
                        const auto freq = 40.0f * std::pow (500.0f, x01);
                        const auto resp = -std::pow (std::abs (std::sin (juce::MathConstants<float>::pi
                                                                          * freq / combHz)), 2.0f / sharpness);
                        const auto x = area.getX() + area.getWidth() * x01;
                        const auto y = area.getCentreY() - resp * notchDepthPx;
                        if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
                    }
                }
                return curve;
            };

            const auto mainCurve = buildTrace (lfo);
            draw::glowStroke (g, mainCurve, colour, 1.5f);

            if (widthAmt > 0.02f)
            {
                const auto spreadOffset = 0.5f * juce::jlimit (0.0f, 1.0f, widthAmt);
                const auto lfo2 = 0.5f + 0.5f * std::sin (((float) sweepPhase + spreadOffset)
                                                          * juce::MathConstants<float>::twoPi);
                draw::glowStroke (g, buildTrace (lfo2), colour.withAlpha (0.4f), 1.0f);
            }
            break;
        }

        case Kind::tremVib:
        {
            // Top half: tremolo as an amplitude envelope over a carrier
            // (SHAPE/RATE/DEPTH/STEREO/MIX), mirroring TremVib::shapeVal.
            // Bottom half: vibrato as a pitch-wobble sine (RATE/DEPTH/MIX).
            // Each half dims independently when its own enable is off.
            const auto tremOn = value (fx::tremEnable) >= 0.5f;
            const auto tremRate = value (fx::tremRate);
            const auto tremSync = value (fx::tremSync) >= 0.5f;
            const auto tremDivisionIdx = (int) value (fx::tremDivision);
            const auto tremDepth = value (fx::tremDepth);
            const auto tremShape = (int) value (fx::tremShape);
            const auto tremStereo = value (fx::tremStereo);
            const auto tremMix = value (fx::tremMix);

            const auto vibOn = value (fx::vibEnable) >= 0.5f;
            const auto vibRate = value (fx::vibRate);
            const auto vibSync = value (fx::vibSync) >= 0.5f;
            const auto vibDivisionIdx = (int) value (fx::vibDivision);
            const auto vibDepth = value (fx::vibDepth);
            const auto vibMix = value (fx::vibMix);

            const auto fallbackBpm = telemetry != nullptr
                                   ? (double) telemetry->bpm.load (std::memory_order_relaxed) : 120.0;
            lastDrawnBpm = (float) fallbackBpm;
            const auto syncHz = [&] (int div)
            {
                return (float) (fallbackBpm / 60.0
                                / juce::jmax (0.01, (double) params::lfoDivisionBeats (div)));
            };
            const auto tremHz = tremSync ? syncHz (tremDivisionIdx) : tremRate;
            const auto vibHz  = vibSync  ? syncHz (vibDivisionIdx)  : vibRate;

            const auto shapeVal = [] (int shape, float phase) -> float
            {
                phase -= std::floor (phase);
                switch (shape)
                {
                    case 1:  return 1.0f - std::abs (2.0f * phase - 1.0f);
                    case 2:  return phase < 0.5f ? 1.0f : 0.0f;
                    case 3:  return phase;
                    default: return 0.5f + 0.5f * std::sin (phase * juce::MathConstants<float>::twoPi);
                }
            };

            const auto showing = isLiveShowing (*this);
            const auto tremPhase = (tremOn && showing) ? (float) std::fmod (fxDisplayNowMs() * 0.001 * tremHz, 1.0) : 0.0f;
            const auto vibPhase  = (vibOn && showing)  ? (float) std::fmod (fxDisplayNowMs() * 0.001 * vibHz, 1.0)  : 0.0f;

            auto top = area.removeFromTop (area.getHeight() * 0.5f);
            auto bottom = area;
            bottom.removeFromTop (2.0f);

            const auto tremColour = tremOn ? colour : t.textSecondary.withAlpha (0.35f);
            const auto vibColour  = vibOn  ? colour : t.textSecondary.withAlpha (0.35f);

            g.setColour (t.outline.withAlpha (0.35f));
            g.drawHorizontalLine ((int) top.getCentreY(), top.getX(), top.getRight());

            // Tremolo: amplitude envelope traced over a fixed-frequency
            // carrier, L trace solid, R trace (stereo offset) fainter.
            constexpr float carrierCycles = 6.0f;
            for (int ch = 0; ch < 2; ++ch)
            {
                if (ch == 1 && tremStereo <= 0.001f)
                    continue;
                juce::Path curve;
                constexpr int steps = 160;
                const auto chOffset = ch == 1 ? 0.5f * tremStereo : 0.0f;
                for (int i = 0; i <= steps; ++i)
                {
                    const auto x01 = (float) i / steps;
                    const auto envPhase = x01 + tremPhase + chOffset;
                    const auto lfo = shapeVal (tremShape, envPhase);
                    const auto gain = 1.0f - tremDepth * (1.0f - lfo);
                    const auto env = 1.0f - tremMix + tremMix * gain;
                    const auto carrier = std::sin (x01 * carrierCycles * juce::MathConstants<float>::twoPi);
                    const auto v = carrier * env;
                    const auto x = top.getX() + top.getWidth() * x01;
                    const auto y = top.getCentreY() - v * top.getHeight() * 0.44f;
                    if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
                }
                draw::glowStroke (g, curve, tremColour.withAlpha (ch == 0 ? 1.0f : 0.5f), 1.3f);
            }

            g.setColour (t.outline.withAlpha (0.35f));
            g.drawHorizontalLine ((int) bottom.getCentreY(), bottom.getX(), bottom.getRight());

            // Vibrato: pitch wobble drawn directly as a swept-frequency
            // sine (visual stand-in for the delay-line pitch modulation).
            {
                juce::Path curve;
                constexpr int steps = 160;
                constexpr float baseCycles = 5.0f;
                float phaseAccum = 0.0f;
                for (int i = 0; i <= steps; ++i)
                {
                    const auto x01 = (float) i / steps;
                    const auto wobble = shapeVal (0, x01 * 2.0f + vibPhase) * 2.0f - 1.0f;
                    phaseAccum += (baseCycles / steps) * (1.0f + vibDepth * wobble);
                    const auto v = std::sin (phaseAccum * juce::MathConstants<float>::twoPi) * vibMix;
                    const auto x = bottom.getX() + bottom.getWidth() * x01;
                    const auto y = bottom.getCentreY() - v * bottom.getHeight() * 0.44f;
                    if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
                }
                draw::glowStroke (g, curve, vibColour, 1.3f);
            }
            break;
        }

        case Kind::eq:
        {
            // Composite response of the 8 active parametric bands, +/-24 dB,
            // computed by the same function the DSP uses.
            std::array<dsp::ParametricEQ::Band, 8> bands;
            for (int b = 0; b < 8; ++b)
            {
                auto& bd = bands[(size_t) b];
                bd.enabled = value (params::id::eqBand (b, fx::eqband::enable)) >= 0.5f;
                bd.type    = (int) value (params::id::eqBand (b, fx::eqband::type));
                bd.freq    = value (params::id::eqBand (b, fx::eqband::freq));
                bd.gainDb  = value (params::id::eqBand (b, fx::eqband::gain));
                bd.q       = value (params::id::eqBand (b, fx::eqband::q));
            }

            g.setColour (t.outline.withAlpha (0.7f));
            g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

            juce::Path curve;
            constexpr int steps = 160;
            for (int i = 0; i <= steps; ++i)
            {
                const auto freq = 20.0f * std::pow (1000.0f, (float) i / steps);
                const auto dB = juce::jlimit (-24.0f, 24.0f,
                    dsp::ParametricEQ::magnitudeDb (bands, freq, 48000.0));
                const auto x = area.getX() + area.getWidth() * (float) i / steps;
                const auto y = juce::jmap (dB, -24.0f, 24.0f, area.getBottom(), area.getY());
                if (i == 0)
                    curve.startNewSubPath (x, y);
                else
                    curve.lineTo (x, y);
            }
            draw::glowStroke (g, curve, colour, 1.6f);
            break;
        }

        case Kind::grain:
        {
            // Live grain cloud. The horizontal axis is time behind the write
            // head (NOW at the right edge, older to the left, scaled to the
            // POSITION knob so the interesting region fills the display), the
            // vertical axis is each grain's pitch (SPR PITCH scatters it, PITCH shifts it). Every active grain is
            // a pill centred on where it is reading right now, as wide as the
            // audio it covers, brightness following its window; a small
            // triangle on its leading edge shows the direction it plays in.
            // The base strip is how much of the ring has been written. FREEZE
            // holds that strip and says so.
            const auto on = value (fx::grainEnable) >= 0.5f;
            const auto frozen = value (fx::grainRelease) >= dsp::GrainFX::infiniteRelease;
            const auto posSec = value (fx::grainPosition) * 0.001f;
            const auto sizeSec = value (fx::grainSize) * 0.001f;
            const auto spread = value (fx::grainSpread);
            // Vertical axis: pitch in semitones (0 st on the centre line), wide enough for
            // PITCH + SPR PITCH. The grain's rate is the audio it covers divided by SIZE.
            const float pitchHalfRange = juce::jlimit (6.0f, 24.0f, std::abs (value (fx::grainPitch))
                                                                      + value (fx::grainSpreadPitch) + 2.0f);
            const float windowSec = juce::jlimit (0.5f, 8.0f,
                                                  1.25f * (posSec + 0.25f * spread + 2.0f * sizeSec));
            const auto xForBack = [&] (float backSec)
            {
                return area.getRight() - area.getWidth() * juce::jlimit (0.0f, 1.0f, backSec / windowSec);
            };

            const auto laneTop = area.getY() + 12.0f;
            const auto laneBottom = area.getBottom() - 10.0f;
            const auto laneH = juce::jmax (4.0f, laneBottom - laneTop);

            // Centre (0 st) line and a faint tick per half second, so the
            // POSITION knob reads against something.
            g.setColour (t.outline.withAlpha (0.6f));
            g.drawHorizontalLine ((int) (laneTop + laneH * 0.5f), area.getX(), area.getRight());
            g.setFont (metrics::labelFont());
            for (float sec = 0.5f; sec < windowSec; sec += windowSec > 3.0f ? 1.0f : 0.5f)
            {
                const auto x = xForBack (sec);
                g.setColour (t.outline.withAlpha (0.35f));
                g.drawVerticalLine ((int) x, laneTop, laneBottom);
            }
            g.setColour (t.textSecondary.withAlpha (on ? 0.7f : 0.4f));
            g.setFont (metrics::smallFont());
            g.drawText ("-" + juce::String (windowSec, windowSec < 2.0f ? 2 : 1) + " s",
                        juce::Rectangle<float> (area.getX() + 1.0f, area.getY(), 56.0f, 11.0f),
                        juce::Justification::centredLeft);
            g.drawText ("NOW", juce::Rectangle<float> (area.getRight() - 40.0f, area.getY(), 40.0f, 11.0f),
                        juce::Justification::centredRight);

            float fill = 0.0f;
            int count = 0;
            if (telemetry != nullptr && on)
            {
                const auto& viz = telemetry->grainFx;
                fill = viz.fill.load (std::memory_order_relaxed);
                count = juce::jmin (dsp::Telemetry::maxGrainFxViz,
                                    viz.count.load (std::memory_order_acquire));
            }

            // Ring fill strip along the bottom edge.
            {
                const auto stripY = area.getBottom() - 4.0f;
                g.setColour (t.outline.withAlpha (0.5f));
                g.fillRect (juce::Rectangle<float> (area.getX(), stripY, area.getWidth(), 2.0f));
                const auto reach = juce::jlimit (0.0f, 1.0f, fill * (float) dsp::GrainFX::bufferSeconds / windowSec);
                g.setColour (colour.withAlpha (frozen && on && ! (telemetry != nullptr && telemetry->grainFx.capturing.load (std::memory_order_relaxed)) ? 0.95f : 0.55f));
                g.fillRect (juce::Rectangle<float> (area.getRight() - area.getWidth() * reach, stripY,
                                                    area.getWidth() * reach, 2.0f));
            }

            for (int i = 0; i < count; ++i)
            {
                const auto& viz = telemetry->grainFx;
                const auto back = viz.back[(size_t) i].load (std::memory_order_relaxed);
                const auto age = juce::jlimit (0.0f, 1.0f, viz.age[(size_t) i].load (std::memory_order_relaxed));
                const auto span = viz.span[(size_t) i].load (std::memory_order_relaxed);
                const auto dir = viz.dir[(size_t) i].load (std::memory_order_relaxed);
                
                const auto win = 0.5f - 0.5f * std::cos (age * juce::MathConstants<float>::twoPi);
                const auto cx = xForBack (back);
                const auto w = juce::jmax (4.0f, area.getWidth() * span / windowSec);
                const auto semis = 12.0f * std::log2 (juce::jmax (0.05f, span / juce::jmax (0.002f, sizeSec)));
        const auto cy = laneTop + laneH * (0.5f - 0.5f * juce::jlimit (-1.0f, 1.0f, semis / pitchHalfRange));
                const auto h = juce::jmin (laneH * 0.3f, 2.0f + 5.0f * win);

                const juce::Rectangle<float> pill (cx - w * 0.5f, cy - h * 0.5f, w, h);
                g.setColour (colour.withAlpha (0.12f + 0.78f * win));
                g.fillRoundedRectangle (pill.getIntersection (area), h * 0.5f);

                // Leading-edge arrowhead: right = forward, left = reversed.
                const auto tipX = dir >= 0.0f ? pill.getRight() : pill.getX();
                juce::Path head;
                head.addTriangle (tipX, cy, tipX - dir * 3.0f, cy - 2.5f, tipX - dir * 3.0f, cy + 2.5f);
                g.setColour (t.textPrimary.withAlpha (0.25f + 0.6f * win));
                g.fillPath (head);
            }

            const bool capturingNow = on && frozen && telemetry != nullptr
                                      && telemetry->grainFx.capturing.load (std::memory_order_relaxed);
            if (on && frozen)
            {
                g.setColour (t.textPrimary.withAlpha (0.9f));
                g.setFont (metrics::smallFontBold());
                g.drawText (capturingNow ? "CAPTURING..." : "FROZEN", juce::Rectangle<float> (area.getX(), area.getY(), area.getWidth(), 11.0f),
                            juce::Justification::centred);
            }
            break;
        }
    }
}

} // namespace spa::ui
