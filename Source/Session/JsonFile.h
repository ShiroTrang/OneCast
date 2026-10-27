#pragma once
#include <JuceHeader.h>

/// Reading and writing the small JSON files OneCast keeps in %APPDATA%\OneCast
/// (settings and presets). Internal to Session/: Settings.cpp and Presets.cpp
/// share it so both get the same crash-safe write and the same idea of "damaged".
namespace JsonFile
{
    enum class Status
    {
        missing,        ///< no such file
        unreadable,     ///< exists, but could not be opened (locked by another program, no permission)
        damaged,        ///< read fine, but is not a JSON object
        ok
    };

    struct ReadResult
    {
        Status status = Status::missing;
        juce::var value;        ///< the top-level object, only when status is ok
    };

    /// Unreadable is kept apart from damaged on purpose: a file that is merely
    /// locked for a moment must never be treated as corrupt and moved aside.
    inline ReadResult read (const juce::File& file)
    {
        if (! file.existsAsFile())
            return {};

        juce::FileInputStream stream (file);

        if (stream.failedToOpen())
            return { Status::unreadable, {} };

        // Strips a UTF-8 byte-order mark and understands UTF-16, which is what
        // Notepad produces when a user edits the file by hand.
        const auto text = stream.readEntireStreamAsString();

        juce::var parsed;

        // An empty file parses to a void var without an error, so the type is
        // checked as well. Top-level arrays and numbers are not our format; note
        // that var::isObject() is also true for an array, hence the DynamicObject test.
        if (juce::JSON::parse (text, parsed).failed() || parsed.getDynamicObject() == nullptr)
            return { Status::damaged, {} };

        return { Status::ok, parsed };
    }

    /// The text of a file. Zero maximum decimal places means "as many as the
    /// number needs", which prints the double 13.37 as 13.37; the default of 15
    /// would print 13.369999999999999.
    inline juce::String toText (const juce::var& value)
    {
        return juce::JSON::toString (value, juce::JSON::FormatOptions {}
                                                .withSpacing (juce::JSON::Spacing::multiLine)
                                                .withMaxDecimalPlaces (0));
    }

    /// Writes <file>.tmp, then swaps it in with ReplaceFile, so a crash or power
    /// cut leaves either the old file or the new one, never half of each.
    /// Creates the folder if needed. False when anything fails; no .tmp is left behind.
    inline bool writeAtomically (const juce::var& value, const juce::File& file)
    {
        if (! file.getParentDirectory().createDirectory().wasOk())
            return false;

        const auto temporary = file.getSiblingFile (file.getFileName() + ".tmp");
        const auto text = toText (value);

        // UTF-8 without a byte-order mark: Vietnamese stays readable in any editor.
        if (temporary.replaceWithText (text, false, false) && temporary.replaceFileIn (file))
            return true;

        temporary.deleteFile();
        return false;
    }
}
