#include "MainComponent.h"
#include "Platform/AutoStart.h"
#include "UI/OptionsPanel.h"
#include "UI/Strings.h"
#include "UI/Theme.h"
#include "Diagnostics/StartupLog.h"

#include <algorithm>

namespace
{
    constexpr double settingsSaveDelayMs     = 1000.0;
    constexpr double settingsRetryDelayMs    = 5000.0;
    constexpr double clipBannerDurationMs    = 5000.0;
    constexpr double deviceListRefreshDelayMs = 1000.0;

    juce::String outputNameForId (const std::vector<WasapiOutputDevice::DeviceInfo>& devices,
                                  const juce::String& id)
    {
        for (const auto& device : devices)
            if (device.id == id)
                return device.name;

        return {};
    }

    /// The engine keeps the driver's English wording for the start-up log.
    /// The banner shows one short Vietnamese reason: users cannot tell "No
    /// such device" from "Couldn't open the input device!" and it is not
    /// translated anywhere.
    juce::String deviceErrorText (const juce::String& detail)
    {
        const auto lower = detail.toLowerCase();

        if (lower.contains ("exclusive") || lower.contains ("in use")
            || lower.contains ("busy") || lower.contains ("being used"))
            return Strings::errorDeviceBusy();

        if (lower.contains ("no such device") || lower.contains ("not found"))
            return Strings::errorNotFound();

        return Strings::errorUnknown();
    }
}

//==============================================================================
MainComponent::MainComponent()
{
    setOpaque (true);
    setSize (1280, 720);

    settings = SettingsIO::load (SettingsIO::getDefaultFile());
    StartupLog::mark ("ctor: settings loaded");

    // The tray appears before any device is opened: the window can be closed
    // (hidden) from that moment on, and Windows startup assumes a quiet launch.
    trayIcon.onShowWindow = [this]() { if (onShowWindow != nullptr) onShowWindow(); };
    trayIcon.onToggleMute = [this]() { setMuted (! isMuted()); };
    trayIcon.onQuit = [this]() { if (onQuit != nullptr) onQuit(); };
    trayIcon.setMuted (isMuted());
    onMuteChanged = [this] (bool muted) { trayIcon.setMuted (muted); };
    StartupLog::mark ("ctor: tray icon");

    hotkey.onPressed = [this]() { setMuted (! isMuted()); };
    applySavedHotkey();
    StartupLog::mark ("ctor: mute hotkey");

    reconcileStartWithWindows();

    addAndMakeVisible (devicePanel);
    addAndMakeVisible (alertBanner);
    addAndMakeVisible (chainPanel);
    addAndMakeVisible (bottomBar);

    // The cards are the only place that edits the chain, and the bottom bar is
    // the only place that edits the preset name; these two links keep them in
    // step without either one knowing the other's layout.
    chainPanel.onEdited = [this]() { bottomBar.refresh(); };
    bottomBar.onPresetApplied = [this]() { chainPanel.refreshFromParams(); };

    devicePanel.onApiChanged = [this] (const juce::String& name) { audioApiChanged (name); };
    devicePanel.onInputChanged = [this] (const juce::String& name) { inputDeviceChanged (name); };
    devicePanel.onChannelChanged = [this] (InputChannel channel) { inputChannelChanged (channel); };
    devicePanel.onBufferSizeChanged = [this] (int samples) { bufferSizeChanged (samples); };
    devicePanel.onOutputChanged = [this] (const juce::String& id) { outputDeviceChanged (id); };
    devicePanel.onOptionsClicked = [this]() { showOptions(); };

    deviceHotplug.addListener (this);

    auto savedDeviceState = parseSavedDeviceState();

    if (savedDeviceState != nullptr)
        StartupLog::mark ("ctor: restoring saved device state");
    else
        StartupLog::mark ("ctor: first run, choosing a device");

    engine.initialise (savedDeviceState.get());
    StartupLog::mark ("ctor: engine.initialise()");

    engine.setParams (settings.params);
    engine.setInputChannel (settings.inputChannel);

    // The members were built before the settings file was read, so this is the
    // first point at which the controls can show the saved sound.
    chainPanel.refreshFromParams();
    bottomBar.refresh();

    StartupLog::mark ("ctor: settings applied to the engine");

    // From here on a plug or unplug feeds the recovery timing instead of
    // needing a restart. Notifications are optional: without them a lost
    // device is polled instead of retried on arrival.
    deviceRecovery.setNotificationsAvailable (deviceHotplug.start());
    StartupLog::mark ("ctor: hotplug.start()");

    // Start the cable at the period that stayed glitch-free last run. The
    // engine may raise it again if this machine still starves; the timer below
    // saves whatever it settles on.
    engine.setOutputPeriodHintFrames (settings.outputPeriodHintFrames);

    refreshDeviceLists();
    chooseOutputDevice (true);
    StartupLog::mark ("ctor: device lists and output");

    // The saved state was inspected before opening, so a device that is not
    // listed is "waiting for a replug", not an error. This runs after the
    // output choice so the input banner wins over a missing-cable banner on a
    // first run with neither device present.
    if (! engine.isDeviceOpen())
    {
        if (savedDeviceState != nullptr && ! engine.isDeviceListed (*savedDeviceState))
        {
            lostDeviceState = engine.inputOnlyState (*savedDeviceState);
            deviceRecovery.deviceLost (juce::Time::getMillisecondCounterHiRes());

            const auto name = inputNameFromState (*lostDeviceState);
            showBanner (BannerKind::inputLost,
                        Strings::bannerInputLost().replace ("%1", name),
                        AlertBanner::Severity::Critical);
            logDeviceEvent ("device missing at start: \"" + name + "\"");
        }
        else
        {
            const auto name = currentInputName();
            showBanner (BannerKind::openFailed,
                        Strings::bannerOpenFailed()
                            .replace ("%1", name)
                            .replace ("%2", deviceErrorText (engine.getLastDeviceError())),
                        AlertBanner::Severity::Critical);
            logDeviceEvent ("device failed to open at start: \"" + name + "\" ("
                            + engine.getLastDeviceError() + ")");
        }
    }

    syncDevicePanelFromEngine();
    devicePanel.setRunning (engine.isDeviceOpen() && engine.isOutputRunning());
    devicePanel.setLatencyMs (engine.getTotalLatencyMs());

    startTimerHz (30);
    StartupLog::mark ("ctor: done");
}

