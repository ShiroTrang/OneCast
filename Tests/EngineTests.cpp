// Unit tests for the input-only audio engine: the pure device-name helpers,
// the refusal to render to anything but the chosen cable endpoint, channel
// selection, the test signal, the raw clip flag and the latency budget. They
// never open a real input or output device: a fake AudioIODevice drives the
// callback and the output ring is read directly.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe engine

#include <JuceHeader.h>

#include "TestSignals.h"

#include "../Source/Audio/AudioEngine.h"
#include "../Source/Audio/RingBuffer.h"
#include "../Source/Audio/WasapiOutputDevice.h"
#include "../Source/Dsp/DspParams.h"
#include "../Source/Dsp/VoiceChain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    using namespace TestSignals;

    void report (const juce::String& message)
    {
        std::printf ("    [measured] %s\n", message.toRawUTF8());
    }

    /// The voice chain with every module off and unity gains: the output is the
    /// input delayed by the safety limiter's look-ahead, which is what the
    /// channel-selection tests compare against.
    VoiceParams modulesOff()
    {
        VoiceParams p;
        p.preGainDb = 0.0f;
        p.hpfOn = false;
        p.gateOn = false;
        p.eqOn = false;
        p.compOn = false;
        p.deEssOn = false;
        p.noiseMode = NoiseMode::off;
        p.noiseAmount = 0.0f;
        p.outputGainDb = 0.0f;
        return p;
    }

    /// The chain latency the engine reports after its first block, measured
    /// from a chain prepared the same way instead of hard-coding 47.
    int chainLatency()
    {
        VoiceChain chain;
        chain.prepare (48000.0, 128);
        std::vector<float> block (128, 0.0f);
        chain.process (block.data(), (int) block.size(), modulesOff());
        return chain.getLatencySamples();
    }

    /// An AudioIODevice with configurable name, type and input channels,
    /// 48 kHz, 128-sample blocks and 100 samples of input latency. No hardware
    /// is touched.
    class FakeInputDevice final : public juce::AudioIODevice
    {
    public:
        explicit FakeInputDevice (int numChannels, juce::String name = "Fake microphone",
                                  juce::String typeName = "Fake")
            : juce::AudioIODevice (std::move (name), std::move (typeName)),
              channels_ (numChannels)
        {
        }

        juce::StringArray getOutputChannelNames() override { return {}; }

        juce::StringArray getInputChannelNames() override
        {
            juce::StringArray names;

            for (int i = 0; i < channels_; ++i)
                names.add ("Input " + juce::String (i + 1));

            return names;
        }

        juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
        juce::Array<int> getAvailableBufferSizes() override { return { 128 }; }
        int getDefaultBufferSize() override { return 128; }

        juce::String open (const juce::BigInteger&, const juce::BigInteger&,
                           double, int) override
        {
            open_ = true;
            return {};
        }

        void close() override { open_ = false; }
        bool isOpen() override { return open_; }
        void start (juce::AudioIODeviceCallback*) override {}
        void stop() override {}
        bool isPlaying() override { return false; }
        juce::String getLastError() override { return {}; }

        int getCurrentBufferSizeSamples() override { return 128; }
        double getCurrentSampleRate() override { return 48000.0; }
        int getCurrentBitDepth() override { return 32; }
        juce::BigInteger getActiveOutputChannels() const override { return {}; }

        juce::BigInteger getActiveInputChannels() const override
        {
            juce::BigInteger active;
            active.setRange (0, channels_, true);
            return active;
        }

        int getOutputLatencyInSamples() override { return 0; }
        int getInputLatencyInSamples() override { return 100; }

    private:
        int channels_ = 1;
        bool open_ = true;
    };

    /// A device type that only creates FakeInputDevice objects. Adding it to an
    /// engine's manager lets a test "choose Mic A" through the real
    /// AudioDeviceManager without opening any hardware.
    class FakeDeviceType final : public juce::AudioIODeviceType
    {
    public:
        explicit FakeDeviceType (juce::StringArray inputs)
            : juce::AudioIODeviceType ("Fake"), inputs_ (std::move (inputs))
        {
        }

        void scanForDevices() override {}
        juce::StringArray getDeviceNames (bool wantInputNames) const override
        {
            return wantInputNames ? inputs_ : juce::StringArray();
        }

        int getDefaultDeviceIndex (bool) const override { return 0; }
        int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return -1; }
        bool hasSeparateInputsAndOutputs() const override { return true; }

        juce::AudioIODevice* createDevice (const juce::String& outputDeviceName,
                                           const juce::String& inputDeviceName) override
        {
            if (inputDeviceName.isEmpty() || outputDeviceName.isNotEmpty())
                return nullptr;

            return new FakeInputDevice (2, inputDeviceName, getTypeName());
        }

    private:
        juce::StringArray inputs_;
    };

    /// Feeds `numSamples` from up to two deinterleaved input channels through
    /// the callback in 128-sample blocks, the way a device would.
    void feed (AudioEngine& engine, int numInputChannels,
               const float* ch0, const float* ch1, int numSamples)
    {
        juce::AudioIODeviceCallbackContext context;
        constexpr int blockSize = 128;

        for (int start = 0; start < numSamples; start += blockSize)
        {
            const int n = std::min (blockSize, numSamples - start);
            const float* input[2] =
            {
                ch0 != nullptr ? ch0 + start : nullptr,
                ch1 != nullptr ? ch1 + start : nullptr
            };

            engine.audioDeviceIOCallbackWithContext (input, numInputChannels,
                                                     nullptr, 0, n, context);
        }
    }

    /// Consumes the output ring into a stereo buffer.
    juce::AudioBuffer<float> drain (AudioEngine& engine, int numSamples, int& framesRead)
    {
        juce::AudioBuffer<float> out (2, numSamples);
        framesRead = engine.getOutputRing().read (out, numSamples);
        return out;
    }
}

