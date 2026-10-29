#include "DevicePanel.h"
#include "Strings.h"
#include "Theme.h"

#include <cmath>

namespace
{
    constexpr int labelGap = 4;
    constexpr int fieldGap = 10;

    // Fixed widths for the fields whose names have a known, short length. The
    // input and output device names take whatever is left, which is where the
    // extra room belongs.
    constexpr int apiLabelW     = 34;
    constexpr int apiComboW     = 110;
    constexpr int deviceLabelW  = 54;
    constexpr int channelLabelW = 40;
    constexpr int channelComboW = 110;
    constexpr int bufferLabelW  = 44;
    constexpr int bufferComboW  = 84;
    constexpr int outputLabelW  = 48;

    constexpr int statusW  = 112;
    constexpr int latencyW = 150;
    constexpr int headerH  = 16;

    // The gear sits in the header line, so it stays clear of the device fields
    // a long input name can squeeze.
    constexpr int optionsW = 96;
    constexpr int optionsH = 18;
}

//==============================================================================
DevicePanel::OptionsButton::OptionsButton()
    : Button (Strings::options())
{
}

void DevicePanel::OptionsButton::paintButton (juce::Graphics& g,
                                              bool shouldDrawButtonAsHighlighted,
                                              bool shouldDrawButtonAsDown)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (shouldDrawButtonAsDown ? Theme::accentDim
                 : shouldDrawButtonAsHighlighted ? Theme::cardBgAlt.brighter (0.12f)
                                                 : Theme::cardBgAlt);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (Theme::border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.0f);

    // A small cog drawn from eight teeth around a ring: at this size an icon
    // font would be at the mercy of installed fonts, and an image would need a
    // resource file.
    const auto icon = bounds.removeFromLeft (bounds.getHeight()).reduced (2.0f);
    const auto centre = icon.getCentre();
    const float outer = icon.getHeight() * 0.42f;
    const float inner = outer * 0.60f;

    g.setColour (Theme::textSecondary);

    for (int i = 0; i < 8; ++i)
    {
        const float angle = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
        const auto direction = juce::Point<float> (std::cos (angle), std::sin (angle));

        g.drawLine ({ centre + direction * inner, centre + direction * outer }, 1.4f);
    }

    g.drawEllipse (centre.x - inner, centre.y - inner, inner * 2.0f, inner * 2.0f, 1.4f);
    g.fillEllipse (centre.x - inner * 0.35f, centre.y - inner * 0.35f,
                   inner * 0.7f, inner * 0.7f);

    g.setColour (Theme::textPrimary);
    g.setFont (Theme::font (12.0f));
    g.drawText (getButtonText(), bounds.reduced (3.0f, 0.0f),
                juce::Justification::centredLeft, false);
}

//==============================================================================
DevicePanel::DevicePanel()
{
    for (auto* label : { &apiLabel, &deviceLabel, &channelLabel, &bufferLabel, &outputLabel })
    {
        Theme::styleLabel (*label, Theme::textSecondary, 12.0f, juce::Justification::centredRight);
        addAndMakeVisible (*label);
    }

    apiLabel.setText     (Strings::apiType(), juce::dontSendNotification);
    deviceLabel.setText  (Strings::device(),  juce::dontSendNotification);
    channelLabel.setText (Strings::channel(), juce::dontSendNotification);
    bufferLabel.setText  (Strings::buffer(),  juce::dontSendNotification);
    outputLabel.setText  (Strings::output(),  juce::dontSendNotification);

    for (auto* combo : { &apiCombo, &inputCombo, &channelCombo, &bufferCombo, &outputCombo })
    {
        Theme::styleComboBox (*combo);
        addAndMakeVisible (*combo);
    }

    channelCombo.addItem (Strings::channel1(),   (int) InputChannel::first  + 1);
    channelCombo.addItem (Strings::channel2(),   (int) InputChannel::second + 1);
    channelCombo.addItem (Strings::channelMix(), (int) InputChannel::mix    + 1);
    channelCombo.setSelectedId ((int) InputChannel::first + 1, juce::dontSendNotification);

    Theme::styleLabel (latencyLabel, Theme::textSecondary, 12.0f, juce::Justification::centredRight);
    latencyLabel.setFont (Theme::valueFont());
    latencyLabel.setText (Strings::latency().replace ("%1", juce::String (0.0f, 1)),
                          juce::dontSendNotification);
    addAndMakeVisible (latencyLabel);

    optionsButton.setTooltip (Strings::options());
    optionsButton.onClick = [this]()
    {
        if (onOptionsClicked != nullptr)
            onOptionsClicked();
    };
    addAndMakeVisible (optionsButton);

    buildCombos();
    setSize (900, preferredHeight);
}

