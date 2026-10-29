#include "Settings.h"
#include "JsonFile.h"

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace
{
    constexpr int settingsVersion = 1;

    /// A float as the double a human would have typed. Widening 13.37f gives
    /// 13.3699998855..., and the file would show all of it. The shortest decimal
    /// that still reads back as the same float is "13.37", and parsing that into
    /// a double and narrowing again returns exactly the float we started from.
    juce::var floatToVar (float value)
    {
        auto result = static_cast<double> (value);

        char buffer[32];
        const auto written = std::to_chars (buffer, buffer + sizeof (buffer), value);

        if (written.ec == std::errc())
        {
            double shortest = 0.0;

            if (std::from_chars (buffer, written.ptr, shortest).ec == std::errc())
                result = shortest;
        }

        return juce::var (result);
    }

    bool isNumber (const juce::var& v)
    {
        return v.isDouble() || v.isInt() || v.isInt64();
    }

    /// The value under `key`, or `fallback` when it is missing, not a number, or
    /// not finite. An overflowing 1e400 would otherwise become an infinity here,
    /// and narrowing a double beyond FLT_MAX is undefined behaviour.
    float readFloat (const juce::var& object, const char* key, float fallback)
    {
        const auto v = object.getProperty (key, {});

        if (! isNumber (v))
            return fallback;

        const auto d = static_cast<double> (v);

        if (! std::isfinite (d))
            return fallback;

        return static_cast<float> (std::clamp (d, -static_cast<double> (FLT_MAX), static_cast<double> (FLT_MAX)));
    }

    /// The output period hint: -1 is a sentinel meaning "use the standard
    /// shared rung", not an error. Any other negative, a non-number or an
    /// overflow is no hint (0).
    int readPeriodHint (const juce::var& object, const char* key, int fallback)
    {
        const auto v = object.getProperty (key, {});

        if (! isNumber (v))
            return fallback;

        const auto d = static_cast<double> (v);

        if (! std::isfinite (d) || d > (double) std::numeric_limits<int>::max())
            return fallback;

        if (d == -1.0)
            return -1;

        if (d < 0.0)
            return 0;

        return (int) d;
    }

    bool readBool (const juce::var& object, const char* key, bool fallback)
    {
        const auto v = object.getProperty (key, {});
        return v.isBool() ? static_cast<bool> (v) : fallback;
    }

    juce::String readString (const juce::var& object, const char* key, const juce::String& fallback)
    {
        const auto v = object.getProperty (key, {});
        return v.isString() ? v.toString() : fallback;
    }

    // Enums are stored by name, not number: a hand-edited file stays readable and
    // the numeric values of the enums can never silently change meaning.
    const char* toName (NoiseMode mode)
    {
        switch (mode)
        {
            case NoiseMode::off:    return "off";
            case NoiseMode::light:  return "light";
            case NoiseMode::ai:     return "ai";
        }

        return "light";  // out of range: the name of the default
    }

    NoiseMode readNoiseMode (const juce::var& object, const char* key, NoiseMode fallback)
    {
        const auto name = readString (object, key, {});

        for (const auto mode : { NoiseMode::off, NoiseMode::light, NoiseMode::ai })
            if (name == toName (mode))
                return mode;

        return fallback;
    }

    const char* toName (InputChannel channel)
    {
        switch (channel)
        {
            case InputChannel::first:   return "first";
            case InputChannel::second:  return "second";
            case InputChannel::mix:     return "mix";
        }

        return "first";
    }

    InputChannel readInputChannel (const juce::var& object, const char* key, InputChannel fallback)
    {
        const auto name = readString (object, key, {});

        for (const auto channel : { InputChannel::first, InputChannel::second, InputChannel::mix })
            if (name == toName (channel))
                return channel;

        return fallback;
    }
}

//==============================================================================
juce::var paramsToVar (const VoiceParams& params)
{
    const auto p = clampToRanges (params);

    auto* o = new juce::DynamicObject();

    o->setProperty ("preGainDb",       floatToVar (p.preGainDb));
    o->setProperty ("hpfOn",           p.hpfOn);
    o->setProperty ("hpfHz",           floatToVar (p.hpfHz));
    o->setProperty ("noiseMode",       toName (p.noiseMode));
    o->setProperty ("noiseAmount",     floatToVar (p.noiseAmount));
    o->setProperty ("gateOn",          p.gateOn);
    o->setProperty ("gateThresholdDb", floatToVar (p.gateThresholdDb));
    o->setProperty ("eqOn",            p.eqOn);
    o->setProperty ("eqMudDb",         floatToVar (p.eqMudDb));
    o->setProperty ("eqClarityDb",     floatToVar (p.eqClarityDb));
    o->setProperty ("eqAirDb",         floatToVar (p.eqAirDb));
    o->setProperty ("compOn",          p.compOn);
    o->setProperty ("compThresholdDb", floatToVar (p.compThresholdDb));
    o->setProperty ("compRatio",       floatToVar (p.compRatio));
    o->setProperty ("compMakeupDb",    floatToVar (p.compMakeupDb));
    o->setProperty ("deEssOn",         p.deEssOn);
    o->setProperty ("deEssAmount",     floatToVar (p.deEssAmount));
    o->setProperty ("outputGainDb",    floatToVar (p.outputGainDb));

    return juce::var (o);
}

