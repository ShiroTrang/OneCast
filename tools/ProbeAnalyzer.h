#pragma once
#include <JuceHeader.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

/// Streaming detector for a test sine: finds gaps (dropouts), phase jumps and
/// level dips. OneCastProbe feeds it the capture of "CABLE Output" while OneCast
/// plays a 440 Hz sine into "CABLE Input"; the 30 minute soak test passes only if
/// it reports nothing, so it has to be right in both directions: no false alarm
/// on a clean (or merely noisy) sine, and no glitch that slips through.
///
/// The rules, in samples at the given rate:
///  - It locks once 100 ms of steady sine has been seen. Nothing before the
///    lock is reported (the sine may simply not have started yet), and a signal
///    that is not a steady sine near `sineHz` never locks (silence, DC, hiss,
///    another pitch), so the caller can tell "no signal" from "clean signal".
///  - dropout: a run of at least 5 consecutive samples with |x| < 1e-6.
///  - jump: a sample step larger than 3 x (2 pi f / sr x amplitude).
///  - level: a 10 ms window whose RMS is more than 6 dB below the locked level.
///  - Glitches closer than 50 ms merge into one. The merged glitch keeps the
///    earliest time and the most specific kind (dropout over level over jump),
///    because the edges of a gap or a dip always also look like jumps.
///
/// The step rule alone misses a phase jump that happens near a zero crossing
/// (a polarity flip at a crossing never steps by more than a normal sample) and
/// a single dropped sample (it steps by less than 3 x). So a jump is also any
/// sample where the sine's own recurrence breaks: a pure sine satisfies
/// x[n] = 2 cos (w) x[n-1] - x[n-2] exactly, whatever its phase or amplitude,
/// and the residual of that equation is zero for a clean sine, about 1e-4 of
/// the amplitude for a 16-bit one, and large at any discontinuity. It stays
/// small for a small pitch error (it scales with w^2 x the error), so two
/// slightly different clocks do not trigger it.
///
/// process() is not real-time safe (a new glitch allocates); run it on a worker
/// or the main thread, never in the audio callback. All state advances one
/// sample at a time, so the result does not depend on how the input is split
/// into blocks.
class ProbeAnalyzer
{
public:
    struct Glitch
    {
        double timeSeconds = 0.0;      // where the glitch starts
        juce::String kind;             // "dropout" | "jump" | "level"
        double durationSeconds = 0.0;  // first to last sample of the merged glitch
    };

    ProbeAnalyzer (double sampleRate, double sineHz)
        : sr (sampleRate),
          sineFrequency (sineHz),
          omega (2.0 * juce::MathConstants<double>::pi * sineHz / sampleRate),
          recurrenceCoeff (2.0 * std::cos (omega)),
          twoSinOmega (2.0 * std::sin (omega)),
          lockSamples (std::max<std::int64_t> (1, std::llround (lockSeconds * sampleRate))),
          windowLength (std::max<std::int64_t> (1, std::llround (levelWindowSeconds * sampleRate))),
          mergeSamples (std::llround (mergeSeconds * sampleRate)),
          levelDropPower (std::pow (10.0, -levelDropDb / 10.0)),
          windowSquares ((size_t) windowLength, 0.0)
    {
        jassert (sampleRate > 0.0 && sineHz > 0.0 && sineHz < sampleRate * 0.5);
        jassert (twoSinOmega > 1.0e-6);
    }

    /// Feeds `numSamples` mono samples. Blocks of any size, including 0, are fine.
    void process (const float* mono, int numSamples) noexcept
    {
        if (mono == nullptr || numSamples <= 0)
            return;

        for (int i = 0; i < numSamples; ++i)
            step (mono[i]);
    }

    /// The glitches so far. The last one may still grow (see getSettledGlitchCount).
    const std::vector<Glitch>& getGlitches() const noexcept   { return glitches; }

    /// How many of getGlitches() can no longer change, so a live log can print
    /// them: all of them once the last one is more than 50 ms + 10 ms behind.
    size_t getSettledGlitchCount() const noexcept
    {
        if (glitches.empty())
            return 0;

        const bool lastIsFinal = (numProcessed - 1 - lastActivity) >= mergeSamples + windowLength;
        return lastIsFinal ? glitches.size() : glitches.size() - 1;
    }

    double getSecondsAnalysed() const noexcept   { return (double) numProcessed / sr; }

