#pragma once
#include <JuceHeader.h>

#include "Audio/AudioEngine.h"
#include "Audio/DeviceRecovery.h"
#include "Platform/DeviceHotplug.h"
#include "Session/Settings.h"
#include "UI/AlertBanner.h"
#include "UI/DevicePanel.h"

#include <functional>
#include <memory>

/** The main window: device routing on top, the processing chain below.

    Task 12 owns the device panel and the recovery logic ported from LiveVocal.
    The chain area is a placeholder until Task 13, and the tray/hotkey live in
    Task 14; the public API below is what those tasks build on.

    Everything here runs on the message thread. The audio callback only ever
    sees AudioEngine's atomics and its ring buffer.
*/
class MainComponent : public juce::Component,
                      private juce::Timer,
                      private DeviceHotplug::Listener
{
public:
    MainComponent();
    ~MainComponent() override;

    AudioEngine& getEngine() noexcept;
    AppSettings& getSettings() noexcept;

    /// Marks the settings dirty; they are written one second after the last
    /// change, so dragging a knob does not write the file on every step.
    void settingsChanged();

    /// Engine, settings and listeners in one call. Mute is deliberately not
    /// persisted (Settings::paramsToVar leaves it out), so a restart never
    /// starts silently muted.
    void setMuted (bool muted);
    bool isMuted() const;

    /// Called on the message thread after setMuted(), for the tray icon.
    std::function<void (bool muted)> onMuteChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    // Which alert is on screen, so recovering from one condition does not
    // dismiss a different, still valid banner.
    enum class BannerKind { none, inputLost, openFailed, outputLost, noCable, inputClip };

    void timerCallback() override;

    // DeviceHotplug::Listener (already dispatched to the message thread)
    void deviceAdded (const juce::String& deviceId) override;
    void deviceRemoved (const juce::String& deviceId) override;
    void deviceStateChanged (const juce::String& deviceId, DWORD newState) override;
    void defaultDeviceChanged (EDataFlow flow, ERole role, const juce::String& newDeviceId) override;

    // DevicePanel callbacks
    void audioApiChanged (const juce::String& typeName);
    void inputDeviceChanged (const juce::String& name);
    void inputChannelChanged (InputChannel channel);
    void bufferSizeChanged (int samples);
    void outputDeviceChanged (const juce::String& id);

    // Device state
    std::unique_ptr<juce::XmlElement> parseSavedDeviceState() const;
    static juce::String inputNameFromState (const juce::XmlElement& state);
    juce::String currentInputName();
    int activeInputChannels();

    void refreshDeviceLists();
    void syncDevicePanelFromEngine();
    void chooseOutputDevice (bool allowCableFallback);
    bool startOutput (const juce::String& id, const juce::String& name);
    void markOutputLost();

    // Recovery
    void watchAudioDevice();
    void audioDeviceLost (bool closeIt);
    void tryReopenLostDevice();
    void endpointArrived();
    void endpointDeparted();
    void scheduleDeviceListRefresh();

    // Settings and banners
    bool saveSettings();
    void showBanner (BannerKind kind, const juce::String& message,
                     AlertBanner::Severity severity);
    void dismissBanner (BannerKind kind);
    void logDeviceEvent (const juce::String& what);

    //==========================================================================
    AppSettings settings;
    AudioEngine engine;
    DevicePanel devicePanel;
    AlertBanner alertBanner;
    DeviceHotplug deviceHotplug;
    DeviceRecovery deviceRecovery;

    /// The device to bring back after an unplug, and what the output was called
    /// for the banner while it is away.
    std::unique_ptr<juce::XmlElement> lostDeviceState;
    juce::String outputName;

    bool settingsDirty = false;
    double settingsSaveDueMs = 0.0;

    /// Set while an ASIO driver may run the message loop; the timer must not
    /// start a second reopen from inside the first one.
    bool deviceBusy = false;

    bool outputConfigured = false;
    bool outputDown = false;
    bool cableMissing = false;

    double deviceListRefreshDueMs = 0.0;
    double clipBannerUntilMs = 0.0;

    BannerKind currentBanner = BannerKind::none;

    juce::Rectangle<int> chainArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
