#include "AudioEngine.h"
#include "../Diagnostics/StartupLog.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    constexpr float clipThresholdDb = -0.1f;
    constexpr double testToneHz = 440.0;
    constexpr double testToneDb = -12.0;
    constexpr double rawPeakReleaseSeconds = 0.15;

    struct ChosenDevice
    {
        juce::String type, inputName, outputName;
    };

    /// Same reading as AudioDeviceManager::initialiseFromXML(): the combined
    /// attribute is the older form, the separate ones are current.
    ChosenDevice readDeviceChoice (const juce::XmlElement& state)
    {
        ChosenDevice choice;
        choice.type = state.getStringAttribute ("deviceType");

        if (const auto both = state.getStringAttribute ("audioDeviceName"); both.isNotEmpty())
        {
            choice.inputName = choice.outputName = both;
        }
        else
        {
            choice.inputName  = state.getStringAttribute ("audioInputDeviceName");
            choice.outputName = state.getStringAttribute ("audioOutputDeviceName");
        }

        return choice;
    }

}

// ==================================================================
//  Construction / destruction
// ==================================================================

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    shutdown();
}

// ==================================================================
//  Pure helpers
// ==================================================================

juce::StringArray AudioEngine::filterInputNames (const juce::StringArray& names)
{
    juce::StringArray filtered;

    for (const auto& name : names)
        if (! name.containsIgnoreCase ("CABLE Output"))
            filtered.add (name);

    return filtered;
}

juce::String AudioEngine::pickFirstRunAsioDevice (const juce::StringArray& asioNames)
{
    for (const auto& name : asioNames)
        if (name.containsIgnoreCase ("M-Track"))
            return name;

    return {};
}

juce::String AudioEngine::pickWasapiInput (const juce::StringArray& names, int defaultIndex)
{
    const auto isCable = [] (const juce::String& name)
    {
        return name.containsIgnoreCase ("CABLE Output");
    };

    if (juce::isPositiveAndBelow (defaultIndex, names.size())
        && ! isCable (names[defaultIndex]))
        return names[defaultIndex];

    for (const auto& name : names)
        if (! isCable (name))
            return name;

    return {};
}

juce::String AudioEngine::findCableInputId (
    const std::vector<WasapiOutputDevice::DeviceInfo>& devices)
{
    for (const auto& device : devices)
        if (device.name.containsIgnoreCase ("CABLE Input"))
            return device.id;

    return {};
}

std::unique_ptr<juce::XmlElement> AudioEngine::makeDeviceState (const juce::String& typeName,
                                                                const juce::String& inputName,
                                                                int bufferSize)
{
    auto state = std::make_unique<juce::XmlElement> ("DEVICESETUP");
    state->setAttribute ("deviceType", typeName);
    state->setAttribute ("audioInputDeviceName", inputName);
    state->setAttribute ("audioOutputDeviceName", "");
    // JUCE reads the presence of audioDeviceOutChans as "use exactly these
    // channels" and its absence as "use the default channel counts". For an
    // empty output name the defaults could resolve to the Windows output, so
    // write the attribute explicitly empty: zero output channels.
    state->setAttribute ("audioDeviceOutChans", "");
    state->setAttribute ("audioDeviceRate", (double) preferredSampleRate);

    if (bufferSize > 0)
        state->setAttribute ("audioDeviceBufferSize", bufferSize);

    return state;
}

std::unique_ptr<juce::XmlElement> AudioEngine::inputOnlyState (const juce::XmlElement& saved)
{
    auto state = std::make_unique<juce::XmlElement> (saved);

    if (const auto combined = state->getStringAttribute ("audioDeviceName"); combined.isNotEmpty())
    {
        if (state->getStringAttribute ("audioInputDeviceName").isEmpty())
            state->setAttribute ("audioInputDeviceName", combined);

        state->removeAttribute ("audioDeviceName");
    }

    // A saved state can come from a build that had monitor output, and a
    // virtual cable output is never a microphone. Both have to go before JUCE
    // sees the state, otherwise it would open an endpoint the user did not
    // choose.
    if (state->getStringAttribute ("audioInputDeviceName").containsIgnoreCase ("CABLE Output"))
        state->setAttribute ("audioInputDeviceName", "");

    state->setAttribute ("audioOutputDeviceName", "");
    state->setAttribute ("audioDeviceOutChans", "");
    return state;
}

