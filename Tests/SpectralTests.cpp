// Unit tests for the light noise suppression (SpectralDenoiser): that it is a
// pure delay at 0 %, that its latency is the number it reports and within the
// budget, that it removes stationary noise without eating speech, that it does
// not depend on the block size and that moving the amount cannot click.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe spectral

#include <JuceHeader.h>

#include "TestSignals.h"

#include "../Source/Dsp/SpectralDenoiser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{
    /// The console runner shows only failures, and the measured reductions are
    /// what one wants to see when tuning or recording a result, so they go to
    /// stdout directly.
    void report (const juce::String& message)
    {
        std::printf ("    [measured] %s\n", message.toRawUTF8());
    }

    /// Runs a fresh denoiser over `x` in blocks of `blockSize`.
    std::vector<float> denoise (const std::vector<float>& x, double sr, float amount, int blockSize)
    {
        SpectralDenoiser denoiser;
        denoiser.prepare (sr, std::max (blockSize, 1));
        denoiser.setAmount (amount);

        return TestSignals::processInBlocks (x, blockSize, [&] (float* block, int n)
        {
            denoiser.process (block, n);
        });
    }

    /// Largest |output[i + latency] - input[i]| over everything the output covers.
    float maxDelayError (const std::vector<float>& in, const std::vector<float>& out, int latency)
    {
        float worst = 0.0f;

        for (size_t i = 0; i + (size_t) latency < out.size() && i < in.size(); ++i)
            worst = std::max (worst, std::abs (out[i + (size_t) latency] - in[i]));

        return worst;
    }

    float maxDifference (const std::vector<float>& a, const std::vector<float>& b)
    {
        float worst = 0.0f;

        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            worst = std::max (worst, std::abs (a[i] - b[i]));

        return worst;
    }

    /// RMS in dBFS of x[i] for the indices in [first, last) where `select (i)` is true.
    template <typename Select>
    float rmsDbWhere (const std::vector<float>& x, size_t first, size_t last, Select&& select)
    {
        double sumSquares = 0.0;
        size_t count = 0;

        for (size_t i = first; i < last && i < x.size(); ++i)
        {
            if (! select (i))
                continue;

            sumSquares += (double) x[i] * (double) x[i];
            ++count;
        }

        if (count == 0)
            return TestSignals::silenceDb;

        const double rms = std::sqrt (sumSquares / (double) count);
        return rms > 1.0e-10 ? (float) (20.0 * std::log10 (rms)) : TestSignals::silenceDb;
    }

    /// How far the last second of `out` is below the last second of `in`, in dB.
    float lastSecondReductionDb (const std::vector<float>& in, const std::vector<float>& out, double sr)
    {
        const size_t count = (size_t) sr;
        const size_t first = out.size() - count;

        return TestSignals::rmsDb (in.data() + first, (int) count) - TestSignals::rmsDb (out.data() + first, (int) count);
    }
}

class SpectralDenoiserTests final : public juce::UnitTest
{
public:
    SpectralDenoiserTests() : juce::UnitTest ("SpectralDenoiser", "spectral") {}

