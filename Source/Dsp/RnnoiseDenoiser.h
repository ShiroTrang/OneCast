#pragma once
#include <JuceHeader.h>

#include "StreamingResampler.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

struct DenoiseState;   // RNNoise's opaque state (rnnoise.h)

/// AI noise suppression: Xiph's RNNoise 0.2 wrapped so it behaves like any
/// other stage of the voice chain - mono, in place, any block size, any sample
/// rate, a fixed reported latency and a dry/wet "amount".
///
/// RNNoise only runs at 48 kHz on 480-sample frames, and wants samples on a
/// 16-bit scale, so this class
/// - collects the stream into 480-sample frames (sample by sample, so the result
///   never depends on how the host cut the audio into blocks),
/// - scales by 32768 into RNNoise and back,
/// - resamples to 48 kHz and back (StreamingResampler, constant delay) when the
///   device runs at another rate.
///
/// Latency. RNNoise's overlap-add plus its one-frame gain look-ahead put its
/// output two frames behind its input, and a frame cannot be returned before
/// all of it has been received, which costs one more: 3 x 480 = 1440 samples
/// (30 ms) at 48 kHz. Resampling adds the two converters' delays, rounded up so
/// that the total is a whole number of samples at the device rate.
/// getLatencySamples() reports that total.
///
/// Output = amount * wet + (1 - amount) * dry, where `dry` is the input delayed
/// by exactly getLatencySamples() and `wet` is RNNoise's output, aligned to the
/// same delay. Changing the amount ramps linearly over 20 ms, one step per
/// sample.
///
/// prepare() allocates; reset(), process() and the getters are real-time safe.
/// process(), reset() and prepare() belong to one thread (the audio thread);
/// setAmount(), getLatencySamples() and getSpeechProbability() may be called from
/// any thread.
class RnnoiseDenoiser
{
public:
    RnnoiseDenoiser();
    ~RnnoiseDenoiser();

    RnnoiseDenoiser (const RnnoiseDenoiser&) = delete;
    RnnoiseDenoiser& operator= (const RnnoiseDenoiser&) = delete;

    /// RNNoise's fixed operating point.
    static constexpr int rnnoiseSampleRate = 48000;
    static constexpr int frameSize = 480;

    /// Delay from input to output of the 48 kHz path, in samples: two frames of
    /// RNNoise's own lag plus one frame of FIFO. Measured in RnnoiseTests.
    static constexpr int latencyAt48k = 3 * frameSize;

    static constexpr double amountRampSeconds = 0.02;

    /// Allocates. A sample rate within 0.5 Hz of 48 kHz runs without resampling.
    void prepare (double sampleRate, int maxBlockSize);

    /// Forgets all audio history and RNNoise's internal state. The amount snaps
    /// to its current target instead of ramping.
    void reset() noexcept;

    /// Wet share in percent, 0..100 (clamped; NaN is ignored). Takes effect at the
    /// next process() call and ramps over amountRampSeconds, except right after
    /// prepare() or reset(), where it applies at once.
    void setAmount (float percent) noexcept;

    /// Mono, in place. Non-finite input samples are treated as 0.
    void process (float* samples, int numSamples) noexcept;

    /// Samples between input and output at the prepared rate: 1440 at 48 kHz,
    /// more with resampling. Constant after prepare().
    int getLatencySamples() const noexcept { return latencySamples.load (std::memory_order_relaxed); }

    /// RNNoise's voice-activity estimate (0..1) for the most recent frame.
    float getSpeechProbability() const noexcept { return speechProbability.load (std::memory_order_relaxed); }

private:
    struct StateDeleter { void operator() (DenoiseState*) const noexcept; };

    /// One sample through the 48 kHz stage: in and out are on a +-1.0 scale. The
    /// output is the input of two frames ago, denoised (zeros while that history
    /// does not exist yet).
    float processAt48k (float x) noexcept;

    void resetRnnoiseState() noexcept;

    std::unique_ptr<DenoiseState, StateDeleter> state;

    double sampleRate = 48000.0;
    bool resampling = false;
    std::atomic<int> latencySamples { latencyAt48k };

    std::array<float, frameSize> collecting {};   // input frame being filled (16-bit scale)
    std::array<float, frameSize> denoised {};     // last finished output frame (16-bit scale)
    int framePosition = 0;

    StreamingResampler toRnnoiseRate, fromRnnoiseRate;

    std::vector<float> dryLine;   // the input delayed by getLatencySamples()
    int dryPosition = 0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> amount;
    std::atomic<float> amountTarget { 1.0f };     // 0..1
    bool snapAmount = true;

    std::atomic<float> speechProbability { 0.0f };

    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<int>::is_always_lock_free);
};
