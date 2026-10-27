#include "BottomBar.h"
#include "../MainComponent.h"
#include "Strings.h"
#include "Theme.h"

#include <algorithm>
#include <memory>

//==============================================================================
BottomBar::BottomBar (MainComponent& owner)
    : main (owner), library (PresetLibrary::getDefaultDirectory())
{
    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);

    muteButton.setButtonText (Strings::mute());
    muteButton.setClickingTogglesState (true);
    Theme::styleTextButton (muteButton);
    muteButton.setColour (juce::TextButton::buttonOnColourId, Theme::danger);
    muteButton.onClick = [this]()
    {
        const bool wantMuted = muteButton.getToggleState();
        auto& settings = main.getSettings();

        if (settings.params.mute != wantMuted)
        {
            // The live switches are not part of a preset any more than pre-gain
            // is, but editing them still means the sound moved away from it.
            settings.presetName.clear();
            main.setMuted (wantMuted);
        }

        refresh();
    };
    addAndMakeVisible (muteButton);

    bypassButton.setButtonText (Strings::bypass());
    bypassButton.setClickingTogglesState (true);
    Theme::styleTextButton (bypassButton);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, Theme::warn);
    bypassButton.onClick = [this]()
    {
        auto& settings = main.getSettings();
        const bool wanted = bypassButton.getToggleState();

        if (settings.params.bypass != wanted)
        {
            settings.params.bypass = wanted;
            settings.presetName.clear();
            main.getEngine().setParams (settings.params);
            main.settingsChanged();
        }

        refresh();
    };
    addAndMakeVisible (bypassButton);

    presetLabel.setText (Strings::preset(), juce::dontSendNotification);
    Theme::styleLabel (presetLabel, Theme::textSecondary, 12.0f,
                       juce::Justification::centredRight);
    addAndMakeVisible (presetLabel);

    Theme::styleComboBox (presetCombo);
    presetCombo.onChange = [this]() { applySelectedPreset(); };
    addAndMakeVisible (presetCombo);

    saveButton.setButtonText (Strings::savePreset());
    Theme::styleTextButton (saveButton, Theme::cardBgAlt, Theme::textPrimary);
    saveButton.onClick = [this]() { savePreset(); };
    addAndMakeVisible (saveButton);

    deleteButton.setButtonText (Strings::deletePreset());
    Theme::styleTextButton (deleteButton, Theme::cardBgAlt, Theme::textPrimary);
    deleteButton.onClick = [this]() { deleteSelectedPreset(); };
    addAndMakeVisible (deleteButton);

    // Only fills the combo; MainComponent calls refresh() once the saved
    // settings are loaded, so the bar does not read a half-built owner.
    populatePresets();
}

BottomBar::~BottomBar() = default;

//==============================================================================
void BottomBar::populatePresets()
{
    presets = library.getAll();

    presetCombo.clear (juce::dontSendNotification);

    for (int i = 0; i < (int) presets.size(); ++i)
        presetCombo.addItem (presets[(size_t) i].name, i + 1);
}

void BottomBar::reloadPresets()
{
    populatePresets();
    refresh();
}

//==============================================================================
void BottomBar::refresh()
{
    auto& settings = main.getSettings();

    // The first run has no stored preset name, but the default parameters are
    // exactly "Tự nhiên". Matching on every refresh also covers a user who
    // dialled the controls back to a built-in sound: the combo then shows the
    // preset that is actually running instead of nothing.
    if (settings.presetName.isEmpty())
        settings.presetName = matchingBuiltInPreset (settings.params);

    muteButton.setToggleState (settings.params.mute, juce::dontSendNotification);
    bypassButton.setToggleState (settings.params.bypass, juce::dontSendNotification);

    int selectedId = 0;

    for (int i = 0; i < (int) presets.size(); ++i)
        if (presets[(size_t) i].name == settings.presetName)
        {
            selectedId = i + 1;
            break;
        }

    presetCombo.setSelectedId (selectedId, juce::dontSendNotification);

    deleteButton.setEnabled (selectedId > 0
                             && ! presets[(size_t) (selectedId - 1)].builtIn);
}

