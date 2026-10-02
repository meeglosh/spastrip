#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>
#include "Telemetry.h"
#include "../params/LfoDivisions.h"

namespace spa::dsp
{

// GRAIN: a granular delay / texture effect in the spirit of Absynth's
// Aetherizer. The live input is written into a fixed ring buffer; a cloud of
// short Hann-windowed grains reads back out of it, each with its own pitch,
// direction, position and stereo pan.
//
// Real-time safety: the ring (8 s at whatever rate the FX chain runs at,
// which is the OVERSAMPLED rate when oversampling is on) and the grain
// slots are allocated in prepare() and never again. The RNG is a private
// xorshift reseeded from a constant on reset(), so a given input renders
// identically every time (tests rely on it).
//
// Feedback safety (1.0.30). A pitched grain re-entering the ring is pitched
// again on its next pass, so with PITCH > 0 the recirculating content climbs
// an interval per pass until it is aliasing noise, and a cloud of overlapping
// grains that read the SAME audio sums coherently (about overlap/2 times the
// input), so the real loop gain was FEEDBACK x that sum -- well above one --
// and the loop sat at the soft-clip ceiling forever, a full-scale squeal.
// Now: (1) the wet bus goes through a ceiling limiter, so nothing leaves
// GRAIN above kCeiling; (2) the feedback tap is scaled by the coherent
// overlap sum and by 2^(-maxPitch/36), so the loop gain stays below FEEDBACK
// however many grains overlap or far they transpose; (3) the tap is high-
// passed and low-passed (2 poles, cutoff falling with |PITCH|), so upward
// drift is filtered out pass by pass instead of accumulating; (4) grains
// read faster than 1x are low-passed before the window, so a pitched-up read
// does not fold the top of the spectrum back below Nyquist (aliasing was
// what the loop kept re-amplifying).
//
// Geometry: every grain reads at `back` samples behind the write head. A
// forward grain at rate r moves that distance by (w - r) per sample (w = 1
// while the ring is being written, 0 while FREEZE holds it), a reversed one
// by (w + r). spawn() widens `back` so no grain can cross the write head
// (that would splice the newest audio onto the oldest) or run off the far
// end of the ring.
class GrainFX
{
public:
    static constexpr int maxGrains = 256;
    static constexpr float maxDensityHz = 400.0f;
    static constexpr float kCeiling = 0.98f;   // wet bus never exceeds this
    static constexpr double bufferSeconds = 8.0;

    struct Params
    {
        bool enable = false;
        float sizeMs = 120.0f;       // grain length
        float densityHz = 14.0f;     // grains per second (free)
        bool sync = false;           // density from a tempo division instead
        int division = 9;            // lfoDivisionNames() index, used when sync
        double bpm = 120.0;
        float pitchSt = 0.0f;        // +/-24
        float spread = 0.25f;        // 0..1 SPREAD TIME: random start position / interval / pan jitter
        float spreadPitchSt = 3.0f;  // 0..12 SPREAD PITCH: random per-grain pitch jitter, +/- semitones
        float positionMs = 300.0f;   // how far behind the write head grains start
        float reverse = 0.0f;        // 0..1 probability a grain plays backwards
        float feedback = 0.0f;       // 0..0.9 wet signal re-written into the ring
        float mix = 0.35f;           // 0..1 linear dry/wet
        bool freeze = false;         // stop writing, keep granulating what is held
    };

    void prepare (double sr, int /*maxBlock*/)
    {
        sampleRate = juce::jmax (8000.0, sr);
        ringSize = (int) (bufferSeconds * sampleRate) + 16;
        ring[0].assign ((size_t) ringSize, 0.0f);
        ring[1].assign ((size_t) ringSize, 0.0f);

        if (window.empty())
        {
            window.resize ((size_t) windowSize + 1);
            for (int i = 0; i <= windowSize; ++i)
                window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                              * (float) i / (float) windowSize);
        }

        mixSmoothed.reset (sampleRate, 0.02);
        fbSmoothed.reset (sampleRate, 0.02);
        reset();
    }