// =============================================================================
class EngineTests final : public juce::UnitTest
{
public:
    EngineTests() : juce::UnitTest ("AudioEngine", "engine") {}

    void runTest() override
    {
        beginTest ("input names exclude the cable");
        {
            juce::StringArray names;
            names.add ("Microphone (USB)");
            names.add ("CABLE Output (VB-Audio Virtual Cable)");
            names.add ("Line In");
            names.add ("cable output (second cable)");

            const auto filtered = AudioEngine::filterInputNames (names);

            expectEquals (filtered.size(), 2);
            expect (filtered.contains ("Microphone (USB)"));
            expect (filtered.contains ("Line In"));
        }

        beginTest ("first-run ASIO pick");
        {
            juce::StringArray asio;
            asio.add ("Generic ASIO");
            asio.add ("M-Audio M-Track Solo and Duo ASIO");

            expectEquals (AudioEngine::pickFirstRunAsioDevice (asio),
                          juce::String ("M-Audio M-Track Solo and Duo ASIO"));

            juce::StringArray otherAsio;
            otherAsio.add ("Generic ASIO");
            expectEquals (AudioEngine::pickFirstRunAsioDevice (otherAsio), juce::String());
            expectEquals (AudioEngine::pickFirstRunAsioDevice ({}), juce::String());
        }

        beginTest ("explicit ASIO pick falls back to the first driver");
        {
            // The M-Track still wins when it is installed.
            juce::StringArray asio;
            asio.add ("Generic ASIO");
            asio.add ("M-Audio M-Track Solo and Duo ASIO");
            expectEquals (AudioEngine::pickAsioDevice (asio),
                          juce::String ("M-Audio M-Track Solo and Duo ASIO"));

            // Without it, an explicit ASIO choice opens one of the installed
            // drivers instead of failing; first run keeps requiring the M-Track.
            juce::StringArray otherAsio;
            otherAsio.add ("Generic ASIO");
            otherAsio.add ("Voicemeeter ASIO");
            expectEquals (AudioEngine::pickAsioDevice (otherAsio), juce::String ("Generic ASIO"));
            expectEquals (AudioEngine::pickFirstRunAsioDevice (otherAsio), juce::String());
            expectEquals (AudioEngine::pickAsioDevice ({}), juce::String());
        }

        beginTest ("WASAPI pick avoids the cable");
        {
            juce::StringArray names;
            names.add ("Microphone (USB)");
            names.add ("CABLE Output (VB-Audio Virtual Cable)");
            names.add ("Line In");

            // The default is the cable, so the first other name is used.
            expectEquals (AudioEngine::pickWasapiInput (names, 1), juce::String ("Microphone (USB)"));
            expectEquals (AudioEngine::pickWasapiInput (names, 0), juce::String ("Microphone (USB)"));
            expectEquals (AudioEngine::pickWasapiInput (names, 99), juce::String ("Microphone (USB)"));

            juce::StringArray cablesOnly;
            cablesOnly.add ("CABLE Output (VB-Audio Virtual Cable)");
            cablesOnly.add ("CABLE Output (VB-Audio Virtual Cable 2)");
            expectEquals (AudioEngine::pickWasapiInput (cablesOnly, 0), juce::String());
            expectEquals (AudioEngine::pickWasapiInput ({}, 0), juce::String());
        }

        beginTest ("finds CABLE Input");
        {
            const std::vector<WasapiOutputDevice::DeviceInfo> devices
            {
                { "speakers-id", "Speakers (Realtek)" },
                { "cable-id", "CABLE Input (VB-Audio Virtual Cable)" }
            };

            expectEquals (AudioEngine::findCableInputId (devices), juce::String ("cable-id"));
            expectEquals (AudioEngine::findCableInputId ({}), juce::String());
        }

        beginTest ("empty input choice is refused");
        {
            // A saved state whose only input is the virtual cable is sanitised
            // to an empty input name. JUCE reads a blank name as "the default
            // device", so it must never reach AudioDeviceManager::initialise().
            juce::XmlElement saved ("DEVICESETUP");
            saved.setAttribute ("deviceType", "Windows Audio");
            saved.setAttribute ("audioDeviceName", "CABLE Output (VB-Audio Virtual Cable)");
            saved.setAttribute ("audioDeviceRate", 48000.0);

            const auto clean = AudioEngine::inputOnlyState (saved);
            expectEquals (clean->getStringAttribute ("audioInputDeviceName"), juce::String());

            AudioEngine engine;
            engine.initialise (&saved);

            expect (! engine.isDeviceOpen(), "a blank input name opened a device");
            expect (engine.getLastDeviceError().isNotEmpty(), "the refusal was not reported");

            const bool reopened = engine.reopenDevice (saved);
            expect (! reopened, "reopenDevice accepted a blank input name");
            expect (! engine.isDeviceOpen(), "reopenDevice opened a device");
            expect (engine.getLastDeviceError().isNotEmpty(),
                    "reopenDevice reported no error");
        }

        beginTest ("generated state has no output layout");
        {
            const auto opened = AudioEngine::makeDeviceState ("Windows Audio",
                                                              "Microphone (USB)", 0);

            // JUCE treats a missing audioDeviceOutChans as "use the default
            // channel counts"; an empty one is an explicit zero.
            expect (opened->hasAttribute ("audioDeviceOutChans"),
                    "a missing audioDeviceOutChans asks JUCE for default output channels");
            expectEquals (opened->getStringAttribute ("audioDeviceOutChans"), juce::String());
            expectEquals (opened->getStringAttribute ("audioOutputDeviceName"), juce::String());
            expectEquals (opened->getStringAttribute ("audioInputDeviceName"),
                          juce::String ("Microphone (USB)"));

            // A state saved by a monitor-equipped build keeps its microphone
            // but loses the output endpoint and its channel layout.
            juce::XmlElement saved ("DEVICESETUP");
            saved.setAttribute ("deviceType", "Windows Audio");
            saved.setAttribute ("audioDeviceName", "Microphone (USB)");
            saved.setAttribute ("audioOutputDeviceName", "Speakers (Realtek)");
            saved.setAttribute ("audioDeviceInChans", "11");
            saved.setAttribute ("audioDeviceOutChans", "11");

            const auto clean = AudioEngine::inputOnlyState (saved);

            expectEquals (clean->getStringAttribute ("audioInputDeviceName"),
                          juce::String ("Microphone (USB)"));
            expectEquals (clean->getStringAttribute ("audioOutputDeviceName"), juce::String());
            expect (clean->hasAttribute ("audioDeviceOutChans"));
            expectEquals (clean->getStringAttribute ("audioDeviceOutChans"), juce::String());
            expect (! clean->hasAttribute ("audioDeviceName"),
                    "the combined device name was not converted to input-only");
        }

        beginTest ("never renders to the default device");
        {
            RingBuffer ring (2, 65536);
            WasapiOutputDevice output;

            expect (! output.start ("", ring));
            expectEquals (output.getErrorMessage(), juce::String ("no output device chosen"));
            expect (! output.isRunning());
            expect (output.hasError());

            AudioEngine engine;
            expect (! engine.setOutputDevice (""));
            expect (! engine.isOutputRunning());
        }

        beginTest ("vanished output id never falls back");
        {
            AudioEngine engine;

            expect (! engine.setOutputDevice ("{bogus-id}"));
            expectEquals (engine.getOutputDeviceId(), juce::String ("{bogus-id}"));
            expect (! engine.isOutputRunning());
            expect (engine.hasOutputError());
        }

        beginTest ("a replaced input is never prepared");
        {
            // JUCE's own audioDeviceListChanged() closes a vanished input and
            // opens the Windows default in its place, which may be another mic
            // or even CABLE Output. The engine has to refuse that device and
            // tell the message thread to put the chosen mic back.
            const auto params = modulesOff();
            const auto sine = TestSignals::sine (48000.0, 440.0, 0.25f, 512);
            const auto silence = std::vector<float> (512, 0.0f);

            const auto openMicA = [&params] (AudioEngine& engine)
            {
                engine.setParams (params);
                engine.getDeviceManager().addAudioDeviceType (
                    std::make_unique<FakeDeviceType> (juce::StringArray { "Mic A" }));
                return engine.setApi ("Fake");
            };

            // A different microphone.
            {
                AudioEngine engine;
                expect (openMicA (engine), "the fake API did not open: "
                                           + engine.getLastDeviceError());

                FakeInputDevice replacement (2, "Mic B", "Fake");
                engine.audioDeviceAboutToStart (&replacement);

                feed (engine, 2, sine.data(), silence.data(), (int) sine.size());

                int framesRead = -1;
                drain (engine, (int) sine.size(), framesRead);
                expectEquals (framesRead, 0, "the replacement device reached the ring");

                expect (engine.takeUnexpectedDeviceFlag(), "the replacement was not flagged");
                expect (! engine.takeUnexpectedDeviceFlag(), "the flag was raised twice");
            }

            // A virtual cable output, which is never a microphone.
            {
                AudioEngine engine;
                expect (openMicA (engine), "the fake API did not open: "
                                           + engine.getLastDeviceError());

                FakeInputDevice cable (2, "CABLE Output (VB-Audio Virtual Cable)", "Fake");
                engine.audioDeviceAboutToStart (&cable);

                feed (engine, 2, sine.data(), silence.data(), (int) sine.size());

                int framesRead = -1;
                drain (engine, (int) sine.size(), framesRead);
                expectEquals (framesRead, 0, "a cable output reached the ring");

                expect (engine.takeUnexpectedDeviceFlag(), "the cable output was not flagged");
                expect (! engine.takeUnexpectedDeviceFlag(), "the flag was raised twice");
            }

            // The chosen mic itself still prepares and reaches the ring, so
            // the refusal above is not simply "nothing ever prepares".
            {
                AudioEngine engine;
                expect (openMicA (engine), "the fake API did not open: "
                                           + engine.getLastDeviceError());

                FakeInputDevice chosen (2, "Mic A", "Fake");
                engine.audioDeviceAboutToStart (&chosen);

                feed (engine, 2, sine.data(), silence.data(), (int) sine.size());

                int framesRead = -1;
                drain (engine, (int) sine.size(), framesRead);
                expectEquals (framesRead, (int) sine.size(), "the chosen mic did not reach the ring");
                expect (! engine.takeUnexpectedDeviceFlag(), "the chosen mic was flagged");
            }
        }

        beginTest ("channel selection");
        {
            constexpr int total = 512;
            const auto params = modulesOff();
            const int latency = chainLatency();
            const float amplitude = juce::Decibels::decibelsToGain (-12.0f);
            const auto sine = TestSignals::sine (48000.0, 440.0, amplitude, total);
            const std::vector<float> silence ((size_t) total, 0.0f);

            expect (latency > 0, "the chain reports no latency");
            report ("chain latency " + juce::String (latency) + " samples");

            // Channel 0 only: both cable channels carry the delayed sine.
            juce::AudioBuffer<float> firstBuffer;
            {
                AudioEngine engine;
                engine.setParams (params);
                FakeInputDevice device (2);
                engine.audioDeviceAboutToStart (&device);
                engine.setInputChannel (InputChannel::first);
                feed (engine, 2, sine.data(), silence.data(), total);

                int framesRead = 0;
                firstBuffer = drain (engine, total, framesRead);
                expectEquals (framesRead, total);
            }

            float firstWorst = 0.0f;
            float stereoWorst = 0.0f;

            for (int i = 0; i < total; ++i)
            {
                const float left = firstBuffer.getSample (0, i);
                const float right = firstBuffer.getSample (1, i);

                stereoWorst = std::max (stereoWorst, std::abs (left - right));

                if (i < latency)
                    firstWorst = std::max (firstWorst, std::abs (left));
                else
                    firstWorst = std::max (firstWorst, std::abs (left - sine[(size_t) (i - latency)]));
            }

            report ("first: worst error " + juce::String (firstWorst, 8)
                    + ", L/R difference " + juce::String (stereoWorst, 8));
            expect (firstWorst <= 1.0e-6f, "the cable signal is not the delayed input");
            expect (stereoWorst <= 1.0e-6f, "the cable channels differ");

            // Channel 1: the selected channel carries silence.
            {
                AudioEngine engine;
                engine.setParams (params);
                FakeInputDevice device (2);
                engine.audioDeviceAboutToStart (&device);
                engine.setInputChannel (InputChannel::second);
                expect (engine.getInputChannel() == InputChannel::second);
                feed (engine, 2, sine.data(), silence.data(), total);

                int framesRead = 0;
                const auto out = drain (engine, total, framesRead);
                expectEquals (framesRead, total);
                const float level = rmsDb (out.getReadPointer (0), total);
                report ("second: " + juce::String (level, 2) + " dBFS");
                expect (level < -90.0f, "the second channel was not selected");
            }

            // Mix: the average of the two channels, so half the amplitude.
            {
                AudioEngine engine;
                engine.setParams (params);
                FakeInputDevice device (2);
                engine.audioDeviceAboutToStart (&device);
                engine.setInputChannel (InputChannel::mix);
                feed (engine, 2, sine.data(), silence.data(), total);

                int framesRead = 0;
                const auto out = drain (engine, total, framesRead);
                expectEquals (framesRead, total);

                const float expected = rmsDb (sine.data(), total) - 6.0206f;
                const float measured = rmsDb (out.getReadPointer (0) + latency, total - latency);
                report ("mix: " + juce::String (measured, 3) + " dBFS, expected "
                        + juce::String (expected, 3));
                expectWithinAbsoluteError (measured, expected, 0.1f);
            }

            // A mono device has no channel 2: "second" falls back to channel 1.
            {
                AudioEngine engine;
                engine.setParams (params);
                FakeInputDevice device (1);
                engine.audioDeviceAboutToStart (&device);
                engine.setInputChannel (InputChannel::second);
                expect (engine.getInputChannel() == InputChannel::first,
                        "a mono device did not report the channel fallback");
                feed (engine, 1, sine.data(), nullptr, total);

                int framesRead = 0;
                const auto out = drain (engine, total, framesRead);
                expectEquals (framesRead, total);

                float worst = 0.0f;
                for (int i = 0; i < total; ++i)
                    worst = std::max (worst, std::abs (out.getSample (0, i)
                                                       - firstBuffer.getSample (0, i)));

                report ("mono second: worst difference " + juce::String (worst, 8));
                expect (worst <= 1.0e-6f, "the mono device did not fall back to channel 1");
            }
        }

        beginTest ("test signal");
        {
            constexpr int total = 48000;   // one second, enough for a frequency count
            const auto params = modulesOff();
            const int latency = chainLatency();

            AudioEngine engine;
            engine.setParams (params);
            engine.setTestSignal (true);

            FakeInputDevice device (1);
            engine.audioDeviceAboutToStart (&device);

            // Whatever the microphone says, the cable gets the test tone.
            const auto noise = whiteNoise (-6.0f, total, 7);
            feed (engine, 1, noise.data(), nullptr, total);

            int framesRead = 0;
            const auto out = drain (engine, total, framesRead);
            expectEquals (framesRead, total);

            const float* data = out.getReadPointer (0);
            const float peak = peakAbs (data + latency, total - latency);
            const float peakDb = juce::Decibels::gainToDecibels (peak, -200.0f);

            int risingCrossings = 0;
            for (int i = latency + 1; i < total; ++i)
                if (data[i - 1] <= 0.0f && data[i] > 0.0f)
                    ++risingCrossings;

            const double seconds = (total - latency - 1) / 48000.0;
            const double frequency = risingCrossings / seconds;

            report ("test signal: peak " + juce::String (peakDb, 2) + " dBFS, "
                    + juce::String (frequency, 1) + " Hz");

            expectWithinAbsoluteError (peakDb, -12.0f, 0.5f);
            expectWithinAbsoluteError (frequency, 440.0, 2.0);
        }

        beginTest ("raw clip flag");
        {
            const auto params = modulesOff();

            AudioEngine engine;
            engine.setParams (params);
            FakeInputDevice device (1);
            engine.audioDeviceAboutToStart (&device);

            std::vector<float> loud (128, 0.0f);
            loud[0] = 1.0f;
            feed (engine, 1, loud.data(), nullptr, (int) loud.size());

            expect (engine.takeInputClipFlag(), "a full-scale sample was not flagged");
            expect (! engine.takeInputClipFlag(), "the clip flag did not reset");

            std::vector<float> quiet (128, 0.0f);
            quiet[0] = 0.5f;
            feed (engine, 1, quiet.data(), nullptr, (int) quiet.size());
            expect (! engine.takeInputClipFlag(), "a quiet sample was flagged");
        }

        beginTest ("latency composition");
        {
            const auto params = modulesOff();
            const int latency = chainLatency();

            AudioEngine engine;
            engine.setParams (params);
            FakeInputDevice device (1);
            engine.audioDeviceAboutToStart (&device);

            std::vector<float> silence (128, 0.0f);
            feed (engine, 1, silence.data(), nullptr, (int) silence.size());

            // 100 samples of driver input latency plus the chain latency, at
            // 48 kHz. The output backlog and device period only add to it.
            const float expected = (100.0f + (float) latency) / 48.0f;
            const float measured = engine.getTotalLatencyMs();

            report ("total latency " + juce::String (measured, 3) + " ms, floor "
                    + juce::String (expected, 3) + " ms");
            expect (measured >= expected - 1.0e-4f, "the latency budget lost a component");
        }
    }
};

static EngineTests engineTests;
