#pragma once
#include <JuceHeader.h>
#include "Strings.h"
#include "Theme.h"

/** A slide-in alert banner that appears just below the header bar.

    - Red for Critical alerts (capture/device lost).
    - Amber for Warnings.
    - Blue for Information (plugin scan progress, "preset saved").

    The banner owns its own geometry: the owner only supplies the y coordinate it
    should slide down to via setSlideTargetY(), so the animation and the layout
    cannot disagree about the height or the parent width.
*/
class AlertBanner : public juce::Component,
                    private juce::Timer
{
public:
    enum class Severity { Info, Warning, Critical };

    static constexpr int bannerHeight = 40;

    //==========================================================================
    AlertBanner()
    {
        dismissButton.setButtonText (Strings::dismiss());
        Theme::styleTextButton (dismissButton, juce::Colours::transparentBlack,
                                juce::Colours::white);
        dismissButton.onClick = [this]() { dismiss(); };
        addAndMakeVisible (dismissButton);

        setVisible (false);
    }

    ~AlertBanner() override
    {
        stopTimer();
    }

    //==========================================================================
    /** The y coordinate the banner slides down to (e.g. the bottom of the header). */
    void setSlideTargetY (int y)
    {
        targetY = y;

        if (! isVisible())
            return;

        applyBounds (animatingOut ? hiddenY() : targetY);
        repaint();
    }

    /** Show the banner with the given message and severity.

        If the banner is already on screen (and not on its way out) the message
        is swapped in place. Re-running the slide animation for every alert made
        the banner flicker away and back whenever two alerts arrived together.
    */
    void show (const juce::String& message, Severity sev)
    {
        const bool alreadyOnScreen = isVisible() && ! animatingOut;

        currentMessage  = message;
        currentSeverity = sev;

        if (alreadyOnScreen)
        {
            dismissButton.setColour (juce::TextButton::textColourOffId, textColourFor (sev));
            repaint();
            return;
        }

        animProgress = 0.0f;
        animatingIn  = true;
        animatingOut = false;

        dismissButton.setColour (juce::TextButton::textColourOffId, textColourFor (sev));

        // Position off-screen before becoming visible so it cannot flash.
        applyBounds (hiddenY());
        setVisible (true);
        toFront (false);
        startTimerHz (60);
    }

    /** Start the dismiss animation. */
    void dismiss()
    {
        animatingOut = true;
        animatingIn  = false;
        startTimerHz (60);
    }

    /** True if the banner is currently visible (including during animations). */
    bool isShowing() const { return isVisible(); }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto bg = backgroundFor (currentSeverity);
        const auto fg = textColourFor (currentSeverity);

        const auto bounds = getLocalBounds().toFloat();

        g.setColour (bg);
        g.fillRoundedRectangle (bounds, (float) Theme::radius);

        // Severity stripe on the left, so the state reads at a glance.
        auto stripe = bounds.reduced (3.0f, 7.0f).removeFromLeft (4.0f);
        g.setColour (fg.withAlpha (0.6f));
        g.fillRoundedRectangle (stripe, 2.0f);

        g.setColour (fg);
        g.setFont (Theme::font (13.0f, juce::Font::bold));

        auto textArea = getLocalBounds().reduced (16, 0);
        textArea.removeFromRight (84);   // room for the dismiss button
        g.drawFittedText (currentMessage, textArea, juce::Justification::centredLeft, 2);
    }

    void resized() override
    {
        dismissButton.setBounds (getLocalBounds().removeFromRight (80).reduced (4, 8));
    }

private:
    //==========================================================================
    void timerCallback() override
    {
        constexpr float speed = 0.12f;

        if (animatingIn)
        {
            animProgress += speed;

            if (animProgress >= 1.0f)
            {
                animProgress = 1.0f;
                animatingIn  = false;
                stopTimer();
            }
        }
        else if (animatingOut)
        {
            animProgress -= speed;

            if (animProgress <= 0.0f)
            {
                animProgress = 0.0f;
                animatingOut = false;
                applyBounds (hiddenY());   // canonical hidden position
                setVisible (false);
                stopTimer();
                return;
            }
        }

        applyBounds (juce::roundToInt (hiddenY() + (targetY - hiddenY()) * animProgress));
        repaint();
    }

    int hiddenY() const noexcept { return targetY - bannerHeight; }

    void applyBounds (int y)
    {
        const int w = getParentComponent() != nullptr ? getParentComponent()->getWidth()
                                                      : getWidth();
        setBounds (0, y, w, bannerHeight);
    }

    static juce::Colour backgroundFor (Severity s)
    {
        switch (s)
        {
            case Severity::Critical: return Theme::danger;
            case Severity::Warning:  return Theme::warn;
            case Severity::Info:
            default:                 return Theme::info;
        }
    }

    static juce::Colour textColourFor (Severity s)
    {
        // Amber needs dark text; red and blue read best with white.
        return s == Severity::Warning ? juce::Colour (0xff2a1a00) : juce::Colours::white;
    }

    //==========================================================================
    juce::String currentMessage;
    Severity     currentSeverity = Severity::Info;
    juce::TextButton dismissButton;

    int   targetY      = 0;
    float animProgress = 0.0f;
    bool  animatingIn  = false;
    bool  animatingOut = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlertBanner)
};