//==============================================================================
void DevicePanel::buildCombos()
{
    // onChange only fires for a real user edit: every show*() call uses
    // dontSendNotification, and repopulating uses the same.
    apiCombo.onChange = [this]()
    {
        const int index = apiCombo.getSelectedItemIndex();

        if (index >= 0 && index < apiNames.size() && onApiChanged != nullptr)
            onApiChanged (apiNames[index]);
    };

    inputCombo.onChange = [this]()
    {
        const int index = inputCombo.getSelectedItemIndex();

        if (index >= 0 && index < inputNames.size() && onInputChanged != nullptr)
            onInputChanged (inputNames[index]);
    };

    channelCombo.onChange = [this]()
    {
        const int id = channelCombo.getSelectedId();

        if (id > 0 && onChannelChanged != nullptr)
            onChannelChanged ((InputChannel) (id - 1));
    };

    bufferCombo.onChange = [this]()
    {
        const int index = bufferCombo.getSelectedItemIndex();

        if (index >= 0 && index < bufferSizeList.size() && onBufferSizeChanged != nullptr)
            onBufferSizeChanged (bufferSizeList[index]);
    };

    outputCombo.onChange = [this]()
    {
        const int index = outputCombo.getSelectedItemIndex();

        if (index >= 0 && index < outputIds.size() && onOutputChanged != nullptr)
            onOutputChanged (outputIds[index]);
    };
}

//==============================================================================
void DevicePanel::setApiTypes (const juce::StringArray& names)
{
    apiNames = names;

    apiCombo.clear (juce::dontSendNotification);

    for (int i = 0; i < apiNames.size(); ++i)
        apiCombo.addItem (apiNames[i], i + 1);
}

void DevicePanel::setInputDevices (const juce::StringArray& names)
{
    inputNames = names;

    inputCombo.clear (juce::dontSendNotification);

    for (int i = 0; i < inputNames.size(); ++i)
        inputCombo.addItem (inputNames[i], i + 1);
}

void DevicePanel::setBufferSizes (const juce::Array<int>& sizes)
{
    bufferSizeList = sizes;

    bufferCombo.clear (juce::dontSendNotification);

    // A buffer size is a number, not copy: the "Buffer" label already says what
    // it is, and the samples unit is the same in every language.
    for (int i = 0; i < bufferSizeList.size(); ++i)
        bufferCombo.addItem (juce::String (bufferSizeList[i]), i + 1);
}

void DevicePanel::setOutputDevices (const std::vector<WasapiOutputDevice::DeviceInfo>& devices)
{
    outputIds.clear();
    outputCombo.clear (juce::dontSendNotification);

    for (const auto& device : devices)
    {
        outputIds.add (device.id);
        outputCombo.addItem (device.name, outputIds.size());
    }
}

//==============================================================================
void DevicePanel::showApi (const juce::String& typeName)
{
    const int index = typeName.isEmpty() ? -1 : apiNames.indexOf (typeName);

    if (index >= 0)
        apiCombo.setSelectedItemIndex (index, juce::dontSendNotification);
    else if (typeName.isEmpty())
        apiCombo.setSelectedId (0, juce::dontSendNotification);
}

void DevicePanel::showInputDevice (const juce::String& name)
{
    if (name.isEmpty())
    {
        inputCombo.setSelectedId (0, juce::dontSendNotification);
        return;
    }

    int index = inputNames.indexOf (name);

    // The chosen microphone is unplugged: keep it visible in the combo, where
    // the user can see what the app is waiting for, instead of silently
    // pretending the next device in the list is the one in use.
    if (index < 0)
    {
        inputNames.add (name);
        inputCombo.addItem (name, inputNames.size());
        index = inputNames.size() - 1;
    }

    inputCombo.setSelectedItemIndex (index, juce::dontSendNotification);
}

void DevicePanel::showChannel (InputChannel channel)
{
    channelCombo.setSelectedId ((int) channel + 1, juce::dontSendNotification);
}

void DevicePanel::showBufferSize (int samples)
{
    const int index = samples > 0 ? bufferSizeList.indexOf (samples) : -1;

    if (index >= 0)
        bufferCombo.setSelectedItemIndex (index, juce::dontSendNotification);
    else if (samples <= 0)
        bufferCombo.setSelectedId (0, juce::dontSendNotification);
}

void DevicePanel::showOutputDevice (const juce::String& id, const juce::String& knownName)
{
    if (id.isEmpty())
    {
        outputCombo.setSelectedId (0, juce::dontSendNotification);
        return;
    }

    int index = outputIds.indexOf (id);

    // Same as the input: a render endpoint that is away is still the chosen
    // output, and the banner explains that it is being waited for.
    if (index < 0)
    {
        outputIds.add (id);
        outputCombo.addItem (knownName.isNotEmpty() ? knownName : id, outputIds.size());
        index = outputIds.size() - 1;
    }

    outputCombo.setSelectedItemIndex (index, juce::dontSendNotification);
}

