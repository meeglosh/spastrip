// Pure-DSP FX engine tests ported from SPASynth's tests/SPASynthTests.cpp
// (stereo chorus, delay ping-pong width, plate reverb arrival/index stress).
// The bodies are unchanged except that the synth-only `modDestination`
// assertions were removed (the field does not exist in SPAStrip's registry).
// Included from SPAStripTests.cpp, where `expect` is in scope.

namespace fxEngineTests
{

// ---------------------------------------------------------------------------
// Stereo chorus (1.0.22). Tester: "Effect Chorus: It's in mono right now. A
// width parameter could be nice here too. Also adding this width will make it
// sounds less like a phaser like it does right now."
//
// juce::dsp::Chorus drives both channels from ONE LFO, so both sides sweep
// identically: the effect images dead centre and reads as a phaser, and JUCE
// exposes no per-channel LFO phase to fix it from outside. spa::dsp::
// StereoChorus replaces it with one modulated delay line per channel and a
// WIDTH control that sets the L/R LFO phase offset, plus a Vintage (Juno-ish
// BBD) / Modern (clean digital) mode selector.
//
// These tests MEASURE the two claims the change is being made on -- that it is
// genuinely stereo, and that Vintage is darker than Modern -- rather than just
// running the code. Nobody has listened to it; brightness and channel
// correlation are what is verified here, not whether Vintage sounds like a
// Juno.
namespace chorusTestHelpers
{
    struct Rendered
    {
        std::vector<float> left, right;
        float peak = 0.0f;
        bool finite = true;
    };

    // Deterministic noise, identical in both channels: any difference between
    // the rendered channels can then only have come from the chorus.
    inline void fillNoise (juce::AudioBuffer<float>& buf, juce::Random& rng, float amp)
    {
        for (int s = 0; s < buf.getNumSamples(); ++s)
        {
            const auto v = amp * (rng.nextFloat() * 2.0f - 1.0f);
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                buf.setSample (ch, s, v);
        }
    }

    inline spa::dsp::FXChain::Params baseParams()
    {
        spa::dsp::FXChain::Params p;
        p.chorusEnable = true;
        p.chorusMode = 1;          // Modern
        p.chorusRate = 1.5f;
        p.chorusDepth = 0.8f;
        p.chorusFeedback = 0.0f;
        p.chorusWidth = 1.0f;
        p.chorusMix = 1.0f;
        return p;
    }

    // Runs `totalSamples` of identical-in-both-channels noise through a fresh
    // FXChain in blocks, discarding `warmupSamples` while the delay line
    // fills, and returns both channels plus aggregate peak/finiteness. No
    // assertions in here -- callers aggregate and assert once per property.
    inline Rendered render (const spa::dsp::FXChain::Params& p, double sr,
                            int totalSamples, int warmupSamples, float amp = 0.4f,
                            int seed = 20220922)
    {
        constexpr int blockSize = 256;
        spa::dsp::FXChain fx;
        fx.prepare (sr, blockSize);

        juce::Random rng (seed);
        juce::AudioBuffer<float> buf (2, blockSize);
        Rendered out;
        out.left.reserve ((size_t) totalSamples);
        out.right.reserve ((size_t) totalSamples);

        int done = 0;
        while (done < warmupSamples + totalSamples)
        {
            fillNoise (buf, rng, amp);
            fx.process (buf, p);

            for (int s = 0; s < blockSize; ++s)
            {
                const auto l = buf.getSample (0, s);
                const auto r = buf.getSample (1, s);
                if (! std::isfinite (l) || ! std::isfinite (r))
                    out.finite = false;
                out.peak = juce::jmax (out.peak, std::abs (l), std::abs (r));

                if (done + s >= warmupSamples && (int) out.left.size() < totalSamples)
                {
                    out.left.push_back (l);
                    out.right.push_back (r);
                }
            }
            done += blockSize;
        }
        return out;
    }

    // Normalised cross-correlation of the two channels: 1 = identical (what a
    // mono-imaging chorus gives you), 0 = unrelated.
    inline double channelCorrelation (const Rendered& r)
    {
        double num = 0.0, dl = 0.0, dr = 0.0;
        for (size_t i = 0; i < r.left.size(); ++i)
        {
            num += (double) r.left[i] * (double) r.right[i];
            dl  += (double) r.left[i] * (double) r.left[i];
            dr  += (double) r.right[i] * (double) r.right[i];
        }
        const auto den = std::sqrt (dl * dr);
        return den > 1.0e-12 ? num / den : 0.0;
    }

    // RMS of the sample-by-sample difference between two renders, relative to
    // the first one's own RMS. 0 means the two renders are identical.
    inline double relativeDifference (const std::vector<float>& a, const std::vector<float>& b)
    {
        const auto n = juce::jmin (a.size(), b.size());
        double diff = 0.0, ref = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            const auto d = (double) a[i] - (double) b[i];
            diff += d * d;
            ref += (double) a[i] * (double) a[i];
        }
        return ref > 1.0e-12 ? std::sqrt (diff / ref) : 0.0;
    }

    // Level-independent brightness proxy: first-difference energy over total
    // energy. A first difference is a high-pass, so a signal with its top end
    // rolled off scores lower whatever its overall level.
    inline double hfRatio (const std::vector<float>& x)
    {
        double diff = 0.0, total = 0.0;
        for (size_t i = 1; i < x.size(); ++i)
        {
            const auto d = (double) x[i] - (double) x[i - 1];
            diff += d * d;
            total += (double) x[i] * (double) x[i];
        }
        return total > 1.0e-12 ? diff / total : 0.0;
    }
}

