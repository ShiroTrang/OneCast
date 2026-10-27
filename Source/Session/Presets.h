#pragma once
#include <JuceHeader.h>

#include "../Dsp/DspParams.h"

#include <optional>
#include <vector>

struct Preset
{
    juce::String name;
    VoiceParams params;
    bool builtIn = false;
};

/// Presets carry processing only: the result keeps current.preGainDb (it depends on the
/// mic and interface), current.bypass and current.mute.
VoiceParams applyPreset (const VoiceParams& current, const VoiceParams& preset) noexcept;

/// The three built-in presets plus the user's own, one JSON file each. Message
/// thread only. Nothing is cached: every call reads the folder, so a preset
/// dropped in by hand shows up on the next call.
///
/// Names are compared exactly (after trimming surrounding spaces). Case-folding
/// Vietnamese is not reliable with the C runtime's tolower, so "tự nhiên" is a
/// different preset from "Tự nhiên".
class PresetLibrary
{
public:
    explicit PresetLibrary (juce::File userDirectory);

    /// %APPDATA%\OneCast\Presets
    static juce::File getDefaultDirectory();

    /// Tự nhiên, Phòng ồn, Phát thanh, in that order. Read-only.
    static std::vector<Preset> builtIns();

    /// Built-ins first, then user presets by name. A user file named like a
    /// built-in is ignored (the built-in wins), and so is a file that is not a
    /// preset; two files with the same name show up once.
    std::vector<Preset> getAll() const;

    std::optional<Preset> find (const juce::String& name) const;

    /// Creates the folder if needed and overwrites the preset of the same name.
    /// False for an empty name, a built-in name, or when the file cannot be written.
    /// The file is named from the sanitised name, so "a/b" and "ab" end up in
    /// different files; the JSON keeps the real name. Bypass and mute are not stored.
    bool saveUserPreset (const juce::String& name, const VoiceParams&);

    /// Deletes the file of a user preset. False for a built-in, an unknown name
    /// or when the file cannot be deleted.
    bool deleteUserPreset (const juce::String& name);

private:
    juce::File directory;
};
