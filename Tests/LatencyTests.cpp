// Unit tests for the output-side latency pieces: the shrinking backlog target
// and the guarantees that keep it real-time safe. No audio device is opened.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe latency

#include <JuceHeader.h>

#include "../Source/Audio/BacklogTarget.h"
#include "../Source/Audio/DriftCompensatedReader.h"
#include "../Source/Audio/OutputCeiling.h"
#include "../Source/Audio/PeriodPolicy.h"
#include "../Source/Audio/RingBuffer.h"
#include "../Source/Audio/SharedStreamFormat.h"
#include "../Source/Audio/WasapiOutputDevice.h"

#include <cmath>
#include <cstdint>
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

    /// A band-limited-ish square: its steps are what makes the 4-point
    /// interpolation ring above the input's own peak.
    std::vector<float> makeSquare (int frames, int halfPeriod, float amplitude)
    {
        std::vector<float> data ((size_t) frames * 2);

        for (int i = 0; i < frames; ++i)
        {
            const float value = ((i / halfPeriod) % 2) == 0 ? amplitude : -amplitude;
            data[(size_t) i * 2] = value;
            data[(size_t) i * 2 + 1] = value;
        }

        return data;
    }

    /// Deterministic white noise at the same amplitude; random neighbours with
    /// opposite signs are the other case the Lagrange polynomial overshoots.
    std::vector<float> makeNoise (int frames, float amplitude)
    {
        std::vector<float> data ((size_t) frames * 2);
        std::uint32_t state = 0x12345678u;

        for (int i = 0; i < frames; ++i)
        {
            state = state * 1664525u + 1013904223u;
            const float value = ((float) (std::int32_t) state / 2147483648.0f) * amplitude;
            data[(size_t) i * 2] = value;
            data[(size_t) i * 2 + 1] = value;
        }

        return data;
    }

    struct ReaderPeaks
    {
        float raw = 0.0f;
        float clamped = 0.0f;
        int frames = 0;
    };

    /// Push a signal through DriftCompensatedReader at a nominal ratio and
    /// report the largest absolute sample before and after the render path's
    /// clamp.
    ReaderPeaks measureReaderPeaks (const std::vector<float>& interleaved,
                                    int totalFrames, double nominalRatio)
    {
        RingBuffer ring (2, 1 << 16);
        DriftCompensatedReader reader;
        reader.prepare (2, 256, 48000.0, 1024);

        std::vector<float> left (256), right (256);
        float* out[] = { left.data(), right.data() };

        ReaderPeaks peaks;
        int fed = 0;

        while (fed < totalFrames)
        {
            // Keep about 2048 frames in the ring: enough to stay primed and
            // far below the reader's resync threshold, so no content is
            // skipped by a resync.
            while (fed < totalFrames && ring.getAvailableForRead() < 2048)
            {
                const int chunk = juce::jmin (256, totalFrames - fed);
                ring.writeInterleaved (interleaved.data() + (size_t) fed * 2, chunk, 2);
                fed += chunk;
            }

            if (reader.read (ring, out, 2, 256, nominalRatio) <= 0)
                continue;

            peaks.frames += 256;

            for (int i = 0; i < 256; ++i)
            {
                peaks.raw = juce::jmax (peaks.raw, std::abs (left[(size_t) i]),
                                        std::abs (right[(size_t) i]));
                peaks.clamped = juce::jmax (peaks.clamped,
                                            std::abs (OutputCeiling::clamp (left[(size_t) i])),
                                            std::abs (OutputCeiling::clamp (right[(size_t) i])));
            }
        }

        return peaks;
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

        beginTest ("period policy: an unaligned maximum is aligned down");
        {
            // The engine's maximum is a bound, not necessarily a period it
            // accepts: only min + k * fundamental is valid. Asking for 470
            // when the grid ends at 464 would be refused, not clamp.
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 470, 10000), 464);
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 470, 465), 464);
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 470, 463), 464);
            expectEquals (PeriodPolicy::initialPeriod (128, 16, 470, 200), 208);
            expectEquals (PeriodPolicy::nextLarger (448, 128, 16, 470), 464);
            expectEquals (PeriodPolicy::nextLarger (464, 128, 16, 470), 464);
        }

        beginTest ("output ceiling: the resampler overshoots, the clamp does not");
        {
            constexpr float ceiling = OutputCeiling::ceiling;

            // Direct behaviour of the clamp itself, including the sign.
            expectEquals (OutputCeiling::clamp (2.0f), ceiling);
            expectEquals (OutputCeiling::clamp (-2.0f), -ceiling);
            expectEquals (OutputCeiling::clamp (0.25f), 0.25f);

            constexpr int frames = 100000;
            const double ratios[] = { 1.002, 44100.0 / 48000.0 };

            for (const double ratio : ratios)
            {
                const std::pair<const char*, std::vector<float>> signals[]
                {
                    { "square", makeSquare (frames, 16, ceiling) },
                    { "noise",  makeNoise (frames, ceiling) }
                };

                for (const auto& signal : signals)
                {
                    const auto peaks = measureReaderPeaks (signal.second, frames, ratio);

                    std::printf ("    [measured] %s ratio %.4f: raw peak %.6f, clamped %.6f "
                                 "(%d frames)\n",
                                 signal.first, ratio, peaks.raw, peaks.clamped, peaks.frames);

                    expect (peaks.frames > 20000, "not enough audio measured");
                    expect (peaks.raw > ceiling,
                            signal.first + juce::String (" at ratio ")
                                + juce::String (ratio)
                                + " must show the interpolation overshoot");
                    expect (peaks.clamped <= ceiling,
                            "the render clamp must bring every sample under -1 dBFS");
                }
            }
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
