// Unit tests for what OneCast remembers between runs: the settings file and the
// presets. Every test works in its own temporary directory, so the real
// %APPDATA%\OneCast is never read or written.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe session

#include <JuceHeader.h>

#include "../Source/Audio/WasapiOutputDevice.h"
#include "../Source/Dsp/DspParams.h"
#include "../Source/Session/JsonFile.h"
#include "../Source/Session/Settings.h"
#include "../Source/Session/Presets.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace
{
    /// A fresh directory under the system temp folder, deleted with its contents
    /// when the test is done.
    struct ScopedTempDir
    {
        ScopedTempDir()
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("OneCastTests-" + juce::Uuid().toString()))
        {
            dir.createDirectory();
        }

        ~ScopedTempDir() { dir.deleteRecursively(); }

        juce::File dir;
    };

    juce::String utf8 (const char* text) { return juce::String::fromUTF8 (text); }

    /// Names of the files (not folders) directly inside a directory, sorted.
    juce::StringArray fileNames (const juce::File& directory)
    {
        juce::StringArray names;

        for (const auto& file : directory.findChildFiles (juce::File::findFiles, false))
            names.add (file.getFileName());

        names.sort (false);
        return names;
    }

    /// Writes raw bytes, so a test can produce a byte-order mark or invalid UTF-8.
    void writeBytes (const juce::File& file, const juce::String& text, bool withBom)
    {
        juce::MemoryBlock bytes;

        if (withBom)
        {
            const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
            bytes.append (bom, sizeof (bom));
        }

        bytes.append (text.toRawUTF8(), text.getNumBytesAsUTF8());
        file.replaceWithData (bytes.getData(), bytes.getSize());
    }

    /// Every field differs from its default, so a field that is not written or
    /// not read shows up as a mismatch. bypass and mute are on: they must not survive.
    VoiceParams unusualParams()
    {
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
        return p;
    }

    AppSettings unusualSettings()
    {
        AppSettings s;
        // Quotes, angle brackets, a backslash, a newline and non-ASCII text: all of
        // it has to survive being embedded in a JSON string.
        s.deviceStateXml = utf8 ("<DEVICESETTINGS deviceType=\"Windows Audio\" "
                                 "audioInputDeviceName=\"Micrô &amp; loa (Realtek®)\" "
                                 "path=\"C:\\Users\\Nhân\"/>\n<!-- \"hai\" dòng -->");
        s.inputChannel     = InputChannel::mix;
        s.outputDeviceId   = "{0.0.0.00000000}.{3b2d9a1c-7f44-4d2e-9d3e-5c1f0a8e2b77}";
        s.outputDeviceName = "CABLE Input (VB-Audio Virtual Cable)";
        s.params           = unusualParams();
        s.presetName       = utf8 ("Của tôi");
        s.startWithWindows = true;
        s.startMinimized   = true;
        s.muteHotkey       = "Ctrl+Shift+F9";
        s.outputPeriodHintFrames = 144;
        return s;
    }

    /// What unusualSettings() must come back as: bypass and mute are never persisted.
    AppSettings unusualSettingsAsLoaded()
    {
        auto s = unusualSettings();
        s.params.bypass = false;
        s.params.mute = false;
        return s;
    }

    /// A user preset file as the app writes it, for tests that need one on disk.
    void writePresetFile (const juce::File& file, const juce::String& name, const juce::var& params)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("name", name);
        object->setProperty ("params", params);
        file.replaceWithText (JsonFile::toText (juce::var (object)), false, false);
    }
}

// =============================================================================
class ParamsJsonTests final : public juce::UnitTest
{
public:
    ParamsJsonTests() : juce::UnitTest ("Params JSON", "session") {}

