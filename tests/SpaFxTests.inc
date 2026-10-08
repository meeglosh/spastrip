// SPAStrip adopts the shared spa-fx module (libs/spa-fx, pinned by libs/spa-fx.pin).
// Every module that moved is proven against a FROZEN copy of SPAStrip 1.0.3's own code
// (tests/legacy/*_103.h, namespace spa::dsp::legacy103). The frozen FXChain
// (tests/legacy/FXChain_103.h) is the complete 1.0.3 chain, so the wiring around each
// module (parameter plumbing, enable gating, filter glue, convolution engine) is covered
// as well as the DSP. Included from SPAStripTests.cpp, where `expect` is in scope.
//
// Expected results: bit-identical for everything except
//   - REVERB: level normalisation (spa-fx PlateReverb) -- identical at the reference cell
//     (SIZE 0.5, DECAY 2 s), and pinned by reverbLevelConstantAcrossSizeDecayTest elsewhere;
//   - GLITTER: the RELEASE tail fix and the removal of FREEZE -- identical with RELEASE off
//     or infinite (the old FREEZE), different on purpose with RELEASE active.

namespace spaFxTests
{

inline bool bitEq (float a, float b) { return std::memcmp (&a, &b, sizeof (float)) == 0; }

inline uint32_t lcg (uint32_t& rng) { rng = rng * 1664525u + 1013904223u; return rng; }
inline float noiseOf (uint32_t& rng) { return ((float) (lcg (rng) >> 9) / (float) (1u << 23)) * 2.0f - 1.0f; }

// Feeds identical noise (first 6 blocks, then silence so tails and feedback are compared
// too) to both engines, applies a per-block parameter schedule to each, and memcmp's every
// channel of every block.
template <typename Old, typename New, typename Run>
inline bool compare (Old& oldFx, New& newFx, int channels, int blocks, int n, uint32_t seed, Run&& run)
{
    uint32_t rng = seed;
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> a (channels, n), c (channels, n);
        for (int cc = 0; cc < channels; ++cc)
            for (int i = 0; i < n; ++i) { const float v = b < 6 ? noiseOf (rng) * 0.5f : 0.0f; a.setSample (cc, i, v); c.setSample (cc, i, v); }
        run (b, oldFx, newFx, a, c);
        for (int cc = 0; cc < channels; ++cc)
            if (std::memcmp (a.getReadPointer (cc), c.getReadPointer (cc), sizeof (float) * (size_t) n) != 0)
                return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Class-level bit identity (the shared class vs SPAStrip's frozen header)
// ---------------------------------------------------------------------------------------

void chorusBitIdenticalTest()
{
    std::cout << "spaFxChorusBitIdenticalTest\n";
    using Old = spa::dsp::legacy103::StereoChorus;
    using New = spa::fx::StereoChorus;
    constexpr int n = 512, blocks = 14;
    int cells = 0, mismatches = 0;
    for (double sr : { 44100.0, 96000.0 })
        for (int mode = 0; mode < 3; ++mode)
            for (float rt : { 0.1f, 0.8f, 5.0f })
                for (float dp : { 0.0f, 0.3f, 1.0f })
                    for (float wd : { 0.0f, 0.5f, 1.0f })
                        for (float fb : { 0.0f, -0.5f, 0.9f })
                            for (int ch = 1; ch <= 2; ++ch)
                            {
                                Old oldC; New newC;
                                oldC.prepare (sr, 512); newC.prepare (sr, 512);
                                const int k = cells;
                                const bool same = compare (oldC, newC, ch, blocks, n, 99u + (uint32_t) cells,
                                    [&] (int b, Old& o, New& nw, juce::AudioBuffer<float>& a, juce::AudioBuffer<float>& c)
                                    {
                                        const int m = b < 5 ? mode : (mode + 1) % 3;
                                        const float d = b < 7 ? dp : 1.0f - dp;
                                        const float w = b < 7 ? wd : 1.0f - wd;
                                        const bool en = ! (b >= 9 && b < 11);
                                        Old::Params po; New::Params pn;
                                        po.enable = pn.enable = en;
                                        po.mode = m == 0 ? Old::Mode::vintage : m == 1 ? Old::Mode::modern : Old::Mode::vhs;
                                        pn.mode = m == 0 ? New::Mode::vintage : m == 1 ? New::Mode::modern : New::Mode::vhs;
                                        po.rateHz = pn.rateHz = rt; po.depth = pn.depth = d; po.width = pn.width = w;
                                        po.feedback = pn.feedback = fb; po.mix = pn.mix = (k % 2) ? 0.5f : 1.0f;
                                        const float vk = (float) (k % 5) * 0.25f;
                                        po.vhsWow = pn.vhsWow = vk; po.vhsFlutter = pn.vhsFlutter = 1.0f - vk;
                                        po.vhsTone = pn.vhsTone = vk; po.vhsSat = pn.vhsSat = 1.0f - vk;
                                        po.vhsHiss = pn.vhsHiss = 0.5f + 0.5f * vk; po.vhsDropouts = pn.vhsDropouts = vk;
                                        o.process (a, po);
                                        nw.process (c.getArrayOfWritePointers(), c.getNumChannels(), n, pn);
                                    });
                                ++cells; if (! same) ++mismatches;
                            }
    expect (mismatches == 0, "spa-fx StereoChorus bit-identical to SPAStrip 1.0.3 over " + juce::String (cells)
                                 + " cells (" + juce::String (mismatches) + " mismatches)");
}

void modEffectBitIdenticalTest()
{
    std::cout << "spaFxModEffectBitIdenticalTest\n";
    using Old = spa::dsp::legacy103::ModEffect;
    using New = spa::fx::ModEffect;
    constexpr int n = 512, blocks = 14;
    int cells = 0, mismatches = 0;
    for (double sr : { 44100.0, 96000.0 })
        for (int type = 0; type < 2; ++type)
            for (int st : { 2, 6, 12 })
                for (float rt : { 0.02f, 0.5f, 8.0f })
                    for (float dp : { 0.0f, 0.5f, 1.0f })
                        for (float fb : { -0.9f, 0.0f, 0.9f })
                            for (int ch = 1; ch <= 2; ++ch)
                            {
                                Old oldM; New newM;
                                oldM.prepare (sr, 512); newM.prepare (sr, 512);
                                const int k = cells;
                                const bool same = compare (oldM, newM, ch, blocks, n, 31u + (uint32_t) cells,
                                    [&] (int b, Old& o, New& nw, juce::AudioBuffer<float>& a, juce::AudioBuffer<float>& c)
                                    {
                                        const int t = b < 5 ? type : 1 - type;
                                        const float d = b < 7 ? dp : 1.0f - dp;
                                        Old::Params po; New::Params pn;
                                        po.enable = pn.enable = ! (b >= 9 && b < 11);
                                        po.type = t == 0 ? Old::Type::phaser : Old::Type::flanger;
                                        pn.type = t == 0 ? New::Type::phaser : New::Type::flanger;
                                        po.rateHz = pn.rateHz = rt; po.depth = pn.depth = d; po.feedback = pn.feedback = fb;
                                        po.stages = pn.stages = b < 7 ? st : 14 - st;
                                        const float centres[] = { 200.0f, 800.0f, 4000.0f };
                                        const float manuals[] = { 0.1f, 3.0f, 20.0f };
                                        po.centreHz = pn.centreHz = centres[k % 3];
                                        po.manualMs = pn.manualMs = manuals[(k / 3) % 3];
                                        po.spread = pn.spread = (float) (k % 3) * 0.5f;
                                        po.mix = pn.mix = (k % 2) ? 0.5f : 1.0f;
                                        o.process (a, po);
                                        nw.process (c.getArrayOfWritePointers(), c.getNumChannels(), n, pn);
                                    });
                                ++cells; if (! same) ++mismatches;
                            }
    expect (mismatches == 0, "spa-fx ModEffect bit-identical to SPAStrip 1.0.3 over " + juce::String (cells)
                                 + " cells (" + juce::String (mismatches) + " mismatches)");
}

void tremVibBitIdenticalTest()
{
    std::cout << "spaFxTremVibBitIdenticalTest\n";
    using Old = spa::dsp::legacy103::TremVib;
    using New = spa::fx::TremVib;
    constexpr int n = 512, blocks = 14;
    int cells = 0, mismatches = 0;
    for (double sr : { 44100.0, 96000.0 })
        for (int shape = 0; shape < 4; ++shape)
            for (float rt : { 0.1f, 5.0f, 20.0f })
                for (float dp : { 0.0f, 0.5f, 1.0f })
                    for (float stt : { 0.0f, 0.5f, 1.0f })
                        for (int vib = 0; vib < 2; ++vib)
                            for (float vd : { 0.0f, 0.5f, 1.0f })
                                for (int ch = 1; ch <= 2; ++ch)
                                {
                                    Old oldT; New newT;
                                    oldT.prepare (sr, 512); newT.prepare (sr, 512);
                                    const int k = cells;
                                    const bool same = compare (oldT, newT, ch, blocks, n, 7u + (uint32_t) cells,
                                        [&] (int b, Old& o, New& nw, juce::AudioBuffer<float>& a, juce::AudioBuffer<float>& c)
                                        {
                                            Old::Params po; New::Params pn;
                                            po.tremOn = pn.tremOn = ! (b >= 9 && b < 11);
                                            po.vibOn = pn.vibOn = (vib != 0) != (b >= 7 && b < 9);
                                            po.tremShape = pn.tremShape = b < 5 ? shape : (shape + 1) % 4;
                                            po.tremRateHz = pn.tremRateHz = rt;
                                            po.tremDepth = pn.tremDepth = b < 7 ? dp : 1.0f - dp;
                                            po.tremStereo = pn.tremStereo = stt;
                                            po.tremMix = pn.tremMix = (k % 2) ? 0.5f : 1.0f;
                                            po.vibRateHz = pn.vibRateHz = rt * 0.5f + 0.2f;
                                            po.vibDepth = pn.vibDepth = b < 7 ? vd : 1.0f - vd;
                                            po.vibMix = pn.vibMix = (k % 3) ? 1.0f : 0.5f;
                                            o.process (a, po);
                                            nw.process (c.getArrayOfWritePointers(), c.getNumChannels(), n, pn);
                                        });
                                    ++cells; if (! same) ++mismatches;
                                }
    expect (mismatches == 0, "spa-fx TremVib bit-identical to SPAStrip 1.0.3 over " + juce::String (cells)
                                 + " cells (" + juce::String (mismatches) + " mismatches)");
}

void eqBitIdenticalTest()
{
    std::cout << "spaFxEqBitIdenticalTest\n";
    using Old = spa::dsp::legacy103::ParametricEQ;
    using New = spa::fx::ParametricEQ;
    int cells = 0, mismatches = 0;
    for (double sr : { 44100.0, 96000.0 })
      for (int character = 0; character < 4; ++character)
        for (int type = 0; type < 8; ++type)
          for (int slope = 0; slope < 6; ++slope)
            for (int ch = 1; ch <= 2; ++ch)
            {
                Old oldE; New newE;
                oldE.prepare (sr, 512); newE.prepare (sr, 512);
                oldE.setCharacter (character); newE.setCharacter (character);
                uint32_t rng = 4242u + (uint32_t) cells;
                bool same = true;
                for (int b = 0; b < 22 && same; ++b)
                {
                    std::array<Old::Band, 8> bo {}; std::array<New::Band, 8> bn {};
                    auto set = [&] (int i, bool en, int t, int sl, float f, float g, float q)
                    {
                        bo[(size_t) i] = { en, t, sl, f, g, q };
                        bn[(size_t) i] = { en, t, sl, f, g, q };
                    };
                    const bool en0 = ! (b >= 14 && b < 18);
                    const int t0 = b < 10 ? type : (type + 3) % 8;
                    const int s0 = b < 10 ? slope : (slope + 2) % 6;
                    const float f0 = b < 5 ? 800.0f : 800.0f + 150.0f * (float) (b - 4);
                    const float g0 = b < 5 ? 6.0f : 6.0f - 0.9f * (float) (b - 4);
                    set (0, en0, t0, s0, f0, g0, b < 5 ? 1.0f : 0.5f + 0.1f * (float) b);
                    set (3, true, (int) Old::Type::bell, 1, 3000.0f, -4.0f, 2.0f);
                    set (6, b >= 3, (int) Old::Type::highShelf, 1, 9000.0f, 3.0f, 0.7f);
                    oldE.updateBands (bo); newE.updateBands (bn);
                    const int n = (b % 3 == 0) ? 64 : 509;
                    juce::AudioBuffer<float> a (ch, n), c (ch, n);
                    for (int cc = 0; cc < ch; ++cc)
                        for (int i = 0; i < n; ++i) { const float v = noiseOf (rng) * 0.5f; a.setSample (cc, i, v); c.setSample (cc, i, v); }
                    oldE.process (a);
                    newE.process (c.getArrayOfWritePointers(), ch, n);
                    for (int cc = 0; cc < ch; ++cc)
                        same = same && std::memcmp (a.getReadPointer (cc), c.getReadPointer (cc), sizeof (float) * (size_t) n) == 0;
                }
                ++cells; if (! same) ++mismatches;
            }
    {
        std::array<New::Band, 8> bn {}; std::array<Old::Band, 8> bo {};
        for (int i = 0; i < 8; ++i)
        {
            bn[(size_t) i] = { true, i, i % 6, 200.0f * (float) (i + 1), 3.0f, 1.2f };
            bo[(size_t) i] = { true, i, i % 6, 200.0f * (float) (i + 1), 3.0f, 1.2f };
        }
        for (float f : { 30.0f, 440.0f, 5000.0f, 15000.0f })
            expect (bitEq ((float) New::magnitudeDb (bn, f, 48000.0), (float) Old::magnitudeDb (bo, f, 48000.0)),
                    "EQ magnitudeDb identical at " + juce::String (f));
    }
    expect (mismatches == 0, "spa-fx EQ bit-identical to SPAStrip 1.0.3 over " + juce::String (cells) + " cells ("
                                 + juce::String (mismatches) + " mismatches)");
}

void limiterBitIdenticalTest()
{
    std::cout << "spaFxLimiterBitIdenticalTest\n";
    using Old = spa::dsp::legacy103::Limiter;
    using New = spa::fx::Limiter;
    constexpr int n = 256, blocks = 8;
    int cells = 0, mismatches = 0, metaMismatches = 0;
    for (double sr : { 44100.0, 96000.0 })
      for (float dr : { 0.0f, 6.0f, 24.0f }) for (float ce : { -12.0f, -0.3f, 0.0f }) for (float re : { 1.0f, 120.0f, 1000.0f })
       for (int ar = 0; ar < 2; ++ar) for (int la = 0; la < 2; ++la)
        for (float lk : { 0.0f, 0.5f, 1.0f }) for (int tp = 0; tp < 2; ++tp) for (int ag = 0; ag < 2; ++ag)
         for (int chr = 0; chr < 3; ++chr) for (int ch = 1; ch <= 2; ++ch)
         {
            if (((cells * 7) % 5) != 0 && ! (chr == 2 && la == 1)) { ++cells; continue; }
            Old o; New nw; o.prepare (sr, n); nw.prepare (sr, n);
            uint32_t rng = 31u + (uint32_t) cells;
            bool same = true;
            for (int b = 0; b < blocks && same; ++b)
            {
                juce::AudioBuffer<float> a (ch, n), c (ch, n);
                const float lvl = b < 5 ? 0.9f : 0.05f;
                for (int cc = 0; cc < ch; ++cc)
                    for (int i = 0; i < n; ++i) { const float v = noiseOf (rng) * lvl; a.setSample (cc, i, v); c.setSample (cc, i, v); }
                Old::Params po; New::Params pn;
                const bool late = b >= 4;
                po.enable = pn.enable = true;
                po.driveDb = pn.driveDb = late ? 24.0f - dr : dr;
                po.ceilingDb = pn.ceilingDb = late ? -6.0f : ce;
                po.releaseMs = pn.releaseMs = re;
                po.autoRelease = pn.autoRelease = ar != 0;
                po.lookahead = pn.lookahead = (la != 0) != late;
                po.stereoLink = pn.stereoLink = lk;
                po.truePeak = pn.truePeak = (tp != 0) != (late && b == 6);
                po.autoGain = pn.autoGain = ag != 0;
                po.character = pn.character = chr;
                o.process (a, po);
                nw.process (c.getWritePointer (0), ch > 1 ? c.getWritePointer (1) : nullptr, n, pn);
                for (int cc = 0; cc < ch; ++cc)
                    same = same && std::memcmp (a.getReadPointer (cc), c.getReadPointer (cc), sizeof (float) * (size_t) n) == 0;
                if (! bitEq (o.gainReductionDb(), nw.gainReductionDb()) || ! bitEq (o.outputPeak(), nw.outputPeak())
                    || o.latencySamples (po) != nw.latencySamples (pn))
                    ++metaMismatches;
            }
            ++cells; if (! same) ++mismatches;
         }
    expect (mismatches == 0, "spa-fx Limiter bit-identical to SPAStrip 1.0.3 (" + juce::String (mismatches)
                                 + " mismatching cells of " + juce::String (cells) + " visited)");
    expect (metaMismatches == 0, "limiter GR meter, output peak and latency identical to SPAStrip 1.0.3");
}

void compBitIdenticalTest()
{
    std::cout << "spaFxCompBitIdenticalTest\n";
    using Old = spa::dsp::legacy103::Multiband;
    using New = spa::fx::Multiband;
    constexpr int n = 512, blocks = 16;
    int cells = 0, mismatches = 0, meterMismatches = 0;
    for (double sr : { 44100.0, 96000.0 })
        for (int ch = 1; ch <= 2; ++ch)
            for (int variant = 0; variant < 8; ++variant)
            {
                Old o; New nw; o.prepare (sr, n); nw.prepare (sr, n);
                const bool same = compare (o, nw, ch, blocks, n, 91u + (uint32_t) cells,
                    [&] (int b, Old& oo, New& nn, juce::AudioBuffer<float>& a, juce::AudioBuffer<float>& c)
                    {
                        const float gain = b < 3 ? 1.0f : 0.03f;
                        a.applyGain (gain); c.applyGain (gain);
                        Old::Params po; New::Params pn;
                        po.enable = pn.enable = true;
                        po.mix = pn.mix = (variant % 3 == 0) ? 1.0f : 0.7f;
                        po.crossoverLowHz = pn.crossoverLowHz = 120.0f + 40.0f * (float) variant;
                        po.crossoverHighHz = pn.crossoverHighHz = 2500.0f + 300.0f * (float) variant;
                        for (int k = 0; k < 3; ++k)
                        {
                            const float knee = (variant & 1) ? (6.0f * (float) (k + 1)) : 0.0f;
                            const bool solo = (variant & 2) && k == (variant >> 2) % 3;
                            const bool byp = (variant & 4) && k == 1 && b >= 8;
                            po.bands[(size_t) k] = { -30.0f + 4.0f * (float) k, 6.0f, 2.5f, 6.0f + 5.0f * (float) k, 90.0f,
                                                     1.5f - (float) k, knee, solo, byp };
                            pn.bands[(size_t) k] = { -30.0f + 4.0f * (float) k, 6.0f, 2.5f, 6.0f + 5.0f * (float) k, 90.0f,
                                                     1.5f - (float) k, knee, solo, byp };
                        }
                        oo.process (a, po);
                        nn.process (c, pn);
                        for (int k = 0; k < 3; ++k)
                            if (! bitEq (oo.bandGainDb (k), nn.bandGainDb (k)) || ! bitEq (oo.bandLevelDb (k), nn.bandLevelDb (k)))
                                ++meterMismatches;
                    });
                ++cells; if (! same) ++mismatches;
            }
    expect (mismatches == 0, "spa-fx Multiband bit-identical to SPAStrip 1.0.3 over " + juce::String (cells)
                                 + " cells (" + juce::String (mismatches) + " mismatches)");
    expect (meterMismatches == 0, "COMP band meters identical (" + juce::String (meterMismatches) + " mismatches)");
}

// GLITTER: bit-identical to SPAStrip 1.0.3 with RELEASE off or infinite (infinite == the old
// FREEZE) over a grid of every knob; RELEASE active differs on purpose (tail fix).
struct GrainCell
{
    float size, density, pitch, spread, spreadPitch, position, reverse, feedback, mix, release;
    bool sync;
    int division;
    double bpm;
};

inline GrainCell grainCell (uint32_t& rng)
{
    auto u = [&rng] { rng = rng * 1664525u + 1013904223u; return (float) (rng >> 8) / (float) (1u << 24); };
    auto pick = [&] (std::initializer_list<float> v) { return *(v.begin() + (size_t) (u() * (float) v.size()) % v.size()); };
    GrainCell c {};
    c.size = pick ({ 2.0f, 20.0f, 120.0f, 500.0f });
    c.density = pick ({ 0.5f, 2.0f, 14.0f, 60.0f, 400.0f });
    c.pitch = pick ({ -24.0f, -12.0f, 0.0f, 7.0f, 24.0f });
    c.spread = pick ({ 0.0f, 0.25f, 1.0f });
    c.spreadPitch = pick ({ 0.0f, 3.0f, 12.0f });
    c.position = pick ({ 0.0f, 50.0f, 300.0f, 2000.0f, 4000.0f });
    c.reverse = pick ({ 0.0f, 0.5f, 1.0f });
    c.feedback = pick ({ 0.0f, 0.5f, 0.9f });
    c.mix = pick ({ 0.35f, 1.0f });
    c.release = pick ({ 0.0f, 0.04f, 0.1f, 1.0f, 12.0f, 30.0f, 31.0f });
    c.sync = u() < 0.3f;
    c.division = (int) (u() * 15.0f) % 15;
    c.bpm = pick ({ 40.0f, 120.0f, 200.0f });
    return c;
}

void grainBitIdenticalTest()
{
    std::cout << "spaFxGrainBitIdenticalTest\n";
    using Old = spa::dsp::legacy103::GrainFX;
    using New = spa::fx::GrainFX;
    constexpr int n = 512, blocks = 40;
    const auto active = [] (float r) { return r >= New::minRelease && r < New::infiniteRelease; };
    int cells = 0, mismatches = 0, activeCells = 0, activeDiffer = 0;
    auto run = [&] (double sr, int ch, const GrainCell& a, const GrainCell& b2, bool oldFreezeHold)
    {
        Old oldG; New newG;
        oldG.prepare (sr, n); newG.prepare (sr, n);
        const bool same = compare (oldG, newG, ch, blocks, n, 777u + (uint32_t) (cells + activeCells),
            [&] (int blk, Old& o, New& nw, juce::AudioBuffer<float>& x, juce::AudioBuffer<float>& y)
            {
                const auto& c = blk < 14 ? a : b2;
                Old::Params po; New::Params pn;
                po.enable = pn.enable = ! (blk == 22);
                po.sizeMs = pn.sizeMs = c.size; po.densityHz = pn.densityHz = c.density;
                po.sync = pn.sync = c.sync; po.division = c.division; po.bpm = pn.bpm = c.bpm;
                pn.syncBeats = spa::params::lfoDivisionBeats (c.division);
                po.pitchSt = pn.pitchSt = c.pitch; po.spread = pn.spread = c.spread;
                po.spreadPitchSt = pn.spreadPitchSt = c.spreadPitch; po.positionMs = pn.positionMs = c.position;
                po.reverse = pn.reverse = c.reverse; po.feedback = pn.feedback = c.feedback; po.mix = pn.mix = c.mix;
                po.releaseSec = pn.releaseSec = c.release;
                if (oldFreezeHold) { po.freeze = true; po.releaseSec = 0.0f; pn.releaseSec = New::infiniteRelease; }
                o.process (x, po);
                {
                    juce::ScopedNoDenormals nd;
                    nw.process (y.getWritePointer (0), ch > 1 ? y.getWritePointer (1) : nullptr, n, pn);
                }
            });
        if (! oldFreezeHold && (active (a.release) || active (b2.release))) { ++activeCells; if (! same) ++activeDiffer; }
        else { ++cells; if (! same) ++mismatches; }
    };
    uint32_t rng = 20261008u;
    for (double sr : { 44100.0, 96000.0 })
        for (int ch = 1; ch <= 2; ++ch)
        {
            for (int i = 0; i < 40; ++i) { auto a = grainCell (rng); auto b2 = grainCell (rng); run (sr, ch, a, b2, false); }
            for (float rel : { 0.0f, 0.04f, 0.05f, 0.1f, 0.5f, 2.0f, 8.0f, 20.0f, 30.0f, 30.4f, 30.5f, 31.0f })
                for (float fb : { 0.0f, 0.9f })
                    for (float pitch : { 0.0f, 7.0f })
                    {
                        auto a = grainCell (rng); a.release = rel; a.feedback = fb; a.pitch = pitch;
                        auto b2 = a; b2.release = 31.0f - rel; b2.position = 150.0f;
                        run (sr, ch, a, b2, false);
                    }
            for (int i = 0; i < 6; ++i) { auto a = grainCell (rng); auto b2 = grainCell (rng); run (sr, ch, a, b2, true); }
        }
    expect (mismatches == 0, "spa-fx GrainFX bit-identical to SPAStrip 1.0.3 over " + juce::String (cells)
                                 + " cells with RELEASE off/infinite and the old FREEZE (" + juce::String (mismatches) + " mismatches)");
    expect (activeCells > 100 && activeDiffer >= 10,
            "RELEASE-active cells (" + juce::String (activeCells) + ") do differ from 1.0.3 on purpose ("
                + juce::String (activeDiffer) + "), so this comparison is not vacuous");
}

// REVERB: identical at the reference cell (SIZE 0.5, DECAY 2 s, where the new level
// normalisation is exactly 1) for every mode; elsewhere changed on purpose.
void plateReverbReferenceCellTest()
{
    std::cout << "spaFxPlateReverbReferenceCellTest\n";
    constexpr double sr = 48000.0;
    constexpr int n = 512, blocks = 6;
    int cells = 0, mismatches = 0;
    for (int mode = 0; mode < 5; ++mode)
        for (float pre : { 0.0f, 20.0f, 100.0f })
            for (int ch = 1; ch <= 2; ++ch)
            {
                spa::dsp::legacy103::PlateReverb oldR; spa::fx::PlateReverb newR;
                oldR.prepare (sr, n); newR.prepare (sr, n);
                spa::dsp::legacy103::PlateReverb::Params po; spa::fx::PlateReverb::Params pn;
                po.mode = pn.mode = mode; po.size = pn.size = 0.5f; po.decaySec = pn.decaySec = 2.0f;
                po.preDelayMs = pn.preDelayMs = pre; po.mix = pn.mix = 0.6f;
                po.hfDamp = pn.hfDamp = 0.37f; po.modDepth = pn.modDepth = 0.55f; po.width = pn.width = 0.8f;
                uint32_t rng = 4242u + (uint32_t) (mode * 7 + ch);
                bool same = true;
                for (int b = 0; b < blocks && same; ++b)
                {
                    juce::AudioBuffer<float> a (ch, n), c (ch, n);
                    for (int cc = 0; cc < ch; ++cc)
                        for (int i = 0; i < n; ++i) { const float v = b < 2 ? noiseOf (rng) * 0.5f : 0.0f; a.setSample (cc, i, v); c.setSample (cc, i, v); }
                    oldR.process (a, po);
                    newR.process (c.getWritePointer (0), ch > 1 ? c.getWritePointer (1) : nullptr, n, pn);
                    same = std::memcmp (a.getReadPointer (0), c.getReadPointer (0), sizeof (float) * (size_t) n) == 0;
                    if (ch > 1) same = same && std::memcmp (a.getReadPointer (1), c.getReadPointer (1), sizeof (float) * (size_t) n) == 0;
                }
                ++cells; if (! same) ++mismatches;
            }
    expect (mismatches == 0, "spa-fx PlateReverb bit-identical to SPAStrip 1.0.3 at the reference cell over "
                                 + juce::String (cells) + " mode/PRE/channel cells (" + juce::String (mismatches) + " mismatches)");
}

// ---------------------------------------------------------------------------------------
// Chain-level bit identity: the frozen SPAStrip 1.0.3 FXChain vs the adopted one
// ---------------------------------------------------------------------------------------

using OldChain = spa::dsp::legacy103::FXChain;
using NewChain = spa::dsp::FXChain;

// Applies the same assignment to both chains' Params (field names are identical).
template <typename F>
inline void both (OldChain::Params& o, NewChain::Params& n, F&& f) { f (o); f (n); }

// Runs `blocks` blocks of noise (first 8 blocks, then silence) through both chains and
// compares every sample. `perBlock(b, o, n)` may retarget the params. Block sizes vary.
template <typename Setup>
inline bool chainsMatch (double sr, int ch, int blocks, uint32_t seed, Setup&& setup, bool compareTail = true)
{
    OldChain oc; NewChain nc;
    oc.prepare (sr, 512); nc.prepare (sr, 512);
    OldChain::Params po; NewChain::Params pn;
    uint32_t rng = seed;
    bool same = true;
    for (int b = 0; b < blocks && same; ++b)
    {
        setup (b, po, pn);
        const int n = (b % 4 == 0) ? 100 : (b % 4 == 1 ? 512 : (b % 4 == 2 ? 37 : 480));
        juce::AudioBuffer<float> x (ch, n), y (ch, n);
        for (int cc = 0; cc < ch; ++cc)
            for (int i = 0; i < n; ++i) { const float v = b < 8 ? noiseOf (rng) * 0.5f : 0.0f; x.setSample (cc, i, v); y.setSample (cc, i, v); }
        oc.process (x, po);
        nc.process (y, pn);
        for (int cc = 0; cc < ch; ++cc)
        {
            same = same && std::memcmp (x.getReadPointer (cc), y.getReadPointer (cc), sizeof (float) * (size_t) n) == 0;
        }
    }
    return same && (! compareTail || juce::exactlyEqual (oc.tailSeconds (po), nc.tailSeconds (pn)));
}

void chainDistortionTest()
{
    std::cout << "spaFxChainDistortionTest\n";
    int cells = 0, bad = 0;
    for (double sr : { 44100.0, 96000.0 })
        for (int type = 0; type < 4; ++type)
            for (float dr : { 0.0f, 0.3f, 0.8f, 1.0f })
                for (float tn : { 500.0f, 8000.0f, 20000.0f })
                    for (int ch = 1; ch <= 2; ++ch)
                    {
                        const bool ok = chainsMatch (sr, ch, 12, 4242u + (uint32_t) cells, [&] (int b, auto& po, auto& pn)
                        {
                            both (po, pn, [&] (auto& p)
                            {
                                p.distEnable = ! (b == 9);
                                p.distType = b < 5 ? type : (type + 1) % 4;
                                p.distDrive = b < 5 ? dr : 1.0f - dr;
                                p.distToneHz = tn; p.distMix = (cells % 2) ? 0.5f : 1.0f;
                            });
                        });
                        ++cells; if (! ok) ++bad;
                    }
    expect (bad == 0, "DIST through the chain bit-identical to SPAStrip 1.0.3 over " + juce::String (cells) + " cells (" + juce::String (bad) + " bad)");
}

void chainDelayTest()
{
    std::cout << "spaFxChainDelayTest\n";
    int cells = 0, bad = 0;
    for (double tm : { 1.0, 62.5, 350.0, 1900.0, 5000.0 })
        for (float fb : { 0.0f, 0.35f, 0.95f })
            for (int pp = 0; pp < 2; ++pp)
                for (float w : { 0.0f, 0.5f, 1.0f })
                    for (int sync = 0; sync < 2; ++sync)
                        for (int ch = 1; ch <= 2; ++ch)
                        {
                            const bool ok = chainsMatch (48000.0, ch, 24, 777u + (uint32_t) cells, [&] (int b, auto& po, auto& pn)
                            {
                                both (po, pn, [&] (auto& p)
                                {
                                    p.delayEnable = true; p.delaySync = sync != 0; p.delayDivision = 3 + (b < 8 ? 0 : 3);
                                    p.bpm = b < 12 ? 120.0 : 87.0;
                                    p.delayTimeMs = (float) (b < 8 ? tm : tm * 0.5);
                                    p.delayFeedback = fb; p.delayPingPong = pp != 0;
                                    p.delayWidth = b < 8 ? w : 1.0f - w; p.delayMix = 0.4f;
                                });
                            });
                            ++cells; if (! ok) ++bad;
                        }
    expect (bad == 0, "DELAY through the chain (output and reported tail) bit-identical to SPAStrip 1.0.3 over " + juce::String (cells)
                          + " cells (" + juce::String (bad) + " bad)");
}

void chainModulationFxTest()
{
    std::cout << "spaFxChainModulationFxTest\n";
    int cells = 0, bad = 0;
    for (int mode = 0; mode < 3; ++mode)
        for (int ch = 1; ch <= 2; ++ch)
            for (int variant = 0; variant < 6; ++variant)
            {
                const bool ok = chainsMatch (48000.0, ch, 20, 31u + (uint32_t) cells, [&] (int b, auto& po, auto& pn)
                {
                    both (po, pn, [&] (auto& p)
                    {
                        p.chorusEnable = ! (b >= 9 && b < 11); p.chorusMode = mode; p.chorusRate = 0.5f + (float) variant;
                        p.chorusDepth = 0.2f * (float) (variant % 4); p.chorusFeedback = 0.1f * (float) variant;
                        p.chorusWidth = 0.5f; p.chorusMix = 0.6f;
                        p.modEnable = ! (b >= 12 && b < 14); p.modType = variant % 2; p.modRate = 0.3f + (float) variant;
                        p.modSync = (variant % 3) == 0; p.modDivision = 5 + variant; p.bpm = 100.0 + 10.0 * (b % 3);
                        p.modDepth = 0.7f; p.modFeedback = 0.4f; p.modStages = 2 + 2 * variant; p.modMix = 0.5f;
                        p.tremEnable = (variant % 2) == 0; p.tremRate = 4.0f + (float) variant; p.tremSync = variant == 4;
                        p.tremDepth = 0.6f; p.tremShape = variant % 4; p.tremStereo = 0.4f; p.tremMix = 0.9f;
                        p.vibEnable = variant > 1; p.vibRate = 3.0f + (float) variant; p.vibDepth = 0.5f; p.vibSync = variant == 5;
                    });
                });
                ++cells; if (! ok) ++bad;
            }
    expect (bad == 0, "CHORUS/MOD/TREM-VIB through the chain bit-identical to SPAStrip 1.0.3 over " + juce::String (cells)
                          + " cells (" + juce::String (bad) + " bad)");
}

void chainFilterTest()
{
    std::cout << "spaFxChainFilterTest\n";
    int cells = 0, bad = 0;
    for (double sr : { 44100.0, 96000.0 })
      for (int t1 = 0; t1 < 8; ++t1) for (int t2 = 0; t2 < 8; t2 += 3)
        for (int routing = 0; routing < 2; ++routing) for (int ch = 1; ch <= 2; ++ch)
        {
            const bool ok = chainsMatch (sr, ch, 14, 777u + (uint32_t) cells, [&] (int b, auto& po, auto& pn)
            {
                both (po, pn, [&] (auto& p)
                {
                    const float f = (float) b / 13.0f;
                    p.filterEnable = (b % 5) != 3; p.filter2Enable = (b % 4) != 1; p.filterRouting = routing;
                    p.filter1Type = t1; p.filter2Type = t2;
                    p.filter1Cutoff = 80.0f * std::pow (200.0f, f); p.filter2Cutoff = 20000.0f - 15000.0f * f;
                    p.filter1Resonance = f; p.filter2Resonance = 1.0f - f;
                    p.filter1Drive = (float) (b % 3) * 0.4f; p.filter2Drive = f * 0.5f;
                    p.filter1Mix = (b == 6) ? 0.0f : 1.0f - 0.3f * f; p.filter2Mix = (b == 7) ? 0.0f : 0.8f;
                });
            });
            ++cells; if (! ok) ++bad;
        }
    expect (bad == 0, "FILTER through the chain bit-identical to SPAStrip 1.0.3 over " + juce::String (cells) + " cells (" + juce::String (bad) + " bad)");
}

void chainDynamicsAndEqTest()
{
    std::cout << "spaFxChainDynamicsAndEqTest\n";
    int cells = 0, bad = 0;
    for (int ch = 1; ch <= 2; ++ch)
        for (int variant = 0; variant < 8; ++variant)
        {
            const bool ok = chainsMatch (48000.0, ch, 20, 91u + (uint32_t) cells, [&] (int b, auto& po, auto& pn)
            {
                both (po, pn, [&] (auto& p)
                {
                    p.limEnable = true; p.limDrive = 3.0f * (float) variant; p.limCeiling = -0.5f - (float) (variant % 3);
                    p.limRelease = 50.0f + 40.0f * (float) variant; p.limAutoRelease = (variant & 1) != 0;
                    p.limCharacter = variant % 3; p.limStereoLink = 0.5f; p.limTruePeak = (variant & 2) != 0;
                    p.limLookahead = (variant & 4) != 0; p.limAutoGain = variant == 5;
                    p.compEnable = true; p.compMix = 0.8f;
                    p.compCrossoverLow = 150.0f + 20.0f * (float) variant; p.compCrossoverHigh = 2500.0f;
                    for (size_t k = 0; k < p.compBands.size(); ++k)
                        p.compBands[k].thresholdDb = -30.0f + 4.0f * (float) k;
                    p.eqEnable = ! (b == 11); p.eqCharacter = variant % 4;
                    p.eqBands[0] = { true, variant % 8, variant % 6, 800.0f + 100.0f * (float) b, 5.0f, 1.0f };
                    p.eqBands[3] = { true, 0, 1, 3000.0f, -4.0f, 2.0f };
                });
            });
            ++cells; if (! ok) ++bad;
        }
    expect (bad == 0, "LIMIT/COMP/EQ through the chain bit-identical to SPAStrip 1.0.3 over " + juce::String (cells) + " cells (" + juce::String (bad) + " bad)");
}

void chainGrainTest()
{
    std::cout << "spaFxChainGrainTest\n";
    int cells = 0, bad = 0;
    // RELEASE off and infinite (the old FREEZE) are bit-identical through the chain; so is the reported tail.
    for (float rel : { 0.0f, 31.0f })
        for (float fb : { 0.0f, 0.6f, 0.95f })
            for (int ch = 1; ch <= 2; ++ch)
            {
                const bool ok = chainsMatch (48000.0, ch, 40, 777u + (uint32_t) cells, [&] (int b, auto& po, auto& pn)
                {
                    both (po, pn, [&] (auto& p)
                    {
                        p.grainEnable = ! (b == 20); p.grainSizeMs = 140.0f; p.grainDensityHz = 22.0f; p.grainPitch = 5.0f;
                        p.grainSpread = 0.4f; p.grainSpreadPitch = 2.0f; p.grainReverse = 0.3f; p.grainFeedback = fb;
                        p.grainMix = 0.7f; p.grainPositionMs = 350.0f; p.grainReleaseSec = rel;
                        p.grainSync = (b >= 25); p.grainDivision = 9; p.bpm = 120.0;
                    });
                    // the old FREEZE with RELEASE off equals RELEASE infinite in the new chain
                    if (rel > 30.0f) { po.grainFreeze = true; po.grainReleaseSec = 0.0f; }
                }, rel < 30.0f);   // the reported tail of a held cloud differs on purpose (300 s ceiling, was the feedback ring-out)
                ++cells; if (! ok) ++bad;
            }
    expect (bad == 0, "GLITTER (RELEASE off, and infinite == the old FREEZE) through the chain bit-identical to SPAStrip 1.0.3 over "
                          + juce::String (cells) + " cells (" + juce::String (bad) + " bad)");
}

// Everything on at once, default order and a rearranged one, REVERB off (changed on purpose).
void chainEverythingTest()
{
    std::cout << "spaFxChainEverythingTest\n";
    int cells = 0, bad = 0;
    for (int order = 0; order < 3; ++order)
        for (int ch = 1; ch <= 2; ++ch)
        {
            const bool ok = chainsMatch (48000.0, ch, 30, 5u + (uint32_t) cells, [&] (int b, auto& po, auto& pn)
            {
                both (po, pn, [&] (auto& p)
                {
                    p.filterEnable = true; p.filter1Cutoff = 4000.0f + 100.0f * (float) b; p.filter1Resonance = 0.3f;
                    p.distEnable = true; p.distType = 1; p.distDrive = 0.3f;
                    p.chorusEnable = true; p.modEnable = true; p.tremEnable = true; p.vibEnable = true;
                    p.grainEnable = true; p.grainMix = 0.3f;
                    p.delayEnable = true; p.delayMix = 0.25f; p.delaySync = false; p.delayTimeMs = 233.0f;
                    p.eqEnable = true; p.eqBands[2] = { true, 0, 1, 1200.0f, 4.0f, 1.0f };
                    p.compEnable = true; p.limEnable = true; p.limDrive = 6.0f;
                    p.reverbEnable = false;
                });
                if (order > 0)
                {
                    using M = OldChain::Module;
                    const M a[] = { M::eq, M::limiter, M::comp, M::delay, M::grain, M::tremVib, M::mod, M::chorus,
                                    M::distortion, M::filter, M::reverb, M::convolve };
                    const M c[] = { M::delay, M::distortion, M::eq, M::filter, M::chorus, M::comp, M::grain, M::mod,
                                    M::tremVib, M::reverb, M::convolve, M::limiter };
                    for (int i = 0; i < 12; ++i)
                    {
                        po.order[i] = order == 1 ? a[i] : c[i];
                        pn.order[i] = (NewChain::Module) (int) po.order[i];
                    }
                }
            });
            ++cells; if (! ok) ++bad;
        }
    expect (bad == 0, "the whole chain (reverb off) bit-identical to SPAStrip 1.0.3 over " + juce::String (cells) + " cells (" + juce::String (bad) + " bad)");
}

// CONV: SPAStrip keeps its own convolution engine; only the IR shaping is shared. Load the
// same buffer IR into the frozen and the adopted chain, reshape mid-stream, compare.
void chainConvolveTest()
{
    std::cout << "spaFxChainConvolveTest\n";
    constexpr int n = 256;
    int cells = 0, bad = 0, shapeBad = 0;
    for (double sr : { 48000.0, 96000.0 })
        for (int irCh = 1; irCh <= 2; ++irCh)
        {
            uint32_t rng = 12345u + (uint32_t) cells;
            juce::AudioBuffer<float> raw (irCh, (int) (0.9 * sr));
            for (int c = 0; c < irCh; ++c)
                for (int i = 0; i < raw.getNumSamples(); ++i)
                    raw.setSample (c, i, noiseOf (rng) * 0.5f * std::exp (-3.0f * (float) i / (float) raw.getNumSamples()));
            for (float decay : { 0.02f, 1.0f })
              for (float damp : { 0.0f, 0.5f })
                for (float start : { 0.0f, 0.3f, 1.0f })
                {
                    OldChain oc; NewChain nc;
                    oc.prepare (sr, n); nc.prepare (sr, n);
                    oc.setConvolutionShaping (decay, damp, start); nc.setConvolutionShaping (decay, damp, start);
                    oc.loadConvolutionIRFromBuffer (raw, sr); nc.loadConvolutionIRFromBuffer (raw, sr);
                    const auto shapeSame = [&]
                    {
                        return bitEq (oc.convolutionStartTrim(), nc.convolutionStartTrim())
                            && juce::exactlyEqual (oc.convolutionLengthSeconds(), nc.convolutionLengthSeconds())
                            && std::memcmp (oc.convolutionEnvelope().data(), nc.convolutionEnvelope().data(), sizeof (float) * 256) == 0;
                    };
                    if (! shapeSame()) ++shapeBad;
                    OldChain::Params po; NewChain::Params pn;
                    both (po, pn, [&] (auto& p) { p.convEnable = true; p.convMix = 0.7f; p.convWidth = 0.6f; p.convPreDelay = 20.0f; });
                    // let both background IR loads finish and swap in
                    juce::AudioBuffer<float> sil (2, n);
                    for (int i = 0; i < 50; ++i)
                    {
                        sil.clear(); oc.process (sil, po); sil.clear(); nc.process (sil, pn);
                        juce::Thread::sleep (10);
                    }
                    oc.reset(); nc.reset();
                    bool same = true;
                    for (int b = 0; b < 24 && same; ++b)
                    {
                        if (b == 12)
                        {
                            oc.setConvolutionShaping (0.4f, 0.5f, 0.2f); nc.setConvolutionShaping (0.4f, 0.5f, 0.2f);
                            for (int i = 0; i < 50; ++i)
                            {
                                sil.clear(); oc.process (sil, po); sil.clear(); nc.process (sil, pn);
                                juce::Thread::sleep (10);
                            }
                            oc.reset(); nc.reset();
                            if (! shapeSame()) ++shapeBad;
                        }
                        juce::AudioBuffer<float> x (2, n), y (2, n);
                        for (int cc = 0; cc < 2; ++cc)
                            for (int i = 0; i < n; ++i) { const float v = (b % 12) < 2 ? noiseOf (rng) * 0.5f : 0.0f; x.setSample (cc, i, v); y.setSample (cc, i, v); }
                        oc.process (x, po); nc.process (y, pn);
                        for (int cc = 0; cc < 2; ++cc)
                            same = same && std::memcmp (x.getReadPointer (cc), y.getReadPointer (cc), sizeof (float) * (size_t) n) == 0;
                    }
                    same = same && juce::exactlyEqual (oc.tailSeconds (po), nc.tailSeconds (pn));
                    ++cells; if (! same) ++bad;
                }
        }
    expect (shapeBad == 0, "CONV shaped-IR envelope/trim/length bit-identical to SPAStrip 1.0.3 (" + juce::String (shapeBad) + " mismatches)");
    expect (bad == 0, "CONV output and reported tail bit-identical to SPAStrip 1.0.3 over " + juce::String (cells) + " cells (" + juce::String (bad) + " bad)");
}

// The chain-level reverb: same at the reference cell, different elsewhere (level fix).
void chainReverbReferenceCellTest()
{
    std::cout << "spaFxChainReverbReferenceCellTest\n";
    int cells = 0, bad = 0;
    for (int mode = 0; mode < 5; ++mode)
        for (int ch = 1; ch <= 2; ++ch)
        {
            const bool ok = chainsMatch (48000.0, ch, 16, 4242u + (uint32_t) cells, [&] (int, auto& po, auto& pn)
            {
                both (po, pn, [&] (auto& p)
                {
                    p.reverbEnable = true; p.reverbMode = mode; p.reverbSize = 0.5f; p.reverbDecay = 2.0f;
                    p.reverbPreDelay = 20.0f; p.reverbMix = 0.5f;
                });
            });
            ++cells; if (! ok) ++bad;
        }
    expect (bad == 0, "REVERB through the chain bit-identical to SPAStrip 1.0.3 at the reference cell (" + juce::String (cells) + " cells, " + juce::String (bad) + " bad)");
}

// ---------------------------------------------------------------------------------------
// Reverb level (spa-fx a788247): constant wet level across SIZE and DECAY, RT60 unchanged.
// ---------------------------------------------------------------------------------------

struct BurstStats { double energy = 0, r50 = 0, rt60 = -1; };

template <typename Proc>
inline BurstStats measureBurst (Proc proc, double rt60Hint, float preMs)
{
    constexpr double sr = 48000.0;
    constexpr int n = 512;
    const int blocks = (int) ((0.7 + 1.4 * rt60Hint + preMs * 0.001) * sr / n) + 1;
    const int excite = (int) (0.2 * sr / n);
    uint32_t rng = 99999u;
    std::vector<float> L ((size_t) n), R ((size_t) n), outL, outR;
    for (int b = 0; b < blocks; ++b)
    {
        for (int s = 0; s < n; ++s)
        {
            L[(size_t) s] = b < excite ? noiseOf (rng) * 0.5f : 0.0f;
            R[(size_t) s] = b < excite ? noiseOf (rng) * 0.5f : 0.0f;
        }
        proc (L.data(), R.data(), n);
        outL.insert (outL.end(), L.begin(), L.end()); outR.insert (outR.end(), R.begin(), R.end());
    }
    const int total = blocks * n;
    BurstStats st;
    for (int i = 0; i < total; ++i)
        st.energy += (double) outL[(size_t) i] * outL[(size_t) i] + (double) outR[(size_t) i] * outR[(size_t) i];
    int first = 0;
    while (first < total && std::abs (outL[(size_t) first]) < 1e-3f && std::abs (outR[(size_t) first]) < 1e-3f) ++first;
    const int w = (int) (0.05 * sr);
    double e50 = 0;
    for (int i = first; i < std::min (total, first + w); ++i)
        e50 += (double) outL[(size_t) i] * outL[(size_t) i] + (double) outR[(size_t) i] * outR[(size_t) i];
    st.r50 = std::sqrt (e50 / (2.0 * w));
    const int start = (int) ((0.2 + preMs * 0.001) * sr);
    if (start < total)
    {
        std::vector<double> sch ((size_t) (total - start));
        double acc = 0;
        for (int i = total - 1; i >= start; --i) { acc += (double) outL[(size_t) i] * outL[(size_t) i]; sch[(size_t) (i - start)] = acc; }
        double t5 = -1, t25 = -1;
        for (size_t i = 0; i < sch.size(); ++i)
        {
            const double db = 10.0 * std::log10 (std::max (1e-30, sch[i] / sch[0]));
            if (t5 < 0 && db <= -5.0) t5 = (double) i / sr;
            if (t25 < 0 && db <= -25.0) { t25 = (double) i / sr; break; }
        }
        if (t5 >= 0 && t25 > t5) st.rt60 = 3.0 * (t25 - t5);
    }
    return st;
}

void reverbLevelConstantAcrossSizeDecayTest()
{
    std::cout << "reverbLevelConstantAcrossSizeDecayTest\n";
    const char* names[] = { "hall", "plate", "chamber", "room", "spring" };
    constexpr float preMs = 20.0f;
    double worstE = 0, worstR = 0, size0E = -99, size0R = -99;
    int bad = 0, beyond2 = 0;
    for (int mode = 0; mode < 5; ++mode)
    {
        const auto cell = [&] (float size, float decay)
        {
            spa::fx::PlateReverb r; r.prepare (48000.0, 512);
            spa::fx::PlateReverb::Params p; p.mode = mode; p.size = size; p.decaySec = decay; p.preDelayMs = preMs; p.mix = 1.0f;
            return measureBurst ([&] (float* l, float* rr, int n) { r.process (l, rr, n, p); }, decay * 1.2 * (mode == 3 ? 0.5 : 1.0), preMs);
        };
        const auto ref = cell (0.5f, 2.0f);
        for (float size : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            for (float decay : { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f })
            {
                const auto c = cell (size, decay);
                const double dE = 10.0 * std::log10 (std::max (1e-30, c.energy) / ref.energy);
                const double dR = 20.0 * std::log10 (std::max (1e-30, c.r50) / ref.r50);
                worstE = std::max (worstE, std::abs (dE)); worstR = std::max (worstR, std::abs (dR));
                if (size < 0.01f) { size0E = std::max (size0E, dE); size0R = std::max (size0R, dR); }
                if (std::abs (dE) > 2.1 || std::abs (dR) > 2.1) ++beyond2;
                if (std::abs (dE) > 3.0 || std::abs (dR) > 3.0) ++bad;
            }
        for (float decay : { 2.0f, 4.0f })
        {
            spa::fx::PlateReverb nr; nr.prepare (48000.0, 512);
            spa::dsp::legacy103::PlateReverb orr; orr.prepare (48000.0, 512);
            spa::fx::PlateReverb::Params pn; spa::dsp::legacy103::PlateReverb::Params po;
            pn.mode = po.mode = mode; pn.size = po.size = 0.5f; pn.decaySec = po.decaySec = decay; pn.preDelayMs = po.preDelayMs = preMs; pn.mix = po.mix = 1.0f;
            const auto a = measureBurst ([&] (float* l, float* rr, int n) { nr.process (l, rr, n, pn); }, decay * 1.2, preMs);
            const auto b = measureBurst ([&] (float* l, float* rr, int n)
                                         { juce::AudioBuffer<float> buf (2, n);
                                           std::memcpy (buf.getWritePointer (0), l, sizeof (float) * (size_t) n);
                                           std::memcpy (buf.getWritePointer (1), rr, sizeof (float) * (size_t) n);
                                           orr.process (buf, po);
                                           std::memcpy (l, buf.getReadPointer (0), sizeof (float) * (size_t) n);
                                           std::memcpy (rr, buf.getReadPointer (1), sizeof (float) * (size_t) n); }, decay * 1.2, preMs);
            expect (a.rt60 > 0 && b.rt60 > 0 && std::abs (a.rt60 / b.rt60 - 1.0) < 0.03,
                    juce::String (names[mode]) + " RT60 unchanged at DECAY " + juce::String (decay, 1) + " s (new "
                    + juce::String (a.rt60, 3) + " s, 1.0.3 " + juce::String (b.rt60, 3) + " s)");
        }
    }
    expect (bad == 0, "every mode x SIZE x DECAY cell within +/-3 dB of its mode's reference cell (worst energy "
                          + juce::String (worstE, 2) + " dB, worst first-50 " + juce::String (worstR, 2) + " dB, " + juce::String (bad) + " cells out)");
    expect (beyond2 <= 24, juce::String (beyond2) + " of 150 cells beyond +/-2.1 dB (limit 24)");
    expect (size0E < 1.5 && size0R < 3.0,
            "SIZE 0 is no hotter than a small margin over the reference (max energy +" + juce::String (size0E, 2)
                + " dB, max first-50 +" + juce::String (size0R, 2) + " dB)");
}

// ---------------------------------------------------------------------------------------
// EQ analyzer taps follow the EQ's position in the user-ordered chain (was: PRE = the
// signal entering the whole chain, POST = the final output, wherever the EQ sat).
// ---------------------------------------------------------------------------------------

void eqAnalyzerTapFollowsChainPositionTest()
{
    std::cout << "eqAnalyzerTapFollowsChainPositionTest\n";
    using FX = spa::dsp::FXChain;
    constexpr double sr = 48000.0;
    constexpr int n = 512;
    auto goertzel = [] (const std::array<std::atomic<float>, spa::dsp::Telemetry::scopeSize>& ring, int w, double hz)
    {
        const int N = spa::dsp::Telemetry::scopeSize;
        double c = 2.0 * std::cos (2.0 * juce::MathConstants<double>::pi * hz / sr), s1 = 0, s2 = 0;
        for (int i = 0; i < N; ++i)
        {
            const double x = ring[(size_t) ((w + i) & (N - 1))].load();
            const double s0 = x + c * s1 - s2; s2 = s1; s1 = s0;
        }
        return std::sqrt (s1 * s1 + s2 * s2 - c * s1 * s2) / (double) N;
    };
    struct Result { double preH, postH, preF, postF; };
    auto run = [&] (bool eqFirst, bool eqOn, int decim)
    {
        FX fx; fx.prepare (sr * decim, n * decim);
        spa::dsp::Telemetry tel;
        fx.setEqTap (&tel, decim);
        FX::Params p;
        p.distEnable = true; p.distType = 1; p.distDrive = 1.0f; p.distMix = 1.0f;
        p.eqEnable = eqOn;
        p.eqBands[0] = { true, (int) spa::dsp::ParametricEQ::Type::highCut, 5, 800.0f, 0.0f, 0.707f };
        FX::Module rest[] = { FX::Module::filter, FX::Module::chorus, FX::Module::mod, FX::Module::tremVib, FX::Module::grain,
                              FX::Module::delay, FX::Module::reverb, FX::Module::convolve, FX::Module::comp, FX::Module::limiter };
        int k = 0;
        if (eqFirst) { p.order[k++] = FX::Module::eq; p.order[k++] = FX::Module::distortion; }
        else         { p.order[k++] = FX::Module::distortion; p.order[k++] = FX::Module::eq; }
        for (auto m : rest) p.order[k++] = m;
        const int len = n * decim;
        double ph = 0.0;
        for (int b = 0; b < 40; ++b)
        {
            juce::AudioBuffer<float> buf (2, len);
            for (int i = 0; i < len; ++i)
            {
                const float v = 0.4f * (float) std::sin (ph); ph += 2.0 * juce::MathConstants<double>::pi * 500.0 / (sr * decim);
                buf.setSample (0, i, v); buf.setSample (1, i, v);
            }
            fx.process (buf, p);
        }
        const int wp = tel.preScopeWrite.load(), wq = tel.scopeWrite.load();
        return Result { goertzel (tel.preScope, wp, 1500.0), goertzel (tel.scope, wq, 1500.0),
                        goertzel (tel.preScope, wp, 500.0), goertzel (tel.scope, wq, 500.0) };
    };
    for (int decim : { 1, 4 })
    {
        const auto before = run (true, true, decim);
        const auto after  = run (false, true, decim);
        const auto off    = run (false, false, decim);
        const juce::String tag = " (decim " + juce::String (decim) + ")";
        expect (before.preH < 0.02 * before.preF, "EQ before DIST: PRE is the clean tone, no 1.5 kHz harmonic" + tag);
        expect (after.preH > 20.0 * before.preH + 1.0e-4, "EQ after DIST: PRE now carries the distortion harmonic" + tag);
        expect (after.postH < 0.05 * after.preH, "EQ after DIST with a high cut: POST removes what PRE had" + tag);
        expect (std::abs (off.preH - off.postH) < 1.0e-9 && std::abs (off.preF - off.postF) < 1.0e-9,
                "EQ off: PRE == POST at its position" + tag);
        expect (off.preH > 1.0e-3, "PRE sees DIST output when EQ sits after it, EQ off" + tag);
    }
}

} // namespace spaFxTests
