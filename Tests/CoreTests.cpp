// Unit tests for the shared building blocks every other part of OneCast sits on:
// the audio ring buffer and drift reader, the safety limiter, device recovery,
// the DSP parameter contract and the test helpers themselves. They run without
// any audio device: clocks, drift and device loss are simulated.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe core

#include <JuceHeader.h>

#include "TestSignals.h"

#include "../Source/Audio/RingBuffer.h"
#include "../Source/Audio/DriftCompensatedReader.h"
#include "../Source/Audio/SafetyLimiter.h"
#include "../Source/Audio/DeviceRecovery.h"
#include "../Source/Dsp/DspParams.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace
{
    constexpr double twoPi = 6.283185307179586;

    /// Simulates a producer on one clock feeding a reader on another, in
    /// packets, and reports how the ring behaved.
    struct ClockSimulation
    {
        double producerRate = 48000.0;          // true clock (unknown to the reader)
        double nominalProducerRate = 48000.0;   // what the device claims
        double consumerRate = 48000.0;
        int producerPacket  = 480;    // WASAPI-style 10 ms packets
        int consumerBlock   = 128;    // ASIO-style blocks
        int targetFill      = 128 + 624;
        double seconds      = 600.0;  // a long song set: drift needs time to show

        // Results
        int underrunsAfterSettling = 0;
        int resyncs = 0;
        double maxFillErrorAfterSettling = 0.0;   // frames, vs target
        double finalRatio = 0.0;

        void run()
        {
            RingBuffer ring (2, 65536);
            DriftCompensatedReader reader;
            reader.prepare (2, consumerBlock, consumerRate, targetFill);

            std::vector<float> packet ((size_t) producerPacket * 2, 0.1f);
            std::vector<float> outL ((size_t) consumerBlock), outR ((size_t) consumerBlock);
            float* outs[] = { outL.data(), outR.data() };

            const double producerFramesPerBlock = consumerBlock * producerRate / consumerRate;
            double owed = 0.0;
            const int numBlocks = (int) (seconds * consumerRate / consumerBlock);
            const int settleBlocks = (int) (30.0 * consumerRate / consumerBlock);
            int underrunsAtSettle = 0;

            for (int b = 0; b < numBlocks; ++b)
            {
                owed += producerFramesPerBlock;

                while (owed >= producerPacket)
                {
                    ring.writeInterleaved (packet.data(), producerPacket, 2);
                    owed -= producerPacket;
                }

                const int fill = ring.getAvailableForRead();
                // Only the nominal rates are known; the drift is hidden.
                reader.read (ring, outs, 2, consumerBlock, nominalProducerRate / consumerRate);

                if (b == settleBlocks)
                    underrunsAtSettle = reader.getUnderrunCount();

                if (b > settleBlocks)
                    maxFillErrorAfterSettling = juce::jmax (maxFillErrorAfterSettling,
                                                            std::abs ((double) fill - targetFill));
            }

            underrunsAfterSettling = reader.getUnderrunCount() - underrunsAtSettle;
            resyncs = reader.getResyncCount();
            finalRatio = reader.getCurrentRatio();

            std::printf ("      [sim %.0f -> %.0f Hz, %.0f s] fill error <= %.0f frames, "
                         "underruns %d, resyncs %d, ratio %.6f\n",
                         producerRate, consumerRate, seconds, maxFillErrorAfterSettling,
                         underrunsAfterSettling, resyncs, finalRatio);
        }
    };
}

// =============================================================================
class RingBufferTests final : public juce::UnitTest
{
public:
    RingBufferTests() : juce::UnitTest ("RingBuffer peek/discard", "core") {}

    void runTest() override
    {
        beginTest ("peek does not consume, discard does");

        RingBuffer ring (2, 16);
        const float l[] = { 1, 2, 3, 4, 5 };
        const float r[] = { -1, -2, -3, -4, -5 };
        const float* in[] = { l, r };
        ring.write (in, 5, 2);

        float a[8] = {}, c[8] = {};
        float* out[] = { a, c };

        expectEquals (ring.peek (out, 3, 2), 3);
        expectEquals (a[2], 3.0f);
        expectEquals (c[2], -3.0f);
        expectEquals (ring.getAvailableForRead(), 5);

        expectEquals (ring.discard (2), 2);
        expectEquals (ring.getAvailableForRead(), 3);
        expectEquals (ring.peek (out, 8, 2), 3);
        expectEquals (a[0], 3.0f);

        expectEquals (ring.discard (100), 3);
        expectEquals (ring.getAvailableForRead(), 0);
    }
};

// =============================================================================
class DriftReaderTests final : public juce::UnitTest
{
public:
    DriftReaderTests() : juce::UnitTest ("DriftCompensatedReader", "core") {}