//==============================================================================
MainComponent::~MainComponent()
{
    stopTimer();

    devicePanel.onApiChanged = nullptr;
    devicePanel.onInputChanged = nullptr;
    devicePanel.onChannelChanged = nullptr;
    devicePanel.onBufferSizeChanged = nullptr;
    devicePanel.onOutputChanged = nullptr;
    devicePanel.onOptionsClicked = nullptr;

    // The hidden hotkey window is destroyed with the member, but silencing the
    // callback first makes sure a queued press cannot reach a half-torn-down
    // component.
    hotkey.onPressed = nullptr;

    deviceHotplug.removeListener (this);
    deviceHotplug.stop();

    // The engine is still running here: its chosen state is what the next
    // launch restores, and the output id is already in the settings.
    if (saveSettings())
        StartupLog::mark ("shutdown: settings saved");
    else
        StartupLog::mark ("shutdown: settings save failed");

    StartupLog::flush();

    engine.shutdown();
}

//==============================================================================
AudioEngine& MainComponent::getEngine() noexcept              { return engine; }
AppSettings& MainComponent::getSettings() noexcept            { return settings; }
ChainPanel& MainComponent::getChainPanel() noexcept           { return chainPanel; }
BottomBar& MainComponent::getBottomBar() noexcept             { return bottomBar; }
bool MainComponent::isMuted() const                           { return settings.params.mute; }

void MainComponent::settingsChanged()
{
    settingsDirty = true;
    settingsSaveDueMs = juce::Time::getMillisecondCounterHiRes() + settingsSaveDelayMs;
}

void MainComponent::setMuted (bool muted)
{
    if (settings.params.mute == muted)
        return;

    settings.params.mute = muted;
    engine.setParams (settings.params);
    settingsChanged();

    // The bottom bar's MUTE button is one of several callers (tray and hotkey
    // are the others), so it follows the state instead of owning it.
    bottomBar.refresh();

    if (onMuteChanged != nullptr)
        onMuteChanged (muted);
}

//==============================================================================
bool MainComponent::applyMuteHotkey (const juce::String& text)
{
    const auto combo = GlobalHotkey::parse (text);

    if (! combo.has_value())
        return false;

    // setHotkey leaves the old combination registered when Windows refuses the
    // new one (another application owns it), so a shortcut that used to work
    // keeps working.
    if (! hotkey.setHotkey (text))
        return false;

    hotkeyRegistered = true;

    // Store the canonical spelling, not what the user typed: the options field
    // can then show exactly the keys Windows holds.
    settings.muteHotkey = GlobalHotkey::format (*combo);
    settingsChanged();
    return true;
}

bool MainComponent::isMuteHotkeyRegistered() const noexcept
{
    return hotkeyRegistered;
}

