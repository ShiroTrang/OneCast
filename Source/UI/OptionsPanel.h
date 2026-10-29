#pragma once
#include <JuceHeader.h>

class MainComponent;

/** The small callout behind the top bar's gear button.

    It edits three settings that have nothing to do with the sound itself:
    whether Windows starts OneCast at sign-in, whether that start is quiet, and
    which shortcut toggles mute. The first one is written straight to the Run
    key, so what the toggle shows is what Windows will really do; the other two
    only change the settings file and take effect next launch (the hotkey
    changes at once, because it can be registered right here).
*/
class OptionsPanel : public juce::Component
{
public:
    static constexpr int preferredWidth  = 360;
    static constexpr int preferredHeight = 196;

    explicit OptionsPanel (MainComponent&);

    void resized() override;

private:
    void startWithWindowsChanged();
    void startMinimizedChanged();
    void applyHotkey();

    void showHotkeyError (bool shouldShow);

    MainComponent& main;

    juce::ToggleButton startWithWindowsButton, startMinimizedButton;
    juce::Label hotkeyLabel;
    juce::TextEditor hotkeyEditor;
    juce::Label hotkeyError;

    /// Set while the code reflects a state into the controls, so a toggle's
    /// onClick cannot mistake that for a user edit.
    bool reflecting = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OptionsPanel)
};
