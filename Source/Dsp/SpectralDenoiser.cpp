#include "SpectralDenoiser.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    constexpr int numSubWindows = 8;

    // Time constants. They are given in seconds and turned into per-frame
    // coefficients in prepare(), so the behaviour does not depend on whether the
    // frame rate is 750 or 689 a second.
    constexpr double powerSmoothingSeconds   = 0.040;   // smoothing of the power the noise tracker watches
    constexpr double subWindowSeconds        = 0.200;   // 8 of these: the noise floor is the quietest 1.6 s
    constexpr double decisionDirectedSeconds = 0.030;   // memory of the a priori SNR
    constexpr double gainRiseSeconds         = 0.002;   // a word's first syllable must not be clipped
    constexpr double gainFallSeconds         = 0.015;   // slow enough not to chop word endings or flutter
    constexpr double amountSmoothingSeconds  = 0.020;   // slider moves are ramped in dB

    /// The minimum of the smoothed periodogram sits below the noise power it
    /// estimates, because it is the smallest of about 60 noisy values. For white
    /// noise it lands at 0.62 of the true level (measured), this puts it back.
    constexpr float noiseBias = 1.6f;

    constexpr float minNoisePower  = 1.0e-10f;   // keeps P / lambda finite in digital silence
    constexpr float silentBinPower = 1.0e-9f;    // mean bin power below this: nothing to learn the noise from
    constexpr float minPriorSnr    = 0.003f;     // -25 dB, below the largest floor anyway

    /// Sample values beyond this cannot come from an audio device; clamping
    /// keeps the squared spectrum inside float range.
    constexpr float maxSample = 1.0e4f;

    float amountToFloorGain (float percent) noexcept
    {
        if (percent <= 0.0f)
            return 1.0f;

        return std::pow (10.0f, -SpectralDenoiser::maxAttenuationDb * percent / 100.0f / 20.0f);
    }
}

void SpectralDenoiser::prepare (double sampleRate, int maxBlockSize)
{
    juce::ignoreUnused (maxBlockSize);
    jassert (sampleRate > 0.0);

    if (! (sampleRate > 0.0))
        sampleRate = 48000.0;

    // 256 points are 5.3 ms at 48 kHz. Above 50 kHz that would be too coarse in
    // frequency, and 512 points still fit the 6 ms budget up to 96 kHz.
    const int order = sampleRate <= 50000.0 ? 8 : 9;

    fftSize = 1 << order;
    hop = fftSize / 4;
    numBins = fftSize / 2 + 1;
    latencySamples = fftSize - 1;
    ringMask = (unsigned) fftSize - 1;

    fft = std::make_unique<juce::dsp::FFT> (order);

    // sqrt-Hann at both ends. Periodic (divided by N, not N - 1), so that the
    // product, a Hann window, adds up to 2 at a hop of N / 4; the synthesis
    // window carries the 1/2 that makes the overlap-add gain exactly one.
    analysisWindow.resize ((size_t) fftSize);
    synthesisWindow.resize ((size_t) fftSize);

    for (int n = 0; n < fftSize; ++n)
    {
        const auto w = (float) std::sin (juce::MathConstants<double>::pi * (double) n / (double) fftSize);
        analysisWindow[(size_t) n]  = w;
        synthesisWindow[(size_t) n] = 0.5f * w;
    }

    inputRing.assign ((size_t) fftSize, 0.0f);
    outputRing.assign ((size_t) fftSize, 0.0f);
    fftData.assign ((size_t) fftSize * 2, 0.0f);

    for (auto* v : { &power, &smoothedPower, &noisePower, &cleanPower, &wienerGain, &appliedGain,
                     &currentMin, &finishedMin })
        v->assign ((size_t) numBins, 0.0f);

    subWindowMin.assign ((size_t) numSubWindows * (size_t) numBins, 0.0f);

    const double frameSeconds = (double) hop / sampleRate;
    subWindowLength  = std::max (1, juce::roundToInt (subWindowSeconds / frameSeconds));
    powerSmoothing   = (float) std::exp (-frameSeconds / powerSmoothingSeconds);
    decisionDirected = (float) std::exp (-frameSeconds / decisionDirectedSeconds);
    gainRise         = (float) (1.0 - std::exp (-frameSeconds / gainRiseSeconds));
    gainFall         = (float) (1.0 - std::exp (-frameSeconds / gainFallSeconds));
    amountCoeff      = (float) (1.0 - std::exp (-1.0 / (amountSmoothingSeconds * sampleRate)));

    reset();
}