void DevicePanel::setChannelCount (int activeInputs)
{
    if (activeInputs <= 0 || activeInputs == channelCount)
        return;

    channelCount = activeInputs;
    channelCombo.setItemEnabled ((int) InputChannel::second + 1, activeInputs >= 2);
}

void DevicePanel::setRunning (bool isRunning)
{
    if (running == isRunning)
        return;

    running = isRunning;
    repaint();
}

void DevicePanel::setLatencyMs (float milliseconds)
{
    const auto text = Strings::latency().replace ("%1", juce::String (milliseconds, 1));

    if (latencyLabel.getText() != text)
        latencyLabel.setText (text, juce::dontSendNotification);
}

juce::Rectangle<int> DevicePanel::getOptionsButtonBounds() const noexcept
{
    return optionsButton.getBounds();
}

//==============================================================================
void DevicePanel::paint (juce::Graphics& g)
{
    Theme::fillCard (g, getLocalBounds(), Theme::panelBg);

    g.setColour (Theme::textSecondary);
    g.setFont (Theme::cardTitleFont());
    g.drawText (Strings::input(), inputTitleArea, juce::Justification::centredLeft, false);

    auto dot = statusArea;

    // A filled dot carries the state before the label is read.
    g.setColour (running ? Theme::ok : Theme::textDim);
    g.fillEllipse ((float) dot.getX(), (float) dot.getCentreY() - 4.0f, 8.0f, 8.0f);

    g.setColour (running ? Theme::ok : Theme::textSecondary);
    g.setFont (Theme::font (12.5f, juce::Font::bold));
    g.drawText (running ? Strings::statusRunning() : Strings::statusStopped(),
                dot.withTrimmedLeft (14), juce::Justification::centredLeft, false);
}

void DevicePanel::resized()
{
    resizedControls();
}

void DevicePanel::resizedControls()
{
    auto content = getLocalBounds().reduced (Theme::pad);

    if (content.getWidth() <= 0 || content.getHeight() <= 0)
        return;

    // The heading/status line. The latency sits at the far right.
    auto header = content.removeFromTop (headerH);
    content.removeFromTop (Theme::gapSmall);

    auto optionsArea = header.removeFromRight (optionsW);
    optionsButton.setBounds (optionsArea.withSizeKeepingCentre (optionsW, optionsH));
    header.removeFromRight (Theme::gapSmall);

    latencyArea = header.removeFromRight (latencyW);
    header.removeFromRight (Theme::gapSmall);
    statusArea = header.removeFromRight (statusW);
    header.removeFromRight (Theme::gapSmall);
    inputTitleArea = header;

    if (content.getHeight() <= 0)
        return;

    auto row = content.removeFromTop (Theme::rowHeight);

    latencyLabel.setBounds (latencyArea);
    apiLabel.setBounds (row.removeFromLeft (apiLabelW));
    row.removeFromLeft (labelGap);
    apiCombo.setBounds (row.removeFromLeft (apiComboW));
    row.removeFromLeft (fieldGap);

    deviceLabel.setBounds (row.removeFromLeft (deviceLabelW));
    row.removeFromLeft (labelGap);

    // Reserve the channel, buffer and output fields before handing the rest to
    // the device name, so a long name shortens instead of pushing fields out.
    const int trailing = channelLabelW + labelGap + channelComboW + fieldGap
                       + bufferLabelW  + labelGap + bufferComboW  + fieldGap
                       + outputLabelW  + labelGap;

    const int availableForFlexible = row.getWidth() - trailing - fieldGap;
    int inputWidth = availableForFlexible / 2;

    if (inputWidth < 120 && availableForFlexible > 0)
        inputWidth = juce::jmax (40, availableForFlexible - 80);

    inputWidth = juce::jlimit (0, row.getWidth(), inputWidth);

    inputCombo.setBounds (row.removeFromLeft (inputWidth));
    row.removeFromLeft (fieldGap);

    channelLabel.setBounds (row.removeFromLeft (channelLabelW));
    row.removeFromLeft (labelGap);
    channelCombo.setBounds (row.removeFromLeft (channelComboW));
    row.removeFromLeft (fieldGap);

    bufferLabel.setBounds (row.removeFromLeft (bufferLabelW));
    row.removeFromLeft (labelGap);
    bufferCombo.setBounds (row.removeFromLeft (bufferComboW));
    row.removeFromLeft (fieldGap);

    outputLabel.setBounds (row.removeFromLeft (outputLabelW));
    row.removeFromLeft (labelGap);
    outputCombo.setBounds (row);
}
