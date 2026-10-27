#pragma once

// Shared test signals and measurements. Header-only so every test file can use it.
//
// loadSpeechFixture() finds Tests/data/speech48k.wav through the
// ONECAST_TEST_DATA_DIR compile definition (an absolute path set by CMake), so
// the tests run from any build directory.

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#ifndef ONECAST_TEST_DATA_DIR
 #error "ONECAST_TEST_DATA_DIR must point at the Tests/data folder (set by CMakeLists.txt)"
#endif

namespace TestSignals
{
    /// Level reported for silence and empty input, instead of -infinity.
    inline constexpr float silenceDb = -200.0f;

    /// amplitude * sin (2 pi hz n / sr + phase), phase in radians.
    inline std::vector<float> sine (double sr, double hz, float amplitude, int numSamples,
                                    double phase = 0)
    {
        std::vector<float> x ((size_t) std::max (numSamples, 0));
        const double w = 2.0 * juce::MathConstants<double>::pi * hz / sr;

        for (size_t i = 0; i < x.size(); ++i)
            x[i] = amplitude * (float) std::sin (w * (double) i + phase);

        return x;
    }

    /// Gaussian white noise scaled to exactly `rmsDb` dBFS RMS. The same seed
    /// always gives the same noise, so a failing test can be reproduced.
    inline std::vector<float> whiteNoise (float rmsDb, int numSamples, juce::int64 seed)
    {
        std::vector<float> x ((size_t) std::max (numSamples, 0));
        juce::Random random (seed);

        // Box-Muller: two uniform draws make one Gaussian sample.
        for (auto& v : x)
        {
            const double u1 = 1.0 - random.nextDouble();   // (0, 1], never log (0)
            const double u2 = random.nextDouble();
            v = (float) (std::sqrt (-2.0 * std::log (u1))
                         * std::cos (2.0 * juce::MathConstants<double>::pi * u2));
        }

        double sumSquares = 0.0;
        for (const float v : x)
            sumSquares += (double) v * (double) v;

        if (sumSquares > 0.0)
        {
            const double measured = std::sqrt (sumSquares / (double) x.size());
            const double wanted   = std::pow (10.0, (double) rmsDb / 20.0);

            for (auto& v : x)
                v = (float) ((double) v * wanted / measured);
        }

        return x;
    }

    /// RMS level in dBFS, or silenceDb for silence and empty input.
    inline float rmsDb (const float* x, int n)
    {
        if (n <= 0)
            return silenceDb;

        double sumSquares = 0.0;
        for (int i = 0; i < n; ++i)
            sumSquares += (double) x[i] * (double) x[i];

        const double rms = std::sqrt (sumSquares / (double) n);
        return rms > 1.0e-10 ? (float) (20.0 * std::log10 (rms)) : silenceDb;
    }

    inline float peakAbs (const float* x, int n)
    {
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
            peak = std::max (peak, std::abs (x[i]));

        return peak;
    }

    /// Largest jump between neighbouring samples, max |x[i] - x[i-1]|. A click
    /// or a gain step shows up here long before it shows up in the RMS.
    inline float maxStep (const float* x, int n)
    {
        float step = 0.0f;
        for (int i = 1; i < n; ++i)
            step = std::max (step, std::abs (x[i] - x[i - 1]));

        return step;
    }

    /// How many samples `delayed` trails `reference`: the lag in [0, maxLag]
    /// that maximises sum (reference[i] * delayed[i + lag]). For
    /// delayed[n] == reference[n - 123] it returns 123. Needs a reference that
    /// is not periodic within maxLag (speech and noise are fine, a sine is not).
    inline int crossCorrelationLag (const std::vector<float>& reference,
                                    const std::vector<float>& delayed, int maxLag)
    {
        const int n = (int) std::min (reference.size(), delayed.size());
        int bestLag = 0;
        double best = -1.0e300;

        for (int lag = 0; lag <= std::min (maxLag, n - 1); ++lag)
        {
            double sum = 0.0;
            for (int i = 0; i + lag < n; ++i)
                sum += (double) reference[(size_t) i] * (double) delayed[(size_t) (i + lag)];

            if (sum > best)
            {
                best = sum;
                bestLag = lag;
            }
        }

        return bestLag;
    }

    /// Tests/data/speech48k.wav as mono floats, resampled to `targetRate` if it
    /// differs from the file's 48 kHz. Empty if the file cannot be read, which
    /// the calling test will notice.
    inline std::vector<float> loadSpeechFixture (double targetRate = 48000)
    {
        const juce::File file = juce::File (juce::String (ONECAST_TEST_DATA_DIR))
                                    .getChildFile ("speech48k.wav");

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            return {};

        const int length = (int) reader->lengthInSamples;
        juce::AudioBuffer<float> buffer ((int) reader->numChannels, length);
        reader->read (&buffer, 0, length, 0, true, true);

        std::vector<float> mono (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);

        if (std::abs (targetRate - reader->sampleRate) < 0.5)
            return mono;

        // The interpolator looks a few samples ahead of the last output sample.
        mono.resize ((size_t) length + 8, 0.0f);

        const double ratio = reader->sampleRate / targetRate;
        std::vector<float> resampled ((size_t) ((double) length / ratio));

        juce::LagrangeInterpolator interpolator;
        interpolator.process (ratio, mono.data(), resampled.data(), (int) resampled.size());
        return resampled;
    }

    /// One flag per sample of `clean`: true where the 10 ms window containing
    /// the sample is louder than `thresholdDb` RMS. Pass the noise-free signal;
    /// the mask tells a test where speech is and where only noise should remain.
    inline std::vector<bool> speechMask (const std::vector<float>& clean, double sr,
                                         float thresholdDb = -35.0f)
    {
        const size_t window = (size_t) std::max (1, juce::roundToInt (sr * 0.010));
        std::vector<bool> mask (clean.size(), false);

        for (size_t start = 0; start < clean.size(); start += window)
        {
            const size_t count = std::min (window, clean.size() - start);
            const bool speech = rmsDb (clean.data() + start, (int) count) > thresholdDb;

            std::fill (mask.begin() + (std::ptrdiff_t) start,
                       mask.begin() + (std::ptrdiff_t) (start + count), speech);
        }

        return mask;
    }

    /// Runs `process` over `x` in consecutive blocks of `blockSize` samples (the
    /// last block may be shorter) and returns the result. `process` is called as
    /// process (float* block, int numSamples) and edits the block in place, so a
    /// stateful processor sees the same block boundaries as in the audio callback.
    template <typename Fn>
    std::vector<float> processInBlocks (std::vector<float> x, int blockSize, Fn&& process)
    {
        jassert (blockSize > 0);
        blockSize = std::max (blockSize, 1);

        const int total = (int) x.size();

        for (int start = 0; start < total; start += blockSize)
            process (x.data() + start, std::min (blockSize, total - start));

        return x;
    }
}