void MainComponent::reconcileStartWithWindows()
{
    if (settings.startWithWindows)
    {
        // Write the entry again on every launch: a moved installation then
        // starts from its new path, and the saved toggle keeps its promise.
        if (! AutoStart::setEnabled (true))
        {
            // Windows refused (a policy, say). The toggle must show what will
            // really happen at the next sign-in.
            settings.startWithWindows = false;
            settingsChanged();
            StartupLog::mark ("autostart: settings on, but the Run entry could not be written");
        }

        return;
    }

    // No saved intent, but Windows has an entry (added by hand, or by an
    // earlier build): the registry is the state Windows follows, so it wins.
    if (AutoStart::isEnabled())
    {
        settings.startWithWindows = true;
        settingsChanged();
        StartupLog::mark ("autostart: Run entry exists, settings reconciled to on");
    }
}

void MainComponent::applySavedHotkey()
{
    if (settings.muteHotkey.isEmpty())
    {
        StartupLog::mark ("hotkey: none configured");
        return;
    }

    // The field keeps the saved text either way, so this flag is what the
    // options callout uses to avoid presenting a refused shortcut as active.
    hotkeyRegistered = hotkey.setHotkey (settings.muteHotkey);

    if (hotkeyRegistered)
    {
        StartupLog::mark ("hotkey: registered " + hotkey.getHotkey());
        return;
    }

    // parse() tells the two failures apart only for this log line: either way
    // there is no shortcut until the user chooses another one in the options.
    StartupLog::mark (GlobalHotkey::parse (settings.muteHotkey).has_value()
                          ? "hotkey: saved shortcut is owned by another application"
                          : "hotkey: saved shortcut is not valid");
    StartupLog::flush();
}

void MainComponent::showOptions()
{
    auto content = std::make_unique<OptionsPanel> (*this);
    content->setSize (OptionsPanel::preferredWidth, OptionsPanel::preferredHeight);

    // Screen coordinates: the callout is a desktop window, so it cannot be
    // clipped by the window edge and stays above the cards.
    const auto target = devicePanel.localAreaToGlobal (devicePanel.getOptionsButtonBounds());

    juce::CallOutBox::launchAsynchronously (std::move (content), target, nullptr);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::windowBg);

    if (! chainArea.isEmpty())
        Theme::fillCard (g, chainArea, Theme::panelBg);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    auto top = area.removeFromTop (DevicePanel::preferredHeight + Theme::gapSmall * 2);
    devicePanel.setBounds (top.reduced (Theme::pad, Theme::gapSmall));

    // The banner slides down from just below the device bar and overlays the
    // chain area; AlertBanner owns its height and animation.
    alertBanner.setSlideTargetY (devicePanel.getBottom() + Theme::gapSmall);

    chainArea = area.reduced (Theme::pad, Theme::gapSmall);

    auto chain = chainArea;
    bottomBar.setBounds (chain.removeFromBottom (BottomBar::preferredHeight));
    chain.removeFromBottom (Theme::gap);
    chainPanel.setBounds (chain);

    // The 1280 x 720 minimum is applied by MainWindow right after it adopts
    // this component. Doing it here was not dependable: resized() need not run
    // again after the window exists, so a window created at another size could
    // be dragged below the size the eight cards need.
}

//==============================================================================
void MainComponent::timerCallback()
{
    // Opening an ASIO driver can run the message loop, so this timer can tick
    // again from inside an open. Starting a second reopen there deleted the
    // device being opened.
    if (deviceBusy)
        return;

    // The audio thread publishes atomics; this timer is the only reader that
    // touches the components, so the meters never work on the render thread.
    const auto levels = engine.getLevels();
    chainPanel.updateMeters (levels);
    bottomBar.updateMeters (levels, engine.getRawInputPeakDb());

    const double now = juce::Time::getMillisecondCounterHiRes();

    devicePanel.setLatencyMs (engine.getTotalLatencyMs());
    devicePanel.setRunning (engine.isDeviceOpen() && engine.isOutputRunning());
    devicePanel.setChannelCount (activeInputChannels());

    // The render thread may have escalated to a larger period. Remember it
    // through the normal debounced save, so the next launch starts there
    // instead of learning the same lesson again.
    const int outputPeriodFrames = engine.getOutputPeriodFrames();

    if (outputPeriodFrames > 0 && outputPeriodFrames != settings.outputPeriodHintFrames)
    {
        settings.outputPeriodHintFrames = outputPeriodFrames;
        settingsChanged();
    }

    // A raw sample at or above -0.1 dBFS is already clipped in the interface;
    // nothing in software can undo that, so tell the user for five seconds.
    if (engine.takeInputClipFlag())
    {
        showBanner (BannerKind::inputClip, Strings::bannerInputClip(),
                    AlertBanner::Severity::Warning);
        clipBannerUntilMs = now + clipBannerDurationMs;
    }

    if (clipBannerUntilMs > 0.0 && now >= clipBannerUntilMs)
    {
        clipBannerUntilMs = 0.0;
        dismissBanner (BannerKind::inputClip);
    }

    if (settingsDirty && now >= settingsSaveDueMs)
        saveSettings();

    if (deviceListRefreshDueMs > 0.0 && now >= deviceListRefreshDueMs)
    {
        deviceListRefreshDueMs = 0.0;
        refreshDeviceLists();
    }

    // The cable's render thread publishes its own death; the endpoint
    // notification below only makes it happen sooner.
    if (outputConfigured && ! outputDown
        && (! engine.isOutputRunning() || engine.hasOutputError()))
        markOutputLost();

    // One list refresh and one output retry per burst of endpoint
    // notifications, once Windows has been quiet for the settle time.
    if (deviceRecovery.takeSettledArrival (now))
    {
        refreshDeviceLists();
        chooseOutputDevice (! outputConfigured);
    }

    // JUCE can open a replacement device without telling us; the engine flags
    // it so the recovery can put the user's microphone back.
    handleUnexpectedDevice();

    watchAudioDevice();
}

