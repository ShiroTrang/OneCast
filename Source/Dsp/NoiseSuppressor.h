#pragma once
#include <JuceHeader.h>

#include "DspParams.h"
#include "RnnoiseDenoiser.h"
#include "SpectralDenoiser.h"

#include <vector>

/// Switches the noise stage between off, SpectralDenoiser (light) and RNNoise
/// (ai) without a click.
///
/// A denoiser that is not heard is normally not run. When the mode changes, the
/// incoming denoiser is run for its own getLatencySamples() samples first and
/// that priming output is thrown away; then the old and new outputs are
/// crossfaded over crossfadeSeconds. The first setMode() after prepare() or
/// reset() snaps instead of switching, because there is no earlier mode to
/// crossfade from. reset() is never used for a mode switch, so a denoiser that
/// is revisited keeps its learned noise estimate.
class NoiseSuppressor
{
public:
    static constexpr double crossfadeSeconds = 0.02;

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void setMode (NoiseMode mode) noexcept;
    void setAmount (float percent) noexcept;
    void process (float* samples, int numSamples) noexcept;

    /// Latency of the mode being switched to, or of the mode heard when idle.
    int getLatencySamples() const noexcept;

    /// The AI voice-activity estimate, or -1 when AI is not the heard mode.
    float getSpeechProbability() const noexcept;

private:
    int modeLatency (NoiseMode mode) const noexcept;
    int crossfadeLength() const noexcept;
    void startSwitch (NoiseMode mode) noexcept;
    void processMode (NoiseMode mode, float* samples, int numSamples) noexcept;
    void processChunk (float* samples, int numSamples) noexcept;

    SpectralDenoiser light_;
    RnnoiseDenoiser ai_;

    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 0;

    // Scratch buffers are allocated by prepare() so process() never allocates.
    std::vector<float> inputScratch_;
    std::vector<float> newScratch_;

    NoiseMode activeMode_ = NoiseMode::off;
    NoiseMode targetMode_ = NoiseMode::off;
    NoiseMode pendingMode_ = NoiseMode::off;
    bool hasPending_ = false;
    bool switching_ = false;
    bool snapMode_ = true;

    int primeLeft_ = 0;       // samples of the new mode still to discard
    int fadePosition_ = 0;    // samples already crossfaded
    int fadeLength_ = 0;      // total crossfade length in samples
};
