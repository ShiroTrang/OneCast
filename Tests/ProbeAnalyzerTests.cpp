// Unit tests for ProbeAnalyzer, the glitch detector behind OneCastProbe. The 30
// minute soak test trusts its verdict, so these tests check both directions:
// every kind of glitch must be found, and a clean (or merely noisy) sine must
// never be reported. No audio device is involved.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe probe

#include <JuceHeader.h>

#include "TestSignals.h"
#include "ProbeAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace
{
    constexpr double sr  = 48000.0;
    constexpr double hz  = 440.0;
    constexpr float  amp = 0.5f;
    constexpr double pi  = 3.14159265358979323846;

    // 440 Hz at 48 kHz repeats every 109.09 samples, and every 6000 samples
    // (125 ms) exactly. 3.0 s, 4.0 s and 6.0 s all land on a zero crossing.
    int at (double seconds) { return (int) std::lround (seconds * sr); }

    using Glitches = std::vector<ProbeAnalyzer::Glitch>;

    Glitches analyse (const std::vector<float>& x)
    {
        ProbeAnalyzer analyzer (sr, hz);
        analyzer.process (x.data(), (int) x.size());
        return analyzer.getGlitches();
    }

    Glitches analyseInRandomBlocks (const std::vector<float>& x, juce::int64 seed)
    {
        ProbeAnalyzer analyzer (sr, hz);
        juce::Random random (seed);

        // Mixes tiny blocks (1 to 3 samples) with device-sized ones. Every
        // block is at least 1 sample: nextInt (int) is [0, max), so an
        // unguarded 0 would leave `start` unchanged and spin forever.
        for (int start = 0; start < (int) x.size();)
        {
            const int wanted = random.nextInt (4) == 0 ? 1 + random.nextInt (3)
                                                       : 1 + random.nextInt (2000);
            const int n = std::min (wanted, (int) x.size() - start);
            analyzer.process (x.data() + start, n);
            start += n;
        }

        return analyzer.getGlitches();
    }

    /// amp * sin (w n) with the phase advanced by `jump` radians from sample
    /// `jumpAt` on.
    std::vector<float> sineWithPhaseJump (int numSamples, int jumpAt, double jump)
    {
        std::vector<float> x ((size_t) numSamples);
        const double w = 2.0 * pi * hz / sr;

        for (int i = 0; i < numSamples; ++i)
            x[(size_t) i] = amp * (float) std::sin (w * i + (i >= jumpAt ? jump : 0.0));

        return x;
    }

    void zero (std::vector<float>& x, int from, int count)
    {
        for (int i = from; i < from + count && i < (int) x.size(); ++i)
            x[(size_t) i] = 0.0f;
    }

    void scale (std::vector<float>& x, int from, int count, float gain)
    {
        for (int i = from; i < from + count && i < (int) x.size(); ++i)
            x[(size_t) i] *= gain;
    }

    bool sameGlitches (const Glitches& a, const Glitches& b)
    {
        if (a.size() != b.size())
            return false;

        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].timeSeconds != b[i].timeSeconds || a[i].kind != b[i].kind
                || a[i].durationSeconds != b[i].durationSeconds)
                return false;

        return true;
    }

    juce::String describe (const Glitches& g)
    {
        juce::String s;
        for (const auto& e : g)
            s << " [" << e.kind << " @" << juce::String (e.timeSeconds, 4) << "]";

        return g.empty() ? juce::String (" none") : s;
    }
}

// =============================================================================
class ProbeAnalyzerTests final : public juce::UnitTest
{
public:
    ProbeAnalyzerTests() : juce::UnitTest ("ProbeAnalyzer", "probe") {}