    void runTest() override
    {
        beginTest ("primes with silence until the target backlog exists");
        {
            RingBuffer ring (2, 4096);
            DriftCompensatedReader reader;
            reader.prepare (2, 64, 48000.0, 256);

            std::vector<float> data (200 * 2, 0.5f);
            ring.writeInterleaved (data.data(), 200, 2);

            float l[64], r[64];
            float* out[] = { l, r };
            expectEquals (reader.read (ring, out, 2, 64, 1.0), 0);
            expectEquals (l[10], 0.0f);
            expectEquals (ring.getAvailableForRead(), 200, "nothing consumed while priming");

            ring.writeInterleaved (data.data(), 100, 2);
            expectEquals (reader.read (ring, out, 2, 64, 1.0), 64);
            expect (std::abs (l[40] - 0.5f) < 1.0e-4f, "audio flows once primed");
        }

        beginTest ("partial underrun fades out instead of cutting to silence");
        {
            RingBuffer ring (1, 4096);
            DriftCompensatedReader reader;
            reader.prepare (1, 256, 48000.0, 512);

            // Exactly the target, so the first read leaves priming and consumes
            // one block, leaving less than the next request needs in the ring.
            std::vector<float> audio (512, 0.5f);
            const float* source[] = { audio.data() };
            ring.write (source, 512, 1);

            float out[256];
            float* dest[] = { out };
            expectEquals (reader.read (ring, dest, 1, 256, 1.0), 256);

            const int produced = reader.read (ring, dest, 1, 256, 1.0);

            float maxStep = 0.0f;
            for (int i = 1; i < 256; ++i)
                maxStep = juce::jmax (maxStep, std::abs (out[i] - out[i - 1]));

            std::printf ("    [measured] partial underrun: %d of 256 frames, "
                         "max step %.5f, ring %d\n",
                         produced, maxStep, ring.getAvailableForRead());

            expect (produced > 0 && produced < 256,
                    "the available audio is delivered, not zeroed");
            expectEquals (reader.getUnderrunCount(), 1);
            expect (reader.isPriming(), "the reader re-primes after a shortfall");

            // A hard cut from 0.5 to zero would be a 0.5 step; the fade keeps
            // every adjacent step far below that.
            expectLessThan (maxStep, 0.05f, "no hard edge into silence");
            expectEquals (out[255], 0.0f);
        }

        beginTest ("same nominal rate, producer 150 ppm fast: holds the backlog");
        {
            ClockSimulation sim;
            sim.producerRate = 48000.0 * (1.0 + 150e-6);
            sim.run();
            expectEquals (sim.underrunsAfterSettling, 0);
            expectEquals (sim.resyncs, 0);
            expect (sim.maxFillErrorAfterSettling < sim.producerPacket + sim.consumerBlock,
                    "fill error " + juce::String (sim.maxFillErrorAfterSettling));
            expect (sim.finalRatio > 1.0, "consumes slightly faster");
        }

        beginTest ("same nominal rate, producer 300 ppm slow: holds the backlog");
        {
            ClockSimulation sim;
            sim.producerRate = 48000.0 * (1.0 - 300e-6);
            sim.run();
            expectEquals (sim.underrunsAfterSettling, 0);
            expectEquals (sim.resyncs, 0);
            expect (sim.maxFillErrorAfterSettling < sim.producerPacket + sim.consumerBlock,
                    "fill error " + juce::String (sim.maxFillErrorAfterSettling));
            expect (sim.finalRatio < 1.0, "consumes slightly slower");
        }

        beginTest ("44.1 kHz producer into a 48 kHz consumer (Windows refused to convert)");
        {
            ClockSimulation sim;
            sim.producerRate = 44100.0 * (1.0 + 80e-6);
            sim.nominalProducerRate = 44100.0;
            sim.producerPacket = 441;
            sim.run();
            expectEquals (sim.underrunsAfterSettling, 0);
            expectEquals (sim.resyncs, 0);
            expect (std::abs (sim.finalRatio - 44100.0 / 48000.0) < 0.001);
            expect (sim.maxFillErrorAfterSettling < sim.producerPacket + sim.consumerBlock,
                    "fill error " + juce::String (sim.maxFillErrorAfterSettling));
        }

        beginTest ("discards a stale backlog at once instead of playing it late");
        {
            RingBuffer ring (2, 65536);
            DriftCompensatedReader reader;
            reader.prepare (2, 128, 48000.0, 1000);

            // 1 s written while nobody was reading (the old cable-output bug).
            std::vector<float> data (48000 * 2, 0.25f);
            ring.writeInterleaved (data.data(), 48000, 2);

            float l[128], r[128];
            float* out[] = { l, r };
            reader.read (ring, out, 2, 128, 1.0);

            expectEquals (reader.getResyncCount(), 1);
            expect (ring.getAvailableForRead() <= 1000, "backlog cut to the target");
        }

        beginTest ("short read returns what the ring holds without underrunning");
        {
            RingBuffer ring (1, 4096);
            DriftCompensatedReader reader;
            reader.prepare (1, 128, 48000.0, 256);

            std::vector<float> data (1024, 0.5f);
            const float* source[] = { data.data() };
            ring.write (source, 1024, 1);

            float out[128];
            float* dest[] = { out };

            // Leave priming with a full read first: readUpTo is the render
            // loop's path for an already primed stream.
            expectEquals (reader.read (ring, dest, 1, 128, 1.0), 128);

            // Exactly 60 frames left, fewer than the 128 asked for.
            ring.discard (ring.getAvailableForRead() - 60);

            const int underrunsBefore = reader.getUnderrunCount();
            const int produced = reader.readUpTo (ring, dest, 1, 128, 1.0);
            const double ratio = reader.getCurrentRatio();
            const int expected = (int) std::floor ((60.0 - 2.0) / ratio);

            std::printf ("    [measured] short read: %d of 128 frames, expected %d "
                         "(ratio %.6f), ring %d\n",
                         produced, expected, ratio, ring.getAvailableForRead());

            expect (produced > 0, "the available audio is delivered");
            expect (produced < 128, "fewer frames than asked for");
            expectEquals (produced, expected);
            expectEquals (reader.getUnderrunCount(), underrunsBefore,
                          "a short read is not an underrun");
            expect (! reader.isPriming(), "a short read must not re-prime");
        }

        beginTest ("short read continues seamlessly into the next read");
        {
            RingBuffer ring (1, 65536);
            DriftCompensatedReader reader;
            reader.prepare (1, 128, 48000.0, 256);

            const int total = 4096;
            std::vector<float> sine ((size_t) total);
            for (int i = 0; i < total; ++i)
                sine[(size_t) i] = (float) std::sin (twoPi * 1000.0 * i / 48000.0);

            int written = 0;
            const auto feed = [&] (int frames)
            {
                const int chunk = juce::jmin (frames, total - written);
                const float* source[] = { sine.data() + written };
                written += ring.write (source, chunk, 1);
            };

            float block[128];
            float* dest[] = { block };
            std::vector<float> received;

            // Two full reads consume about 2 * (128 + 2) input frames, so the
            // 384 fed here leave the next request short by a few frames.
            feed (384);
            expectEquals (reader.read (ring, dest, 1, 128, 1.0), 128);
            received.insert (received.end(), block, block + 128);
            expectEquals (reader.read (ring, dest, 1, 128, 1.0), 128);
            received.insert (received.end(), block, block + 128);

            const int underrunsBefore = reader.getUnderrunCount();
            const int produced = reader.readUpTo (ring, dest, 1, 128, 1.0);
            received.insert (received.end(), block, block + produced);

            std::printf ("    [measured] seamless short read: %d of 128 frames, "
                         "underrun %d -> %d, priming %d\n",
                         produced, underrunsBefore, reader.getUnderrunCount(),
                         reader.isPriming() ? 1 : 0);

            expect (produced > 0 && produced < 128, "the event really was short");
            expectEquals (reader.getUnderrunCount(), underrunsBefore);
            expect (! reader.isPriming());

            // The producer catches up; the waveform must continue with no
            // splice, because the interpolator keeps its state.
            feed (1024);
            expectEquals (reader.read (ring, dest, 1, 128, 1.0), 128);
            received.insert (received.end(), block, block + 128);

            float maxStep = 0.0f;
            for (size_t i = 1; i < received.size(); ++i)
                maxStep = juce::jmax (maxStep, std::abs (received[i] - received[i - 1]));

            std::printf ("    [measured] seamless short read: max step %.5f\n", maxStep);

            // A 1 kHz sine at 48 kHz steps by at most 2*pi*1000/48000 = 0.131.
            expectLessThan (maxStep, 0.14f, "the short read must not splice the waveform");
        }

        beginTest ("readUpTo equals read when the ring has enough");
        {
            const int total = 1024;
            std::vector<float> sine ((size_t) total);
            for (int i = 0; i < total; ++i)
                sine[(size_t) i] = (float) std::sin (twoPi * 440.0 * i / 48000.0);

            RingBuffer ringRead (1, 4096), ringUpTo (1, 4096);
            DriftCompensatedReader readerRead, readerUpTo;
            readerRead.prepare (1, 128, 48000.0, 256);
            readerUpTo.prepare (1, 128, 48000.0, 256);

            const float* source[] = { sine.data() };
            ringRead.write (source, total, 1);
            ringUpTo.write (source, total, 1);

            float blockRead[128], blockUpTo[128];
            float* destRead[] = { blockRead };
            float* destUpTo[] = { blockUpTo };

            // Same priming read on both.
            expectEquals (readerRead.read (ringRead, destRead, 1, 128, 1.0), 128);
            expectEquals (readerUpTo.read (ringUpTo, destUpTo, 1, 128, 1.0), 128);

            expectEquals (readerRead.read (ringRead, destRead, 1, 128, 1.0), 128);
            expectEquals (readerUpTo.readUpTo (ringUpTo, destUpTo, 1, 128, 1.0), 128);

            float maxDifference = 0.0f;
            for (int i = 0; i < 128; ++i)
                maxDifference = juce::jmax (maxDifference,
                                            std::abs (blockRead[i] - blockUpTo[i]));

            expectLessThan (maxDifference, 1.0e-7f,
                            "a fully supplied readUpTo must match read sample for sample");
        }

        beginTest ("passes a sine through intact at ratio 1");
        {
            RingBuffer ring (1, 65536);
            DriftCompensatedReader reader;
            reader.prepare (1, 256, 48000.0, 512);

            const int total = 48000;
            std::vector<float> sine ((size_t) total);
            for (int i = 0; i < total; ++i)
                sine[(size_t) i] = (float) std::sin (twoPi * 1000.0 * i / 48000.0);

            std::vector<float> received;
            float block[256];
            float* out[] = { block };
            int written = 0;

            while (written < total - 512)
            {
                const float* in[] = { sine.data() + written };
                written += ring.write (in, 256, 1);

                if (reader.read (ring, out, 1, 256, 1.0) > 0)
                    received.insert (received.end(), block, block + 256);
            }

            expect (received.size() > 20000);

            // Compare with the source at the best-matching lag: shape preserved.
            double bestErr = 1.0e9;
            for (int lag = -8; lag <= 8; ++lag)
            {
                double err = 0.0;
                for (size_t i = 2000; i < 12000; ++i)
                    err = juce::jmax (err, (double) std::abs (received[i] - sine[(size_t) ((int) i + lag)]));
                bestErr = juce::jmin (bestErr, err);
            }

            // Lagrange at a tiny fractional offset: well under -40 dB error.
            expect (bestErr < 0.01, "max error " + juce::String (bestErr));
        }
    }
};

