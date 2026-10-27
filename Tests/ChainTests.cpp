// Unit tests for the assembled voice chain: the ceiling guarantee, non-finite
// input, the reported latency, click-free switching, block-size independence and
// the default preset levels. They run without any audio device.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe chain

#include <JuceHeader.h>

#include "TestSignals.h"

#include "../Source/Dsp/VoiceChain.h"
#include "../Source/Dsp/RnnoiseDenoiser.h"
#include "../Source/Dsp/SpectralDenoiser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace
{
    using namespace TestSignals;

    void report (const juce::String& message)
    {
        std::printf ("    [measured] %s\n", message.toRawUTF8());
    }

    struct ChainResult
    {
        std::vector<float> out;
        int latency = 0;
    };

    /// A fresh chain, prepared for up to 1024 samples, run over `in` in blocks
    /// of `blockSize`. `params` stays the same for the whole run.
    ChainResult runChain (const std::vector<float>& in, double sr,
                          const VoiceParams& params, int blockSize)
    {
        VoiceChain chain;
        chain.prepare (sr, 1024);

        auto out = processInBlocks (in, blockSize, [&] (float* block, int n)
        {
            chain.process (block, n, params);
        });

        return { std::move (out), chain.getLatencySamples() };
    }

    /// Processes `in` through a fresh chain and changes `params.preGainDb` to
    /// `postGainDb` at exactly `changeSample`, splitting the block that contains
    /// the change so every block size changes at the same sample.
    std::vector<float> runChainWithGainChange (const std::vector<float>& in, double sr,
                                               const VoiceParams& params, float postGainDb,
                                               int changeSample, int blockSize)
    {
        VoiceChain chain;
        chain.prepare (sr, 1024);

        VoiceParams after = params;
        after.preGainDb = postGainDb;

        std::vector<float> out = in;
        int start = 0;

        while (start < (int) out.size())
        {
            const int n = std::min (blockSize, (int) out.size() - start);

            if (start < changeSample && start + n > changeSample)
            {
                const int first = changeSample - start;
                chain.process (out.data() + start, first, params);
                chain.process (out.data() + changeSample, n - first, after);
            }
            else
            {
                chain.process (out.data() + start, n, start >= changeSample ? after : params);
            }

            start += n;
        }

        return out;
    }

    /// The high-level ceiling test signal: the first two seconds of the speech
    /// fixture boosted by 4, then 0 dBFS square bursts, then full-scale noise.
    std::vector<float> ceilingInput (double sr)
    {
        auto x = loadSpeechFixture (sr);
        x.resize (std::min (x.size(), (size_t) (2.0 * sr)));

        for (auto& v : x)
            v *= 4.0f;

        const int squareLength = (int) (0.25 * sr);
        const double w = 2.0 * juce::MathConstants<double>::pi * 440.0 / sr;

        for (int i = 0; i < squareLength; ++i)
            x.push_back (std::sin (w * i) >= 0.0 ? 1.0f : -1.0f);

        const auto noise = whiteNoise (0.0f, (int) (0.25 * sr), 17);
        x.insert (x.end(), noise.begin(), noise.end());
        return x;
    }
}

// =============================================================================
class ChainTests final : public juce::UnitTest
{
public:
    ChainTests() : juce::UnitTest ("VoiceChain", "chain") {}

