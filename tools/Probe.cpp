// OneCastProbe: records an input device and reports dropouts, phase jumps and
// level dips in a test sine. The 30 minute soak test plays a 440 Hz sine into
// "CABLE Input" with OneCast and records "CABLE Output" here.
//
//   OneCastProbe --device "CABLE Output" --seconds 60 --sine 440 [--wav out.wav --wav-seconds 30]
//   OneCastProbe --self-test
//
// Exit code: 0 clean, 1 glitches (or the sine never showed up), 2 device not
// found, 3 device could not be opened, 4 bad arguments.
//
// Threads: the WASAPI device runs its own high priority thread and calls us from
// there. That thread only mixes the input down to mono into a lock-free FIFO.
// The main thread drains the FIFO, runs the analyser (which allocates when it
// records a glitch), writes the optional WAV file and prints. --seconds counts
// captured audio, not wall time, so it does not matter which thread the
// callbacks come from; the wall clock is only a watchdog against a stalled
// device.

#include <JuceHeader.h>

#include "ProbeAnalyzer.h"

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr double captureRate = 48000.0;
    constexpr int    drainChunk  = 480;   // 10 ms: also the resolution of the "locked" message

    std::atomic<bool> stopRequested { false };

    extern "C" void onInterrupt (int)   { stopRequested = true; }

    void say (const char* format, ...)
    {
        va_list args;
        va_start (args, format);
        std::vprintf (format, args);
        va_end (args);
    }

    //==========================================================================
    struct Options
    {
        juce::String device { "CABLE Output" };
        double seconds = 60.0;
        double sine = 440.0;
        juce::String wavPath;
        double wavSeconds = 30.0;
        bool selfTest = false;
        bool help = false;
        juce::String error;
    };

    bool isPositiveNumber (const juce::String& s)
    {
        return s.isNotEmpty() && s.containsOnly ("0123456789.") && s.getDoubleValue() > 0.0;
    }

    Options parseArguments (int argc, char** argv)
    {
        Options o;

        for (int i = 1; i < argc && o.error.isEmpty(); ++i)
        {
            const juce::String arg (argv[i]);

            auto value = [&]() -> juce::String
            {
                if (i + 1 >= argc)
                {
                    o.error = arg + " needs a value";
                    return {};
                }

                return juce::String (argv[++i]);
            };

            auto number = [&]() -> double
            {
                const auto text = value();

                if (o.error.isEmpty() && ! isPositiveNumber (text))
                    o.error = arg + " needs a positive number, got \"" + text + "\"";

                return text.getDoubleValue();
            };

            if      (arg == "--device")       o.device = value();
            else if (arg == "--seconds")      o.seconds = number();
            else if (arg == "--sine")         o.sine = number();
            else if (arg == "--wav")          o.wavPath = value();
            else if (arg == "--wav-seconds")  o.wavSeconds = number();
            else if (arg == "--self-test")    o.selfTest = true;
            else if (arg == "--help" || arg == "-h" || arg == "/?") o.help = true;
            else o.error = "unknown argument \"" + arg + "\"";
        }

        if (o.error.isEmpty() && o.device.isEmpty())
            o.error = "--device must not be empty";

        // The analyser needs a pitch well below Nyquist and above the mains hum.
        if (o.error.isEmpty() && (o.sine < 20.0 || o.sine > 0.45 * captureRate))
            o.error = "--sine must be between 20 and " + juce::String (0.45 * captureRate, 0) + " Hz";

        return o;
    }

    void printUsage()
    {
        say ("usage: OneCastProbe --device \"CABLE Output\" --seconds 60 --sine 440 [--wav out.wav --wav-seconds 30]\n"
             "       OneCastProbe --self-test\n"
             "\n"
             "Records the first input device (Windows Audio, 48 kHz) whose name contains --device,\n"
             "mixes it to mono and reports dropouts, phase jumps and level dips in the test sine.\n"
             "Prints one line per glitch and a final line: seconds=... level=...dB glitches=N\n"
             "Exit code: 0 clean, 1 glitches or no sine found, 2 device not found,\n"
             "           3 device could not be opened, 4 bad arguments.\n");
    }

    //==========================================================================
    /// Mixes the device's input channels down to mono into a FIFO. Real-time
    /// safe: no allocation, no lock, no I/O. A full FIFO means the main thread
    /// stalled; the block is dropped and counted so the run is not trusted.
    class CaptureCallback final : public juce::AudioIODeviceCallback
    {
    public:
        explicit CaptureCallback (int capacity)
            : fifo (capacity), storage ((size_t) capacity, 0.0f) {}

        void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
        void audioDeviceStopped() override {}

        void audioDeviceError (const juce::String& message) override
        {
            const juce::SpinLock::ScopedLockType lock (errorLock);
            errorMessage = message;
            hasError = true;
        }

        void audioDeviceIOCallbackWithContext (const float* const* input, int numInputChannels,
                                               float* const* output, int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (output[ch] != nullptr)
                    juce::FloatVectorOperations::clear (output[ch], numSamples);

            if (numSamples <= 0)
                return;

            deliveredSamples += numSamples;

            if (fifo.getFreeSpace() < numSamples)
            {
                overrunSamples += numSamples;
                return;
            }

            int active = 0;
            for (int ch = 0; ch < numInputChannels; ++ch)
                if (input[ch] != nullptr)
                    ++active;

            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo.prepareToWrite (numSamples, start1, size1, start2, size2);

            // Mean of the channels, so L = R = s comes out as exactly s.
            const float scale = active > 0 ? 1.0f / (float) active : 0.0f;

            auto mix = [&] (int destination, int count, int sourceOffset)
            {
                for (int i = 0; i < count; ++i)
                {
                    float sum = 0.0f;

                    for (int ch = 0; ch < numInputChannels; ++ch)
                        if (input[ch] != nullptr)
                            sum += input[ch][sourceOffset + i];

                    storage[(size_t) (destination + i)] = sum * scale;
                }
            };

            mix (start1, size1, 0);
            mix (start2, size2, size1);
            fifo.finishedWrite (size1 + size2);
        }

        /// Main thread: copies up to `maxSamples` out. Returns how many.
        int read (float* destination, int maxSamples)
        {
            const int n = juce::jmin (maxSamples, fifo.getNumReady());

            if (n <= 0)
                return 0;

            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo.prepareToRead (n, start1, size1, start2, size2);

            std::copy_n (storage.begin() + start1, size1, destination);
            std::copy_n (storage.begin() + start2, size2, destination + size1);
            fifo.finishedRead (size1 + size2);
            return n;
        }

        juce::String getError() const
        {
            const juce::SpinLock::ScopedLockType lock (errorLock);
            return hasError ? errorMessage : juce::String();
        }

        std::atomic<juce::int64> deliveredSamples { 0 }, overrunSamples { 0 };

    private:
        juce::AbstractFifo fifo;
        std::vector<float> storage;
        mutable juce::SpinLock errorLock;
        juce::String errorMessage;
        bool hasError = false;
    };

    //==========================================================================
    void printGlitch (const ProbeAnalyzer::Glitch& g)
    {
        say ("glitch t=%.3fs kind=%s duration=%.1fms\n",
             g.timeSeconds, g.kind.toRawUTF8(), g.durationSeconds * 1000.0);
    }

    /// Synthetic 10 s sine, 20 zero samples at 3.0 s, a half-cycle phase jump at
    /// 6.0 s: the analyser must report exactly these two and nothing else.
    int runSelfTest()
    {
        constexpr double sr = 48000.0, hz = 440.0;
        constexpr int total = (int) (10.0 * sr);
        constexpr double w = 2.0 * 3.14159265358979323846 * hz / sr;

        std::vector<float> x ((size_t) total);

        for (int i = 0; i < total; ++i)
        {
            const double phase = i >= (int) (6.0 * sr) ? 3.14159265358979323846 : 0.0;
            x[(size_t) i] = 0.5f * (float) std::sin (w * i + phase);
        }

        for (int i = (int) (3.0 * sr); i < (int) (3.0 * sr) + 20; ++i)
            x[(size_t) i] = 0.0f;

        ProbeAnalyzer analyzer (sr, hz);

        // Fed in device-sized blocks, as the real capture is.
        for (int start = 0; start < total; start += drainChunk)
            analyzer.process (x.data() + start, juce::jmin (drainChunk, total - start));

        const auto& glitches = analyzer.getGlitches();

        for (const auto& g : glitches)
            printGlitch (g);

        const bool pass = analyzer.isLocked()
                       && glitches.size() == 2
                       && glitches[0].kind == "dropout" && std::abs (glitches[0].timeSeconds - 3.0) < 0.01
                       && glitches[1].kind == "jump"    && std::abs (glitches[1].timeSeconds - 6.0) < 0.01;

        say ("seconds=%.2f level=%.1fdB glitches=%d\n",
             analyzer.getSecondsAnalysed(), (double) analyzer.getLevelDb(), (int) glitches.size());
        say ("self-test %s: expected a dropout at 3.0 s and a jump at 6.0 s, nothing else\n",
             pass ? "PASS" : "FAIL");

        return pass ? 0 : 1;
    }

    //==========================================================================
    int runCapture (const Options& o)
    {
        // Straight to the Windows Audio (WASAPI shared) device type: no
        // AudioDeviceManager, which would also open the default output device.
        std::unique_ptr<juce::AudioIODeviceType> type (
            juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));

        if (type == nullptr)
        {
            say ("error: the Windows Audio device type is not available in this build\n");
            return 3;
        }

        type->scanForDevices();
        const auto inputNames = type->getDeviceNames (true);

        int index = -1;
        for (int i = 0; i < inputNames.size(); ++i)
            if (inputNames[i].containsIgnoreCase (o.device)) { index = i; break; }

        if (index < 0)
        {
            say ("error: no %s input device contains \"%s\". Input devices:\n",
                 type->getTypeName().toRawUTF8(), o.device.toRawUTF8());

            for (const auto& name : inputNames)
                say ("  %s\n", name.toRawUTF8());

            return 2;
        }

        const juce::String deviceName = inputNames[index];

        // Input only: an empty output name never opens a render endpoint.
        std::unique_ptr<juce::AudioIODevice> device (type->createDevice ({}, deviceName));

        if (device == nullptr)
        {
            say ("error: could not create \"%s\"\n", deviceName.toRawUTF8());
            return 3;
        }

        juce::BigInteger channels;
        channels.setRange (0, juce::jmax (1, device->getInputChannelNames().size()), true);

        const auto openError = device->open (channels, juce::BigInteger(), captureRate, 0);

        if (openError.isNotEmpty() || ! device->isOpen())
        {
            say ("error: could not open \"%s\": %s\n", deviceName.toRawUTF8(),
                 openError.isNotEmpty() ? openError.toRawUTF8() : device->getLastError().toRawUTF8());
            return 3;
        }

        const double rate = device->getCurrentSampleRate();
        const int numChannels = device->getActiveInputChannels().countNumberOfSetBits();

        say ("device=\"%s\" type=\"%s\" rate=%.0f channels=%d buffer=%d sine=%.1fHz seconds=%.1f\n",
             deviceName.toRawUTF8(), type->getTypeName().toRawUTF8(), rate, numChannels,
             device->getCurrentBufferSizeSamples(), o.sine, o.seconds);

        if (std::abs (rate - captureRate) > 0.5)
            say ("note: the device runs at %.0f Hz, not 48000 Hz; analysing at that rate\n", rate);

        // Up to 20 s of slack for a main thread that is busy writing a file.
        CaptureCallback callback ((int) (rate * 20.0));
        ProbeAnalyzer analyzer (rate, o.sine);

        // Optional recording of the mono mix (32-bit float WAV, the first N seconds).
        std::unique_ptr<juce::AudioFormatWriter> wavWriter;
        juce::int64 wavSamplesLeft = 0;

        if (o.wavPath.isNotEmpty())
        {
            const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (o.wavPath);
            file.deleteFile();
            std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());

            if (stream != nullptr)
            {
                juce::WavAudioFormat wav;
                wavWriter.reset (wav.createWriterFor (stream.get(), rate, 1, 32, {}, 0));

                if (wavWriter != nullptr)
                    stream.release();   // the writer owns it now
            }

            if (wavWriter == nullptr)
                say ("warning: could not write \"%s\", continuing without a WAV file\n", file.getFullPathName().toRawUTF8());
            else
                wavSamplesLeft = (juce::int64) std::llround (juce::jmin (o.wavSeconds, o.seconds) * rate);
        }

        std::signal (SIGINT, onInterrupt);

        const juce::int64 targetSamples = (juce::int64) std::llround (o.seconds * rate);
        juce::int64 processed = 0;
        size_t printed = 0;
        bool lockReported = false;
        std::vector<float> chunk ((size_t) drainChunk);

        const double startMs = juce::Time::getMillisecondCounterHiRes();
        const double deadlineMs = (o.seconds * 1.1 + 5.0) * 1000.0;

        device->start (&callback);

        std::string stopReason;

        for (;;)
        {
            // Everything captured so far, up to the requested length.
            while (processed < targetSamples)
            {
                const int n = callback.read (chunk.data(), (int) juce::jmin<juce::int64> (drainChunk, targetSamples - processed));

                if (n == 0)
                    break;

                analyzer.process (chunk.data(), n);
                processed += n;

                if (wavWriter != nullptr && wavSamplesLeft > 0)
                {
                    const float* channel = chunk.data();
                    const int toWrite = (int) juce::jmin<juce::int64> (n, wavSamplesLeft);
                    wavWriter->writeFromFloatArrays (&channel, 1, toWrite);
                    wavSamplesLeft -= toWrite;

                    if (wavSamplesLeft == 0)
                        wavWriter.reset();   // closes and finalises the file
                }

                if (! lockReported && analyzer.isLocked())
                {
                    lockReported = true;
                    say ("locked on the sine at about %.2fs, level=%.1fdB\n",
                         analyzer.getSecondsAnalysed(), (double) analyzer.getLevelDb());
                }

                for (; printed < analyzer.getSettledGlitchCount(); ++printed)
                    printGlitch (analyzer.getGlitches()[printed]);
            }

            if (processed >= targetSamples)
                break;

            if (stopRequested)                       { stopReason = "interrupted";  break; }
            if (callback.getError().isNotEmpty())    { stopReason = "device error: " + callback.getError().toStdString(); break; }
            if (! device->isOpen())                  { stopReason = "the capture thread stopped (device removed or silent for 1 s)"; break; }

            if (juce::Time::getMillisecondCounterHiRes() - startMs > deadlineMs)
            {
                stopReason = "capture stalled";
                break;
            }

            juce::Thread::sleep (10);
        }

        device->stop();
        const int xruns = device->getXRunCount();
        device->close();
        wavWriter.reset();

        for (; printed < analyzer.getGlitches().size(); ++printed)
            printGlitch (analyzer.getGlitches()[printed]);

        // Anything that makes the run untrustworthy counts as a glitch, so the
        // summary line and the exit code can never say "clean" for it.
        int problems = 0;

        if (! analyzer.isLocked())
        {
            ++problems;
            say ("problem: no steady %.1f Hz sine found in %.2fs of input (rms %.1fdB). Is the test signal playing?\n",
                 o.sine, analyzer.getSecondsAnalysed(), (double) analyzer.getLevelDb());
        }

        if (callback.overrunSamples > 0)
        {
            ++problems;
            say ("problem: the probe dropped %lld samples of its own (main thread too slow); this run is not valid\n",
                 (long long) callback.overrunSamples.load());
        }

        if (stopReason == "interrupted")
        {
            say ("note: interrupted after %.2fs of %.1fs\n", analyzer.getSecondsAnalysed(), o.seconds);
        }
        else if (! stopReason.empty())
        {
            ++problems;
            say ("problem: %s after %.2fs of %.1fs\n", stopReason.c_str(), analyzer.getSecondsAnalysed(), o.seconds);
        }

        if (xruns > 0)
            say ("note: the Windows Audio capture reported %d xrun(s)\n", xruns);

        const int total = (int) analyzer.getGlitches().size() + problems;

        say ("seconds=%.2f level=%.1fdB glitches=%d\n",
             analyzer.getSecondsAnalysed(), (double) analyzer.getLevelDb(), total);

        return total == 0 ? 0 : 1;
    }
}

int main (int argc, char** argv)
{
    // Creates the message manager (and initialises COM) that the device types
    // rely on. No message loop has to run: WASAPI delivers on its own thread.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    // Unbuffered, so a log redirected to a file is current if the run is killed.
    std::setvbuf (stdout, nullptr, _IONBF, 0);

    const auto options = parseArguments (argc, argv);

    if (options.help)
    {
        printUsage();
        return 0;
    }

    if (options.error.isNotEmpty())
    {
        say ("error: %s\n\n", options.error.toRawUTF8());
        printUsage();
        return 4;
    }

    return options.selfTest ? runSelfTest() : runCapture (options);
}
