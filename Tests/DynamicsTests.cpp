// Unit tests for the dynamics stages of the voice chain: noise gate, compressor
// and de-esser.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe dynamics

#include <JuceHeader.h>

#include "TestSignals.h"

#include "../Source/Dsp/Compressor.h"
#include "../Source/Dsp/DeEsser.h"
#include "../Source/Dsp/NoiseGate.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace
{
    constexpr double rate = 48000.0;
    constexpr double pi = 3.14159265358979323846;

    int samplesFor (double seconds)   { return juce::roundToInt (rate * seconds); }

    float toLinear (float db)         { return juce::Decibels::decibelsToGain (db); }

    /// Sine whose PEAK is `peakDb` dBFS.
    std::vector<float> tone (double hz, float peakDb, int numSamples, double phase = 0.0)
    {
        return TestSignals::sine (rate, hz, toLinear (peakDb), numSamples, phase);
    }

    /// Sine whose RMS is `rmsDb` dBFS (peak = RMS + 3.01 dB).
    std::vector<float> toneRms (double hz, float rmsDb, int numSamples)
    {
        return TestSignals::sine (rate, hz, toLinear (rmsDb) * (float) std::sqrt (2.0), numSamples);
    }

    /// Runs `process` over `x` in blocks of `blockSize` samples and returns what
    /// `meter()` read after each block: entry k is the reading at sample
    /// (k + 1) * blockSize.
    template <typename Process, typename Meter>
    std::vector<float> meterTrace (std::vector<float>& x, int blockSize, Process&& process, Meter&& meter)
    {
        std::vector<float> trace;
        const int total = (int) x.size();

        for (int start = 0; start < total; start += blockSize)
        {
            process (x.data() + start, std::min (blockSize, total - start));
            trace.push_back (meter());
        }

        return trace;
    }

    /// Processes the first `jumpAt` samples, calls `jump()` (the parameter
    /// change, between two blocks like in the app), then processes the rest.
    template <typename Process, typename Jump>
    std::vector<float> processWithJump (std::vector<float> x, int jumpAt, Process&& process, Jump&& jump)
    {
        process (x.data(), jumpAt);
        jump();
        process (x.data() + jumpAt, (int) x.size() - jumpAt);
        return x;
    }

    /// The part of the signal that can differ between a run with a parameter jump
    /// and one without; compared on its own so a start-up transient cannot hide a
    /// click at the jump.
    float stepAfter (const std::vector<float>& x, int jumpAt)
    {
        return TestSignals::maxStep (x.data() + jumpAt - 1, (int) x.size() - jumpAt + 1);
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

    bool allFinite (const std::vector<float>& x)
    {
        return std::all_of (x.begin(), x.end(), [] (float v) { return std::isfinite (v); });
    }

    /// RMS change in dB of a steady tone through `process`, the first 200 ms skipped
    /// and the next 200 ms measured. `process` gets a freshly generated buffer.
    template <typename Process>
    float toneGainDb (const std::vector<float>& in, Process&& process)
    {
        auto out = in;
        process (out.data(), (int) out.size());

        const int skip = samplesFor (0.2), window = samplesFor (0.2);
        return TestSignals::rmsDb (out.data() + skip, window) - TestSignals::rmsDb (in.data() + skip, window);
    }
}

// =============================================================================
class DynamicsTests final : public juce::UnitTest
{
public:
    DynamicsTests() : juce::UnitTest ("Dynamics", "dynamics") {}

    void runTest() override
    {
        using namespace TestSignals;

        // ---------------------------------------------------------------------
        beginTest ("gate opens within 2 ms");
        {
            const int total = samplesFor (1.0), onset = samplesFor (0.5), window = samplesFor (0.005);

            // -70 dBFS room noise, then a -20 dBFS 300 Hz tone from 0.5 s.
            auto in = whiteNoise (-70.0f, total, 1234);
            const auto burst = tone (300.0, -20.0f, total - onset);
            for (size_t i = 0; i < burst.size(); ++i)
                in[(size_t) onset + i] += burst[i];

            NoiseGate gate;
            gate.prepare (rate);
            gate.setThresholdDb (-50.0f);

            const auto out = processInBlocks (in, 64, [&] (float* x, int n) { gate.process (x, n); });

            const float inDb  = rmsDb (in.data() + onset, window);
            const float outDb = rmsDb (out.data() + onset, window);
            expectWithinAbsoluteError (outDb, inDb, 1.5f,
                                       "5 ms after onset: " + juce::String (outDb) + " dB vs " + juce::String (inDb) + " dB in");

            // The noise before the tone really was held back, or the test proves nothing.
            const float noiseIn  = rmsDb (in.data() + samplesFor (0.3), samplesFor (0.15));
            const float noiseOut = rmsDb (out.data() + samplesFor (0.3), samplesFor (0.15));
            expect (noiseOut <= noiseIn - 25.0f,
                    "noise before the tone: " + juce::String (noiseOut) + " dB vs " + juce::String (noiseIn) + " dB in");
        }

        // ---------------------------------------------------------------------
        beginTest ("gate holds then closes");
        {
            const int total = samplesFor (2.0), blockSize = 48;   // 1 ms blocks

            auto in = whiteNoise (-70.0f, total, 99);
            const auto burst = tone (300.0, -20.0f, samplesFor (0.5));   // 0.5 s .. 1.0 s
            for (size_t i = 0; i < burst.size(); ++i)
                in[(size_t) samplesFor (0.5) + i] += burst[i];

            NoiseGate gate;
            gate.prepare (rate);
            gate.setThresholdDb (-50.0f);

            const auto gr = meterTrace (in, blockSize,
                                        [&] (float* x, int n) { gate.process (x, n); },
                                        [&] { return gate.getGainReductionDb(); });

            auto timeOf = [&] (size_t block) { return (double) ((int) block + 1) * blockSize / rate; };

            float worstWhileOpen = 0.0f, worstInHold = 0.0f, worstWhenClosed = -1000.0f;
            for (size_t k = 0; k < gr.size(); ++k)
            {
                const double t = timeOf (k);

                if (t > 0.6 && t <= 1.0)          worstWhileOpen = std::min (worstWhileOpen, gr[k]);
                if (t > 1.0 && t <= 1.12)         worstInHold = std::min (worstInHold, gr[k]);
                if (t >= 1.0 + 0.15 + 0.15 + 0.05) worstWhenClosed = std::max (worstWhenClosed, gr[k]);
            }

            expect (worstWhileOpen > -1.0f, "reduction while the tone plays: " + juce::String (worstWhileOpen));
            expect (worstInHold > -1.0f, "reduction during the hold: " + juce::String (worstInHold));
            expect (worstWhenClosed <= -25.0f, "reduction after hold + release + 50 ms: " + juce::String (worstWhenClosed));
            expectWithinAbsoluteError (gr.back(), -30.0f, 1.0f, "closed: " + juce::String (gr.back()));
            expect (gr.back() <= 0.0f);
        }

        // ---------------------------------------------------------------------
        beginTest ("gate does not chatter");
        {
            const int blockSize = 48, burstEnd = samplesFor (0.1), segment = samplesFor (0.02);
            const int total = burstEnd + samplesFor (1.0);

            // A -30 dBFS burst opens the gate, then the level flips every 20 ms
            // between -52 dBFS (inside the 4 dB hysteresis band under the -50 dBFS
            // threshold) and -49 dBFS (just above it).
            std::vector<float> in ((size_t) total);
            for (int i = 0; i < total; ++i)
            {
                const float level = i < burstEnd ? -30.0f
                                                 : (((i - burstEnd) / segment) % 2 == 0 ? -52.0f : -49.0f);
                in[(size_t) i] = toLinear (level) * (float) std::sin (2.0 * pi * 200.0 * (double) i / rate);
            }

            NoiseGate gate;
            gate.prepare (rate);
            gate.setThresholdDb (-50.0f);

            const auto gr = meterTrace (in, blockSize,
                                        [&] (float* x, int n) { gate.process (x, n); },
                                        [&] { return gate.getGainReductionDb(); });

            const size_t firstBlockAfterBurst = (size_t) (burstEnd / blockSize);
            expect (gr[firstBlockAfterBurst - 1] > -1.0f, "the burst did not open the gate");

            // "Closed" means more than half of the 30 dB range is applied.
            int changes = 0;
            bool wasClosed = false;
            float worst = 0.0f;
            for (size_t k = firstBlockAfterBurst; k < gr.size(); ++k)
            {
                const bool closed = gr[k] < -15.0f;
                changes += closed != wasClosed ? 1 : 0;
                wasClosed = closed;
                worst = std::min (worst, gr[k]);
            }

            expect (changes <= 1, "state changes: " + juce::String (changes));
            expect (worst > -1.0f, "the gate started closing: " + juce::String (worst) + " dB");
        }

        // ---------------------------------------------------------------------
        beginTest ("gate hysteresis keeps its state inside the band");
        {
            const int blockSize = 48;

            // 200 Hz keeps a long gap between the peaks the detector sees.
            auto run = [&] (bool openFirst, float levelDb, double seconds)
            {
                const int burstLength = openFirst ? samplesFor (0.1) : 0;
                const int total = burstLength + samplesFor (seconds);

                std::vector<float> in ((size_t) total);
                for (int i = 0; i < total; ++i)
                    in[(size_t) i] = toLinear (i < burstLength ? -30.0f : levelDb)
                                     * (float) std::sin (2.0 * pi * 200.0 * (double) i / rate);

                NoiseGate gate;
                gate.prepare (rate);
                gate.setThresholdDb (-50.0f);

                const auto gr = meterTrace (in, blockSize,
                                            [&] (float* x, int n) { gate.process (x, n); },
                                            [&] { return gate.getGainReductionDb(); });

                struct Result { float worst, best, last; };
                const auto afterBurst = gr.begin() + burstLength / blockSize;
                return Result { *std::min_element (afterBurst, gr.end()),
                                *std::max_element (afterBurst, gr.end()),
                                gr.back() };
            };

            // Open gate, level 2 dB under the open threshold but 2 dB over the close threshold: stays open.
            const auto stays = run (true, -52.0f, 1.5);
            expect (stays.worst > -1.0f, "open gate at -52 dBFS: worst " + juce::String (stays.worst));

            // Closed gate, same level: stays closed (it only opens above -50 dBFS).
            const auto stillClosed = run (false, -52.0f, 1.0);
            expect (stillClosed.best < -29.0f, "closed gate at -52 dBFS: best " + juce::String (stillClosed.best));

            // Open gate, level 2 dB under the close threshold: closes.
            const auto closes = run (true, -56.0f, 1.0);
            expect (closes.last <= -29.0f, "open gate at -56 dBFS ends at " + juce::String (closes.last));
        }

        // ---------------------------------------------------------------------
        beginTest ("compressor static curve");
        {
            auto steadyOutDb = [&] (float inRmsDb, float thresholdDb, float ratio, float makeupDb)
            {
                Compressor comp;
                comp.prepare (rate);
                comp.setParams (thresholdDb, ratio, makeupDb);

                auto x = toneRms (1000.0, inRmsDb, samplesFor (0.5));
                comp.process (x.data(), (int) x.size());
                return rmsDb (x.data() + samplesFor (0.3), samplesFor (0.1));
            };

            const float loud  = steadyOutDb (-5.0f, -20.0f, 3.0f, 3.0f);
            const float quiet = steadyOutDb (-30.0f, -20.0f, 3.0f, 3.0f);
            const float unity = steadyOutDb (-5.0f, -20.0f, 1.0f, 3.0f);

            expectWithinAbsoluteError (loud, -12.0f, 0.5f, "-5 dB RMS in: " + juce::String (loud));
            expectWithinAbsoluteError (quiet, -27.0f, 0.5f, "-30 dB RMS in: " + juce::String (quiet));
            expectWithinAbsoluteError (unity, -2.0f, 0.1f, "ratio 1: " + juce::String (unity));

            // staticCurveDb: values from the Giannoulis/Reiss formulas.
            expectWithinAbsoluteError (Compressor::staticCurveDb (-30.0f, -20.0f, 3.0f, 6.0f), -30.0f, 1.0e-4f);
            expectWithinAbsoluteError (Compressor::staticCurveDb (-23.0f, -20.0f, 3.0f, 6.0f), -23.0f, 1.0e-4f);
            expectWithinAbsoluteError (Compressor::staticCurveDb (-20.0f, -20.0f, 3.0f, 6.0f), -20.5f, 1.0e-4f);
            expectWithinAbsoluteError (Compressor::staticCurveDb (-17.0f, -20.0f, 3.0f, 6.0f), -19.0f, 1.0e-4f);
            expectWithinAbsoluteError (Compressor::staticCurveDb (-5.0f, -20.0f, 3.0f, 6.0f), -15.0f, 1.0e-4f);

            // Continuous and monotonic across the knee, for several settings.
            struct Setting { float threshold, ratio, knee; };
            for (const auto& s : { Setting { -20.0f, 3.0f, 6.0f }, Setting { -10.0f, 10.0f, 6.0f },
                                   Setting { -35.0f, 1.5f, 6.0f }, Setting { -20.0f, 4.0f, 0.0f } })
            {
                float previous = Compressor::staticCurveDb (-40.0f, s.threshold, s.ratio, s.knee);
                bool monotonic = true, continuous = true;

                for (int i = 1; i <= 4000; ++i)
                {
                    const float in = -40.0f + 0.01f * (float) i;
                    const float out = Compressor::staticCurveDb (in, s.threshold, s.ratio, s.knee);

                    monotonic = monotonic && out >= previous - 1.0e-4f;
                    // The slope never exceeds 1, so a 0.01 dB step moves the output by at most 0.01 dB.
                    continuous = continuous && out - previous <= 0.01f + 1.0e-4f;
                    previous = out;
                }

                expect (monotonic, "not monotonic for threshold " + juce::String (s.threshold)
                                   + ", ratio " + juce::String (s.ratio));
                expect (continuous, "not continuous for threshold " + juce::String (s.threshold)
                                    + ", ratio " + juce::String (s.ratio));
            }
        }

        // ---------------------------------------------------------------------
        beginTest ("compressor first setParams does not glide");
        {
            Compressor comp;
            comp.prepare (rate);
            comp.setParams (-20.0f, 3.0f, 12.0f);   // the very first call after prepare()

            // -40 dB RMS is far below the knee: the output is the input plus the makeup,
            // from the first sample on.
            auto x = toneRms (1000.0, -40.0f, samplesFor (0.01));
            const float inDb = rmsDb (x.data(), (int) x.size());
            comp.process (x.data(), (int) x.size());

            expectWithinAbsoluteError (rmsDb (x.data(), (int) x.size()) - inDb, 12.0f, 0.3f);
        }

        // ---------------------------------------------------------------------
        beginTest ("compressor attack and release");
        {
            const int blockSize = 48;   // 1 ms
            const int stepUp = samplesFor (0.5), stepDown = stepUp + samplesFor (0.6);
            const int total = stepDown + samplesFor (0.8);

            // 1 kHz, -40 dB RMS, then -5 dB RMS from 0.5 s, back to -40 dB RMS 0.6 s later.
            std::vector<float> in ((size_t) total);
            for (int i = 0; i < total; ++i)
            {
                const float rms = (i >= stepUp && i < stepDown) ? -5.0f : -40.0f;
                in[(size_t) i] = toLinear (rms) * (float) std::sqrt (2.0)
                                 * (float) std::sin (2.0 * pi * 1000.0 * (double) i / rate);
            }

            Compressor comp;
            comp.prepare (rate);
            comp.setParams (-20.0f, 3.0f, 0.0f);

            const auto gr = meterTrace (in, blockSize,
                                        [&] (float* x, int n) { comp.process (x, n); },
                                        [&] { return comp.getGainReductionDb(); });

            auto after = [&] (int stepSample, double seconds)
            {
                return gr[(size_t) ((stepSample + samplesFor (seconds)) / blockSize - 1)];
            };

            const float steady = after (stepUp, 0.59);
            const float at30ms = after (stepUp, 0.030);
            const float released = after (stepDown, 0.6);

            expectWithinAbsoluteError (steady, -10.0f, 0.5f, "steady reduction " + juce::String (steady));
            expect (at30ms <= 0.8f * steady, "30 ms after the step: " + juce::String (at30ms) + " of " + juce::String (steady));
            expect (released > -1.0f, "600 ms after the step down: " + juce::String (released));
            expect (released <= 0.0f);

            // The release is slow: 20 ms after the step down most of the reduction is still there.
            expect (after (stepDown, 0.020) < 0.5f * steady,
                    "20 ms after the step down: " + juce::String (after (stepDown, 0.020)));
        }

        // ---------------------------------------------------------------------
        beginTest ("de-esser idle is flat");
        {
            DeEsser mid;
            mid.prepare (rate);
            mid.setAmount (50.0f);
            const float body = toneGainDb (tone (1000.0, -20.0f, samplesFor (0.4)),
                                           [&] (float* x, int n) { mid.process (x, n); });
            expectWithinAbsoluteError (body, 0.0f, 0.1f, "1 kHz at amount 50: " + juce::String (body));

            DeEsser off;
            off.prepare (rate);
            off.setAmount (0.0f);
            const float hiss = toneGainDb (tone (7000.0, -15.0f, samplesFor (0.4)),
                                           [&] (float* x, int n) { off.process (x, n); });
            expectWithinAbsoluteError (hiss, 0.0f, 0.1f, "7 kHz at amount 0: " + juce::String (hiss));
            expectWithinAbsoluteError (off.getGainReductionDb(), 0.0f, 0.01f);
        }

        // ---------------------------------------------------------------------
        beginTest ("de-esser reduces sibilance");
        {
            auto run = [&] (float amount)
            {
                DeEsser d;
                d.prepare (rate);
                d.setAmount (amount);
                const float change = toneGainDb (tone (7000.0, -15.0f, samplesFor (0.4)),
                                                 [&] (float* x, int n) { d.process (x, n); });
                return std::pair { change, d.getGainReductionDb() };
            };

            const auto half = run (50.0f);
            const auto full = run (100.0f);

            expect (half.first <= -3.0f, "amount 50: " + juce::String (half.first) + " dB");
            expect (full.first <= -6.0f, "amount 100: " + juce::String (full.first) + " dB");
            expect (full.first < half.first, "amount 100 is not stronger than amount 50");
            expect (half.second < -2.0f, "meter at amount 50: " + juce::String (half.second));
            expect (full.second < -2.0f, "meter at amount 100: " + juce::String (full.second));
            expect (full.second >= -DeEsser::maxReductionDb - 0.01f);
        }

        // ---------------------------------------------------------------------
        beginTest ("de-esser spares the voice body");
        {
            // A 1 kHz vowel with a faint 7 kHz component: the band does not dominate.
            auto voice = tone (1000.0, -10.0f, samplesFor (0.4));
            const auto air = tone (7000.0, -40.0f, samplesFor (0.4));
            for (size_t i = 0; i < voice.size(); ++i)
                voice[i] += air[i];

            for (const float amount : { 50.0f, 100.0f })
            {
                DeEsser d;
                d.prepare (rate);
                d.setAmount (amount);
                const float change = toneGainDb (voice, [&] (float* x, int n) { d.process (x, n); });

                expectWithinAbsoluteError (change, 0.0f, 0.3f,
                                           "amount " + juce::String (amount) + ": " + juce::String (change));
                expect (d.getGainReductionDb() > -0.5f, "meter " + juce::String (d.getGainReductionDb()));
            }
        }

        // ---------------------------------------------------------------------
        beginTest ("de-esser first setAmount does not glide");
        {
            DeEsser d;
            d.prepare (rate);
            d.setAmount (100.0f);   // the very first call after prepare()

            // The detectors and the 2 ms attack need a few ms, a 20 ms glide from the
            // default 50 % to 100 % would still be well short of 12 dB after 8 ms.
            auto x = tone (7000.0, -15.0f, samplesFor (0.008));
            d.process (x.data(), (int) x.size());
            expect (d.getGainReductionDb() <= -10.0f, "meter after 8 ms: " + juce::String (d.getGainReductionDb()));
        }

        // ---------------------------------------------------------------------
        beginTest ("gate keeps speech and quiets the pauses");
        {
            const auto speech = loadSpeechFixture (rate);
            expect (speech.size() > 100000, "speech fixture did not load");

            // Speech over -72 dBFS room noise, gate at -50 dBFS. The detector follows peaks and the
            // peaks of white noise sit about 12 dB over its RMS, so the noise has to peak under the
            // close threshold (-54 dBFS) to be gated.
            const auto mask = speechMask (speech, rate, -35.0f);
            auto noisy = speech;
            const auto noise = whiteNoise (-72.0f, (int) speech.size(), 7);
            for (size_t i = 0; i < noisy.size(); ++i)
                noisy[i] += noise[i];

            NoiseGate gate;
            gate.prepare (rate);
            gate.setThresholdDb (-50.0f);
            const auto out = processInBlocks (noisy, 480, [&] (float* x, int n) { gate.process (x, n); });

            // Power in the speech frames, and in the pause samples: those that come more than
            // 0.4 s (hold plus release plus a margin) after the speech last peaked over
            // -60 dBFS, so what is left is the room noise alone.
            double speechIn = 0.0, speechOut = 0.0, pauseIn = 0.0, pauseOut = 0.0;
            size_t pauseSamples = 0, quietRun = 0;

            for (size_t i = 0; i < noisy.size(); ++i)
            {
                quietRun = std::abs (speech[i]) < toLinear (-60.0f) ? quietRun + 1 : 0;

                if (mask[i])
                {
                    speechIn  += (double) noisy[i] * noisy[i];
                    speechOut += (double) out[i] * out[i];
                }
                else if (quietRun > (size_t) samplesFor (0.4))
                {
                    pauseIn  += (double) noisy[i] * noisy[i];
                    pauseOut += (double) out[i] * out[i];
                    ++pauseSamples;
                }
            }

            expect (pauseSamples > (size_t) samplesFor (0.5), "the fixture has too little pause to test: " + juce::String ((int) pauseSamples));

            const float speechChange = (float) (10.0 * std::log10 (speechOut / speechIn));
            const float pauseChange = (float) (10.0 * std::log10 (pauseOut / pauseIn));
            expect (speechChange > -1.0f, "speech changed by " + juce::String (speechChange) + " dB");
            expect (pauseChange < -20.0f, "pauses changed by " + juce::String (pauseChange) + " dB");
        }

        // ---------------------------------------------------------------------
        beginTest ("compressor evens out speech");
        {
            const auto speech = loadSpeechFixture (rate);
            expect (speech.size() > 100000, "speech fixture did not load");

            Compressor comp;
            comp.prepare (rate);
            comp.setParams (-40.0f, 6.0f, 0.0f);
            const auto out = processInBlocks (speech, 480, [&] (float* x, int n) { comp.process (x, n); });

            // Loudness range: 90th minus 20th percentile of the 50 ms levels that are not silence.
            auto loudnessRange = [&] (const std::vector<float>& x)
            {
                std::vector<float> levels;
                for (size_t start = 0; start + 2400 <= x.size(); start += 2400)
                {
                    const float level = rmsDb (x.data() + start, 2400);
                    if (level > -60.0f)
                        levels.push_back (level);
                }

                std::sort (levels.begin(), levels.end());
                return levels[levels.size() * 9 / 10] - levels[levels.size() / 5];
            };

            const float before = loudnessRange (speech), after = loudnessRange (out);
            expect (after < before - 2.0f, "loudness range " + juce::String (before) + " dB -> " + juce::String (after) + " dB");
        }

        // ---------------------------------------------------------------------
        beginTest ("de-esser works on the sibilants of speech and spares the rest");
        {
            const auto speech = loadSpeechFixture (rate);
            expect (speech.size() > 100000, "speech fixture did not load");

            DeEsser d;
            d.prepare (rate);
            d.setAmount (100.0f);

            float deepest = 0.0f;
            const auto out = processInBlocks (speech, 480, [&] (float* x, int n)
            {
                d.process (x, n);
                deepest = std::min (deepest, d.getGainReductionDb());
            });

            expect (deepest <= -6.0f, "deepest cut " + juce::String (deepest) + " dB");

            // The sibilants are a small part of the energy: the whole recording hardly changes.
            const float change = rmsDb (out.data(), (int) out.size()) - rmsDb (speech.data(), (int) speech.size());
            expect (change > -0.5f && change <= 0.0f, "overall level change " + juce::String (change) + " dB");
        }

        // ---------------------------------------------------------------------
        beginTest ("output is finite and non-finite parameters are ignored");
        {
            const auto speech = loadSpeechFixture (rate);
            expect (speech.size() > 48000, "speech fixture did not load");

            // Speech, loud noise, then digital silence: the three cases that break detectors.
            std::vector<float> in = speech;
            const auto noise = whiteNoise (-3.0f, samplesFor (1.0), 5);
            in.insert (in.end(), noise.begin(), noise.end());
            in.insert (in.end(), (size_t) samplesFor (2.0), 0.0f);

            // The most extreme settings the UI allows.
            NoiseGate gate;
            gate.prepare (rate);
            gate.setThresholdDb (-20.0f);
            auto gated = in;
            gate.process (gated.data(), (int) gated.size());
            expect (allFinite (gated), "gate output is not finite");
            expect (peakAbs (gated.data(), (int) gated.size()) <= peakAbs (in.data(), (int) in.size()),
                    "the gate made the signal louder");

            Compressor comp;
            comp.prepare (rate);
            comp.setParams (-40.0f, 10.0f, 18.0f);
            auto squashed = in;
            comp.process (squashed.data(), (int) squashed.size());
            expect (allFinite (squashed), "compressor output is not finite");
            expect (peakAbs (squashed.data(), (int) squashed.size()) < 100.0f, "compressor output is out of range");
            expect (comp.getGainReductionDb() <= 0.0f && std::isfinite (comp.getGainReductionDb()));

            DeEsser esser;
            esser.prepare (rate);
            esser.setAmount (100.0f);
            auto cleaned = in;
            esser.process (cleaned.data(), (int) cleaned.size());
            expect (allFinite (cleaned), "de-esser output is not finite");
            expect (peakAbs (cleaned.data(), (int) cleaned.size()) < 10.0f, "de-esser output is out of range");

            // After digital silence the meters are back at rest.
            expect (gate.getGainReductionDb() <= 0.0f);
            expect (esser.getGainReductionDb() > -0.01f, "de-esser meter after silence: " + juce::String (esser.getGainReductionDb()));
            expect (comp.getGainReductionDb() > -0.01f, "compressor meter after silence: " + juce::String (comp.getGainReductionDb()));

            // NaN and infinity are ignored, so the settings stay what they were.
            const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
            const auto probe = tone (500.0, -20.0f, samplesFor (0.3));

            auto gateOut = [&] (bool poke)
            {
                NoiseGate g;
                g.prepare (rate);
                g.setThresholdDb (-25.0f);
                if (poke) { g.setThresholdDb (nan); g.setThresholdDb (inf); g.setThresholdDb (-inf); }
                auto x = probe;
                g.process (x.data(), (int) x.size());
                return x;
            };
            auto compOut = [&] (bool poke)
            {
                Compressor c;
                c.prepare (rate);
                c.setParams (-30.0f, 4.0f, 2.0f);
                if (poke) { c.setParams (nan, 2.0f, 2.0f); c.setParams (-30.0f, inf, 2.0f); c.setParams (-30.0f, 4.0f, -inf); }
                auto x = probe;
                c.process (x.data(), (int) x.size());
                return x;
            };
            auto esserOut = [&] (bool poke)
            {
                DeEsser e;
                e.prepare (rate);
                e.setAmount (70.0f);
                if (poke) { e.setAmount (nan); e.setAmount (inf); e.setAmount (-inf); }
                auto x = tone (7000.0, -15.0f, samplesFor (0.3));
                e.process (x.data(), (int) x.size());
                return x;
            };

            expect (largestDifference (gateOut (false), gateOut (true)) == 0.0f, "gate took a non-finite threshold");
            expect (largestDifference (compOut (false), compOut (true)) == 0.0f, "compressor took a non-finite parameter");
            expect (largestDifference (esserOut (false), esserOut (true)) == 0.0f, "de-esser took a non-finite amount");

            // Out-of-range values are limited, not rejected: 500 % acts like 100 %.
            auto esserAt = [&] (float amount)
            {
                DeEsser e;
                e.prepare (rate);
                e.setAmount (amount);
                auto x = tone (7000.0, -15.0f, samplesFor (0.3));
                e.process (x.data(), (int) x.size());
                return x;
            };
            expect (largestDifference (esserAt (500.0f), esserAt (100.0f)) == 0.0f, "amount is not limited to 100");
            expect (largestDifference (esserAt (-30.0f), esserAt (0.0f)) == 0.0f, "amount is not limited to 0");
        }

        // ---------------------------------------------------------------------
        beginTest ("smooth parameter changes");
        {
            const int total = samplesFor (1.0);
            // 1 kHz has a 48 sample period, so sample 12 mod 48 is a peak: the worst
            // place for an unsmoothed gain step.
            const int jumpAt = 24000 + 12;

            auto expectNoClick = [&] (const char* label, const std::vector<float>& calm,
                                      const std::vector<float>& jumped)
            {
                const float calmStep = stepAfter (calm, jumpAt), jumpedStep = stepAfter (jumped, jumpAt);
                expect (calmStep > 0.0f, juce::String (label) + ": silent test signal");
                expect (jumpedStep <= 1.5f * calmStep,
                        juce::String (label) + ": maxStep " + juce::String (jumpedStep)
                        + " vs " + juce::String (calmStep) + " without the jump");
            };

            // Gate: the threshold flips the decision; the gain behind it has to ramp.
            {
                const auto signal = tone (1000.0, -30.0f, total);

                auto gateRun = [&] (float before, float after)
                {
                    NoiseGate g;
                    g.prepare (rate);
                    g.setThresholdDb (before);
                    return processWithJump (signal, jumpAt, [&] (float* x, int n) { g.process (x, n); },
                                            [&] { g.setThresholdDb (after); });
                };

                const auto open = gateRun (-50.0f, -50.0f);
                const auto closing = gateRun (-50.0f, -20.0f);   // open, then shut by the new threshold
                const auto opening = gateRun (-20.0f, -50.0f);   // shut, then opened by the new threshold

                expectNoClick ("gate closing", open, closing);
                expectNoClick ("gate opening", open, opening);

                // The jumps really did change the gate.
                expect (rmsDb (closing.data() + total - 4800, 4800) <= rmsDb (open.data() + total - 4800, 4800) - 25.0f);
                expectWithinAbsoluteError (rmsDb (opening.data() + total - 4800, 4800),
                                           rmsDb (open.data() + total - 4800, 4800), 0.5f);
            }

            // Compressor: +2 dB of makeup under the threshold, and a deeper setting on a loud tone.
            {
                auto compRun = [&] (const std::vector<float>& signal, float t0, float r0, float m0,
                                    float t1, float r1, float m1)
                {
                    Compressor c;
                    c.prepare (rate);
                    c.setParams (t0, r0, m0);
                    return processWithJump (signal, jumpAt, [&] (float* x, int n) { c.process (x, n); },
                                            [&] { c.setParams (t1, r1, m1); });
                };

                const auto quiet = toneRms (1000.0, -30.0f, total);
                const auto calmQuiet = compRun (quiet, -20.0f, 3.0f, 3.0f, -20.0f, 3.0f, 3.0f);
                const auto louder = compRun (quiet, -20.0f, 3.0f, 3.0f, -20.0f, 3.0f, 5.0f);
                expectNoClick ("compressor makeup", calmQuiet, louder);
                expectWithinAbsoluteError (rmsDb (louder.data() + total - 4800, 4800)
                                           - rmsDb (calmQuiet.data() + total - 4800, 4800), 2.0f, 0.2f);

                const auto loud = toneRms (1000.0, -12.0f, total);
                const auto calmLoud = compRun (loud, -20.0f, 3.0f, 3.0f, -20.0f, 3.0f, 3.0f);
                const auto squeezed = compRun (loud, -20.0f, 3.0f, 3.0f, -26.0f, 4.0f, 5.0f);
                expectNoClick ("compressor threshold, ratio and makeup", calmLoud, squeezed);
                expect (rmsDb (squeezed.data() + total - 4800, 4800) < rmsDb (calmLoud.data() + total - 4800, 4800) - 2.0f);
            }

            // De-esser: the reduction ramps in and out.
            {
                const auto signal = tone (7000.0, -15.0f, total);

                auto esserRun = [&] (float before, float after)
                {
                    DeEsser d;
                    d.prepare (rate);
                    d.setAmount (before);
                    return processWithJump (signal, jumpAt, [&] (float* x, int n) { d.process (x, n); },
                                            [&] { d.setAmount (after); });
                };

                const auto idle = esserRun (0.0f, 0.0f);
                const auto engaging = esserRun (0.0f, 100.0f);
                expectNoClick ("de-esser engaging", idle, engaging);

                // A 7 kHz tone is too fast for maxStep to see a gain step, so also look at
                // the first millisecond after the jump: the cut must still be building up.
                const float firstMs = rmsDb (engaging.data() + jumpAt, 48) - rmsDb (idle.data() + jumpAt, 48);
                expect (firstMs > -2.0f, "de-esser engaging: already " + juce::String (firstMs) + " dB down after 1 ms");
                expect (rmsDb (engaging.data() + total - 4800, 4800) < rmsDb (idle.data() + total - 4800, 4800) - 6.0f);

                // Letting go must not overshoot the level of a de-esser that was idle all along.
                const auto letGo = esserRun (100.0f, 0.0f);
                expectNoClick ("de-esser letting go", idle, letGo);
                expectWithinAbsoluteError (rmsDb (letGo.data() + total - 4800, 4800),
                                           rmsDb (idle.data() + total - 4800, 4800), 0.3f);
            }
        }

        // ---------------------------------------------------------------------
        beginTest ("block-size independence");
        {
            const auto speech = loadSpeechFixture (rate);
            expect (speech.size() > 100000, "speech fixture did not load");

            // Speech followed by 100 ms bursts of a 7 kHz tone with gaps, so the
            // de-esser has sibilance to work on as well.
            std::vector<float> in = speech;
            for (int burst = 0; burst < 6; ++burst)
            {
                const auto part = (burst % 2 == 0) ? tone (7000.0, -20.0f, samplesFor (0.1))
                                                   : std::vector<float> ((size_t) samplesFor (0.1), 0.0f);
                in.insert (in.end(), part.begin(), part.end());
            }

            // Parameter changes at fixed samples, none on a multiple of 7, 64 or 1024.
            // The later ones land in the sibilance burst section.
            const int changeAt[] = { 10001, 30011, 84001, (int) speech.size() + 5003 };

            auto runGate = [&] (int blockSize)
            {
                NoiseGate g;
                g.prepare (rate);
                g.setThresholdDb (-60.0f);
                const float thresholds[] = { -35.0f, -70.0f, -25.0f, -45.0f };
                return runWithChanges (in, blockSize, changeAt,
                                       [&] (float* x, int n) { g.process (x, n); },
                                       [&] (int c) { g.setThresholdDb (thresholds[c]); });
            };

            auto runComp = [&] (int blockSize)
            {
                Compressor c;
                c.prepare (rate);
                c.setParams (-30.0f, 3.0f, 3.0f);
                const float t[] = { -40.0f, -25.0f, -35.0f, -30.0f }, r[] = { 4.0f, 2.0f, 8.0f, 3.0f },
                            m[] = { 6.0f, 0.0f, 12.0f, 3.0f };
                return runWithChanges (in, blockSize, changeAt,
                                       [&] (float* x, int n) { c.process (x, n); },
                                       [&] (int k) { c.setParams (t[k], r[k], m[k]); });
            };

            auto runEsser = [&] (int blockSize)
            {
                DeEsser d;
                d.prepare (rate);
                d.setAmount (30.0f);
                const float amounts[] = { 100.0f, 0.0f, 60.0f, 100.0f };
                return runWithChanges (in, blockSize, changeAt,
                                       [&] (float* x, int n) { d.process (x, n); },
                                       [&] (int k) { d.setAmount (amounts[k]); });
            };

            struct Stage { const char* name; std::function<std::vector<float> (int)> run; };
            const Stage stages[] = { { "gate", runGate }, { "compressor", runComp }, { "de-esser", runEsser } };

            for (const auto& stage : stages)
            {
                const auto reference = stage.run (1);

                // Guard against a test that passes because nothing happened.
                expect (largestDifference (reference, in) > 1.0e-3f, juce::String (stage.name) + ": output is the input");

                for (const int blockSize : { 7, 64, 1024 })
                {
                    const float diff = largestDifference (reference, stage.run (blockSize));
                    expect (diff <= 1.0e-6f, juce::String (stage.name) + ": block size " + juce::String (blockSize)
                                             + " differs by " + juce::String (diff));
                }
            }
        }
    }

private:
    /// Runs `process` over `in` in blocks of `blockSize`, calling `change (k)` at
    /// the sample `changeAt[k]` (a block is cut short to land exactly on it).
    template <typename Process, typename Change>
    static std::vector<float> runWithChanges (const std::vector<float>& in, int blockSize, const int (&changeAt)[4],
                                              Process&& process, Change&& change)
    {
        std::vector<float> out = in;
        int position = 0, next = 0;

        while (position < (int) out.size())
        {
            if (next < 4 && position == changeAt[next])
                change (next++);

            int count = std::min (blockSize, (int) out.size() - position);
            if (next < 4)
                count = std::min (count, changeAt[next] - position);

            process (out.data() + position, count);
            position += count;
        }

        return out;
    }
};

static DynamicsTests dynamicsTests;
