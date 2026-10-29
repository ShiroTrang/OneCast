#include "Presets.h"
#include "Settings.h"
#include "JsonFile.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace
{
    constexpr int presetVersion = 1;

    juce::String utf8 (const char* text)
    {
        return juce::String::fromUTF8 (text);
    }

    bool isBuiltInName (const juce::String& name)
    {
        for (const auto& preset : PresetLibrary::builtIns())
            if (preset.name == name)
                return true;

        return false;
    }

    /// A user preset file and what it holds.
    struct UserFile
    {
        juce::File file;
        Preset preset;
    };

    /// Null for anything that is not a preset file. A file without a name is
    /// named after the file, so one written by hand still shows up.
    std::optional<Preset> readPresetFile (const juce::File& file)
    {
        const auto read = JsonFile::read (file);

        if (read.status != JsonFile::Status::ok)
            return std::nullopt;

        const auto params = read.value.getProperty ("params", {});

        if (params.getDynamicObject() == nullptr)
            return std::nullopt;

        const auto name = read.value.getProperty ("name", {});

        Preset preset;
        preset.name    = (name.isString() ? name.toString() : juce::String()).trim();
        preset.params  = paramsFromVar (params);
        preset.builtIn = false;

        if (preset.name.isEmpty())
            preset.name = file.getFileNameWithoutExtension().trim();

        if (preset.name.isEmpty())
            return std::nullopt;

        return preset;
    }

    /// Every *.json in the folder that is a preset, in file-name order so that
    /// duplicates resolve the same way on every run.
    std::vector<UserFile> readUserFiles (const juce::File& directory)
    {
        std::vector<UserFile> found;

        if (! directory.isDirectory())
            return found;

        std::vector<juce::File> files;

        for (const auto& file : directory.findChildFiles (juce::File::findFiles, false, "*.json"))
            files.push_back (file);

        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
        {
            return a.getFileName().compareIgnoreCase (b.getFileName()) < 0;
        });

        for (const auto& file : files)
            if (auto preset = readPresetFile (file))
                found.push_back ({ file, std::move (*preset) });

        return found;
    }

    /// Windows treats these as devices whatever the extension ("CON.json" writes
    /// to the console), so a preset named "con" needs another file name.
    bool isReservedDeviceName (const juce::String& stem)
    {
        const auto base = stem.upToFirstOccurrenceOf (".", false, false).trimEnd().toUpperCase();

        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL")
            return true;

        return (base.startsWith ("COM") || base.startsWith ("LPT"))
            && base.length() == 4
            && base[3] >= '1' && base[3] <= '9';
    }

    /// The file name (without ".json") for a preset name. createLegalFileName
    /// removes the characters Windows forbids but leaves control characters, can
    /// return nothing at all ("???"), and knows nothing about device names.
    juce::String fileStemFor (const juce::String& name)
    {
        juce::String stem;

        for (const auto c : juce::File::createLegalFileName (name))
            if (c >= 32)
                stem += juce::String::charToString (c);

        stem = stem.trim().trimCharactersAtEnd (". ");

        if (stem.isEmpty())
            stem = "preset";

        if (isReservedDeviceName (stem))
            stem = "_" + stem;

        return stem;
    }

    /// A file name in `directory` that nothing uses yet. Two names can sanitise to
    /// the same stem ("a/b" and "ab"), and NTFS ignores case ("AB" and "ab"), so an
    /// existing file with another preset's name is never overwritten: a number is added.
    juce::File freshFileFor (const juce::File& directory, const juce::String& name)
    {
        const auto stem = fileStemFor (name);
        auto candidate = directory.getChildFile (stem + ".json");

        for (int n = 2; candidate.exists() && n < 1000; ++n)
            candidate = directory.getChildFile (stem + " (" + juce::String (n) + ").json");

        return candidate.exists() ? juce::File() : candidate;
    }
}

//==============================================================================
VoiceParams applyPreset (const VoiceParams& current, const VoiceParams& preset) noexcept
{
    auto result = preset;

    // The mic gain depends on the microphone and interface, not on the voice
    // style, and bypass/mute are the user's live switches: neither may change
    // just because a preset was picked.
    result.preGainDb = current.preGainDb;
    result.bypass    = current.bypass;
    result.mute      = current.mute;

    return result;
}

