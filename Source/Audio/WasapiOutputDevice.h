#pragma once
#include <JuceHeader.h>
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <atomic>
#include <thread>
#include <vector>

class RingBuffer;

class WasapiOutputDevice
{
public:
    /// How the stream was opened. The ladder is tried once per start(), in
    /// this order, and the first rung the endpoint accepts wins.
    enum class OutputMode
    {
        exclusive,        ///< exclusive mode, event-driven, minimum device period
        sharedLowLatency, ///< IAudioClient3 shared stream at the minimum engine period
        shared            ///< shared mode at the default period (legacy behaviour)
    };

    WasapiOutputDevice();
    ~WasapiOutputDevice();

    /** Start rendering audio from the given RingBuffer to the specified WASAPI
        render endpoint. An empty deviceId fails without opening anything: this
        app must never render to the Windows default endpoint (the user's
        headphones), only to the virtual cable the user chose.
        @param deviceId      WASAPI endpoint ID string; empty is refused
        @param inputBuffer   RingBuffer to read from (must outlive this session)
        @param producerRate  Sample rate the producer writes at (e.g. JUCE device rate).
                             If non-zero and different from the endpoint's native rate,
                             WASAPI AUTOCONVERTPCM handles resampling. Pass 0 to use
                             the endpoint's native mix format rate (legacy behaviour).
        @returns true on success, false on failure (check getErrorMessage()). */
    bool start (const juce::String& deviceId, RingBuffer& inputBuffer,
                int producerRate = 0);
    void stop();

    /** Tell the render thread the rate the producer currently writes at. Call
        whenever the engine's device rate or block size changes: the render
        thread resamples if the rate no longer matches the stream it opened,
        and sizes its (minimal) buffering from the block size. Thread-safe. */
    void setProducerFormat (double rate, int blockSize);

    bool isRunning() const;
    bool hasError() const;
    juce::String getErrorMessage() const;

    int getSampleRate() const;
    int getNumChannels() const;

    /** Backlog the render thread tries to hold in the ring, in producer frames.
        Written by the render thread; safe to read from any thread. */
    int getBacklogTargetFrames() const noexcept;

    /** WASAPI's device period for the open stream, in milliseconds: the period
        actually in use, not the default. Written by the render thread; safe to
        read from any thread. */
    double getDevicePeriodMs() const noexcept;

    /** The rung of the mode ladder that opened the stream. */
    OutputMode getOutputMode() const noexcept;

    /** Output latency of the open stream, in milliseconds: the smoothed ring
        fill at the producer rate plus the smoothed endpoint padding at the
        device rate (or the stream latency when padding is unavailable). This
        is what is actually queued, not a backlog target. Safe from any thread. */
    double getMeasuredLatencyMs() const noexcept;

    /** Underruns seen by the drift-compensated reader since the stream opened. */
    int getUnderrunCount() const noexcept;

    struct DeviceInfo
    {
        juce::String id;
        juce::String name;
    };

    /** Enumerate all active WASAPI render (output) devices. */
    static std::vector<DeviceInfo> getAvailableDevices();

private:
    /// Every failure path publishes one of these instead of a juce::String:
    /// the render thread must not allocate, and the message is only built from
    /// the caller's thread in getErrorMessage().
    enum class ErrorCode
    {
        none = 0,
        noDeviceChosen,
        comInitialization,
        deviceEnumerator,
        renderDevice,
        activateAudioClient,
        mixFormat,
        initializeAudioClient,
        setEventHandle,
        renderClient,
        bufferSize,
        startAudioClient,
        startTimedOut,
        deviceDisconnected,
        waitFailed
    };

    void setError (ErrorCode code, int detail = 0) noexcept;
    void renderThread (juce::String deviceId, RingBuffer* buffer, int producerRate);

    std::thread thread;
    HANDLE stopEvent   = nullptr;
    HANDLE renderEvent = nullptr;

    // Signalled by the render thread once the endpoint is open and running, or
    // once it has given up, so start() can report a vanished endpoint id
    // synchronously instead of leaving it to the background thread.
    HANDLE startEvent  = nullptr;

    std::atomic<bool> running     { false };
    std::atomic<int>  sampleRate  { 0 };
    std::atomic<int>  numChannels { 0 };
    std::atomic<double> producerSampleRate { 0.0 };
    std::atomic<int>    producerBlockSize { 0 };
    std::atomic<int>    backlogTargetFrames { 0 };
    std::atomic<double> devicePeriodMs { 0.0 };
    std::atomic<int>    outputMode { (int) OutputMode::shared };
    std::atomic<int>    underrunCount { 0 };

    // Honest output latency, published by the render thread. The ring fill is
    // in producer frames and the endpoint padding in device frames, both
    // one-pole smoothed over about 0.5 s so a single event does not make the
    // UI jump. streamLatencyMs is the fallback when GetCurrentPadding fails.
    std::atomic<double> measuredRingFillFrames { 0.0 };
    std::atomic<double> measuredPaddingFrames  { 0.0 };
    std::atomic<double> streamLatencyMs        { 0.0 };
    std::atomic<bool>   paddingValid           { false };

    // Published by the render thread with release ordering; errorDetail is the
    // HRESULT or GetLastError value associated with the code. The reader loads
    // the code with acquire ordering before using the detail.
    std::atomic<int>    errorCode { (int) ErrorCode::none };
    std::atomic<int>    errorDetail { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WasapiOutputDevice)
};