// Registry lock for the two parameters the stereo chorus added. The mode
// choice order is serialized in presets and sessions, so it is append-only
// from here on.
static void chorusWidthModeParamsTest()
{
    std::cout << "chorusWidthModeParamsTest\n";
    namespace params = spa::params;
    namespace fx = spa::params::id::fx;

    const params::ParamDef* width = nullptr;
    const params::ParamDef* mode = nullptr;
    for (const auto& def : params::all())
    {
        if (def.id == fx::chorusWidth) width = &def;
        if (def.id == fx::chorusMode)  mode = &def;
    }

    expect (width != nullptr, "fxChorus.width is in the registry");
    if (width != nullptr)
    {
        expect (std::abs (width->defaultValue - 50.0f) < 0.001f,
                "chorus width defaults to 50 % (0 % would be the mono sound this fixes; got "
                + juce::String (width->defaultValue) + ")");
        expect (std::abs (width->range.start) < 0.001f && std::abs (width->range.end - 100.0f) < 0.001f,
                "chorus width range is 0..100");
        expect (width->unit == "%", "chorus width is a percentage");
        expect (width->random.enabled, "chorus width is randomizable");
    }

    expect (mode != nullptr, "fxChorus.mode is in the registry");
    if (mode != nullptr)
    {
        expect (mode->choices.size() == 3, "chorus mode has exactly 3 choices (got "
                + juce::String (mode->choices.size()) + ")");
        expect (mode->choices[0] == "Vintage", "chorus mode index 0 is Vintage (append-only)");
        expect (mode->choices[1] == "Modern", "chorus mode index 1 is Modern (append-only)");
        expect (mode->choices[2] == "VHS", "chorus mode index 2 is VHS (appended, SPASynth 1.0.32)");
        expect (std::abs (mode->defaultValue - 1.0f) < 0.001f,
                "chorus mode defaults to Modern, closest to the engine it replaced");
        expect (mode->random.enabled, "chorus mode is randomizable");
    }
}

// THE point of the change: with WIDTH up, the two channels genuinely differ.
// Identical noise goes into both channels, so the only thing that can
// decorrelate them is the per-channel LFO phase offset.
static void chorusStereoWidthTest()
{
    std::cout << "chorusStereoWidthTest\n";
    using namespace chorusTestHelpers;

    constexpr double sr = 48000.0;

    auto p = baseParams();
    p.chorusWidth = 0.0f;
    const auto narrow = render (p, sr, 48000, 4096);
    const auto corrNarrow = channelCorrelation (narrow);

    p.chorusWidth = 0.5f;
    const auto mid = render (p, sr, 48000, 4096);
    const auto corrMid = channelCorrelation (mid);

    p.chorusWidth = 1.0f;
    const auto wide = render (p, sr, 48000, 4096);
    const auto corrWide = channelCorrelation (wide);

    expect (narrow.finite && wide.finite && mid.finite, "chorus width renders stay finite");

    expect (corrNarrow > 0.99,
            "chorus at width 0 is effectively mono (L/R correlation "
            + juce::String (corrNarrow, 4) + ")");
    expect (corrWide < 0.8,
            "chorus at max width is genuinely stereo (L/R correlation "
            + juce::String (corrWide, 4) + ", well below 1)");
    expect (corrNarrow - corrWide > 0.2,
            "width 0 is far more correlated than max width (" + juce::String (corrNarrow, 4)
            + " vs " + juce::String (corrWide, 4) + ")");
    // Channel correlation saturates: for broadband material, a delay
    // difference of more than a fraction of a sample already decorrelates the
    // two channels almost completely, so correlation cannot tell half width
    // from full width (measured: both land near zero). WIDTH being a
    // continuous control rather than a switch is therefore checked on the
    // rendered audio itself -- the right channel must differ from BOTH
    // extremes -- while the left channel, which carries no phase offset at
    // all, must be untouched by width.
    const auto midVsNarrow = relativeDifference (mid.right, narrow.right);
    const auto midVsWide = relativeDifference (mid.right, wide.right);
    const auto leftDrift = relativeDifference (mid.left, wide.left);

    expect (midVsNarrow > 0.2 && midVsWide > 0.2,
            "half width renders differently from both extremes (vs width 0: "
            + juce::String (midVsNarrow, 3) + ", vs max width: " + juce::String (midVsWide, 3)
            + "); correlation at half width was " + juce::String (corrMid, 4));
    expect (leftDrift < 1.0e-6,
            "width offsets only the right LFO -- the left channel is identical at every width "
            "(relative difference " + juce::String (leftDrift) + ")");
}

// Vintage's bucket-brigade voicing rolls the wet path off around 7 kHz;
// Modern runs full bandwidth. Same input, same everything else.
static void chorusVintageDarkerTest()
{
    std::cout << "chorusVintageDarkerTest\n";
    using namespace chorusTestHelpers;

    constexpr double sr = 48000.0;

    auto p = baseParams();
    p.chorusDepth = 0.3f;
    p.chorusRate = 0.8f;
    p.chorusWidth = 0.5f;

    p.chorusMode = 1;   // Modern
    const auto modern = render (p, sr, 48000, 4096, 0.3f);
    p.chorusMode = 0;   // Vintage
    const auto vintage = render (p, sr, 48000, 4096, 0.3f);

    expect (modern.finite && vintage.finite, "both chorus modes stay finite");

    const auto hfModern = hfRatio (modern.left);
    const auto hfVintage = hfRatio (vintage.left);

    expect (hfVintage < hfModern * 0.8,
            "Vintage is measurably darker than Modern (HF ratio " + juce::String (hfVintage, 4)
            + " vs " + juce::String (hfModern, 4) + ")");
}

