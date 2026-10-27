#pragma once
#include <JuceHeader.h>
#include <atomic>

/**
    A vertical audio level meter with smooth peak decay and peak hold indicator.

    Thread-safe: call setLevel() from the audio thread; the component
    repaints itself at ~30 Hz via juce::Timer.

    Visual range: -60 dB to +6 dB.
    Colour gradient: green (-60 to -12), yellow (-12 to -3), red (-3 to +6).
*/
class MeterComponent : public juce::Component,
                       private juce::Timer
{
public:
    MeterComponent();
    ~MeterComponent() override = default;

    /** Set the current peak level in dB. Called from the message thread;
        the audio thread publishes the level through an atomic in AudioEngine. */
    void setLevel (float newLevelDb);

    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;

    std::atomic<float> currentLevel { -60.0f };
    float displayLevel  = -60.0f;
    float peakLevel     = -60.0f;
    int   peakHoldCounter = 0;

    static constexpr float minDb = -60.0f;
    static constexpr float maxDb =   6.0f;
    static constexpr int   peakHoldFrames = 30;   // ~1 s at 30 Hz
    static constexpr float decayRate      = 1.5f; // dB per frame

    /** Map a dB value to 0..1 normalised position inside the meter. */
    static float dbToNorm (float db);

    /** Return the gradient colour for a given normalised position. */
    static juce::Colour colourForNorm (float norm);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterComponent)
};