// =============================================================================
class SafetyLimiterTests final : public juce::UnitTest
{
public:
    SafetyLimiterTests() : juce::UnitTest ("SafetyLimiter", "core") {}

    void runTest() override
    {
        const double sr = 48000.0;

        beginTest ("never exceeds the ceiling, even on sudden full-scale bursts");
        {
            SafetyLimiter lim;
            lim.prepare (2, sr, -1.0f);
            const float ceiling = lim.getCeilingGain();

            juce::Random rng (42);
            juce::AudioBuffer<float> buf (2, 512);
            float worst = 0.0f;

            for (int block = 0; block < 400; ++block)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                    {
                        const bool burst = (block % 7) == 3 && i > 200 && i < 260;
                        const float level = burst ? 6.0f : 0.6f;
                        buf.setSample (ch, i, (rng.nextFloat() * 2.0f - 1.0f) * level);
                    }

                lim.process (buf, 512);
                worst = juce::jmax (worst, buf.getMagnitude (0, 0, 512), buf.getMagnitude (1, 0, 512));
            }

            expect (worst <= ceiling + 1.0e-6f, "peak " + juce::String (worst));
            expect (lim.getGainReductionDb() <= 0.0f);
        }

        beginTest ("is transparent below the ceiling (pure delay)");
        {
            SafetyLimiter lim;
            lim.prepare (1, sr, -1.0f);
            const int latency = lim.getLatencySamples();
            expect (latency > 0 && latency < 100, "about 1 ms of look-ahead");

            juce::AudioBuffer<float> buf (1, 1024);
            std::vector<float> src (1024);
            for (int i = 0; i < 1024; ++i)
                buf.setSample (0, i, src[(size_t) i] = 0.5f * (float) std::sin (twoPi * 440.0 * i / sr));

            lim.process (buf, 1024);

            float err = 0.0f;
            for (int i = latency; i < 1024; ++i)
                err = juce::jmax (err, std::abs (buf.getSample (0, i) - src[(size_t) (i - latency)]));

            expect (err < 1.0e-6f, "error " + juce::String (err));
            expectEquals (lim.getGainReductionDb(), 0.0f);
        }