// MIX is a plain dry/wet crossfade: 0 must be bit-for-bit the input, 1 must be
// the delayed signal only (which is silent until the delay line first reads
// back, something the dry signal could never be).
static void chorusMixTest()
{
    std::cout << "chorusMixTest\n";
    using namespace chorusTestHelpers;

    constexpr double sr = 48000.0;
    constexpr int n = 1024;

    // (a) mix = 0 leaves the input untouched.
    {
        spa::dsp::FXChain fx;
        fx.prepare (sr, n);
        auto p = baseParams();
        p.chorusMix = 0.0f;

        juce::Random rng (99);
        juce::AudioBuffer<float> buf (2, n);
        fillNoise (buf, rng, 0.7f);
        juce::AudioBuffer<float> dry (buf);

        fx.process (buf, p);

        float maxDiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < n; ++s)
                maxDiff = juce::jmax (maxDiff, std::abs (buf.getSample (ch, s) - dry.getSample (ch, s)));

        expect (maxDiff == 0.0f, "chorus mix=0 leaves the input untouched (max diff "
                + juce::String (maxDiff) + ")");
    }

    // (b) mix = 1 is fully wet. With depth 0 the Modern taps sit at a fixed
    // 12 ms and 8 ms, so the first 8 ms of output must be silent -- no dry
    // signal is leaking through.
    {
        spa::dsp::FXChain fx;
        fx.prepare (sr, n);
        auto p = baseParams();
        p.chorusMix = 1.0f;
        p.chorusDepth = 0.0f;
        p.chorusFeedback = 0.0f;

        juce::Random rng (101);
        juce::AudioBuffer<float> buf (2, n);
        fillNoise (buf, rng, 0.7f);
        const auto inputRms = buf.getRMSLevel (0, 0, n);

        fx.process (buf, p);

        const int shortestTapSamples = (int) (0.008 * sr);   // 8 ms
        float preTapPeak = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < shortestTapSamples - 8; ++s)
                preTapPeak = juce::jmax (preTapPeak, std::abs (buf.getSample (ch, s)));

        const auto tailRms = buf.getRMSLevel (0, shortestTapSamples + 64,
                                              n - shortestTapSamples - 64);

        expect (inputRms > 0.1f, "chorus mix=1 test input is substantial (RMS "
                + juce::String (inputRms, 4) + ")");
        expect (preTapPeak < 1.0e-6f,
                "chorus mix=1 is fully wet: nothing before the shortest delay tap (peak "
                + juce::String (preTapPeak) + ")");
        expect (tailRms > 0.05f, "chorus mix=1 does produce wet signal after the tap (RMS "
                + juce::String (tailRms, 4) + ")");
    }
}

// Feedback recirculates, so it is the one path here that could run away. At
// the knob's maximum, over a long render, both modes must stay finite and
// bounded.
static void chorusFeedbackStabilityTest()
{
    std::cout << "chorusFeedbackStabilityTest\n";
    using namespace chorusTestHelpers;

    constexpr double sr = 48000.0;

    for (int mode = 0; mode < 2; ++mode)
    {
        auto p = baseParams();
        p.chorusMode = mode;
        p.chorusFeedback = 0.9f;    // registry maximum
        p.chorusDepth = 1.0f;
        p.chorusWidth = 1.0f;
        p.chorusMix = 1.0f;

        // 10 seconds -- long enough for a comb at near-unity feedback to have
        // built to whatever it is going to build to.
        const auto r = render (p, sr, (int) sr * 10, 0, 0.5f);

        const juce::String label = mode == 0 ? "Vintage" : "Modern";
        expect (r.finite, "chorus " + label + " at max feedback stays finite over 10 s");
        expect (r.peak < 4.0f, "chorus " + label + " at max feedback stays bounded (peak "
                + juce::String (r.peak, 3) + ")");
        expect (r.peak > 0.1f, "chorus " + label + " at max feedback is still producing signal (peak "
                + juce::String (r.peak, 3) + ")");
    }
}

// Every corner of the parameter space, both modes: finite and sanely bounded.
// Aggregated, so this is one assertion per property rather than one per
// sample.
static void chorusParameterExtremesTest()
{
    std::cout << "chorusParameterExtremesTest\n";
    using namespace chorusTestHelpers;

    constexpr double sr = 48000.0;

    bool allFinite = true;
    float worstPeak = 0.0f;
    int combos = 0;

    for (int mode : { 0, 1 })
      for (float width : { 0.0f, 1.0f })
        for (float depth : { 0.0f, 1.0f })
          for (float feedback : { -0.9f, 0.0f, 0.9f })
            for (float rate : { 0.05f, 5.0f })
              for (float mix : { 0.0f, 1.0f })
              {
                  auto p = baseParams();
                  p.chorusMode = mode;
                  p.chorusWidth = width;
                  p.chorusDepth = depth;
                  p.chorusFeedback = feedback;
                  p.chorusRate = rate;
                  p.chorusMix = mix;

                  const auto r = render (p, sr, 2048, 0, 0.9f);
                  allFinite = allFinite && r.finite;
                  worstPeak = juce::jmax (worstPeak, r.peak);
                  ++combos;
              }

    expect (combos == 96, "chorus extremes covered 96 parameter corners (got "
            + juce::String (combos) + ")");
    expect (allFinite, "chorus output is finite at every parameter extreme, both modes");
    expect (worstPeak < 4.0f, "chorus output stays bounded at every extreme (worst peak "
            + juce::String (worstPeak, 3) + ")");
}

