#include "ChainPanel.h"
#include "../MainComponent.h"
#include "ModuleCard.h"
#include "Strings.h"
#include "Theme.h"

namespace
{
    std::vector<ModuleCard::Knob> oneKnob (const juce::String& label,
                                           const ParamRange& range,
                                           const juce::String& unit)
    {
        return { ModuleCard::Knob { label, range, unit } };
    }
}

//==============================================================================
ChainPanel::ChainPanel (MainComponent& owner)
    : main (owner)
{
    juce::StringArray noiseModes;
    noiseModes.add (Strings::noiseOff());
    noiseModes.add (Strings::noiseLight());
    noiseModes.add (Strings::noiseAi());

    preGainCard = std::make_unique<ModuleCard> (Strings::preGain(), false,
                      oneKnob (Strings::preGain(), ParamRanges::preGainDb, "dB"));

    hpfCard = std::make_unique<ModuleCard> (Strings::hpf(), true,
                  oneKnob (Strings::hpf(), ParamRanges::hpfHz, "Hz"));

    noiseCard = std::make_unique<ModuleCard> (Strings::noise(), false,
                    oneKnob (Strings::amount(), ParamRanges::noiseAmount, "%"),
                    noiseModes);

    gateCard = std::make_unique<ModuleCard> (Strings::gate(), true,
                   oneKnob (Strings::threshold(), ParamRanges::gateThresholdDb, "dB"),
                   juce::StringArray(), true);

    eqCard = std::make_unique<ModuleCard> (Strings::eq(), true,
                 std::vector<ModuleCard::Knob> {
                     { Strings::eqMud(),     ParamRanges::eqMudDb,     "dB" },
                     { Strings::eqClarity(), ParamRanges::eqClarityDb, "dB" },
                     { Strings::eqAir(),     ParamRanges::eqAirDb,     "dB" } });

    compCard = std::make_unique<ModuleCard> (Strings::compressor(), true,
                   std::vector<ModuleCard::Knob> {
                       { Strings::threshold(), ParamRanges::compThresholdDb, "dB" },
                       { Strings::ratio(),     ParamRanges::compRatio,       ":1" },
                       { Strings::makeup(),    ParamRanges::compMakeupDb,    "dB" } },
                   juce::StringArray(), true);

    deEssCard = std::make_unique<ModuleCard> (Strings::deEsser(), true,
                     oneKnob (Strings::amount(), ParamRanges::deEssAmount, "%"),
                     juce::StringArray(), true);

    outputCard = std::make_unique<ModuleCard> (Strings::output(), false,
                      oneKnob (Strings::outputGain(), ParamRanges::outputGainDb, "dB"),
                      juce::StringArray(), true);

    cards = { preGainCard.get(), hpfCard.get(), noiseCard.get(), gateCard.get(),
              eqCard.get(), compCard.get(), deEssCard.get(), outputCard.get() };

    for (auto* card : cards)
        addAndMakeVisible (*card);

    // Each callback writes the field it owns and then runs the common path, so
    // no card has to know about the others or about the preset name.
    preGainCard->onKnob = [this] (int, float value)
    {
        main.getSettings().params.preGainDb = value;
        pushParams();
    };

    hpfCard->onToggle = [this] (bool on)
    {
        main.getSettings().params.hpfOn = on;
        pushParams();
    };
    hpfCard->onKnob = [this] (int, float value)
    {
        main.getSettings().params.hpfHz = value;
        pushParams();
    };

    noiseCard->onMode = [this] (int index)
    {
        main.getSettings().params.noiseMode = (NoiseMode) index;
        pushParams();
    };
    noiseCard->onKnob = [this] (int, float value)
    {
        main.getSettings().params.noiseAmount = value;
        pushParams();
    };

    gateCard->onToggle = [this] (bool on)
    {
        main.getSettings().params.gateOn = on;
        pushParams();
    };
    gateCard->onKnob = [this] (int, float value)
    {
        main.getSettings().params.gateThresholdDb = value;
        pushParams();
    };

    eqCard->onToggle = [this] (bool on)
    {
        main.getSettings().params.eqOn = on;
        pushParams();
    };
    eqCard->onKnob = [this] (int index, float value)
    {
        auto& params = main.getSettings().params;

        if (index == 0) params.eqMudDb = value;
        else if (index == 1) params.eqClarityDb = value;
        else if (index == 2) params.eqAirDb = value;

        pushParams();
    };

    compCard->onToggle = [this] (bool on)
    {
        main.getSettings().params.compOn = on;
        pushParams();
    };
    compCard->onKnob = [this] (int index, float value)
    {
        auto& params = main.getSettings().params;

        if (index == 0) params.compThresholdDb = value;
        else if (index == 1) params.compRatio = value;
        else if (index == 2) params.compMakeupDb = value;

        pushParams();
    };

    deEssCard->onToggle = [this] (bool on)
    {
        main.getSettings().params.deEssOn = on;
        pushParams();
    };
    deEssCard->onKnob = [this] (int, float value)
    {
        main.getSettings().params.deEssAmount = value;
        pushParams();
    };

    outputCard->onKnob = [this] (int, float value)
    {
        main.getSettings().params.outputGainDb = value;
        pushParams();
    };
}

