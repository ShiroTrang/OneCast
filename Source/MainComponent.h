#pragma once
#include <JuceHeader.h>

/// Placeholder content for the main window. It only proves the scaffold links
/// and starts; the real UI replaces it.
class MainComponent : public juce::Component
{
public:
    MainComponent();

    void paint (juce::Graphics& g) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
