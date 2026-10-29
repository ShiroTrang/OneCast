// Unit tests for the voice-chain filters: the biquad building block (shared with
// the de-esser), the 3rd-order high-pass and the three-band voice EQ.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe filters

#include <JuceHeader.h>

#include "TestSignals.h"

#include "../Source/Dsp/Biquad.h"
#include "../Source/Dsp/HighPassFilter.h"
#include "../Source/Dsp/VoiceEq.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
    constexpr double pi = 3.14159265358979323846;

    /// Gain of `process` at `hz` in dB, measured the way the spec says: a steady
    /// sine, the first 200 ms skipped so the filter and its smoothers settle,
    /// then the RMS of the next 200 ms against the RMS of the input over the
    /// same window. `process` is called as process (float* samples, int n) on a
    /// freshly prepared processor owned by the caller.
    template <typename Fn>
    float measureGainDb (double sr, double hz, Fn&& process)
    {
        const int skip = juce::roundToInt (sr * 0.2);
        const int window = juce::roundToInt (sr * 0.2);

        const auto in = TestSignals::sine (sr, hz, 0.25f, skip + window);
        auto out = in;
        process (out.data(), (int) out.size());

        return TestSignals::rmsDb (out.data() + skip, window)
             - TestSignals::rmsDb (in.data() + skip, window);
    }

    /// |H (e^jw)| in dB straight from the coefficients, so the design formulas
    /// are checked without running any audio through them.
    float magnitudeDb (const BiquadCoeffs& c, double sr, double hz)
    {
        const std::complex<double> z1 = std::polar (1.0, -2.0 * pi * hz / sr);
        const std::complex<double> z2 = z1 * z1;

        const auto num = (double) c.b0 + (double) c.b1 * z1 + (double) c.b2 * z2;
        const auto den = 1.0 + (double) c.a1 * z1 + (double) c.a2 * z2;

        return (float) (20.0 * std::log10 (std::abs (num / den)));
    }

    float largestDifference (const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size())
            return 1.0e9f;

        float worst = 0.0f;
        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max (worst, std::abs (a[i] - b[i]));

        return worst;
    }
}

// =============================================================================
class FilterTests final : public juce::UnitTest
{
public:
    FilterTests() : juce::UnitTest ("Filters", "filters") {}