void MainComponent::handleUnexpectedDevice()
{
    if (! engine.takeUnexpectedDeviceFlag())
        return;

    // Capture the choice before closeLostDevice() forgets it; the manager's
    // current device is the replacement, not what the user picked.
    auto recorded = engine.getRecordedChoiceState();

    if (recorded == nullptr)
        recorded = parseSavedDeviceState();

    {
        // Closing and reopening are device operations like any other: keep the
        // timer's device watch from running in the middle of one.
        const juce::ScopedValueSetter<bool> busy (deviceBusy, true);
        engine.closeLostDevice();
    }

    lostDeviceState = std::move (recorded);

    if (lostDeviceState == nullptr)
    {
        deviceRecovery.watchNewDevice();
        showBanner (BannerKind::openFailed,
                    Strings::bannerOpenFailed()
                        .replace ("%1", currentInputName())
                        .replace ("%2", Strings::errorUnknown()),
                    AlertBanner::Severity::Critical);
        logDeviceEvent ("unexpected input device; no earlier choice to reopen");
        syncDevicePanelFromEngine();
        return;
    }

    // The device is away until the reopen succeeds: the same wait a replug
    // starts, including the settle time for endpoint notifications.
    deviceRecovery.deviceLost (juce::Time::getMillisecondCounterHiRes());

    const auto name = inputNameFromState (*lostDeviceState);

    logDeviceEvent ("unexpected input device; reopening \"" + name + "\"");
    showBanner (BannerKind::inputLost,
                Strings::bannerInputLost().replace ("%1", name),
                AlertBanner::Severity::Critical);

    syncDevicePanelFromEngine();
}

void MainComponent::watchAudioDevice()
{
    const double now = juce::Time::getMillisecondCounterHiRes();

    if (deviceRecovery.isLost())
    {
        if (deviceRecovery.isReopenDue (now))
            tryReopenLostDevice();

        return;
    }

    if (deviceRecovery.deviceStopped (engine.isDeviceOpen(), engine.getCallbackCount(), now))
    {
        // Stuck inside our own callback is a hung driver, not a lost device:
        // closing it would wait on that callback forever.
        if (! engine.isInsideCallback())
            audioDeviceLost (true);
    }
}

//==============================================================================
//  DevicePanel edits
//==============================================================================
void MainComponent::audioApiChanged (const juce::String& typeName)
{
    bool applied = false;
    {
        // Opening an ASIO input can run the message loop: keep the 30 Hz watch
        // from treating the transiently closed device as lost (or saving it)
        // while the user's choice is being applied.
        const juce::ScopedValueSetter<bool> busy (deviceBusy, true);
        devicePanel.setEnabled (false);
        applied = engine.setApi (typeName);
        devicePanel.setEnabled (true);
    }

    if (! applied)
    {
        // The banner says one short Vietnamese reason; the driver's own text
        // is only useful in the start-up log.
        logDeviceEvent ("api change failed: \"" + typeName + "\" ("
                        + engine.getLastDeviceError() + ")");
        showBanner (BannerKind::openFailed,
                    Strings::bannerOpenFailed()
                        .replace ("%1", currentInputName())
                        .replace ("%2", deviceErrorText (engine.getLastDeviceError())),
                    AlertBanner::Severity::Critical);
        syncDevicePanelFromEngine();
        return;
    }

    deviceRecovery.watchNewDevice();
    lostDeviceState.reset();

    // The new API reopens the device with a default channel; reapply the
    // user's choice so the engine and the panel cannot disagree.
    engine.setInputChannel (settings.inputChannel);

    refreshDeviceLists();
    syncDevicePanelFromEngine();
    dismissBanner (BannerKind::inputLost);
    dismissBanner (BannerKind::openFailed);
    settingsChanged();

    // The input is back but the cable may still be missing: that banner was
    // pushed aside by the input alert and has to come back.
    if (cableMissing)
        showBanner (BannerKind::noCable, Strings::bannerNoCable(), AlertBanner::Severity::Warning);
}

