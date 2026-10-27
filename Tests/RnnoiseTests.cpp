// Unit tests for the AI noise stage: RNNoise wrapped as RnnoiseDenoiser, and the
// StreamingResampler that carries non-48 kHz devices through it. They use the
// speech fixture (sentences separated by digital-silence gaps) so "speech",
// "noise only" and "alignment" can be measured, not guessed. Every measured
// figure is logged so a run documents what the stage really does.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe rnnoise

#include <JuceHeader.h>

#include "TestSignals.h"

#include "../Source/Dsp/RnnoiseDenoiser.h"
#include "../Source/Dsp/StreamingResampler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{
    using namespace TestSignals;

    /// The console runner does not show juce::Logger output, and the measured
    /// figures are the point of several tests, so they go straight to stdout.
    void report (const juce::String& line)
    {
        std::printf ("    [measured] %s\n", line.toRawUTF8());
        std::fflush (stdout);
    }

    struct Result
    {
        std::vector<float> out;
        int latency = 0;
    };

    /// Fresh denoiser, amount set before the first sample (so it applies at once),
    /// the whole signal pushed through in blocks of `blockSize`.
    Result denoise (const std::vector<float>& in, double sr, float amountPercent, int blockSize = 480)
    {
        RnnoiseDenoiser denoiser;
        denoiser.prepare (sr, blockSize);
        denoiser.setAmount (amountPercent);

        Result r;
        r.out = processInBlocks (in, blockSize, [&] (float* block, int n) { denoiser.process (block, n); });
        r.latency = denoiser.getLatencySamples();
        return r;
    }

    std::vector<float> add (std::vector<float> a, const std::vector<float>& b)
    {
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            a[i] += b[i];

        return a;
    }

    std::vector<float> firstSeconds (const std::vector<float>& x, double sr, double seconds)
    {
        const size_t n = std::min (x.size(), (size_t) (seconds * sr));
        return std::vector<float> (x.begin(), x.begin() + (std::ptrdiff_t) n);
    }

    /// RMS in dBFS of `x` over the samples where `include` is true.
    float rmsDbWhere (const std::vector<float>& x, const std::vector<bool>& include,
                      size_t from, size_t to)
    {
        double sumSquares = 0.0;
        size_t count = 0;

        for (size_t i = from; i < to && i < x.size() && i < include.size(); ++i)
            if (include[i])
            {
                sumSquares += (double) x[i] * (double) x[i];
                ++count;
            }

        if (count == 0)
            return silenceDb;

        const double rms = std::sqrt (sumSquares / (double) count);
        return rms > 1.0e-10 ? (float) (20.0 * std::log10 (rms)) : silenceDb;
    }

    /// Output sample i + latency belongs to input sample i: shift the output back
    /// so both can be compared index by index. The tail that has no counterpart
    /// is padded with zeros.
    std::vector<float> alignToInput (const std::vector<float>& out, int latency)
    {
        std::vector<float> aligned (out.size(), 0.0f);

        for (size_t i = 0; i + (size_t) latency < out.size(); ++i)
            aligned[i] = out[i + (size_t) latency];

        return aligned;
    }

    std::vector<bool> invert (std::vector<bool> mask)
    {
        mask.flip();
        return mask;
    }
}

class RnnoiseDenoiserTests final : public juce::UnitTest
{
public:
    RnnoiseDenoiserTests() : juce::UnitTest ("RnnoiseDenoiser", "rnnoise") {}