// --- Delay ping-pong WIDTH (1.0.25) -----------------------------------------
//
// Ping-pong (fxDelay.pingpong) only ever crossed the FEEDBACK paths; the
// injection was always left[i]->left line, right[i]->right line. For a
// mono/centre source (left==right) that makes ping-pong a no-op -- crossing
// two identical streams produces the same identical streams. WIDTH blends
// the injection itself from that old behaviour (0) to a true mono-sum-into-
// one-line bounce (1). See FXChain::processDelay.
namespace delayWidthTestHelpers
{
    using FX = spa::dsp::FXChain;

    // Runs `warmupSamples` of silence through a fresh FXChain so the
    // per-sample delaySamplesSmoothed/delayWidthSmoothed ramps (linear, sr*
    // 0.1s and sr*0.05s respectively) fully settle to their exact target
    // -- not just close to it -- before the impulse, so echo timing is
    // sample-exact and predictable. Then injects a single one-sample
    // impulse and renders `renderSamples` more, returning that render.
    inline void renderImpulseThroughDelay (const FX::Params& p, double sr, int blockSize,
                                            float impulseL, float impulseR, int renderSamples,
                                            std::vector<float>& outL, std::vector<float>& outR)
    {
        FX fx;
        fx.prepare (sr, blockSize);

        juce::AudioBuffer<float> buf (2, blockSize);

        // Both smoothers ramp over at most sr*0.1s; a healthy margin over
        // that in silent blocks settles them exactly (linear ramp, not
        // asymptotic). Block-aligned (a whole number of blockSize blocks)
        // so the impulse lands at the same absolute sample index here and
        // in the sample-at-a-time reference implementation below.
        const int minWarmupSamples = (int) (sr * 0.2) + blockSize;
        const int warmupBlocks = (minWarmupSamples + blockSize - 1) / blockSize;
        const int warmupSamples = warmupBlocks * blockSize;
        int done = 0;
        while (done < warmupSamples)
        {
            buf.clear();
            fx.process (buf, p);
            done += blockSize;
        }

        outL.clear(); outR.clear();
        outL.reserve ((size_t) renderSamples);
        outR.reserve ((size_t) renderSamples);

        bool firstBlock = true;
        int produced = 0;
        while (produced < renderSamples)
        {
            buf.clear();
            if (firstBlock)
            {
                buf.setSample (0, 0, impulseL);
                buf.setSample (1, 0, impulseR);
                firstBlock = false;
            }
            fx.process (buf, p);
            for (int s = 0; s < blockSize && produced < renderSamples; ++s, ++produced)
            {
                outL.push_back (buf.getSample (0, s));
                outR.push_back (buf.getSample (1, s));
            }
        }
    }

    // Reference reimplementation of the PRE-1.0.25 processDelay loop
    // (crossed feedback only, no width blend at all) -- used to pin that
    // WIDTH 0 %, ping-pong on, is bit-exact to what shipped before this
    // feature existed. Deliberately duplicated rather than calling
    // production code, so a regression in the real processDelay's width==0
    // path cannot silently drag this reference along with it.
    inline void renderImpulseThroughOldPingPongDelay (const FX::Params& p, double sr,
                                                        int blockSize, float impulseL,
                                                        float impulseR, int renderSamples,
                                                        std::vector<float>& outL,
                                                        std::vector<float>& outR)
    {
        juce::AudioBuffer<float> delayBuf (2, (int) (sr * 4.0) + 8);
        delayBuf.clear();
        auto* bufL = delayBuf.getWritePointer (0);
        auto* bufR = delayBuf.getWritePointer (1);
        const auto bufLen = delayBuf.getNumSamples();
        int writePos = 0;

        juce::SmoothedValue<float> delaySamplesSmoothed;
        delaySamplesSmoothed.reset (sr, 0.1);
        const auto timeSeconds = p.delaySync
                               ? spa::params::lfoDivisionBeats (p.delayDivision) * 60.0 / p.bpm
                               : (double) p.delayTimeMs * 0.001;
        const auto targetSamples = (float) juce::jlimit (
            32.0, (double) bufLen - 8.0, timeSeconds * sr);
        delaySamplesSmoothed.setTargetValue (targetSamples);

        // Must match renderImpulseThroughDelay's block-aligned warmup exactly
        // (both count silent samples the same way) so the impulse lands at
        // the identical absolute sample index in both renders.
        const int minWarmupSamples = (int) (sr * 0.2) + blockSize;
        const int warmupBlocks = (minWarmupSamples + blockSize - 1) / blockSize;
        const int warmupSamples = warmupBlocks * blockSize;
        const int totalSamples = warmupSamples + renderSamples;
        outL.clear(); outR.clear();
        outL.reserve ((size_t) renderSamples);
        outR.reserve ((size_t) renderSamples);

        for (int i = 0; i < totalSamples; ++i)
        {
            const float left = (i == warmupSamples) ? impulseL : 0.0f;
            const float right = (i == warmupSamples) ? impulseR : 0.0f;

            const auto delaySamples = delaySamplesSmoothed.getNextValue();
            auto readPos = (double) writePos - (double) delaySamples;
            while (readPos < 0.0) readPos += (double) bufLen;
            auto r0 = (int) readPos;
            const auto frac = (float) (readPos - (double) r0);
            while (r0 >= bufLen) r0 -= bufLen;
            const auto r1 = (r0 + 1) % bufLen;

            const auto outLv = bufL[r0] + frac * (bufL[r1] - bufL[r0]);
            const auto outRv = bufR[r0] + frac * (bufR[r1] - bufR[r0]);

            bufL[writePos] = left + (p.delayPingPong ? outRv : outLv) * p.delayFeedback;
            bufR[writePos] = right + (p.delayPingPong ? outLv : outRv) * p.delayFeedback;

            const auto sampleOutL = left + outLv * p.delayMix;
            const auto sampleOutR = right + outRv * p.delayMix;

            if (i >= warmupSamples)
            {
                outL.push_back (sampleOutL);
                outR.push_back (sampleOutR);
            }

            writePos = (writePos + 1) % bufLen;
        }
    }

