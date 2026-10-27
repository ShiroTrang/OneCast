#include "WasapiOutputDevice.h"
#include "BacklogTarget.h"
#include "RingBuffer.h"
#include "DriftCompensatedReader.h"
#include "../Diagnostics/StartupLog.h"
#include <avrt.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "avrt.lib")

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if ((p) != nullptr) { (p)->Release(); (p) = nullptr; } } while (0)
#endif

namespace
{
    /// Sample layouts an exclusive stream may accept. Shared mode always uses
    /// the mix format (32-bit float).
    enum class PcmKind { float32, pcm24in32, pcm16 };

    int bytesPerSample (PcmKind kind) noexcept
    {
        return kind == PcmKind::pcm16 ? 2 : 4;
    }

    const char* pcmKindName (PcmKind kind) noexcept
    {
        switch (kind)
        {
            case PcmKind::float32:   return "float32";
            case PcmKind::pcm24in32: return "pcm24in32";
            case PcmKind::pcm16:     return "pcm16";
        }

        return "unknown";
    }

    const char* modeName (WasapiOutputDevice::OutputMode mode) noexcept
    {
        switch (mode)
        {
            case WasapiOutputDevice::OutputMode::exclusive:        return "exclusive";
            case WasapiOutputDevice::OutputMode::sharedLowLatency: return "sharedLowLatency";
            case WasapiOutputDevice::OutputMode::shared:           return "shared";
        }

        return "unknown";
    }

    /// One stereo exclusive-mode candidate. 24-in-32 keeps 24 valid bits in a
    /// 32-bit container (left-justified); 16-bit uses plain PCM because some
    /// drivers only accept the classic header for that depth.
    void fillExclusiveFormat (WAVEFORMATEXTENSIBLE& format, PcmKind kind, int rate) noexcept
    {
        std::memset (&format, 0, sizeof (format));

        const int bytes = bytesPerSample (kind);

        format.Format.nChannels       = 2;
        format.Format.nSamplesPerSec  = (DWORD) rate;
        format.Format.wBitsPerSample  = (WORD) (8 * bytes);
        format.Format.nBlockAlign     = (WORD) (format.Format.nChannels * bytes);
        format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;

        if (kind == PcmKind::pcm16)
        {
            format.Format.wFormatTag = WAVE_FORMAT_PCM;
            format.Samples.wValidBitsPerSample = 16;
        }
        else
        {
            format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            format.Format.cbSize = (WORD) (sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX));
            format.Samples.wValidBitsPerSample = kind == PcmKind::pcm24in32 ? 24 : 32;
            format.SubFormat = kind == PcmKind::float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                                        : KSDATAFORMAT_SUBTYPE_PCM;
            format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        }
    }

    REFERENCE_TIME framesToRefTime (int frames, int rate) noexcept
    {
        const auto safeRate = rate > 0 ? rate : 48000;
        return (REFERENCE_TIME) (((long long) frames * 10000000LL) / safeRate);
    }

    /// Full scale stays at +/-32767 / +/-8388607: symmetric and below the
    /// safety limiter's ceiling, so these conversions can never wrap.
    int16_t toPcm16 (float sample) noexcept
    {
        return (int16_t) juce::roundToInt (juce::jlimit (-1.0f, 1.0f, sample) * 32767.0f);
    }

    int32_t toPcm24In32 (float sample) noexcept
    {
        // Times 256, not a left shift: shifting a negative int would be
        // undefined, and every 24-bit value still fits the 32-bit container.
        return (int32_t) (juce::roundToInt (juce::jlimit (-1.0f, 1.0f, sample) * 8388607.0f) * 256);
    }
}

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
    outputMode.store ((int) OutputMode::shared, std::memory_order_relaxed);
    underrunCount.store (0, std::memory_order_relaxed);
    measuredRingFillFrames.store (0.0, std::memory_order_relaxed);
    measuredPaddingFrames.store (0.0, std::memory_order_relaxed);
    streamLatencyMs.store (0.0, std::memory_order_relaxed);
    paddingValid.store (false, std::memory_order_relaxed);
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
    measuredRingFillFrames.store (0.0, std::memory_order_relaxed);
    measuredPaddingFrames.store (0.0, std::memory_order_relaxed);
    streamLatencyMs.store (0.0, std::memory_order_relaxed);
    paddingValid.store (false, std::memory_order_relaxed);
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

