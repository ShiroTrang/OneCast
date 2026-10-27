#pragma once
#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <vector>

/// Sample-rate converter with an exact, constant delay, built for the one job
/// RnnoiseDenoiser has: carry a mic signal to 48 kHz and back without the
/// latency report or the dry/wet alignment ever being off by a fraction of a
/// sample.
///
/// juce::LagrangeInterpolator and WindowedSincInterpolator are not used because
/// they consume a data-dependent number of input samples per call, which makes
/// streaming in arbitrary block sizes (and knowing the delay) awkward. Here the
/// caller pushes one input sample at a time and pulls output samples while they
/// are available, so the result depends only on the sample sequence, never on
/// how it was cut into blocks.
///
/// Output sample m is the band-limited input signal evaluated at
/// `m * inputRate / outputRate - delay` (in input samples), i.e. a pure delay
/// of `delay` input samples. The delay may be fractional but never less than
/// getMinimumDelay(), which is the kernel's look-ahead (the converter has to
/// wait for the input samples the kernel reaches).
///
/// Kernel: Kaiser-windowed sinc, 16 zero crossings per side, cut off at 95 % of
/// the lower Nyquist so downsampling does not alias and upsampling does not
/// image. Sub-sample positions come from a 256-phase table with linear
/// interpolation between phases (error far below -90 dB) and every phase is
/// normalised to unity DC gain.
///
/// prepare() allocates; push(), pull() and reset() are real-time safe.
class StreamingResampler
{
public:
    static constexpr int    kernelZeroCrossings = 16;    // each side of the centre
    static constexpr double passbandFraction    = 0.95;  // of the lower Nyquist
    static constexpr int    numPhases           = 256;
    static constexpr double kaiserBeta          = 9.0;

    /// Smallest legal delay, in input samples: how far the kernel reaches ahead.
    static int getMinimumDelay (double inputRate, double outputRate) noexcept
    {
        return getHalfWidth (getCutoff (inputRate, outputRate));
    }

    /// @param delayInputSamples  Constant delay, in input samples. Clamped up to
    ///                           getMinimumDelay().
    void prepare (double inputRate, double outputRate, double delayInputSamples)
    {
        jassert (inputRate > 0.0 && outputRate > 0.0);

        step = inputRate / outputRate;

        const double cutoff = getCutoff (inputRate, outputRate);
        halfWidth = getHalfWidth (cutoff);
        taps      = 2 * halfWidth;
        delay     = std::max (delayInputSamples, (double) halfWidth);

        // The ring only needs to hold the kernel plus however far ahead of the
        // output the input may run (a few samples; one burst at most).
        ringSize = 1;
        while (ringSize < 2 * taps + 64)
            ringSize <<= 1;

        ring.assign ((size_t) ringSize * 2, 0.0f);   // mirrored, see push()
        table.assign ((size_t) ((numPhases + 1) * taps), 0.0f);

        const double i0Beta = besselI0 (kaiserBeta);

        for (int p = 0; p <= numPhases; ++p)
        {
            double sum = 0.0;
            std::vector<double> row ((size_t) taps);

            for (int k = 0; k < taps; ++k)
            {
                // Distance from the output position to input sample `k` of the
                // window, in input samples; the window starts at floor (position)
                // - (halfWidth - 1).
                const double u = (double) (halfWidth - 1) + (double) p / numPhases - (double) k;
                const double r = u / (double) halfWidth;
                const double window = std::abs (r) < 1.0
                                          ? besselI0 (kaiserBeta * std::sqrt (1.0 - r * r)) / i0Beta
                                          : 0.0;

                row[(size_t) k] = cutoff * sinc (cutoff * u) * window;
                sum += row[(size_t) k];
            }

            // Unity gain at DC for every sub-sample position.
            for (int k = 0; k < taps; ++k)
                table[(size_t) (p * taps + k)] = (float) (row[(size_t) k] / sum);
        }

        reset();
    }

    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        pushed = 0;
        produced = 0;
    }

    /// Appends the next input sample.
    void push (float x) noexcept
    {
        const int slot = (int) (pushed & (ringSize - 1));
        ring[(size_t) slot] = x;
        ring[(size_t) (slot + ringSize)] = x;   // mirror, so a window never wraps
        ++pushed;
    }

    /// Produces the next output sample if every input sample it needs has been
    /// pushed. Returns false (and leaves `out` alone) otherwise.
    bool pull (float& out) noexcept
    {
        const double position = (double) produced * step - delay;
        const double whole = std::floor (position);
        const juce::int64 base = (juce::int64) whole;

        if (base + halfWidth >= pushed)
            return false;

        const double phasePosition = (position - whole) * numPhases;
        const int phase = std::min ((int) phasePosition, numPhases - 1);
        const float weight = (float) (phasePosition - (double) phase);

        const int start = (int) ((base - (halfWidth - 1)) & (ringSize - 1));
        const float* x  = ring.data() + start;
        const float* t0 = table.data() + (size_t) phase * (size_t) taps;
        const float* t1 = t0 + taps;

        float a = 0.0f, b = 0.0f;
        for (int k = 0; k < taps; ++k)
        {
            a += x[k] * t0[k];
            b += x[k] * t1[k];
        }

        out = a + weight * (b - a);
        ++produced;
        return true;
    }

private:
    static double getCutoff (double inputRate, double outputRate) noexcept
    {
        return passbandFraction * std::min (1.0, outputRate / inputRate);
    }

    static int getHalfWidth (double cutoff) noexcept
    {
        return (int) std::ceil ((double) kernelZeroCrossings / cutoff);
    }

    static double sinc (double x) noexcept
    {
        if (std::abs (x) < 1.0e-12)
            return 1.0;

        const double px = juce::MathConstants<double>::pi * x;
        return std::sin (px) / px;
    }

    /// Modified Bessel function of the first kind, order 0 (power series).
    static double besselI0 (double x) noexcept
    {
        double sum = 1.0, term = 1.0;
        const double q = x * x / 4.0;

        for (int k = 1; k < 60 && term > 1.0e-18 * sum; ++k)
        {
            term *= q / ((double) k * (double) k);
            sum += term;
        }

        return sum;
    }

    double step = 1.0;
    double delay = 0.0;
    int halfWidth = 1;
    int taps = 2;
    int ringSize = 128;

    std::vector<float> ring;    // two copies of the history, back to back
    std::vector<float> table;   // (numPhases + 1) rows of `taps` weights

    juce::int64 pushed = 0;     // input samples received
    juce::int64 produced = 0;   // output samples delivered
};