    /// True once 100 ms of steady sine has been seen. False means the input never
    /// looked like the sine, which is a failure for a test that expects one.
    bool isLocked() const noexcept   { return locked; }

    /// RMS in dBFS of the signal since the lock (including the 100 ms that
    /// locked it), or of everything seen so far while not locked.
    float getLevelDb() const noexcept
    {
        const double meanSquare = locked ? lockedSumSquares / (double) std::max<std::int64_t> (1, lockedCount)
                                         : allSumSquares / (double) std::max<std::int64_t> (1, numProcessed);

        return meanSquare > 1.0e-20 ? (float) (10.0 * std::log10 (meanSquare)) : -200.0f;
    }

private:
    // --- The rules from the brief --------------------------------------------
    static constexpr double lockSeconds        = 0.100;
    static constexpr double silenceThreshold   = 1.0e-6;
    static constexpr int    dropoutRunLength   = 5;
    static constexpr double jumpStepFactor     = 3.0;
    static constexpr double levelWindowSeconds = 0.010;
    static constexpr double levelDropDb        = 6.0;
    static constexpr double mergeSeconds       = 0.050;

    // --- What it takes to also catch jumps the step rule cannot see -----------
    /// A sample whose recurrence residual exceeds this fraction of the amplitude
    /// is a jump (-46 dB; a 16-bit sine stays below -70 dB of a -6 dB sine).
    static constexpr double residualRelative = 0.005;
    /// ... but never less than this, so a quiet sine's own quantisation noise
    /// (at most 2 LSB of 16 bit = 6e-5) cannot trip it.
    static constexpr double residualFloor = 2.0e-4;
    /// A sine quieter than this (-50 dBFS) is too close to the noise to lock on.
    static constexpr double lockMinAmplitude = 0.003;
    /// While locking, the amplitude may wander this far from its running mean.
    static constexpr double lockAmplitudeTolerance = 0.10;
    /// The measured pitch must be within this fraction of `sineHz` to lock.
    static constexpr double lockPitchTolerance = 0.02;

    enum Priority { priorityJump = 1, priorityLevel = 2, priorityDropout = 3 };

    static double residualLimitFor (double amplitude) noexcept
    {
        return std::max (residualRelative * amplitude, residualFloor);
    }

    void step (float raw) noexcept
    {
        const bool finite = std::isfinite (raw);
        const double x = finite ? (double) raw : 0.0;   // a NaN must not poison the history
        const std::int64_t n = numProcessed++;

        // 10 ms sliding window of x^2, refreshed exactly once per lap so the
        // running sum cannot drift over an hour.
        {
            const double square = x * x;
            windowSum += square - windowSquares[windowPos];
            windowSquares[windowPos] = square;

            if (++windowPos == windowSquares.size())
            {
                windowPos = 0;
                windowSum = std::accumulate (windowSquares.begin(), windowSquares.end(), 0.0);
            }
        }

        allSumSquares += x * x;

        // Zeros are counted from the start so a run is measured from its first sample.
        zeroRun = std::abs (x) < silenceThreshold ? zeroRun + 1 : 0;

        // What the sine's own recurrence says this sample should have been.
        const bool haveHistory = n >= 2;
        const double residual = haveHistory ? x - recurrenceCoeff * x1 + x2 : 0.0;

        if (locked)
        {
            lockedSumSquares += x * x;
            ++lockedCount;

            if (! finite)
                record (priorityJump, n, n);

            if (zeroRun >= dropoutRunLength)
                record (priorityDropout, n - zeroRun + 1, n);

            if (n >= 1 && std::abs (x - x1) > stepLimit)
                record (priorityJump, n, n);

            if (haveHistory && std::abs (residual) > residualLimit)
                record (priorityJump, n - 1, n);

            // The window first crosses the line when about 3/4 of it lies inside a
            // drop to silence (a bit more for a shallower dip), so the drop began
            // about 3/4 of a window ago, not at the window's oldest sample.
            if (windowSum < levelLimitMeanSquare * (double) windowLength)
                record (priorityLevel, n - (windowLength * 3) / 4 + 1, n);
        }
        else if (haveHistory)
        {
            acquire (x, finite, residual);
        }

        x2 = x1;
        x1 = x;
    }

