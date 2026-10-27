#pragma once
#include <JuceHeader.h>

#include "../Audio/WasapiOutputDevice.h"
#include "../Dsp/DspParams.h"

#include <functional>
#include <vector>

/** The top bar: where the voice comes from and where it goes.

    A dumb view. MainComponent enumerates the devices (the engine already
    filters the microphone list and the caller sorts the render endpoints with
    CABLE Input first) and the panel only shows them. Every user edit leaves
    through one of the std::function callbacks, so the panel never touches the
    audio engine itself.

    The panel paints its own card and group heading; only the combos and the
    latency label are child components.
*/
class DevicePanel : public juce::Component
{
public:
    /// The layout needs two lines: the group heading/status line and the controls.
    static constexpr int preferredHeight = 68;

    DevicePanel();

    //==========================================================================
    //  Population (message thread, when devices come or go)

    void setApiTypes      (const juce::StringArray& names);
    void setInputDevices  (const juce::StringArray& names);
    void setBufferSizes   (const juce::Array<int>& sizes);
    void setOutputDevices (const std::vector<WasapiOutputDevice::DeviceInfo>& devices);

    //==========================================================================
    //  Display (what is really in use; never sends a change notification)

    void showApi          (const juce::String& typeName);
    void showInputDevice  (const juce::String& name);
    void showChannel      (InputChannel channel);
    void showBufferSize   (int samples);

    /// A lost output is no longer in the list; the known name keeps the combo
    /// showing what is waiting to come back instead of going blank.
    void showOutputDevice (const juce::String& id, const juce::String& knownName = {});

    /// Disables the second channel on a mono microphone. Counts <= 0 mean "unknown"
    /// (no device open): the current state is left alone.
    void setChannelCount (int activeInputs);

    void setRunning   (bool isRunning);
    void setLatencyMs (float milliseconds);

    //==========================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;

    //==========================================================================
    //  User edits. The panel has reselected the combo already; the owner
    //  decides whether the engine accepted the change and calls show* again.
    std::function<void (const juce::String& typeName)> onApiChanged;
    std::function<void (const juce::String& inputName)> onInputChanged;
    std::function<void (InputChannel channel)> onChannelChanged;
    std::function<void (int bufferSize)> onBufferSizeChanged;
    std::function<void (const juce::String& outputId)> onOutputChanged;

private:
    void buildCombos();
    void resizedControls();

    //==========================================================================
    juce::Label apiLabel, deviceLabel, channelLabel, bufferLabel, outputLabel;
    juce::ComboBox apiCombo, inputCombo, channelCombo, bufferCombo, outputCombo;
    juce::Label latencyLabel;

    juce::StringArray apiNames, inputNames, outputIds;
    juce::Array<int> bufferSizeList;

    bool running = false;
    int channelCount = -1;

    // Laid out once in resized(); paint() reuses them so the heading, the dot
    // and the text cannot drift away from their controls.
    juce::Rectangle<int> inputTitleArea, statusArea, latencyArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DevicePanel)
};