    void runTest() override
    {
        beginTest ("zero amount is a pure delay");
        {
            const auto speech = TestSignals::loadSpeechFixture (48000.0);
            expect (! speech.empty(), "speech fixture not found");

            SpectralDenoiser denoiser;
            denoiser.prepare (48000.0, 480);
            const int latency = denoiser.getLatencySamples();
            expect (latency > 0, "a short-time transform cannot be latency free");

            for (const int blockSize : { 480, 1000 })
            {
                const auto out = denoise (speech, 48000.0, 0.0f, blockSize);
                const float error = maxDelayError (speech, out, latency);
                expect (error <= 1.0e-4f, "blocks of " + juce::String (blockSize)
                                              + ": worst deviation from a pure delay is " + juce::String (error));
            }
        }

        beginTest ("latency budget");
        {
            for (const double sr : { 44100.0, 48000.0, 96000.0 })
            {
                SpectralDenoiser denoiser;
                denoiser.prepare (sr, 512);
                const int latency = denoiser.getLatencySamples();

                expect ((double) latency <= 0.006 * sr, "latency " + juce::String (latency)
                                                            + " samples exceeds 6 ms at " + juce::String (sr) + " Hz");

                // The number must also be true, because the voice chain compensates by it.
                const auto noise = TestSignals::whiteNoise (-20.0f, (int) sr, 7);
                const auto out = denoise (noise, sr, 0.0f, 480);
                const float error = maxDelayError (noise, out, latency);

                expect (error <= 1.0e-4f, "at " + juce::String (sr) + " Hz the output is not the input delayed by "
                                              + juce::String (latency) + " samples (worst deviation "
                                              + juce::String (error) + ")");
                expect (TestSignals::crossCorrelationLag (noise, out, 1000) == latency,
                        "at " + juce::String (sr) + " Hz the cross-correlation lag differs from getLatencySamples()");

                report ("latency " + juce::String (sr, 0) + " Hz: " + juce::String (latency) + " samples = "
                        + juce::String (1000.0 * latency / sr, 2) + " ms (budget "
                        + juce::String (6.0, 1) + " ms)");
            }
        }

        beginTest ("stationary noise is reduced");
        {
            // 48 kHz is the case of the brief; the other rates check that the
            // time constants and the FFT size follow the sample rate.
            for (const double sr : { 48000.0, 44100.0, 96000.0 })
            {
                const auto noise = TestSignals::whiteNoise (-40.0f, (int) (4 * sr), 11);

                const float full = lastSecondReductionDb (noise, denoise (noise, sr, 100.0f, 480), sr);
                report ("white noise -40 dBFS, " + juce::String (sr, 0) + " Hz, amount 100: last second "
                        + juce::String (full, 1) + " dB lower");
                expect (full >= 9.0f, "at " + juce::String (sr) + " Hz stationary noise fell only "
                                          + juce::String (full, 1) + " dB");

                // The slider scales the floor: half the amount, roughly half the dB.
                const float half = lastSecondReductionDb (noise, denoise (noise, sr, 50.0f, 480), sr);
                report ("same at amount 50: " + juce::String (half, 1) + " dB lower");
                expect (half >= 6.0f && half < full - 3.0f, "at " + juce::String (sr)
                                                               + " Hz amount 50 gave a reduction of " + juce::String (half, 1) + " dB");
            }
        }

        beginTest ("speech survives");
        {
            constexpr double sr = 48000.0;
            const auto clean = TestSignals::loadSpeechFixture (sr);
            expect (! clean.empty(), "speech fixture not found");

            const auto noise = TestSignals::whiteNoise (-45.0f, (int) clean.size(), 5);
            std::vector<float> noisy (clean.size());
            for (size_t i = 0; i < clean.size(); ++i)
                noisy[i] = clean[i] + noise[i];

            SpectralDenoiser denoiser;
            denoiser.prepare (sr, 480);
            const int latency = denoiser.getLatencySamples();

            const auto out = denoise (noisy, sr, 100.0f, 480);

            // Output sample i + latency answers input sample i.
            std::vector<float> shiftedOut (clean.size(), 0.0f);
            for (size_t i = 0; i + (size_t) latency < out.size() && i < clean.size(); ++i)
                shiftedOut[i] = out[i + (size_t) latency];

            const size_t end = clean.size() - (size_t) latency;
            const size_t afterFirstSecond = (size_t) sr;

            // Speech: the 10 ms windows louder than -35 dBFS, the default mask.
            // Noise only: the windows where the clean signal is under -60 dBFS,
            // that is the pauses (digital silence in the fixture). With the
            // default mask "noise only" would also take in the endings and
            // consonants between -35 and -60 dBFS, which sit 10 to 20 dB above
            // this -45 dBFS noise and are speech; nothing that keeps speech can
            // remove them, and they would swamp the measurement (reported below).
            // What lies between the two masks counts for neither side.
            const auto loudMask    = TestSignals::speechMask (clean, sr);
            const auto audibleMask = TestSignals::speechMask (clean, sr, -60.0f);

            const auto isSpeech      = [&] (size_t i) { return (bool) loudMask[i]; };
            const auto isPause       = [&] (size_t i) { return ! audibleMask[i]; };
            const auto isBelowSpeech = [&] (size_t i) { return ! loudMask[i]; };

            const float speechIn  = rmsDbWhere (noisy,      0, end, isSpeech);
            const float speechOut = rmsDbWhere (shiftedOut, 0, end, isSpeech);
            const float pauseIn   = rmsDbWhere (noisy,      afterFirstSecond, end, isPause);
            const float pauseOut  = rmsDbWhere (shiftedOut, afterFirstSecond, end, isPause);

            report ("speech samples (> -35 dBFS): " + juce::String (speechIn, 2) + " dB in, " + juce::String (speechOut, 2)
                    + " dB out, drop " + juce::String (speechIn - speechOut, 2) + " dB");
            report ("noise-only samples (clean < -60 dBFS, after 1 s): " + juce::String (pauseIn, 2) + " dB in, "
                    + juce::String (pauseOut, 2) + " dB out, drop " + juce::String (pauseIn - pauseOut, 2) + " dB");

            const float quietIn  = rmsDbWhere (noisy,      afterFirstSecond, end, isBelowSpeech);
            const float quietOut = rmsDbWhere (shiftedOut, afterFirstSecond, end, isBelowSpeech);
            report ("for reference, everything under -35 dBFS (endings included): " + juce::String (quietIn, 2)
                    + " dB in, " + juce::String (quietOut, 2) + " dB out, drop " + juce::String (quietIn - quietOut, 2) + " dB");

            expect (speechIn - speechOut <= 3.0f, "speech lost " + juce::String (speechIn - speechOut, 2) + " dB");
            expect (pauseIn - pauseOut >= 8.0f, "noise between phrases fell only " + juce::String (pauseIn - pauseOut, 2) + " dB");
        }

        beginTest ("block-size independence");
        {
            constexpr double sr = 48000.0;
            auto speech = TestSignals::loadSpeechFixture (sr);
            expect (! speech.empty(), "speech fixture not found");
            speech.resize (std::min (speech.size(), (size_t) (5 * sr)));

            const auto noise = TestSignals::whiteNoise (-45.0f, (int) speech.size(), 9);
            for (size_t i = 0; i < speech.size(); ++i)
                speech[i] += noise[i];

            // 60 %, so the floor is in play and not at one of its two extremes.
            const auto reference = denoise (speech, sr, 60.0f, 480);

            for (const int blockSize : { 1, 37, 1024 })
            {
                const float worst = maxDifference (denoise (speech, sr, 60.0f, blockSize), reference);
                expect (worst <= 1.0e-5f, "blocks of " + juce::String (blockSize) + " differ from blocks of 480 by "
                                              + juce::String (worst));
            }
        }

        beginTest ("amount change is click-free");
        {
            constexpr double sr = 48000.0;
            constexpr int block = 480;
            const auto sine = TestSignals::sine (sr, 440.0, 0.5f, (int) (3 * sr));

            const auto unchanged = denoise (sine, sr, 0.0f, block);

            SpectralDenoiser denoiser;
            denoiser.prepare (sr, block);
            denoiser.setAmount (0.0f);

            std::vector<float> changed = sine;

            for (int start = 0; start < (int) changed.size(); start += block)
            {
                if (start == (int) sr)
                    denoiser.setAmount (100.0f);

                denoiser.process (changed.data() + start, std::min (block, (int) changed.size() - start));
            }

            const float stepUnchanged = TestSignals::maxStep (unchanged.data(), (int) unchanged.size());
            const float stepChanged   = TestSignals::maxStep (changed.data(),   (int) changed.size());

            report ("max step: unchanged " + juce::String (stepUnchanged, 5) + ", amount 0 to 100 at 1 s "
                    + juce::String (stepChanged, 5));
            expect (stepChanged <= 1.5f * stepUnchanged, "amount change made a step of " + juce::String (stepChanged, 5)
                                                           + " against " + juce::String (stepUnchanged, 5));

            // A steady tone is stationary, so the estimator treats it as noise
            // and the change really does pull it down. Without that, the check
            // above would pass by never changing anything.
            const size_t tail = changed.size() - (size_t) (0.5 * sr);
            const float levelChanged   = TestSignals::rmsDb (changed.data()   + tail, (int) (0.5 * sr));
            const float levelUnchanged = TestSignals::rmsDb (unchanged.data() + tail, (int) (0.5 * sr));
            report ("last half second: " + juce::String (levelUnchanged, 1) + " dB unchanged, "
                    + juce::String (levelChanged, 1) + " dB after the change");
            expect (levelChanged <= levelUnchanged - 6.0f, "the amount change did not take effect");
        }

        beginTest ("silence, bad input and reset");
        {
            constexpr double sr = 48000.0;

            const std::vector<float> silence ((size_t) sr, 0.0f);
            const auto silent = denoise (silence, sr, 100.0f, 480);
            expect (TestSignals::peakAbs (silent.data(), (int) silent.size()) == 0.0f, "silence in must give silence out");

            // A NaN or an infinity from a broken driver must not stay in the
            // recursive state: the output remains finite and the denoiser recovers.
            auto damaged = TestSignals::whiteNoise (-40.0f, (int) (3 * sr), 4);
            damaged[1000] = std::numeric_limits<float>::quiet_NaN();
            damaged[20000] = std::numeric_limits<float>::infinity();
            damaged[40000] = -std::numeric_limits<float>::infinity();

            const auto recovered = denoise (damaged, sr, 100.0f, 480);
            bool allFinite = true;
            for (const float v : recovered)
                allFinite = allFinite && std::isfinite (v);

            expect (allFinite, "non-finite input produced non-finite output");
            expect (lastSecondReductionDb (damaged, recovered, sr) >= 9.0f,
                    "the denoiser did not recover from non-finite input");

            // reset() must bring back exactly the state of a fresh prepare().
            const auto noise = TestSignals::whiteNoise (-40.0f, (int) sr, 3);

            SpectralDenoiser denoiser;
            denoiser.prepare (sr, 480);
            denoiser.setAmount (80.0f);

            const auto first  = TestSignals::processInBlocks (noise, 480, [&] (float* b, int n) { denoiser.process (b, n); });
            denoiser.reset();
            const auto second = TestSignals::processInBlocks (noise, 480, [&] (float* b, int n) { denoiser.process (b, n); });

            const float worst = maxDifference (first, second);
            expect (worst <= 1.0e-6f, "reset() must restore the start state (deviation " + juce::String (worst) + ")");
        }
    }
};

static SpectralDenoiserTests spectralDenoiserTests;