    /// One more sample of the search for 100 ms of steady sine.
    void acquire (double x, bool finite, double residual) noexcept
    {
        // For x[k] = A sin (w k + p): x1 = A sin t and (x - x2) / (2 sin w) = A cos t
        // with t the phase at the previous sample, so their norm is A at any phase.
        const double quadrature = (x - x2) / twoSinOmega;
        const double amplitude = std::sqrt (x1 * x1 + quadrature * quadrature);

        bool steady = finite && amplitude >= lockMinAmplitude
                        && std::abs (residual) <= residualLimitFor (amplitude);

        if (steady && runLength > 0)
        {
            const double mean = runSumAmplitude / (double) runLength;
            steady = std::abs (amplitude - mean) <= lockAmplitudeTolerance * mean;
        }

        if (! steady)
        {
            resetRun();
            return;
        }

        ++runLength;
        runSumAmplitude += amplitude;
        runSumSquares   += x * x;
        runSumRecurrence += x1 * (x + x2);
        runSumPrevSquares += x1 * x1;

        if (runLength >= lockSamples)
            tryLock();
    }

    void tryLock() noexcept
    {
        // Least-squares fit of x[n] + x[n-2] = 2 cos (w) x[n-1] over the whole run.
        // A constant (or a pitch that happens to satisfy the per-sample residual
        // test, like 100 Hz against 440 Hz) is turned away here.
        const double cosine = std::clamp (runSumRecurrence / (2.0 * std::max (runSumPrevSquares, 1.0e-30)),
                                          -1.0, 1.0);
        const double measuredHz = std::acos (cosine) * sr / (2.0 * juce::MathConstants<double>::pi);

        if (std::abs (measuredHz - sineFrequency) > lockPitchTolerance * sineFrequency)
        {
            resetRun();
            return;
        }

        const double amplitude = runSumAmplitude / (double) runLength;

        locked = true;
        stepLimit = jumpStepFactor * omega * amplitude;
        residualLimit = residualLimitFor (amplitude);
        levelLimitMeanSquare = (runSumSquares / (double) runLength) * levelDropPower;
        lockedSumSquares = runSumSquares;
        lockedCount = runLength;
    }

    void resetRun() noexcept
    {
        runLength = 0;
        runSumAmplitude = runSumSquares = runSumRecurrence = runSumPrevSquares = 0.0;
    }

    /// Notes a glitch that begins at sample `start`, seen while processing sample
    /// `now`. Within 50 ms of the previous glitch it joins it instead.
    void record (Priority priority, std::int64_t start, std::int64_t now) noexcept
    {
        if (! glitches.empty() && start - lastActivity < mergeSamples)
        {
            auto& glitch = glitches.back();
            clusterStart = std::min (clusterStart, start);
            lastActivity = std::max (lastActivity, now);

            if (priority > clusterPriority)
            {
                clusterPriority = priority;
                glitch.kind = kindName (priority);
            }

            glitch.timeSeconds = (double) clusterStart / sr;
            glitch.durationSeconds = (double) (lastActivity - clusterStart + 1) / sr;
            return;
        }

        clusterStart = start;
        lastActivity = now;
        clusterPriority = priority;

        Glitch glitch;
        glitch.timeSeconds = (double) start / sr;
        glitch.kind = kindName (priority);
        glitch.durationSeconds = (double) (now - start + 1) / sr;
        glitches.push_back (std::move (glitch));
    }

    static const char* kindName (int priority) noexcept
    {
        return priority == priorityDropout ? "dropout" : priority == priorityLevel ? "level" : "jump";
    }

    // Fixed by the constructor
    const double sr, sineFrequency, omega, recurrenceCoeff, twoSinOmega;
    const std::int64_t lockSamples, windowLength, mergeSamples;
    const double levelDropPower;

    // History
    std::int64_t numProcessed = 0;
    double x1 = 0.0, x2 = 0.0;
    int zeroRun = 0;
    double allSumSquares = 0.0;

    // 10 ms window
    std::vector<double> windowSquares;
    size_t windowPos = 0;
    double windowSum = 0.0;

    // Searching for the sine
    std::int64_t runLength = 0;
    double runSumAmplitude = 0.0, runSumSquares = 0.0, runSumRecurrence = 0.0, runSumPrevSquares = 0.0;

    // Locked on the sine
    bool locked = false;
    double stepLimit = 0.0, residualLimit = 0.0, levelLimitMeanSquare = 0.0;
    double lockedSumSquares = 0.0;
    std::int64_t lockedCount = 0;

    // Glitches
    std::vector<Glitch> glitches;
    std::int64_t clusterStart = 0, lastActivity = 0;
    int clusterPriority = 0;
};
