#pragma once
#include <JuceHeader.h>

#include "DspParams.h"

#include <algorithm>
#include <atomic>
#include <cmath>

/// Noise gate for the room between sentences: a downward expander with a fixed
/// 30 dB range, driven by a peak detector, with hysteresis and a hold time.
///
///  - The detector is instant on the way up and falls with a 5 ms time constant,
///    so a word's first sample already counts and the ripple of a low voice
///    (the gap between two waveform peaks) is bridged.
///  - Hysteresis: the gate opens when the level reaches the threshold and only
///    starts to close once it stays under (threshold - 4 dB). Speech that hovers
///    around the threshold therefore does not flutter the gate.
///  - Hold: the level must stay under the close threshold for 150 ms in a row
///    before the gate closes, so short gaps inside a sentence stay open. Any peak
///    over the close threshold restarts the hold.
///  - The gain follows the state with straight ramps in dB: the full 30 dB in
///    1 ms upwards, the full 30 dB in 150 ms downwards. A closed gate therefore
///    reaches -30 dB exactly releaseMs after the hold ran out, rather than
///    approaching it asymptotically like a one-pole release would.
///
/// There is no look-ahead (zero latency), and the gate starts closed so that
/// start-up or a reset() never lets a burst of room noise through. The threshold
/// only decides the state and the gain behind it is always ramped, so it needs no
/// smoothing of its own. Nothing here depends on block boundaries; process() is
/// real-time safe.
class NoiseGate
{
public:
    static constexpr float hysteresisDb = 4.0f, rangeDb = 30.0f;
    static constexpr float attackMs = 1.0f, holdMs = 150.0f, releaseMs = 150.0f;

    void prepare (double sampleRate)
    {
        jassert (sampleRate > 0.0);

        const double attackSamples  = std::max (1.0, attackMs  * 0.001 * sampleRate);
        const double releaseSamples = std::max (1.0, releaseMs * 0.001 * sampleRate);

        detectorRelease = (float) std::exp (-1.0 / (detectorReleaseSeconds * sampleRate));
        holdSamples     = std::max (1, juce::roundToInt (holdMs * 0.001 * sampleRate));
        attackStep      = std::pow (10.0,  (double) rangeDb / 20.0 / attackSamples);
        releaseStep     = std::pow (10.0, -(double) rangeDb / 20.0 / releaseSamples);

        setThresholdDb (ParamRanges::gateThresholdDb.defaultValue);
        reset();
    }

    /// Closes the gate and clears the detector, so the output after reset()
    /// depends only on the input that follows it.
    void reset() noexcept
    {
        level = 0.0f;
        holdLeft = 0;
        open = false;
        gain = floorGain;
        gainReductionDb.store (-rangeDb, std::memory_order_relaxed);
    }

    /// The gate opens at `db` and starts to close under `db - hysteresisDb`.
    /// Non-finite values are ignored, others are limited to the range of the UI.
    void setThresholdDb (float db) noexcept
    {
        if (! std::isfinite (db))
            return;

        db = std::clamp (db, ParamRanges::gateThresholdDb.min, ParamRanges::gateThresholdDb.max);

        openThreshold  = (float) std::pow (10.0, (double) db / 20.0);
        closeThreshold = (float) std::pow (10.0, (double) (db - hysteresisDb) / 20.0);
    }

    void process (float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float in = samples[i];

            level = std::max (std::abs (in), level * detectorRelease);

            if (level < tinyLevel)   // flush before the decay turns into denormals
                level = 0.0f;

            if (open)
            {
                if (level >= closeThreshold)
                    holdLeft = holdSamples;
                else if (--holdLeft <= 0)
                    open = false;
            }
            else if (level >= openThreshold)
            {
                open = true;
                holdLeft = holdSamples;
            }

            gain = open ? std::min (1.0, gain * attackStep)
                        : std::max (floorGain, gain * releaseStep);

            samples[i] = (float) ((double) in * gain);
        }

        gainReductionDb.store ((float) (20.0 * std::log10 (gain)), std::memory_order_relaxed);
    }

    /// Where the gain is at the end of the last block: 0 when open, -rangeDb
    /// when closed, in between while it ramps. Safe to read from any thread.
    float getGainReductionDb() const noexcept   { return gainReductionDb.load (std::memory_order_relaxed); }

private:
    static constexpr double detectorReleaseSeconds = 0.005;
    static constexpr float tinyLevel = 1.0e-20f;
    static constexpr double floorGain = 0.031622776601683793;   // 10^(-rangeDb / 20)

    static_assert (std::atomic<float>::is_always_lock_free);

    std::atomic<float> gainReductionDb { 0.0f };

    float detectorRelease = 0.99f;
    double attackStep = 1.07, releaseStep = 0.999;
    double gain = floorGain;
    int holdSamples = 7200;
    int holdLeft = 0;

    float openThreshold = 0.00316f, closeThreshold = 0.002f;
    float level = 0.0f;
    bool open = false;
};