void SpectralDenoiser::reset() noexcept
{
    if (fftSize == 0)
        return;

    std::fill (inputRing.begin(), inputRing.end(), 0.0f);
    std::fill (outputRing.begin(), outputRing.end(), 0.0f);
    pos = 0;
    hopCounter = 0;

    // The stream is pretended to have started fftSize samples of silence ago,
    // so the first overlapping frames are complete but say nothing about the
    // noise: they are passed on untouched.
    warmupFrames = fftSize / hop - 1;

    for (auto* v : { &power, &smoothedPower, &noisePower, &cleanPower, &wienerGain, &currentMin, &finishedMin })
        std::fill (v->begin(), v->end(), 0.0f);

    std::fill (appliedGain.begin(), appliedGain.end(), 1.0f);
    std::fill (subWindowMin.begin(), subWindowMin.end(), std::numeric_limits<float>::max());

    subWindowCounter = 0;
    subWindowIndex = 0;
    trackerPrimed = false;

    snapAmount = true;
}

void SpectralDenoiser::setAmount (float percent) noexcept
{
    if (std::isnan (percent))
        return;

    amountTarget.store (std::clamp (percent, ParamRanges::noiseAmount.min, ParamRanges::noiseAmount.max),
                        std::memory_order_relaxed);
}

void SpectralDenoiser::process (float* samples, int numSamples) noexcept
{
    if (fftSize == 0 || samples == nullptr)
        return;

    juce::ScopedNoDenormals noDenormals;

    const float target = amountTarget.load (std::memory_order_relaxed);

    if (snapAmount)
    {
        amountSmoothed = target;
        snapAmount = false;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        // One-pole in the amount, advanced every sample: where the ramp is
        // after n samples does not depend on how the host cut the stream.
        const float diff = target - amountSmoothed;
        amountSmoothed = std::abs (diff) < 1.0e-4f ? target : amountSmoothed + diff * amountCoeff;

        // A NaN or an infinity would stay in the recursive state for good.
        const float x = samples[i];
        inputRing[pos] = std::isfinite (x) ? std::clamp (x, -maxSample, maxSample) : 0.0f;

        pos = (pos + 1) & ringMask;

        if (++hopCounter == hop)
        {
            hopCounter = 0;
            processFrame();
        }

        // `pos` is now the slot of the oldest of the last fftSize inputs, and
        // that slot of the output ring is the next sample to leave: it is
        // complete once the frame ending at the newest input has been added.
        // That is fftSize - 1 samples after the input it answers, whatever the
        // block size.
        samples[i] = outputRing[pos];
        outputRing[pos] = 0.0f;
    }
}

void SpectralDenoiser::processFrame() noexcept
{
    float* data = fftData.data();

    for (int n = 0; n < fftSize; ++n)
        data[n] = inputRing[(pos + (unsigned) n) & ringMask] * analysisWindow[(size_t) n];

    fft->performRealOnlyForwardTransform (data, true);

    if (warmupFrames > 0)
    {
        --warmupFrames;
    }
    else
    {
        float total = 0.0f;

        for (int k = 0; k < numBins; ++k)
        {
            const float re = data[2 * k], im = data[2 * k + 1];
            power[(size_t) k] = re * re + im * im;
            total += power[(size_t) k];
        }

        updateNoiseEstimate (total >= silentBinPower * (float) numBins);

        if (trackerPrimed)
            applyGain (data, amountToFloorGain (amountSmoothed));
    }

    fft->performRealOnlyInverseTransform (data);

    for (int n = 0; n < fftSize; ++n)
        outputRing[(pos + (unsigned) n) & ringMask] += data[n] * synthesisWindow[(size_t) n];
}