    void reset()
    {
        for (auto& r : ring)
            std::fill (r.begin(), r.end(), 0.0f);
        for (auto& g : grains)
            g.active = false;
        numActive = 0;
        writeIdx = 0;
        written = 0;
        audible = 0;
        capturing = false;
        countdown = 0.0;
        rng = 0x9E3779B9u;
        dcX = dcY = { 0.0f, 0.0f };
        lp1 = lp2 = { 0.0f, 0.0f };
        limGain = 1.0f;
        frozen = false;
        wasEnabled = false;
        spawned = 0;
        mixSmoothed.setCurrentAndTargetValue (mixSmoothed.getTargetValue());
        fbSmoothed.setCurrentAndTargetValue (fbSmoothed.getTargetValue());
    }

    void process (juce::AudioBuffer<float>& buffer, const Params& p)
    {
        if (! p.enable)
        {
            // Hard stop, and forget the ring: re-enabling must not replay
            // audio from whenever the effect was last on.
            if (wasEnabled)
                reset();
            wasEnabled = false;
            return;
        }

        const int n = buffer.getNumSamples();
        const int numCh = juce::jmin (2, buffer.getNumChannels());
        if (n <= 0 || numCh <= 0 || ringSize <= 0)
            return;

        juce::ScopedNoDenormals noDenormals;
        wasEnabled = true;

        // FREEZE must never hold an empty or too-short ring (it would granulate
        // silence forever): it keeps WRITING until the ring holds enough
        // AUDIBLE material for the current SIZE + POSITION (+ spread reach),
        // minimum 250 ms, and only then holds. `audible` counts samples
        // written since the first non-silent one, so a silent input keeps
        // capturing rather than freezing silence.
        {
            const double needMs = juce::jmax (250.0,
                  (double) juce::jlimit (0.0f, 4000.0f, p.positionMs)
                + (double) juce::jlimit (2.0f, 500.0f, p.sizeMs)
                + 250.0 * (double) juce::jlimit (0.0f, 1.0f, p.spread));
            const int need = (int) juce::jmin (needMs * 0.001 * sampleRate, (double) (ringSize - 64));
            capturing = p.freeze && audible < need;
            frozen = p.freeze && ! capturing;
            freezeRequested = p.freeze;
        }

        mixSmoothed.setTargetValue (juce::jlimit (0.0f, 1.0f, p.mix));

        // Per-block grain parameters. All of these only act on grains spawned
        // from here on, so there is nothing to smooth.
        const float sr = (float) sampleRate;
        cur.lenSamples = juce::jlimit (1.0f, 0.5f * sr,
                                       juce::jlimit (2.0f, 500.0f, p.sizeMs) * 0.001f * sr);
        cur.pitch = juce::jlimit (-24.0f, 24.0f, p.pitchSt);
        cur.spread = juce::jlimit (0.0f, 1.0f, p.spread);
        cur.spreadPitch = juce::jlimit (0.0f, 12.0f, p.spreadPitchSt);
        cur.backSamples = juce::jlimit (0.0f, 4.0f, p.positionMs * 0.001f) * sr;
        cur.reverse = juce::jlimit (0.0f, 1.0f, p.reverse);

        const double grainsPerSecond = p.sync
            ? 1.0 / juce::jmax (1.0e-3, (double) params::lfoDivisionBeats (p.division)
                                        * 60.0 / juce::jmax (20.0, p.bpm))
            : (double) juce::jlimit (0.5f, maxDensityHz, p.densityHz);
        cur.intervalSamples = sampleRate / juce::jlimit (0.5, (double) maxDensityHz, grainsPerSecond);

        // Expected simultaneous grains; the cloud is scaled so a dense one is
        // not louder than a sparse one (uncorrelated Hann grains sum in power).
        const float overlap = (float) ((double) cur.lenSamples / cur.intervalSamples);
        cur.gain = 1.0f / std::sqrt (juce::jmax (1.0f, 0.375f * overlap));

        // Feedback loop gain. Grains that read the same audio add coherently
        // (Hann overlap-add: about overlap/2 x gain), which is the worst case
        // for the loop, so the tap is divided by that sum when it exceeds one;
        // extra attenuation of 2^(-maxPitch/36) keeps the gain per octave of
        // drift below one, and the cutoff falls with the pitch reach.
        const float maxSt = std::abs (cur.pitch) + cur.spreadPitch;
        // Mean of the summed grain windows (Hann: half a window per grain
        // alive) times the per-grain gain = the gain a coherent signal gets
        // through the cloud. Dividing the tap by it makes the loop gain per
        // repeat FEEDBACK whatever SIZE and DENSITY are (a sparse cloud is
        // quieter by design, and used to feed back less because of it).
        // Floored at 0.25 so a nearly empty cloud cannot boost without bound.
        const float coherent = 0.5f * overlap * cur.gain;
        fbSmoothed.setTargetValue (juce::jlimit (0.0f, maxFeedback, p.feedback)
                                   * std::exp2 (-maxSt / 36.0f)
                                   / juce::jmax (0.25f, coherent));
        {
            const float fc = juce::jlimit (2500.0f, 0.4f * sr, 8000.0f * std::exp2 (-0.5f * maxSt / 12.0f));
            lpCoef = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * fc / sr);
            hpCoef = std::exp (-juce::MathConstants<float>::twoPi * 60.0f / sr);
        }
        limRelease = 1.0f - std::exp (-1.0f / (0.04f * sr));

