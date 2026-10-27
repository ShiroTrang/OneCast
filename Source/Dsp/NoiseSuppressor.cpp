#include "NoiseSuppressor.h"

#include <algorithm>
#include <cmath>

void NoiseSuppressor::prepare (double sampleRate, int maxBlockSize)
{
    jassert (sampleRate > 0.0 && maxBlockSize > 0);

    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlockSize_ = std::max (1, maxBlockSize);

    inputScratch_.assign ((size_t) maxBlockSize_, 0.0f);
    newScratch_.assign ((size_t) maxBlockSize_, 0.0f);

    light_.prepare (sampleRate_, maxBlockSize_);
    ai_.prepare (sampleRate_, maxBlockSize_);

    reset();
}

void NoiseSuppressor::reset() noexcept
{
    light_.reset();
    ai_.reset();

    activeMode_ = NoiseMode::off;
    targetMode_ = NoiseMode::off;
    pendingMode_ = NoiseMode::off;
    hasPending_ = false;
    switching_ = false;
    snapMode_ = true;
    primeLeft_ = 0;
    fadePosition_ = 0;
    fadeLength_ = 0;
}

void NoiseSuppressor::setMode (NoiseMode mode) noexcept
{
    switch (mode)
    {
        case NoiseMode::off:
        case NoiseMode::light:
        case NoiseMode::ai:
            break;

        default:
            mode = NoiseMode::light;
            break;
    }

    // The first mode after prepare()/reset() is the starting point, not a
    // switch: there is no earlier output to crossfade from.
    if (snapMode_)
    {
        activeMode_ = mode;
        targetMode_ = mode;
        switching_ = false;
        hasPending_ = false;
        snapMode_ = false;
        return;
    }

    // A switch is already running. Queue the newest request instead of
    // restarting in the middle of a crossfade, which would click.
    if (switching_)
    {
        if (mode == targetMode_)
            return;

        pendingMode_ = mode;
        hasPending_ = true;
        return;
    }

    if (mode == activeMode_)
        return;

    startSwitch (mode);
}

void NoiseSuppressor::setAmount (float percent) noexcept
{
    light_.setAmount (percent);
    ai_.setAmount (percent);
}

void NoiseSuppressor::process (float* samples, int numSamples) noexcept
{
    if (samples == nullptr || numSamples <= 0 || maxBlockSize_ <= 0)
        return;

    juce::ScopedNoDenormals noDenormals;

    int done = 0;

    while (done < numSamples)
    {
        const int n = std::min (numSamples - done, maxBlockSize_);
        processChunk (samples + done, n);
        done += n;
    }
}

int NoiseSuppressor::getLatencySamples() const noexcept
{
    return switching_ ? modeLatency (targetMode_) : modeLatency (activeMode_);
}

float NoiseSuppressor::getSpeechProbability() const noexcept
{
    return activeMode_ == NoiseMode::ai ? ai_.getSpeechProbability() : -1.0f;
}

int NoiseSuppressor::modeLatency (NoiseMode mode) const noexcept
{
    switch (mode)
    {
        case NoiseMode::light:  return light_.getLatencySamples();
        case NoiseMode::ai:     return ai_.getLatencySamples();
        case NoiseMode::off:
        default:                return 0;
    }
}

int NoiseSuppressor::crossfadeLength() const noexcept
{
    return std::max (1, (int) std::lround (crossfadeSeconds * sampleRate_));
}

void NoiseSuppressor::startSwitch (NoiseMode mode) noexcept
{
    targetMode_ = mode;
    switching_ = true;

    // A denoiser outputs silence for its first getLatencySamples() samples, so
    // its output is discarded until it has produced that many primed samples.
    primeLeft_ = mode == NoiseMode::off ? 0 : modeLatency (mode);
    fadeLength_ = crossfadeLength();
    fadePosition_ = 0;
}

void NoiseSuppressor::processMode (NoiseMode mode, float* samples, int numSamples) noexcept
{
    switch (mode)
    {
        case NoiseMode::light:  light_.process (samples, numSamples); break;
        case NoiseMode::ai:     ai_.process (samples, numSamples); break;
        case NoiseMode::off:
        default:                break;
    }
}

void NoiseSuppressor::processChunk (float* samples, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    // The original input is kept for the incoming processor; `samples` itself
    // may be overwritten with the heard output below.
    std::copy (samples, samples + numSamples, inputScratch_.begin());

    int pos = 0;

    while (pos < numSamples)
    {
        if (! switching_)
        {
            processMode (activeMode_, samples + pos, numSamples - pos);
            pos = numSamples;
            break;
        }

        if (primeLeft_ > 0)
        {
            const int k = std::min (primeLeft_, numSamples - pos);

            // The old mode stays on the output while the new one primes.
            processMode (activeMode_, samples + pos, k);

            std::copy (inputScratch_.begin() + pos, inputScratch_.begin() + pos + k, newScratch_.begin());
            processMode (targetMode_, newScratch_.data(), k);

            primeLeft_ -= k;
            pos += k;
            continue;
        }

        const int k = std::min (fadeLength_ - fadePosition_, numSamples - pos);

        processMode (activeMode_, samples + pos, k);

        std::copy (inputScratch_.begin() + pos, inputScratch_.begin() + pos + k, newScratch_.begin());
        processMode (targetMode_, newScratch_.data(), k);

        for (int i = 0; i < k; ++i)
        {
            const float mix = (float) (fadePosition_ + i) / (float) fadeLength_;
            samples[pos + i] += mix * (newScratch_[i] - samples[pos + i]);
        }

        fadePosition_ += k;
        pos += k;

        if (fadePosition_ >= fadeLength_)
        {
            switching_ = false;
            activeMode_ = targetMode_;
            fadePosition_ = 0;

            if (hasPending_)
            {
                startSwitch (pendingMode_);
                hasPending_ = false;
            }
        }
    }
}