//==============================================================================
void BottomBar::applySelectedPreset()
{
    const int index = presetCombo.getSelectedId() - 1;

    if (! juce::isPositiveAndBelow (index, (int) presets.size()))
        return;

    // Read the file again rather than trusting the cached copy: a preset edited
    // by hand since the list was built should apply with its current values.
    const auto found = library.find (presets[(size_t) index].name);

    if (! found.has_value())
    {
        reloadPresets();
        return;
    }

    auto& settings = main.getSettings();

    settings.params = applyPreset (settings.params, found->params);
    settings.presetName = found->name;

    main.getEngine().setParams (settings.params);
    main.settingsChanged();

    refresh();

    if (onPresetApplied != nullptr)
        onPresetApplied();
}

//==============================================================================
bool BottomBar::isBuiltInName (const juce::String& name)
{
    for (const auto& preset : PresetLibrary::builtIns())
        if (preset.name == name)
            return true;

    return false;
}

juce::String BottomBar::matchingBuiltInPreset (const VoiceParams& params)
{
    // applyPreset keeps the user's pre-gain, bypass and mute, so comparing its
    // result with the current parameters ignores exactly those three.
    for (const auto& preset : PresetLibrary::builtIns())
        if (applyPreset (params, preset.params) == params)
            return preset.name;

    return {};
}

void BottomBar::showPresetError (const juce::String& title, const juce::String& message)
{
    // Two callers: delete, and the save prompt's modal callback. Posting keeps
    // the window from being opened while the prompt is still closing.
    juce::MessageManager::callAsync ([title, message]()
    {
        auto* window = new juce::AlertWindow (title, message,
                                              juce::MessageBoxIconType::WarningIcon);
        window->addButton (Strings::dismiss(), 1,
                           juce::KeyPress (juce::KeyPress::returnKey));
        window->enterModalState (true, nullptr, true);
    });
}

void BottomBar::savePreset()
{
    promptForPresetName (main.getSettings().presetName, false);
}