VoiceParams paramsFromVar (const juce::var& v)
{
    // Every member starts at its default, so a missing key keeps it.
    VoiceParams p;

    p.preGainDb       = readFloat (v, "preGainDb",       p.preGainDb);
    p.hpfOn           = readBool  (v, "hpfOn",           p.hpfOn);
    p.hpfHz           = readFloat (v, "hpfHz",           p.hpfHz);
    p.noiseMode       = readNoiseMode (v, "noiseMode",   p.noiseMode);
    p.noiseAmount     = readFloat (v, "noiseAmount",     p.noiseAmount);
    p.gateOn          = readBool  (v, "gateOn",          p.gateOn);
    p.gateThresholdDb = readFloat (v, "gateThresholdDb", p.gateThresholdDb);
    p.eqOn            = readBool  (v, "eqOn",            p.eqOn);
    p.eqMudDb         = readFloat (v, "eqMudDb",         p.eqMudDb);
    p.eqClarityDb     = readFloat (v, "eqClarityDb",     p.eqClarityDb);
    p.eqAirDb         = readFloat (v, "eqAirDb",         p.eqAirDb);
    p.compOn          = readBool  (v, "compOn",          p.compOn);
    p.compThresholdDb = readFloat (v, "compThresholdDb", p.compThresholdDb);
    p.compRatio       = readFloat (v, "compRatio",       p.compRatio);
    p.compMakeupDb    = readFloat (v, "compMakeupDb",    p.compMakeupDb);
    p.deEssOn         = readBool  (v, "deEssOn",         p.deEssOn);
    p.deEssAmount     = readFloat (v, "deEssAmount",     p.deEssAmount);
    p.outputGainDb    = readFloat (v, "outputGainDb",    p.outputGainDb);

    // bypass and mute are deliberately not read (see Settings.h).
    return clampToRanges (p);
}

//==============================================================================
namespace SettingsIO
{

juce::File getDefaultFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("OneCast")
               .getChildFile ("settings.json");
}

juce::var toVar (const AppSettings& settings)
{
    auto* o = new juce::DynamicObject();

    o->setProperty ("format",           "OneCast settings");
    o->setProperty ("version",          settingsVersion);
    o->setProperty ("deviceStateXml",   settings.deviceStateXml);
    o->setProperty ("inputChannel",     toName (settings.inputChannel));
    o->setProperty ("outputDeviceId",   settings.outputDeviceId);
    o->setProperty ("outputDeviceName", settings.outputDeviceName);
    o->setProperty ("presetName",       settings.presetName);
    o->setProperty ("startWithWindows", settings.startWithWindows);
    o->setProperty ("startMinimized",   settings.startMinimized);
    o->setProperty ("muteHotkey",       settings.muteHotkey);
    o->setProperty ("outputPeriodHintFrames", settings.outputPeriodHintFrames);
    o->setProperty ("params",           paramsToVar (settings.params));

    return juce::var (o);
}

AppSettings fromVar (const juce::var& v)
{
    AppSettings s;

    // var::isObject() is also true for an array, which has no keys to read.
    if (v.getDynamicObject() == nullptr)
        return s;

    s.deviceStateXml   = readString (v, "deviceStateXml",   s.deviceStateXml);
    s.inputChannel     = readInputChannel (v, "inputChannel", s.inputChannel);
    s.outputDeviceId   = readString (v, "outputDeviceId",   s.outputDeviceId);
    s.outputDeviceName = readString (v, "outputDeviceName", s.outputDeviceName);
    s.presetName       = readString (v, "presetName",       s.presetName);
    s.startWithWindows = readBool   (v, "startWithWindows", s.startWithWindows);
    s.startMinimized   = readBool   (v, "startMinimized",   s.startMinimized);
    s.muteHotkey       = readString (v, "muteHotkey",       s.muteHotkey);
    s.outputPeriodHintFrames = readPeriodHint (v, "outputPeriodHintFrames",
                                               s.outputPeriodHintFrames);
    s.params           = paramsFromVar (v.getProperty ("params", {}));

    return s;
}

bool save (const AppSettings& settings, const juce::File& file)
{
    return JsonFile::writeAtomically (toVar (settings), file);
}

AppSettings load (const juce::File& file)
{
    const auto read = JsonFile::read (file);

    switch (read.status)
    {
        case JsonFile::Status::ok:
            return fromVar (read.value);

        case JsonFile::Status::damaged:
        {
            // Keep what the user had: the next save would otherwise overwrite it.
            const auto bad = file.getSiblingFile (file.getFileName() + ".bad");
            bad.deleteFile();
            file.moveFileTo (bad);
            return {};
        }

        case JsonFile::Status::missing:
        case JsonFile::Status::unreadable:
            break;
    }

    return {};
}

} // namespace SettingsIO