    void runTest() override
    {
        beginTest ("a clean 10 s sine has no glitches and reports its level");
        {
            const auto x = TestSignals::sine (sr, hz, amp, at (10.0));
            ProbeAnalyzer analyzer (sr, hz);
            analyzer.process (x.data(), (int) x.size());

            expect (analyzer.getGlitches().empty(), "glitches:" + describe (analyzer.getGlitches()));
            expect (analyzer.isLocked(), "locks on the sine");
            expect (std::abs (analyzer.getSecondsAnalysed() - 10.0) < 1.0e-9);
            expect (std::abs (analyzer.getLevelDb() - TestSignals::rmsDb (x.data(), (int) x.size())) < 0.1f,
                    "level " + juce::String (analyzer.getLevelDb()));
        }

        beginTest ("a 20-sample gap at 3.0 s is one dropout");
        {
            auto x = TestSignals::sine (sr, hz, amp, at (10.0));
            zero (x, at (3.0), 20);

            const auto g = analyse (x);
            expectEquals ((int) g.size(), 1, "glitches:" + describe (g));

            if (g.size() == 1)
            {
                expectEquals (g[0].kind, juce::String ("dropout"));
                expect (std::abs (g[0].timeSeconds - 3.0) < 0.01, "time " + juce::String (g[0].timeSeconds));
                expect (std::abs (g[0].durationSeconds - 20.0 / sr) < 6.0 / sr,   // the edges add a few samples
                        "duration " + juce::String (g[0].durationSeconds));
            }
        }

        beginTest ("a gap is a dropout wherever it starts in the cycle, from 5 samples up");
        {
            for (const int length : { 5, 20, 480 })
            {
                for (int offset = 0; offset < 110; offset += 7)
                {
                    auto x = TestSignals::sine (sr, hz, amp, at (2.0));
                    const int start = at (1.0) + offset;
                    zero (x, start, length);

                    const auto g = analyse (x);
                    const auto where = "length " + juce::String (length) + " offset " + juce::String (offset)
                                       + ":" + describe (g);

                    expectEquals ((int) g.size(), 1, where);

                    if (g.size() == 1)
                    {
                        expectEquals (g[0].kind, juce::String ("dropout"), where);
                        expect (std::abs (g[0].timeSeconds - (double) start / sr) < 0.001, where);
                    }
                }
            }

            // Four zeros are below the dropout rule, but the waveform still
            // jumps at their edges, so they are not allowed to go unnoticed.
            auto x = TestSignals::sine (sr, hz, amp, at (2.0));
            zero (x, at (1.0) + 27, 4);   // around a crest, where zeros are a big step

            const auto g = analyse (x);
            expectEquals ((int) g.size(), 1, "4 zeros:" + describe (g));
            if (g.size() == 1)
                expectEquals (g[0].kind, juce::String ("jump"), "4 zeros are not a dropout");
        }

        beginTest ("a half-cycle phase jump at 6.0 s is one jump");
        {
            // 6.0 s is a zero crossing: the sign flips without any large
            // sample-to-sample step, so a step threshold alone cannot see it.
            const auto g = analyse (sineWithPhaseJump (at (10.0), at (6.0), pi));
            expectEquals ((int) g.size(), 1, "glitches:" + describe (g));

            if (g.size() == 1)
            {
                expectEquals (g[0].kind, juce::String ("jump"));
                expect (std::abs (g[0].timeSeconds - 6.0) < 0.01, "time " + juce::String (g[0].timeSeconds));
            }
        }

        beginTest ("phase jumps are found at every point of the cycle");
        {
            for (const double jump : { pi, pi / 2.0, 0.5 })
            {
                for (int offset = 0; offset < 110; ++offset)
                {
                    const int start = at (1.0) + offset;
                    const auto g = analyse (sineWithPhaseJump (at (1.5), start, jump));
                    const auto where = "jump " + juce::String (jump, 2) + " offset " + juce::String (offset)
                                       + ":" + describe (g);

                    expectEquals ((int) g.size(), 1, where);

                    if (g.size() == 1)
                    {
                        expectEquals (g[0].kind, juce::String ("jump"), where);
                        expect (std::abs (g[0].timeSeconds - (double) start / sr) < 0.01, where);
                    }
                }
            }
        }

        beginTest ("a dropped sample is found at nearly every phase, 8 dropped samples at all of them");
        {
            int found = 0;

            for (int offset = 0; offset < 110; ++offset)
            {
                auto x = TestSignals::sine (sr, hz, amp, at (1.5));
                x.erase (x.begin() + at (1.0) + offset);
                found += analyse (x).size() == 1 ? 1 : 0;
            }

            // The residual of a one-sample slip is 0.058 * cos (phase), so the
            // few phases near a zero of the cosine fall under the 0.5 % threshold
            // (4 of 110 do).
            expect (found >= 105, "found " + juce::String (found) + " of 110");

            for (int offset = 0; offset < 110; ++offset)
            {
                auto x = TestSignals::sine (sr, hz, amp, at (1.5));
                x.erase (x.begin() + at (1.0) + offset, x.begin() + at (1.0) + offset + 8);
                const auto g = analyse (x);
                expectEquals ((int) g.size(), 1, "offset " + juce::String (offset) + ":" + describe (g));
            }
        }

        beginTest ("a -10 dB dip for 50 ms is one level");
        {
            auto x = TestSignals::sine (sr, hz, amp, at (10.0));
            scale (x, at (4.0), at (0.050), 0.316f);

            const auto g = analyse (x);
            expectEquals ((int) g.size(), 1, "glitches:" + describe (g));

            if (g.size() == 1)
            {
                expectEquals (g[0].kind, juce::String ("level"));
                expect (std::abs (g[0].timeSeconds - 4.0) < 0.02, "time " + juce::String (g[0].timeSeconds));
            }
        }

        beginTest ("the dip stays one level whatever phase its hard edges fall on");
        {
            for (const int offset : { 0, 13, 29, 55, 71, 91, 105 })
            {
                auto x = TestSignals::sine (sr, hz, amp, at (2.0));
                scale (x, at (1.0) + offset, at (0.050), 0.316f);

                const auto g = analyse (x);
                expectEquals ((int) g.size(), 1, "offset " + juce::String (offset) + ":" + describe (g));

                if (g.size() == 1)
                    expectEquals (g[0].kind, juce::String ("level"), "offset " + juce::String (offset));
            }

            // Drifting by less than 6 dB is not a glitch: a smooth -4 dB fade
            // over 4 s must pass.
            auto fade = TestSignals::sine (sr, hz, amp, at (4.0));
            for (size_t i = 0; i < fade.size(); ++i)
                fade[i] *= (float) std::pow (10.0, -4.0 / 20.0 * (double) i / (double) fade.size());

            expect (analyse (fade).empty(), "a slow -4 dB fade is not a glitch");
        }

        beginTest ("the same result whatever the block sizes");
        {
            const auto clean = TestSignals::sine (sr, hz, amp, at (10.0));
            expect (sameGlitches (analyse (clean), analyseInRandomBlocks (clean, 1)));
            expect (analyseInRandomBlocks (clean, 2).empty());

            auto bad = clean;
            zero (bad, at (3.0) + 11, 20);
            scale (bad, at (5.0) + 5, at (0.050), 0.316f);
            for (size_t i = (size_t) at (7.0) + 3; i < bad.size(); ++i)
                bad[i] = -bad[i];   // a polarity flip: a half-cycle phase jump off the zero crossing

            const auto oneBlock = analyse (bad);
            expectEquals ((int) oneBlock.size(), 3, "glitches:" + describe (oneBlock));

            for (juce::int64 seed = 1; seed <= 6; ++seed)
                expect (sameGlitches (oneBlock, analyseInRandomBlocks (bad, seed)),
                        "random blocks, seed " + juce::String (seed));

            for (const int blockSize : { 1, 7, 480, 4096 })
            {
                ProbeAnalyzer analyzer (sr, hz);
                for (int start = 0; start < (int) bad.size(); start += blockSize)
                    analyzer.process (bad.data() + start, std::min (blockSize, (int) bad.size() - start));

                expect (sameGlitches (oneBlock, analyzer.getGlitches()), "block size " + juce::String (blockSize));
            }
        }

        beginTest ("only a steady sine at the expected pitch locks");
        {
            const auto n = at (3.0);
            const auto silence = std::vector<float> ((size_t) n, 0.0f);
            const auto dc      = std::vector<float> ((size_t) n, 0.3f);
            const auto noise   = TestSignals::whiteNoise (-20.0f, n, 7);
            const auto wrongHz = TestSignals::sine (sr, 1000.0, amp, n);
            const auto lowHz   = TestSignals::sine (sr, 100.0, amp, n);

            // 5 Hz tremolo, 50 % deep: the level never holds still for 100 ms.
            auto tremolo = TestSignals::sine (sr, hz, amp, n);
            for (size_t i = 0; i < tremolo.size(); ++i)
                tremolo[i] *= (float) (1.0 + 0.5 * std::sin (2.0 * pi * 5.0 * (double) i / sr));

            const std::pair<const char*, const std::vector<float>*> inputs[] = {
                { "silence", &silence }, { "DC", &dc }, { "white noise", &noise },
                { "1 kHz sine", &wrongHz }, { "100 Hz sine", &lowHz }, { "tremolo", &tremolo } };

            for (const auto& [label, signal] : inputs)
            {
                ProbeAnalyzer analyzer (sr, hz);
                analyzer.process (signal->data(), (int) signal->size());
                expect (! analyzer.isLocked(), juce::String (label) + " must not lock");
                expect (analyzer.getGlitches().empty(), juce::String (label) + " has no glitches to report");
            }
        }

        beginTest ("silence before the sine is not a glitch, silence after it is");
        {
            auto x = TestSignals::sine (sr, hz, amp, at (5.0));
            zero (x, 0, at (1.0));        // the sine only starts after 1 s
            zero (x, at (3.0), 400);

            const auto g = analyse (x);
            expectEquals ((int) g.size(), 1, "glitches:" + describe (g));

            if (g.size() == 1)
            {
                expectEquals (g[0].kind, juce::String ("dropout"));
                expect (std::abs (g[0].timeSeconds - 3.0) < 0.001);
            }
        }

        beginTest ("a long silence is one dropout that lasts as long as the silence");
        {
            auto x = TestSignals::sine (sr, hz, amp, at (8.0));
            zero (x, at (3.0), at (2.0));

            const auto g = analyse (x);
            expectEquals ((int) g.size(), 1, "glitches:" + describe (g));

            if (g.size() == 1)
            {
                expectEquals (g[0].kind, juce::String ("dropout"));
                expect (std::abs (g[0].timeSeconds - 3.0) < 0.001);
                expect (std::abs (g[0].durationSeconds - 2.0) < 0.01, "duration " + juce::String (g[0].durationSeconds));
            }
        }

        beginTest ("glitches closer than 50 ms merge, further apart stay separate");
        {
            auto close = TestSignals::sine (sr, hz, amp, at (6.0));
            zero (close, at (3.0), 20);
            zero (close, at (3.0) + at (0.030), 20);
            expectEquals ((int) analyse (close).size(), 1, "30 ms apart:" + describe (analyse (close)));

            auto far = TestSignals::sine (sr, hz, amp, at (6.0));
            zero (far, at (3.0), 20);
            zero (far, at (3.0) + at (0.200), 20);
            const auto g = analyse (far);
            expectEquals ((int) g.size(), 2, "200 ms apart:" + describe (g));

            if (g.size() == 2)
                expect (std::abs (g[1].timeSeconds - 3.2) < 0.001);
        }

        beginTest ("quantisation noise, hiss and a small pitch error are not glitches");
        {
            for (const double scaleToInt : { 32768.0, 8388608.0 })   // 16 and 24 bit
            {
                auto x = TestSignals::sine (sr, hz, amp, at (30.0));
                for (auto& v : x)
                    v = (float) (std::round ((double) v * scaleToInt) / scaleToInt);

                const auto g = analyse (x);
                expect (g.empty(), juce::String ((int) std::log2 (scaleToInt)) + " bit:" + describe (g));
            }

            auto hiss = TestSignals::sine (sr, hz, amp, at (30.0));
            const auto noise = TestSignals::whiteNoise (-80.0f, (int) hiss.size(), 3);
            for (size_t i = 0; i < hiss.size(); ++i)
                hiss[i] += noise[i];

            expect (analyse (hiss).empty(), "-80 dBFS hiss:" + describe (analyse (hiss)));

            // Two clocks never agree exactly: 0.01 % and 0.5 % off the nominal pitch.
            for (const double ratio : { 1.0001, 0.9999, 1.005, 0.995 })
            {
                const auto x = TestSignals::sine (sr, hz * ratio, amp, at (30.0));
                expect (analyse (x).empty(), "pitch x" + juce::String (ratio, 4) + ":" + describe (analyse (x)));
            }
        }

        beginTest ("the level follows the sine and is not a glitch to be reported twice");
        {
            const auto x = TestSignals::sine (sr, hz, 0.25f, at (2.0));
            ProbeAnalyzer analyzer (sr, hz);
            analyzer.process (x.data(), (int) x.size());

            expect (std::abs (analyzer.getLevelDb() - (-15.05f)) < 0.05f, "level " + juce::String (analyzer.getLevelDb()));
            expect (analyzer.getGlitches().empty());
        }

        beginTest ("odd input does not crash or poison the analysis");
        {
            ProbeAnalyzer analyzer (sr, hz);
            analyzer.process (nullptr, 0);
            analyzer.process (nullptr, -5);
            expectEquals (analyzer.getSecondsAnalysed(), 0.0);

            auto x = TestSignals::sine (sr, hz, amp, at (3.0));
            x[(size_t) at (2.0)] = std::numeric_limits<float>::quiet_NaN();
            analyzer.process (x.data(), (int) x.size());

            expect (analyzer.getGlitches().size() == 1, "a NaN is reported:" + describe (analyzer.getGlitches()));
            expect (std::isfinite (analyzer.getLevelDb()), "level stays finite");
        }
    }
};

static ProbeAnalyzerTests probeAnalyzerTests;