        auto* l = buffer.getWritePointer (0);
        auto* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;
        auto* ringL = ring[0].data();
        auto* ringR = ring[1].data();
        const int sz = ringSize;

        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i];
            const float inR = r != nullptr ? r[i] : inL;

            if (countdown <= 0.0)
            {
                spawn();
                // Spread also jitters the interval, so a cloud with spread
                // does not tick like a metronome.
                const double jitter = 1.0 + 0.8 * (double) cur.spread * ((double) nextUnit() * 2.0 - 1.0);
                countdown += cur.intervalSamples * juce::jmax (0.05, jitter);
            }
            countdown -= 1.0;

            float wetL = 0.0f, wetR = 0.0f;
            for (int gi = 0; gi < numActive;)
            {
                auto& g = grains[(size_t) gi];
                const float w = windowAt (g.age);
                float gl, gr;
                readHermite (ringL, ringR, g.readPos, sz, gl, gr);
                if (g.aa)
                {
                    // Reads faster than 1x fold the top of the spectrum
                    // below Nyquist; low-pass them at the read-rate Nyquist.
                    g.z[0] += g.aaCoef * (gl - g.z[0]);  g.z[1] += g.aaCoef * (g.z[0] - g.z[1]);
                    g.z[2] += g.aaCoef * (gr - g.z[2]);  g.z[3] += g.aaCoef * (g.z[2] - g.z[3]);
                    gl = g.z[1]; gr = g.z[3];
                }
                wetL += gl * w * g.gainL;
                wetR += gr * w * g.gainR;

                g.readPos += g.step;
                if (g.readPos >= (double) sz) g.readPos -= (double) sz;
                else if (g.readPos < 0.0)     g.readPos += (double) sz;
                g.age += g.ageStep;
                if (g.age >= 1.0f)
                {
                    g = grains[(size_t) --numActive];   // swap-remove; re-test slot gi
                    continue;
                }
                ++gi;
            }

            // Ceiling limiter (instant attack, 40 ms release) on the linked
            // wet bus, then a soft clip as the last backstop.
            {
                if (! (std::abs (wetL) < 1.0e6f)) wetL = 0.0f;
                if (! (std::abs (wetR) < 1.0e6f)) wetR = 0.0f;
                const float pk = std::max (std::abs (wetL), std::abs (wetR));
                const float target = pk > kCeiling ? kCeiling / pk : 1.0f;
                limGain = target < limGain ? target : limGain + (1.0f - limGain) * limRelease;
                wetL = softClip (wetL * limGain);
                wetR = softClip (wetR * limGain);
            }

            const float fb = fbSmoothed.getNextValue();
            const float mix = mixSmoothed.getNextValue();

            if (! frozen)
            {
                if (audible > 0 || std::abs (inL) > 1.0e-4f || std::abs (inR) > 1.0e-4f)
                    audible = juce::jmin (sz, audible + 1);

                // High-pass (DC / rumble), then a 2-pole low-pass: what is
                // fed back gets darker every pass.
                float fbL = feedbackFilter (0, wetL * fb);
                float fbR = feedbackFilter (1, wetR * fb);
                const float sL = (std::abs (inL) < 1.0e6f) ? inL : 0.0f;
                const float sR = (std::abs (inR) < 1.0e6f) ? inR : 0.0f;
                ringL[writeIdx] = flush (sL + fbL);
                ringR[writeIdx] = flush (sR + fbR);
                if (++writeIdx >= sz) writeIdx = 0;
                if (written < sz) ++written;
            }

            l[i] = inL + mix * (wetL - inL);
            if (r != nullptr)
                r[i] = inR + mix * (wetR - inR);
        }
    }

    // ---- observation (audio thread writes, any thread may read counters) ----
    std::uint64_t grainsSpawned() const { return spawned; }
    bool isFrozen() const { return frozen; }
    // Test hooks: pitch (semitones) and centre delay behind the head (samples) of the newest grain.
    float lastSpawnSemis() const { return lastSemis; }
    double lastSpawnBackSamples() const { return lastBack; }
    bool isCapturing() const { return capturing; }
    int audibleSamples() const { return audible; }
    int activeGrains() const { return numActive; }
    // Largest |sample| currently in the ring (test hook; O(ring)).
    float ringPeak() const
    {
        float m = 0.0f;
        for (const auto& r : ring)
            for (float v : r) m = std::max (m, std::abs (v));
        return m;
    }
    const float* ringData (int ch) const { return ring[(size_t) juce::jlimit (0, 1, ch)].data(); }
    int ringLength() const { return ringSize; }

    // Publishes the live cloud for the display. Positions are reported as
    // seconds behind the write head (the display scales them), plus age 0..1,
    // direction and pan. Cosmetic, so relaxed atomics, count last.
    void publish (Telemetry::GrainFxViz& viz) const
    {
        int c = 0;
        // Past maxGrainFxViz live grains the display gets an even subset.
        const int stride = juce::jmax (1, (numActive + Telemetry::maxGrainFxViz - 1) / Telemetry::maxGrainFxViz);
        for (int gi = 0; gi < numActive && c < Telemetry::maxGrainFxViz; gi += stride)
        {
            const auto& g = grains[(size_t) gi];
            double back = (double) writeIdx - g.readPos;
            if (back < 0.0) back += (double) ringSize;
            viz.back[(size_t) c].store ((float) (back / sampleRate), std::memory_order_relaxed);
            viz.age[(size_t) c].store (g.age, std::memory_order_relaxed);
            viz.span[(size_t) c].store ((float) (std::abs (g.step) * (double) g.lenSamples / sampleRate),
                                        std::memory_order_relaxed);
            viz.dir[(size_t) c].store (g.step < 0.0 ? -1.0f : 1.0f, std::memory_order_relaxed);
            viz.pan[(size_t) c].store (g.pan, std::memory_order_relaxed);
            ++c;
        }
        viz.fill.store ((float) written / (float) juce::jmax (1, ringSize), std::memory_order_relaxed);
        viz.frozen.store (frozen && wasEnabled, std::memory_order_relaxed);
        viz.capturing.store (capturing && wasEnabled, std::memory_order_relaxed);
        viz.active.store (wasEnabled, std::memory_order_relaxed);
        viz.count.store (c, std::memory_order_release);
    }

    static constexpr float maxFeedback = 0.9f;