WasapiOutputDevice::OutputMode WasapiOutputDevice::getOutputMode() const noexcept
{
    return (OutputMode) outputMode.load (std::memory_order_relaxed);
}

double WasapiOutputDevice::getMeasuredLatencyMs() const noexcept
{
    if (! running.load (std::memory_order_relaxed))
        return 0.0;

    const double deviceRate = (double) sampleRate.load (std::memory_order_relaxed);
    if (deviceRate <= 0.0)
        return 0.0;

    double producerRate = producerSampleRate.load (std::memory_order_relaxed);
    if (producerRate <= 0.0)
        producerRate = deviceRate;

    // What is actually queued: the audio waiting in the ring plus the frames
    // already sitting in the endpoint. The stream latency is only a fallback
    // for endpoints that do not report padding, otherwise it would count the
    // same buffering twice.
    const double ringMs = 1000.0 * measuredRingFillFrames.load (std::memory_order_relaxed)
                            / producerRate;
    const double paddingMs = paddingValid.load (std::memory_order_relaxed)
                               ? 1000.0 * measuredPaddingFrames.load (std::memory_order_relaxed)
                                   / deviceRate
                               : streamLatencyMs.load (std::memory_order_relaxed);

    return ringMs + paddingMs;
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

    // ---- Activate audio client and get the mix format ----
    hr = device->Activate (__uuidof (IAudioClient), CLSCTX_ALL,
                           nullptr, reinterpret_cast<void**> (&audioClient));
    if (FAILED (hr) || audioClient == nullptr)
    {
        setError (ErrorCode::activateAudioClient, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

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

    const int nativeSampleRate = static_cast<int> (mixFormat->nSamplesPerSec);

    // If the producer writes at a different rate, override the format so that
    // WASAPI's AUTOCONVERTPCM resamples transparently in shared mode. The
    // exclusive rung below tries the producer rate itself and otherwise relies
    // on the drift-compensated reader's ratio.
    const bool needsResample = (producerRate > 0 && producerRate != nativeSampleRate);

    if (needsResample)
    {
        mixFormat->nSamplesPerSec  = static_cast<DWORD> (producerRate);
        mixFormat->nBlockAlign     = static_cast<WORD> (mixFormat->nChannels * (mixFormat->wBitsPerSample / 8));
        mixFormat->nAvgBytesPerSec = mixFormat->nSamplesPerSec * mixFormat->nBlockAlign;
    }

    // ---- Choose the stream mode (ladder, highest quality of service first) ----
    OutputMode mode      = OutputMode::shared;
    PcmKind    kind      = PcmKind::float32;
    int deviceSampleRate = 0;
    int deviceChannels   = 2;
    int frameSize        = 0;
    int periodFrames     = 0;
    UINT32 bufferSize    = 0;
    HRESULT lastError    = AUDCLNT_E_UNSUPPORTED_FORMAT;
    bool opened          = false;

    // Initialize() succeeds at most once per client, so every rung starts
    // from a freshly activated one. A refused format or mode then cannot
    // poison the next attempt.
    auto activateClient = [&]() -> HRESULT
    {
        SAFE_RELEASE (audioClient);
        return device->Activate (__uuidof (IAudioClient), CLSCTX_ALL,
                                 nullptr, reinterpret_cast<void**> (&audioClient));
    };

    // ---- Rung 1: exclusive, event-driven, minimum device period ----
    {
        int rates[3] = { 0, 0, 0 };
        int rateCount = 0;

        auto addRate = [&] (int rate)
        {
            if (rate <= 0)
                return;

            for (int i = 0; i < rateCount; ++i)
                if (rates[i] == rate)
                    return;

            if (rateCount < (int) (sizeof (rates) / sizeof (rates[0])))
                rates[rateCount++] = rate;
        };

        addRate (producerRate);
        addRate (48000);
        addRate (44100);

        const PcmKind kinds[] = { PcmKind::float32, PcmKind::pcm24in32, PcmKind::pcm16 };

        for (int r = 0; r < rateCount && ! opened; ++r)
        {
            for (PcmKind candidate : kinds)
            {
                WAVEFORMATEXTENSIBLE format;
                fillExclusiveFormat (format, candidate, rates[r]);

                hr = activateClient();
                if (FAILED (hr) || audioClient == nullptr)
                {
                    lastError = hr;
                    continue;
                }

                REFERENCE_TIME defaultDevPeriod = 0, minDevPeriod = 0;
                hr = audioClient->GetDevicePeriod (&defaultDevPeriod, &minDevPeriod);
                if (FAILED (hr) || minDevPeriod <= 0)
                {
                    lastError = FAILED (hr) ? hr : AUDCLNT_E_UNSUPPORTED_FORMAT;
                    continue;
                }

                // For exclusive mode the nearest-format output must be null;
                // anything but S_OK means this exact format is not available.
                const HRESULT supported = audioClient->IsFormatSupported (
                    AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*) &format, nullptr);

                if (supported != S_OK)
                {
                    lastError = supported;
                    continue;
                }

                REFERENCE_TIME period = minDevPeriod;
                bool initialised = false;

                for (int attempt = 0; attempt < 2; ++attempt)
                {
                    hr = audioClient->Initialize (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                  period, period,
                                                  (WAVEFORMATEX*) &format, nullptr);

                    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
                    {
                        // Standard realign-and-retry: the endpoint reports the
                        // aligned buffer size and the duration follows from it.
                        // The failed client is replaced first.
                        UINT32 alignedFrames = 0;

                        if (FAILED (audioClient->GetBufferSize (&alignedFrames))
                            || alignedFrames == 0)
                        {
                            lastError = hr;
                            break;
                        }

                        period = framesToRefTime ((int) alignedFrames, rates[r]);

                        hr = activateClient();
                        if (FAILED (hr) || audioClient == nullptr)
                        {
                            lastError = hr;
                            break;
                        }

                        continue;
                    }

                    if (SUCCEEDED (hr))
                        initialised = true;
                    else
                        lastError = hr;

                    break;
                }

                if (! initialised)
                    continue;

                mode             = OutputMode::exclusive;
                kind             = candidate;
                deviceSampleRate = rates[r];
                deviceChannels   = (int) format.Format.nChannels;
                frameSize        = (int) format.Format.nBlockAlign;
                opened           = true;
                break;
            }
        }
    }

    // ---- Rung 2: shared low-latency (IAudioClient3, minimum engine period) ----
    if (! opened)
    {
        hr = activateClient();

        if (FAILED (hr) || audioClient == nullptr)
        {
            lastError = hr;
        }
        else
        {
            IAudioClient3* client3 = nullptr;
            hr = audioClient->QueryInterface (__uuidof (IAudioClient3),
                                              reinterpret_cast<void**> (&client3));

            if (SUCCEEDED (hr) && client3 != nullptr)
            {
                UINT32 sharedDefault = 0, fundamental = 0, sharedMin = 0, sharedMax = 0;

                hr = client3->GetSharedModeEnginePeriod ((WAVEFORMATEX*) mixFormat,
                                                         &sharedDefault, &fundamental,
                                                         &sharedMin, &sharedMax);

                if (SUCCEEDED (hr) && sharedMin > 0)
                {
                    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
                    if (needsResample)
                        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                               | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

                    hr = client3->InitializeSharedAudioStream (flags, sharedMin,
                                                               (WAVEFORMATEX*) mixFormat,
                                                               nullptr);

                    if (SUCCEEDED (hr))
                    {
                        mode             = OutputMode::sharedLowLatency;
                        kind             = PcmKind::float32;
                        deviceSampleRate = (int) mixFormat->nSamplesPerSec;
                        deviceChannels   = (int) mixFormat->nChannels;
                        frameSize        = (int) mixFormat->nBlockAlign;
                        periodFrames     = (int) sharedMin;
                        opened           = true;
                    }
                    else
                    {
                        lastError = hr;
                    }
                }
                else
                {
                    lastError = FAILED (hr) ? hr : AUDCLNT_E_UNSUPPORTED_FORMAT;
                }
            }
            else
            {
                lastError = FAILED (hr) ? hr : E_NOINTERFACE;
            }

            SAFE_RELEASE (client3);
        }
    }

    // ---- Rung 3: shared, default period (the previous behaviour) ----
    if (! opened)
    {
        hr = activateClient();
        if (FAILED (hr) || audioClient == nullptr)
        {
            setError (ErrorCode::activateAudioClient, (int) hr);
            running.store (false, std::memory_order_release);
            cleanup();
            return;
        }

        REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
        hr = audioClient->GetDevicePeriod (&defaultPeriod, &minPeriod);
        if (FAILED (hr))
        {
            setError (ErrorCode::initializeAudioClient, (int) hr);
            running.store (false, std::memory_order_release);
            cleanup();
            return;
        }

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

        mode             = OutputMode::shared;
        kind             = PcmKind::float32;
        deviceSampleRate = (int) mixFormat->nSamplesPerSec;
        deviceChannels   = (int) mixFormat->nChannels;
        frameSize        = (int) mixFormat->nBlockAlign;
        periodFrames     = juce::jmax (1, (int) (defaultPeriod * deviceSampleRate / 10000000));
        opened           = true;
    }

    if (! opened || audioClient == nullptr)
    {
        setError (ErrorCode::initializeAudioClient, (int) lastError);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // ---- Set event handle and get the render client ----
    hr = audioClient->SetEventHandle (renderEvent);
    if (FAILED (hr))
    {
        setError (ErrorCode::setEventHandle, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    hr = audioClient->GetService (__uuidof (IAudioRenderClient),
                                  reinterpret_cast<void**> (&renderClient));
    if (FAILED (hr) || renderClient == nullptr)
    {
        setError (ErrorCode::renderClient, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // ---- Get the endpoint buffer size ----
    hr = audioClient->GetBufferSize (&bufferSize);
    if (FAILED (hr) || bufferSize == 0)
    {
        setError (ErrorCode::bufferSize, (int) hr);
        running.store (false, std::memory_order_release);
        cleanup();
        return;
    }

    // An exclusive event fires once per aligned endpoint buffer, so that is
    // the period actually in use. The shared rungs already know theirs.
    if (mode == OutputMode::exclusive)
        periodFrames = (int) bufferSize;

    periodFrames = juce::jmax (1, periodFrames);

    REFERENCE_TIME streamLatency = 0;
    if (FAILED (audioClient->GetStreamLatency (&streamLatency)))
        streamLatency = 0;

    const double periodMs = 1000.0 * periodFrames / (double) deviceSampleRate;

    sampleRate.store (deviceSampleRate);
    numChannels.store (deviceChannels);
    outputMode.store ((int) mode);
    devicePeriodMs.store (periodMs);
    streamLatencyMs.store (streamLatency / 10000.0);
    paddingValid.store (false);
    measuredRingFillFrames.store (0.0);
    measuredPaddingFrames.store (0.0);

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
    // then interleave into the WASAPI output buffer. The format conversion
    // below also happens outside the wait, so nothing here allocates again.
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
    // The backlog target is the largest request of the last few seconds plus
    // a small margin; it grows at once for a large endpoint request and then
    // shrinks back, so one burst no longer costs latency for the session.
    BacklogTarget backlog;
    backlog.prepare ((double) deviceSampleRate, periodFrames,
                     producerBlockSize.load (std::memory_order_relaxed));

    const double initialProducerRate = producerSampleRate.load (std::memory_order_relaxed);
    const double initialRatio = initialProducerRate > 0.0
                                  ? initialProducerRate / (double) deviceSampleRate
                                  : 1.0;
    const int initialTarget = (int) std::lround (backlog.getTargetFrames() * initialRatio);

    // The engine was already running while this stream opened, so the ring
    // holds audio the listener never heard. Playing it now would only add its
    // age to the output latency, and the drift reader would trim it only above
    // its 100 ms resync cliff (and then at 0.2 % per second). Cut it to the
    // starting target here, before the stream starts.
    {
        const int staleFrames = buffer->getAvailableForRead() - initialTarget;
        if (staleFrames > 0)
            buffer->discard (staleFrames);
    }

    backlogTargetFrames.store (initialTarget, std::memory_order_relaxed);

    DriftCompensatedReader reader;
    reader.prepare (buffer->getNumChannels(), static_cast<int> (bufferSize),
                    static_cast<double> (deviceSampleRate), initialTarget);

    // Before the real-time loop, not inside it: this is the one place the
    // render thread may build a string.
    StartupLog::mark ("output: mode=" + juce::String (modeName (mode))
                      + " format=" + juce::String (pcmKindName (kind))
                      + " " + juce::String (deviceChannels) + " ch "
                      + juce::String (deviceSampleRate) + " Hz period="
                      + juce::String (periodMs, 2) + " ms");

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

    double ringFillEstimate = 0.0;
    double paddingEstimate  = 0.0;
    constexpr double smoothingSeconds = 0.5;
    int eventsSeen = 0;

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

            const int framesToRead = static_cast<int> (available);

            const double currentProducerRate = producerSampleRate.load (std::memory_order_relaxed);
            const double nominalRatio = currentProducerRate > 0.0 ? currentProducerRate / deviceSampleRate : 1.0;

            // Honest queue measurement, taken before this read consumes
            // anything: what is in the ring plus what the endpoint has not
            // played yet. One-pole smoothed so a single event does not make
            // the latency label jump.
            const int ringFill = buffer->getAvailableForRead();
            const double smoothing = 1.0 - std::exp (-(double) framesToRead
                                                     / (smoothingSeconds * (double) deviceSampleRate));

            ringFillEstimate += ((double) ringFill - ringFillEstimate) * smoothing;
            paddingEstimate  += ((double) padding  - paddingEstimate)  * smoothing;

            measuredRingFillFrames.store (ringFillEstimate, std::memory_order_relaxed);
            measuredPaddingFrames.store (paddingEstimate, std::memory_order_relaxed);
            paddingValid.store (true, std::memory_order_relaxed);

            // The target is in producer frames because that is what the ring
            // holds; the nominal ratio converts from device frames.
            backlog.setProducerBlockFrames (producerBlockSize.load (std::memory_order_relaxed));

            // The first events after Start() can ask for a whole buffer while
            // the endpoint is still priming; treating that as the steady
            // request would raise the target for the whole session.
            if (++eventsSeen > 3)
                backlog.onRequest (framesToRead);

            const int targetFrames = (int) std::lround (backlog.getTargetFrames() * nominalRatio);
            reader.setTargetFill (targetFrames);
            backlogTargetFrames.store (targetFrames, std::memory_order_relaxed);

            // Always fills all framesToRead frames: audio, or silence while
            // the ring is priming after start-up or an underrun.
            reader.read (*buffer, tempChannelPtrs.data(), deviceChannels,
                         framesToRead, nominalRatio);
            underrunCount.store (reader.getUnderrunCount(), std::memory_order_relaxed);

            // Interleave the per-channel data into the output buffer, in the
            // format the stream actually accepted.
            if (kind == PcmKind::float32)
            {
                float* outputFloat = reinterpret_cast<float*> (data);

                for (int i = 0; i < framesToRead; ++i)
                    for (int ch = 0; ch < deviceChannels; ++ch)
                        outputFloat[i * deviceChannels + ch] = tempChannelPtrs[(size_t) ch][i];
            }
            else if (kind == PcmKind::pcm16)
            {
                auto* outputPcm = reinterpret_cast<int16_t*> (data);

                for (int i = 0; i < framesToRead; ++i)
                    for (int ch = 0; ch < deviceChannels; ++ch)
                        outputPcm[i * deviceChannels + ch] = toPcm16 (tempChannelPtrs[(size_t) ch][i]);
            }
            else
            {
                auto* outputPcm = reinterpret_cast<int32_t*> (data);

                for (int i = 0; i < framesToRead; ++i)
                    for (int ch = 0; ch < deviceChannels; ++ch)
                        outputPcm[i * deviceChannels + ch] = toPcm24In32 (tempChannelPtrs[(size_t) ch][i]);
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
