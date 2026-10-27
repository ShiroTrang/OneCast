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
}

WasapiOutputDevice::~WasapiOutputDevice()
{
    stop();

    if (stopEvent != nullptr)
        CloseHandle (stopEvent);
    if (renderEvent != nullptr)
        CloseHandle (renderEvent);
}

//==============================================================================
bool WasapiOutputDevice::start (const juce::String& deviceId, RingBuffer& inputBuffer,
                                int producerRate)
{
    if (running.load (std::memory_order_acquire))
        return false;

    // Join any dead thread from a previous failed start
    if (thread.joinable())
        thread.join();

    error.store (false, std::memory_order_relaxed);
    errorMessage.clear();
    ResetEvent (stopEvent);

    if (producerRate > 0)
        producerSampleRate.store (producerRate, std::memory_order_relaxed);

    running.store (true, std::memory_order_release);

    thread = std::thread ([this, deviceId, &inputBuffer, producerRate]()
    {
        renderThread (deviceId, &inputBuffer, producerRate);
    });

    return true;
}

void WasapiOutputDevice::stop()
{
    SetEvent (stopEvent);

    if (thread.joinable())
        thread.join();

    running.store (false, std::memory_order_release);
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
    return error.load (std::memory_order_relaxed);
}

juce::String WasapiOutputDevice::getErrorMessage() const
{
    return errorMessage;
}

int WasapiOutputDevice::getSampleRate() const
{
    return sampleRate.load (std::memory_order_relaxed);
}

int WasapiOutputDevice::getNumChannels() const
{
    return numChannels.load (std::memory_order_relaxed);
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
        errorMessage = "Failed to initialize COM: 0x" + juce::String::toHexString ((int) hr);
        error.store (true);
        running.store (false);
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
    };

    // ---- Get device enumerator ----
    hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr,
                          CLSCTX_INPROC_SERVER,
                          __uuidof (IMMDeviceEnumerator),
                          reinterpret_cast<void**> (&enumerator));
    if (FAILED (hr) || enumerator == nullptr)
    {
        errorMessage = "Failed to create device enumerator: 0x" + juce::String::toHexString ((int) hr);
        error.store (true);
        running.store (false);
        cleanup();
        return;
    }

    // ---- Get render device ----
    if (deviceId.isEmpty())
    {
        hr = enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &device);
    }
    else
    {
        hr = enumerator->GetDevice (deviceId.toWideCharPointer(), &device);
    }

    if (FAILED (hr) || device == nullptr)
    {
        errorMessage = "Failed to get render device: 0x" + juce::String::toHexString ((int) hr);
        error.store (true);
        running.store (false);
        cleanup();
        return;
    }

    // ---- Activate audio client ----
    hr = device->Activate (__uuidof (IAudioClient), CLSCTX_ALL,
                           nullptr, reinterpret_cast<void**> (&audioClient));
    if (FAILED (hr) || audioClient == nullptr)
    {
        errorMessage = "Failed to activate audio client: 0x" + juce::String::toHexString ((int) hr);
        error.store (true);
        running.store (false);
        cleanup();
        return;
    }

    // ---- Get mix format ----
    hr = audioClient->GetMixFormat (&mixFormat);
    if (FAILED (hr) || mixFormat == nullptr)
    {
        errorMessage = "Failed to get mix format: 0x" + juce::String::toHexString ((int) hr);
        error.store (true);
        running.store (false);
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
        errorMessage = "Failed to initialize audio client: 0x" + juce::String::toHexString ((int) hr);
        error.store (true);
        running.store (false);
        cleanup();
        return;
    }

    // ---- Set event handle ----
    hr = audioClient->SetEventHandle (renderEvent);
    if (FAILED (hr))
    {
        errorMessage = "Failed to set event handle: 0x" + juce::String::toHexString ((int) hr);
        error.store (true);
        running.store (false);
        cleanup();
        return;
    }

    // ---- Get render client ----
    hr = audioClient->GetService (__uuidof (IAudioRenderClient),
                                  reinterpret_cast<void**> (&renderClient));
    if (FAILED (hr) || renderClient == nullptr)
    {
        errorMessage = "Failed to get render client: 0x" + juce::String::toHexString ((int) hr);
        error.store (true);
        running.store (false);
        cleanup();
        return;
    }

    // ---- Get endpoint buffer size ----
    UINT32 bufferSize = 0;
    hr = audioClient->GetBufferSize (&bufferSize);
    if (FAILED (hr) || bufferSize == 0)
    {
        errorMessage = "Failed to get buffer size: 0x" + juce::String::toHexString ((int) hr);
        error.store (true);
        running.store (false);
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

    DriftCompensatedReader reader;
    reader.prepare (buffer->getNumChannels(), static_cast<int> (bufferSize),
                    static_cast<double> (deviceSampleRate), periodFrames);

    // ---- Start the stream ----
    hr = audioClient->Start();
    if (FAILED (hr))
    {
        errorMessage = "Failed to start audio client: 0x" + juce::String::toHexString ((int) hr);
        error.store (true);
        running.store (false);
        cleanup();
        return;
    }

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
                errorMessage = "Audio device was disconnected";
                error.store (true);
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
                errorMessage = "Audio device was disconnected";
                error.store (true);
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

            // Always fills all framesToRead frames: audio, or silence while
            // the ring is priming after start-up or an underrun.
            reader.read (*buffer, tempChannelPtrs.data(), deviceChannels,
                         framesToRead, nominalRatio);

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
            errorMessage = "WaitForMultipleObjects failed: " + juce::String ((int) GetLastError());
            error.store (true);
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