void MainComponent::inputDeviceChanged (const juce::String& name)
{
    bool applied = false;
    {
        // A driver can run the message loop while it opens. The busy flag and
        // the disabled panel mirror the reopen path so the timer and a second
        // edit cannot touch the device mid-open.
        const juce::ScopedValueSetter<bool> busy (deviceBusy, true);
        devicePanel.setEnabled (false);
        applied = engine.setInputDevice (name);
        devicePanel.setEnabled (true);
    }

    if (! applied)
    {
        logDeviceEvent ("input change failed: \"" + name + "\" ("
                        + engine.getLastDeviceError() + ")");
        showBanner (BannerKind::openFailed,
                    Strings::bannerOpenFailed()
                        .replace ("%1", name)
                        .replace ("%2", deviceErrorText (engine.getLastDeviceError())),
                    AlertBanner::Severity::Critical);
        syncDevicePanelFromEngine();
        return;
    }

    deviceRecovery.watchNewDevice();
    lostDeviceState.reset();

    // A mono device falls back to channel 1, so a later stereo device would
    // otherwise open on channel 1 even though the settings say channel 2.
    engine.setInputChannel (settings.inputChannel);

    refreshDeviceLists();
    syncDevicePanelFromEngine();
    dismissBanner (BannerKind::inputLost);
    dismissBanner (BannerKind::openFailed);
    settingsChanged();

    if (cableMissing)
        showBanner (BannerKind::noCable, Strings::bannerNoCable(), AlertBanner::Severity::Warning);
}

void MainComponent::inputChannelChanged (InputChannel channel)
{
    settings.inputChannel = channel;
    engine.setInputChannel (channel);
    settingsChanged();

    // The engine falls back to channel 1 on a mono microphone; show what is
    // really in use rather than the item the user clicked.
    syncDevicePanelFromEngine();
}

void MainComponent::bufferSizeChanged (int samples)
{
    bool applied = false;
    {
        // Changing the buffer restarts the stream and may run the message loop;
        // hold the timer off so it cannot take a half-applied device for lost.
        const juce::ScopedValueSetter<bool> busy (deviceBusy, true);
        devicePanel.setEnabled (false);
        applied = engine.setBufferSize (samples);
        devicePanel.setEnabled (true);
    }

    if (! applied)
    {
        logDeviceEvent ("buffer size change failed: " + juce::String (samples) + " ("
                        + engine.getLastDeviceError() + ")");
        showBanner (BannerKind::openFailed,
                    Strings::bannerOpenFailed()
                        .replace ("%1", currentInputName())
                        .replace ("%2", deviceErrorText (engine.getLastDeviceError())),
                    AlertBanner::Severity::Critical);
        syncDevicePanelFromEngine();
        return;
    }

    // The restart resets the channel selection just like an API change.
    engine.setInputChannel (settings.inputChannel);

    // The buffer size lives in the device state XML, so a dirty marker is all
    // that is needed to remember it.
    settingsChanged();
    syncDevicePanelFromEngine();
}

void MainComponent::outputDeviceChanged (const juce::String& id)
{
    const auto name = outputNameForId (WasapiOutputDevice::getAvailableDevices(), id);

    settings.outputDeviceId = id;
    settings.outputDeviceName = name;
    settingsChanged();

    startOutput (id, name);
    syncDevicePanelFromEngine();
}

//==============================================================================
//  Device state
//==============================================================================
std::unique_ptr<juce::XmlElement> MainComponent::parseSavedDeviceState() const
{
    if (settings.deviceStateXml.isEmpty())
        return nullptr;

    auto state = juce::XmlDocument::parse (settings.deviceStateXml);

    if (state == nullptr || ! state->hasTagName ("DEVICESETUP"))
        return nullptr;

    return state;
}

juce::String MainComponent::inputNameFromState (const juce::XmlElement& state)
{
    auto name = state.getStringAttribute ("audioInputDeviceName");

    // Older states used one combined name for input and output.
    if (name.isEmpty())
        name = state.getStringAttribute ("audioDeviceName");

    return name;
}

juce::String MainComponent::currentInputName()
{
    auto& deviceManager = engine.getDeviceManager();

    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getName();

    const auto setup = deviceManager.getAudioDeviceSetup();

    if (setup.inputDeviceName.isNotEmpty())
        return setup.inputDeviceName;

    if (lostDeviceState != nullptr)
        return inputNameFromState (*lostDeviceState);

    return {};
}