        beginTest ("a single overshooting sample is brought down without a hard clip");
        {
            SafetyLimiter lim;
            lim.prepare (1, sr, -1.0f);
            const int latency = lim.getLatencySamples();

            juce::AudioBuffer<float> buf (1, 512);
            buf.clear();
            buf.setSample (0, 100, 2.0f);
            lim.process (buf, 512);

            // Exactly at the ceiling, reached through the gain (not the clamp).
            expectWithinAbsoluteError (buf.getSample (0, 100 + latency), lim.getCeilingGain(), 1.0e-5f);
            expect (lim.getGainReductionDb() < -6.0f);
        }

        beginTest ("monitor soft clip: transparent below -1 dBFS, never above 0 dBFS, no latency");
        {
            for (float x : { 0.0f, 0.1f, -0.5f, 0.8f, -0.89f })
                expectEquals (MonitorSoftClip::process (x), x);

            float previous = MonitorSoftClip::process (MonitorSoftClip::knee);
            float biggestStep = 0.0f;

            for (int i = 1; i <= 4000; ++i)
            {
                const float x = MonitorSoftClip::knee + (float) i * 0.001f;   // up to ~+13 dBFS
                const float y = MonitorSoftClip::process (x);
                expect (y <= 1.0f && y >= previous, "at " + juce::String (x));
                expectEquals (MonitorSoftClip::process (-x), -y);
                biggestStep = juce::jmax (biggestStep, y - previous);
                previous = y;
            }

            expect (biggestStep <= 0.001f + 1.0e-6f, "slope never above 1 (continuous knee)");
            expectEquals (MonitorSoftClip::process (std::numeric_limits<float>::quiet_NaN()), 0.0f);
        }

        beginTest ("non-finite samples from a broken plug-in become silence");
        {
            SafetyLimiter lim;
            lim.prepare (1, sr);
            juce::AudioBuffer<float> buf (1, 256);
            for (int i = 0; i < 256; ++i)
                buf.setSample (0, i, 0.1f);
            buf.setSample (0, 50, std::numeric_limits<float>::quiet_NaN());
            buf.setSample (0, 60, std::numeric_limits<float>::infinity());

            lim.process (buf, 256);

            for (int i = 0; i < 256; ++i)
                expect (std::isfinite (buf.getSample (0, i)));

            expectEquals (lim.getGainReductionDb(), 0.0f, "NaN must not trigger reduction");
        }
    }
};

