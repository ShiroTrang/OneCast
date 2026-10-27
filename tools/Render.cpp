// Offline renderer for the voice chain: the same DSP as the app, but reading a
// WAV file instead of a microphone, so a preset or a noise mode can be compared
// without an audio device or a virtual cable.
//
//   OneCastRender <in.wav> <out.wav> [--preset <name>] [--mode off|light|ai] [--keep-latency]
//
// By default the chain latency is removed, so the output lines up with the
// input sample for sample. The input is followed by enough silence to flush the
// delayed tail, and the written file is truncated back to the input length.
// --keep-latency writes the chain output untouched, at the same length.

#include <JuceHeader.h>

#include "../Source/Dsp/VoiceChain.h"
#include "../Source/Session/Presets.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

namespace
{
    constexpr int blockSize = 128;
    constexpr float silenceDb = -200.0f;

    void printUsage()
    {
        std::printf ("usage: OneCastRender <in.wav> <out.wav> [--preset <name>] "
                     "[--mode off|light|ai] [--keep-latency]\n");
    }

    bool parseMode (const juce::String& text, NoiseMode& mode)
    {
        const auto lower = text.toLowerCase();

        if (lower == "off")   { mode = NoiseMode::off;   return true; }
        if (lower == "light") { mode = NoiseMode::light; return true; }
        if (lower == "ai")    { mode = NoiseMode::ai;    return true; }

        return false;
    }

    /// The built-in preset the app starts with, found by the same name the UI
    /// shows rather than by keeping a second copy of the Vietnamese string here.
    const Preset* findPreset (const juce::String& name, const std::vector<Preset>& presets)
    {
        for (const auto& preset : presets)
            if (preset.name == name)
                return &preset;

        return nullptr;
    }

    float toDb (float gain)
    {
        return juce::Decibels::gainToDecibels (gain, silenceDb);
    }

    /// Reads any sample rate and any channel count, and averages the channels
    /// down to the mono voice path. Returns false and fills `error` on failure.
    bool readMono (const juce::File& file, std::vector<float>& mono,
                   double& sampleRate, juce::String& error)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
        {
            error = "cannot read " + file.getFullPathName();
            return false;
        }

        const auto length = reader->lengthInSamples;

        if (length <= 0 || length > (juce::int64) std::numeric_limits<int>::max())
        {
            error = "unsupported length in " + file.getFullPathName();
            return false;
        }

        juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) length);
        reader->read (&buffer, 0, (int) length, 0, true, true);

        sampleRate = reader->sampleRate;
        mono.assign ((size_t) length, 0.0f);

        const int channels = (int) reader->numChannels;

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto* samples = buffer.getReadPointer (ch);

            for (int i = 0; i < (int) length; ++i)
                mono[(size_t) i] += samples[i];
        }

        if (channels > 1)
        {
            const float scale = 1.0f / (float) channels;

            for (auto& v : mono)
                v *= scale;
        }

        return true;
    }
}