int MainComponent::activeInputChannels()
{
    auto& deviceManager = engine.getDeviceManager();

    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getActiveInputChannels().countNumberOfSetBits();

    return 0;
}

//==============================================================================
void MainComponent::refreshDeviceLists()
{
    if (deviceBusy)
        return;

    devicePanel.setApiTypes (engine.getApiNames());
    devicePanel.setInputDevices (engine.getInputDeviceNames());
    devicePanel.setBufferSizes (engine.getAvailableBufferSizes());

    auto outputs = WasapiOutputDevice::getAvailableDevices();

    // CABLE Input is the app's destination: put it first, keep the rest in the
    // order Windows reported them.
    std::stable_partition (outputs.begin(), outputs.end(),
                           [] (const WasapiOutputDevice::DeviceInfo& device)
                           {
                               return device.name.containsIgnoreCase ("CABLE Input");
                           });

    devicePanel.setOutputDevices (outputs);
    syncDevicePanelFromEngine();
}

void MainComponent::syncDevicePanelFromEngine()
{
    auto& deviceManager = engine.getDeviceManager();

    juce::String type, input;
    int buffer = 0;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        type   = device->getTypeName();
        input  = device->getName();
        buffer = device->getCurrentBufferSizeSamples();
    }
    else
    {
        if (lostDeviceState != nullptr)
        {
            type  = lostDeviceState->getStringAttribute ("deviceType");
            input = inputNameFromState (*lostDeviceState);
        }

        const auto setup = deviceManager.getAudioDeviceSetup();

        if (type.isEmpty())
            type = deviceManager.getCurrentAudioDeviceType();

        if (input.isEmpty())
            input = setup.inputDeviceName;

        buffer = setup.bufferSize;
    }

    devicePanel.showApi (type);
    devicePanel.showInputDevice (input);
    devicePanel.showChannel (engine.getInputChannel());
    devicePanel.showBufferSize (buffer);
    devicePanel.showOutputDevice (engine.getOutputDeviceId(), outputName);
    devicePanel.setChannelCount (activeInputChannels());
}

//==============================================================================
//  The virtual cable
//==============================================================================
void MainComponent::chooseOutputDevice (bool allowCableFallback)
{
    auto devices = WasapiOutputDevice::getAvailableDevices();

    juce::String chosenId, chosenName;

    // The saved id first: endpoint ids survive a restart on Windows.
    if (settings.outputDeviceId.isNotEmpty())
        for (const auto& device : devices)
            if (device.id == settings.outputDeviceId)
            {
                chosenId = device.id;
                chosenName = device.name;
                break;
            }

    // A reinstall (or a driver update) can change the id; the name survives.
    if (chosenId.isEmpty() && settings.outputDeviceName.isNotEmpty())
        for (const auto& device : devices)
            if (device.name == settings.outputDeviceName)
            {
                chosenId = device.id;
                chosenName = device.name;
                break;
            }

    if (chosenId.isEmpty() && allowCableFallback)
    {
        chosenId = AudioEngine::findCableInputId (devices);
        chosenName = outputNameForId (devices, chosenId);
    }

    if (chosenId.isEmpty())
    {
        // A chosen output that is away is left alone: the endpoint arrival
        // retries it, and replacing it with the cable would be a silent switch.
        if (! allowCableFallback)
            return;

        const bool newlyMissing = ! cableMissing;

        engine.setOutputDevice ({});
        outputConfigured = false;
        outputDown = false;
        cableMissing = true;

        if (newlyMissing)
            showBanner (BannerKind::noCable, Strings::bannerNoCable(),
                        AlertBanner::Severity::Warning);

        syncDevicePanelFromEngine();
        return;
    }

    cableMissing = false;

    // Already streaming to exactly this endpoint: nothing to restart.
    if (engine.getOutputDeviceId() == chosenId
        && engine.isOutputRunning() && ! engine.hasOutputError())
    {
        outputConfigured = true;
        outputDown = false;

        if (settings.outputDeviceId != chosenId || settings.outputDeviceName != chosenName)
        {
            settings.outputDeviceId = chosenId;
            settings.outputDeviceName = chosenName;
            settingsChanged();
        }

        dismissBanner (BannerKind::outputLost);
        dismissBanner (BannerKind::noCable);
        syncDevicePanelFromEngine();
        return;
    }

    const bool changed = settings.outputDeviceId != chosenId
                         || settings.outputDeviceName != chosenName;

    settings.outputDeviceId = chosenId;
    settings.outputDeviceName = chosenName;

    startOutput (chosenId, chosenName);

    if (changed)
        settingsChanged();

    syncDevicePanelFromEngine();
}

