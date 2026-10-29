#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <utility>

/// Chooses the IAudioClient3 period: the smallest one this machine sustains.
/// Pure, no JUCE, no allocation.
///
/// The low-latency engine reports a minimum period, a fundamental step and a
/// maximum. Every period the endpoint accepts is min + k * fundamental, so a
/// hint remembered from the last run has to be rounded up to that grid. A
/// period below the minimum would be refused by the endpoint, not give a
/// smaller latency.
struct PeriodPolicy
{
    /// The endpoint only accepts min + k * fundamental. A maximum that is not
    /// on that grid (the engine may report one) is not a period the endpoint
    /// will take, so align it down before it is used as a bound.
    static int alignedMax (int minFrames, int fundamentalFrames, int maxFrames) noexcept
    {
        const long long steps = ((long long) maxFrames - minFrames) / fundamentalFrames;
        return (int) ((long long) minFrames + steps * fundamentalFrames);
    }

    /// Returns the smallest allowed period >= hintFrames (0 = no hint -> min).
    static int initialPeriod (int minFrames, int fundamentalFrames, int maxFrames,
                              int hintFrames) noexcept
    {
        // A nonsensical range cannot be navigated; the minimum is the only
        // safe answer and avoids a division by a zero fundamental.
        if (minFrames < 1 || maxFrames < minFrames || fundamentalFrames <= 0)
            return std::max (1, minFrames);

        const int highest = alignedMax (minFrames, fundamentalFrames, maxFrames);

        if (hintFrames <= minFrames)
            return minFrames;

        if (hintFrames >= highest)
            return highest;

        const long long span  = (long long) hintFrames - minFrames;
        const long long steps = (span + fundamentalFrames - 1) / fundamentalFrames;
        const long long period = (long long) minFrames + steps * fundamentalFrames;

        return (int) std::min<long long> (period, highest);
    }

    /// The next larger allowed period, or `current` when already at max.
    static int nextLarger (int current, int minFrames, int fundamentalFrames,
                           int maxFrames) noexcept
    {
        if (minFrames < 1 || maxFrames < minFrames || fundamentalFrames <= 0)
            return std::max (1, minFrames);

        const int highest = alignedMax (minFrames, fundamentalFrames, maxFrames);

        if (current >= highest)
            return current;

        if (current < minFrames)
            return minFrames;

        const long long steps = ((long long) current - minFrames) / fundamentalFrames + 1;
        const long long period = (long long) minFrames + steps * fundamentalFrames;

        return (int) std::min<long long> (period, highest);
    }

    /// Smallest allowed period whose buffer would cover `gapMs` with 20 %
    /// headroom, given the current stream's buffer/period ratio (e.g.
    /// 280/128 = 2.19). Never smaller than nextLarger(current): an escalation
    /// only goes up, even when the measured gap would have fit the current
    /// period. Returns the aligned maximum when nothing smaller covers the
    /// gap; the caller then falls back to the standard shared rung if a gap
    /// beyond the maximum period's coverage recurs.
    static int periodToCover (double gapMs, double sampleRate, double bufferToPeriodRatio,
                              int current, int minFrames, int fundamentalFrames,
                              int maxFrames) noexcept
    {
        const int next = nextLarger (current, minFrames, fundamentalFrames, maxFrames);

        // Bad evidence gets the old one-step escalation: guessing a period
        // from a zero rate or a zero ratio could jump straight to the maximum
        // and stay there for the session.
        if (! (gapMs > 0.0) || ! (sampleRate > 0.0) || ! (bufferToPeriodRatio > 0.0))
            return next;

        // The buffer needs to hold the whole wake-up plus 20 %: a period that
        // barely covers the observed gap glitches again on the next, slightly
        // longer stall.
        const double requiredPeriod = gapMs * 1.2 * sampleRate
                                        / (1000.0 * bufferToPeriodRatio);

        // NaN and infinity fail this comparison and fall back to one step.
        if (! (requiredPeriod > 0.0))
            return next;

        // A huge gap should reach the maximum, not overflow the int cast.
        const double capped = requiredPeriod > 1.0e9 ? 1.0e9 : requiredPeriod;

        const int coverage = initialPeriod (minFrames, fundamentalFrames, maxFrames,
                                            (int) std::ceil (capped));

        return std::max (coverage, next);
    }