    inline FX::Params delayBaseParams()
    {
        FX::Params p;
        p.delayEnable = true;
        p.delaySync = false;
        p.delayTimeMs = 50.0f;      // 2400 samples at 48k
        p.delayFeedback = 0.5f;
        p.delayMix = 1.0f;
        p.delayPingPong = true;
        p.delayWidth = 1.0f;
        return p;
    }
}

// STEP 0 measurement (see the task brief): mono impulse, ping-pong on,
// width 1 -> echoes alternate hard L/R; width 0 -> today's pre-1.0.25
// ping-pong, which is a no-op on a centred/mono source (both channels equal
// at every echo, because crossing two identical feedback streams changes
// nothing).
static void delayPingPongWidthTest()
{
    std::cout << "delayPingPongWidthTest\n";
    using namespace delayWidthTestHelpers;

    constexpr double sr = 48000.0;
    constexpr int blockSize = 256;
    constexpr int delaySamples = 2400;   // 50ms at 48k
    constexpr int renderSamples = delaySamples * 4 + 200;

    auto echoPeaks = [&] (const std::vector<float>& l, const std::vector<float>& r, int echoIndex)
    {
        // Reads within a narrow window around the exact predicted echo
        // sample (see the derivation in the task's worked-through math:
        // echo k lands at i = k * delaySamples, sample-exact once the
        // smoothers are fully settled).
        const int centre = echoIndex * delaySamples;
        const int lo = juce::jmax (0, centre - 4);
        const int hi = juce::jmin ((int) l.size(), centre + 5);
        juce::AudioBuffer<float> tmp (2, hi - lo);
        for (int i = lo; i < hi; ++i)
        {
            tmp.setSample (0, i - lo, l[(size_t) i]);
            tmp.setSample (1, i - lo, r[(size_t) i]);
        }
        return std::make_pair (tmp.getMagnitude (0, 0, hi - lo), tmp.getMagnitude (1, 0, hi - lo));
    };

    // --- width 1: hard alternating bounce ---
    {
        auto p = delayBaseParams();
        p.delayWidth = 1.0f;
        std::vector<float> l, r;
        renderImpulseThroughDelay (p, sr, blockSize, 1.0f, 1.0f, renderSamples, l, r);

        const auto [e1l, e1r] = echoPeaks (l, r, 1);
        const auto [e2l, e2r] = echoPeaks (l, r, 2);

        // Both alternating sides have measurable energy (echo 1 on the left,
        // echo 2 on the right) -- the OTHER channel at each echo is
        // predicted to be exactly 0 (a fully isolated mono-sum bounce at
        // width 1), so this only checks the loud side of each.
        expect (e1l > 1.0e-4f && e2r > 1.0e-4f, "width=1: both echoes have measurable energy "
                "(echo1 L=" + juce::String (e1l, 6) + " R=" + juce::String (e1r, 6)
                + ", echo2 L=" + juce::String (e2l, 6) + " R=" + juce::String (e2r, 6) + ")");
        expect (e1l > e1r * 10.0f, "width=1 echo 1 is predominantly LEFT (L="
                + juce::String (e1l, 6) + " R=" + juce::String (e1r, 6) + ")");
        expect (e2r > e2l * 10.0f, "width=1 echo 2 is predominantly RIGHT, i.e. it bounced ("
                "L=" + juce::String (e2l, 6) + " R=" + juce::String (e2r, 6) + ")");
    }

    // --- width 0: matches today's (pre-1.0.25) pingpong-on-mono behaviour --
    // no bounce, both channels track together at every echo.
    {
        auto p = delayBaseParams();
        p.delayWidth = 0.0f;
        std::vector<float> l, r;
        renderImpulseThroughDelay (p, sr, blockSize, 1.0f, 1.0f, renderSamples, l, r);

        const auto [e1l, e1r] = echoPeaks (l, r, 1);
        expect (e1l > 1.0e-4f && e1r > 1.0e-4f, "width=0: echo 1 has measurable energy on "
                "both channels (L=" + juce::String (e1l, 6) + " R=" + juce::String (e1r, 6) + ")");
        expect (std::abs (e1l - e1r) < 1.0e-5f, "width=0 on a mono source: L and R echo peaks "
                "are equal, i.e. ping-pong is still a no-op here, as before this feature "
                "(L=" + juce::String (e1l, 6) + " R=" + juce::String (e1r, 6) + ")");
    }
}

