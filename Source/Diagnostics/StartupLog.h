#pragma once
#include <JuceHeader.h>

/** Lightweight start-up phase timing.

    Writes one file per launch to %APPDATA%/OneCast/startup.log. DBG() compiles
    out in Release builds and the debugger is not always attached, so this is the
    only practical way to find out which phase of start-up is slow.
*/
namespace StartupLog
{
    struct State
    {
        double first = juce::Time::getMillisecondCounterHiRes();
        double last  = first;
        juce::String text;
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