void SpectralDenoiser::updateNoiseEstimate (bool learn) noexcept
{
    // Digital silence says nothing about the noise, and learning from it would
    // pin the estimate at zero until the sub-windows have turned over.
    if (learn)
    {
        // Three-bin average first: fewer degrees of freedom to be wrong about.
        const auto neighbourAverage = [this] (int k)
        {
            const float below = power[(size_t) std::max (k - 1, 0)];
            const float above = power[(size_t) std::min (k + 1, numBins - 1)];
            return 0.25f * below + 0.5f * power[(size_t) k] + 0.25f * above;
        };

        if (! trackerPrimed)
        {
            for (int k = 0; k < numBins; ++k)
            {
                smoothedPower[(size_t) k] = neighbourAverage (k);
                currentMin[(size_t) k] = smoothedPower[(size_t) k];
                finishedMin[(size_t) k] = std::numeric_limits<float>::max();
            }

            subWindowCounter = 0;
            subWindowIndex = 0;
            trackerPrimed = true;
        }
        else
        {
            for (int k = 0; k < numBins; ++k)
            {
                auto& s = smoothedPower[(size_t) k];
                s = powerSmoothing * s + (1.0f - powerSmoothing) * neighbourAverage (k);
                currentMin[(size_t) k] = std::min (currentMin[(size_t) k], s);
            }

            if (++subWindowCounter >= subWindowLength)
            {
                subWindowCounter = 0;

                float* slot = subWindowMin.data() + (size_t) subWindowIndex * (size_t) numBins;
                subWindowIndex = (subWindowIndex + 1) % numSubWindows;

                for (int k = 0; k < numBins; ++k)
                {
                    slot[k] = currentMin[(size_t) k];
                    currentMin[(size_t) k] = smoothedPower[(size_t) k];
                }

                for (int k = 0; k < numBins; ++k)
                {
                    float m = std::numeric_limits<float>::max();

                    for (int u = 0; u < numSubWindows; ++u)
                        m = std::min (m, subWindowMin[(size_t) u * (size_t) numBins + (size_t) k]);

                    finishedMin[(size_t) k] = m;
                }
            }
        }
    }

    if (trackerPrimed)
    {
        for (int k = 0; k < numBins; ++k)
        {
            const float floorOfBin = std::min (finishedMin[(size_t) k], currentMin[(size_t) k]);
            noisePower[(size_t) k] = std::max (noiseBias * floorOfBin, minNoisePower);
        }
    }
}

void SpectralDenoiser::applyGain (float* spectrum, float floorGain) noexcept
{
    // Decision-directed a priori SNR (Ephraim and Malah) and the Wiener gain
    // it gives. cleanPower remembers what the previous frame kept of each bin.
    for (int k = 0; k < numBins; ++k)
    {
        const auto i = (size_t) k;
        const float invNoise = 1.0f / noisePower[i];
        const float posteriori = power[i] * invNoise;

        const float xi = std::max (decisionDirected * cleanPower[i] * invNoise
                                       + (1.0f - decisionDirected) * std::max (posteriori - 1.0f, 0.0f),
                                   minPriorSnr);
        const float g = xi / (1.0f + xi);

        cleanPower[i] = g * g * power[i];
        wienerGain[i] = g;
    }

    // Smooth over frequency, then over time (quick up, slow down), then floor.
    // The floor goes last, so a floor of 1 gives exactly 1 in every bin.
    float below = wienerGain[0];

    for (int k = 0; k < numBins; ++k)
    {
        const auto i = (size_t) k;
        const float here = wienerGain[i];
        const float above = wienerGain[(size_t) std::min (k + 1, numBins - 1)];
        const float wanted = std::max (0.25f * below + 0.5f * here + 0.25f * above, floorGain);
        below = here;

        float g = appliedGain[i];
        g += (wanted - g) * (wanted > g ? gainRise : gainFall);
        g = std::clamp (g, floorGain, 1.0f);
        appliedGain[i] = g;

        spectrum[2 * k]     *= g;
        spectrum[2 * k + 1] *= g;
    }
}
