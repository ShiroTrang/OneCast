#pragma once
#include <JuceHeader.h>

#include "../Dsp/DspParams.h"

#include <functional>
#include <memory>
#include <vector>

/** One card of the voice chain.

    A card owns its controls but not the audio state: the owner reads and writes
    VoiceParams through the callbacks below. The card only knows the ranges and
    default values from ParamRanges, so a preset can move every control without
    the card deciding anything.

    The reduction meter is fed with the gain reduction reported by a stage (0 or
    negative dB). It is drawn as a plain bar labelled through Strings::reduction,
    because the stages do not all measure the same thing: the gate is
    peak-detected, the de-esser reports the cut at the centre of its band, and
    the compressor and limiter report true gain reduction.
*/
class ModuleCard : public juce::Component
{
public:
    struct Knob
    {
        juce::String label;
        ParamRange range;
        juce::String unit;
    };

    ModuleCard (juce::String title, bool hasToggle, std::vector<Knob> knobs,
                juce::StringArray modes = {}, bool hasReductionMeter = false);
    ~ModuleCard() override;

    //==========================================================================
    //  Display. A notification type other than dontSendNotification also runs
    //  the matching on* callback, exactly like a user edit.

    void setToggle (bool on, juce::NotificationType notification);
    void setKnob (int index, float value, juce::NotificationType notification);
    void setMode (int index, juce::NotificationType notification);
    void setReductionDb (float db);

    std::function<void (bool)> onToggle;
    std::function<void (int, float)> onKnob;
    std::function<void (int)> onMode;

    //==========================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    class KnobLookAndFeel;
    class PowerButton;

    void applyDimming();
    static juce::String suffixFor (const juce::String& unit);
    static int decimalPlacesFor (const juce::String& unit);

    juce::String title;
    bool hasReduction = false;
    bool toggleOn = true;

    /// Declared before the sliders so it outlives them; the destructor detaches
    /// every slider first, so JUCE never sees a slider outlive its look.
    std::unique_ptr<KnobLookAndFeel> lookAndFeel;

    std::unique_ptr<PowerButton> toggle;
    std::vector<std::unique_ptr<juce::Label>> knobLabels;
    std::vector<std::unique_ptr<juce::Slider>> knobs;
    std::vector<std::unique_ptr<juce::TextButton>> modeButtons;

    float reductionDb = 0.0f;

    juce::Rectangle<int> titleArea, meterArea;

    /// The gate is the deepest stage (a closed gate is -30 dB), so the bar is
    /// scaled to that rather than to a compressor-sized reduction.
    static constexpr float meterRangeDb = 30.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModuleCard)
};
