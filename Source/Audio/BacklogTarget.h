#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>
#include <utility>

/// Ring backlog target for the cable render thread: grows at once to cover a
/// larger endpoint request, shrinks back slowly after a quiet stretch so one
/// burst does not cost latency for the rest of the session. Pure; no JUCE.
///
/// The target is expressed in device frames and is what the drift-compensated
/// reader holds in the ring (converted to producer frames by the caller). It
/// never falls below one period or one endpoint buffer request, whichever is
/// larger: a late event can ask for the whole endpoint buffer, and a ring
/// sized for one period only would underrun and re-prime into a dropout.
class BacklogTarget
{
public:
    /// Fixed history capacity. At 48 kHz with a 3 ms period the five-second
    /// window needs about 1700 entries; the shortest exclusive periods need
    /// more, so keep room. A longer requested window is clamped to this size
    /// (the window then holds the most recent events only).
    static constexpr int windowCapacity = 8192;

    /// @param deviceRate           The endpoint's stream rate, in Hz.
    /// @param periodFrames         Frames the endpoint asks for in one event.
    /// @param producerBlockFrames  One engine block, in producer frames.
    /// @param marginMs             Jitter margin added above the covered request.
    /// @param shrinkAfterSeconds   How long a large request keeps the target up.
    /// @param producerRate         The engine's rate, in Hz. The block is kept
    ///                             in producer frames and converted to device
    ///                             frames when the target is reported; 0 uses
    ///                             the device rate (a ratio of one).
    /// @param maxRequestFrames     Largest request one render event can make,
    ///                             normally the endpoint buffer size. A late
    ///                             (but still running) event may ask for the
    ///                             whole buffer, and a target below it would
    ///                             make the reader underrun and re-prime into a
    ///                             long dropout. 0 keeps `periodFrames`.
    void prepare (double deviceRate, int periodFrames, int producerBlockFrames,
                  double marginMs = 1.5, double shrinkAfterSeconds = 5.0,
                  double producerRate = 0.0, int maxRequestFrames = 0) noexcept
    {
        rate   = deviceRate > 1.0 ? deviceRate : 1.0;
        period = std::max (1, periodFrames);
        floor  = std::max (period, maxRequestFrames);
        block  = std::max (0, producerBlockFrames);
        producerRate_ = producerRate > 1.0 ? producerRate : 0.0;
        margin = std::max (0, (int) std::lround (rate * marginMs / 1000.0));

        // The window is a count of render events, one per device period.
        const double eventsPerSecond = rate / (double) period;
        window = std::clamp ((int) std::lround (shrinkAfterSeconds * eventsPerSecond),
                             1, windowCapacity);

        // One period per second: at exactly one event per period that is
        // period / eventsPerSecond frames per event.
        dropPerEvent = (double) period / eventsPerSecond;
        dropBudget   = dropPerEvent;

        // Start with a full window of the floor request. onRequest() then
        // overwrites the oldest entry, so a burst leaves after the window.
        history.fill (floor);
        writeIndex = 0;
        windowMax  = floor;
        covered    = (double) floor;
    }

    /// Call once per render event with the frames the endpoint requested.
    /// Real-time safe: fixed storage, no allocation, no locking.
    void onRequest (int framesRequested) noexcept
    {
        // The floor is the largest request one event can make (the endpoint
        // buffer) or one period, whichever is larger. A smaller request is not
        // a smaller latency: the backlog must still be able to answer a full
        // buffer request after a stall without underrunning.
        const int request = std::max (floor, framesRequested);

        const int outgoing = history[(size_t) writeIndex];
        history[(size_t) writeIndex] = request;
        writeIndex = (writeIndex + 1) % window;

        if (request >= windowMax)
            windowMax = request;
        else if (outgoing == windowMax)
        {
            // The largest request just left the window. Rescan the (fixed)
            // history once; this happens at most once per burst.
            windowMax = period;

            for (int value : history)
                windowMax = std::max (windowMax, value);
        }

        if (request > covered)
        {
            // A larger request is covered immediately, before the next event.
            covered = (double) request;
            return;
        }

        // Shrink toward the largest request still in the window, but never
        // faster than one period per second. Replenishing the budget only up
        // to one event's share keeps that bound strict even after a long idle
        // stretch.
        dropBudget = std::min (dropPerEvent, dropBudget + dropPerEvent);

        if (windowMax < covered)
        {
            const double step = std::min ((double) (covered - windowMax), dropBudget);
            covered    -= step;
            dropBudget -= step;
        }
    }

    /// Target in device frames: covered request + margin + one producer block.
    /// The block is converted here, not by the caller: the caller converts the
    /// whole target to producer frames afterwards, and scaling the block twice
    /// would size it by the ratio squared.
    int getTargetFrames() const noexcept
    {
        return (int) std::lround (covered) + margin + getBlockInDeviceFrames();
    }

    /// The engine's block size, or its rate, can change while the stream stays
    /// open; the request history and the one-period-per-second shrink limit
    /// stay put.
    void setProducerFormat (int blockFrames, double producerRate) noexcept
    {
        block = std::max (0, blockFrames);
        producerRate_ = producerRate > 1.0 ? producerRate : 0.0;
    }

    /// One engine block in device frames, i.e. after the producer/device ratio
    /// conversion. The caller adds it to the target in producer frames, which
    /// is exactly the un-scaled block the old code used.
    int getBlockInDeviceFrames() const noexcept
    {
        if (producerRate_ <= 1.0)
            return block;

        return (int) std::lround ((double) block * rate / producerRate_);
    }

private:
    double rate         = 48000.0;
    int    period       = 1;
    int    floor        = 1;
    int    block        = 0;
    int    margin       = 0;
    int    window       = 1;
    double dropPerEvent = 0.0;
    double dropBudget   = 0.0;
    // Zero means the producer and device rates are the same, so the block
    // needs no conversion.
    double producerRate_ = 0.0;

    std::array<int, windowCapacity> history {};
    int    writeIndex = 0;
    int    windowMax  = 1;
    double covered    = 1.0;
};

// The render thread calls onRequest() inside its loop: fixed-size storage,
// trivially copyable, and no allocation can happen there.
static_assert (std::is_trivially_copyable_v<BacklogTarget>,
               "BacklogTarget must stay trivially copyable and allocation-free");
static_assert (noexcept (std::declval<BacklogTarget&>().onRequest (0)),
               "BacklogTarget::onRequest must be noexcept");