    void runTest() override
    {
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

        beginTest ("params survive a trip through JSON text, bit for bit");
        {
            // Random values in range: float -> text -> float must be exact, or a
            // preset drifts a little every time it is saved and loaded.
            juce::Random rng (20261027);
            bool allExact = true;

            for (int round = 0; round < 300; ++round)
            {
                VoiceParams p;
                for (const auto& [member, range] : floats)
                    p.*member = range.min + rng.nextFloat() * (range.max - range.min);

                p.hpfOn = rng.nextBool();
                p.gateOn = rng.nextBool();
                p.eqOn = rng.nextBool();
                p.compOn = rng.nextBool();
                p.deEssOn = rng.nextBool();
                p.noiseMode = (NoiseMode) rng.nextInt (3);

                const auto text = JsonFile::toText (paramsToVar (p));
                allExact = allExact && paramsFromVar (juce::JSON::parse (text)) == p;
            }

            expect (allExact, "a random VoiceParams changed in a JSON round trip");
        }

        beginTest ("the text stays readable and bypass and mute are not written");
        {
            const auto text = JsonFile::toText (paramsToVar (unusualParams()));

            expect (text.contains ("13.37") && ! text.contains ("13.3699"), text);
            expect (text.contains ("\"ai\""), "noise mode is written by name");
            expect (! text.containsIgnoreCase ("bypass") && ! text.containsIgnoreCase ("mute"), text);
        }

        beginTest ("paramsToVar never writes a NaN or an out-of-range number");
        {
            VoiceParams bad;
            bad.preGainDb = std::numeric_limits<float>::quiet_NaN();
            bad.hpfHz = 1.0e9f;
            bad.noiseMode = (NoiseMode) 9;

            const auto back = paramsFromVar (juce::JSON::parse (JsonFile::toText (paramsToVar (bad))));
            expectEquals (back.preGainDb, ParamRanges::preGainDb.defaultValue);
            expectEquals (back.hpfHz, ParamRanges::hpfHz.max);
            expect (back.noiseMode == NoiseMode::light, "an invalid mode comes back as the default");
        }

        beginTest ("the three mode names are read as themselves, ai included");
        {
            // The default is light, so an explicit "ai" must not fall into the
            // same branch as an unknown name.
            expect (paramsFromVar (juce::JSON::parse (R"({"noiseMode": "off"})")).noiseMode == NoiseMode::off);
            expect (paramsFromVar (juce::JSON::parse (R"({"noiseMode": "light"})")).noiseMode == NoiseMode::light);
            expect (paramsFromVar (juce::JSON::parse (R"({"noiseMode": "ai"})")).noiseMode == NoiseMode::ai);
            expect (paramsFromVar (juce::JSON::parse (R"({})")).noiseMode == NoiseMode::light, "missing key: default");

            const auto settings = SettingsIO::fromVar (juce::JSON::parse (R"({"params": {"noiseMode": "ai"}})"));
            expect (settings.params.noiseMode == NoiseMode::ai);
            expect (AppSettings {}.params.noiseMode == NoiseMode::light, "a fresh settings file starts on Light");

            VoiceParams ai;
            ai.noiseMode = NoiseMode::ai;
            expect (paramsFromVar (juce::JSON::parse (JsonFile::toText (paramsToVar (ai)))).noiseMode == NoiseMode::ai);
        }

        beginTest ("paramsFromVar: damaged values fall back to defaults, then clamp");
        {
            const auto p = paramsFromVar (juce::JSON::parse (R"({
                "preGainDb": 99,
                "hpfOn": 1,
                "hpfHz": "loud",
                "noiseMode": 2,
                "noiseAmount": null,
                "gateThresholdDb": -1e400,
                "eqMudDb": 5,
                "compRatio": [3],
                "bypass": true,
                "mute": true
            })"));

            expectEquals (p.preGainDb, ParamRanges::preGainDb.max);
            expect (p.hpfOn, "a number is not a bool: default (on)");
            expectEquals (p.hpfHz, ParamRanges::hpfHz.defaultValue);
            expect (p.noiseMode == NoiseMode::light, "a number is not a mode name: default");
            expectEquals (p.noiseAmount, ParamRanges::noiseAmount.defaultValue);
            expect (p.gateThresholdDb >= ParamRanges::gateThresholdDb.min
                    && p.gateThresholdDb <= ParamRanges::gateThresholdDb.max, "an overflowing number stays in range");
            expectEquals (p.eqMudDb, ParamRanges::eqMudDb.max);
            expectEquals (p.compRatio, ParamRanges::compRatio.defaultValue);
            expect (! p.bypass && ! p.mute, "bypass and mute are never read");
        }

        beginTest ("paramsFromVar: anything that is not an object gives the defaults");
        {
            expect (paramsFromVar ({}) == VoiceParams {});
            expect (paramsFromVar (juce::var (5)) == VoiceParams {});
            expect (paramsFromVar (juce::JSON::parse ("[1, 2, 3]")) == VoiceParams {});
            expect (paramsFromVar (juce::var ("text")) == VoiceParams {});
        }
    }
};

