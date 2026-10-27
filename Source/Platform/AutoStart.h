#pragma once
#include <JuceHeader.h>

/// "Start with Windows" through the per-user Run key
/// (HKCU\Software\Microsoft\Windows\CurrentVersion\Run): no admin rights, and
/// the user can still turn the entry off in Task Manager > Startup apps.
///
/// valueName is the entry's name under the Run key. Only tests pass anything
/// but the default, so they can exercise the code without touching the real
/// entry.
namespace AutoStart
{
    /// What Windows runs at sign-in: the quoted path of this executable and
    /// --minimized, so the app starts in the tray instead of opening a window.
    juce::String commandLine();

    /// True when the entry exists. The command it holds is not compared with
    /// commandLine(): an install that was moved still reads as on, and
    /// setEnabled (true) points it at the new location.
    bool isEnabled (const juce::String& valueName = "OneCast");

    /// Creates or rewrites the entry, or removes it. Removing an entry that is
    /// not there succeeds. False when the registry refuses, or valueName is
    /// empty or contains a backslash (it would name a subkey, not a value).
    bool setEnabled (bool enabled, const juce::String& valueName = "OneCast");
}
