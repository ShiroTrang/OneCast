#pragma once
#include <JuceHeader.h>

#include <functional>

/** The notification-area icon: the app lives here while the window is hidden.

    The glyph is drawn in code rather than loaded from a file, so the executable
    keeps working from any folder and the muted state only needs a colour change.
    Everything here runs on the message thread, like the rest of the UI.

    The callbacks are owned by the application: the tray itself never closes
    the window, toggles the engine or quits, it only asks.
*/
class TrayIcon : public juce::SystemTrayIconComponent
{
public:
    TrayIcon();
    ~TrayIcon() override = default;

    /// Accent while live, grey while muted; the tooltip and menu follow as well.
    void setMuted (bool muted);

    /// Left click, and the menu's "show" item.
    std::function<void()> onShowWindow;

    /// The menu's mute/unmute item.
    std::function<void()> onToggleMute;

    /// The menu's quit item.
    std::function<void()> onQuit;

    void mouseDown (const juce::MouseEvent&) override;

private:
    void updateAppearance();
    void showMenu();

    /// A 64 px image scaled by the shell; 4x the usual 16 px tray slot keeps the
    /// rounded capsule and the cradle smooth on high-DPI taskbars.
    static juce::Image createIcon (juce::Colour glyphColour);

    bool muted = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrayIcon)
};
