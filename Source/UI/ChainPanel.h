#pragma once
#include <JuceHeader.h>

#include "../Dsp/VoiceChain.h"

#include <array>
#include <functional>
#include <memory>

class MainComponent;
class ModuleCard;

/** The eight processing cards, in signal order.

    The panel owns the view only. Every edit is written straight into the
    settings, handed to the engine and persisted through MainComponent, so the
    cards, the audio thread and the settings file always describe the same
    VoiceParams.

    Layout is two rows of four, which keeps every card legible at the window's
    1280 x 720 minimum instead of squeezing eight columns across.
*/
class ChainPanel : public juce::Component
{
public:
    explicit ChainPanel (MainComponent&);
    ~ChainPanel() override;

    /// Pulls every control from the current settings. Used at start-up and
    /// after a preset is applied; it never sends a change back to the engine.
    void refreshFromParams();

    /// Feeds the four gain-reduction meters. Safe to call at the UI timer rate.
    void updateMeters (const VoiceChain::Levels& levels);

    /// Fired after a user edit, so the owner can drop the preset selection.
    std::function<void()> onEdited;

    void resized() override;

private:
    void pushParams();

    MainComponent& main;

    std::unique_ptr<ModuleCard> preGainCard, hpfCard, noiseCard, gateCard,
                                eqCard, compCard, deEssCard, outputCard;

    std::array<ModuleCard*, 8> cards {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainPanel)
};
