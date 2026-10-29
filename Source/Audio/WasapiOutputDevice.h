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

    /// A period hint with this value means "this machine needs the standard
    /// shared rung": start() skips the IAudioClient3 low-latency rung and does
    /// not waste a re-open on a period it has already failed to sustain.
    static constexpr int standardSharedPeriodHint = -1;

    WasapiOutputDevice();
    ~WasapiOutputDevice();

    /** Start rendering audio from the given RingBuffer to the specified WASAPI
        render endpoint. An empty deviceId fails without opening anything: this
        app must never render to the Windows default endpoint (the user's
        headphones), only to the virtual cable the user chose.
        @param deviceId      WASAPI endpoint ID string; empty is refused
        @param inputBuffer   RingBuffer to read from (must outlive this session)
        @param producerRate  Sample rate the producer writes at (e.g. JUCE device rate).
                             The low-latency shared rung keeps the endpoint's mix
                             rate and the drift-compensated reader converts; the
                             legacy shared rung keeps AUTOCONVERTPCM. Pass 0 to use
                             the endpoint's native mix format rate.
        @param periodHintFrames  IAudioClient3 period this machine sustained last
                             time, in frames; 0 starts at the endpoint minimum.
                             standardSharedPeriodHint (-1) skips the low-latency
                             rung, for a machine that had to fall back before.
                             The engine escalates from a real period if the
                             endpoint starves.
        @returns true on success, false on failure (check getErrorMessage()). */
    bool start (const juce::String& deviceId, RingBuffer& inputBuffer,
                int producerRate = 0, int periodHintFrames = 0);
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

    /** Resyncs (large backlog drops) done by the drift-compensated reader. */
    int getResyncCount() const noexcept;

    /** Longest interval between consecutive render events since the stream
        opened, in milliseconds, measured with QueryPerformanceCounter. */
    double getMaxRenderGapMs() const noexcept;

    /** Render events whose interval exceeded twice the device period. */
    int getLateEventCount() const noexcept;

    /** Failed GetBuffer or ReleaseBuffer calls on the render client. */
    int getBufferErrorCount() const noexcept;

    /** Render events at which GetCurrentPadding found the endpoint empty: the
        endpoint had played everything and ran dry since the previous event.
        This is the real glitch signal, unlike a late event that still filled
        the buffer. */
    int getStarvedEventCount() const noexcept;

    /** The period hint to persist for the next run, in frames: the
        IAudioClient3 period currently in use (it changes when the render
        thread escalates), standardSharedPeriodHint (-1) when the stream is on
        the standard shared rung, 0 when no stream is open or it is exclusive. */
    int getOutputPeriodFrames() const noexcept;

    /** Times the render thread left its loop to raise the period (or to fall
        back to the standard shared rung) because the endpoint starved. */
    int getEscalationCount() const noexcept;

    /** Whether the render thread got an MMCSS ("Pro Audio" or "Audio")
        registration. Published for describeLatency(); false while no stream is
        open. */
    bool isMmcssActive() const noexcept;

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
        streamStalled,
        waitFailed
    };

    void setError (ErrorCode code, int detail = 0) noexcept;
    void renderThread (juce::String deviceId, RingBuffer* buffer, int producerRate,
                       int periodHintFrames);

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
    std::atomic<int>    resyncCount { 0 };

    // Render-deadline diagnostics, published by the render thread. A late
    // event or a buffer error is counted, never logged: the loop may not touch
    // the file system or build a string.
    std::atomic<double> maxRenderGapMs { 0.0 };
    std::atomic<int>    lateEventCount { 0 };
    std::atomic<int>    bufferErrorCount { 0 };

    // The endpoint buffer was empty when a render event arrived: the endpoint
    // played everything it had. A thread that woke up too late produces this,
    // so it drives the adaptive period.
    std::atomic<int>    starvedEventCount { 0 };
    std::atomic<int>    outputPeriodFrames { 0 };
    std::atomic<int>    escalationCount { 0 };

    // MMCSS registration is published as an atomic so describeLatency() can
    // report it without reading the render thread's local handle.
    std::atomic<bool>   mmcssActive { false };

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
