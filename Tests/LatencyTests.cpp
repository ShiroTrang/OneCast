// Unit tests for the output-side latency pieces: the shrinking backlog target
// and the guarantees that keep it real-time safe. No audio device is opened.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe latency

#include <JuceHeader.h>

#include "../Source/Audio/BacklogTarget.h"
#include "../Source/Audio/RingBuffer.h"
#include "../Source/Audio/WasapiOutputDevice.h"

#include <cmath>
#include <cstdio>
#include <type_traits>
#include <utility>

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
            BacklogTarget target;
            target.prepare (rate, period, block);

            target.onRequest (960);

            // One event per period: 100 events per second, so the fixed
            // window is 500 events for the default five seconds.
            int secondStart = target.getTargetFrames();
            int worstDropPerSecond = 0;

            for (int i = 0; i < 600; ++i)
            {
                target.onRequest (period);

                if ((i + 1) % 100 == 0)
                {
                    const int now = target.getTargetFrames();
                    worstDropPerSecond = juce::jmax (worstDropPerSecond, secondStart - now);
                    secondStart = now;
                }
            }

            const int expected = period + margin + block;
            std::printf ("    [measured] shrink: %d frames after 6 s, expected %d, worst drop %d/s\n",
                         target.getTargetFrames(), expected, worstDropPerSecond);

            expectWithinAbsoluteError ((float) target.getTargetFrames(), (float) expected, 1.0f);
            expect (worstDropPerSecond <= period,
                    "the target dropped more than one period in a second");
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
