#pragma once
#include <JuceHeader.h>

/** Central design tokens for the LiveVocal UI.

    Every panel pulls its colours, fonts and metrics from here so the whole app
    shares one visual language instead of each component inventing its own
    hex literals.
*/
namespace Theme
{
    // ======================================================================
    //  Palette
    // ======================================================================
    inline const juce::Colour windowBg      { 0xff0f1117 };  // app background
    inline const juce::Colour panelBg       { 0xff161923 };  // panel background
    inline const juce::Colour cardBg        { 0xff1d212e };  // card / row surface
    inline const juce::Colour cardBgAlt     { 0xff232838 };  // hovered / alt surface
    inline const juce::Colour wellBg        { 0xff0b0d13 };  // inset wells (meter, combo)
    inline const juce::Colour border        { 0xff2a3040 };  // hairlines
    inline const juce::Colour borderStrong  { 0xff3a4257 };

    inline const juce::Colour accent        { 0xffe0446a };  // primary / brand
    inline const juce::Colour accentDim     { 0xff8c2a43 };
    inline const juce::Colour accent2       { 0xff7c5cff };  // secondary / violet
    inline const juce::Colour info          { 0xff4a9eff };

    inline const juce::Colour textPrimary   { 0xffe8eaf0 };
    inline const juce::Colour textSecondary { 0xff9aa3b8 };
    inline const juce::Colour textDim       { 0xff656d82 };

    inline const juce::Colour ok            { 0xff2fbf71 };
    inline const juce::Colour warn          { 0xffe8a33d };
    inline const juce::Colour danger        { 0xffe5484d };

    // ======================================================================
    //  Metrics
    // ======================================================================
    inline constexpr int pad        = 10;   // outer panel padding
    inline constexpr int gap        = 8;    // gap between cards
    inline constexpr int gapSmall   = 4;
    inline constexpr int radius     = 8;    // card corner radius
    inline constexpr int rowHeight  = 28;   // standard control height
    inline constexpr int headerH    = 26;   // card title bar height
    inline constexpr int toolbarH   = 52;   // top action bar height
    inline constexpr int labelW     = 118;  // right-aligned field label width

    // ======================================================================
    //  Fonts
    // ======================================================================
    inline juce::Font font (float height, int style = juce::Font::plain)
    {
        return juce::Font (juce::FontOptions (height, style));
    }

    inline juce::Font appTitleFont()  { return font (16.0f, juce::Font::bold); }
    inline juce::Font cardTitleFont() { return font (11.5f, juce::Font::bold); }
    inline juce::Font headingFont()   { return font (13.0f, juce::Font::bold); }
    inline juce::Font bodyFont()      { return font (12.5f); }