// Ping-pong OFF: WIDTH must have zero effect -- sample-exact identical
// output whether WIDTH is 0 or 1.
static void delayWidthNoOpWhenPingPongOffTest()
{
    std::cout << "delayWidthNoOpWhenPingPongOffTest\n";
    using namespace delayWidthTestHelpers;

    constexpr double sr = 48000.0;
    constexpr int blockSize = 256;
    constexpr int renderSamples = 2400 * 3;

    auto render = [&] (float width)
    {
        auto p = delayBaseParams();
        p.delayPingPong = false;
        p.delayWidth = width;
        std::vector<float> l, r;
        // Hard-left impulse (not mono) so a width bug that leaked through
        // the disabled gate would be visible even without ping-pong summing.
        renderImpulseThroughDelay (p, sr, blockSize, 1.0f, 0.0f, renderSamples, l, r);
        return std::make_pair (l, r);
    };

    const auto [l0, r0] = render (0.0f);
    const auto [l1, r1] = render (1.0f);

    // Bit-exact compare via memcmp rather than == on floats (sample-exact
    // reproducibility is exactly the property under test, so this is
    // deliberate, not a tolerance-loosening shortcut).
    bool identical = l0.size() == l1.size() && r0.size() == r1.size()
                   && std::memcmp (l0.data(), l1.data(), l0.size() * sizeof (float)) == 0
                   && std::memcmp (r0.data(), r1.data(), r0.size() * sizeof (float)) == 0;

    expect (identical, "ping-pong OFF: WIDTH 0 and WIDTH 1 render sample-exact identical output");
}

// WIDTH 0 with ping-pong ON must be bit-exact to the pre-1.0.25 algorithm
// (crossed feedback only, direct left/right injection, no width blend at
// all) -- reimplemented independently above as
// renderImpulseThroughOldPingPongDelay.
static void delayWidthZeroMatchesPreChangeAlgorithmTest()
{
    std::cout << "delayWidthZeroMatchesPreChangeAlgorithmTest\n";
    using namespace delayWidthTestHelpers;

    constexpr double sr = 48000.0;
    constexpr int blockSize = 256;
    constexpr int renderSamples = 2400 * 3;

    auto p = delayBaseParams();
    p.delayWidth = 0.0f;

    std::vector<float> newL, newR, oldL, oldR;
    renderImpulseThroughDelay (p, sr, blockSize, 1.0f, 0.35f, renderSamples, newL, newR);
    renderImpulseThroughOldPingPongDelay (p, sr, blockSize, 1.0f, 0.35f, renderSamples, oldL, oldR);

    bool identical = newL.size() == oldL.size() && newR.size() == oldR.size()
                   && std::memcmp (newL.data(), oldL.data(), newL.size() * sizeof (float)) == 0
                   && std::memcmp (newR.data(), oldR.data(), newR.size() * sizeof (float)) == 0;

    expect (identical, "WIDTH 0 %, ping-pong on: bit-exact to the independently reimplemented "
            "pre-1.0.25 algorithm");
}


namespace
{
    struct ReverbBuildup { double t60Ms = -1.0, t30Ms = -1.0, t12Ms = -1.0, tPeakMs = -1.0; double peakVal = 0.0; };

    ReverbBuildup measureReverbBuildup (int mode, float size, float preMs, double sr)
    {
        using FX = spa::dsp::FXChain;
        constexpr int block = 256;
        FX fx;
        fx.prepare (sr, block);
        FX::Params p;
        p.reverbEnable = true;
        p.reverbMode = mode;
        p.reverbPreDelay = preMs;
        p.reverbSize = size;
        p.reverbMix = 1.0f;
        // decay/damping/modDepth/lowCut/highCut/width left at FX::Params'
        // own defaults, which match the ParameterRegistry defaults exactly
        // (verified against ParameterRegistry.cpp above).

        const double renderSeconds = 3.0;
        const int totalSamples = (int) (renderSeconds * sr);
        const int windowSamples = juce::jmax (1, (int) std::round (0.001 * sr)); // ~1ms peak-hold window
        std::vector<float> envelope;
        envelope.reserve ((size_t) (totalSamples / windowSamples) + 2);

        juce::AudioBuffer<float> buf (2, block);
        float windowPeak = 0.0f;
        int windowCount = 0;
        bool firstBlock = true;
        for (int done = 0; done < totalSamples; done += block)
        {
            buf.clear();
            if (firstBlock) { buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f); firstBlock = false; }
            fx.process (buf, p);
            for (int s = 0; s < block; ++s)
            {
                const float mono = 0.5f * (buf.getSample (0, s) + buf.getSample (1, s));
                windowPeak = juce::jmax (windowPeak, std::abs (mono));
                if (++windowCount >= windowSamples)
                {
                    envelope.push_back (windowPeak);
                    windowPeak = 0.0f;
                    windowCount = 0;
                }
            }
        }
        if (windowCount > 0)
            envelope.push_back (windowPeak);

        ReverbBuildup out;
        size_t peakIdx = 0; float peakVal = 0.0f;
        for (size_t i = 0; i < envelope.size(); ++i)
            if (envelope[i] > peakVal) { peakVal = envelope[i]; peakIdx = i; }
        out.peakVal = (double) peakVal;
        if (peakVal <= 0.0f)
            return out; // leave times at -1 (never crossed)
        out.tPeakMs = (double) peakIdx * (double) windowSamples / sr * 1000.0;

