#pragma once
#include <JuceHeader.h>

#include "DspParams.h"

#include <atomic>
#include <memory>
#include <vector>

/// Light noise suppression ("Nhe" in the UI): the low-latency alternative to RNNoise.
///
/// A streaming short-time Fourier transform with a spectral gain per bin.
/// FFT 256 at sample rates up to 50 kHz and 512 above, hop = FFT / 4, so the
/// delay is FFT - 1 samples (5.3 ms at 48 kHz) against 10 ms and a fixed
/// 48 kHz for RNNoise. Analysis and synthesis windows are both sqrt-Hann and
/// together sum to exactly one at a 75 % overlap, so a gain of 1 in every bin
/// is a perfect pure delay.
///
/// The gain comes from a decision-directed Wiener rule driven by a noise
/// estimate that follows the quietest level each bin reaches over the last
/// second and a half (minimum statistics), so it needs no voice detector and
/// no calibration step. It is smoothed over frequency and time against musical
/// noise, and floored at -maxAttenuationDb * amount / 100: that floor is what
/// the amount slider moves, and at 0 % it is exactly 1.
///
/// Every sample goes through the same per-sample path (input FIFO, frame every
/// hop, output FIFO), so the output does not depend on how the host chops the
/// stream into blocks. prepare() allocates; process() does not, takes no lock
/// of its own and belongs to the audio thread (juce::dsp::FFT guards its
/// scratch with an uncontended SpinLock, which only this object ever takes).
/// setAmount() may be called from any thread.
class SpectralDenoiser
{
public:
    /// Spectral floor at amount 100 %.
    static constexpr float maxAttenuationDb = 18.0f;

    /// Allocates everything for `sampleRate`. process() takes any block size, so
    /// `maxBlockSize` is only part of the prepare contract shared by the chain's
    /// stages.
    void prepare (double sampleRate, int maxBlockSize);

    /// Forgets the signal and the noise estimate (the amount is kept). The next
    /// process() call takes the amount as it is, without a ramp from an old one.
    /// Not to be called while process() runs.
    void reset() noexcept;

    /// Noise reduction in percent, 0 = delayed pass-through. A change ramps with
    /// a 20 ms time constant, linear in dB, so moving the slider cannot click.
    void setAmount (float percent) noexcept;

    /// In place, any block size. The output is always finite, and silent for silent input.
    void process (float* samples, int numSamples) noexcept;

    /// Delay of process() in samples. Constant after prepare().
    int getLatencySamples() const noexcept { return latencySamples; }

private:
    void processFrame() noexcept;
    void updateNoiseEstimate (bool learn) noexcept;
    void applyGain (float* spectrum, float floorGain) noexcept;

    int fftSize = 0;
    int hop = 0;
    int numBins = 0;
    int latencySamples = 0;
    unsigned ringMask = 0;

    std::unique_ptr<juce::dsp::FFT> fft;

    // Both rings share one position: `pos` is the slot of the oldest input
    // sample, which is also the slot of the next output sample to leave.
    std::vector<float> inputRing;
    std::vector<float> outputRing;
    unsigned pos = 0;
    int hopCounter = 0;
    int warmupFrames = 0;      // frames that still contain pre-roll silence

    std::vector<float> analysisWindow;
    std::vector<float> synthesisWindow;   // includes the 1 / 2 overlap-add gain
    std::vector<float> fftData;           // 2 * fftSize, as juce::dsp::FFT wants

    // Per-bin state.
    std::vector<float> power;             // |Y|^2 of the current frame
    std::vector<float> smoothedPower;     // 3-bin average of power, smoothed in time (what the tracker watches)
    std::vector<float> noisePower;        // the estimate lambda
    std::vector<float> cleanPower;        // |G Y|^2 of the previous frame (decision-directed memory)
    std::vector<float> wienerGain;        // the rule's gain for the current frame, before smoothing
    std::vector<float> appliedGain;       // smoothed over frequency and time: what the frame is multiplied by

    // Minimum statistics: the running minimum of the current sub-window, the
    // minima of the last few finished sub-windows, and the minimum of those.
    std::vector<float> subWindowMin;      // numSubWindows * numBins
    std::vector<float> currentMin;
    std::vector<float> finishedMin;       // minimum over the finished sub-windows
    int subWindowLength = 0;
    int subWindowCounter = 0;
    int subWindowIndex = 0;
    bool trackerPrimed = false;

    // Smoothing constants, per frame (they depend on the sample rate).
    float powerSmoothing = 0.0f;
    float decisionDirected = 0.0f;
    float gainRise = 0.0f;
    float gainFall = 0.0f;

    // Amount: the target is set from any thread, the smoothed value is audio-thread only.
    std::atomic<float> amountTarget { ParamRanges::noiseAmount.defaultValue };
    float amountSmoothed = ParamRanges::noiseAmount.defaultValue;
    float amountCoeff = 0.0f;
    bool snapAmount = true;
};
