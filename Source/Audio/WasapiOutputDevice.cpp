#include "WasapiOutputDevice.h"
#include "RingBuffer.h"
#include "DriftCompensatedReader.h"
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if ((p) != nullptr) { (p)->Release(); (p) = nullptr; } } while (0)
#endif

//==============================================================================
WasapiOutputDevice::WasapiOutputDevice()
{
    stopEvent   = CreateEvent (nullptr, TRUE, FALSE, nullptr);   // manual-reset
    renderEvent = CreateEvent (nullptr, FALSE, FALSE, nullptr);  // auto-reset
    startEvent  = CreateEvent (nullptr, TRUE, FALSE, nullptr);   // manual-reset
}

WasapiOutputDevice::~WasapiOutputDevice()
{
    stop();

    if (stopEvent != nullptr)
        CloseHandle (stopEvent);
    if (renderEvent != nullptr)
        CloseHandle (renderEvent);
    if (startEvent != nullptr)
        CloseHandle (startEvent);
}

//==============================================================================
bool WasapiOutputDevice::start (const juce::String& deviceId, RingBuffer& inputBuffer,
                                int producerRate)
{
    // Before anything else, and certainly before COM touches an endpoint: an
    // empty id used to mean "the Windows default", which would send the
    // processed microphone to the user's headphones. OneCast only ever renders
    // to the chosen virtual cable.
    if (deviceId.isEmpty())
    {
        setError (ErrorCode::noDeviceChosen);
        return false;
    }

    if (running.load (std::memory_order_acquire))
        return false;

    // Join any dead thread from a previous failed start
    if (thread.joinable())
        thread.join();

    setError (ErrorCode::none);
    backlogTargetFrames.store (0, std::memory_order_relaxed);
    devicePeriodMs.store (0.0, std::memory_order_relaxed);
    underrunCount.store (0, std::memory_order_relaxed);
    ResetEvent (stopEvent);
    ResetEvent (startEvent);

    if (producerRate > 0)
        producerSampleRate.store (producerRate, std::memory_order_relaxed);

    running.store (true, std::memory_order_release);

    thread = std::thread ([this, deviceId, &inputBuffer, producerRate]()
    {
        renderThread (deviceId, &inputBuffer, producerRate);
    });

    // Wait until the render thread has opened the endpoint or given up. A
    // vanished endpoint id must fail here, where the caller can report it,
    // rather than in a background thread the message thread never hears from.
    const DWORD waitResult = WaitForSingleObject (startEvent, 2000);

    if (waitResult != WAIT_OBJECT_0)
    {
        // Nothing signalled success or failure: do not claim a stream that may
        // not exist. Stop and join the thread, then report the timeout.
        setError (ErrorCode::startTimedOut, (int) waitResult);
        SetEvent (stopEvent);

        if (thread.joinable())
            thread.join();

        running.store (false, std::memory_order_release);
        return false;
    }

    if (hasError())
    {
        if (thread.joinable())
            thread.join();

        running.store (false, std::memory_order_release);
        return false;
    }

    return true;
}

void WasapiOutputDevice::stop()
{
    SetEvent (stopEvent);

    if (thread.joinable())
        thread.join();

    running.store (false, std::memory_order_release);

    // No stream, no output latency to report.
    backlogTargetFrames.store (0, std::memory_order_relaxed);
    devicePeriodMs.store (0.0, std::memory_order_relaxed);
}

void WasapiOutputDevice::setProducerFormat (double rate, int blockSize)
{
    producerSampleRate.store (rate, std::memory_order_relaxed);
    producerBlockSize.store (blockSize, std::memory_order_relaxed);
}

bool WasapiOutputDevice::isRunning() const
{
    return running.load (std::memory_order_relaxed);
}

bool WasapiOutputDevice::hasError() const
{
    return errorCode.load (std::memory_order_acquire) != (int) ErrorCode::none;
}

