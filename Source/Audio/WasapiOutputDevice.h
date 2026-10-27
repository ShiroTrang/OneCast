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
    WasapiOutputDevice();
    ~WasapiOutputDevice();

    /** Start rendering audio from the given RingBuffer to the specified WASAPI
        render endpoint. Pass an empty deviceId to use the system default output.
        @param deviceId      WASAPI endpoint ID string (empty = default device)
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

    struct DeviceInfo
    {
        juce::String id;
        juce::String name;
    };

    /** Enumerate all active WASAPI render (output) devices. */
    static std::vector<DeviceInfo> getAvailableDevices();

private:
    void renderThread (juce::String deviceId, RingBuffer* buffer, int producerRate);

    std::thread thread;
    HANDLE stopEvent   = nullptr;
    HANDLE renderEvent = nullptr;

    std::atomic<bool> running     { false };
    std::atomic<bool> error       { false };
    std::atomic<int>  sampleRate  { 0 };
    std::atomic<int>  numChannels { 0 };
    std::atomic<double> producerSampleRate { 0.0 };
    std::atomic<int>    producerBlockSize { 0 };
    juce::String errorMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WasapiOutputDevice)
};
