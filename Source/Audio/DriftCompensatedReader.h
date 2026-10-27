#pragma once
#include <JuceHeader.h>
#include "RingBuffer.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

/// Consumer side of a RingBuffer whose producer runs on a different clock.
///
/// Two audio devices never run at exactly the same rate. The WASAPI loopback
/// capture follows the clock of the Windows output device while the engine
/// follows the ASIO / mic interface clock; the virtual-cable render thread is
/// the same problem in reverse. Read naively, the ring either drains slowly
/// (a dropout every few minutes) or fills slowly (latency that grows until the
/// ring overflows). On top of that the producer may not be able to run at the
/// consumer's rate at all.
///
/// This reader resamples by `nominalRatio * (1 + correction)`, where the
/// correction (at most +/-0.2 %, about 3 cents - inaudible) is a slow control
/// loop that holds the ring's backlog at a target level:
///
/// - It starts "priming": silence until the backlog reaches the target, so the
///   first blocks do not underrun by construction.
/// - On an underrun it goes back to priming instead of clicking every block.
/// - On a large overshoot (a producer that wrote while nobody was reading) it
///   discards the excess at once instead of slewing for minutes.
///
/// prepare() allocates; everything else is real-time safe. read() and reset()
/// belong to the consumer thread; the getters may be called from any thread.
class DriftCompensatedReader
{
public:
    /// Largest correction the control loop may apply, as a fraction of the
    /// nominal ratio. Real clock drift is typically under 200 ppm.
    static constexpr double maxCorrection = 0.002;

    /// Nominal ratios outside this range are clamped (it bounds scratch size).
    static constexpr double minNominalRatio = 0.25;
    static constexpr double maxNominalRatio = 4.0;

    /// @param numChannels        Channels to resample (normally the ring's).
    /// @param maxFramesPerRead   Largest @p numFrames read() will be asked for.
    /// @param consumerRate       Sample rate of the consumer, in Hz.
    /// @param targetFillFrames   Backlog to hold in the ring, in producer frames.
    void prepare (int numChannels, int maxFramesPerRead, double consumerRate,
                  int targetFillFrames)
    {
        jassert (numChannels > 0 && maxFramesPerRead > 0 && consumerRate > 0.0);

        channels   = numChannels;
        maxFrames  = maxFramesPerRead;
        sampleRate = consumerRate;
        target.store (juce::jmax (1, targetFillFrames), std::memory_order_relaxed);

        scratchSize = static_cast<int> (std::ceil (maxFramesPerRead * maxNominalRatio
                                                   * (1.0 + maxCorrection))) + 8;
        scratch.assign (static_cast<size_t> (channels * scratchSize), 0.0f);
        scratchPtrs.resize (static_cast<size_t> (channels));

        for (int ch = 0; ch < channels; ++ch)
            scratchPtrs[(size_t) ch] = scratch.data() + (size_t) (ch * scratchSize);

        spareOut.assign (static_cast<size_t> (maxFramesPerRead), 0.0f);

        interpolators.clear();

        for (int ch = 0; ch < channels; ++ch)
            interpolators.push_back (std::make_unique<juce::LagrangeInterpolator>());

        reset();
    }

    /// Return to the priming state. Consumer thread only.
    void reset() noexcept
    {
        priming = true;
        fillEstimate = 0.0;

        for (auto& interp : interpolators)
            interp->reset();

        currentRatio.store (1.0, std::memory_order_relaxed);
    }