bool MainComponent::startOutput (const juce::String& id, const juce::String& name)
{
    if (id.isEmpty())
    {
        engine.setOutputDevice ({});
        outputConfigured = false;
        outputDown = false;
        return false;
    }

    bool started = false;
    {
        // The start waits for the render thread (up to 2 s), so keep the timer
        // and the device watch from re-entering while it runs.
        const juce::ScopedValueSetter<bool> busy (deviceBusy, true);

        // A restart after an unplug should reuse what this machine learned,
        // not start from the endpoint minimum again.
        engine.setOutputPeriodHintFrames (settings.outputPeriodHintFrames);
        started = engine.setOutputDevice (id);
    }

    outputName = name.isNotEmpty() ? name : id;

    if (! started)
    {
        // The endpoint is still the desired one: an arrival retries it.
        outputConfigured = true;
        outputDown = true;

        showBanner (BannerKind::outputLost,
                    Strings::bannerOutputLost().replace ("%1", outputName),
                    AlertBanner::Severity::Critical);

        logDeviceEvent ("output failed: \"" + outputName + "\"");
        return false;
    }

    outputConfigured = true;
    outputDown = false;
    cableMissing = false;

    StartupLog::mark ("output: started \"" + outputName + "\"");
    dismissBanner (BannerKind::outputLost);
    dismissBanner (BannerKind::noCable);
    return true;
}

void MainComponent::markOutputLost()
{
    if (! outputConfigured || outputDown)
        return;

    outputDown = true;

    if (outputName.isEmpty())
        outputName = engine.getOutputDeviceId();

    showBanner (BannerKind::outputLost,
                Strings::bannerOutputLost().replace ("%1", outputName),
                AlertBanner::Severity::Critical);

    logDeviceEvent ("output lost: \"" + outputName + "\"");
}

//==============================================================================
//  Lost input device and hotplug notifications
//==============================================================================
void MainComponent::audioDeviceLost (bool closeIt)
{
    lostDeviceState = engine.getChosenDeviceState();

    if (lostDeviceState == nullptr)
        lostDeviceState = parseSavedDeviceState();

    // A device that closed itself may have taken its saved state with it; the
    // last setup is enough to name it and to try it again.
    if (lostDeviceState == nullptr)
    {
        auto& deviceManager = engine.getDeviceManager();
        const auto setup = deviceManager.getAudioDeviceSetup();

        if (setup.inputDeviceName.isNotEmpty())
            lostDeviceState = AudioEngine::makeDeviceState (deviceManager.getCurrentAudioDeviceType(),
                                                            setup.inputDeviceName,
                                                            setup.bufferSize);
    }

    if (lostDeviceState == nullptr)
    {
        deviceRecovery.watchNewDevice();
        showBanner (BannerKind::openFailed,
                    Strings::bannerOpenFailed()
                        .replace ("%1", currentInputName())
                        .replace ("%2", deviceErrorText (engine.getLastDeviceError())),
                    AlertBanner::Severity::Critical);
        logDeviceEvent ("device lost without a state to reopen ("
                        + engine.getLastDeviceError() + ")");
        return;
    }

    deviceRecovery.deviceLost (juce::Time::getMillisecondCounterHiRes());

    // Release the driver now, as quitting the app before replugging did: it
    // then starts clean when the hardware is back.
    if (closeIt)
    {
        const juce::ScopedValueSetter<bool> busy (deviceBusy, true);
        engine.closeLostDevice();
    }

    const auto name = inputNameFromState (*lostDeviceState);

    logDeviceEvent ("device lost: \"" + name + "\"" + (closeIt ? " (closed)" : ""));
    showBanner (BannerKind::inputLost,
                Strings::bannerInputLost().replace ("%1", name),
                AlertBanner::Severity::Critical);

    syncDevicePanelFromEngine();
}