//==============================================================================
PresetLibrary::PresetLibrary (juce::File userDirectory)
    : directory (std::move (userDirectory))
{
}

juce::File PresetLibrary::getDefaultDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("OneCast")
               .getChildFile ("Presets");
}

std::vector<Preset> PresetLibrary::builtIns()
{
    // Only the differences from the VoiceParams defaults are listed. The default
    // noise mode is Light, so `natural` and `broadcast` run Light; the noisy room
    // is the one preset worth the extra ~25 ms of AI noise suppression.
    Preset natural;
    natural.name = utf8 ("Tự nhiên");
    natural.builtIn = true;

    Preset noisyRoom;
    noisyRoom.name = utf8 ("Phòng ồn");
    noisyRoom.builtIn = true;
    noisyRoom.params.noiseMode = NoiseMode::ai;
    noisyRoom.params.hpfHz = 100.0f;
    noisyRoom.params.gateThresholdDb = -42.0f;

    Preset broadcast;
    broadcast.name = utf8 ("Phát thanh");
    broadcast.builtIn = true;
    broadcast.params.hpfHz = 90.0f;
    broadcast.params.eqMudDb = -3.0f;
    broadcast.params.eqClarityDb = 4.0f;
    broadcast.params.eqAirDb = 2.0f;
    broadcast.params.compThresholdDb = -24.0f;
    broadcast.params.compRatio = 4.0f;
    broadcast.params.compMakeupDb = 6.0f;
    broadcast.params.deEssAmount = 65.0f;

    return { natural, noisyRoom, broadcast };
}

std::vector<Preset> PresetLibrary::getAll() const
{
    auto all = builtIns();
    std::vector<Preset> user;

    for (auto& entry : readUserFiles (directory))
    {
        const auto& name = entry.preset.name;

        if (isBuiltInName (name))
            continue;

        if (std::any_of (user.begin(), user.end(), [&name] (const Preset& p) { return p.name == name; }))
            continue;

        user.push_back (std::move (entry.preset));
    }

    std::sort (user.begin(), user.end(), [] (const Preset& a, const Preset& b)
    {
        const auto order = a.name.compareIgnoreCase (b.name);
        return order != 0 ? order < 0 : a.name.compare (b.name) < 0;
    });

    all.insert (all.end(), std::make_move_iterator (user.begin()), std::make_move_iterator (user.end()));
    return all;
}

std::optional<Preset> PresetLibrary::find (const juce::String& name) const
{
    const auto wanted = name.trim();

    for (auto& preset : getAll())
        if (preset.name == wanted)
            return std::move (preset);

    return std::nullopt;
}

bool PresetLibrary::saveUserPreset (const juce::String& name, const VoiceParams& params)
{
    const auto trimmed = name.trim();

    if (trimmed.isEmpty() || isBuiltInName (trimmed))
        return false;

    // Replace the file that already holds this name, wherever it is called.
    juce::File target;

    for (const auto& entry : readUserFiles (directory))
        if (entry.preset.name == trimmed)
        {
            target = entry.file;
            break;
        }

    if (target == juce::File())
    {
        if (! directory.createDirectory().wasOk())
            return false;

        target = freshFileFor (directory, trimmed);

        if (target == juce::File())
            return false;
    }

    auto* o = new juce::DynamicObject();
    o->setProperty ("format",  "OneCast preset");
    o->setProperty ("version", presetVersion);
    o->setProperty ("name",    trimmed);
    o->setProperty ("params",  paramsToVar (params));

    return JsonFile::writeAtomically (juce::var (o), target);
}

bool PresetLibrary::deleteUserPreset (const juce::String& name)
{
    const auto wanted = name.trim();

    if (wanted.isEmpty() || isBuiltInName (wanted))
        return false;

    // Every file with the name goes, so a copy dropped in by hand cannot bring a
    // deleted preset back.
    bool deletedAny = false;

    for (const auto& entry : readUserFiles (directory))
        if (entry.preset.name == wanted && entry.file.deleteFile())
            deletedAny = true;

    return deletedAny;
}
