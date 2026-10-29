#pragma once
#include <JuceHeader.h>

#include "Biquad.h"
#include "DspParams.h"

#include <algorithm>
#include <atomic>
#include <cmath>

/// Split-band de-esser: only the 5-9 kHz region is turned down, and only while
/// that region dominates the signal.
///
/// The band is cut out with a unity-gain band-pass b (centre sqrt (5 kHz * 9 kHz),
/// bandwidth 4 kHz), and the output is  x - (1 - g) * b  with a gain g <= 1.
/// Everything outside the band is untouched, the cut is exactly g at the centre
/// of the band, and at g = 1 the output is bit-identical to the input, so an idle
/// de-esser colours nothing.
///
/// The band-pass is built from Biquad.h's peaking bell instead of a second
/// filter design: a bell of gain A^2 is  1 + (A^2 - 1) * bandpass  whose
/// band-pass has Q_bandpass = A * Q_bell, so  (bell (x) - x) / (A^2 - 1)  is the
/// band-pass. One fixed biquad does both jobs, the sidechain and the audio path.
///
/// "Dominates" is measured as the power in the band divided by the power of the
/// whole signal (a tiny floor keeps a near-silent signal from counting). Voiced
/// speech keeps most of its power under 5 kHz, an "s" keeps most of it inside the
/// band. Under dominanceLow the de-esser does nothing, over dominanceHigh it cuts
/// by the full amount, in between it scales. The amount sets the deepest cut:
/// maxReductionDb at 100 %.
///
/// The cut follows the detector with a 2 ms attack and a 40 ms release, and the
/// amount is smoothed (about 20 ms). Smoothing and detectors are indexed by the
/// sample count since reset(), never by block boundaries, so the output does not
/// depend on how the host splits the stream into blocks. There is no look-ahead
/// (zero latency). process() is real-time safe.
class DeEsser
{
public:
    static constexpr double bandLowHz = 5000.0, bandHighHz = 9000.0;
    static constexpr float maxReductionDb = 12.0f;     // at amount 100 %

    void prepare (double sampleRate)
    {
        jassert (sampleRate > 0.0);

        const double a = std::pow (10.0, bellGainDb / 40.0);
        const double centreHz = std::sqrt (bandLowHz * bandHighHz);
        const double bandQ = centreHz / (bandHighHz - bandLowHz);

        band.setCoeffs (BiquadDesign::peaking (sampleRate, centreHz, bandQ / a, bellGainDb));
        bandScale = (float) (1.0 / (a * a - 1.0));

        detectorCoef = 1.0 - std::exp (-1.0 / (detectorSeconds * sampleRate));
        attackCoef   = (float) std::exp (-1.0 / (attackSeconds  * sampleRate));
        releaseCoef  = (float) std::exp (-1.0 / (releaseSeconds * sampleRate));

        amount.reset (sampleRate, smoothingSeconds);
        amount.setCurrentAndTargetValue (ParamRanges::deEssAmount.defaultValue);
        primed = false;

        reset();
    }

    /// Clears the filter and the detectors and jumps the smoothed amount to its
    /// target, so the output after reset() depends only on the input that follows.
    void reset() noexcept
    {
        band.reset();
        bandPower = 0.0;
        fullPower = 0.0;
        activity = 0.0f;

        amount.setCurrentAndTargetValue (amount.getTargetValue());
        gainReductionDb.store (0.0f, std::memory_order_relaxed);
    }

    /// 0 turns the de-esser off, 100 allows maxReductionDb. The first call after
    /// prepare() takes effect immediately (there is no earlier setting to glide
    /// from); later calls glide over about 20 ms. Non-finite values are ignored,
    /// others are limited to 0..100.
    void setAmount (float percent) noexcept
    {
        if (! std::isfinite (percent))
            return;

        percent = std::clamp (percent, ParamRanges::deEssAmount.min, ParamRanges::deEssAmount.max);

        if (primed)
            amount.setTargetValue (percent);
        else
            amount.setCurrentAndTargetValue (percent);

        primed = true;
    }

    void process (float* samples, int numSamples) noexcept
    {
        float cutDb = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float in = samples[i];
            const float bandSignal = (band.processSample (in) - in) * bandScale;

            bandPower += detectorCoef * ((double) bandSignal * bandSignal - bandPower);
            fullPower += detectorCoef * ((double) in * in - fullPower);

            if (bandPower < tinyPower)   // flush before the decay turns into denormals
                bandPower = 0.0;
            if (fullPower < tinyPower)
                fullPower = 0.0;

            const double dominance = bandPower / (fullPower + powerFloor);
            const float wanted = (float) std::clamp ((dominance - dominanceLow) / (dominanceHigh - dominanceLow),
                                                     0.0, 1.0);

            activity = wanted + (wanted > activity ? attackCoef : releaseCoef) * (activity - wanted);

            if (activity < 1.0e-6f)
                activity = 0.0f;

            cutDb = amount.getNextValue() * (0.01f * maxReductionDb) * activity;

            const float gain = std::exp (-cutDb * dbToNeper);
            samples[i] = in - (1.0f - gain) * bandSignal;
        }

        if (numSamples > 0)
            gainReductionDb.store (-cutDb, std::memory_order_relaxed);
    }

    /// The cut at the centre of the band at the end of the last block in dB (0 or
    /// negative). Safe to read from any thread.
    float getGainReductionDb() const noexcept   { return gainReductionDb.load (std::memory_order_relaxed); }

private:
    using Smoother = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    static constexpr double bellGainDb = 12.0;          // of the bell the band-pass is made from
    static constexpr double detectorSeconds = 0.004;
    static constexpr double attackSeconds = 0.002, releaseSeconds = 0.04;
    static constexpr double smoothingSeconds = 0.02;
    static constexpr double dominanceLow = 0.126, dominanceHigh = 0.398;   // band power / total power: -9 dB, -4 dB
    static constexpr double powerFloor = 1.0e-6;        // -60 dBFS: quieter than that never counts as sibilance
    static constexpr double tinyPower = 1.0e-30;
    static constexpr float dbToNeper = 0.11512925f;     // ln (10) / 20

    static_assert (std::atomic<float>::is_always_lock_free);

    std::atomic<float> gainReductionDb { 0.0f };

    Smoother amount { ParamRanges::deEssAmount.defaultValue };
    Biquad band;

    double detectorCoef = 0.005;
    double bandPower = 0.0, fullPower = 0.0;
    float attackCoef = 0.99f, releaseCoef = 0.999f;
    float bandScale = 0.3358f;
    float activity = 0.0f;
    bool primed = false;
};
