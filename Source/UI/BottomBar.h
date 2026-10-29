#pragma once
#include <JuceHeader.h>

#include "../Dsp/VoiceChain.h"
#include "../Session/Presets.h"
#include "MeterComponent.h"

#include <functional>
#include <vector>

class MainComponent;

/** The bottom bar: what goes in and out, the two live switches, and presets.

    The in/out meters are read from the engine; the preset list is shown exactly
    as PresetLibrary returns it (built-in names are never copied into Strings).
    Choosing a preset applies it through applyPreset, so the user's pre-gain,
    bypass and mute survive. Touching any control clears the selected name.
*/
class BottomBar : public juce::Component
{
public:
    static constexpr int preferredHeight = 90;

    explicit BottomBar (MainComponent&);
    ~BottomBar() override;

    void updateMeters (const VoiceChain::Levels& levels, float rawInputPeakDb);

    /// Reflects mute, bypass, the selected preset and whether it is deletable.
    /// Reads only the cached preset names; open/save/delete reload them.
    void refresh();

    /// Fired after a preset was applied, so the cards can follow.
    std::function<void()> onPresetApplied;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void populatePresets();
    void reloadPresets();
    void applySelectedPreset();
    void savePreset();

    /// A modal loop is not permitted in this JUCE build, so the prompt is an
    /// asynchronous AlertWindow. A built-in name reopens it with the reason.
    void promptForPresetName (juce::String suggested, bool nameWasTaken);
    void deleteSelectedPreset();

    /// The name of a built-in whose processing exactly matches `params`, with
    /// pre-gain, bypass and mute ignored just like applyPreset() ignores them.
    static juce::String matchingBuiltInPreset (const VoiceParams& params);

    /// A disk or permission failure looked silent; say so. Posted asynchronously
    /// because the save path calls it from inside a closing modal window.
    static void showPresetError (const juce::String& title, const juce::String& message);

    static bool isBuiltInName (const juce::String& name);

    MainComponent& main;
    PresetLibrary library;
    std::vector<Preset> presets;

    MeterComponent inMeter, outMeter;
    juce::TextButton muteButton, bypassButton, saveButton, deleteButton;
    juce::Label presetLabel;
    juce::ComboBox presetCombo;

    double clipUntilMs = 0.0;
    bool clipLit = false;

    juce::Rectangle<int> inLabelArea, outLabelArea, clipArea;

    static constexpr float clipThresholdDb = -0.1f;
    static constexpr double clipHoldMs = 1000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BottomBar)
};
