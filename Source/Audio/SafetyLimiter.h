#pragma once
#include <JuceHeader.h>

#include <atomic>
#include <cmath>
#include <vector>

/// Zero-latency protection for the headphone monitor, which may not be
/// delayed even by a look-ahead: transparent up to -1 dBFS, then a tanh knee
/// that never exceeds 0 dBFS. Continuous, with slope 1 at the knee, so it only
/// colours peaks that would otherwise have clipped hard. Non-finite samples
/// become silence.
struct MonitorSoftClip
{
    static constexpr float knee = 0.8912509f;   // -1 dBFS

    static float process (float x) noexcept
    {
        if (! std::isfinite (x))
            return 0.0f;

        const float a = std::abs (x);

        if (a <= knee)
            return x;

        const float over = (a - knee) / (1.0f - knee);
        return std::copysign (knee + (1.0f - knee) * std::tanh (over), x);
    }
};

/// Look-ahead brick-wall peak limiter for the master bus.
///
/// Desktop + voice + reverb are summed raw, so a loud chorus over a loud
/// backing track goes past 0 dBFS and the virtual cable clips it hard. This
/// limiter guarantees no sample leaves above the ceiling, and is transparent
/// while the mix stays below it.
///
/// Method (no overshoot by construction): the gain each sample needs is held
/// for `lookahead` samples and then box-averaged over the same length, while
/// the audio is delayed by `lookahead - 1`. Every averaged value that reaches
/// a peak includes that peak's own requirement, so the gain has fully ramped
/// down by the time the peak comes out. Release is a one-pole recovery. A final
/// clamp catches rounding, and non-finite samples from a misbehaving plug-in
/// are replaced with silence so they cannot poison the gain computation.
///
/// prepare() allocates; process() is real-time safe and belongs to the audio
/// thread. getGainReductionDb() may be read from any thread.
class SafetyLimiter
{
public:
    void prepare (int numChannels, double sampleRate, float ceilingDb = -1.0f,
                  double lookaheadSeconds = 0.001, double releaseSeconds = 0.15)
    {
        jassert (numChannels > 0 && sampleRate > 0.0);

        channels  = numChannels;
        lookahead = juce::jmax (1, static_cast<int> (std::round (lookaheadSeconds * sampleRate)));
        ceiling   = juce::Decibels::decibelsToGain (ceilingDb);
        releaseCoeff = static_cast<float> (std::exp (-1.0 / (releaseSeconds * sampleRate)));

        holdBuf.assign ((size_t) lookahead, 1.0f);
        boxBuf.assign  ((size_t) lookahead, 1.0f);
        delayBuf.assign ((size_t) channels, std::vector<float> ((size_t) lookahead, 0.0f));

        reset();
    }

    void reset() noexcept
    {
        std::fill (holdBuf.begin(), holdBuf.end(), 1.0f);
        std::fill (boxBuf.begin(), boxBuf.end(), 1.0f);

        for (auto& d : delayBuf)
            std::fill (d.begin(), d.end(), 0.0f);

        boxSum = static_cast<double> (lookahead);
        pos = 0;
        gain = 1.0f;
        reductionDb.store (0.0f, std::memory_order_relaxed);
    }

    /// Latency added to everything passing through, in samples.
    int getLatencySamples() const noexcept { return lookahead - 1; }

    float getCeilingGain() const noexcept  { return ceiling; }

    /// Deepest gain reduction during the last processed block (<= 0 dB).
    float getGainReductionDb() const noexcept { return reductionDb.load (std::memory_order_relaxed); }

    void process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        const int numCh = juce::jmin (channels, buffer.getNumChannels());

        if (numCh == 0 || holdBuf.empty())
            return;

        auto* const* data = buffer.getArrayOfWritePointers();
        float minGain = 1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float peak = 0.0f;

            for (int ch = 0; ch < numCh; ++ch)
            {
                float x = data[ch][i];

                if (! std::isfinite (x))
                    x = data[ch][i] = 0.0f;

                peak = juce::jmax (peak, std::abs (x));
            }

            const float required = peak > ceiling ? ceiling / peak : 1.0f;

            // Hold: minimum requirement over the last `lookahead` samples.
            holdBuf[(size_t) pos] = required;

            float held = 1.0f;
            for (float r : holdBuf)
                held = juce::jmin (held, r);

            // Box average of the held curve over the same length.
            boxSum += static_cast<double> (held) - static_cast<double> (boxBuf[(size_t) pos]);
            boxBuf[(size_t) pos] = held;
            const float smoothed = static_cast<float> (boxSum / lookahead);

            // Attack is already shaped by the box filter; release slowly.
            gain = smoothed < gain ? smoothed
                                   : smoothed + (gain - smoothed) * releaseCoeff;

            minGain = juce::jmin (minGain, gain);

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto& d = delayBuf[(size_t) ch];

                // Write, then read the oldest slot: a delay of lookahead - 1,
                // which lines each sample up with the end of its gain ramp.
                d[(size_t) pos] = data[ch][i];
                const float delayed = d[(size_t) ((pos + 1) % lookahead)];

                data[ch][i] = juce::jlimit (-ceiling, ceiling, delayed * gain);
            }

            pos = (pos + 1) % lookahead;
        }

        reductionDb.store (juce::Decibels::gainToDecibels (minGain, -60.0f),
                           std::memory_order_relaxed);
    }

private:
    int channels = 0;
    int lookahead = 1;
    float ceiling = 1.0f;
    float releaseCoeff = 0.0f;

    std::vector<float> holdBuf;
    std::vector<float> boxBuf;
    std::vector<std::vector<float>> delayBuf;

    double boxSum = 0.0;
    int pos = 0;
    float gain = 1.0f;

    std::atomic<float> reductionDb { 0.0f };
};