// ==================================================================
//  Lifecycle
// ==================================================================

void AudioEngine::initialise (const juce::XmlElement* savedDeviceState)
{
    if (savedDeviceState != nullptr)
    {
        auto state = inputOnlyState (*savedDeviceState);
        const auto input = state->getStringAttribute ("audioInputDeviceName");

        // A blank input name must never reach AudioDeviceManager::initialise():
        // JUCE reads it as "the default device", which would silently open a
        // microphone the user did not choose. A saved cable input is blanked
        // by inputOnlyState(), so refuse here instead.
        if (input.isEmpty())
            lastDeviceError_ = "no input device chosen";
        else
            lastDeviceError_ = deviceManager.initialise (2, 0, state.get(), false);
    }
    else
    {
        openFirstRunChoice();
    }

    if (auto* device = deviceManager.getCurrentAudioDevice())
        StartupLog::mark ("init: opened type=\"" + device->getTypeName()
                          + "\" device=\"" + device->getName() + "\"");
    else
        StartupLog::mark ("init: no device opened (" + lastDeviceError_ + ")");

    if (! engineStarted_)
    {
        // addAudioCallback() calls our audioDeviceAboutToStart() for an
        // already-open device, so the chain is prepared even though the
        // device started before this line.
        deviceManager.addAudioCallback (this);
        engineStarted_ = true;
    }
}

void AudioEngine::shutdown()
{
    // Stop the cable first: the callback must not write into a ring nobody
    // drains after the device is gone.
    output_.stop();

    deviceManager.removeAudioCallback (this);
    engineStarted_ = false;
    prepared_.store (false, std::memory_order_release);
}

juce::AudioDeviceManager& AudioEngine::getDeviceManager()
{
    return deviceManager;
}

// ==================================================================
//  Device type and input selection
// ==================================================================

juce::AudioIODeviceType* AudioEngine::findDeviceType (const juce::String& name)
{
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        if (type != nullptr && type->getTypeName() == name)
            return type;

    return nullptr;
}

juce::String AudioEngine::openChoice (const juce::String& typeName,
                                      const juce::String& inputName, int bufferSize)
{
    // Every path that can end up without a usable microphone (all inputs are
    // CABLE Outputs, a saved cable input, an empty API list) arrives here with
    // a blank name. JUCE would treat that as "the default device", so refuse
    // before initialise() is called.
    if (inputName.isEmpty())
    {
        lastDeviceError_ = "no input device chosen";
        return lastDeviceError_;
    }

    auto state = makeDeviceState (typeName, inputName, bufferSize);
    return deviceManager.initialise (2, 0, state.get(), false);
}

void AudioEngine::openFirstRunChoice()
{
    // The M-Track's ASIO driver is the best path when it is installed: no
    // Windows resampling and an accurate input latency. It is only picked
    // when the driver is actually present; otherwise the app falls back to
    // Windows Audio, whose default input may be the cable and is avoided.
    if (auto* asio = findDeviceType ("ASIO"))
    {
        asio->scanForDevices();
        const auto driver = pickFirstRunAsioDevice (asio->getDeviceNames (true));

        if (driver.isNotEmpty())
        {
            lastDeviceError_ = openChoice ("ASIO", driver, firstRunAsioBufferSize);
            return;
        }
    }

    auto* wasapi = findDeviceType ("Windows Audio");

    if (wasapi == nullptr)
        wasapi = deviceManager.getAvailableDeviceTypes().getFirst();

    if (wasapi == nullptr)
    {
        lastDeviceError_ = "no audio API available";
        return;
    }

    wasapi->scanForDevices();
    const auto input = pickWasapiInput (wasapi->getDeviceNames (true),
                                        wasapi->getDefaultDeviceIndex (true));
    lastDeviceError_ = openChoice (wasapi->getTypeName(), input, 0);
}

juce::StringArray AudioEngine::getApiNames() const
{
    juce::StringArray names;

    for (auto* type : deviceManager.getAvailableDeviceTypes())
        if (type != nullptr)
            names.add (type->getTypeName());

    return names;
}