// =============================================================================
class DeviceRecoveryTests final : public juce::UnitTest
{
public:
    DeviceRecoveryTests() : juce::UnitTest ("DeviceRecovery", "core") {}

    void runTest() override
    {
        constexpr double tick   = 1000.0 / 30.0;   // the meter timer
        constexpr double settle = DeviceRecovery::settleMs;

        // The watchdog at 30 Hz from t for ms; the device calls back while
        // `calling` (128 samples at 48 kHz: ~12 callbacks per tick). Returns
        // when it reported the device stopped, or -1.
        auto run = [tick] (DeviceRecovery& r, double& t, std::uint32_t& count,
                           double ms, bool calling) -> double
        {
            for (const double end = t + ms; t < end; t += tick)
            {
                if (calling)
                    count += 12;

                if (r.deviceStopped (true, count, t))
                    return t;
            }

            return -1.0;
        };

        beginTest ("a device that stops calling back is lost after a second, not before");
        {
            DeviceRecovery r;
            double t = 0.0;
            std::uint32_t count = 0;

            expectEquals (run (r, t, count, 5000.0, true), -1.0, "lost while running");

            const double stoppedAt = t;
            const double lostAt = run (r, t, count, 3000.0, false);
            expectWithinAbsoluteError (lostAt - stoppedAt, DeviceRecovery::stallMs, tick + 1.0);
        }

        beginTest ("a blocked message thread is not a stalled device");
        {
            DeviceRecovery r;
            double t = 0.0;
            std::uint32_t count = 0;
            run (r, t, count, 2000.0, true);

            // 3 s without a tick while the device ran on: one late tick.
            t += 3000.0;
            count += 1125;
            expect (! r.deviceStopped (true, count, t));

            // A couple of late ticks with the count still: too few to tell.
            t += 2000.0;
            expect (! r.deviceStopped (true, count, t));
            t += 2000.0;
            expect (! r.deviceStopped (true, count, t));

            // Ticking normally again, and still nothing: now it is stalled.
            expect (run (r, t, count, 1000.0, false) > 0.0, "a real stall after the block");
        }

        beginTest ("only a device seen running can be lost");
        {
            DeviceRecovery r;
            double t = 0.0;
            std::uint32_t count = 0;

            // A device that opened but never called back (e.g. one the user
            // just picked) is the user's to deal with, not "recovered".
            expectEquals (run (r, t, count, 5000.0, false), -1.0);
            expect (! r.deviceStopped (false, count, t), "closed without ever running");

            // One that ran and is then closed under us is lost at once.
            run (r, t, count, 500.0, true);
            expect (r.deviceStopped (false, count, t));
        }

        beginTest ("unplugged: nothing is tried until the interface is back and its notifications settle");
        {
            DeviceRecovery r;
            double t = 0.0;
            std::uint32_t count = 0;
            run (r, t, count, 2000.0, true);

            // Its speakers and microphone go away, then the callbacks stop.
            r.endpointDeparted (t + 100.0);
            r.endpointDeparted (t + 120.0);
            const double lostAt = run (r, t, count, 3000.0, false);
            r.deviceLost (lostAt);

            expect (r.isLost());
            expect (! r.isReopenDue (lostAt + 60000.0), "tried with no hardware");

            // Plugged back in: a burst of notifications.
            const double plug = lostAt + 60000.0;
            r.endpointArrived (plug);
            r.endpointArrived (plug + 300.0);
            r.endpointArrived (plug + 700.0);

            expect (! r.isReopenDue (plug + 700.0 + settle - 1.0), "tried during the burst");
            expect (r.isReopenDue (plug + 700.0 + settle));
        }

        beginTest ("replugged before the loss was noticed: tried without waiting for another notification");
        {
            DeviceRecovery r;
            r.endpointDeparted (100.0);
            r.endpointArrived (600.0);
            r.endpointArrived (800.0);
            r.deviceLost (1100.0);

            expect (! r.isReopenDue (1100.0 + settle - 1.0));
            expect (r.isReopenDue (1100.0 + settle));
        }

        beginTest ("unplugged, but the driver only gives up seconds later: still waits for the plug");
        {
            // The M-Track's driver kept calling back for ~6 s after its
            // endpoints went away, then asked for a reset.
            DeviceRecovery r;
            r.endpointDeparted (28347.0);
            r.endpointDeparted (28431.0);
            r.deviceLost (34612.0);

            expect (! r.isReopenDue (34612.0 + 60000.0), "knocked on a driver with no hardware");

            r.endpointArrived (58669.0);
            r.endpointArrived (58826.0);
            expect (r.isReopenDue (58826.0 + settle));
        }

        beginTest ("a try in progress is not started again when the timer ticks inside it");
        {
            // Opening the ASIO driver ran the message loop; a second reopen
            // from a nested tick deleted the device being opened.
            DeviceRecovery r;
            r.deviceLost (0.0);
            expect (r.isReopenDue (settle));

            r.reopenStarted();
            expect (! r.isReopenDue (settle + 444.0));
            expect (! r.isReopenDue (settle + 60000.0));

            r.reopenFailed (settle + 500.0);
            expect (r.isReopenDue (settle + 500.0 + DeviceRecovery::retryDelaysMs[0]));
        }

        beginTest ("a driver that stopped by itself is tried again straight away");
        {
            DeviceRecovery r;
            r.endpointDeparted (1000.0);   // long before: something else
            r.deviceLost (60000.0);

            expect (! r.isReopenDue (60000.0 + settle - 1.0));
            expect (r.isReopenDue (60000.0 + settle));
        }

        beginTest ("a departure reported just after the loss still means: wait for the plug");
        {
            DeviceRecovery r;
            r.deviceLost (1000.0);
            r.endpointDeparted (1200.0);

            expect (! r.isReopenDue (1000.0 + settle));
            expect (! r.isReopenDue (100000.0));

            r.endpointArrived (100000.0);
            expect (r.isReopenDue (100000.0 + settle));
        }

        beginTest ("failed tries are repeated further apart, then it waits for the next arrival");
        {
            DeviceRecovery r;
            r.deviceLost (0.0);

            double due = settle;

            for (const double delay : DeviceRecovery::retryDelaysMs)
            {
                expect (! r.isReopenDue (due - 1.0));
                expect (r.isReopenDue (due));
                r.reopenFailed (due);
                due += delay;
            }

            expect (r.isReopenDue (due));
            r.reopenFailed (due);
            expect (! r.isReopenDue (due + 3600000.0), "kept knocking on a driver with no hardware");

            // Another device plugged in starts a new round.
            r.endpointArrived (due + 5000.0);
            expect (r.isReopenDue (due + 5000.0 + settle));
        }

        beginTest ("without notifications a lost device is tried every few seconds");
        {
            DeviceRecovery r;
            r.setNotificationsAvailable (false);
            r.deviceLost (0.0);

            double t = settle;

            for (int i = 0; i <= DeviceRecovery::numRetries; ++i)
            {
                r.reopenFailed (t);
                t += (i < DeviceRecovery::numRetries ? DeviceRecovery::retryDelaysMs[i]
                                                     : DeviceRecovery::pollMs);
            }

            expect (! r.isReopenDue (t - 1.0));
            expect (r.isReopenDue (t));
            r.reopenFailed (t);
            expect (r.isReopenDue (t + DeviceRecovery::pollMs));
        }

        beginTest ("one settled signal per burst of arrivals");
        {
            DeviceRecovery r;
            expect (! r.takeSettledArrival (10000.0));

            r.endpointArrived (0.0);
            r.endpointArrived (200.0);
            r.endpointArrived (400.0);

            expect (! r.takeSettledArrival (400.0 + settle - 1.0));
            expect (r.takeSettledArrival (400.0 + settle));
            expect (! r.takeSettledArrival (10000.0), "fired twice for one burst");
        }

        beginTest ("a reopened device leaves the lost state and is watched afresh");
        {
            DeviceRecovery r;
            double t = 0.0;
            std::uint32_t count = 0;
            run (r, t, count, 1000.0, true);
            r.deviceLost (run (r, t, count, 3000.0, false));
            expect (r.isLost());

            r.watchNewDevice();
            expect (! r.isLost());
            expect (! r.isReopenDue (t + 100000.0));

            // The new device has to run before it can be lost again.
            expectEquals (run (r, t, count, 3000.0, false), -1.0);
            run (r, t, count, 1000.0, true);
            expect (run (r, t, count, 3000.0, false) > 0.0);
        }
    }
};

