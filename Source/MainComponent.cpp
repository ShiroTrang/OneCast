#include "MainComponent.h"
#include "UI/Theme.h"

MainComponent::MainComponent()
{
    setSize (480, 320);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::windowBg);

    g.setColour (Theme::textPrimary);
    g.setFont (Theme::appTitleFont());
    g.drawText ("OneCast", getLocalBounds(), juce::Justification::centred, false);
}