        auto thresholdTimeMs = [&] (double db) -> double
        {
            const float thr = peakVal * (float) std::pow (10.0, db / 20.0);
            for (size_t i = 0; i < envelope.size(); ++i)
                if (envelope[i] >= thr)
                    return (double) i * (double) windowSamples / sr * 1000.0;
            return -1.0;
        };
        out.t60Ms = thresholdTimeMs (-60.0);
        out.t30Ms = thresholdTimeMs (-30.0);
        out.t12Ms = thresholdTimeMs (-12.0);
        return out;
    }
}

// --- 1.0.25 reverb arrival fix: correctness pins -----------------------

// Pins the actual defect reverbPreDelayReportTest measured (opt-in, human-
// read only): before 1.0.25, PlateReverb's wet tap was ONE end-of-line
// read (delay2 at its full, scaled length), so a sample had to traverse the
// WHOLE of delay1+decayAP+delay2 in a branch before any wet energy could
// reach the output at all -- at Hall SIZE 0.5 that was ~285ms after a 32ms
// PRE-DELAY (tester Paul: "says 32ms but sounds more like 320ms"), and once
// energy did arrive it arrived essentially all at once (every threshold
// crossing landing in the same ~1ms measurement window), not as a building
// envelope. The fix (Dattorro's own distributed multi-tap output,
// PlateReverb.h) reads several points along each branch, so early energy
// reaches the output almost immediately after the input diffusers.
//
// For every mode x SIZE {0, 0.5, 1} x PRE {0, 32, 100} ms: the first wet
// energy above -60dB (re the envelope's own eventual peak) must arrive
// within PRE + 25ms (25ms is the input-diffusion chain's own latency --
// four series allpasses plus a modulated allpass each leak an instant
// scaled copy of their input per the standard allpass structure, so this
// is generous, not tight), and the envelope's own PEAK must arrive
// meaningfully LATER than that first crossing -- proving the reverb
// actually builds/blooms over some tens of ms rather than jumping straight
// to full level in one step (which is what the old code did, just 285ms
// later than the knob). NOTE: an earlier version of this test instead
// required the -60dB and -12dB crossings to land in different 1ms windows,
// modelled directly on the old bug's description -- that measured false
// on Hall/Plate/Chamber even after the fix, because the FIRST of the new
// early taps to arrive is a single discrete reflection (the diffuser chain
// applied to a single-sample test impulse stays sparse for the first
// several ms, not a dense continuum), and one early reflection legitimately
// arriving within 12dB of the eventual peak is not a defect -- a real
// room's first reflection can be prominent too. tPeakMs vs t60Ms is the
// more robust and still fully anti-vacuous discriminator: it directly
// measures "does the tail keep building for a while", which the old
// end-of-line-only tap could never do (once ANYTHING arrived, the whole
// already-recirculated tank dumped out essentially at once).
static void reverbArrivalFollowsPreDelayTest()
{
    std::cout << "reverbArrivalFollowsPreDelayTest\n";

    const struct { int idx; const char* name; } modes[] = {
        { 0, "Hall" }, { 1, "Plate" }, { 2, "Chamber" }, { 3, "Room" }, { 4, "Spring" }
    };
    const float sizes[] = { 0.0f, 0.5f, 1.0f };
    const float pres[] = { 0.0f, 32.0f, 100.0f };
    constexpr double sr = 48000.0;

    for (const auto& m : modes)
    {
        for (float size : sizes)
        {
            for (float pre : pres)
            {
                const auto r = measureReverbBuildup (m.idx, size, pre, sr);
                expect (r.peakVal > 0.0,
                        juce::String (m.name) + " size " + juce::String (size, 2) + " pre "
                        + juce::String (pre, 0) + "ms produces a measurable wet impulse response");
                if (r.peakVal <= 0.0) continue;

                expect (r.t60Ms >= 0.0 && r.t60Ms <= pre + 25.0,
                        juce::String (m.name) + " size " + juce::String (size, 2) + " pre "
                        + juce::String (pre, 0) + "ms: wet energy arrives within PRE+25ms (measured "
                        + juce::String (r.t60Ms, 2) + "ms, bound " + juce::String (pre + 25.0, 1) + "ms)");

                expect (r.tPeakMs >= 0.0 && (r.tPeakMs - r.t60Ms) >= 5.0,
                        juce::String (m.name) + " size " + juce::String (size, 2) + " pre "
                        + juce::String (pre, 0) + "ms: the envelope BUILDS -- the peak arrives well "
                        "after the first crossing, not in the same instant (t-60dB "
                        + juce::String (r.t60Ms, 2) + "ms, tPeak " + juce::String (r.tPeakMs, 2) + "ms)");
            }
        }
    }
}

