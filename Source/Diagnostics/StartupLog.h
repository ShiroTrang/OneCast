#pragma once
#include <JuceHeader.h>

/** Lightweight start-up phase timing.

    Writes one file per launch to %APPDATA%/OneCast/startup.log. DBG() compiles
    out in Release builds and the debugger is not always attached, so this is the
    only practical way to find out which phase of start-up is slow.

    mark() and flush() are message-thread only. The one look-alike is the
    "output: mode=..." line WasapiOutputDevice::start() writes; start() runs on
    the message thread and is still inside it when that line is written, just
    blocked waiting for the render loop to begin (the render thread never calls
    mark()). Keeping every call on one thread means the String append needs no
    lock, which matters for a tray-resident app that logs for days.
*/
namespace StartupLog
{
    /// The first lines explain a slow launch; the last ones explain the
    /// present. The middle is expendable, so the text cannot grow without
    /// bound in a process that stays up for days.
    constexpr int headLines = 200;
    constexpr int tailLines = 400;
    constexpr int maxRetainedLines = headLines + tailLines;

    struct State
    {
        double first = juce::Time::getMillisecondCounterHiRes();
        double last  = first;
        juce::String text;
        int lineCount = 0;
    };

    inline State& state()
    {
        static State s;
        return s;
    }

    /** Record that a start-up phase just finished. */
    inline void mark (const juce::String& what)
    {
        auto& s = state();
        const double now = juce::Time::getMillisecondCounterHiRes();

        s.text << juce::String (now - s.last, 1).paddedLeft (' ', 9) << " ms  (t+"
               << juce::String (now - s.first, 1).paddedLeft (' ', 9) << " ms)  "
               << what << juce::newLine;

        s.last = now;

        if (++s.lineCount > maxRetainedLines)
        {
            // The gap is visible from the (t+...) values: the first lines keep
            // the launch sequence, the last lines the recent history.
            auto lines = juce::StringArray::fromLines (s.text);

            if (lines.size() > maxRetainedLines)
            {
                juce::String trimmed;
                trimmed.preallocateBytes ((size_t) maxRetainedLines * 96);

                for (int i = 0; i < headLines; ++i)
                    trimmed << lines[i] << juce::newLine;

                for (int i = lines.size() - tailLines; i < lines.size(); ++i)
                    trimmed << lines[i] << juce::newLine;

                s.text = std::move (trimmed);
                s.lineCount = maxRetainedLines;
            }
            else
            {
                // A message with an embedded newline can make the real line
                // count differ from the number of marks; realign instead of
                // rebuilding every later mark.
                s.lineCount = lines.size();
            }
        }
    }

    /** (Re)write the accumulated log. Safe to call more than once; the last call
        writes everything recorded so far. */
    inline void flush()
    {
        auto& s = state();

        auto file = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                        .getChildFile ("OneCast")
                        .getChildFile ("startup.log");

        file.getParentDirectory().createDirectory();
        file.replaceWithText (s.text, false, false, "\r\n");
    }
}
