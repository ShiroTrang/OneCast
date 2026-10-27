#include "ModuleCard.h"
#include "Strings.h"
#include "Theme.h"

#include <cmath>
#include <utility>

namespace
{
    /// Radio groups are process-wide; a high base keeps these cards away from
    /// any group id the rest of the UI might pick.
    int nextModeGroupId()
    {
        static int next = 0x4000;
        return ++next;
    }
}

//==============================================================================
//  The rotary style shared by every knob in a card. It follows the LiveVocal
//  mixer's look: a recessed arc, a coloured value arc and a pointer.
//==============================================================================
class ModuleCard::KnobLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    KnobLookAndFeel()
    {
        setColour (juce::Slider::textBoxTextColourId,        Theme::textPrimary);
        setColour (juce::Slider::textBoxBackgroundColourId,  Theme::wellBg);
        setColour (juce::Slider::textBoxOutlineColourId,     Theme::border);
        setColour (juce::Slider::textBoxHighlightColourId,   Theme::accentDim);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider& slider) override
    {
        const auto area = juce::Rectangle<float> ((float) x, (float) y,
                                                   (float) width, (float) height);
        const float radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.40f;
        const float centreX = area.getCentreX();
        const float centreY = area.getCentreY();
        const float angle = rotaryStartAngle
                          + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const float arcWidth = 3.5f;

        juce::Path bgArc;
        bgArc.addCentredArc (centreX, centreY, radius, radius, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (Theme::wellBg);
        g.strokePath (bgArc, juce::PathStrokeType (arcWidth,
                      juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path valueArc;
        valueArc.addCentredArc (centreX, centreY, radius, radius, 0.0f,
                                rotaryStartAngle, angle, true);
        g.setColour (slider.findColour (juce::Slider::thumbColourId));
        g.strokePath (valueArc, juce::PathStrokeType (arcWidth,
                      juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float innerRadius = radius * 0.60f;
        g.setColour (Theme::cardBgAlt);
        g.fillEllipse (centreX - innerRadius, centreY - innerRadius,
                       innerRadius * 2.0f, innerRadius * 2.0f);
        g.setColour (Theme::border);
        g.drawEllipse (centreX - innerRadius, centreY - innerRadius,
                       innerRadius * 2.0f, innerRadius * 2.0f, 1.0f);

        const float pointerLength = innerRadius * 0.85f;
        juce::Path pointer;
        pointer.addRoundedRectangle (-1.2f, -pointerLength, 2.4f, pointerLength, 1.0f);
        pointer.applyTransform (juce::AffineTransform::rotation (angle)
                                    .translated (centreX, centreY));
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.fillPath (pointer);
    }
};

//==============================================================================
//  A compact power switch, so a card's title row stays one line high.
//==============================================================================
class ModuleCard::PowerButton final : public juce::Button
{
public:
    explicit PowerButton (juce::Colour litColour)
        : juce::Button ("power"), lit (litColour)
    {
        setClickingTogglesState (true);
        setToggleState (true, juce::dontSendNotification);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const bool on = getToggleState();
        auto area = getLocalBounds().toFloat().reduced (1.0f);
        const float size = juce::jmin (area.getWidth(), area.getHeight());
        area = area.withSizeKeepingCentre (size, size);

        auto colour = on ? lit : Theme::textDim;

        if (highlighted || down)
            colour = colour.brighter (0.3f);

        if (on)
        {
            g.setColour (lit.withAlpha (0.18f));
            g.fillEllipse (area);
        }

        // IEC power symbol: an open ring with a bar through the gap.
        const auto centre = area.getCentre();
        const float ringRadius = size * 0.28f;

        juce::Path symbol;
        symbol.addCentredArc (centre.x, centre.y, ringRadius, ringRadius, 0.0f,
                              juce::degreesToRadians (40.0f),
                              juce::degreesToRadians (320.0f), true);
        symbol.startNewSubPath (centre.x, centre.y - ringRadius * 1.3f);
        symbol.lineTo (centre.x, centre.y - ringRadius * 0.15f);

        g.setColour (colour);
        g.strokePath (symbol, juce::PathStrokeType (1.6f,
                      juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    juce::Colour lit;
};
//==============================================================================
ModuleCard::ModuleCard (juce::String titleText, bool hasToggle,
                        std::vector<Knob> knobSpecs, juce::StringArray modes,
                        bool hasReductionMeter)
    : title (std::move (titleText)), hasReduction (hasReductionMeter)
{
    lookAndFeel = std::make_unique<KnobLookAndFeel>();

    if (hasToggle)
    {
        toggle = std::make_unique<PowerButton> (Theme::accent);
        toggle->onClick = [this]()
        {
            toggleOn = toggle->getToggleState();
            applyDimming();

            if (onToggle != nullptr)
                onToggle (toggleOn);
        };
        addAndMakeVisible (*toggle);
    }

    for (int i = 0; i < (int) knobSpecs.size(); ++i)
    {
        const auto& spec = knobSpecs[(size_t) i];

        // The label sits above the knob; the value is the slider's own readout,
        // so a preset or a typed value can never disagree with the knob.
        auto label = std::make_unique<juce::Label>();
        Theme::styleLabel (*label, Theme::textSecondary, 11.0f,
                           juce::Justification::centred);
        label->setText (spec.label, juce::dontSendNotification);
        addAndMakeVisible (*label);
        knobLabels.push_back (std::move (label));

        auto slider = std::make_unique<juce::Slider> (juce::Slider::RotaryVerticalDrag,
                                                      juce::Slider::TextBoxBelow);
        slider->setRange (spec.range.min, spec.range.max);
        slider->setValue (spec.range.defaultValue, juce::dontSendNotification);
        slider->setDoubleClickReturnValue (true, spec.range.defaultValue);
        slider->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 0, 18);
        slider->setTextBoxIsEditable (false);
        slider->setNumDecimalPlacesToDisplay (decimalPlacesFor (spec.unit));
        slider->setTextValueSuffix (suffixFor (spec.unit));
        slider->setColour (juce::Slider::thumbColourId, Theme::accent);
        slider->setColour (juce::Slider::rotarySliderFillColourId, Theme::accent);
        slider->setColour (juce::Slider::rotarySliderOutlineColourId, Theme::wellBg);
        slider->setLookAndFeel (lookAndFeel.get());
        slider->setMouseCursor (juce::MouseCursor::PointingHandCursor);
        slider->onValueChange = [this, i]()
        {
            if (onKnob != nullptr)
                onKnob (i, (float) knobs[(size_t) i]->getValue());
        };
        addAndMakeVisible (*slider);
        knobs.push_back (std::move (slider));
    }

    if (! modes.isEmpty())
    {
        const int groupId = nextModeGroupId();

        for (int i = 0; i < modes.size(); ++i)
        {
            auto button = std::make_unique<juce::TextButton> (modes[i]);
            Theme::styleTextButton (*button, Theme::cardBgAlt, Theme::textSecondary);
            button->setClickingTogglesState (true);
            button->setRadioGroupId (groupId, juce::dontSendNotification);
            button->onClick = [this, i]()
            {
                // The radio group has already moved the visual state; mirror it
                // in the other buttons without running the callback twice.
                setMode (i, juce::dontSendNotification);

                if (onMode != nullptr)
                    onMode (i);
            };
            addAndMakeVisible (*button);
            modeButtons.push_back (std::move (button));
        }

        modeButtons[0]->setToggleState (true, juce::dontSendNotification);
    }

    setSize (300, 240);
    applyDimming();
}

ModuleCard::~ModuleCard()
{
    // Detach before the look is destroyed, so no slider is left pointing at it.
    for (auto& slider : knobs)
        slider->setLookAndFeel (nullptr);
}

//==============================================================================
void ModuleCard::setToggle (bool on, juce::NotificationType notification)
{
    toggleOn = on;

    if (toggle != nullptr)
        toggle->setToggleState (on, juce::dontSendNotification);

    applyDimming();

    if (notification != juce::dontSendNotification && onToggle != nullptr)
        onToggle (on);
}

void ModuleCard::setKnob (int index, float value, juce::NotificationType notification)
{
    if (! juce::isPositiveAndBelow (index, (int) knobs.size()))
        return;

    knobs[(size_t) index]->setValue (value,
        notification == juce::dontSendNotification ? juce::dontSendNotification
                                                   : juce::sendNotificationSync);
}

void ModuleCard::setMode (int index, juce::NotificationType notification)
{
    if (! juce::isPositiveAndBelow (index, (int) modeButtons.size()))
        return;

    for (int i = 0; i < (int) modeButtons.size(); ++i)
        modeButtons[(size_t) i]->setToggleState (i == index,
                                                juce::dontSendNotification);

    if (notification != juce::dontSendNotification && onMode != nullptr)
        onMode (index);
}

void ModuleCard::setReductionDb (float db)
{
    const float clamped = juce::jlimit (-meterRangeDb, 0.0f,
                                        std::isfinite (db) ? db : 0.0f);

    // The meters are fed at 30 Hz; repaint only on a visible change so a quiet
    // stage does not redraw its card for every frame.
    if (std::abs (clamped - reductionDb) < 0.05f)
        return;

    reductionDb = clamped;

    if (hasReduction)
        repaint (meterArea.isEmpty() ? getLocalBounds() : meterArea);
}

//==============================================================================
juce::String ModuleCard::suffixFor (const juce::String& unit)
{
    if (unit.isEmpty())
        return {};

    // A percentage and a ratio read better without a space ("100%" / "3.0:1").
    if (unit == "%" || unit.startsWith (":"))
        return unit;

    return " " + unit;
}

int ModuleCard::decimalPlacesFor (const juce::String& unit)
{
    // Hz and percent are whole numbers; dB and the compressor ratio are not.
    return (unit == "Hz" || unit == "%") ? 0 : 1;
}

void ModuleCard::applyDimming()
{
    // A module that is off stays adjustable, so its setting can be prepared
    // before switching it back on. Only its visual weight changes.
    const float alpha = (toggle == nullptr || toggleOn) ? 1.0f : 0.4f;

    for (auto& knob : knobs)
        knob->setAlpha (alpha);

    for (auto& label : knobLabels)
        label->setAlpha (alpha);

    for (auto& button : modeButtons)
        button->setAlpha (alpha);
}

//==============================================================================
void ModuleCard::paint (juce::Graphics& g)
{
    Theme::fillCard (g, getLocalBounds(), Theme::cardBg);

    g.setColour ((toggle == nullptr || toggleOn) ? Theme::textSecondary : Theme::textDim);
    g.setFont (Theme::cardTitleFont());
    g.drawText (title, titleArea, juce::Justification::centredLeft, false);

    if (! hasReduction || meterArea.isEmpty())
        return;

    auto area = meterArea;
    auto labelArea = area.removeFromLeft (34);
    auto valueArea = area.removeFromRight (50);

    g.setColour (Theme::textDim);
    g.setFont (Theme::font (10.0f));
    g.drawText (Strings::reduction(), labelArea, juce::Justification::centredLeft, false);

    auto bar = area.reduced (0, 4).toFloat();
    g.setColour (Theme::wellBg);
    g.fillRoundedRectangle (bar, 2.0f);

    const bool active = toggle == nullptr || toggleOn;
    const float norm = juce::jlimit (0.0f, 1.0f, -reductionDb / meterRangeDb);

    if (norm > 0.0f)
    {
        auto fill = bar.withWidth (bar.getWidth() * norm);
        g.setColour (active ? Theme::accent2 : Theme::accent2.withAlpha (0.35f));
        g.fillRoundedRectangle (fill, 2.0f);
    }

    g.setColour (Theme::border);
    g.drawRoundedRectangle (bar.reduced (0.5f), 2.0f, 1.0f);

    g.setColour (active ? Theme::textSecondary : Theme::textDim);
    g.setFont (Theme::font (10.0f));
    g.drawText (juce::String (reductionDb, 1) + " dB", valueArea,
                juce::Justification::centredRight, false);
}

//==============================================================================
void ModuleCard::resized()
{
    auto inner = getLocalBounds().reduced (Theme::pad);

    if (inner.getWidth() <= 0 || inner.getHeight() <= 0)
        return;

    titleArea = inner.removeFromTop (Theme::headerH);

    if (toggle != nullptr)
        toggle->setBounds (titleArea.removeFromRight (20).withSizeKeepingCentre (18, 18));

    inner.removeFromTop (Theme::gapSmall);

    if (! modeButtons.empty())
    {
        auto modeRow = inner.removeFromTop (22);
        const int gap = Theme::gapSmall;
        const int count = (int) modeButtons.size();
        const int width = (modeRow.getWidth() - gap * (count - 1)) / count;

        for (int i = 0; i < count; ++i)
        {
            modeButtons[(size_t) i]->setBounds (modeRow.removeFromLeft (width));
            modeRow.removeFromLeft (gap);
        }

        inner.removeFromTop (Theme::gapSmall);
    }

    if (hasReduction)
    {
        meterArea = inner.removeFromBottom (16);
        inner.removeFromBottom (Theme::gapSmall);
    }

    const int count = (int) knobs.size();

    if (count <= 0 || inner.getWidth() <= 0 || inner.getHeight() <= 0)
        return;

    const int gap = Theme::gap;
    const int cellWidth = (inner.getWidth() - gap * (count - 1)) / count;

    for (int i = 0; i < count; ++i)
    {
        auto cell = inner.removeFromLeft (cellWidth);
        inner.removeFromLeft (gap);

        knobLabels[(size_t) i]->setBounds (cell.removeFromTop (14));

        // A lone knob does not need the full card width; capping it keeps the
        // dial a sensible size and leaves the value readout readable.
        const int maxWidth = count == 1 ? 120 : cell.getWidth();
        const int knobWidth = juce::jmin (maxWidth, cell.getWidth());

        knobs[(size_t) i]->setBounds (cell.withSizeKeepingCentre (knobWidth,
                                                                  cell.getHeight()));
    }
}