    /// One escalation decision: the low-latency period to ask for, or 0 when
    /// even the aligned maximum has no room left above `current`. The caller
    /// reads 0 as "leave the low-latency rung for the standard shared one",
    /// which is also the persisted -1 hint.
    static int escalationTarget (double gapMs, double sampleRate, double bufferToPeriodRatio,
                                 int current, int minFrames, int fundamentalFrames,
                                 int maxFrames) noexcept
    {
        const int covered = periodToCover (gapMs, sampleRate, bufferToPeriodRatio,
                                           current, minFrames, fundamentalFrames, maxFrames);

        return covered > current ? covered : 0;
    }

    /// Escalate when the endpoint starved at least `threshold` times within
    /// `windowSeconds`. One starvation is jitter; two inside a minute point at
    /// a wake-up that is consistently too late for this period.
    static constexpr int    threshold    = 2;
    static constexpr double windowSeconds = 60.0;

    /// A freshly started stream primes its endpoint, so an early empty padding
    /// says nothing about scheduling. Starvation during this grace is ignored.
    static constexpr double startupGraceSeconds = 1.0;
};

/// The counting part of the policy: a fixed-capacity ring of
/// QueryPerformanceCounter timestamps, each optionally carrying a magnitude
/// (the duration of a risky gap). Real-time safe by construction - no
/// allocation and no locking in record() - and pure so the decision can be
/// tested without a device. The same thresholds serve starvation and risky
/// wake-up gaps: both mean this machine cannot sustain the current period.
class EscalationWindow
{
public:
    /// Threshold is two, so the ring only needs to hold enough events to let
    /// the window expire. A little slack covers clock jitter around the edge.
    static constexpr int capacity = 16;

    /// Start a fresh window. `startSeconds` is on the same monotonic clock as
    /// record(); events during the first second after it are ignored.
    void reset (double startSeconds) noexcept
    {
        startTime = startSeconds;
        count_    = 0;
        writeIndex = 0;
    }

    /// Record one event and report whether the threshold has been reached
    /// inside the window. Returns false during the start-up grace and does not
    /// store those events. `magnitude` is what largestWithin() reports for
    /// this event; the starvation path leaves it at 0.
    bool record (double nowSeconds, double magnitude = 0.0) noexcept
    {
        if (! (nowSeconds - startTime >= PeriodPolicy::startupGraceSeconds))
            return false;

        times[(size_t) writeIndex]      = nowSeconds;
        magnitudes[(size_t) writeIndex] = magnitude;
        writeIndex = (writeIndex + 1) % capacity;

        if (count_ < capacity)
            ++count_;

        return countWithin (nowSeconds) >= PeriodPolicy::threshold;
    }

    /// How many stored events lie no more than `windowSeconds` before now.
    int countWithin (double nowSeconds) const noexcept
    {
        int inWindow = 0;

        for (int i = 0; i < count_; ++i)
            if (nowSeconds - times[(size_t) i] <= PeriodPolicy::windowSeconds)
                ++inWindow;

        return inWindow;
    }

    /// The largest magnitude among the events still inside the window. The
    /// escalation picks its period from this, not from the event that
    /// happened to push the count over the threshold: one 17 ms stall says
    /// more about the needed buffer than the 6 ms one after it.
    double largestWithin (double nowSeconds) const noexcept
    {
        double largest = 0.0;

        for (int i = 0; i < count_; ++i)
            if (nowSeconds - times[(size_t) i] <= PeriodPolicy::windowSeconds)
                largest = std::max (largest, magnitudes[(size_t) i]);

        return largest;
    }

    /// Stored events, including any that have already left the window. Only
    /// the tests need this; the decision above always uses countWithin().
    int count() const noexcept { return count_; }

private:
    std::array<double, capacity> times {};
    std::array<double, capacity> magnitudes {};
    int    writeIndex = 0;
    int    count_     = 0;
    double startTime  = 0.0;
};

// The render thread calls record() once per event: it has to stay free of
// allocation and locks, like every other object the loop touches.
static_assert (std::is_trivially_copyable_v<EscalationWindow>,
               "EscalationWindow must stay trivially copyable and allocation-free");
static_assert (noexcept (std::declval<EscalationWindow&>().record (0.0, 0.0)),
               "EscalationWindow::record must be noexcept");
static_assert (noexcept (PeriodPolicy::initialPeriod (0, 0, 0, 0)),
               "PeriodPolicy::initialPeriod must be noexcept");