// Stress-proves the new taps' bounds by MEASUREMENT, not just by reading
// the code: every tap offset is computed via tapBack() (PlateReverb.h),
// which clamps into [1, lineLen-1] of that line's CURRENT (already size/
// rate-scaled) length -- this test exercises the full cross product the
// 1.0.24 note flagged as "confirmed by reading, not by measurement" (the
// plate's index bounds) and extends it to the new taps. An out-of-range
// Ring read is a real heap access (Ring::buf is a std::vector, sized to
// kMaxSizeScale's worst case) so ASan will trap any bug this test's own
// finite/silence checks might miss; this test is meant to be run under the
// ASan build as well as the plain one (see the verification ritual).
static void plateReverbIndexStressTest()
{
    std::cout << "plateReverbIndexStressTest\n";
    using FX = spa::dsp::FXChain;

    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    const float sizes[] = { 0.0f, 0.01f, 0.5f, 0.99f, 1.0f };
    const float modDepths[] = { 0.0f, 1.0f };
    constexpr int block = 128;

    uint32_t rng = 424242u;
    auto noise = [&rng]
    {
        rng = rng * 1664525u + 1013904223u;
        return ((float) (rng >> 9) / (float) (1u << 23)) * 2.0f - 1.0f;
    };

    int combosRun = 0;
    for (int mode = 0; mode < 5; ++mode)
    {
        for (double sr : rates)
        {
            for (float size : sizes)
            {
                for (float modDepth : modDepths)
                {
                    FX fx;
                    fx.prepare (sr, block);
                    bool finite = true;
                    float peak = 0.0f;

                    // ~0.4s: random noise, an impulse, more noise, silence --
                    // and the reverb's own params (size/decay/damping/predelay/
                    // width/mix) change EVERY block, mid-stream, exercising the
                    // geometry recompute (scale, geom[].*Len, the tap offsets)
                    // under constantly shifting line lengths, not just a single
                    // fixed configuration.
                    const int totalBlocks = (int) (0.4 * sr / block) + 1;
                    juce::AudioBuffer<float> buf (2, block);
                    for (int b = 0; b < totalBlocks; ++b)
                    {
                        FX::Params p;
                        p.reverbEnable = true;
                        p.reverbMode = mode;
                        p.reverbSize = juce::jlimit (0.0f, 1.0f, size + 0.05f * noise());
                        p.reverbModDepth = modDepth;
                        p.reverbDecay = 0.15f + 4.85f * (0.5f + 0.5f * noise());
                        p.reverbDamping = 0.5f + 0.5f * noise();
                        p.reverbPreDelay = juce::jmax (0.0f, 40.0f + 40.0f * noise());
                        p.reverbWidth = 0.5f + 0.5f * noise();
                        p.reverbMix = 1.0f;

                        buf.clear();
                        if (b % 7 == 0)
                            for (int s = 0; s < block; ++s)
                            {
                                buf.setSample (0, s, noise() * 0.9f);
                                buf.setSample (1, s, noise() * 0.9f);
                            }
                        else if (b % 11 == 0)
                        {
                            buf.setSample (0, 0, 1.0f);
                            buf.setSample (1, 0, 1.0f);
                        }

                        fx.process (buf, p);
                        for (int ch = 0; ch < 2; ++ch)
                            for (int s = 0; s < block; ++s)
                            {
                                const float v = buf.getSample (ch, s);
                                if (! std::isfinite (v)) finite = false;
                                peak = juce::jmax (peak, std::abs (v));
                            }
                    }

                    expect (finite,
                            "mode " + juce::String (mode) + " sr " + juce::String (sr, 0)
                            + " size " + juce::String (size, 2) + " modDepth " + juce::String (modDepth, 1)
                            + " stays finite across shifting geometry");
                    expect (peak < 50.0f,
                            "mode " + juce::String (mode) + " sr " + juce::String (sr, 0)
                            + " size " + juce::String (size, 2) + " modDepth " + juce::String (modDepth, 1)
                            + " stays bounded, no runaway from an OOB tap read (peak "
                            + juce::String (peak) + ")");
                    ++combosRun;
                }
            }
        }
    }
    expect (combosRun == 5 * 5 * 5 * 2,
            "every mode x rate x size x modDepth combination ran (" + juce::String (combosRun) + ")");
}

// Reported tail = real -60 dB feedback ring-out, ceiling 300 s. It was clamped
// to 12 s, so hosts stopped there and cut long delay decays off.
static void delayTailLengthTest()
{
    std::cout << "delayTailLengthTest\n";
    spa::dsp::FXChain fx;
    auto tailFor = [&] (float ms, float fb)
    {
        spa::dsp::FXChain::Params p;
        p.reverbEnable = false; p.convEnable = false; p.grainEnable = false;
        p.delayEnable = true; p.delaySync = false; p.delayTimeMs = ms; p.delayFeedback = fb;
        return fx.tailSeconds (p);
    };
    expect (std::abs (tailFor (350.0f, 0.95f) - 47.1) < 0.3, "350 ms / 0.95 -> ~47 s");
    expect (std::abs (tailFor (350.0f, 0.80f) - 10.8) < 0.1, "350 ms / 0.80 -> ~10.8 s");
    expect (std::abs (tailFor (2000.0f, 0.95f) - 269.4) < 0.5, "2 s / 0.95 -> ~269 s");
    expect (tailFor (4000.0f, 0.999f) <= 300.0 && tailFor (4000.0f, 0.999f) > 299.99, "ceiling 300 s holds");

    spa::dsp::FXChain::Params g;
    g.reverbEnable = false; g.convEnable = false; g.delayEnable = false; g.grainEnable = true;
    g.grainPositionMs = 4000.0f; g.grainSizeMs = 500.0f; g.grainFeedback = 0.999f; g.grainReleaseSec = 0.0f;
    expect (fx.tailSeconds (g) > 12.0 && fx.tailSeconds (g) <= 300.0, "GLITTER tail not capped at 12 s, ceiling 300 s");
}

} // namespace fxEngineTests