bool AudioEngine::setApi (const juce::String& typeName)
{
    auto* type = findDeviceType (typeName);

    if (type == nullptr)
    {
        lastDeviceError_ = "unknown audio API: " + typeName;
        return false;
    }

    type->scanForDevices();
    const auto input = typeName == "ASIO"
                         ? pickFirstRunAsioDevice (type->getDeviceNames (true))
                         : pickWasapiInput (type->getDeviceNames (true),
                                            type->getDefaultDeviceIndex (true));

    if (input.isEmpty())
    {
        lastDeviceError_ = "no input device available on " + typeName;
        return false;
    }

    lastDeviceError_ = openChoice (typeName, input,
                                   typeName == "ASIO" ? firstRunAsioBufferSize : 0);

    return lastDeviceError_.isEmpty() && isDeviceOpen();
}

juce::StringArray AudioEngine::getInputDeviceNames() const
{
    juce::StringArray names;

    if (auto* type = deviceManager.getCurrentDeviceTypeObject())
        names = type->getDeviceNames (true);

    return filterInputNames (names);
}

bool AudioEngine::setInputDevice (const juce::String& name)
{
    if (name.isEmpty())
    {
        lastDeviceError_ = "no input device chosen";
        return false;
    }

    if (name.containsIgnoreCase ("CABLE Output"))
    {
        lastDeviceError_ = "a virtual cable output is not a microphone";
        return false;
    }

    const auto previousSetup = deviceManager.getAudioDeviceSetup();
    auto setup = previousSetup;
    setup.inputDeviceName  = name;
    setup.outputDeviceName = {};
    setup.sampleRate       = (double) preferredSampleRate;
    setup.useDefaultInputChannels  = true;
    // Input-only means exactly zero output channels, not "whatever JUCE's
    // default output layout is": the latter would depend on the empty output
    // name never being resolved to a device.
    setup.outputChannels.clear();
    setup.useDefaultOutputChannels = false;

    lastDeviceError_ = deviceManager.setAudioDeviceSetup (setup, true);

    // JUCE's ASIO wrapper ignores a refused rate and carries on at the old
    // one. Some drivers only accept the new rate from a freshly loaded driver,
    // so close the device completely and try once more.
    if (lastDeviceError_.isEmpty() && getDeviceSampleRate() != preferredSampleRate)
    {
        deviceManager.closeAudioDevice();
        lastDeviceError_ = deviceManager.setAudioDeviceSetup (setup, true);
    }

    if (lastDeviceError_.isEmpty() && getDeviceSampleRate() != preferredSampleRate)
        lastDeviceError_ = "the device did not open at 48 kHz";

    if (lastDeviceError_.isNotEmpty() || ! isDeviceOpen())
    {
        if (lastDeviceError_.isEmpty())
            lastDeviceError_ = "the input device could not be opened";

        // Do not leave the user without audio because one device refused.
        // Restoring the previous choice is not a silent fallback: the caller
        // gets false and the error above.
        const auto error = lastDeviceError_;
        deviceManager.setAudioDeviceSetup (previousSetup, true);
        lastDeviceError_ = error;
        return false;
    }

    return true;
}

juce::Array<int> AudioEngine::getAvailableBufferSizes() const
{
    juce::Array<int> sizes;

    if (auto* device = deviceManager.getCurrentAudioDevice())
        sizes = device->getAvailableBufferSizes();

    return sizes;
}

bool AudioEngine::setBufferSize (int samples)
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        lastDeviceError_ = "no input device open";
        return false;
    }

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.bufferSize = samples;
    lastDeviceError_ = deviceManager.setAudioDeviceSetup (setup, true);

    if (lastDeviceError_.isNotEmpty())
        return false;

    if (auto* opened = deviceManager.getCurrentAudioDevice())
    {
        if (opened->getCurrentBufferSizeSamples() != samples)
        {
            lastDeviceError_ = "the device did not accept buffer size "
                               + juce::String (samples);
            return false;
        }

        return true;
    }

    lastDeviceError_ = "no input device open";
    return false;
}

void AudioEngine::setInputChannel (InputChannel channel) noexcept
{
    int value = (int) channel;

    if (value < 0 || value > 2)
        value = 0;

    // A mono microphone has no second channel; the only useful choice is the
    // first one, and that is what the UI has to show as selected.
    if (value == (int) InputChannel::second
        && inputChannelCount_.load (std::memory_order_relaxed) < 2)
        value = (int) InputChannel::first;

    inputChannel_.store (value, std::memory_order_relaxed);
}

InputChannel AudioEngine::getInputChannel() const noexcept
{
    return (InputChannel) inputChannel_.load (std::memory_order_relaxed);
}

