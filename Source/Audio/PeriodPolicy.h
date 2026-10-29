#pragma once
#include <algorithm>
#include <array>
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
    /// Returns the smallest allowed period >= hintFrames (0 = no hint -> min).
    static int initialPeriod (int minFrames, int fundamentalFrames, int maxFrames,
                              int hintFrames) noexcept
    {
        // A nonsensical range cannot be navigated; the minimum is the only
        // safe answer and avoids a division by a zero fundamental.
        if (minFrames < 1 || maxFrames < minFrames || fundamentalFrames <= 0)
            return std::max (1, minFrames);

        if (hintFrames <= minFrames)
            return minFrames;

        if (hintFrames >= maxFrames)
            return maxFrames;

        const long long span  = (long long) hintFrames - minFrames;
        const long long steps = (span + fundamentalFrames - 1) / fundamentalFrames;
        const long long period = (long long) minFrames + steps * fundamentalFrames;

        return (int) std::min<long long> (period, maxFrames);
    }

    /// The next larger allowed period, or `current` when already at max.
    static int nextLarger (int current, int minFrames, int fundamentalFrames,
                           int maxFrames) noexcept
    {
        if (minFrames < 1 || maxFrames < minFrames || fundamentalFrames <= 0)
            return std::max (1, minFrames);

        if (current >= maxFrames)
            return current;

        if (current < minFrames)
            return minFrames;

        const long long steps = ((long long) current - minFrames) / fundamentalFrames + 1;
        const long long period = (long long) minFrames + steps * fundamentalFrames;

        return (int) std::min<long long> (period, maxFrames);
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

/// The starvation part of the policy: a fixed-capacity ring of QueryPerformance
/// Counter timestamps. Real-time safe by construction - no allocation and no
/// locking in record() - and pure so the decision can be tested without a
/// device.
class StarvationWindow
{
public:
    /// Threshold is two, so the ring only needs to hold enough events to let
    /// the window expire. A little slack covers clock jitter around the edge.
    static constexpr int capacity = 16;

    /// Start a fresh window. `startSeconds` is on the same monotonic clock as
    /// record(); starvation during the first second after it is ignored.
    void reset (double startSeconds) noexcept
    {
        startTime = startSeconds;
        count_    = 0;
        writeIndex = 0;
    }

    /// Record one starvation and report whether the threshold has been reached
    /// inside the window. Returns false during the start-up grace and does not
    /// store those events.
    bool record (double nowSeconds) noexcept
    {
        if (! (nowSeconds - startTime >= PeriodPolicy::startupGraceSeconds))
            return false;

        times[(size_t) writeIndex] = nowSeconds;
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

    /// Stored events, including any that have already left the window. Only
    /// the tests need this; the decision above always uses countWithin().
    int count() const noexcept { return count_; }

private:
    std::array<double, capacity> times {};
    int    writeIndex = 0;
    int    count_     = 0;
    double startTime  = 0.0;
};

// The render thread calls record() once per starvation: it has to stay free of
// allocation and locks, like every other object the loop touches.
static_assert (std::is_trivially_copyable_v<StarvationWindow>,
               "StarvationWindow must stay trivially copyable and allocation-free");
static_assert (noexcept (std::declval<StarvationWindow&>().record (0.0)),
               "StarvationWindow::record must be noexcept");
static_assert (noexcept (PeriodPolicy::initialPeriod (0, 0, 0, 0)),
               "PeriodPolicy::initialPeriod must be noexcept");