    /// Produce exactly @p numFrames frames in @p dest. Frames the ring cannot
    /// supply are silence. Channels of @p dest beyond the prepared channel
    /// count are cleared.
    ///
    /// @param nominalRatio  producer rate / consumer rate (1.0 when equal).
    /// @return Number of frames that carry real audio (0 or numFrames).
    int read (RingBuffer& ring, float* const* dest, int numDestChannels,
              int numFrames, double nominalRatio) noexcept
    {
        jassert (numFrames <= maxFrames);
        numFrames = juce::jmin (numFrames, maxFrames);

        if (numFrames <= 0 || channels == 0)
            return 0;

        const int targetFill = target.load (std::memory_order_relaxed);
        int available = ring.getAvailableForRead();

        // A large overshoot cannot be slewed away at 0.2 %: drop it now.
        const int resyncThreshold = targetFill + juce::jmax (static_cast<int> (0.1 * sampleRate),
                                                             4 * maxFrames);
        if (available > resyncThreshold)
        {
            ring.discard (available - targetFill);
            available = targetFill;
            fillEstimate = targetFill;
            resyncs.fetch_add (1, std::memory_order_relaxed);
        }

        if (priming)
        {
            if (available < targetFill)
                return silence (dest, numDestChannels, numFrames);

            priming = false;
            fillEstimate = available;

            for (auto& interp : interpolators)
                interp->reset();
        }

        // Smooth the backlog: it is a sawtooth driven by the producer's packet
        // size, and only its average is meaningful.
        const double smoothing = 1.0 - std::exp (-numFrames / (fillSmoothingSeconds * sampleRate));
        fillEstimate += (available - fillEstimate) * smoothing;

        const double errorSeconds = (fillEstimate - targetFill) / sampleRate;
        const double correction = juce::jlimit (-maxCorrection, maxCorrection,
                                                errorSeconds / correctionSeconds);

        const double nominal = juce::jlimit (minNominalRatio, maxNominalRatio, nominalRatio);
        const double ratio = nominal * (1.0 + correction);
        currentRatio.store (ratio, std::memory_order_relaxed);

        // Upper bound of what the interpolator can consume for numFrames outputs.
        const int needed = juce::jmin (scratchSize,
                                       static_cast<int> (std::ceil (numFrames * ratio)) + 2);

        if (available < needed)
        {
            priming = true;
            underruns.fetch_add (1, std::memory_order_relaxed);
            return silence (dest, numDestChannels, numFrames);
        }

        ring.peek (scratchPtrs.data(), needed, channels);

        int used = 0;
        const int channelsToWrite = juce::jmin (channels, numDestChannels);

        // Every channel's interpolator must advance identically, so channels
        // the destination does not want still run, into a throwaway buffer.
        for (int ch = 0; ch < channels; ++ch)
        {
            float* out = ch < channelsToWrite ? dest[ch] : spareOut.data();
            used = interpolators[(size_t) ch]->process (ratio, scratchPtrs[(size_t) ch], out,
                                                         numFrames, needed, 0);
        }

        for (int ch = channelsToWrite; ch < numDestChannels; ++ch)
            juce::FloatVectorOperations::clear (dest[ch], numFrames);

        ring.discard (used);
        return numFrames;
    }

    int    getTargetFill() const noexcept    { return target.load (std::memory_order_relaxed); }
    void   setTargetFill (int frames) noexcept { target.store (juce::jmax (1, frames), std::memory_order_relaxed); }
    double getCurrentRatio() const noexcept  { return currentRatio.load (std::memory_order_relaxed); }
    int    getUnderrunCount() const noexcept { return underruns.load (std::memory_order_relaxed); }
    int    getResyncCount() const noexcept   { return resyncs.load (std::memory_order_relaxed); }

    /// Consumer thread only.
    bool isPriming() const noexcept          { return priming; }

private:
    int silence (float* const* dest, int numDestChannels, int numFrames) noexcept
    {
        for (int ch = 0; ch < numDestChannels; ++ch)
            if (dest[ch] != nullptr)
                juce::FloatVectorOperations::clear (dest[ch], numFrames);

        return 0;
    }

    /// The loop is critically damped when correctionSeconds = 4 * fillSmoothingSeconds.
    static constexpr double fillSmoothingSeconds = 0.5;
    static constexpr double correctionSeconds    = 2.0;

    int channels = 0;
    int maxFrames = 0;
    int scratchSize = 0;
    double sampleRate = 48000.0;

    std::vector<float>  scratch;
    std::vector<float*> scratchPtrs;
    std::vector<float>  spareOut;

    // Held by pointer: the interpolators are neither copyable nor movable.
    std::vector<std::unique_ptr<juce::LagrangeInterpolator>> interpolators;

    bool   priming = true;
    double fillEstimate = 0.0;

    std::atomic<int>    target { 1 };
    std::atomic<double> currentRatio { 1.0 };
    std::atomic<int>    underruns { 0 };
    std::atomic<int>    resyncs { 0 };
};