    void runTest() override
    {
        beginTest ("the speech fixture is available");
        const auto speech48 = loadSpeechFixture (48000.0);
        expect (! speech48.empty(), "speech fixture could not be loaded");
        if (speech48.empty())
            return;

        beginTest ("the reported latency is 3 frames at 48 kHz and grows when resampling");
        {
            RnnoiseDenoiser d;
            d.prepare (48000.0, 512);
            expectEquals (d.getLatencySamples(), 1440);

            for (const double sr : { 22050.0, 32000.0, 44100.0, 88200.0, 96000.0, 192000.0 })
            {
                d.prepare (sr, 512);
                const double ideal = 1440.0 * sr / 48000.0;
                expect (d.getLatencySamples() > ideal,
                        "latency at " + juce::String (sr) + " must include the converters");
                expect (d.getLatencySamples() < ideal + 0.01 * sr,
                        "latency at " + juce::String (sr) + " grew by more than 10 ms: "
                            + juce::String (d.getLatencySamples()));
            }
        }

        beginTest ("zero amount is a pure delay");
        {
            for (const double sr : { 48000.0, 44100.0, 96000.0 })
            {
                const auto in = loadSpeechFixture (sr);
                const auto r = denoise (in, sr, 0.0f);
                expect (r.latency > 0);

                float worst = 0.0f;
                for (size_t i = 0; i + (size_t) r.latency < in.size(); ++i)
                    worst = std::max (worst, std::abs (r.out[i + (size_t) r.latency] - in[i]));

                expect (worst < 1.0e-6f, "worst error at " + juce::String (sr) + " Hz: " + juce::String (worst));

                float headPeak = peakAbs (r.out.data(), r.latency);
                expectEquals (headPeak, 0.0f, "the first latency samples must be silent");
            }
        }

        beginTest ("wet path is aligned with the reported latency");
        {
            // RNNoise is not a pure delay (its spectral gains and DC filter shift
            // the correlation peak by a fraction of a 48 kHz sample, measured
            // below), so the tolerance is the brief's +-1 at 48 kHz and +-2 at
            // 44.1 and 96 kHz, and one device-rate sample more per 48 kHz for
            // the extra rates.
            struct Case { double sr; int tolerance; };

            for (const auto c : { Case { 48000.0, 1 }, Case { 44100.0, 2 }, Case { 96000.0, 2 },
                                  Case { 32000.0, 2 }, Case { 88200.0, 3 }, Case { 192000.0, 5 } })
            {
                // A few seconds are plenty for the correlation, and keep the
                // brute-force search affordable at 192 kHz.
                const auto in = firstSeconds (loadSpeechFixture (c.sr), c.sr, c.sr > 100000.0 ? 1.5 : 3.0);
                const auto r = denoise (in, c.sr, 100.0f);

                const int lag = crossCorrelationLag (in, r.out, r.latency + 400);
                report ("wet lag at " + juce::String (c.sr) + " Hz: " + juce::String (lag)
                        + " (reported " + juce::String (r.latency) + ")");

                expect (std::abs (lag - r.latency) <= c.tolerance,
                        "lag " + juce::String (lag) + " vs reported " + juce::String (r.latency)
                            + " at " + juce::String (c.sr) + " Hz");
            }
        }

        beginTest ("the resampled path never runs dry, at any rate or block size");
        {
            // If the down-converter ever had to wait for input, that output
            // sample would be an exact 0.0 and the whole path would slip. Noise
            // makes every genuine output sample non-zero, so zeros give it away.
            for (const double sr : { 8000.0, 11025.0, 16000.0, 22050.0, 24000.0, 32000.0, 37800.0, 44100.0,
                                     47999.0, 50000.0, 64000.0, 88200.0, 96000.0, 176400.0, 192000.0, 384000.0 })
            {
                RnnoiseDenoiser d;
                d.prepare (sr, 64);
                d.setAmount (100.0f);

                const int latency = d.getLatencySamples();
                const int n = latency + (int) (0.4 * sr);
                const auto out = processInBlocks (whiteNoise (-30.0f, n, 101), 7,
                                                  [&] (float* b, int count) { d.process (b, count); });

                int zeros = 0;
                for (size_t i = (size_t) latency + 2000; i < out.size(); ++i)
                    zeros += out[i] == 0.0f ? 1 : 0;

                expectEquals (zeros, 0, "exact zeros after start-up at " + juce::String (sr) + " Hz");
                expect (latency > 1440.0 * sr / 48000.0, "latency at " + juce::String (sr) + " Hz is too small");
            }
        }

        beginTest ("noise is removed");
        {
            struct Case { double sr; float minimumDb; };

            for (const auto c : { Case { 48000.0, 15.0f }, Case { 44100.0, 12.0f } })
            {
                const int n = (int) (3.0 * c.sr);
                const auto in = whiteNoise (-35.0f, n, 11);
                const auto r = denoise (in, c.sr, 100.0f);

                const int tail = (int) c.sr;   // last second
                const float before = rmsDb (in.data() + n - tail, tail);
                const float after  = rmsDb (r.out.data() + n - tail, tail);
                const float reduction = before - after;

                report ("white noise -35 dBFS at " + juce::String (c.sr) + " Hz: "
                            + juce::String (reduction, 1) + " dB reduction");
                expect (reduction >= c.minimumDb,
                        "only " + juce::String (reduction, 1) + " dB at " + juce::String (c.sr) + " Hz");
            }
        }

        beginTest ("speech survives, the noise between sentences does not");
        {
            const auto noisy = add (speech48, whiteNoise (-45.0f, (int) speech48.size(), 21));
            const auto r = denoise (noisy, 48000.0, 100.0f);
            const auto aligned = alignToInput (r.out, r.latency);

            const auto speechMaskClean = speechMask (speech48, 48000.0);
            // Noise-only samples are the fixture's digital-silence pauses (clean
            // signal below -60 dBFS), speech samples the usual -35 dBFS mask.
            const auto noiseOnly = invert (speechMask (speech48, 48000.0, -60.0f));

            const size_t from = 48000;   // noise-only is judged after the first second
            const size_t end = noisy.size() - (size_t) r.latency;

            const float speechBefore = rmsDbWhere (noisy,   speechMaskClean, 0, end);
            const float speechAfter  = rmsDbWhere (aligned, speechMaskClean, 0, end);
            const float noiseBefore  = rmsDbWhere (noisy,   noiseOnly, from, end);
            const float noiseAfter   = rmsDbWhere (aligned, noiseOnly, from, end);

            report ("speech RMS " + juce::String (speechBefore, 1) + " -> " + juce::String (speechAfter, 1)
                        + " dBFS (drop " + juce::String (speechBefore - speechAfter, 1) + " dB)");
            report ("noise-only RMS " + juce::String (noiseBefore, 1) + " -> " + juce::String (noiseAfter, 1)
                        + " dBFS (drop " + juce::String (noiseBefore - noiseAfter, 1) + " dB)");

            expect (speechBefore - speechAfter <= 4.0f,
                    "speech lost " + juce::String (speechBefore - speechAfter, 1) + " dB");
            expect (noiseBefore - noiseAfter >= 12.0f,
                    "noise only fell " + juce::String (noiseBefore - noiseAfter, 1) + " dB");
        }

        beginTest ("VAD separates speech from noise");
        {
            const int frame = RnnoiseDenoiser::frameSize;
            const auto noisy = add (speech48, whiteNoise (-45.0f, (int) speech48.size(), 31));
            const auto clean = speechMask (speech48, 48000.0);

            RnnoiseDenoiser d;
            d.prepare (48000.0, frame);

            double speechSum = 0.0, noiseSum = 0.0;
            int speechFrames = 0, noiseFrames = 0;
            std::vector<float> block ((size_t) frame);

            for (int k = 0; (k + 1) * frame <= (int) noisy.size(); ++k)
            {
                std::copy (noisy.begin() + k * frame, noisy.begin() + (k + 1) * frame, block.begin());
                d.process (block.data(), frame);

                // After the first second, so RNNoise has settled.
                if (k * frame < 48000)
                    continue;

                // The estimate belongs to the analysis window centred on the
                // start of this frame, i.e. the previous and the current frame.
                int speechSamples = 0;
                for (int i = (k - 1) * frame; i < (k + 1) * frame; ++i)
                    speechSamples += clean[(size_t) i] ? 1 : 0;

                if (speechSamples >= 2 * frame * 9 / 10)
                {
                    speechSum += d.getSpeechProbability();
                    ++speechFrames;
                }
                else if (speechSamples == 0)
                {
                    noiseSum += d.getSpeechProbability();
                    ++noiseFrames;
                }
            }

            expect (speechFrames > 100 && noiseFrames > 50,
                    "too few frames: " + juce::String (speechFrames) + " speech, " + juce::String (noiseFrames) + " noise");

            const double speechMean = speechSum / std::max (speechFrames, 1);
            const double noiseMean = noiseSum / std::max (noiseFrames, 1);
            report ("mean VAD: speech " + juce::String (speechMean, 3) + " (" + juce::String (speechFrames)
                        + " frames), noise only " + juce::String (noiseMean, 3) + " (" + juce::String (noiseFrames) + " frames)");

            expect (speechMean > noiseMean + 0.3,
                    "speech " + juce::String (speechMean, 3) + " vs noise " + juce::String (noiseMean, 3));
        }

        beginTest ("block-size independence");
        {
            for (const double sr : { 48000.0, 44100.0 })
            {
                const auto in = add (firstSeconds (loadSpeechFixture (sr), sr, 4.0),
                                     whiteNoise (-40.0f, (int) (4.0 * sr), 41));

                const auto reference = denoise (in, sr, 70.0f, 1024).out;

                for (const int blockSize : { 1, 64, 441, 1024 })
                {
                    const auto out = denoise (in, sr, 70.0f, blockSize).out;

                    float worst = 0.0f;
                    for (size_t i = 0; i < out.size(); ++i)
                        worst = std::max (worst, std::abs (out[i] - reference[i]));

                    expect (worst < 1.0e-5f, "block size " + juce::String (blockSize) + " at "
                                                 + juce::String (sr) + " Hz differs by " + juce::String (worst));
                }
            }
        }

        beginTest ("cheap enough");
        {
            const int n = 480000;   // 10 s
            auto x = add (std::vector<float> (speech48.begin(), speech48.begin() + std::min ((size_t) n, speech48.size())),
                          whiteNoise (-40.0f, n, 51));
            x.resize ((size_t) n, 0.0f);

            RnnoiseDenoiser d;
            d.prepare (48000.0, 128);

            // Best of three: other builds and tests often share the machine, and
            // the question is what the code costs, not what the scheduler did
            // to one unlucky run.
            double ms = 1.0e9;
            for (int run = 0; run < 3; ++run)
            {
                d.reset();
                auto block = x;

                const double start = juce::Time::getMillisecondCounterHiRes();
                for (int i = 0; i < n; i += 128)
                    d.process (block.data() + i, std::min (128, n - i));

                ms = std::min (ms, juce::Time::getMillisecondCounterHiRes() - start);
            }

            report ("10 s of audio at 48 kHz processed in " + juce::String (ms, 1) + " ms (best of 3)");

           #if JUCE_DEBUG
            // The budget is for an optimised build; RNNoise's C sources are not
            // optimised in Debug and run an order of magnitude slower.
            juce::ignoreUnused (ms);
           #else
            expect (ms < 1000.0, "took " + juce::String (ms, 1) + " ms");
           #endif
        }

        beginTest ("amount ramps linearly over 20 ms and is independent of the block size");
        {
            const auto in = add (firstSeconds (speech48, 48000.0, 3.0), whiteNoise (-40.0f, 144000, 61));
            const auto wet = denoise (in, 48000.0, 100.0f).out;
            const auto dry = denoise (in, 48000.0, 0.0f).out;

            const int switchAt = 48000;
            const int ramp = (int) std::lround (RnnoiseDenoiser::amountRampSeconds * 48000.0);

            for (const int blockSize : { 480, 160 })
            {
                RnnoiseDenoiser d;
                d.prepare (48000.0, blockSize);
                d.setAmount (100.0f);

                auto x = in;
                for (int start = 0; start < (int) x.size(); start += blockSize)
                {
                    if (start == switchAt)
                        d.setAmount (0.0f);

                    d.process (x.data() + start, std::min (blockSize, (int) x.size() - start));
                }

                float worst = 0.0f;
                for (int i = 0; i < (int) x.size(); ++i)
                {
                    const float a = i < switchAt ? 1.0f
                                                 : std::max (0.0f, 1.0f - (float) (i - switchAt + 1) / (float) ramp);
                    const float expected = a * wet[(size_t) i] + (1.0f - a) * dry[(size_t) i];
                    worst = std::max (worst, std::abs (x[(size_t) i] - expected));
                }

                expect (worst < 1.0e-4f, "block " + juce::String (blockSize) + ": deviates from a "
                                             + juce::String (ramp) + "-sample linear ramp by " + juce::String (worst));
            }
        }

        beginTest ("half amount is the average of wet and dry");
        {
            const auto in = add (firstSeconds (speech48, 48000.0, 2.0), whiteNoise (-40.0f, 96000, 71));
            const auto wet = denoise (in, 48000.0, 100.0f).out;
            const auto dry = denoise (in, 48000.0, 0.0f).out;
            const auto half = denoise (in, 48000.0, 50.0f).out;

            float worst = 0.0f;
            for (size_t i = 0; i < half.size(); ++i)
                worst = std::max (worst, std::abs (half[i] - 0.5f * (wet[i] + dry[i])));

            expect (worst < 1.0e-6f, "worst error " + juce::String (worst));
        }

        beginTest ("non-finite input becomes silence and does not poison the state");
        {
            auto in = add (firstSeconds (speech48, 48000.0, 2.0), whiteNoise (-40.0f, 96000, 81));
            auto replaced = in;

            for (size_t i = 20000; i < 20010; ++i)
            {
                in[i] = std::numeric_limits<float>::quiet_NaN();
                replaced[i] = 0.0f;
            }

            in[30000] = std::numeric_limits<float>::infinity();
            replaced[30000] = 0.0f;
            in[30001] = -std::numeric_limits<float>::infinity();
            replaced[30001] = 0.0f;

            const auto a = denoise (in, 48000.0, 100.0f).out;
            const auto b = denoise (replaced, 48000.0, 100.0f).out;

            bool allFinite = true;
            float worst = 0.0f;
            for (size_t i = 0; i < a.size(); ++i)
            {
                allFinite = allFinite && std::isfinite (a[i]);
                worst = std::max (worst, std::abs (a[i] - b[i]));
            }

            expect (allFinite, "output contains NaN or Inf");
            expect (worst < 1.0e-6f, "differs from the zero-substituted run by " + juce::String (worst));
        }

        beginTest ("reset forgets the past; prepare can switch sample rate");
        {
            const auto in = add (firstSeconds (speech48, 48000.0, 2.0), whiteNoise (-40.0f, 96000, 91));

            RnnoiseDenoiser d;
            d.prepare (48000.0, 480);
            d.setAmount (80.0f);
            const auto first = processInBlocks (in, 480, [&] (float* b, int n) { d.process (b, n); });

            d.reset();
            const auto second = processInBlocks (in, 480, [&] (float* b, int n) { d.process (b, n); });

            float worst = 0.0f;
            for (size_t i = 0; i < in.size(); ++i)
                worst = std::max (worst, std::abs (first[i] - second[i]));

            expect (worst < 1.0e-6f, "after reset the output differs by " + juce::String (worst));

            const int latency48 = d.getLatencySamples();
            d.prepare (44100.0, 480);
            expect (d.getLatencySamples() != latency48, "latency must follow the sample rate");

            const auto in44 = firstSeconds (loadSpeechFixture (44100.0), 44100.0, 2.0);
            d.setAmount (0.0f);
            const auto out44 = processInBlocks (in44, 480, [&] (float* b, int n) { d.process (b, n); });

            float worst44 = 0.0f;
            for (size_t i = 0; i + (size_t) d.getLatencySamples() < in44.size(); ++i)
                worst44 = std::max (worst44, std::abs (out44[i + (size_t) d.getLatencySamples()] - in44[i]));

            expect (worst44 < 1.0e-6f, "pure delay at 44.1 kHz after re-prepare: " + juce::String (worst44));
        }

        beginTest ("setAmount clamps to 0..100 and ignores NaN");
        {
            const auto in = firstSeconds (speech48, 48000.0, 1.0);
            const auto dry = denoise (in, 48000.0, 0.0f).out;
            const auto wet = denoise (in, 48000.0, 100.0f).out;

            expect (denoise (in, 48000.0, -50.0f).out == dry, "negative amount must act as 0");
            expect (denoise (in, 48000.0, 250.0f).out == wet, "amount above 100 must act as 100");

            RnnoiseDenoiser d;
            d.prepare (48000.0, 480);
            d.setAmount (std::numeric_limits<float>::quiet_NaN());   // keeps the default, 100 %
            const auto nan = processInBlocks (in, 480, [&] (float* b, int n) { d.process (b, n); });
            expect (nan == wet, "NaN amount must leave the default of 100 % untouched");
        }
    }
};