// =============================================================================
class DspParamsTests final : public juce::UnitTest
{
public:
    DspParamsTests() : juce::UnitTest ("DspParams", "core") {}

    void runTest() override
    {
        // Every float field next to its range, so each test below covers all
        // twelve and a field left out of clampToRanges cannot hide.
        using Member = float VoiceParams::*;
        const std::vector<std::pair<Member, ParamRange>> floats
        {
            { &VoiceParams::preGainDb,       ParamRanges::preGainDb },
            { &VoiceParams::hpfHz,           ParamRanges::hpfHz },
            { &VoiceParams::noiseAmount,     ParamRanges::noiseAmount },
            { &VoiceParams::gateThresholdDb, ParamRanges::gateThresholdDb },
            { &VoiceParams::eqMudDb,         ParamRanges::eqMudDb },
            { &VoiceParams::eqClarityDb,     ParamRanges::eqClarityDb },
            { &VoiceParams::eqAirDb,         ParamRanges::eqAirDb },
            { &VoiceParams::compThresholdDb, ParamRanges::compThresholdDb },
            { &VoiceParams::compRatio,       ParamRanges::compRatio },
            { &VoiceParams::compMakeupDb,    ParamRanges::compMakeupDb },
            { &VoiceParams::deEssAmount,     ParamRanges::deEssAmount },
            { &VoiceParams::outputGainDb,    ParamRanges::outputGainDb },
        };

        beginTest ("the defaults of VoiceParams are the defaults of ParamRanges");
        {
            const VoiceParams defaults;

            for (const auto& [member, range] : floats)
            {
                expect (range.min <= range.defaultValue && range.defaultValue <= range.max);
                expectEquals (defaults.*member, range.defaultValue);
            }

            expect (clampToRanges (defaults) == defaults, "defaults must already be in range");
            expect (defaults.noiseMode == NoiseMode::light, "the default noise mode is Light (AI is 30 ms, Light about 5 ms)");
            expect (! defaults.bypass && ! defaults.mute);
        }

        beginTest ("clampToRanges clamps every float to its range");
        {
            VoiceParams high, low;
            for (const auto& entry : floats)
            {
                high.*(entry.first) =  1.0e6f;
                low.*(entry.first)  = -1.0e6f;
            }

            const auto clampedHigh = clampToRanges (high);
            const auto clampedLow  = clampToRanges (low);

            for (const auto& [member, range] : floats)
            {
                expectEquals (clampedHigh.*member, range.max);
                expectEquals (clampedLow.*member,  range.min);
            }
        }

        beginTest ("a value inside its range passes through untouched");
        {
            VoiceParams p;
            for (const auto& [member, range] : floats)
                p.*member = 0.5f * (range.min + range.max);

            expect (clampToRanges (p) == p);

            for (const auto& [member, range] : floats)
            {
                VoiceParams edge;
                edge.*member = range.min;
                expectEquals (clampToRanges (edge).*member, range.min);
                edge.*member = range.max;
                expectEquals (clampToRanges (edge).*member, range.max);
            }
        }

        beginTest ("a NaN becomes the default instead of poisoning the chain");
        {
            for (const auto& [member, range] : floats)
            {
                VoiceParams p;
                p.*member = std::numeric_limits<float>::quiet_NaN();
                expectEquals (clampToRanges (p).*member, range.defaultValue);
            }
        }

        beginTest ("a NoiseMode outside its range becomes the default, light");
        {
            VoiceParams p;

            p.noiseMode = (NoiseMode) 7;
            expect (clampToRanges (p).noiseMode == NoiseMode::light);

            p.noiseMode = (NoiseMode) -1;
            expect (clampToRanges (p).noiseMode == NoiseMode::light);

            // The three real modes are kept, not swept into the default; an
            // explicit ai in particular must not be mistaken for an invalid value.
            p.noiseMode = NoiseMode::off;
            expect (clampToRanges (p).noiseMode == NoiseMode::off);
            p.noiseMode = NoiseMode::light;
            expect (clampToRanges (p).noiseMode == NoiseMode::light);
            p.noiseMode = NoiseMode::ai;
            expect (clampToRanges (p).noiseMode == NoiseMode::ai);
        }

        beginTest ("the on/off switches are not touched by clamping");
        {
            VoiceParams p;
            p.hpfOn = false;
            p.gateOn = false;
            p.eqOn = false;
            p.compOn = false;
            p.deEssOn = false;
            p.bypass = true;
            p.mute = true;
            p.preGainDb = 1.0e6f;

            const auto c = clampToRanges (p);
            expect (! c.hpfOn && ! c.gateOn && ! c.eqOn && ! c.compOn && ! c.deEssOn);
            expect (c.bypass && c.mute);
        }

        beginTest ("AtomicVoiceParams starts at the defaults");
        {
            const AtomicVoiceParams atomic;
            expect (atomic.load() == VoiceParams {});
        }

        beginTest ("AtomicVoiceParams round-trips a non-default VoiceParams exactly");
        {
            // Every field differs from its default, so a field that is not
            // copied in store() or load() shows up as a mismatch.
            VoiceParams p;
            p.preGainDb = 13.37f;
            p.hpfOn = false;           p.hpfHz = 123.5f;
            p.noiseMode = NoiseMode::ai;  p.noiseAmount = 42.25f;
            p.gateOn = false;          p.gateThresholdDb = -61.75f;
            p.eqOn = false;            p.eqMudDb = -7.125f;  p.eqClarityDb = 5.5f;  p.eqAirDb = 3.25f;
            p.compOn = false;          p.compThresholdDb = -27.5f;  p.compRatio = 6.75f;  p.compMakeupDb = 9.125f;
            p.deEssOn = false;         p.deEssAmount = 71.5f;
            p.outputGainDb = -4.875f;
            p.bypass = true;
            p.mute = true;

            expect (! (p == VoiceParams {}));

            AtomicVoiceParams atomic;
            atomic.store (p);
            expect (atomic.load() == p);

            // A second store replaces the first completely.
            VoiceParams q;
            q.noiseMode = NoiseMode::off;
            q.outputGainDb = 2.5f;
            atomic.store (q);
            expect (atomic.load() == q);
            expect (atomic.load().noiseMode == NoiseMode::off);
        }
    }
};

