#include "TrayIcon.h"
#include "Strings.h"
#include "Theme.h"

//==============================================================================
TrayIcon::TrayIcon()
{
    updateAppearance();
}

void TrayIcon::setMuted (bool shouldBeMuted)
{
    if (muted == shouldBeMuted)
        return;

    muted = shouldBeMuted;
    updateAppearance();
}

void TrayIcon::updateAppearance()
{
    const auto glyph = createIcon (muted ? Theme::textDim : Theme::accent);

    // Windows and Linux use the colour image; macOS would use the template one,
    // and a grey glyph is the right shape for both roles.
    setIconImage (glyph, glyph);
    setIconTooltip (muted ? Strings::trayTooltipMuted() : Strings::appName());
}

juce::Image TrayIcon::createIcon (juce::Colour glyphColour)
{
    constexpr int size = 64;
    juce::Image image (juce::Image::ARGB, size, size, true);
    juce::Graphics g (image);

    g.setColour (glyphColour);

    // The microphone: a rounded capsule, the open cradle below it, a short stem
    // and a base. Drawn as shapes, so it stays sharp at any tray scale.
    g.fillRoundedRectangle (25.0f, 10.0f, 14.0f, 25.0f, 7.0f);

    juce::Path cradle;
    cradle.addCentredArc (32.0f, 28.0f, 15.0f, 15.0f, 0.0f,
                          0.0f, juce::MathConstants<float>::pi, true);
    g.strokePath (cradle, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));

    g.fillRoundedRectangle (30.0f, 43.0f, 4.0f, 8.0f, 2.0f);
    g.fillRoundedRectangle (22.0f, 50.0f, 20.0f, 4.0f, 2.0f);

    return image;
}

//==============================================================================
void TrayIcon::mouseDown (const juce::MouseEvent& event)
{
    // Windows reports a tray right-click as the popup modifier; the shell shows
    // its menu on the button-down, so do the same instead of waiting for a
    // release the cursor may never deliver inside the icon.
    if (event.mods.isPopupMenu())
    {
        showMenu();
        return;
    }

    if (event.mods.isLeftButtonDown() && onShowWindow != nullptr)
        onShowWindow();
}

void TrayIcon::showMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, Strings::trayShow());
    menu.addItem (2, muted ? Strings::trayUnmute() : Strings::trayMute());
    menu.addSeparator();
    menu.addItem (3, Strings::trayQuit());

    // The menu is asynchronous and outlives this call; the safe pointer covers
    // the app quitting while it is still open.
    juce::Component::SafePointer<TrayIcon> safeThis (this);
    const auto position = juce::Desktop::getMousePosition();
    const auto target = juce::Rectangle<int> (position.x, position.y, 1, 1);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (target),
                        [safeThis] (int result)
                        {
                            if (safeThis == nullptr || result == 0)
                                return;

                            if (result == 1 && safeThis->onShowWindow != nullptr)
                                safeThis->onShowWindow();
                            else if (result == 2 && safeThis->onToggleMute != nullptr)
                                safeThis->onToggleMute();
                            else if (result == 3 && safeThis->onQuit != nullptr)
                                safeThis->onQuit();
                        });
}