juce::String AudioEngine::getLastDeviceError() const
{
    return lastDeviceError_;
}

int AudioEngine::getDeviceSampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return juce::roundToInt (device->getCurrentSampleRate());

    return 0;
}

// ==================================================================
//  Output: the virtual cable
// ==================================================================

bool AudioEngine::setOutputDevice (const juce::String& endpointId)
{
    output_.stop();
    outputDeviceId_ = endpointId;

    if (endpointId.isEmpty())
        return false;

    const int rate = juce::roundToInt (sampleRate_.load (std::memory_order_relaxed));

    return output_.start (endpointId, outputRing_,
                          rate > 0 ? rate : preferredSampleRate);
}

juce::String AudioEngine::getOutputDeviceId() const
{
    return outputDeviceId_;
}

bool AudioEngine::isOutputRunning() const
{
    return output_.isRunning();
}

bool AudioEngine::hasOutputError() const
{
    return output_.hasError();
}

// ==================================================================
//  Processing
// ==================================================================

void AudioEngine::setParams (const VoiceParams& params) noexcept
{
    params_.store (params);
}

VoiceParams AudioEngine::getParams() const noexcept
{
    return params_.load();
}

void AudioEngine::setTestSignal (bool on) noexcept
{
    testSignal_.store (on, std::memory_order_relaxed);
}

VoiceChain::Levels AudioEngine::getLevels() const noexcept
{
    return chain_.getLevels();
}

float AudioEngine::getRawInputPeakDb() const noexcept
{
    return rawPeakDb_.load (std::memory_order_relaxed);
}

bool AudioEngine::takeInputClipFlag() noexcept
{
    return inputClip_.exchange (false, std::memory_order_relaxed);
}

float AudioEngine::getTotalLatencyMs() const noexcept
{
    const double sr = sampleRate_.load (std::memory_order_relaxed);
    const double rate = sr > 1.0 ? sr : (double) preferredSampleRate;

    // The backlog is in producer frames, which are engine frames.
    const double inputMs   = 1000.0 * inputLatencySamples_.load (std::memory_order_relaxed) / rate;
    const double chainMs   = 1000.0 * chain_.getLatencySamples() / rate;
    const double backlogMs = 1000.0 * output_.getBacklogTargetFrames() / rate;
    const double periodMs  = output_.getDevicePeriodMs();

    return (float) (inputMs + chainMs + backlogMs + periodMs);
}

int AudioEngine::getOutputUnderrunCount() const noexcept
{
    return output_.getUnderrunCount();
}

RingBuffer& AudioEngine::getOutputRing() noexcept
{
    return outputRing_;
}

// ==================================================================
//  Device loss and recovery (message thread)
// ==================================================================

std::uint32_t AudioEngine::getCallbackCount() const noexcept
{
    return callbackCount_.load (std::memory_order_relaxed);
}

bool AudioEngine::isInsideCallback() const noexcept
{
    return insideCallback_.load (std::memory_order_relaxed);
}

bool AudioEngine::isDeviceOpen() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr && device->isOpen();
}

std::unique_ptr<juce::XmlElement> AudioEngine::getChosenDeviceState() const
{
    if (auto state = deviceManager.createStateXml())
        return state;

    // Never chosen explicitly (the first-run choice was open): the device in
    // use, in the same form AudioDeviceManager saves a choice.
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return {};

    const auto setup = deviceManager.getAudioDeviceSetup();

    auto state = std::make_unique<juce::XmlElement> ("DEVICESETUP");
    state->setAttribute ("deviceType",            deviceManager.getCurrentAudioDeviceType());
    state->setAttribute ("audioOutputDeviceName", setup.outputDeviceName);
    state->setAttribute ("audioInputDeviceName",  setup.inputDeviceName);
    // Input-only: an explicit empty channel layout, never the default outputs.
    state->setAttribute ("audioDeviceOutChans",   "");
    state->setAttribute ("audioDeviceRate",       device->getCurrentSampleRate());
    state->setAttribute ("audioDeviceBufferSize", device->getCurrentBufferSizeSamples());
    return state;
}