static RnnoiseDenoiserTests rnnoiseDenoiserTests;

class StreamingResamplerTests final : public juce::UnitTest
{
public:
    StreamingResamplerTests() : juce::UnitTest ("StreamingResampler", "rnnoise") {}

    void runTest() override
    {
        beginTest ("a tone comes out as the same tone, delayed by exactly the requested amount");
        {
            struct Case { double in, out, hz, delay, tolerance; };

            for (const auto c : { Case { 44100.0, 48000.0,  1000.0, 17.0,  1.0e-3 },
                                  Case { 48000.0, 44100.0,  1000.0, 19.4,  1.0e-3 },
                                  Case { 96000.0, 48000.0,  1000.0, 40.0,  1.0e-3 },
                                  Case { 48000.0, 96000.0,  1000.0, 17.5,  1.0e-3 },
                                  Case { 44100.0, 48000.0, 12000.0, 17.0,  1.0e-3 },
                                  Case { 48000.0, 44100.0, 12000.0, 25.25, 1.0e-3 },
                                  Case { 192000.0, 48000.0, 5000.0, 80.0,  1.0e-3 },
                                  Case { 48000.0, 32000.0,  3000.0, 30.0,  1.0e-3 } })
            {
                StreamingResampler r;
                r.prepare (c.in, c.out, c.delay);

                const double w = 2.0 * juce::MathConstants<double>::pi * c.hz;
                double worst = 0.0;
                int produced = 0;

                const int numInput = (int) (0.2 * c.in);
                for (int n = 0; n < numInput; ++n)
                {
                    r.push ((float) (0.8 * std::sin (w * (double) n / c.in)));

                    float y;
                    while (r.pull (y))
                    {
                        // Output m is the tone at input time m * in / out - delay.
                        const double t = (double) produced * c.in / c.out - c.delay;
                        const double expected = 0.8 * std::sin (w * t / c.in);

                        if (produced > 400)   // past the start-up, where the history is silence
                            worst = std::max (worst, std::abs ((double) y - expected));

                        ++produced;
                    }
                }

                report (juce::String (c.in, 0) + " -> " + juce::String (c.out, 0) + " Hz, " + juce::String (c.hz, 0)
                        + " Hz tone, delay " + juce::String (c.delay, 2) + ": worst error " + juce::String (worst, 6));

                expect (produced > (int) (0.19 * c.out), "too few output samples: " + juce::String (produced));
                expect (worst < c.tolerance, juce::String (c.in) + " -> " + juce::String (c.out) + " Hz, "
                                                 + juce::String (c.hz) + " Hz tone: error "
                                                 + juce::String (worst, 6));
            }
        }

        beginTest ("content above the output Nyquist is removed, not aliased");
        {
            StreamingResampler r;
            r.prepare (96000.0, 48000.0, StreamingResampler::getMinimumDelay (96000.0, 48000.0));

            std::vector<float> out;
            const double w = 2.0 * juce::MathConstants<double>::pi * 30000.0;   // folds to 18 kHz if it leaks

            for (int n = 0; n < 96000; ++n)
            {
                r.push ((float) (0.8 * std::sin (w * (double) n / 96000.0)));

                float y;
                while (r.pull (y))
                    out.push_back (y);
            }

            const float level = rmsDb (out.data() + 2000, (int) out.size() - 2000);
            report ("30 kHz tone through 96 -> 48 kHz: " + juce::String (level, 1) + " dBFS RMS");
            expect (level < -80.0f, "alias at " + juce::String (level, 1) + " dBFS");
        }
    }
};

static StreamingResamplerTests streamingResamplerTests;