private:
    struct Grain
    {
        bool active = false;
        double readPos = 0.0;     // absolute ring index (fractional)
        double step = 1.0;        // ring samples per output sample, signed
        float age = 0.0f;         // 0..1 through the window
        float ageStep = 0.0f;
        float lenSamples = 1.0f;
        float gainL = 1.0f, gainR = 1.0f;
        float pan = 0.0f;
        bool aa = false;
        float aaCoef = 1.0f;
        float z[4] {};
    };

    struct Current
    {
        float lenSamples = 4800.0f;
        float pitch = 0.0f;
        float spread = 0.0f;
        float spreadPitch = 0.0f;
        float backSamples = 0.0f;
        float reverse = 0.0f;
        double intervalSamples = 48000.0 / 14.0;
        float gain = 1.0f;
    };

    // xorshift32; unit() is 0..1
    float nextUnit()
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (float) (rng >> 8) * (1.0f / 16777216.0f);
    }

    void spawn()
    {
        Grain* slot = numActive < maxGrains ? &grains[(size_t) numActive] : nullptr;
        // Draws are consumed whether or not a slot is free, so a full cloud
        // does not shift the random sequence.
        const float uPitch = nextUnit(), uTime = nextUnit(), uDir = nextUnit(), uPan = nextUnit();
        ++spawned;
        if (slot == nullptr)
            return;

        const float semis = juce::jlimit (-36.0f, 36.0f,
                                          cur.pitch + cur.spreadPitch * (uPitch * 2.0f - 1.0f));
        const double rate = juce::jlimit (0.25, 4.0, std::pow (2.0, (double) semis / 12.0));
        const bool backwards = uDir < cur.reverse;
        const double len = (double) cur.lenSamples;
        const double w = frozen ? 0.0 : 1.0;

        // POSITION is the delay at the grain's CENTRE, so the echo spacing is
        // governed by POSITION alone. A grain that reads at rate r while the
        // head moves at w drifts by (w - r) per sample (forward) or (w + r)
        // (reversed); start it half of that drift away from the target. (When
        // POSITION is shorter than the drift allows, spawn()'s geometry clamps
        // below win and the echo comes slightly later.)
        const double centreShift = backwards ? -(w + rate) * 0.5 * len : (rate - w) * 0.5 * len;
        double back = (double) cur.backSamples + centreShift
                    + (double) cur.spread * 0.25 * sampleRate * (double) uTime;
        // While a FREEZE is requested the hold can begin mid-grain (capture
        // -> hold), so the geometry must be safe for both write states.
        const double wLo = freezeRequested ? 0.0 : w;
        const double wHi = freezeRequested ? 1.0 : w;
        double minBack = 6.0;
        double maxBack = (double) ringSize - 8.0;
        if (backwards)
            maxBack -= (wHi + rate) * len;
        else
        {
            minBack += juce::jmax (0.0, rate - wLo) * len;
            maxBack -= juce::jmax (0.0, wHi - rate) * len;
        }
        // A grain must START inside the valid (written) region -- right after
        // a clear the rest of the ring is empty, and reading it would only
        // granulate silence. (Material that scrolls past the oldest sample
        // mid-grain is what the input was before it started: silence.)
        if (written < ringSize)
        {
            if ((double) written - 2.0 < minBack)
                return;   // nothing valid behind the head for this grain yet
            maxBack = juce::jmin (maxBack, (double) written - 2.0);
        }
        back = juce::jlimit (minBack, juce::jmax (minBack, maxBack), back);

        lastSemis = semis; lastBack = back - centreShift;   // delay at the grain centre
        double pos = (double) writeIdx - back;
        while (pos < 0.0) pos += (double) ringSize;

        // Pan: the cloud widens with SPREAD but is never mono-locked.
        const float pan = (uPan * 2.0f - 1.0f) * (0.2f + 0.8f * cur.spread);
        const float ang = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        constexpr float rt2 = 1.41421356f;

        slot->active = true;
        slot->aa = rate > 1.02;
        slot->aaCoef = slot->aa ? 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 0.4f * (float) sampleRate / (float) rate / (float) sampleRate)
                                : 1.0f;
        slot->z[0] = slot->z[1] = slot->z[2] = slot->z[3] = 0.0f;
        ++numActive;
        slot->readPos = pos;
        slot->step = backwards ? -rate : rate;
        slot->age = 0.0f;
        slot->lenSamples = cur.lenSamples;
        slot->ageStep = 1.0f / cur.lenSamples;
        slot->gainL = std::cos (ang) * rt2 * cur.gain;
        slot->gainR = std::sin (ang) * rt2 * cur.gain;
        slot->pan = pan;
    }

    float windowAt (float age) const
    {
        const float x = age * (float) windowSize;
        const int i = juce::jlimit (0, windowSize - 1, (int) x);
        const float f = x - (float) i;
        return window[(size_t) i] + f * (window[(size_t) i + 1] - window[(size_t) i]);
    }

    // 4-point Hermite on both channels at a fractional ring index. spawn()
    // guarantees the four taps are never across the write head.
    static void readHermite (const float* a, const float* b, double pos, int sz, float& outA, float& outB)
    {
        int i1 = (int) pos;
        const float f = (float) (pos - (double) i1);
        int i0 = i1 - 1; if (i0 < 0) i0 += sz;
        int i2 = i1 + 1; if (i2 >= sz) i2 -= sz;
        int i3 = i2 + 1; if (i3 >= sz) i3 -= sz;
        if (i1 >= sz) i1 -= sz;

        const auto h = [f] (float y0, float y1, float y2, float y3)
        {
            const float c0 = y1;
            const float c1 = 0.5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            return ((c3 * f + c2) * f + c1) * f + c0;
        };
        outA = h (a[i0], a[i1], a[i2], a[i3]);
        outB = h (b[i0], b[i1], b[i2], b[i3]);
    }

    // Transparent up to 0.9, then a smooth tanh knee that tops out at 1.0.
    static float softClip (float x)
    {
        const float a = std::abs (x);
        if (a <= 0.9f) return x;
        const float y = 0.9f + 0.1f * std::tanh ((a - 0.9f) * 10.0f);
        return x < 0.0f ? -y : y;
    }

    static float flush (float v) { return std::abs (v) < 1.0e-20f ? 0.0f : v; }

    float feedbackFilter (int ch, float x)
    {
        const float y = x - dcX[(size_t) ch] + hpCoef * dcY[(size_t) ch];
        dcX[(size_t) ch] = x;
        dcY[(size_t) ch] = flush (y);
        lp1[(size_t) ch] = flush (lp1[(size_t) ch] + lpCoef * (y - lp1[(size_t) ch]));
        lp2[(size_t) ch] = flush (lp2[(size_t) ch] + lpCoef * (lp1[(size_t) ch] - lp2[(size_t) ch]));
        return lp2[(size_t) ch];
    }

    double sampleRate = 48000.0;
    int ringSize = 0;
    std::array<std::vector<float>, 2> ring;
    int writeIdx = 0;
    int written = 0;
    bool frozen = false;          // actually holding
    bool capturing = false;       // FREEZE requested but still filling the ring
    bool freezeRequested = false;
    int audible = 0;
    bool wasEnabled = false;

    static constexpr int windowSize = 2048;
    std::vector<float> window;

    std::array<Grain, maxGrains> grains {};
    Current cur;
    double countdown = 0.0;
    std::uint32_t rng = 0x9E3779B9u;
    std::uint64_t spawned = 0;
    std::array<float, 2> dcX {}, dcY {}, lp1 {}, lp2 {};
    float lpCoef = 0.5f, hpCoef = 0.995f;
    float limGain = 1.0f, limRelease = 0.001f;
    int numActive = 0;
    float lastSemis = 0.0f;
    double lastBack = 0.0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmoothed, fbSmoothed;

    JUCE_LEAK_DETECTOR (GrainFX)
};

} // namespace spa::dsp
