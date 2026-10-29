#pragma once
#include <JuceHeader.h>

#include "Biquad.h"
#include "DspParams.h"

#include <algorithm>
#include <cmath>
#include <numbers>

/// 3rd-order Butterworth high-pass (18 dB/octave) that removes rumble, desk
/// thumps and breath pops below the voice without touching its body.
///
/// Built as a one-pole high-pass followed by a biquad with Q = 1.0, the two
/// sections a 3rd-order Butterworth factors into. Both are bilinear transforms
/// prewarped at the same frequency, so the corner is -3 dB at every sample rate.
///
/// The cut-off is smoothed (about 20 ms, geometric because pitch is) so dragging
/// the slider never clicks. Smoothing and coefficient updates are indexed by the
/// sample count since reset(), never by block boundaries: the coefficients are
/// recomputed every 32nd sample, so the output does not depend on how the host
/// splits the stream into blocks. process() is real-time safe.
class HighPassFilter
{
public:
    void prepare (double sampleRate)
    {
        jassert (sampleRate > 0.0);

        sr = sampleRate;
        cutoff.reset (sr, smoothingSeconds);
        cutoff.setCurrentAndTargetValue (ParamRanges::hpfHz.defaultValue);
        primed = false;

        reset();
    }

    /// Clears the filter memory and jumps the smoothed cut-off to its target, so
    /// the output after reset() depends only on the input that follows it.
    void reset() noexcept
    {
        onePole.reset();
        stage.reset();

        cutoff.setCurrentAndTargetValue (cutoff.getTargetValue());
        updateCounter = 0;
        appliedHz = -1.0f;   // below the minimum cut-off: forces a design on the first sample
    }

    /// The first call after prepare() takes effect immediately (there is no
    /// earlier setting to glide from); later calls glide over about 20 ms.
    /// Non-finite values are ignored, others are limited to what the filter
    /// design can represent at the current sample rate.
    void setCutoffHz (float hz) noexcept
    {
        if (! std::isfinite (hz))
            return;

        hz = std::clamp (hz, minCutoffHz, (float) (0.45 * sr));

        if (primed)
        {
            cutoff.setTargetValue (hz);
        }
        else
        {
            cutoff.setCurrentAndTargetValue (hz);
            primed = true;
        }
    }

    void process (float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float hz = cutoff.getNextValue();

            if (updateCounter == 0 && hz != appliedHz)
                redesign (hz);

            samples[i] = stage.processSample (onePole.processSample (samples[i]));
            updateCounter = (updateCounter + 1) % updateInterval;
        }
    }

private:
    static constexpr int updateInterval = 32;           // samples between coefficient updates
    static constexpr double smoothingSeconds = 0.02;
    static constexpr float minCutoffHz = 5.0f;

    /// First-order high-pass, -3 dB at `hz`:
    ///   H(z) = (1 - z^-1) / ((1 + k) + (k - 1) z^-1),  k = tan (pi hz / sr).
    /// The same prewarp as the RBJ biquad, so the two sections share one corner.
    static BiquadCoeffs onePoleHighPass (double sampleRate, double hz) noexcept
    {
        const double k = std::tan (std::numbers::pi * hz / sampleRate);
        const double norm = 1.0 / (1.0 + k);

        BiquadCoeffs c;
        c.b0 = (float) norm;
        c.b1 = (float) -norm;
        c.a1 = (float) ((k - 1.0) * norm);
        return c;
    }

    void redesign (float hz) noexcept
    {
        onePole.setCoeffs (onePoleHighPass (sr, hz));
        stage.setCoeffs (BiquadDesign::highPass (sr, hz, 1.0));
        appliedHz = hz;
    }

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>
        cutoff { ParamRanges::hpfHz.defaultValue };

    Biquad onePole, stage;

    double sr = 48000.0;
    float appliedHz = -1.0f;
    int updateCounter = 0;
    bool primed = false;
};