int wmain (int argc, wchar_t** argv)
{
    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (juce::String (argv[i]));

    juce::StringArray positional;
    juce::String presetName;
    juce::String modeText;
    bool keepLatency = false;

    for (int i = 0; i < args.size(); ++i)
    {
        const auto& arg = args[i];

        if (arg == "--keep-latency")
        {
            keepLatency = true;
        }
        else if (arg == "--preset" || arg == "--mode")
        {
            if (i + 1 >= args.size())
            {
                std::printf ("error: %s needs a value\n", arg.toRawUTF8());
                printUsage();
                return 2;
            }

            (arg == "--preset" ? presetName : modeText) = args[++i];
        }
        else if (arg.startsWith ("--"))
        {
            std::printf ("error: unknown option %s\n", arg.toRawUTF8());
            printUsage();
            return 2;
        }
        else
        {
            positional.add (arg);
        }
    }

    if (positional.size() != 2)
    {
        printUsage();
        return 2;
    }

    NoiseMode mode = NoiseMode::light;
    const bool haveMode = modeText.isNotEmpty();

    if (haveMode && ! parseMode (modeText, mode))
    {
        std::printf ("error: unknown mode %s (expected off, light or ai)\n",
                     modeText.toRawUTF8());
        return 2;
    }

    const auto presets = PresetLibrary::builtIns();
    const auto wantedPreset = presetName.isNotEmpty() ? presetName : presets.front().name;
    const auto* preset = findPreset (wantedPreset, presets);

    if (preset == nullptr)
    {
        std::printf ("error: unknown built-in preset %s\n", wantedPreset.toRawUTF8());
        return 2;
    }

    auto params = clampToRanges (preset->params);

    if (haveMode)
        params.noiseMode = mode;

    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const auto inFile = cwd.getChildFile (positional[0]);
    const auto outFile = cwd.getChildFile (positional[1]);

    std::vector<float> mono;
    double sampleRate = 48000.0;
    juce::String error;

    if (! readMono (inFile, mono, sampleRate, error))
    {
        std::printf ("error: %s\n", error.toRawUTF8());
        return 1;
    }

    VoiceChain chain;
    chain.prepare (sampleRate, blockSize);

    // Both paths append before processing, because VoiceChain works in place.
    std::vector<float> out;
    out.reserve (mono.size() + (size_t) std::lround (0.1 * sampleRate) + blockSize);

    int latency = 0;
    bool latencyKnown = false;

    for (int pos = 0; pos < (int) mono.size(); pos += blockSize)
    {
        const int n = std::min (blockSize, (int) mono.size() - pos);
        const size_t at = out.size();

        out.resize (at + (size_t) n);
        std::copy (mono.begin() + pos, mono.begin() + pos + n, out.begin() + (std::ptrdiff_t) at);
        chain.process (out.data() + at, n, params);

        if (! latencyKnown)
        {
            latency = chain.getLatencySamples();
            latencyKnown = true;
        }
    }

    if (! keepLatency)
    {
        // Feed silence until the delayed input tail has left the chain, then
        // drop the delay so output sample i is input sample i.
        const size_t wanted = mono.size() + (size_t) latency;

        while (out.size() < wanted)
        {
            const size_t at = out.size();
            const int n = (int) std::min<size_t> ((size_t) blockSize, wanted - at);

            out.resize (at + (size_t) n, 0.0f);
            chain.process (out.data() + at, n, params);
        }

        out.erase (out.begin(), out.begin() + latency);
    }

    float peak = 0.0f;
    double sumSquares = 0.0;

    for (const float v : out)
    {
        peak = std::max (peak, std::abs (v));
        sumSquares += (double) v * (double) v;
    }

    const double rms = out.empty() ? 0.0 : std::sqrt (sumSquares / (double) out.size());

    outFile.getParentDirectory().createDirectory();
    outFile.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (outFile.createOutputStream());

    if (stream == nullptr)
    {
        std::printf ("error: cannot write %s\n", outFile.getFullPathName().toRawUTF8());
        return 1;
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0));

    if (writer == nullptr)
    {
        std::printf ("error: cannot create a 24-bit WAV writer for %s\n",
                     outFile.getFullPathName().toRawUTF8());
        return 1;
    }

    stream.release();   // the writer owns the stream now

    const float* channels[1] = { out.data() };
    const bool written = writer->writeFromFloatArrays (channels, 1, (int) out.size());
    writer.reset();

    if (! written)
    {
        std::printf ("error: writing %s failed\n", outFile.getFullPathName().toRawUTF8());
        return 1;
    }

    const double latencyMs = sampleRate > 0.0 ? (double) latency * 1000.0 / sampleRate : 0.0;

    std::printf ("latency=%d (%.1f ms) peak=%.2f dBFS rms=%.2f dBFS\n",
                 latency, latencyMs, toDb (peak), toDb ((float) rms));

    return 0;
}