// =============================================================================
class TestSignalsTests final : public juce::UnitTest
{
public:
    TestSignalsTests() : juce::UnitTest ("TestSignals", "core") {}

    void runTest() override
    {
        using namespace TestSignals;

        const auto speech = loadSpeechFixture();

        beginTest ("the speech fixture is 8-12 s and peaks above -12 dBFS");
        {
            expect (speech.size() >= (size_t) (8.0 * 48000.0), "fixture shorter than 8 s");
            expect (speech.size() <= (size_t) (12.0 * 48000.0), "fixture longer than 12 s");

            const float peakDb = juce::Decibels::gainToDecibels (peakAbs (speech.data(), (int) speech.size()));
            expect (peakDb > -12.0f, "fixture peaks at only " + juce::String (peakDb) + " dBFS");
            expect (peakDb <= 0.0f, "fixture clips");
        }

        beginTest ("the speech fixture has speech and the pauses the denoiser tests need");
        {
            const auto mask = speechMask (speech, 48000.0);
            expectEquals ((int) mask.size(), (int) speech.size());

            const int speechSamples = (int) std::count (mask.begin(), mask.end(), true);
            const int quietSamples  = (int) mask.size() - speechSamples;

            expect (speechSamples > 4 * 48000, "less than 4 s of speech");
            expect (quietSamples > (int) (1.5 * 48000.0), "less than 1.5 s of pauses");

            // The pauses are digital silence, so a noise test starts from a clean floor.
            int longestQuietRun = 0, run = 0;
            for (size_t i = 0; i < speech.size(); ++i)
            {
                run = speech[i] == 0.0f ? run + 1 : 0;
                longestQuietRun = juce::jmax (longestQuietRun, run);
            }
            expect (longestQuietRun >= (int) (0.75 * 48000.0), "no ~0.8 s digital-silence gap");
        }

        beginTest ("loadSpeechFixture resamples to the requested rate");
        {
            for (const double rate : { 44100.0, 96000.0 })
            {
                const auto resampled = loadSpeechFixture (rate);
                const double expected = (double) speech.size() * rate / 48000.0;

                expect (std::abs ((double) resampled.size() - expected) <= 2.0,
                        "wrong length at " + juce::String (rate));
                expect (std::abs (rmsDb (resampled.data(), (int) resampled.size())
                                  - rmsDb (speech.data(), (int) speech.size())) < 0.5f,
                        "level changed by resampling at " + juce::String (rate));
            }
        }

        beginTest ("crossCorrelationLag finds a known delay of the speech fixture");
        {
            for (const int delay : { 0, 1, 123, 480 })
            {
                std::vector<float> delayed ((size_t) delay, 0.0f);
                delayed.insert (delayed.end(), speech.begin(), speech.end());

                expectEquals (crossCorrelationLag (speech, delayed, 1000), delay);
            }
        }

        beginTest ("sine, whiteNoise and the level measurements agree with each other");
        {
            const auto tone = sine (48000.0, 1000.0, 0.5f, 48000);
            expectEquals ((int) tone.size(), 48000);
            expectWithinAbsoluteError (peakAbs (tone.data(), 48000), 0.5f, 0.001f);
            expectWithinAbsoluteError (rmsDb (tone.data(), 48000), -9.03f, 0.01f);   // 0.5 / sqrt 2
            expectEquals (tone[0], 0.0f);

            const auto shifted = sine (48000.0, 1000.0, 0.5f, 100, juce::MathConstants<double>::halfPi);
            expectWithinAbsoluteError (shifted[0], 0.5f, 0.0001f);

            // Largest step of a sine is amplitude * 2 pi f / sr (at the zero crossing).
            expectWithinAbsoluteError (maxStep (tone.data(), 48000), 0.5f * 0.1309f, 0.001f);

            const auto noise = whiteNoise (-40.0f, 48000, 7);
            expectWithinAbsoluteError (rmsDb (noise.data(), 48000), -40.0f, 0.01f);
            expect (whiteNoise (-40.0f, 1000, 7) == whiteNoise (-40.0f, 1000, 7), "same seed must repeat");
            expect (! (whiteNoise (-40.0f, 1000, 7) == whiteNoise (-40.0f, 1000, 8)), "seeds must differ");

            expectEquals (rmsDb (nullptr, 0), silenceDb);
            const std::vector<float> zeros (64, 0.0f);
            expectEquals (rmsDb (zeros.data(), 64), silenceDb);
        }

        beginTest ("speechMask follows 10 ms windows");
        {
            auto x = std::vector<float> (4800, 0.0f);                 // 0.1 s of silence
            const auto tone = sine (48000.0, 440.0, 0.3f, 4800);      // 0.1 s of tone
            x.insert (x.end(), tone.begin(), tone.end());

            const auto mask = speechMask (x, 48000.0);
            expectEquals ((int) mask.size(), 9600);
            expect (! mask[0] && ! mask[4799]);
            expect (mask[4800] && mask[9599]);
        }

        beginTest ("processInBlocks hands the processor consecutive blocks and returns the result");
        {
            std::vector<int> sizes;
            const std::vector<float> ramp { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

            const auto out = processInBlocks (ramp, 4, [&] (float* block, int n)
            {
                sizes.push_back (n);
                for (int i = 0; i < n; ++i)
                    block[i] += 100.0f;
            });

            expect (sizes == std::vector<int> ({ 4, 4, 2 }));
            expectEquals ((int) out.size(), 10);
            expectEquals (out[0], 100.0f);
            expectEquals (out[9], 109.0f);
        }
    }
};

static RingBufferTests      ringBufferTests;
static DriftReaderTests     driftReaderTests;
static SafetyLimiterTests   safetyLimiterTests;
static DeviceRecoveryTests  deviceRecoveryTests;
static DspParamsTests       dspParamsTests;
static TestSignalsTests     testSignalsTests;
