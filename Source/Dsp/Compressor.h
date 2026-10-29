#pragma once
#include <JuceHeader.h>

#include "DspParams.h"

#include <algorithm>
#include <atomic>
#include <cmath>

/// Feed-forward compressor for the voice: an RMS-like level detector, the
/// textbook soft-knee gain computer (Giannoulis/Reiss) and a smoothed gain
/// reduction with separate attack and release, followed by make-up gain.
///
///  - Level: the mean square of the input through a one-pole low-pass with a
///    10 ms time constant, read in dB. A steady sine therefore reads at its peak
///    level - 3.01 dB, i.e. at its RMS, which is also what the threshold is
///    compared with.
///  - Gain computer: below (threshold - knee / 2) the level passes unchanged,
///    above (threshold + knee / 2) it rises 1 dB per `ratio` dB, and a quadratic
///    bridges the two so the knee has no corner.
///  - The reduction (in dB, so the smoothing is symmetric in loudness) follows
///    that target with an 8 ms time constant when it grows and 120 ms when it
///    falls. The slow release keeps the gain from pumping on the gaps inside a
///    word; the 10 ms detector makes the compressor ignore single glottal peaks.
///
/// Threshold, ratio and make-up are smoothed (about 20 ms) so moving a slider
/// never clicks. Smoothing and the detector are indexed by the sample count since
/// reset(), never by block boundaries, so the output does not depend on how the
/// host splits the stream into blocks. There is no look-ahead (zero latency).
/// process() is real-time safe.
class Compressor
{
public:
    static constexpr float kneeDb = 6.0f, attackMs = 8.0f, releaseMs = 120.0f;

    /// The static input/output level curve in dB. Continuous and monotonic for
    /// every ratio >= 1 (a smaller ratio is treated as 1) and every knee >= 0
    /// (0 is a hard knee).
    static float staticCurveDb (float inDb, float thresholdDb, float ratio, float kneeWidthDb) noexcept
    {
        const double over  = (double) inDb - (double) thresholdDb;
        const double slope = 1.0 / std::max (1.0, (double) ratio);   // output dB per input dB above the knee
        const double width = std::max (0.0, (double) kneeWidthDb);

        if (2.0 * over < -width)
            return inDb;

        if (2.0 * over > width)
            return (float) ((double) thresholdDb + over * slope);

        if (width <= 0.0)   // exactly on the threshold of a hard knee
            return inDb;

        const double t = over + 0.5 * width;
        return (float) ((double) inDb + (slope - 1.0) * t * t / (2.0 * width));
    }

    void prepare (double sampleRate)
    {
        jassert (sampleRate > 0.0);

        detectorCoef = 1.0 - std::exp (-1.0 / (detectorSeconds * sampleRate));
        attackCoef   = std::exp (-1.0 / (attackMs  * 0.001 * sampleRate));
        releaseCoef  = std::exp (-1.0 / (releaseMs * 0.001 * sampleRate));

        for (auto* s : { &thresholdDb, &ratio, &makeupDb })
            s->reset (sampleRate, smoothingSeconds);

        thresholdDb.setCurrentAndTargetValue (ParamRanges::compThresholdDb.defaultValue);
        ratio.setCurrentAndTargetValue (ParamRanges::compRatio.defaultValue);
        makeupDb.setCurrentAndTargetValue (ParamRanges::compMakeupDb.defaultValue);
        primed = false;

        reset();
    }

    /// Clears the detector and the gain reduction, and jumps the smoothed
    /// parameters to their targets, so the output after reset() depends only on
    /// the input that follows it.
    void reset() noexcept
    {
        meanSquare = 0.0;
        reductionDb = 0.0;

        thresholdDb.setCurrentAndTargetValue (thresholdDb.getTargetValue());
        ratio.setCurrentAndTargetValue (ratio.getTargetValue());
        makeupDb.setCurrentAndTargetValue (makeupDb.getTargetValue());

        gainReductionDb.store (0.0f, std::memory_order_relaxed);
    }

    /// The first call after prepare() takes effect immediately (there is no
    /// earlier setting to glide from); later calls glide over about 20 ms.
    /// Non-finite values are ignored, others are limited to the ranges of the UI.
    void setParams (float newThresholdDb, float newRatio, float newMakeupDb) noexcept
    {
        setTarget (thresholdDb, newThresholdDb, ParamRanges::compThresholdDb);
        setTarget (ratio, newRatio, ParamRanges::compRatio);
        setTarget (makeupDb, newMakeupDb, ParamRanges::compMakeupDb);
        primed = true;
    }

    void process (float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const double in = samples[i];

            meanSquare += detectorCoef * (in * in - meanSquare);

            if (meanSquare < tinyPower)   // flush before the decay turns into denormals
                meanSquare = 0.0;

            const float threshold = thresholdDb.getNextValue();
            const float slope     = ratio.getNextValue();
            const float makeup    = makeupDb.getNextValue();

            const float levelDb = (float) (10.0 * std::log10 (std::max (meanSquare, floorPower)));
            const double target = (double) staticCurveDb (levelDb, threshold, slope, kneeDb) - (double) levelDb;

            const double coef = target < reductionDb ? attackCoef : releaseCoef;
            reductionDb = target + coef * (reductionDb - target);

            if (reductionDb > -1.0e-9)
                reductionDb = 0.0;

            samples[i] = (float) (in * std::exp ((reductionDb + (double) makeup) * dbToNeper));
        }

        gainReductionDb.store ((float) reductionDb, std::memory_order_relaxed);
    }

    /// The gain reduction at the end of the last block in dB (0 or negative),
    /// without the make-up gain. Safe to read from any thread.
    float getGainReductionDb() const noexcept   { return gainReductionDb.load (std::memory_order_relaxed); }

private:
    using Smoother = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    static constexpr double detectorSeconds = 0.010;
    static constexpr double smoothingSeconds = 0.02;
    static constexpr double dbToNeper = 0.11512925464970229;   // ln (10) / 20: 10^(x / 20) == exp (x * dbToNeper)
    static constexpr double tinyPower = 1.0e-30;
    static constexpr double floorPower = 1.0e-10;               // -100 dB: far under any threshold

    static_assert (std::atomic<float>::is_always_lock_free);

    void setTarget (Smoother& smoother, float value, const ParamRange& range) noexcept
    {
        if (! std::isfinite (value))
            return;

        value = std::clamp (value, range.min, range.max);

        if (primed)
            smoother.setTargetValue (value);
        else
            smoother.setCurrentAndTargetValue (value);
    }

    std::atomic<float> gainReductionDb { 0.0f };

    Smoother thresholdDb { ParamRanges::compThresholdDb.defaultValue };
    Smoother ratio       { ParamRanges::compRatio.defaultValue };
    Smoother makeupDb    { ParamRanges::compMakeupDb.defaultValue };

    double detectorCoef = 0.002, attackCoef = 0.99, releaseCoef = 0.999;
    double meanSquare = 0.0;
    double reductionDb = 0.0;
    bool primed = false;
};