// =============================================================================
class SettingsTests final : public juce::UnitTest
{
public:
    SettingsTests() : juce::UnitTest ("Settings", "session") {}

    void runTest() override
    {
        beginTest ("settings round-trip");
        {
            const auto original = unusualSettings();
            const auto defaults = AppSettings {};

            // The fixture must not be accidentally equal to the defaults anywhere.
            expect (original.deviceStateXml != defaults.deviceStateXml);
            expect (original.inputChannel != defaults.inputChannel);
            expect (original.outputDeviceId != defaults.outputDeviceId);
            expect (original.outputDeviceName != defaults.outputDeviceName);
            expect (original.presetName != defaults.presetName);
            expect (original.startWithWindows != defaults.startWithWindows);
            expect (original.startMinimized != defaults.startMinimized);
            expect (original.muteHotkey != defaults.muteHotkey);
            expect (original.outputPeriodHintFrames != defaults.outputPeriodHintFrames);

            const auto text = JsonFile::toText (SettingsIO::toVar (original));
            const auto back = SettingsIO::fromVar (juce::JSON::parse (text));

            expectEquals (back.deviceStateXml, original.deviceStateXml);
            expect (back.inputChannel == InputChannel::mix);
            expectEquals (back.outputDeviceId, original.outputDeviceId);
            expectEquals (back.outputDeviceName, original.outputDeviceName);
            expectEquals (back.presetName, original.presetName);
            expect (back.startWithWindows);
            expect (back.startMinimized);
            expectEquals (back.muteHotkey, original.muteHotkey);
            expectEquals (back.outputPeriodHintFrames, original.outputPeriodHintFrames);

            expect (back.params.bypass == false && back.params.mute == false,
                    "bypass and mute always come back off");
            expect (back == unusualSettingsAsLoaded(), "a field was lost or changed");

            // Each input channel keeps its identity, not just the one in the fixture.
            for (const auto channel : { InputChannel::first, InputChannel::second, InputChannel::mix })
            {
                AppSettings s;
                s.inputChannel = channel;
                expect (SettingsIO::fromVar (juce::JSON::parse (JsonFile::toText (SettingsIO::toVar (s))))
                            .inputChannel == channel);
            }
        }

        beginTest ("settings tolerate damage");
        {
            expect (SettingsIO::fromVar (juce::JSON::parse ("{}")) == AppSettings {}, "missing keys give defaults");
            expect (SettingsIO::fromVar ({}) == AppSettings {});
            expect (SettingsIO::fromVar (juce::JSON::parse ("[1, 2]")) == AppSettings {});

            const auto clamped = SettingsIO::fromVar (juce::JSON::parse (R"({"params": {"preGainDb": 99}})"));
            expectEquals (clamped.params.preGainDb, 24.0f);

            const auto badMode = SettingsIO::fromVar (juce::JSON::parse (R"({"params": {"noiseMode": "x"}})"));
            expect (badMode.params.noiseMode == NoiseMode::light);

            // One bad key must not cost the good ones next to it.
            const auto partial = SettingsIO::fromVar (juce::JSON::parse (utf8 (R"({
                "presetName": "Phát thanh",
                "inputChannel": "x",
                "startMinimized": "yes",
                "startWithWindows": true,
                "muteHotkey": 7,
                "outputDeviceId": ["a"],
                "params": { "hpfHz": "x", "eqAirDb": 4, "mute": true, "bypass": true }
            })")));

            expectEquals (partial.presetName, utf8 ("Phát thanh"));
            expect (partial.inputChannel == InputChannel::first, "unknown channel: default");
            expect (! partial.startMinimized, "wrong type: default");
            expect (partial.startWithWindows);
            expectEquals (partial.muteHotkey, AppSettings {}.muteHotkey);
            expectEquals (partial.outputDeviceId, juce::String());
            expectEquals (partial.params.hpfHz, ParamRanges::hpfHz.defaultValue);
            expectEquals (partial.params.eqAirDb, 4.0f);
            expect (! partial.params.mute && ! partial.params.bypass, "a hand-edited mute does not stick");

            // An explicitly empty hotkey means "none", not "use the default".
            expectEquals (SettingsIO::fromVar (juce::JSON::parse (R"({"muteHotkey": ""})")).muteHotkey, juce::String());

            // The output period hint is a frame count: no hint is 0. -1 is the
            // "standard shared rung" sentinel and must survive; any other
            // negative or non-numeric value is the same as no hint.
            expectEquals (SettingsIO::fromVar (juce::JSON::parse ("{}")).outputPeriodHintFrames, 0);
            expectEquals (SettingsIO::fromVar (juce::JSON::parse (R"({"outputPeriodHintFrames": -1})"))
                            .outputPeriodHintFrames, -1);
            expectEquals (SettingsIO::fromVar (juce::JSON::parse (R"({"outputPeriodHintFrames": -7})"))
                            .outputPeriodHintFrames, 0);
            expectEquals (SettingsIO::fromVar (juce::JSON::parse (R"({"outputPeriodHintFrames": "144"})"))
                            .outputPeriodHintFrames, 0);
            expectEquals (SettingsIO::fromVar (juce::JSON::parse (R"({"outputPeriodHintFrames": 1e308})"))
                            .outputPeriodHintFrames, 0);

            // The sentinel round-trips through the file format too.
            AppSettings sentinel;
            sentinel.outputPeriodHintFrames = WasapiOutputDevice::standardSharedPeriodHint;
            expectEquals (SettingsIO::fromVar (juce::JSON::parse (JsonFile::toText (SettingsIO::toVar (sentinel))))
                            .outputPeriodHintFrames, -1);

            ScopedTempDir tmp;
            const auto file = tmp.dir.getChildFile ("settings.json");
            const auto bad  = tmp.dir.getChildFile ("settings.json.bad");

            expect (SettingsIO::load (file) == AppSettings {}, "no file: defaults");
            expect (! file.exists() && ! bad.exists(), "loading must not create anything");

            file.replaceWithText ("{not json", false, false);
            expect (SettingsIO::load (file) == AppSettings {}, "unparsable: defaults");
            expect (bad.existsAsFile(), "the damaged file is kept as settings.json.bad");
            expect (! file.exists(), "and moved out of the way");
            expectEquals (bad.loadFileAsString(), juce::String ("{not json"));

            // A second damaged file replaces the first .bad instead of failing on it.
            file.replaceWithText ("[oops", false, false);
            expect (SettingsIO::load (file) == AppSettings {});
            expectEquals (bad.loadFileAsString(), juce::String ("[oops"));
            expect (! file.exists());

            // Valid JSON that is not an object, and an empty file, are damage too.
            for (const char* content : { "[1, 2, 3]", "", "   \n", "42" })
            {
                file.replaceWithText (content, false, false);
                expect (SettingsIO::load (file) == AppSettings {}, juce::String ("content: ") + content);
                expect (! file.exists() && bad.existsAsFile(), juce::String ("content: ") + content);
                bad.deleteFile();
            }
        }

        beginTest ("settings load accepts a byte-order mark and a file edited by hand");
        {
            ScopedTempDir tmp;
            const auto file = tmp.dir.getChildFile ("settings.json");

            const auto text = JsonFile::toText (SettingsIO::toVar (unusualSettings()));
            writeBytes (file, text, true);
            expect (SettingsIO::load (file) == unusualSettingsAsLoaded(), "UTF-8 file with a BOM");
            expect (! tmp.dir.getChildFile ("settings.json.bad").exists());
        }

        beginTest ("settings save is complete");
        {
            ScopedTempDir tmp;
            const auto file = tmp.dir.getChildFile ("OneCast").getChildFile ("settings.json");

            expect (SettingsIO::save (unusualSettings(), file), "creates the folder as needed");
            expect (SettingsIO::load (file) == unusualSettingsAsLoaded());
            expect (! file.getSiblingFile ("settings.json.tmp").exists(), "no .tmp left behind");
            expect (fileNames (file.getParentDirectory()) == juce::StringArray ("settings.json"),
                    fileNames (file.getParentDirectory()).joinIntoString (", "));

            // Saving again replaces the file and still leaves nothing else.
            auto changed = unusualSettings();
            changed.presetName = utf8 ("Phòng ồn");
            changed.startMinimized = false;
            expect (SettingsIO::save (changed, file));

            auto expected = unusualSettingsAsLoaded();
            expected.presetName = utf8 ("Phòng ồn");
            expected.startMinimized = false;
            expect (SettingsIO::load (file) == expected);
            expect (fileNames (file.getParentDirectory()) == juce::StringArray ("settings.json"));

            // The text on disk is UTF-8 without a BOM (editable in Notepad).
            juce::MemoryBlock raw;
            file.loadFileAsData (raw);
            expect (raw.getSize() > 3);
            expect (! (raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF), "no BOM written");
            expect (juce::String::fromUTF8 ((const char*) raw.getData(), (int) raw.getSize()).contains (utf8 ("Phòng ồn")),
                    "Vietnamese is stored as UTF-8, not escaped");
        }

        beginTest ("settings save reports failure instead of throwing");
        {
            ScopedTempDir tmp;
            const auto blocker = tmp.dir.getChildFile ("blocker");
            blocker.replaceWithText ("a file where a folder is needed", false, false);

            expect (! SettingsIO::save (AppSettings {}, blocker.getChildFile ("settings.json")));
            expect (blocker.existsAsFile(), "the obstacle is untouched");
        }

        beginTest ("the default settings file lives in %APPDATA%\\OneCast");
        {
            const auto file = SettingsIO::getDefaultFile();
            expectEquals (file.getFileName(), juce::String ("settings.json"));
            expectEquals (file.getParentDirectory().getFileName(), juce::String ("OneCast"));
            expect (file.getParentDirectory().getParentDirectory()
                        == juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory));
        }
    }
};

