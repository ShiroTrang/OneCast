#include "AutoStart.h"

namespace
{
    const juce::String runKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";

    /// A backslash would make JUCE read the name as a subkey path, and an empty
    /// name addresses the key's default value: neither is an autostart entry.
    bool isValidValueName (const juce::String& valueName)
    {
        return valueName.isNotEmpty() && ! valueName.containsChar ('\\');
    }

    juce::String valuePath (const juce::String& valueName)
    {
        return runKey + "\\" + valueName;
    }
}

namespace AutoStart
{
    juce::String commandLine()
    {
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();

        // Quoted because the install folder can hold spaces ("Program Files").
        return "\"" + exe + "\" --minimized";
    }

    bool isEnabled (const juce::String& valueName)
    {
        return isValidValueName (valueName)
            && juce::WindowsRegistry::valueExists (valuePath (valueName));
    }

    bool setEnabled (bool enabled, const juce::String& valueName)
    {
        if (! isValidValueName (valueName))
            return false;

        const auto path = valuePath (valueName);

        if (enabled)
            return juce::WindowsRegistry::setValue (path, commandLine());

        // deleteValue() reports a missing value as a failure; being off already
        // is what the caller asked for.
        return ! juce::WindowsRegistry::valueExists (path)
            || juce::WindowsRegistry::deleteValue (path);
    }
}