void BottomBar::promptForPresetName (juce::String suggested, bool nameWasTaken)
{
    // Modal loops are compiled out of this JUCE build, and they are the wrong
    // shape here anyway: the answer arrives after the click returns.
    auto* window = new juce::AlertWindow (Strings::savePreset(),
                                          nameWasTaken ? Strings::presetNameTaken()
                                                       : Strings::presetNamePrompt(),
                                          juce::MessageBoxIconType::NoIcon);
    // The message line above already asks for the name, so the editor carries no
    // second on-screen label.
    window->addTextEditor ("name", suggested, {});
    window->addButton (Strings::savePreset(), 1,
                       juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton (Strings::dismiss(), 0,
                       juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<BottomBar> safeThis (this);

    window->enterModalState (true,
        juce::ModalCallbackFunction::create ([safeThis, window] (int result)
        {
            // The window is ours to delete: deleteWhenDismissed stays false so
            // the text editor is still readable here.
            std::unique_ptr<juce::AlertWindow> owned (window);

            if (safeThis == nullptr || result != 1)
                return;

            const auto name = window->getTextEditorContents ("name").trim();

            if (name.isEmpty())
                return;

            if (isBuiltInName (name))
            {
                // Keep what was typed; the new prompt explains what went wrong.
                // Deferred: this window is still modal, so the replacement must
                // not be created from inside its own callback.
                juce::MessageManager::callAsync ([safeThis, name]()
                {
                    if (safeThis != nullptr)
                        safeThis->promptForPresetName (name, true);
                });
                return;
            }

            if (safeThis->library.saveUserPreset (name, safeThis->main.getSettings().params))
            {
                safeThis->main.getSettings().presetName = name;
                safeThis->main.settingsChanged();
                safeThis->reloadPresets();
            }
            else
            {
                // The name was valid and not built in, so the only reason left
                // is that the preset file could not be written.
                showPresetError (Strings::savePreset(), Strings::presetSaveFailed());
            }
        }),
        false);
}

void BottomBar::deleteSelectedPreset()
{
    const int index = presetCombo.getSelectedId() - 1;

    if (! juce::isPositiveAndBelow (index, (int) presets.size()))
        return;

    const auto preset = presets[(size_t) index];

    if (preset.builtIn)
        return;

    if (library.deleteUserPreset (preset.name))
    {
        auto& settings = main.getSettings();

        if (settings.presetName == preset.name)
            settings.presetName.clear();

        main.settingsChanged();
        reloadPresets();
    }
    else
    {
        // Built-in names are filtered above and the combo only lists known
        // presets, so a false here means the file could not be removed.
        showPresetError (Strings::deletePreset(), Strings::presetDeleteFailed());
    }
}

//==============================================================================
void BottomBar::updateMeters (const VoiceChain::Levels& levels, float rawInputPeakDb)
{
    // "Vao" is what the chain actually sees: the microphone after the pre-gain,
    // so turning the pre-gain up moves this meter. The raw peak is kept for the
    // clip LED only, because clipping happens in the interface before any
    // software gain, and the test signal replaces the microphone after the raw
    // meter but before the chain.
    inMeter.setLevel (levels.inputDb);
    outMeter.setLevel (levels.outputDb);

    const double now = juce::Time::getMillisecondCounterHiRes();

    if (rawInputPeakDb >= clipThresholdDb)
        clipUntilMs = now + clipHoldMs;

    const bool lit = clipUntilMs > now;

    if (lit != clipLit)
    {
        clipLit = lit;

        if (! clipArea.isEmpty())
            repaint (clipArea);
        else
            repaint();
    }
}

//==============================================================================
void BottomBar::paint (juce::Graphics& g)
{
    Theme::fillCard (g, getLocalBounds(), Theme::panelBg);

    g.setFont (Theme::font (10.5f, juce::Font::bold));
    g.setColour (Theme::textSecondary);
    g.drawText (Strings::meterIn(), inLabelArea, juce::Justification::centred, false);
    g.drawText (Strings::meterOut(), outLabelArea, juce::Justification::centred, false);

    auto clip = clipArea;
    auto led = clip.removeFromLeft (16).withSizeKeepingCentre (10, 10);

    g.setColour (clipLit ? Theme::danger : Theme::wellBg);
    g.fillEllipse (led.toFloat());
    g.setColour (clipLit ? Theme::danger.brighter (0.3f) : Theme::border);
    g.drawEllipse (led.toFloat().reduced (0.5f), 1.0f);

    g.setColour (clipLit ? Theme::danger : Theme::textDim);
    g.setFont (Theme::font (9.5f, juce::Font::bold));
    g.drawText (Strings::clip(), clip, juce::Justification::centredLeft, false);
}

//==============================================================================
void BottomBar::resized()
{
    auto inner = getLocalBounds().reduced (Theme::pad, Theme::gapSmall);

    if (inner.getWidth() <= 0 || inner.getHeight() <= 0)
        return;

    constexpr int meterWidth = 54;
    constexpr int meterGap = 8;

    auto meterBlock = inner.removeFromLeft (meterWidth * 2 + meterGap);

    auto inColumn = meterBlock.removeFromLeft (meterWidth);
    inLabelArea = inColumn.removeFromTop (14);
    inMeter.setBounds (inColumn);

    meterBlock.removeFromLeft (meterGap);
    auto outColumn = meterBlock.removeFromLeft (meterWidth);
    outLabelArea = outColumn.removeFromTop (14);
    outMeter.setBounds (outColumn);

    inner.removeFromLeft (Theme::gap);
    clipArea = inner.removeFromLeft (44).withSizeKeepingCentre (44, 30);
    inner.removeFromLeft (Theme::gap);

    auto row = inner.withSizeKeepingCentre (inner.getWidth(), Theme::rowHeight);

    muteButton.setBounds (row.removeFromLeft (78));
    row.removeFromLeft (Theme::gap);
    bypassButton.setBounds (row.removeFromLeft (78));
    row.removeFromLeft (Theme::gap);

    presetLabel.setBounds (row.removeFromLeft (44).withSizeKeepingCentre (44, 16));
    row.removeFromLeft (Theme::gapSmall);

    // Reserve the two preset buttons before the combo takes the rest, so a long
    // preset name shortens the combo instead of pushing the buttons out.
    deleteButton.setBounds (row.removeFromRight (104));
    row.removeFromRight (Theme::gap);
    saveButton.setBounds (row.removeFromRight (96));
    row.removeFromRight (Theme::gap);

    presetCombo.setBounds (row);
}
