#include "OptionsPanel.h"
#include "../Diagnostics/StartupLog.h"
#include "../MainComponent.h"
#include "../Platform/AutoStart.h"
#include "Strings.h"
#include "Theme.h"

//==============================================================================
OptionsPanel::OptionsPanel (MainComponent& owner)
    : main (owner)
{
    auto& settings = main.getSettings();

    startWithWindowsButton.setButtonText (Strings::startWithWindows());
    startWithWindowsButton.setToggleState (settings.startWithWindows,
                                           juce::dontSendNotification);
    startWithWindowsButton.onClick = [this]() { startWithWindowsChanged(); };
    addAndMakeVisible (startWithWindowsButton);

    startMinimizedButton.setButtonText (Strings::startMinimized());
    startMinimizedButton.setToggleState (settings.startMinimized,
                                         juce::dontSendNotification);
    startMinimizedButton.onClick = [this]() { startMinimizedChanged(); };
    addAndMakeVisible (startMinimizedButton);

    hotkeyLabel.setText (Strings::muteHotkey(), juce::dontSendNotification);
    Theme::styleLabel (hotkeyLabel, Theme::textSecondary, 12.0f,
                       juce::Justification::centredLeft);
    addAndMakeVisible (hotkeyLabel);

    hotkeyEditor.setMultiLine (false);
    hotkeyEditor.setReturnKeyStartsNewLine (false);
    hotkeyEditor.setSelectAllWhenFocused (true);
    hotkeyEditor.setFont (Theme::bodyFont());
    hotkeyEditor.setText (settings.muteHotkey, false);
    hotkeyEditor.onReturnKey = [this]() { applyHotkey(); };
    addAndMakeVisible (hotkeyEditor);

    // Always laid out, shown only after a refused combination: switching its
    // visibility keeps the callout the same size instead of jumping.
    hotkeyError.setText (Strings::hotkeyInvalid(), juce::dontSendNotification);
    Theme::styleLabel (hotkeyError, Theme::danger, 11.5f,
                       juce::Justification::topLeft);
    hotkeyError.setVisible (false);
    addAndMakeVisible (hotkeyError);

    setSize (preferredWidth, preferredHeight);
}

//==============================================================================
void OptionsPanel::startWithWindowsChanged()
{
    if (reflecting)
        return;

    const bool wanted = startWithWindowsButton.getToggleState();
    auto& settings = main.getSettings();

    if (wanted == settings.startWithWindows)
        return;

    if (AutoStart::setEnabled (wanted))
    {
        settings.startWithWindows = wanted;
        main.settingsChanged();
        return;
    }

    // The registry refused (a locked-down policy, say). Put the toggle back on
    // the state Windows actually has, so it never promises an autostart that
    // will not happen.
    reflecting = true;
    startWithWindowsButton.setToggleState (settings.startWithWindows,
                                           juce::dontSendNotification);
    reflecting = false;
    StartupLog::mark ("options: could not write the start-with-Windows entry");
    StartupLog::flush();
}

void OptionsPanel::startMinimizedChanged()
{
    if (reflecting)
        return;

    auto& settings = main.getSettings();
    const bool wanted = startMinimizedButton.getToggleState();

    if (wanted != settings.startMinimized)
    {
        // Only the next launch acts on this; the current window is already open.
        settings.startMinimized = wanted;
        main.settingsChanged();
    }
}

void OptionsPanel::applyHotkey()
{
    if (reflecting)
        return;

    const auto text = hotkeyEditor.getText().trim();

    if (main.applyMuteHotkey (text))
    {
        // Show the canonical spelling ("ctrl+alt+m" becomes "Ctrl+Alt+M") now
        // that it is the shortcut really registered with Windows.
        reflecting = true;
        hotkeyEditor.setText (main.getSettings().muteHotkey, false);
        reflecting = false;
        showHotkeyError (false);
        return;
    }

    // Both a typo and a combination owned by another application land here;
    // the copy covers both, and MainComponent has left the old shortcut alone.
    showHotkeyError (true);
}

void OptionsPanel::showHotkeyError (bool shouldShow)
{
    hotkeyError.setVisible (shouldShow);
    repaint();
}

//==============================================================================
void OptionsPanel::resized()
{
    auto content = getLocalBounds().reduced (Theme::pad + 2, Theme::pad);

    startWithWindowsButton.setBounds (content.removeFromTop (26));
    content.removeFromTop (Theme::gapSmall);
    startMinimizedButton.setBounds (content.removeFromTop (26));
    content.removeFromTop (Theme::gap);

    hotkeyLabel.setBounds (content.removeFromTop (18));
    hotkeyEditor.setBounds (content.removeFromTop (28));
    content.removeFromTop (Theme::gapSmall);

    hotkeyError.setBounds (content.removeFromTop (juce::jmin (36, content.getHeight())));
}