bool AudioEngine::isDeviceListed (const juce::XmlElement& state)
{
    const auto chosen = readDeviceChoice (state);

    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if (type == nullptr || type->getTypeName() != chosen.type)
            continue;

        type->scanForDevices();

        return (chosen.inputName.isEmpty()  || type->getDeviceNames (true) .contains (chosen.inputName))
            && (chosen.outputName.isEmpty() || type->getDeviceNames (false).contains (chosen.outputName));
    }

    return false;
}

void AudioEngine::closeLostDevice()
{
    // Stops the device (our callback gets audioDeviceStopped()) and deletes
    // it; for ASIO that unloads the driver.
    deviceManager.closeAudioDevice();
}

bool AudioEngine::reopenDevice (const juce::XmlElement& state)
{
    // A new device object, not a restart of the old one: a fresh ASIO driver
    // instance is what a replugged interface needs. setAudioDeviceSetup()
    // would do nothing for an unchanged setup while the old object exists.
    deviceManager.closeAudioDevice();

    auto clean = inputOnlyState (state);

    // Same guard as initialise(): a blank name would ask JUCE for the default
    // microphone, so fail the reopen instead of opening something else.
    if (clean->getStringAttribute ("audioInputDeviceName").isEmpty())
        lastDeviceError_ = "no input device chosen";
    else
        lastDeviceError_ = deviceManager.initialise (2, 0, clean.get(), false);

    if (auto* device = deviceManager.getCurrentAudioDevice())
        StartupLog::mark ("reopen: type=\"" + device->getTypeName() + "\" device=\""
                          + device->getName() + "\" " + juce::String (getDeviceSampleRate())
                          + " Hz, " + juce::String (device->getCurrentBufferSizeSamples())
                          + " samples");
    else
        StartupLog::mark ("reopen: failed (" + lastDeviceError_ + ")");

    return lastDeviceError_.isEmpty() && isDeviceOpen();
}

// ==================================================================
//  juce::AudioIODeviceCallback
// ==================================================================

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device == nullptr)
    {
        prepared_.store (false, std::memory_order_release);
        return;
    }

    const auto sr = device->getCurrentSampleRate();
    const auto bs = device->getCurrentBufferSizeSamples();

    // JUCE's ASIO driver reset calls this even when reopening the driver
    // failed: right after an unplug, with a sample rate of 0. Preparing a
    // filter chain at 0 Hz divides by zero. Such a device never calls back,
    // so stay unprepared; the reconnect prepares it properly.
    if (sr <= 0.0 || bs <= 0 || ! device->isOpen())
    {
        prepared_.store (false, std::memory_order_release);
        return;
    }

    sampleRate_.store (sr, std::memory_order_relaxed);
    bufferSize_.store (bs, std::memory_order_relaxed);
    inputLatencySamples_.store (juce::jmax (0, device->getInputLatencyInSamples()),
                                std::memory_order_relaxed);

    const int activeInputs = juce::jmax (1, device->getActiveInputChannels()
                                                .countNumberOfSetBits());
    inputChannelCount_.store (activeInputs, std::memory_order_relaxed);

    if (activeInputs < 2
        && inputChannel_.load (std::memory_order_relaxed) == (int) InputChannel::second)
        inputChannel_.store ((int) InputChannel::first, std::memory_order_relaxed);

    mono_.assign ((size_t) bs, 0.0f);
    chain_.prepare (sr, bs);

    rawPeakRelease_ = (float) std::exp (-1.0 / (rawPeakReleaseSeconds * sr));
    rawPeak_ = 0.0f;
    testTonePhase_ = 0.0;

    // The cable render thread sizes its buffering from our block size, and
    // resamples if its stream was opened at another rate.
    output_.setProducerFormat (sr, bs);

    prepared_.store (true, std::memory_order_release);
}

void AudioEngine::audioDeviceStopped()
{
    prepared_.store (false, std::memory_order_release);

    // Nothing updates the meter until the next start: do not leave it showing
    // a signal from a device that is gone.
    rawPeak_ = 0.0f;
    rawPeakDb_.store (-200.0f, std::memory_order_relaxed);
}

void AudioEngine::clearOutputs (float* const* outputChannelData, int numOutputChannels,
                                int numSamples) noexcept
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData != nullptr && outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
}