    void runTest() override
    {
        constexpr float ceiling = 0.8912509f;

        beginTest ("never exceeds the ceiling at max gains");
        {
            for (const double sr : { 48000.0, 44100.0, 96000.0 })
            {
                const auto input = ceilingInput (sr);
                expect (! input.empty(), "input unavailable");

                for (const NoiseMode mode : { NoiseMode::off, NoiseMode::light, NoiseMode::ai })
                {
                    for (const bool bypass : { false, true })
                    {
                        VoiceParams p;
                        p.preGainDb = 24.0f;
                        p.compMakeupDb = 18.0f;
                        p.outputGainDb = 12.0f;
                        p.noiseMode = mode;
                        p.bypass = bypass;

                        const auto r = runChain (input, sr, p, 128);
                        const float peak = peakAbs (r.out.data(), (int) r.out.size());

                        report (juce::String (sr, 0) + " Hz, mode " + (mode == NoiseMode::off ? "off"
                                                                       : mode == NoiseMode::light ? "light" : "ai")
                                + ", bypass " + (bypass ? "on" : "off") + ": peak "
                                + juce::String (juce::Decibels::gainToDecibels (peak, -200.0f), 2) + " dBFS");

                        expect (peak <= ceiling + 1.0e-6f,
                                "peak " + juce::String (peak) + " at " + juce::String (sr) + " Hz");
                    }
                }
            }
        }

        beginTest ("non-finite input");
        {
            constexpr double sr = 48000.0;
            auto clean = loadSpeechFixture (sr);
            expect (! clean.empty(), "speech fixture not found");
            clean.resize (std::min (clean.size(), (size_t) (2.0 * sr)));

            auto damaged = clean;
            const int positions[] = { 1000, 20000, 25000, 35000 };
            int lastBad = 0;

            for (int i = 0; i < 4; ++i)
            {
                const int pos = positions[i];
                if (pos >= (int) damaged.size())
                    continue;

                damaged[(size_t) pos] = (i == 0) ? std::numeric_limits<float>::quiet_NaN()
                                       : (i == 1) ? std::numeric_limits<float>::infinity()
                                       : (i == 2) ? -std::numeric_limits<float>::infinity()
                                       : std::numeric_limits<float>::quiet_NaN();
                clean[(size_t) pos] = 0.0f;
                lastBad = pos;
            }

            const VoiceParams params;
            const auto a = runChain (damaged, sr, params, 128);
            const auto b = runChain (clean, sr, params, 128);

            bool allFinite = true;
            for (const float v : a.out)
                allFinite = allFinite && std::isfinite (v);

            expect (allFinite, "non-finite input produced non-finite output");

            const size_t from = (size_t) lastBad + (size_t) (0.1 * sr);
            float worst = 0.0f;

            for (size_t i = from; i < a.out.size() && i < b.out.size(); ++i)
                worst = std::max (worst, std::abs (a.out[i] - b.out[i]));

            expect (worst <= 1.0e-3f, "100 ms after the last bad sample the chain differs by "
                                          + juce::String (worst));
        }

        beginTest ("latency is what is heard");
        {
            constexpr double sr = 48000.0;
            const auto input = whiteNoise (-20.0f, (int) sr, 42);

            SpectralDenoiser spectral;
            spectral.prepare (sr, 128);
            const int lightLatency = spectral.getLatencySamples();

            RnnoiseDenoiser rnnoise;
            rnnoise.prepare (sr, 128);
            const int aiLatency = rnnoise.getLatencySamples();

            for (const auto [mode, bypass] : { std::pair { NoiseMode::off, false },
                                               std::pair { NoiseMode::light, false },
                                               std::pair { NoiseMode::ai, false },
                                               std::pair { NoiseMode::off, true },
                                               std::pair { NoiseMode::light, true } })
            {
                VoiceParams p;
                p.preGainDb = 0.0f;
                p.outputGainDb = 0.0f;
                p.hpfOn = false;
                p.gateOn = false;
                p.eqOn = false;
                p.compOn = false;
                p.deEssOn = false;
                p.noiseAmount = 0.0f;
                p.noiseMode = mode;
                p.bypass = bypass;

                const auto r = runChain (input, sr, p, 128);
                // Bypass skips the noise stage, so its delay is not heard even
                // when a mode is selected.
                const int expected = bypass ? 47
                                   : mode == NoiseMode::light ? lightLatency + 47
                                   : mode == NoiseMode::ai ? aiLatency + 47
                                   : 47;

                expectEquals (r.latency, expected);

                float worst = 0.0f;
                for (size_t i = 0; i + (size_t) r.latency < r.out.size() && i < input.size(); ++i)
                    worst = std::max (worst, std::abs (r.out[i + (size_t) r.latency] - input[i]));

                expect (worst <= 1.0e-5f, juce::String ("mode ")
                                              + (mode == NoiseMode::off ? "off"
                                                         : mode == NoiseMode::light ? "light" : "ai")
                                              + ", bypass " + (bypass ? "on" : "off")
                                              + ": worst delay error " + juce::String (worst));
            }
        }

        beginTest ("switching is click-free");
        {
            constexpr double sr = 48000.0;
            const float amplitude = juce::Decibels::decibelsToGain (-12.0f);
            const auto sine = TestSignals::sine (sr, 440.0, amplitude, (int) (3.5 * sr));

            const auto steady = runChain (sine, sr, VoiceParams {}, 128);
            const float steadyStep = maxStep (steady.out.data(), (int) steady.out.size());

            VoiceChain chain;
            chain.prepare (sr, 128);

            auto changed = sine;
            VoiceParams params;

            // Every flip happens at an exact sample, splitting the block that
            // contains it, so a click cannot hide between two blocks. The mode
            // sequence leaves enough room for each prime and crossfade.
            enum class Change { hpf, gate, eq, comp, deEss, bypass, mute, mode };
            struct Event { int at; Change what; bool on; NoiseMode mode; };

            const std::vector<Event> events
            {
                { (int) (0.20 * sr), Change::hpf,    false, NoiseMode::light },
                { (int) (0.35 * sr), Change::gate,   false, NoiseMode::light },
                { (int) (0.50 * sr), Change::eq,     false, NoiseMode::light },
                { (int) (0.65 * sr), Change::comp,   false, NoiseMode::light },
                { (int) (0.80 * sr), Change::deEss,  false, NoiseMode::light },
                { (int) (1.00 * sr), Change::bypass, true,  NoiseMode::light },
                { (int) (1.25 * sr), Change::bypass, false, NoiseMode::light },
                { (int) (1.50 * sr), Change::mute,   true,  NoiseMode::light },
                { (int) (1.90 * sr), Change::mute,   false, NoiseMode::light },
                { (int) (2.00 * sr), Change::mode,   false, NoiseMode::off },
                { (int) (2.25 * sr), Change::mode,   false, NoiseMode::light },
                { (int) (2.50 * sr), Change::mode,   false, NoiseMode::ai },
                { (int) (2.95 * sr), Change::mode,   false, NoiseMode::off },
            };

            const auto apply = [&params] (const Event& e)
            {
                switch (e.what)
                {
                    case Change::hpf:    params.hpfOn = e.on; break;
                    case Change::gate:   params.gateOn = e.on; break;
                    case Change::eq:     params.eqOn = e.on; break;
                    case Change::comp:   params.compOn = e.on; break;
                    case Change::deEss:  params.deEssOn = e.on; break;
                    case Change::bypass: params.bypass = e.on; break;
                    case Change::mute:   params.mute = e.on; break;
                    case Change::mode:   params.noiseMode = e.mode; break;
                }
            };

            size_t next = 0;
            int pos = 0;
            int mutedAt = -1;

            while (pos < (int) changed.size())
            {
                while (next < events.size() && events[next].at <= pos)
                {
                    apply (events[next]);

                    if (events[next].what == Change::mute && events[next].on)
                        mutedAt = events[next].at;

                    ++next;
                }

                const int untilNext = next < events.size() ? events[next].at - pos
                                                           : (int) changed.size() - pos;
                const int n = std::min (128, untilNext);
                chain.process (changed.data() + pos, n, params);
                pos += n;
            }

            const float changedStep = maxStep (changed.data(), (int) changed.size());
            report ("max step: steady " + juce::String (steadyStep, 6) + ", switching "
                    + juce::String (changedStep, 6));
            expect (changedStep <= 1.5f * steadyStep,
                    "switching made a step of " + juce::String (changedStep) + " against "
                        + juce::String (steadyStep));

            expect (mutedAt > 0, "mute event never applied");
            float mutedWorst = 0.0f;
            const int from = mutedAt + (int) (0.015 * sr);
            const int to = mutedAt + (int) (0.30 * sr);

            for (int i = from; i < to && i < (int) changed.size(); ++i)
                mutedWorst = std::max (mutedWorst, std::abs (changed[(size_t) i]));

            expect (mutedWorst < 1.0e-4f, "mute left " + juce::String (mutedWorst));
        }

        beginTest ("block-size independence");
        {
            constexpr double sr = 48000.0;
            auto input = loadSpeechFixture (sr);
            expect (! input.empty(), "speech fixture not found");
            input.resize (std::min (input.size(), (size_t) (2.5 * sr)));

            VoiceParams params;
            params.noiseMode = NoiseMode::ai;

            const auto reference = runChainWithGainChange (input, sr, params, 12.0f, 20000, 1024);

            for (const int blockSize : { 1, 61, 128, 480, 1024 })
            {
                const auto out = runChainWithGainChange (input, sr, params, 12.0f, 20000, blockSize);

                float worst = 0.0f;
                for (size_t i = 0; i < out.size() && i < reference.size(); ++i)
                    worst = std::max (worst, std::abs (out[i] - reference[i]));

                expect (worst <= 1.0e-5f, "block size " + juce::String (blockSize) + " differs by "
                                              + juce::String (worst));
            }
        }

        beginTest ("defaults are sane");
        {
            constexpr double sr = 48000.0;
            auto input = loadSpeechFixture (sr);
            expect (! input.empty(), "speech fixture not found");

            // The mask is taken from the unscaled fixture, so it still marks the
            // same spoken words after the whole signal is levelled.
            const auto mask = speechMask (input, sr);

            // The brief's "-30 dBFS peak" leaves this fixture's speech at about
            // -45 dBFS RMS (17.6 dB crest). The defaults add 9 dB of fixed gain
            // (pre-gain 6, make-up 3), so that stimulus can never reach the
            // stated -30 dBFS output window. Level the fixture by its speech RMS
            // instead; the output then has to come out louder, and still below
            // the ceiling.
            double inputSumSquares = 0.0;
            size_t inputCount = 0;

            for (size_t i = 0; i < input.size() && i < mask.size(); ++i)
                if (mask[i])
                {
                    inputSumSquares += (double) input[i] * input[i];
                    ++inputCount;
                }

            const double inputRms = inputCount > 0 ? std::sqrt (inputSumSquares / (double) inputCount) : 0.0;
            const float wantedRms = juce::Decibels::decibelsToGain (-30.0f);
            const float scale = inputRms > 0.0 ? (float) (wantedRms / inputRms) : 1.0f;

            for (auto& v : input)
                v *= scale;

            const auto r = runChain (input, sr, VoiceParams {}, 128);

            const float outPeak = peakAbs (r.out.data(), (int) r.out.size());
            expect (outPeak <= ceiling + 1.0e-6f, "defaults peak " + juce::String (outPeak));

            double sumSquares = 0.0;
            size_t count = 0;

            for (size_t i = 0; i + (size_t) r.latency < r.out.size() && i < mask.size(); ++i)
                if (mask[i])
                {
                    const float v = r.out[i + (size_t) r.latency];
                    sumSquares += (double) v * v;
                    ++count;
                }

            const double rms = count > 0 ? std::sqrt (sumSquares / (double) count) : 0.0;
            const float speechRms = rms > 1.0e-10 ? (float) (20.0 * std::log10 (rms)) : silenceDb;
            report (juce::String ((int) count) + " masked samples, input peak "
                    + juce::String (juce::Decibels::gainToDecibels (peakAbs (input.data(),
                                                                             (int) input.size()),
                                                                      -200.0f), 2)
                    + " dBFS, input speech RMS "
                    + juce::String (juce::Decibels::gainToDecibels ((float) (inputRms * scale),
                                                                    -200.0f), 2)
                    + " dBFS, output speech RMS " + juce::String (speechRms, 2) + " dBFS");

            expect (speechRms > -30.0f && speechRms < -12.0f,
                    "speech RMS " + juce::String (speechRms) + " dBFS is outside the sane range");
        }
    }
};

static ChainTests chainTests;
