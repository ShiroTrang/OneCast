#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

/// When to give up on the audio device, and when to try opening it again.
///
/// Unplugging and replugging the interface is how the user clears its
/// occasional crackle, so the app has to survive the device going away while
/// it runs and pick it up again when it is back, without a restart. This
/// class holds the timing rules; MainComponent feeds it what it sees (its
/// 30 Hz timer, the Windows endpoint notifications) and does the closing and
/// reopening. Pure logic for the message thread, so it is unit-tested.
///
/// Lost: a device that was running is closed under us, or stops calling back
/// for stallMs (an unplugged ASIO interface usually just goes quiet). Only a
/// device seen running counts: one the user picks that never starts is not
/// "recovered" behind their back.
///
/// Reopen: ASIO drivers are not in the Windows notifications, but the same
/// USB interface's Windows endpoints are. When endpoints went away with the
/// device (an unplug), nothing is tried until something comes back; a driver
/// that stopped by itself is tried again straight away. A USB connect sends
/// several notifications in a burst and the ASIO driver can need a moment
/// after them, so a try waits until they have been quiet for settleMs, and a
/// failed try is repeated a few times, further apart.
class DeviceRecovery
{
public:
    /// No callback for this long (and for this many timer ticks, so a message
    /// thread that was blocked is not mistaken for a stalled device).
    static constexpr double stallMs    = 1000.0;
    static constexpr int    stallTicks = 15;

    /// Quiet time after the last endpoint notification before a try.
    static constexpr double settleMs = 1500.0;

    /// An endpoint that went away this close before the loss means an unplug.
    /// Generous: the M-Track's driver kept calling back for ~6 s after its
    /// endpoints went away, and only then asked for a reset.
    static constexpr double unplugWindowMs = 15000.0;

    /// Waits after each failed try; then nothing until the next arrival.
    static constexpr double retryDelaysMs[] = { 2000.0, 3000.0, 5000.0, 8000.0 };
    static constexpr int numRetries = (int) (sizeof (retryDelaysMs) / sizeof (retryDelaysMs[0]));

    /// Without endpoint notifications a lost device is tried this often.
    static constexpr double pollMs = 5000.0;

    //==========================================================================
    // Watching the running device

    /// Once per timer tick while not lost. True when a device that was
    /// running has stopped: it was closed, or no callback came for stallMs.
    bool deviceStopped (bool deviceOpen, std::uint32_t callbackCount, double nowMs) noexcept
    {
        if (! deviceOpen)
        {
            const bool wasRunning = running;
            forgetHistory();
            return wasRunning;
        }

        if (! primed || callbackCount != lastCount)
        {
            running = running || primed;   // moved since the first look
            primed = true;
            lastCount = callbackCount;
            lastMoveMs = nowMs;
            stillTicks = 0;
            return false;
        }

        if (! running)
            return false;

        if (++stillTicks >= stallTicks && nowMs - lastMoveMs >= stallMs)
        {
            forgetHistory();
            return true;
        }

        return false;
    }

    /// A device was (re)opened - by a reconnect or by the user: stop waiting
    /// for the old one, and watch this one afresh.
    void watchNewDevice() noexcept
    {
        forgetHistory();
        lost = false;
        nextAttemptMs = never;
        arrivalPending = false;
    }

    //==========================================================================
    // Windows endpoint notifications

    void endpointDeparted (double nowMs) noexcept
    {
        lastDepartureMs = nowMs;

        // Reported just after the loss was noticed: still an unplug, so wait
        // for the interface rather than knocking on a driver with no hardware.
        if (lost && lastArrivalMs < lostAtMs && nowMs - lostAtMs <= unplugWindowMs)
            nextAttemptMs = never;
    }

    void endpointArrived (double nowMs) noexcept
    {
        lastArrivalMs = nowMs;
        arrivalPending = true;

        if (lost)
        {
            nextAttemptMs = nowMs + settleMs;   // each one of a burst pushes it back
            retriesDone = 0;
        }
    }

    /// True once per burst of arrivals, settleMs after its last one.
    bool takeSettledArrival (double nowMs) noexcept
    {
        if (! arrivalPending || nowMs - lastArrivalMs < settleMs)
            return false;

        arrivalPending = false;
        return true;
    }

    /// False when the notifications could not be registered: then a lost
    /// device is retried every pollMs instead of on arrivals.
    void setNotificationsAvailable (bool available) noexcept   { notifications = available; }

    //==========================================================================
    // The lost device

    void deviceLost (double nowMs) noexcept
    {
        lost = true;
        lostAtMs = nowMs;
        retriesDone = 0;

        const bool unplugged = nowMs - lastDepartureMs <= unplugWindowMs
                               && lastArrivalMs < lastDepartureMs;

        nextAttemptMs = unplugged ? never
                                  : std::max (nowMs, lastArrivalMs) + settleMs;
    }

    bool isLost() const noexcept                       { return lost; }
    bool isReopenDue (double nowMs) const noexcept     { return lost && nowMs >= nextAttemptMs; }

    /// A try is starting: no other is due until it has failed. Opening an
    /// ASIO driver can run the message loop, so the timer may tick again
    /// while the try is still inside the driver.
    void reopenStarted() noexcept                      { nextAttemptMs = never; }

    void reopenFailed (double nowMs) noexcept
    {
        if (retriesDone < numRetries)
            nextAttemptMs = nowMs + retryDelaysMs[retriesDone++];
        else
            nextAttemptMs = notifications ? never : nowMs + pollMs;
    }

private:
    static constexpr double never = std::numeric_limits<double>::infinity();

    void forgetHistory() noexcept
    {
        primed = false;
        running = false;
        stillTicks = 0;
    }

    // Watchdog
    bool primed = false, running = false;
    std::uint32_t lastCount = 0;
    double lastMoveMs = 0.0;
    int stillTicks = 0;

    // Notifications
    bool notifications = true;
    double lastDepartureMs = -never, lastArrivalMs = -never;
    bool arrivalPending = false;

    // Lost device
    bool lost = false;
    double lostAtMs = 0.0;
    double nextAttemptMs = never;
    int retriesDone = 0;
};
