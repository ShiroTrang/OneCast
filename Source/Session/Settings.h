#pragma once
#include <JuceHeader.h>

#include "../Dsp/DspParams.h"

/// Everything OneCast remembers between runs. Plain data: the message thread
/// owns it, fills it from the UI and the audio engine, and hands it to SettingsIO.
struct AppSettings
{
    juce::String deviceStateXml;          // AudioDeviceManager::createStateXml()->toString(), "" = never chosen
    InputChannel inputChannel = InputChannel::first;
    juce::String outputDeviceId;          // WASAPI endpoint id; "" = auto-pick CABLE Input
    juce::String outputDeviceName;        // re-find by name when the id changed
    VoiceParams  params;                  // bypass and mute are not persisted (always false on load)
    juce::String presetName;              // UTF-8, "" = none
    bool startWithWindows = false;
    bool startMinimized   = false;
    juce::String muteHotkey { "Ctrl+Alt+M" };

    /// IAudioClient3 period that stayed glitch-free last run, in frames.
    /// 0 = no hint, start at the endpoint minimum; negative and unreadable
    /// values load as 0.
    int outputPeriodHintFrames = 0;

    bool operator== (const AppSettings&) const = default;
};

/// The voice parameters as a JSON object, keyed by the VoiceParams member names.
/// Shared by the settings file and the preset files. Bypass and mute are left
/// out on purpose: restarting the app, or applying a preset, must never leave
/// the user muted or unprocessed without knowing why. Values are clamped
/// first, so a stray NaN or out-of-range number never reaches the disk.
juce::var   paramsToVar   (const VoiceParams&);

/// The inverse, forgiving: a missing key or one of the wrong JSON type takes the
/// VoiceParams default, then everything is run through clampToRanges. Enums are
/// read by name ("off", "light", "ai"); an unknown name is the default, light. Bypass
/// and mute are always false, even if a hand-edited file says otherwise.
VoiceParams paramsFromVar (const juce::var&);

namespace SettingsIO
{
    /// %APPDATA%\OneCast\settings.json
    juce::File  getDefaultFile();

    juce::var   toVar   (const AppSettings&);

    /// Same tolerance as paramsFromVar: damage costs the field, never the whole file.
    AppSettings fromVar (const juce::var&);

    /// Writes <file>.tmp, then replaces the file with it, creating the folder if needed.
    bool        save (const AppSettings&, const juce::File&);

    /// A missing file gives the defaults. A file that cannot be parsed also gives
    /// the defaults, and is renamed to <file>.bad (replacing an older .bad) so
    /// the user's data is not overwritten by the next save. A file that merely
    /// cannot be opened right now is left alone.
    AppSettings load (const juce::File&);
}
