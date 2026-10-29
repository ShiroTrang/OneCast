#pragma once
#include <algorithm>
#include <cmath>

/// How much of a shared render event's free space to release, and whether the
/// shortfall is real. Pure; no JUCE, no allocation.
///
/// The render loop used to fill the whole free space every event. After a
/// stall that asked for a whole buffer, the ring had to hold the whole buffer
/// too, which is where half of the escalated latency came from. Releasing only
/// what the ring actually produced keeps the endpoint alive without making the
/// ring hold a full buffer, but only while the endpoint still holds enough to
/// reach the next event. Below that the input has really stopped and the
/// classic fade-and-reprime path has to run, or the endpoint runs dry and
/// feeds the starvation escalation.
struct RenderFill
{
    struct Decision
    {
        int  frames;
        bool fallbackToUnderrun;
    };

    /// @param free      Frames the endpoint is not already holding.
    /// @param padding   Frames the endpoint still holds at this event, before
    ///                  the write.
    /// @param produced  Frames the reader could produce (free while priming).
    /// @param keepAlive One period plus 1 ms: what the endpoint must still hold
    ///                  after the write to reach the next event.
    /// @param priming   The reader is priming: today's path fills the whole
    ///                  free space with audio or silence.
    static Decision framesToRelease (int free, int padding, int produced,
                                     int keepAlive, bool priming) noexcept
    {
        if (priming)
            return { free, false };

        const int released = std::clamp (produced, 0, std::max (0, free));

        if ((long long) padding + released >= (long long) keepAlive)
            return { released, false };

        // Not enough to survive to the next event: hand the event back to the
        // underrun path, which fills the whole free space with the fade and
        // silence and re-primes the reader.
        return { free, true };
    }

    /// A wake-up gap is audible when it outlasts what the endpoint held after
    /// the last write, not when it outlasts the buffer. After a short write
    /// the endpoint holds less than a full buffer, and a gap the full buffer
    /// covered can glitch silently. Half a millisecond of margin covers an
    /// event that arrives just as the buffer runs dry.
    static bool gapIsRisky (double gapMs, int paddingAfterLastWrite,
                            double deviceRate) noexcept
    {
        if (! (deviceRate > 0.0))
            return false;

        const double coverMs = 1000.0 * (double) paddingAfterLastWrite / deviceRate - 0.5;

        return gapMs > coverMs;
    }
};