    /** Monospaced so numeric readouts stay aligned column to column. */
    inline juce::Font valueFont()
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                              12.5f, juce::Font::bold));
    }

    // ======================================================================
    //  Drawing helpers
    // ======================================================================

    /** Fill a rounded card with a hairline border. */
    inline void fillCard (juce::Graphics& g, juce::Rectangle<int> area,
                          juce::Colour bg = cardBg)
    {
        const auto r = area.toFloat();
        g.setColour (bg);
        g.fillRoundedRectangle (r, (float) radius);
        g.setColour (border);
        g.drawRoundedRectangle (r.reduced (0.5f), (float) radius, 1.0f);
    }

    /** Fill a card and draw an upper-case section title inside it.
        @returns the area left below the title bar. */
    inline juce::Rectangle<int> drawCardWithTitle (juce::Graphics& g,
                                                   juce::Rectangle<int> area,
                                                   const juce::String& title,
                                                   juce::Colour titleColour = textSecondary,
                                                   juce::Colour bg = cardBg)
    {
        fillCard (g, area, bg);

        if (title.isEmpty())
            return area.reduced (pad);

        auto header = area.reduced (pad, 0).removeFromTop (headerH);
        g.setColour (titleColour);
        g.setFont (cardTitleFont());
        g.drawText (title.toUpperCase(), header, juce::Justification::centredLeft, false);

        auto rest = area.reduced (pad);
        rest.removeFromTop (headerH);
        return rest;
    }

    // ======================================================================
    //  Control styling
    // ======================================================================
    inline void styleLabel (juce::Label& l,
                            juce::Colour colour = textPrimary,
                            float height = 12.5f,
                            juce::Justification just = juce::Justification::centredRight)
    {
        l.setColour (juce::Label::textColourId, colour);
        l.setFont (font (height));
        l.setJustificationType (just);
    }

    inline void styleComboBox (juce::ComboBox& c)
    {
        c.setColour (juce::ComboBox::backgroundColourId, wellBg);
        c.setColour (juce::ComboBox::textColourId,       textPrimary);
        c.setColour (juce::ComboBox::outlineColourId,    border);
        c.setColour (juce::ComboBox::arrowColourId,      accent);
        c.setColour (juce::ComboBox::focusedOutlineColourId, accent);
    }

    inline void styleTextButton (juce::TextButton& b,
                                 juce::Colour bg = cardBgAlt,
                                 juce::Colour text = textPrimary)
    {
        b.setColour (juce::TextButton::buttonColourId,   bg);
        b.setColour (juce::TextButton::buttonOnColourId, accent);
        b.setColour (juce::TextButton::textColourOffId,  text);
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    }

    /** Colour for a meter reading, used for numeric readouts. */
    inline juce::Colour colourForDb (float db)
    {
        if (db >= -0.5f)  return danger;
        if (db >= -6.0f)  return warn;
        if (db <= -60.0f) return textDim;
        return ok;
    }

    // ======================================================================
    //  Application-wide LookAndFeel
    // ======================================================================
    /** Themes the pieces that are only reachable through the default
        LookAndFeel: popup menus, scrollbars, tooltips and text editors. */
    class LookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel()
        {
            setColour (juce::ResizableWindow::backgroundColourId, windowBg);

            setColour (juce::PopupMenu::backgroundColourId,            cardBg);
            setColour (juce::PopupMenu::textColourId,                  textPrimary);
            setColour (juce::PopupMenu::headerTextColourId,            textSecondary);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, accentDim);
            setColour (juce::PopupMenu::highlightedTextColourId,       juce::Colours::white);

            setColour (juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);
            setColour (juce::ScrollBar::thumbColourId,      borderStrong);
            setColour (juce::ScrollBar::trackColourId,      wellBg);

            setColour (juce::TooltipWindow::backgroundColourId, cardBgAlt);
            setColour (juce::TooltipWindow::textColourId,       textPrimary);
            setColour (juce::TooltipWindow::outlineColourId,    border);

            setColour (juce::TextEditor::backgroundColourId,     wellBg);
            setColour (juce::TextEditor::textColourId,           textPrimary);
            setColour (juce::TextEditor::highlightColourId,      accentDim);
            setColour (juce::TextEditor::highlightedTextColourId, juce::Colours::white);
            setColour (juce::TextEditor::outlineColourId,        accent);
            setColour (juce::TextEditor::focusedOutlineColourId, accent);
            setColour (juce::CaretComponent::caretColourId,      accent);

            setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            setColour (juce::Label::textColourId,       textPrimary);

            setColour (juce::ToggleButton::textColourId, textSecondary);
            setColour (juce::ToggleButton::tickColourId, accent);

            setColour (juce::TextButton::buttonColourId, cardBgAlt);
            setColour (juce::TextButton::textColourOffId, textPrimary);
        }

        juce::Font getLabelFont (juce::Label&) override   { return bodyFont(); }
        juce::Font getTextButtonFont (juce::TextButton&, int) override { return font (12.5f); }
        juce::Font getComboBoxFont (juce::ComboBox&) override { return font (12.5f); }
        juce::Font getPopupMenuFont() override { return font (13.0f); }
    };
}