void MainComponent::tryReopenLostDevice()
{
    if (lostDeviceState == nullptr)
    {
        deviceRecovery.reopenFailed (juce::Time::getMillisecondCounterHiRes());
        return;
    }

    const double now = juce::Time::getMillisecondCounterHiRes();

    // An ASIO driver is not in the Windows notifications, but its interface's
    // endpoints are: this keeps a Windows Audio stand-in out of the way until
    // the chosen device is really back.
    if (! engine.isDeviceListed (*lostDeviceState))
    {
        logDeviceEvent ("reopen skipped: the device is not listed");
        deviceRecovery.reopenFailed (now);
        return;
    }

    logDeviceEvent ("reopen: trying");
    deviceRecovery.reopenStarted();

    bool reopened = false;
    {
        // The driver may run the message loop while it starts: keep the device
        // watch and the device controls out of the way until it is done.
        const juce::ScopedValueSetter<bool> busy (deviceBusy, true);
        devicePanel.setEnabled (false);
        reopened = engine.reopenDevice (*lostDeviceState);
        devicePanel.setEnabled (true);
    }

    if (! reopened)
    {
        // Listed but refused: the driver is busy or broken, not unplugged.
        // Timed from the end of the try, which can take a few seconds.
        deviceRecovery.reopenFailed (juce::Time::getMillisecondCounterHiRes());

        const auto name = inputNameFromState (*lostDeviceState);
        showBanner (BannerKind::openFailed,
                    Strings::bannerOpenFailed()
                        .replace ("%1", name)
                        .replace ("%2", deviceErrorText (engine.getLastDeviceError())),
                    AlertBanner::Severity::Critical);
        logDeviceEvent ("reopen failed: \"" + name + "\" (" + engine.getLastDeviceError() + ")");
        return;
    }

    deviceRecovery.watchNewDevice();
    lostDeviceState.reset();

    // A mono microphone may have replaced a stereo one while it was away.
    engine.setInputChannel (settings.inputChannel);

    refreshDeviceLists();
    syncDevicePanelFromEngine();
    dismissBanner (BannerKind::inputLost);
    dismissBanner (BannerKind::openFailed);
    logDeviceEvent ("device reconnected");
    settingsChanged();

    if (cableMissing)
        showBanner (BannerKind::noCable, Strings::bannerNoCable(), AlertBanner::Severity::Warning);
    else
        chooseOutputDevice (! outputConfigured);
}

void MainComponent::deviceAdded (const juce::String&)
{
    endpointArrived();
}

void MainComponent::deviceRemoved (const juce::String& deviceId)
{
    if (deviceId.isNotEmpty() && deviceId == engine.getOutputDeviceId())
        markOutputLost();

    endpointDeparted();
}

void MainComponent::deviceStateChanged (const juce::String& deviceId, DWORD newState)
{
    // A USB interface coming or going shows up here, not as added/removed.
    if (newState == DEVICE_STATE_ACTIVE)
    {
        endpointArrived();
    }
    else
    {
        if (deviceId.isNotEmpty() && deviceId == engine.getOutputDeviceId())
            markOutputLost();

        endpointDeparted();
    }
}

void MainComponent::defaultDeviceChanged (EDataFlow, ERole, const juce::String&)
{
    scheduleDeviceListRefresh();
}

void MainComponent::endpointArrived()
{
    logDeviceEvent ("endpoint arrived");
    deviceRecovery.endpointArrived (juce::Time::getMillisecondCounterHiRes());
    scheduleDeviceListRefresh();
}

void MainComponent::endpointDeparted()
{
    logDeviceEvent ("endpoint departed");
    deviceRecovery.endpointDeparted (juce::Time::getMillisecondCounterHiRes());
    scheduleDeviceListRefresh();
}

void MainComponent::scheduleDeviceListRefresh()
{
    // Each notification of a burst pushes it back: one rescan after the burst.
    deviceListRefreshDueMs = juce::Time::getMillisecondCounterHiRes() + deviceListRefreshDelayMs;
}

//==============================================================================
//  Settings and banners
//==============================================================================
bool MainComponent::saveSettings()
{
    if (auto state = engine.getChosenDeviceState())
        settings.deviceStateXml = state->toString();

    if (SettingsIO::save (settings, SettingsIO::getDefaultFile()))
    {
        settingsDirty = false;
        settingsSaveDueMs = 0.0;
        return true;
    }

    // A permission, disk-full or antivirus-lock failure must not lose the
    // user's device state and params. Keep the change pending and retry on a
    // slow clock instead of writing (and failing) at 30 Hz; the dirty flag is
    // also what lets the shutdown path report the failure truthfully.
    settingsSaveDueMs = juce::Time::getMillisecondCounterHiRes() + settingsRetryDelayMs;
    logDeviceEvent ("settings save failed; retry pending");
    return false;
}

void MainComponent::showBanner (BannerKind kind, const juce::String& message,
                                AlertBanner::Severity severity)
{
    // A clipping microphone is worth a warning, but it must not hide a lost
    // device or a driver failure for five seconds.
    if (kind == BannerKind::inputClip
        && currentBanner != BannerKind::none
        && currentBanner != BannerKind::inputClip
        && alertBanner.isShowing())
        return;

    currentBanner = kind;
    alertBanner.show (message, severity);
}

void MainComponent::dismissBanner (BannerKind kind)
{
    if (currentBanner != kind)
        return;

    currentBanner = BannerKind::none;
    alertBanner.dismiss();
}

void MainComponent::logDeviceEvent (const juce::String& what)
{
    // Written at once: if the driver takes the app down next, the events
    // leading up to it are on disk (%APPDATA%\OneCast\startup.log).
    StartupLog::mark (what);
    StartupLog::flush();
}