// =============================================================================
class PresetTests final : public juce::UnitTest
{
public:
    PresetTests() : juce::UnitTest ("Presets", "session") {}

    void runTest() override
    {
        const auto natural   = utf8 ("Tự nhiên");
        const auto noisy     = utf8 ("Phòng ồn");
        const auto broadcast = utf8 ("Phát thanh");

        beginTest ("built-in presets");
        {
            const auto presets = PresetLibrary::builtIns();
            expectEquals ((int) presets.size(), 3);

            if (presets.size() == 3)
            {
                expectEquals (presets[0].name, natural);
                expectEquals (presets[1].name, noisy);
                expectEquals (presets[2].name, broadcast);

                VoiceParams expectedNatural;

                VoiceParams expectedNoisy;
                expectedNoisy.noiseMode = NoiseMode::ai;
                expectedNoisy.hpfHz = 100.0f;
                expectedNoisy.gateThresholdDb = -42.0f;

                VoiceParams expectedBroadcast;
                expectedBroadcast.hpfHz = 90.0f;
                expectedBroadcast.eqMudDb = -3.0f;
                expectedBroadcast.eqClarityDb = 4.0f;
                expectedBroadcast.eqAirDb = 2.0f;
                expectedBroadcast.compThresholdDb = -24.0f;
                expectedBroadcast.compRatio = 4.0f;
                expectedBroadcast.compMakeupDb = 6.0f;
                expectedBroadcast.deEssAmount = 65.0f;

                expect (presets[0].params == expectedNatural, "natural keeps every default");
                expect (presets[0].params.noiseMode == NoiseMode::light, "natural is Light");
                expect (presets[1].params.noiseMode == NoiseMode::ai, "noisy room uses AI");
                expect (presets[2].params.noiseMode == NoiseMode::light, "broadcast is Light");
                expect (presets[1].params == expectedNoisy, "noisy room");
                expect (presets[2].params == expectedBroadcast, "broadcast");
            }

            for (const auto& preset : presets)
            {
                expect (preset.builtIn, preset.name);
                expect (clampToRanges (preset.params) == preset.params, preset.name + " is outside the ranges");
                expect (! preset.params.bypass && ! preset.params.mute, preset.name);
            }
        }

        beginTest ("user presets");
        {
            ScopedTempDir tmp;
            const auto folder = tmp.dir.getChildFile ("Presets");
            PresetLibrary library (folder);

            expectEquals ((int) library.getAll().size(), 3, "a library whose folder does not exist yet lists the built-ins");
            expect (! library.find (utf8 ("Của tôi")).has_value());

            VoiceParams mine;
            mine.hpfHz = 150.0f;
            mine.noiseMode = NoiseMode::ai;
            mine.eqClarityDb = 6.5f;
            mine.deEssOn = false;

            const auto mineName = utf8 ("Của tôi");
            expect (library.saveUserPreset (mineName, mine));
            expect (folder.isDirectory(), "saving creates the folder");

            const auto all = library.getAll();
            expectEquals ((int) all.size(), 4);

            if (all.size() == 4)
            {
                expectEquals (all[0].name, natural);
                expectEquals (all[3].name, mineName);
                expect (! all[3].builtIn);
                expect (all[3].params == mine);
            }

            const auto found = library.find (mineName);
            expect (found.has_value() && found->params == mine && ! found->builtIn);
            expect (library.find (natural).has_value() && library.find (natural)->builtIn);

            // Built-in names and empty names are refused, and the built-in survives.
            expect (! library.saveUserPreset (natural, mine));
            expect (! library.saveUserPreset (broadcast, mine));
            expect (! library.saveUserPreset ("", mine));
            expect (! library.saveUserPreset ("  \t ", mine));
            expect (library.find (natural)->params == VoiceParams {});
            expectEquals ((int) library.getAll().size(), 4);

            // A name with characters Windows will not allow in a file name.
            VoiceParams odd;
            odd.compRatio = 8.0f;
            expect (library.saveUserPreset ("a/b:c", odd));

            PresetLibrary reopened (folder);
            const auto oddFound = reopened.find ("a/b:c");
            expect (oddFound.has_value(), "found again by its real name");
            expect (oddFound.has_value() && oddFound->name == "a/b:c" && oddFound->params == odd);
            expect (folder.getChildFile ("abc.json").existsAsFile(), "file named from the sanitised name");
            expect (reopened.find (mineName).has_value(), "the first preset is still there for a new library object");

            // Delete removes the file, only that one.
            expect (folder.getChildFile (mineName + ".json").existsAsFile(), "named after the preset");
            expect (library.deleteUserPreset (mineName));
            expect (! folder.getChildFile (mineName + ".json").exists(), "the file is gone");
            expect (! library.find (mineName).has_value());
            expectEquals ((int) fileNames (folder).size(), 1);
            expect (! library.deleteUserPreset (mineName), "already gone");
            expect (! library.deleteUserPreset (natural), "built-ins cannot be deleted");
            expect (! library.deleteUserPreset (""));
            expect (library.find (natural).has_value());
            expect (library.find ("a/b:c").has_value());

            expect (library.deleteUserPreset ("a/b:c"));
            expectEquals ((int) fileNames (folder).size(), 0);
            expectEquals ((int) library.getAll().size(), 3);
        }

        beginTest ("user presets: saving again replaces, names are listed in order");
        {
            ScopedTempDir tmp;
            PresetLibrary library (tmp.dir);

            VoiceParams a, b;
            a.hpfHz = 60.0f;
            b.hpfHz = 70.0f;

            expect (library.saveUserPreset ("Bravo", a));
            expect (library.saveUserPreset ("alpha", a));
            expect (library.saveUserPreset ("Charlie", a));
            expect (library.saveUserPreset (" Bravo ", b), "surrounding spaces are not part of the name");

            const auto all = library.getAll();
            expectEquals ((int) all.size(), 6);

            if (all.size() == 6)
            {
                expectEquals (all[3].name, juce::String ("alpha"));
                expectEquals (all[4].name, juce::String ("Bravo"));
                expectEquals (all[5].name, juce::String ("Charlie"));
                expectEquals (all[4].params.hpfHz, 70.0f);
            }

            expectEquals ((int) fileNames (tmp.dir).size(), 3, "one file per name");
            expect (library.find ("  alpha")->name == "alpha", "lookup ignores surrounding spaces too");
        }

        beginTest ("user presets: names that collide as file names stay separate");
        {
            ScopedTempDir tmp;
            PresetLibrary library (tmp.dir);

            VoiceParams slash, plain, upper;
            slash.hpfHz = 50.0f;
            plain.hpfHz = 60.0f;
            upper.hpfHz = 70.0f;

            // "a/b" and "ab" both sanitise to ab.json; Windows also treats AB.json as the same file.
            expect (library.saveUserPreset ("a/b", slash));
            expect (library.saveUserPreset ("ab", plain));
            expect (library.saveUserPreset ("AB", upper));

            expectEquals (library.find ("a/b")->params.hpfHz, 50.0f);
            expectEquals (library.find ("ab")->params.hpfHz, 60.0f);
            expectEquals (library.find ("AB")->params.hpfHz, 70.0f);
            expectEquals ((int) fileNames (tmp.dir).size(), 3);

            expect (library.deleteUserPreset ("ab"));
            expect (library.find ("a/b").has_value() && library.find ("AB").has_value());
            expect (! library.find ("ab").has_value());
        }

        beginTest ("user presets: names that cannot be file names still save");
        {
            ScopedTempDir tmp;
            PresetLibrary library (tmp.dir);

            VoiceParams p;
            p.compRatio = 5.0f;

            // Nothing is left after sanitising, a device name, a trailing dot, a control character.
            for (const char* awkward : { "???", "CON", "nul.txt", "Lpt1", "dots...", "line\nbreak" })
            {
                expect (library.saveUserPreset (awkward, p), juce::String ("save ") + awkward);
                expect (library.find (awkward).has_value(), juce::String ("find ") + awkward);
            }

            expectEquals ((int) fileNames (tmp.dir).size(), 6);

            for (const auto& file : tmp.dir.findChildFiles (juce::File::findFiles, false))
                expect (file.getFileName().containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._ -()")
                        && file.hasFileExtension ("json"), file.getFileName());
        }

        beginTest ("user presets: bypass and mute are not part of a preset");
        {
            ScopedTempDir tmp;
            PresetLibrary library (tmp.dir);

            VoiceParams p;
            p.bypass = true;
            p.mute = true;
            p.eqAirDb = 5.0f;
            expect (library.saveUserPreset (utf8 ("Tắt tiếng"), p));

            const auto found = library.find (utf8 ("Tắt tiếng"));
            expect (found.has_value() && ! found->params.bypass && ! found->params.mute);
            expectEquals (found->params.eqAirDb, 5.0f);
        }

        beginTest ("user presets: damaged and foreign files are skipped or repaired");
        {
            ScopedTempDir tmp;
            PresetLibrary library (tmp.dir);

            auto* wild = new juce::DynamicObject();
            wild->setProperty ("preGainDb", 99);
            wild->setProperty ("noiseMode", "nonsense");
            wild->setProperty ("compRatio", 4.5);

            writePresetFile (tmp.dir.getChildFile ("wild.json"), utf8 ("Hoang dã"), juce::var (wild));
            writePresetFile (tmp.dir.getChildFile ("clash.json"), utf8 ("Phòng ồn"), juce::var (new juce::DynamicObject()));
            tmp.dir.getChildFile ("broken.json").replaceWithText ("{ oops", false, false);
            tmp.dir.getChildFile ("empty.json").replaceWithText ("", false, false);
            tmp.dir.getChildFile ("noparams.json").replaceWithText (R"({"name": "No params"})", false, false);
            tmp.dir.getChildFile ("arrayparams.json").replaceWithText (R"({"name": "Array", "params": [1, 2]})", false, false);
            tmp.dir.getChildFile ("toparray.json").replaceWithText (R"([{"name": "Top", "params": {}}])", false, false);
            tmp.dir.getChildFile ("notes.txt").replaceWithText ("not a preset", false, false);
            tmp.dir.getChildFile ("anonymous.json").replaceWithText (R"({"params": {"hpfHz": 55}})", false, false);

            const auto all = library.getAll();
            juce::StringArray names;
            for (const auto& preset : all)
                names.add (preset.name);

            expectEquals (names.joinIntoString ("|"),
                          natural + "|" + noisy + "|" + broadcast + "|anonymous|" + utf8 ("Hoang dã"),
                          "built-ins, then the two usable files; a file named like a built-in does not shadow it");

            const auto wildFound = library.find (utf8 ("Hoang dã"));
            expect (wildFound.has_value());
            expectEquals (wildFound->params.preGainDb, 24.0f);
            expect (wildFound->params.noiseMode == NoiseMode::light);
            expectEquals (wildFound->params.compRatio, 4.5f);
            expectEquals (wildFound->params.hpfHz, ParamRanges::hpfHz.defaultValue, "missing keys: defaults");

            expectEquals (library.find (noisy)->params.hpfHz, 100.0f, "the built-in wins over a file with its name");
            expectEquals (library.find ("anonymous")->params.hpfHz, 55.0f, "a file without a name is named after the file");

            // A preset in a hand-edited file is not left in a broken state by deleting a neighbour.
            expect (library.deleteUserPreset ("anonymous"));
            expect (tmp.dir.getChildFile ("broken.json").existsAsFile(), "files that are not presets are never touched");
        }

        beginTest ("applyPreset keeps the mic gain");
        {
            VoiceParams current;
            current.preGainDb = 12.0f;
            current.bypass = true;
            current.mute = true;
            current.outputGainDb = 3.0f;
            current.hpfOn = false;

            VoiceParams presetParams = PresetLibrary::builtIns()[2].params;
            const auto applied = applyPreset (current, presetParams);

            expectEquals (applied.preGainDb, 12.0f);
            expect (applied.bypass);
            expect (applied.mute);
            expectEquals (applied.compRatio, 4.0f);
            expectEquals (applied.hpfHz, 90.0f);
            expectEquals (applied.outputGainDb, 0.0f, "everything else comes from the preset");
            expect (applied.hpfOn, "including the switches");

            // Mic gain and bypass/mute are the only things taken from current.
            auto expected = presetParams;
            expected.preGainDb = 12.0f;
            expected.bypass = true;
            expected.mute = true;
            expect (applied == expected);

            // Applying onto a muted, bypassed state must not clear them either.
            expect (applyPreset (current, VoiceParams {}).mute);

            static_assert (noexcept (applyPreset (std::declval<const VoiceParams&>(),
                                                   std::declval<const VoiceParams&>())));
        }

        beginTest ("the default preset folder lives in %APPDATA%\\OneCast\\Presets");
        {
            const auto folder = PresetLibrary::getDefaultDirectory();
            expectEquals (folder.getFileName(), juce::String ("Presets"));
            expectEquals (folder.getParentDirectory().getFileName(), juce::String ("OneCast"));
            expect (folder.getParentDirectory() == SettingsIO::getDefaultFile().getParentDirectory());
        }
    }
};

static ParamsJsonTests paramsJsonTests;
static SettingsTests   settingsTests;
static PresetTests     presetTests;
