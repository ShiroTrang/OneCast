#pragma once
#include <JuceHeader.h>

#include "RingBuffer.h"
#include "WasapiOutputDevice.h"
#include "../Dsp/DspParams.h"
#include "../Dsp/VoiceChain.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

/// The single input-only audio engine: one microphone through the voice chain
/// and into the virtual cable.
///
/// The JUCE device manager only ever opens inputs. The processed voice leaves
/// through a RingBuffer that a WasapiOutputDevice drains into the chosen
/// "CABLE Input" endpoint; nothing is rendered to the Windows default device,
/// and there is no monitor path.
///
/// The device handling is ported from LiveVocal: an input device that is
/// unplugged can be closed and reopened, a running device that goes quiet is
/// detected through the callback count, and the ASIO guard for JUCE's
/// rate-0 "reset" start keeps the chain unprepared instead of preparing it at
/// 0 Hz. The callback itself does no allocation, locking, I/O or logging.
class AudioEngine : public juce::AudioIODeviceCallback
{
public:
    static constexpr int preferredSampleRate = 48000, firstRunAsioBufferSize = 128;

    AudioEngine();
    ~AudioEngine() override;

    // ------------------------------------------------------------------
    //  Pure helpers (unit-tested)
    // ------------------------------------------------------------------

    /// Drops every input whose name contains "CABLE Output": a virtual cable
    /// output is a playback endpoint, never a microphone.
    static juce::StringArray filterInputNames (const juce::StringArray& names);

    /// The ASIO driver an explicit "ASIO" choice opens: the M-Track when it is
    /// installed, otherwise the first listed driver (there is no system default
    /// ASIO device, and the user asked for ASIO). "" when none is listed.
    static juce::String pickAsioDevice (const juce::StringArray& asioNames);

    /// The first ASIO driver that looks like the M-Track, or "".
    static juce::String pickFirstRunAsioDevice (const juce::StringArray& asioNames);

    /// The default input unless it is a cable output; otherwise the first
    /// input that is not one, or "" when every input is a cable.
    static juce::String pickWasapiInput (const juce::StringArray& names, int defaultIndex);

    /// The id of the first render endpoint whose name contains "CABLE Input",
    /// or "" when VB-Audio Virtual Cable is not installed.
    static juce::String findCableInputId (const std::vector<WasapiOutputDevice::DeviceInfo>& devices);

    /// The DEVICESETUP used to open an explicitly chosen input. The output
    /// layout is written as an explicitly empty audioDeviceOutChans: a missing
    /// attribute would let JUCE fall back to the default output channels.
    static std::unique_ptr<juce::XmlElement> makeDeviceState (const juce::String& typeName,
                                                              const juce::String& inputName,
                                                              int bufferSize);

    /// A saved state reduced to input-only. The old combined name becomes the
    /// input, a virtual cable output is dropped, and the output endpoint plus
    /// its channel layout are removed.
    static std::unique_ptr<juce::XmlElement> inputOnlyState (const juce::XmlElement& saved);

    // ------------------------------------------------------------------
    //  Lifecycle (message thread)
    // ------------------------------------------------------------------

    /// Open an input-only device and register the callback. With a saved state
    /// the user's last device comes back; without one the first-run choice is
    /// used (M-Track ASIO when present, otherwise a non-cable WASAPI input).
    void initialise (const juce::XmlElement* savedDeviceState);

    void shutdown();
    juce::AudioDeviceManager& getDeviceManager();

    // ------------------------------------------------------------------
    //  Input
    // ------------------------------------------------------------------

    juce::StringArray getApiNames() const;
    bool setApi (const juce::String& typeName);
    juce::StringArray getInputDeviceNames() const;
    bool setInputDevice (const juce::String& name);
    juce::Array<int> getAvailableBufferSizes() const;
    bool setBufferSize (int samples);
    void setInputChannel (InputChannel channel) noexcept;
    InputChannel getInputChannel() const noexcept;
    juce::String getLastDeviceError() const;

    // ------------------------------------------------------------------
    //  Output (the virtual cable)
    // ------------------------------------------------------------------

    /// Start rendering to the given WASAPI endpoint. An empty id stops the
    /// output and returns false: the Windows default endpoint is never used.
    bool setOutputDevice (const juce::String& endpointId);
    juce::String getOutputDeviceId() const;
    bool isOutputRunning() const;
    bool hasOutputError() const;

    /// The IAudioClient3 period that stayed glitch-free on the previous run,
    /// in frames; 0 starts at the endpoint minimum. Set before the next
    /// setOutputDevice() call. Thread-safe.
    void setOutputPeriodHintFrames (int frames) noexcept;

    /// The period the cable stream is using now, in frames; 0 when no
    /// low-latency stream is open. Stored in the settings so the next launch
    /// starts where this machine settled.
    int getOutputPeriodFrames() const noexcept;

    // ------------------------------------------------------------------
    //  Processing
    // ------------------------------------------------------------------

