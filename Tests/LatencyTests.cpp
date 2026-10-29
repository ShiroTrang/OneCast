// Unit tests for the output-side latency pieces: the shrinking backlog target
// and the guarantees that keep it real-time safe. No audio device is opened.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe latency

#include <JuceHeader.h>

#include "../Source/Audio/BacklogTarget.h"
#include "../Source/Audio/PeriodPolicy.h"
#include "../Source/Audio/RingBuffer.h"
#include "../Source/Audio/SharedStreamFormat.h"
#include "../Source/Audio/WasapiOutputDevice.h"

#include <cmath>
#include <cstdio>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    int expectedMargin (double deviceRate, double marginMs)
    {
        return (int) std::lround (deviceRate * marginMs / 1000.0);
    }
}

class LatencyTests final : public juce::UnitTest
{
public:
    LatencyTests() : juce::UnitTest ("Latency", "latency") {}

    void runTest() override
    {
        constexpr double rate = 48000.0;
        constexpr int period = 480;
        constexpr int block = 128;
        const int margin = expectedMargin (rate, 1.5);

        beginTest ("backlog grows at once");
        {
            BacklogTarget target;
            target.prepare (rate, period, block);

            target.onRequest (period);
            const int before = target.getTargetFrames();
            target.onRequest (960);
            const int after = target.getTargetFrames();

            std::printf ("    [measured] grow: %d -> %d frames (margin %d)\n",
                         before, after, margin);

            expectEquals (after, 960 + margin + block);
            expect (after > before);
        }

        beginTest ("backlog shrinks after a quiet stretch");
        {
            // Six periods, not one: a one-period burst sits exactly on the
            // floor after the first request and would pass an instant snap.
            constexpr int burst = 6 * period;
            constexpr int eventsPerSecond = 100;

            BacklogTarget target;
            target.prepare (rate, period, block);

            // Record the target after every event, so the check below guards
            // every one-second span and not just the sampled boundaries.
            std::vector<int> targets;
            targets.reserve (1201);

            target.onRequest (burst);
            targets.push_back (target.getTargetFrames());
            const int peak = targets.back();

            for (int i = 0; i < 1200; ++i)
            {
                target.onRequest (period);
                targets.push_back (target.getTargetFrames());
            }

            int worstDropPerSecond = 0;
            for (size_t i = eventsPerSecond; i < targets.size(); ++i)
                worstDropPerSecond = juce::jmax (worstDropPerSecond,
                                                 targets[i - eventsPerSecond] - targets[i]);

            const int expected = period + margin + block;
            std::printf ("    [measured] shrink: %d -> %d frames, peak %d, worst drop %d/s\n",
                         peak, targets.back(), peak, worstDropPerSecond);

            expect (peak >= burst + margin + block);
            expect (worstDropPerSecond <= period,
                    "the target dropped more than one period in any one-second span");
            expectWithinAbsoluteError ((float) targets.back(), (float) expected, 1.0f);
        }

        beginTest ("producer block is converted once");
        {
            // 44.1 kHz endpoint, 48 kHz engine: one 128-frame engine block is
            // 117.6 endpoint frames. The render loop then converts the whole
            // device target back to producer frames, where the block must be
            // exactly 128 again, not 128 scaled twice by the ratio.
            constexpr double blockRate = 48000.0;
            constexpr double deviceRate = 44100.0;
            constexpr int devicePeriod = 441;

            const int deviceMargin = expectedMargin (deviceRate, 1.5);
            const int expectedDeviceBlock = (int) std::lround (block * deviceRate / blockRate);

            BacklogTarget target;
            target.prepare (deviceRate, devicePeriod, block, 1.5, 5.0, blockRate);
            target.onRequest (devicePeriod);

            const int deviceTarget = target.getTargetFrames();
            const int producerTarget = (int) std::lround (deviceTarget * blockRate / deviceRate);

            std::printf ("    [measured] ratio: device target %d, producer target %d\n",
                         deviceTarget, producerTarget);

            expectEquals (deviceTarget, devicePeriod + deviceMargin + expectedDeviceBlock);
            expectEquals (producerTarget,
                          (int) std::lround ((devicePeriod + deviceMargin) * blockRate / deviceRate)
                              + block);
        }

        beginTest ("low-latency rung keeps the endpoint mix format");
        {
            // A 44.1 kHz producer against a 48 kHz endpoint must still get rung
            // 2: that stream keeps the endpoint's mix rate and format, with no
            // AUTOCONVERTPCM, and the reader's nominal ratio converts.
            const auto lowLatency = chooseSharedStreamFormat (true, 48000, 44100);

            expectEquals (lowLatency.rate, 48000);
            expect (! lowLatency.convert,
                    "rung 2 must not ask WASAPI to convert the sample rate");

            // The legacy shared rung keeps the previous behaviour.
            const auto legacy = chooseSharedStreamFormat (false, 48000, 44100);

            expectEquals (legacy.rate, 44100);
            expect (legacy.convert,
                    "rung 3 keeps AUTOCONVERTPCM for a rate mismatch");

            // Matching rates never need conversion.
            expectEquals (chooseSharedStreamFormat (true, 48000, 48000).rate, 48000);
            expectEquals (chooseSharedStreamFormat (false, 48000, 48000).rate, 48000);
            expect (! chooseSharedStreamFormat (false, 48000, 48000).convert);
        }

        beginTest ("period policy: initial period rounds up to an allowed step");
        {
            // The VB cable is expected to report a 128-frame minimum and a
            // 16-frame grid here; the real numbers come from
            // GetSharedModeEnginePeriod at run time. A hint between two steps
            // must choose the larger one: a smaller period than the endpoint
            // offers is not an option.
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 480, 0), 128);
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 480, 100), 128);
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 480, 128), 128);
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 480, 129), 144);
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 480, 160), 160);
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 480, 479), 480);
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 480, 480), 480);
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 480, 10000), 480);

            // Degenerate ranges keep the minimum and must not divide by zero.
            expectEquals (PeriodPolicy::initialPeriod (128, 0, 480, 200), 128);
            expectEquals (PeriodPolicy::initialPeriod (480, 16, 128, 0), 480);
            expectEquals (PeriodPolicy::initialPeriod (128, -16, 480, 200), 128);
        }

        beginTest ("period policy: next larger steps by the fundamental");
        {
            expectEquals (PeriodPolicy::nextLarger (128, 128, 16, 480), 144);
            expectEquals (PeriodPolicy::nextLarger (144, 128, 16, 480), 160);
            expectEquals (PeriodPolicy::nextLarger (464, 128, 16, 480), 480);
            expectEquals (PeriodPolicy::nextLarger (480, 128, 16, 480), 480);
            expectEquals (PeriodPolicy::nextLarger (1000, 128, 16, 480), 1000);

            // Same degenerate rules as initialPeriod.
            expectEquals (PeriodPolicy::nextLarger (200, 128, 0, 480), 128);
            expectEquals (PeriodPolicy::nextLarger (200, 480, 16, 128), 480);
        }

        beginTest ("starvation window: two events within the window escalate");
        {
            StarvationWindow window;
            window.reset (0.0);

            // Nothing during the first second after Start: a fresh endpoint is
            // priming and its first events are not evidence of a late thread.
            expect (! window.record (0.2));
            expect (! window.record (0.9));
            expectEquals (window.count(), 0);

            expect (! window.record (1.2));
            expect (window.record (1.3));
        }

        beginTest ("starvation window: events more than a window apart do not");
        {
            StarvationWindow window;
            window.reset (0.0);

            expect (! window.record (100.0));
            expect (! window.record (161.0), "61 s apart is outside the 60 s window");

            // A third event close to the second does escalate: only the events
            // still inside the window count.
            expect (window.record (162.0));
        }

        beginTest ("starvation window: a long quiet stretch forgets old events");
        {
            StarvationWindow window;
            window.reset (0.0);

            for (int i = 0; i < 20; ++i)
                window.record (100.0 + i * 61.0);

            // Each event arrives after the previous one left the window.
            expect (! window.record (2000.0));
            expectEquals (window.countWithin (2000.0), 1);
        }

        beginTest ("backlog floor");
        {
            BacklogTarget target;
            target.prepare (rate, period, block);

            for (int i = 0; i < 1000; ++i)
                target.onRequest (i % 7);

            std::printf ("    [measured] floor: %d frames, expected %d\n",
                         target.getTargetFrames(), period + margin + block);

            expectEquals (target.getTargetFrames(), period + margin + block);
        }

        beginTest ("no allocation");
        {
            static_assert (std::is_trivially_copyable_v<BacklogTarget>,
                           "BacklogTarget must be trivially copyable");
            static_assert (noexcept (std::declval<BacklogTarget&>().onRequest (0)),
                           "onRequest must be noexcept");

            BacklogTarget target;
            target.prepare (rate, period, block);
            target.onRequest (period);

            expect (target.getTargetFrames() > 0);
        }

        beginTest ("device guards still hold");
        {
            RingBuffer ring (2, 65536);
            WasapiOutputDevice output;

            // An empty id is refused before COM touches an endpoint.
            expect (! output.start ("", ring));
            expect (! output.isRunning());
            expect (output.hasError());

            // A vanished id never falls back to the Windows default endpoint.
            expect (! output.start ("{bogus-id}", ring));
            expect (! output.isRunning());
            expect (output.hasError());
        }
    }
};

static LatencyTests latencyTests;