void WasapiOutputDevice::setError (ErrorCode code, int detail) noexcept
{
    // Detail first, then the code with release ordering: a reader that sees
    // the code with acquire ordering also sees the matching detail.
    errorDetail.store (detail, std::memory_order_relaxed);
    errorCode.store ((int) code, std::memory_order_release);
}

juce::String WasapiOutputDevice::getErrorMessage() const
{
    // Built here, on the caller's thread, from an atomic code: the render
    // thread never allocates and no juce::String is shared between threads.
    const auto code = (ErrorCode) errorCode.load (std::memory_order_acquire);
    const int detail = errorDetail.load (std::memory_order_relaxed);

    const auto hresult = [detail] (const char* prefix)
    {
        return juce::String (prefix) + "0x" + juce::String::toHexString (detail);
    };

    switch (code)
    {
        case ErrorCode::none:                   return {};
        case ErrorCode::noDeviceChosen:         return "no output device chosen";
        case ErrorCode::comInitialization:      return hresult ("Failed to initialize COM: ");
        case ErrorCode::deviceEnumerator:       return hresult ("Failed to create device enumerator: ");
        case ErrorCode::renderDevice:           return hresult ("Failed to get render device: ");
        case ErrorCode::activateAudioClient:    return hresult ("Failed to activate audio client: ");
        case ErrorCode::mixFormat:              return hresult ("Failed to get mix format: ");
        case ErrorCode::initializeAudioClient:  return hresult ("Failed to initialize audio client: ");
        case ErrorCode::setEventHandle:         return hresult ("Failed to set event handle: ");
        case ErrorCode::renderClient:           return hresult ("Failed to get render client: ");
        case ErrorCode::bufferSize:             return hresult ("Failed to get buffer size: ");
        case ErrorCode::startAudioClient:       return hresult ("Failed to start audio client: ");
        case ErrorCode::deviceDisconnected:     return "Audio device was disconnected";
        case ErrorCode::startTimedOut:          return "the render thread did not start ("
                                                       + juce::String (detail) + ")";
        case ErrorCode::waitFailed:             return "WaitForMultipleObjects failed: "
                                                       + juce::String (detail);
    }

    return {};
}

int WasapiOutputDevice::getSampleRate() const
{
    return sampleRate.load (std::memory_order_relaxed);
}

int WasapiOutputDevice::getNumChannels() const
{
    return numChannels.load (std::memory_order_relaxed);
}

int WasapiOutputDevice::getBacklogTargetFrames() const noexcept
{
    return backlogTargetFrames.load (std::memory_order_relaxed);
}

double WasapiOutputDevice::getDevicePeriodMs() const noexcept
{
    return devicePeriodMs.load (std::memory_order_relaxed);
}

int WasapiOutputDevice::getUnderrunCount() const noexcept
{
    return underrunCount.load (std::memory_order_relaxed);
}

//==============================================================================
std::vector<WasapiOutputDevice::DeviceInfo> WasapiOutputDevice::getAvailableDevices()
{
    std::vector<DeviceInfo> devices;

    HRESULT hr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED (hr);
    if (hr == RPC_E_CHANGED_MODE) { /* COM already initialized in a compatible mode */ }
    if (FAILED (hr) && hr != RPC_E_CHANGED_MODE)
        return devices;

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  __uuidof (IMMDeviceEnumerator),
                                  reinterpret_cast<void**> (&pEnum));
    if (FAILED (hr) || pEnum == nullptr)
    {
        if (comOwned)
            CoUninitialize();
        return devices;
    }

    IMMDeviceCollection* collection = nullptr;
    hr = pEnum->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &collection);

    if (SUCCEEDED (hr) && collection != nullptr)
    {
        UINT count = 0;
        collection->GetCount (&count);

        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED (collection->Item (i, &dev)) && dev != nullptr)
            {
                LPWSTR id = nullptr;
                dev->GetId (&id);

                IPropertyStore* props = nullptr;
                dev->OpenPropertyStore (STGM_READ, &props);

                juce::String name;
                if (props != nullptr)
                {
                    PROPVARIANT varName;
                    PropVariantInit (&varName);
                    if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &varName)))
                        name = juce::String (varName.pwszVal);
                    PropVariantClear (&varName);
                    props->Release();
                }

                devices.push_back ({ juce::String (id), name });

                if (id != nullptr)
                    CoTaskMemFree (id);
                dev->Release();
            }
        }

        collection->Release();
    }

    pEnum->Release();

    if (comOwned)
        CoUninitialize();

    return devices;
}

