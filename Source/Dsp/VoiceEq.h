#pragma once
#include <JuceHeader.h>

#include "Biquad.h"
#include "DspParams.h"

#include <algorithm>
#include <cmath>

/// Three-band voice EQ with fixed centres and one gain each: a bell that cuts
/// the boxy "mud" around 250 Hz, a bell that adds presence ("clarity") around
/// 3.5 kHz, and a gentle high shelf for "air" from 10 kHz up.
///
/// The gains are smoothed in dB (about 20 ms) so moving a slider never clicks.
/// Smoothing and coefficient updates are indexed by the sample count since
/// reset(), never by block boundaries: the coefficients are recomputed every
/// 32nd sample, so the output does not depend on how the host splits the stream
/// into blocks. At 0 dB a band is an exact pass-through. process() is real-time
/// safe.
class VoiceEq
{
public:
    static constexpr double mudHz = 250.0, mudQ = 1.0;
    static constexpr double clarityHz = 3500.0, clarityQ = 0.9;
    static constexpr double airHz = 10000.0, airSlope = 0.7;

    void prepare (double sampleRate)
    {
        jassert (sampleRate > 0.0);

        sr = sampleRate;

        for (auto* s : { &mudDb, &clarityDb, &airDb })
            s->reset (sr, smoothingSeconds);

        mudDb.setCurrentAndTargetValue (ParamRanges::eqMudDb.defaultValue);
        clarityDb.setCurrentAndTargetValue (ParamRanges::eqClarityDb.defaultValue);
        airDb.setCurrentAndTargetValue (ParamRanges::eqAirDb.defaultValue);
        primed = false;

        reset();
    }

    /// Clears the filter memory and jumps the smoothed gains to their targets, so
    /// the output after reset() depends only on the input that follows it.
    void reset() noexcept
    {
        mud.reset();
        clarity.reset();
        air.reset();

        mudDb.setCurrentAndTargetValue (mudDb.getTargetValue());
        clarityDb.setCurrentAndTargetValue (clarityDb.getTargetValue());
        airDb.setCurrentAndTargetValue (airDb.getTargetValue());

        updateCounter = 0;
        designPending = true;
    }

    /// The first call after prepare() takes effect immediately (there is no
    /// earlier setting to glide from); later calls glide over about 20 ms.
    /// Non-finite values are ignored, others are limited to +-24 dB.
    void setGains (float mudGainDb, float clarityGainDb, float airGainDb) noexcept
    {
        setTarget (mudDb, mudGainDb);
        setTarget (clarityDb, clarityGainDb);
        setTarget (airDb, airGainDb);
        primed = true;
    }

    void process (float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float m = mudDb.getNextValue();
            const float c = clarityDb.getNextValue();
            const float a = airDb.getNextValue();

            if (updateCounter == 0)
                redesign (m, c, a);

            samples[i] = air.processSample (clarity.processSample (mud.processSample (samples[i])));
            updateCounter = (updateCounter + 1) % updateInterval;
        }
    }

private:
    using Smoother = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

    static constexpr int updateInterval = 32;           // samples between coefficient updates
    static constexpr double smoothingSeconds = 0.02;
    static constexpr float maxGainDb = 24.0f;

    void setTarget (Smoother& smoother, float db) noexcept
    {
        if (! std::isfinite (db))
            return;

        db = std::clamp (db, -maxGainDb, maxGainDb);

        if (primed)
            smoother.setTargetValue (db);
        else
            smoother.setCurrentAndTargetValue (db);
    }

    /// Only the bands whose gain actually moved are redesigned, so a settled EQ
    /// costs three comparisons every 32 samples.
    void redesign (float m, float c, float a) noexcept
    {
        if (designPending || m != appliedMud)
        {
            mud.setCoeffs (BiquadDesign::peaking (sr, mudHz, mudQ, m));
            appliedMud = m;
        }

        if (designPending || c != appliedClarity)
        {
            clarity.setCoeffs (BiquadDesign::peaking (sr, clarityHz, clarityQ, c));
            appliedClarity = c;
        }

        if (designPending || a != appliedAir)
        {
            air.setCoeffs (BiquadDesign::highShelf (sr, airHz, airSlope, a));
            appliedAir = a;
        }

        designPending = false;
    }

    Smoother mudDb     { ParamRanges::eqMudDb.defaultValue };
    Smoother clarityDb { ParamRanges::eqClarityDb.defaultValue };
    Smoother airDb     { ParamRanges::eqAirDb.defaultValue };

    Biquad mud, clarity, air;

    double sr = 48000.0;
    float appliedMud = 0.0f, appliedClarity = 0.0f, appliedAir = 0.0f;
    int updateCounter = 0;
    bool designPending = true;
    bool primed = false;
};
