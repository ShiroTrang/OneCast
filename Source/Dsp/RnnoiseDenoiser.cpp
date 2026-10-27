#include "RnnoiseDenoiser.h"

#include "DspParams.h"

#include "rnnoise.h"

#include <algorithm>
#include <cmath>

namespace
{
    /// RNNoise works on the 16-bit scale; the rest of the chain on +-1.0.
    constexpr float toRnnoiseScale   = 32768.0f;
    constexpr float fromRnnoiseScale = 1.0f / 32768.0f;
}

void RnnoiseDenoiser::StateDeleter::operator() (DenoiseState* s) const noexcept
{
    if (s != nullptr)
        rnnoise_destroy (s);
}

RnnoiseDenoiser::RnnoiseDenoiser()
{
    // The compile-time constants must agree with the library that is linked.
    jassert (rnnoise_get_frame_size() == frameSize);

    state.reset (rnnoise_create (nullptr));   // the built-in model
    prepare (48000.0, 512);
}

RnnoiseDenoiser::~RnnoiseDenoiser() = default;

void RnnoiseDenoiser::prepare (double newSampleRate, int maxBlockSize)
{
    jassert (newSampleRate > 0.0 && maxBlockSize > 0);
    juce::ignoreUnused (maxBlockSize);   // everything runs sample by sample, no scratch buffers

    // Bounds keep the converters' tables small and a bogus rate harmless.
    sampleRate = juce::jlimit (8000.0, 768000.0, newSampleRate > 0.0 ? newSampleRate : 48000.0);
    resampling = std::abs (sampleRate - (double) rnnoiseSampleRate) >= 0.5;

    if (! resampling)
    {
        sampleRate = (double) rnnoiseSampleRate;
        latencySamples.store (latencyAt48k, std::memory_order_relaxed);
    }
    else
    {
        // Both converters add a constant delay; the total, in samples at the
        // device rate, must be a whole number so the dry path can match it exactly.
        //
        //   latency = d1 + (latencyAt48k + d2) * sampleRate / 48000
        //
        // d1 (device-rate samples) is the up-converter's own look-ahead. The
        // down-converter has its look-ahead h2 (48 kHz samples) plus one sample
        // of margin, so it never has to wait for input that the up-converter
        // has not produced yet; the fractional remainder of the rounding goes
        // into its delay d2, which the converter accepts as a real number.
        const double deviceSamplesPerRnnoiseSample = sampleRate / (double) rnnoiseSampleRate;

        const int d1 = StreamingResampler::getMinimumDelay (sampleRate, (double) rnnoiseSampleRate);
        const int h2 = StreamingResampler::getMinimumDelay ((double) rnnoiseSampleRate, sampleRate);

        const int tail = (int) std::ceil ((double) (latencyAt48k + h2 + 1) * deviceSamplesPerRnnoiseSample);
        const double d2 = (double) tail / deviceSamplesPerRnnoiseSample - (double) latencyAt48k;

        toRnnoiseRate.prepare (sampleRate, (double) rnnoiseSampleRate, (double) d1);
        fromRnnoiseRate.prepare ((double) rnnoiseSampleRate, sampleRate, d2);

        latencySamples.store (d1 + tail, std::memory_order_relaxed);
    }

    dryLine.assign ((size_t) getLatencySamples(), 0.0f);

    amount.reset (sampleRate, amountRampSeconds);

    if (state == nullptr)
        state.reset (rnnoise_create (nullptr));

    reset();
}

void RnnoiseDenoiser::resetRnnoiseState() noexcept
{
    // Re-initialises the existing state in place (no allocation: with the
    // built-in model it only clears the struct and points at static weights).
    if (state != nullptr)
    {
        const int result = rnnoise_init (state.get(), nullptr);
        jassert (result == 0);
        juce::ignoreUnused (result);
    }
}

void RnnoiseDenoiser::reset() noexcept
{
    resetRnnoiseState();

    collecting.fill (0.0f);
    denoised.fill (0.0f);
    framePosition = 0;

    toRnnoiseRate.reset();
    fromRnnoiseRate.reset();

    std::fill (dryLine.begin(), dryLine.end(), 0.0f);
    dryPosition = 0;

    snapAmount = true;
    speechProbability.store (0.0f, std::memory_order_relaxed);
}

void RnnoiseDenoiser::setAmount (float percent) noexcept
{
    if (std::isnan (percent))
        return;

    const float clamped = std::clamp (percent, ParamRanges::noiseAmount.min, ParamRanges::noiseAmount.max);
    amountTarget.store (clamped * 0.01f, std::memory_order_relaxed);
}

float RnnoiseDenoiser::processAt48k (float x) noexcept
{
    // The output of the previous finished frame plays while this one is being
    // collected. That is the FIFO's frame of delay.
    const float y = denoised[(size_t) framePosition] * fromRnnoiseScale;
    collecting[(size_t) framePosition] = x * toRnnoiseScale;

    if (++framePosition == frameSize)
    {
        framePosition = 0;

        if (state != nullptr)
        {
            speechProbability.store (rnnoise_process_frame (state.get(), denoised.data(), collecting.data()),
                                     std::memory_order_relaxed);

            // A non-finite value in RNNoise's recurrent state would repeat forever,
            // so a bad frame is dropped and the network starts over.
            bool finite = true;
            for (const float v : denoised)
                finite = finite && std::isfinite (v);

            if (! finite)
            {
                resetRnnoiseState();
                denoised.fill (0.0f);
                speechProbability.store (0.0f, std::memory_order_relaxed);
            }
        }
        else
        {
            denoised.fill (0.0f);
        }
    }

    return y;
}

void RnnoiseDenoiser::process (float* samples, int numSamples) noexcept
{
    // The network's recurrent state decays towards denormals in silence, which
    // some CPUs handle very slowly.
    juce::ScopedNoDenormals noDenormals;

    if (snapAmount)
    {
        amount.setCurrentAndTargetValue (amountTarget.load (std::memory_order_relaxed));
        snapAmount = false;
    }
    else
    {
        amount.setTargetValue (amountTarget.load (std::memory_order_relaxed));
    }

    const int lineLength = (int) dryLine.size();

    for (int i = 0; i < numSamples; ++i)
    {
        const float x = std::isfinite (samples[i]) ? samples[i] : 0.0f;

        // Dry: the input from exactly getLatencySamples() ago.
        const float dry = dryLine[(size_t) dryPosition];
        dryLine[(size_t) dryPosition] = x;
        if (++dryPosition == lineLength)
            dryPosition = 0;

        float wet = 0.0f;

        if (! resampling)
        {
            wet = processAt48k (x);
        }
        else
        {
            toRnnoiseRate.push (x);

            float s;
            while (toRnnoiseRate.pull (s))
                fromRnnoiseRate.push (processAt48k (s));

            // The margin built into the delays means this always succeeds.
            if (! fromRnnoiseRate.pull (wet))
            {
                jassertfalse;
                wet = 0.0f;
            }
        }

        const float a = amount.getNextValue();
        samples[i] = a * wet + (1.0f - a) * dry;
    }
}