//==============================================================================
void WasapiOutputDevice::renderThread (juce::String deviceId, RingBuffer* buffer, int producerRate)
{
    // ---- COM initialization ----
    HRESULT hr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    if (FAILED (hr))
    {
        setError (ErrorCode::comInitialization, (int) hr);
        running.store (false, std::memory_order_release);
        SetEvent (startEvent);
        return;
    }

    // Local COM pointers -- all released in the cleanup block below.
    IMMDeviceEnumerator* enumerator   = nullptr;
    IMMDevice*           device       = nullptr;
    IAudioClient*        audioClient  = nullptr;
    IAudioRenderClient*  renderClient = nullptr;
    WAVEFORMATEX*        mixFormat    = nullptr;

    // Lambda to clean up all COM resources.
    auto cleanup = [&]()
    {
        if (mixFormat != nullptr)
        {
            CoTaskMemFree (mixFormat);
            mixFormat = nullptr;
        }
        SAFE_RELEASE (renderClient);
        SAFE_RELEASE (audioClient);
        SAFE_RELEASE (device);
        SAFE_RELEASE (enumerator);
        CoUninitialize();

        // Every failure path ends here, so start() waiting on this event sees
        // it and can return the real result (the error code is already set).
        SetEvent (startEvent);
    };

    // Defence in depth: start() refuses an empty id before the thread exists,
    // but this function is also the last line of defence. GetDefaultAudioEndpoint
    // would send the processed microphone to the user's headphones.
    if (deviceId.isEmpty())
    {
        setError (ErrorCode::noDeviceChosen);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // ---- Get device enumerator ----
    hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr,
                          CLSCTX_INPROC_SERVER,
                          __uuidof (IMMDeviceEnumerator),
                          reinterpret_cast<void**> (&enumerator));
    if (FAILED (hr) || enumerator == nullptr)
    {
        setError (ErrorCode::deviceEnumerator, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // ---- Get render device ----
    // Only the endpoint the caller named. There is deliberately no fallback to
    // the Windows default endpoint.
    hr = enumerator->GetDevice (deviceId.toWideCharPointer(), &device);

    if (FAILED (hr) || device == nullptr)
    {
        setError (ErrorCode::renderDevice, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // ---- Activate audio client ----
    hr = device->Activate (__uuidof (IAudioClient), CLSCTX_ALL,
                           nullptr, reinterpret_cast<void**> (&audioClient));
    if (FAILED (hr) || audioClient == nullptr)
    {
        setError (ErrorCode::activateAudioClient, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // ---- Get mix format ----
    hr = audioClient->GetMixFormat (&mixFormat);
    if (FAILED (hr) || mixFormat == nullptr)
    {
        setError (ErrorCode::mixFormat, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // Resolve the actual sample format. WASAPI shared mode almost always
    // returns WAVE_FORMAT_EXTENSIBLE wrapping IEEE_FLOAT, but we validate.
    WORD bitsPerSample = mixFormat->wBitsPerSample;
    bool isFloat = false;

    if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*> (mixFormat);
        if (IsEqualGUID (ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            isFloat = true;
    }
    else if (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        isFloat = true;
    }

    if (! isFloat || bitsPerSample != 32)
    {
        // Force IEEE float 32-bit -- this is what WASAPI shared mode
        // supports on virtually every Windows 10+ system.
        if (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*> (mixFormat);
            ext->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            ext->Samples.wValidBitsPerSample = 32;
        }
        else
        {
            mixFormat->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        }

        mixFormat->wBitsPerSample = 32;
        mixFormat->nBlockAlign = static_cast<WORD> (mixFormat->nChannels * (32 / 8));
        mixFormat->nAvgBytesPerSec = mixFormat->nSamplesPerSec * mixFormat->nBlockAlign;
    }

    const int nativeSampleRate  = static_cast<int> (mixFormat->nSamplesPerSec);
    const int deviceChannels   = static_cast<int> (mixFormat->nChannels);

    // If the producer writes at a different rate, override the format so that
    // WASAPI's AUTOCONVERTPCM resamples transparently.
    const bool needsResample = (producerRate > 0 && producerRate != nativeSampleRate);

    if (needsResample)
    {
        mixFormat->nSamplesPerSec  = static_cast<DWORD> (producerRate);
        mixFormat->nBlockAlign     = static_cast<WORD> (mixFormat->nChannels * (mixFormat->wBitsPerSample / 8));
        mixFormat->nAvgBytesPerSec = mixFormat->nSamplesPerSec * mixFormat->nBlockAlign;
    }

    const int deviceSampleRate = static_cast<int> (mixFormat->nSamplesPerSec);
    const int frameSize        = static_cast<int> (mixFormat->nBlockAlign);

    sampleRate.store (deviceSampleRate);
    numChannels.store (deviceChannels);

    // ---- Initialize audio client (shared mode, event-driven) ----
    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minPeriod = 0;
    audioClient->GetDevicePeriod (&defaultPeriod, &minPeriod);

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (needsResample)
        streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = audioClient->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                  streamFlags,
                                  defaultPeriod,
                                  0,
                                  mixFormat,
                                  nullptr);
    if (FAILED (hr))
    {
        setError (ErrorCode::initializeAudioClient, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // ---- Set event handle ----
    hr = audioClient->SetEventHandle (renderEvent);
    if (FAILED (hr))
    {
        setError (ErrorCode::setEventHandle, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // ---- Get render client ----
    hr = audioClient->GetService (__uuidof (IAudioRenderClient),
                                  reinterpret_cast<void**> (&renderClient));
    if (FAILED (hr) || renderClient == nullptr)
    {
        setError (ErrorCode::renderClient, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // ---- Get endpoint buffer size ----
    UINT32 bufferSize = 0;
    hr = audioClient->GetBufferSize (&bufferSize);
    if (FAILED (hr) || bufferSize == 0)
    {
        setError (ErrorCode::bufferSize, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // ---- Pre-fill with silence ----
    {
        BYTE* data = nullptr;
        hr = renderClient->GetBuffer (bufferSize, &data);
        if (SUCCEEDED (hr) && data != nullptr)
        {
            std::memset (data, 0, static_cast<size_t> (bufferSize) * frameSize);
            renderClient->ReleaseBuffer (bufferSize, 0);
        }
    }

    // ---- Allocate temporary per-channel read buffers ----
    // Used to read from the RingBuffer (which stores per-channel data) and
    // then interleave into the WASAPI output buffer.
    std::vector<std::vector<float>> tempChannelBuffers (static_cast<size_t> (deviceChannels));
    for (auto& ch : tempChannelBuffers)
        ch.resize (static_cast<size_t> (bufferSize), 0.0f);

    std::vector<float*> tempChannelPtrs (static_cast<size_t> (deviceChannels));
    for (int ch = 0; ch < deviceChannels; ++ch)
        tempChannelPtrs[static_cast<size_t> (ch)] = tempChannelBuffers[static_cast<size_t> (ch)].data();

    // The engine (ASIO / mic interface clock) produces and this endpoint
    // consumes on its own clock, so read through a drift-compensating reader.
    // Without it the backlog wanders with the clock drift - in practice it
    // grew for the whole session - and it also drops whatever the engine
    // wrote before this stream was live.
    //
    // The backlog is kept as small as the two clocks allow: each render event
    // asks for about one device period, the engine delivers in blocks, so hold
    // the largest request seen + one engine block + 2 ms of jitter margin.
    // Recomputed every event: the engine's block size can change, and a
    // driver that asks for more than one period at a time must not underrun.
    const int periodFrames = juce::jmax (1, static_cast<int> (defaultPeriod * deviceSampleRate / 10000000));
    int largestRequest = periodFrames;
    int eventsSeen = 0;

    // The engine's latency budget needs to know how long WASAPI holds a
    // period of audio, even before the first render event arrives.
    devicePeriodMs.store (1000.0 * periodFrames / deviceSampleRate,
                          std::memory_order_relaxed);

    DriftCompensatedReader reader;
    reader.prepare (buffer->getNumChannels(), static_cast<int> (bufferSize),
                    static_cast<double> (deviceSampleRate), periodFrames);

    // ---- Start the stream ----
    hr = audioClient->Start();
    if (FAILED (hr))
    {
        setError (ErrorCode::startAudioClient, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // The endpoint is open and running: start() can return true now.
    SetEvent (startEvent);

    // ---- Elevate thread priority via MMCSS ----
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW (L"Audio", &mmcssTaskIndex);

    // ---- Render loop ----
    HANDLE events[] = { stopEvent, renderEvent };

    while (true)
    {
        DWORD result = WaitForMultipleObjects (2, events, FALSE, 2000);

        if (result == WAIT_OBJECT_0)
        {
            // Stop event signalled.
            break;
        }

        if (result == WAIT_OBJECT_0 + 1)
        {
            // Render event: WASAPI wants more audio data.
            UINT32 padding = 0;
            hr = audioClient->GetCurrentPadding (&padding);

            if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
            {
                setError (ErrorCode::deviceDisconnected);
                break;
            }

            if (FAILED (hr))
                continue;

            UINT32 available = bufferSize - padding;
            if (available == 0)
                continue;

            BYTE* data = nullptr;
            hr = renderClient->GetBuffer (available, &data);

            if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
            {
                setError (ErrorCode::deviceDisconnected);
                break;
            }

            if (FAILED (hr) || data == nullptr)
                continue;

            // Read per-channel data from the ring buffer, then interleave
            // into the WASAPI output buffer.
            float* outputFloat = reinterpret_cast<float*> (data);
            const int framesToRead = static_cast<int> (available);

            const double currentProducerRate = producerSampleRate.load (std::memory_order_relaxed);
            const double nominalRatio = currentProducerRate > 0.0 ? currentProducerRate / deviceSampleRate : 1.0;

            // The first few events after Start() can be irregular.
            if (++eventsSeen > 3)
                largestRequest = juce::jmax (largestRequest, framesToRead);

            // Target in producer frames (that is what the ring holds).
            const int targetFrames = static_cast<int> ((largestRequest + deviceSampleRate * 0.002)
                                                       * nominalRatio)
                                   + producerBlockSize.load (std::memory_order_relaxed);
            reader.setTargetFill (targetFrames);
            backlogTargetFrames.store (targetFrames, std::memory_order_relaxed);

            // Always fills all framesToRead frames: audio, or silence while
            // the ring is priming after start-up or an underrun.
            reader.read (*buffer, tempChannelPtrs.data(), deviceChannels,
                         framesToRead, nominalRatio);
            underrunCount.store (reader.getUnderrunCount(), std::memory_order_relaxed);

            // Interleave the per-channel data into the output buffer.
            for (int i = 0; i < framesToRead; ++i)
            {
                for (int ch = 0; ch < deviceChannels; ++ch)
                    outputFloat[i * deviceChannels + ch] = tempChannelPtrs[static_cast<size_t> (ch)][i];
            }

            renderClient->ReleaseBuffer (available, 0);
        }
        else if (result == WAIT_TIMEOUT)
        {
            // Liveness check -- if the device was invalidated between events
            // we detect it on the next GetCurrentPadding call. Just continue.
        }
        else
        {
            // WAIT_FAILED or other unexpected return.
            setError (ErrorCode::waitFailed, (int) GetLastError());
            break;
        }
    }

    // ---- Shutdown ----
    if (mmcssHandle != nullptr)
        AvRevertMmThreadCharacteristics (mmcssHandle);

    audioClient->Stop();
    cleanup();
    running.store (false);
}