void AudioEngine::audioDeviceIOCallbackWithContext (
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // Proof of life for the device watchdog (lock-free, no allocation).
    callbackCount_.fetch_add (1, std::memory_order_relaxed);
    insideCallback_.store (true, std::memory_order_relaxed);
    const juce::ScopeGuard leaving { [this] { insideCallback_.store (false, std::memory_order_relaxed); } };

    // The scratch buffer is sized in audioDeviceAboutToStart(). A device that
    // hands us more samples than that, or one that never prepared us, gets
    // silence rather than a write past the allocation.
    if (! prepared_.load (std::memory_order_acquire) || numSamples <= 0
        || numSamples > (int) mono_.size())
    {
        clearOutputs (outputChannelData, numOutputChannels, numSamples);
        return;
    }

    // ------------------------------------------------------------------
    //  1. Pick the microphone channel (or the average of both)
    // ------------------------------------------------------------------
    const int requested = inputChannel_.load (std::memory_order_relaxed);
    const bool mix = requested == (int) InputChannel::mix;

    if (mix)
    {
        const float* a = (numInputChannels > 0 && inputChannelData != nullptr)
                           ? inputChannelData[0] : nullptr;
        const float* b = (numInputChannels > 1 && inputChannelData != nullptr)
                           ? inputChannelData[1] : nullptr;

        if (a != nullptr && b != nullptr)
            for (int i = 0; i < numSamples; ++i)
                mono_[(size_t) i] = 0.5f * (a[i] + b[i]);
        else if (a != nullptr)
            std::copy (a, a + numSamples, mono_.begin());
        else if (b != nullptr)
            std::copy (b, b + numSamples, mono_.begin());
        else
            std::fill (mono_.begin(), mono_.begin() + numSamples, 0.0f);
    }
    else
    {
        // "Second" on a 1-channel device falls back to the first channel, so
        // the user still gets the microphone they have.
        const int channel = (requested == (int) InputChannel::second && numInputChannels > 1)
                              ? 1 : 0;
        const float* source = (inputChannelData != nullptr && channel < numInputChannels)
                                ? inputChannelData[channel] : nullptr;

        if (source != nullptr)
            std::copy (source, source + numSamples, mono_.begin());
        else
            std::fill (mono_.begin(), mono_.begin() + numSamples, 0.0f);
    }

    // ------------------------------------------------------------------
    //  2. Raw input metering, before the chain changes anything
    // ------------------------------------------------------------------
    const float threshold = juce::Decibels::decibelsToGain (clipThresholdDb);
    float peak = 0.0f;
    bool clipped = false;

    for (int i = 0; i < numSamples; ++i)
    {
        const float absolute = std::abs (mono_[(size_t) i]);
        peak = juce::jmax (peak, absolute);

        if (absolute >= threshold)
            clipped = true;
    }

    // Fast attack, slow release, so a transient between two UI refreshes is
    // still visible on the meter.
    rawPeak_ = juce::jmax (peak, rawPeak_ * rawPeakRelease_);
    rawPeakDb_.store (juce::Decibels::gainToDecibels (rawPeak_, -200.0f),
                      std::memory_order_relaxed);

    if (clipped)
        inputClip_.store (true, std::memory_order_relaxed);

    // ------------------------------------------------------------------
    //  3. The test signal replaces the microphone, after the raw meter
    // ------------------------------------------------------------------
    if (testSignal_.load (std::memory_order_relaxed))
    {
        const double rate = sampleRate_.load (std::memory_order_relaxed);
        const double step = 2.0 * juce::MathConstants<double>::pi * testToneHz
                              / (rate > 0.0 ? rate : (double) preferredSampleRate);
        const float amplitude = juce::Decibels::decibelsToGain ((float) testToneDb);

        for (int i = 0; i < numSamples; ++i)
        {
            mono_[(size_t) i] = amplitude * (float) std::sin (testTonePhase_);
            testTonePhase_ += step;

            if (testTonePhase_ >= 2.0 * juce::MathConstants<double>::pi)
                testTonePhase_ -= 2.0 * juce::MathConstants<double>::pi;
        }
    }

    // ------------------------------------------------------------------
    //  4. Voice chain, then the cable ring (mono duplicated to stereo)
    // ------------------------------------------------------------------
    chain_.process (mono_.data(), numSamples, params_.load());

    const float* cable[2] = { mono_.data(), mono_.data() };
    outputRing_.write (cable, numSamples, 2);

    // ------------------------------------------------------------------
    //  5. This engine is input-only: any output the device hands us is silence
    // ------------------------------------------------------------------
    clearOutputs (outputChannelData, numOutputChannels, numSamples);
}