    void setParams (const VoiceParams& params) noexcept;
    VoiceParams getParams() const noexcept;

    /// Replaces the microphone with a 440 Hz sine at -12 dBFS (soak tests and
    /// the "is the cable alive" check).
    void setTestSignal (bool on) noexcept;

    // ------------------------------------------------------------------
    //  Meters and status (safe from any thread)
    // ------------------------------------------------------------------

    VoiceChain::Levels getLevels() const noexcept;
    float getRawInputPeakDb() const noexcept;

    /// True once per raw input sample at or above -0.1 dBFS, then clears.
    bool takeInputClipFlag() noexcept;

    /// True when the device JUCE started is not the input the user chose, or is
    /// a virtual cable output. The engine leaves the chain unprepared for such
    /// a device; the message thread closes it and reopens the chosen mic.
    bool takeUnexpectedDeviceFlag() noexcept;

    /// The input the user last chose explicitly, as a state the recovery code
    /// can reopen, or nullptr when nothing was chosen. It is read before
    /// closeLostDevice(), which forgets the choice.
    std::unique_ptr<juce::XmlElement> getRecordedChoiceState() const;

    /// Driver input latency + chain latency + the output latency actually
    /// queued. While no output runs it falls back to the backlog target plus
    /// the device period, which is the best estimate available then.
    float getTotalLatencyMs() const noexcept;

    /// Message-thread summary for the start-up log, e.g.
    /// total=21.4 ms in=12.0 chain=6.3 out=3.1 mode=exclusive period=3.0 ms.
    juce::String describeLatency() const;

    int getOutputUnderrunCount() const noexcept;

    /// What the render thread drains; tests read it directly.
    RingBuffer& getOutputRing() noexcept;

    // ------------------------------------------------------------------
    //  Device loss and recovery (message thread)
    // ------------------------------------------------------------------

    std::uint32_t getCallbackCount() const noexcept;
    bool isInsideCallback() const noexcept;
    bool isDeviceOpen() const;
    std::unique_ptr<juce::XmlElement> getChosenDeviceState() const;
    bool isDeviceListed (const juce::XmlElement& state);
    void closeLostDevice();
    bool reopenDevice (const juce::XmlElement& state);

    // ------------------------------------------------------------------
    //  juce::AudioIODeviceCallback
    // ------------------------------------------------------------------

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    /// What the user last asked for, message thread only. JUCE's own
    /// device-list handling can replace a vanished input with the Windows
    /// default, so the callback compares what started against this.
    struct DeviceChoice
    {
        juce::String typeName, inputName;
        int bufferSize = 0;
    };

    void rememberChoice (const juce::String& typeName, const juce::String& inputName,
                         int bufferSize) noexcept;
    bool isUnexpectedDevice (const juce::String& typeName,
                             const juce::String& inputName) const noexcept;

    juce::AudioIODeviceType* findDeviceType (const juce::String& name);
    juce::String openChoice (const juce::String& typeName, const juce::String& inputName,
                             int bufferSize);
    void openFirstRunChoice();
    int getDeviceSampleRate() const;
    void clearOutputs (float* const* outputChannelData, int numOutputChannels,
                       int numSamples) noexcept;

    /// Lazily scans for devices inside getApiNames() const, so it is logically
    /// const even though JUCE's accessor is not.
    mutable juce::AudioDeviceManager deviceManager;

    VoiceChain chain_;
    RingBuffer outputRing_ { 2, 65536 };
    WasapiOutputDevice output_;

    AtomicVoiceParams params_;
    std::atomic<int> inputChannel_ { (int) InputChannel::first };
    // Active input channels of the running device: "second" falls back to
    // "first" when there is only one, and getInputChannel() says so.
    std::atomic<int> inputChannelCount_ { 2 };
    std::atomic<bool> testSignal_ { false };

    // The voice chain and its scratch buffer are prepared for the running
    // device; the callback plays (and writes) silence until they are.
    std::atomic<bool> prepared_ { false };
    std::atomic<std::uint32_t> callbackCount_ { 0 };
    std::atomic<bool> insideCallback_ { false };
    std::atomic<int> inputLatencySamples_ { 0 };
    std::atomic<double> sampleRate_ { 48000.0 };
    std::atomic<int> bufferSize_ { 0 };
    std::atomic<int> outputPeriodHintFrames_ { 0 };

    std::atomic<float> rawPeakDb_ { -200.0f };
    std::atomic<bool> inputClip_ { false };
    std::atomic<bool> unexpectedDevice_ { false };

    // Message thread only.
    DeviceChoice chosen_;

    // Audio thread only.
    std::vector<float> mono_;
    float rawPeak_ = 0.0f;
    float rawPeakRelease_ = 0.0f;
    double testTonePhase_ = 0.0;

    juce::String outputDeviceId_;
    juce::String lastDeviceError_;
    bool engineStarted_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