ChainPanel::~ChainPanel() = default;

//==============================================================================
void ChainPanel::pushParams()
{
    auto& settings = main.getSettings();

    // Touching any control means the sound no longer matches the preset it came
    // from, so the name is dropped before the settings are marked dirty.
    settings.presetName.clear();

    main.getEngine().setParams (settings.params);
    main.settingsChanged();

    if (onEdited != nullptr)
        onEdited();
}

//==============================================================================
void ChainPanel::refreshFromParams()
{
    const auto params = main.getSettings().params;

    preGainCard->setKnob (0, params.preGainDb, juce::dontSendNotification);

    hpfCard->setToggle (params.hpfOn, juce::dontSendNotification);
    hpfCard->setKnob (0, params.hpfHz, juce::dontSendNotification);

    noiseCard->setMode ((int) params.noiseMode, juce::dontSendNotification);
    noiseCard->setKnob (0, params.noiseAmount, juce::dontSendNotification);

    gateCard->setToggle (params.gateOn, juce::dontSendNotification);
    gateCard->setKnob (0, params.gateThresholdDb, juce::dontSendNotification);

    eqCard->setToggle (params.eqOn, juce::dontSendNotification);
    eqCard->setKnob (0, params.eqMudDb, juce::dontSendNotification);
    eqCard->setKnob (1, params.eqClarityDb, juce::dontSendNotification);
    eqCard->setKnob (2, params.eqAirDb, juce::dontSendNotification);

    compCard->setToggle (params.compOn, juce::dontSendNotification);
    compCard->setKnob (0, params.compThresholdDb, juce::dontSendNotification);
    compCard->setKnob (1, params.compRatio, juce::dontSendNotification);
    compCard->setKnob (2, params.compMakeupDb, juce::dontSendNotification);

    deEssCard->setToggle (params.deEssOn, juce::dontSendNotification);
    deEssCard->setKnob (0, params.deEssAmount, juce::dontSendNotification);

    outputCard->setKnob (0, params.outputGainDb, juce::dontSendNotification);
}

void ChainPanel::updateMeters (const VoiceChain::Levels& levels)
{
    gateCard->setReductionDb (levels.gateDb);
    compCard->setReductionDb (levels.compDb);
    deEssCard->setReductionDb (levels.deEssDb);
    outputCard->setReductionDb (levels.limiterDb);
}

//==============================================================================
void ChainPanel::resized()
{
    auto area = getLocalBounds();

    constexpr int columns = 4;
    constexpr int rows = 2;

    if (area.getWidth() <= 0 || area.getHeight() <= 0)
        return;

    const int gap = Theme::gap;
    const int cellWidth = juce::jmax (1, (area.getWidth() - gap * (columns - 1)) / columns);
    const int cellHeight = juce::jmax (1, (area.getHeight() - gap * (rows - 1)) / rows);

    for (int i = 0; i < (int) cards.size(); ++i)
    {
        const int column = i % columns;
        const int row = i / columns;

        cards[(size_t) i]->setBounds (area.getX() + column * (cellWidth + gap),
                                      area.getY() + row * (cellHeight + gap),
                                      cellWidth, cellHeight);
    }
}