    void runTest() override
    {
        using namespace TestSignals;

        beginTest ("Biquad design matches the RBJ responses");
        {
            const double sr = 48000.0;

            // Butterworth high-pass: -3.01 dB at the corner, flat far above it.
            const auto hp = BiquadDesign::highPass (sr, 1000.0, 0.70710678);
            expectWithinAbsoluteError (magnitudeDb (hp, sr, 1000.0), -3.01f, 0.05f);
            expectWithinAbsoluteError (magnitudeDb (hp, sr, 15000.0), 0.0f, 0.05f);
            expect (magnitudeDb (hp, sr, 100.0) < -35.0f);

            // Peaking bell: exactly the requested gain at its centre, nothing at the edges.
            for (const double gain : { -9.0, 6.0 })
            {
                const auto bell = BiquadDesign::peaking (sr, 2000.0, 1.3, gain);
                expectWithinAbsoluteError (magnitudeDb (bell, sr, 2000.0), (float) gain, 0.02f);
                expectWithinAbsoluteError (magnitudeDb (bell, sr, 20.0), 0.0f, 0.05f);
                expectWithinAbsoluteError (magnitudeDb (bell, sr, 23000.0), 0.0f, 0.2f);
            }

            // High shelf: nothing below, the full gain above, half the gain (in dB)
            // at the corner for a slope of 1.
            for (const double gain : { -6.0, 5.0 })
            {
                const auto shelf = BiquadDesign::highShelf (sr, 8000.0, 1.0, gain);
                expectWithinAbsoluteError (magnitudeDb (shelf, sr, 40.0), 0.0f, 0.05f);
                expectWithinAbsoluteError (magnitudeDb (shelf, sr, 23900.0), (float) gain, 0.3f);
                expectWithinAbsoluteError (magnitudeDb (shelf, sr, 8000.0), (float) (gain / 2.0), 0.05f);
            }

            // Zero gain must be an exact identity, or a flat EQ would colour the voice.
            const auto flatBell = BiquadDesign::peaking (sr, 250.0, 1.0, 0.0);
            const auto flatShelf = BiquadDesign::highShelf (sr, 10000.0, 0.7, 0.0);
            expect (flatBell.b0 == 1.0f && flatBell.b1 == flatBell.a1 && flatBell.b2 == flatBell.a2);
            expect (flatShelf.b0 == 1.0f && flatShelf.b1 == flatShelf.a1 && flatShelf.b2 == flatShelf.a2);
        }

        beginTest ("Biquad keeps its state across setCoeffs and clears it on reset");
        {
            // b0 = 0, b1 = 1 is a one-sample delay: the held sample sits in the state.
            BiquadCoeffs delay;
            delay.b0 = 0.0f;
            delay.b1 = 1.0f;

            Biquad biquad;
            biquad.setCoeffs (delay);
            expectEquals (biquad.processSample (1.0f), 0.0f);

            biquad.setCoeffs (delay);   // new coefficients, same state
            expectEquals (biquad.processSample (0.0f), 1.0f);

            biquad.processSample (1.0f);
            biquad.reset();
            expectEquals (biquad.processSample (0.0f), 0.0f);

            // Default coefficients pass the signal untouched.
            Biquad fresh;
            expectEquals (fresh.processSample (0.37f), 0.37f);
        }

        beginTest ("HPF corner and stopband");
        {
            auto gainAt = [] (double sr, float cutoffHz, double hz)
            {
                HighPassFilter hpf;
                hpf.prepare (sr);
                hpf.setCutoffHz (cutoffHz);
                return measureGainDb (sr, hz, [&] (float* x, int n) { hpf.process (x, n); });
            };

            const float corner48 = gainAt (48000.0, 80.0f, 80.0);
            const float stop48   = gainAt (48000.0, 80.0f, 40.0);
            const float pass48   = gainAt (48000.0, 80.0f, 1000.0);

            expectWithinAbsoluteError (corner48, -3.0f, 0.5f, "80 Hz @ 48k: " + juce::String (corner48));
            expect (stop48 <= -15.0f, "40 Hz @ 48k: " + juce::String (stop48));
            expectWithinAbsoluteError (pass48, 0.0f, 0.1f, "1 kHz @ 48k: " + juce::String (pass48));

            for (const double sr : { 44100.0, 96000.0 })
            {
                const float corner = gainAt (sr, 80.0f, 80.0);
                expectWithinAbsoluteError (corner, -3.0f, 0.5f,
                                           "80 Hz @ " + juce::String (sr) + ": " + juce::String (corner));
            }

            // The cut-off range of the UI: the corner must follow the setting.
            expectWithinAbsoluteError (gainAt (48000.0, 40.0f, 40.0), -3.0f, 0.5f);
            expectWithinAbsoluteError (gainAt (48000.0, 200.0f, 200.0), -3.0f, 0.5f);
        }

        beginTest ("EQ band gains");
        {
            auto gainAt = [] (double sr, double hz)
            {
                VoiceEq eq;
                eq.prepare (sr);
                eq.setGains (-2.0f, 2.0f, 1.0f);
                return measureGainDb (sr, hz, [&] (float* x, int n) { eq.process (x, n); });
            };

            const double sr = 48000.0;
            const float mud = gainAt (sr, 250.0);
            const float clarity = gainAt (sr, 3500.0);
            const float air = gainAt (sr, 15000.0);
            const float mid = gainAt (sr, 1000.0);

            expectWithinAbsoluteError (mud, -2.0f, 0.3f, "250 Hz: " + juce::String (mud));
            expectWithinAbsoluteError (clarity, 2.0f, 0.3f, "3500 Hz: " + juce::String (clarity));
            expectWithinAbsoluteError (air, 1.0f, 0.3f, "15 kHz: " + juce::String (air));
            expectWithinAbsoluteError (mid, 0.0f, 0.6f, "1 kHz: " + juce::String (mid));

            // The bands are independent of each other: only the one that is set moves.
            VoiceEq eq;
            eq.prepare (sr);
            eq.setGains (0.0f, 6.0f, 0.0f);
            const float onlyClarity250 = measureGainDb (sr, 250.0, [&] (float* x, int n) { eq.process (x, n); });
            expectWithinAbsoluteError (onlyClarity250, 0.0f, 0.3f);
        }

        beginTest ("EQ flat is identity");
        {
            const auto speech = loadSpeechFixture (48000.0);
            expect (speech.size() > 48000, "speech fixture did not load");

            VoiceEq eq;
            eq.prepare (48000.0);
            eq.setGains (0.0f, 0.0f, 0.0f);

            auto out = speech;
            eq.process (out.data(), (int) out.size());

            expect (largestDifference (speech, out) <= 1.0e-6f,
                    "largest difference " + juce::String (largestDifference (speech, out)));
        }

        beginTest ("smooth parameter changes");
        {
            const double sr = 48000.0;
            const int total = 48000;
            const int jumpAt = 24000;
            const float level = juce::Decibels::decibelsToGain (-12.0f);

            // Runs a 1 kHz sine through the chosen stages. `jump` changes the
            // parameters at sample `jumpAt`, between two blocks like in the app.
            auto run = [&] (bool useHpf, bool useEq, bool jump)
            {
                HighPassFilter hpf;
                VoiceEq eq;
                hpf.prepare (sr);
                eq.prepare (sr);
                hpf.setCutoffHz (40.0f);
                eq.setGains (0.0f, 0.0f, 0.0f);

                auto chain = [&] (float* x, int n)
                {
                    if (useHpf) hpf.process (x, n);
                    if (useEq)  eq.process (x, n);
                };

                auto first = sine (sr, 1000.0, level, jumpAt);
                chain (first.data(), jumpAt);

                if (jump)
                {
                    hpf.setCutoffHz (200.0f);
                    eq.setGains (-12.0f, 8.0f, 6.0f);
                }

                auto second = sine (sr, 1000.0, level, total - jumpAt,
                                    2.0 * pi * 1000.0 / sr * (double) jumpAt);
                chain (second.data(), total - jumpAt);

                first.insert (first.end(), second.begin(), second.end());
                return first;
            };

            for (const auto& [useHpf, useEq, label] : { std::tuple { true, false, "HPF" },
                                                       std::tuple { false, true, "EQ" },
                                                       std::tuple { true, true, "HPF + EQ" } })
            {
                const auto calm = run (useHpf, useEq, false);
                const auto jumped = run (useHpf, useEq, true);

                // Only the part after the jump can differ; compare it on its own so
                // the start-up transient cannot hide a click at the jump.
                const float calmStep = maxStep (calm.data() + jumpAt - 1, total - jumpAt + 1);
                const float jumpedStep = maxStep (jumped.data() + jumpAt - 1, total - jumpAt + 1);

                expect (jumpedStep <= 1.2f * calmStep,
                        juce::String (label) + ": maxStep " + juce::String (jumpedStep)
                        + " vs " + juce::String (calmStep) + " without the jump");
            }

            // The change really is a ramp of about 20 ms, not a step. A tone sits
            // where the parameter bites; right after the jump it must still have
            // most of its level (a step would already have taken it away), and
            // within 100 ms it must have settled at the new gain.
            auto jumpResponse = [&] (double toneHz, auto&& setBefore, auto&& setAfter, auto&& process)
            {
                const int at = 12000;
                const int period = (int) std::round (sr / toneHz);

                setBefore();
                auto tone = sine (sr, toneHz, level, 2 * at);
                process (tone.data(), at);
                setAfter();
                process (tone.data() + at, at);

                struct Result { float firstPeriodRatio, settledDb; };
                return Result { peakAbs (tone.data() + at, period) / peakAbs (tone.data() + at - 2 * period, period),
                                rmsDb (tone.data() + at + 4800, 4800) - rmsDb (tone.data() + at - 4800, 4800) };
            };

            {
                VoiceEq eq;
                eq.prepare (sr);

                const auto r = jumpResponse (250.0,
                                             [&] { eq.setGains (0.0f, 0.0f, 0.0f); },
                                             [&] { eq.setGains (-12.0f, 0.0f, 0.0f); },
                                             [&] (float* x, int n) { eq.process (x, n); });

                expect (r.firstPeriodRatio >= 0.8f,
                        "EQ: first period after the jump already at " + juce::String (r.firstPeriodRatio));
                expectWithinAbsoluteError (r.settledDb, -12.0f, 0.5f);
            }

            {
                HighPassFilter hpf;
                hpf.prepare (sr);

                // 150 Hz is 8.2 dB down once the corner sits at 200 Hz.
                const auto r = jumpResponse (150.0,
                                             [&] { hpf.setCutoffHz (40.0f); },
                                             [&] { hpf.setCutoffHz (200.0f); },
                                             [&] (float* x, int n) { hpf.process (x, n); });

                expect (r.firstPeriodRatio >= 0.8f,
                        "HPF: first period after the jump already at " + juce::String (r.firstPeriodRatio));
                expectWithinAbsoluteError (r.settledDb, -8.2f, 0.5f);
            }
        }

        beginTest ("block-size independence");
        {
            const auto speech = loadSpeechFixture (48000.0);
            expect (speech.size() > 100000, "speech fixture did not load");

            // The fixture is digital silence for its first 0.3 s, so the jump at
            // sample 10000 only proves the plumbing. The later ones land in loud
            // speech, and none of them sits on a multiple of the block sizes 7, 64 or 1024.
            struct Change { int at; float cutoffHz, mudDb, clarityDb, airDb; };
            const Change changes[] = { { 10000, 150.0f, -10.0f, 6.0f, 4.0f },
                                       { 30011,  50.0f,  -2.0f, 2.0f, 1.0f },
                                       { 84001, 120.0f,  -6.0f, 8.0f, 3.0f } };

            // Parameters change at the same sample whatever the block size, so any
            // difference between the runs comes from the filters.
            auto run = [&] (int blockSize, bool useHpf, bool useEq)
            {
                HighPassFilter hpf;
                VoiceEq eq;
                hpf.prepare (48000.0);
                eq.prepare (48000.0);
                hpf.setCutoffHz (60.0f);
                eq.setGains (-2.0f, 2.0f, 1.0f);

                auto chain = [&] (float* x, int n)
                {
                    if (useHpf) hpf.process (x, n);
                    if (useEq)  eq.process (x, n);
                };

                std::vector<float> out;
                size_t position = 0;

                auto processUpTo = [&] (size_t end)
                {
                    std::vector<float> part (speech.begin() + (std::ptrdiff_t) position,
                                             speech.begin() + (std::ptrdiff_t) end);
                    const auto done = processInBlocks (std::move (part), blockSize, chain);
                    out.insert (out.end(), done.begin(), done.end());
                    position = end;
                };

                for (const auto& change : changes)
                {
                    processUpTo ((size_t) change.at);
                    hpf.setCutoffHz (change.cutoffHz);
                    eq.setGains (change.mudDb, change.clarityDb, change.airDb);
                }

                processUpTo (speech.size());
                return out;
            };

            for (const auto& [useHpf, useEq, label] : { std::tuple { true, false, "HPF" },
                                                       std::tuple { false, true, "EQ" },
                                                       std::tuple { true, true, "HPF + EQ" } })
            {
                const auto reference = run (1, useHpf, useEq);

                // Guard against a test that passes because nothing happened.
                expect (largestDifference (reference, speech) > 1.0e-3f,
                        juce::String (label) + ": output is the input");

                for (const int blockSize : { 7, 64, 1024 })
                {
                    const float diff = largestDifference (reference, run (blockSize, useHpf, useEq));
                    expect (diff <= 1.0e-6f,
                            juce::String (label) + ": block size " + juce::String (blockSize)
                            + " differs by " + juce::String (diff));
                }
            }
        }
    }
};

static FilterTests filterTests;
