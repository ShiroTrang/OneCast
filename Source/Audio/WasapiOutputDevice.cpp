#include "WasapiOutputDevice.h"
#include "BacklogTarget.h"
#include "PeriodPolicy.h"
#include "RingBuffer.h"
#include "DriftCompensatedReader.h"
#include "OutputCeiling.h"
#include "SharedStreamFormat.h"
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

    /// The value getOutputPeriodFrames() publishes for the next launch: the
    /// period that was sustained, or the "use standard shared" sentinel when
    /// the low-latency rung was left behind. Exclusive has no IAudioClient3
    /// period, so it keeps the no-hint value.
    int periodHintFor (WasapiOutputDevice::OutputMode mode, int sharedPeriod) noexcept
    {
        if (mode == WasapiOutputDevice::OutputMode::sharedLowLatency)
            return sharedPeriod;

        return mode == WasapiOutputDevice::OutputMode::shared
                 ? WasapiOutputDevice::standardSharedPeriodHint
                 : 0;
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

    /// The endpoint's friendly name ("CABLE Input (VB-Audio Virtual Cable)",
    /// say). Used both by the device list and by the render thread, which has
    /// to know whether it may try the exclusive rung.
    juce::String getDeviceFriendlyName (IMMDevice* device)
    {
        if (device == nullptr)
            return {};

        IPropertyStore* props = nullptr;

        if (FAILED (device->OpenPropertyStore (STGM_READ, &props)) || props == nullptr)
            return {};

        juce::String name;
        PROPVARIANT varName;
        PropVariantInit (&varName);

        if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &varName))
            && varName.pwszVal != nullptr)
            name = juce::String (varName.pwszVal);

        PropVariantClear (&varName);
        props->Release();
        return name;
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
                                int producerRate, int periodHintFrames)
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
    resyncCount.store (0, std::memory_order_relaxed);
    maxRenderGapMs.store (0.0, std::memory_order_relaxed);
    lateEventCount.store (0, std::memory_order_relaxed);
    bufferErrorCount.store (0, std::memory_order_relaxed);
    starvedEventCount.store (0, std::memory_order_relaxed);
    riskyGapCount.store (0, std::memory_order_relaxed);
    maxRiskyGapMs.store (0.0, std::memory_order_relaxed);
    outputPeriodFrames.store (0, std::memory_order_relaxed);
    escalationCount.store (0, std::memory_order_relaxed);
    measuredRingFillFrames.store (0.0, std::memory_order_relaxed);
    measuredPaddingFrames.store (0.0, std::memory_order_relaxed);
    streamLatencyMs.store (0.0, std::memory_order_relaxed);
    paddingValid.store (false, std::memory_order_relaxed);
    mmcssActive.store (false, std::memory_order_relaxed);
    ResetEvent (stopEvent);
    ResetEvent (startEvent);

    if (producerRate > 0)
        producerSampleRate.store (producerRate, std::memory_order_relaxed);

    running.store (true, std::memory_order_release);

    thread = std::thread ([this, deviceId, &inputBuffer, producerRate, periodHintFrames]()
    {
        renderThread (deviceId, &inputBuffer, producerRate, periodHintFrames);
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
    resyncCount.store (0, std::memory_order_relaxed);
    maxRenderGapMs.store (0.0, std::memory_order_relaxed);
    lateEventCount.store (0, std::memory_order_relaxed);
    bufferErrorCount.store (0, std::memory_order_relaxed);
    starvedEventCount.store (0, std::memory_order_relaxed);
    riskyGapCount.store (0, std::memory_order_relaxed);
    maxRiskyGapMs.store (0.0, std::memory_order_relaxed);
    outputPeriodFrames.store (0, std::memory_order_relaxed);
    escalationCount.store (0, std::memory_order_relaxed);
    measuredRingFillFrames.store (0.0, std::memory_order_relaxed);
    measuredPaddingFrames.store (0.0, std::memory_order_relaxed);
    streamLatencyMs.store (0.0, std::memory_order_relaxed);
    paddingValid.store (false, std::memory_order_relaxed);
    mmcssActive.store (false, std::memory_order_relaxed);
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
        case ErrorCode::streamStalled:          return "the output stream stalled "
                                                       "(no render event or no usable buffer for "
                                                       "500 ms)";
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

int WasapiOutputDevice::getResyncCount() const noexcept
{
    return resyncCount.load (std::memory_order_relaxed);
}

double WasapiOutputDevice::getMaxRenderGapMs() const noexcept
{
    return maxRenderGapMs.load (std::memory_order_relaxed);
}

int WasapiOutputDevice::getLateEventCount() const noexcept
{
    return lateEventCount.load (std::memory_order_relaxed);
}

int WasapiOutputDevice::getBufferErrorCount() const noexcept
{
    return bufferErrorCount.load (std::memory_order_relaxed);
}

int WasapiOutputDevice::getStarvedEventCount() const noexcept
{
    return starvedEventCount.load (std::memory_order_relaxed);
}

int WasapiOutputDevice::getRiskyGapCount() const noexcept
{
    return riskyGapCount.load (std::memory_order_relaxed);
}

double WasapiOutputDevice::getMaxRiskyGapMs() const noexcept
{
    return maxRiskyGapMs.load (std::memory_order_relaxed);
}

int WasapiOutputDevice::getOutputPeriodFrames() const noexcept
{
    return outputPeriodFrames.load (std::memory_order_relaxed);
}

int WasapiOutputDevice::getEscalationCount() const noexcept
{
    return escalationCount.load (std::memory_order_relaxed);
}

bool WasapiOutputDevice::isMmcssActive() const noexcept
{
    return mmcssActive.load (std::memory_order_relaxed);
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

                devices.push_back ({ juce::String (id), getDeviceFriendlyName (dev) });

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
void WasapiOutputDevice::renderThread (juce::String deviceId, RingBuffer* buffer,
                                       int producerRate, int periodHintFrames)
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

    // Exclusive mode takes the endpoint away from every other application:
    // while OneCast holds it, Windows plays nothing else through that device.
    // That is acceptable for the virtual cable (its only user is this app),
    // but a user who picks their speakers or headphones as the output must
    // keep hearing everything else, so only the cable may be opened
    // exclusively. Everything else starts at the shared low-latency rung.
    const bool cableEndpoint = getDeviceFriendlyName (device)
                                   .containsIgnoreCase ("CABLE Input");

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

    // Rung 3 keeps the old behaviour: when the producer writes at a different
    // rate, the stream format is at the producer rate and AUTOCONVERTPCM lets
    // WASAPI resample to the endpoint's mix rate. Rung 2 must NOT do this (see
    // chooseSharedStreamFormat): it keeps the original mix format below, and
    // the drift-compensated reader's nominal ratio covers the difference.
    SharedStreamFormat defaultSharedChoice = chooseSharedStreamFormat (false, nativeSampleRate, producerRate);

    // A copy for rung 3, so mixFormat itself stays the endpoint's native
    // format for the low-latency rung.
    std::vector<BYTE> defaultSharedStorage;
    WAVEFORMATEX* defaultSharedFormat = mixFormat;

    if (defaultSharedChoice.convert)
    {
        const size_t formatBytes = sizeof (WAVEFORMATEX) + mixFormat->cbSize;
        defaultSharedStorage.resize (formatBytes);
        std::memcpy (defaultSharedStorage.data(), mixFormat, formatBytes);

        auto* converted = reinterpret_cast<WAVEFORMATEX*> (defaultSharedStorage.data());
        converted->nSamplesPerSec  = static_cast<DWORD> (defaultSharedChoice.rate);
        converted->nBlockAlign     = static_cast<WORD> (converted->nChannels
                                                        * (converted->wBitsPerSample / 8));
        converted->nAvgBytesPerSec = converted->nSamplesPerSec * converted->nBlockAlign;
        defaultSharedFormat = converted;
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

    // The engine period range of the low-latency rung, kept for an escalation
    // after the stream is running. All zero unless rung 2 opened.
    int sharedMinPeriod   = 0;
    int sharedFundamental = 0;
    int sharedMaxPeriod   = 0;
    int sharedPeriod      = 0;

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
    // Only for the virtual cable; see cableEndpoint above.
    if (cableEndpoint)
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
    // A hint of standardSharedPeriodHint means this machine already had to
    // fall back: opening the low-latency stream again would only starve and
    // re-open once more before settling on the same rung.
    if (! opened && periodHintFrames != standardSharedPeriodHint)
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

                // The low-latency engine period is queried with, and the
                // stream opened with, the endpoint's original mix format. A
                // producer-rate format would be refused here, which used to
                // push every rate mismatch down to the 10 ms rung.
                const SharedStreamFormat lowLatencyChoice =
                    chooseSharedStreamFormat (true, nativeSampleRate, producerRate);

                hr = client3->GetSharedModeEnginePeriod ((WAVEFORMATEX*) mixFormat,
                                                         &sharedDefault, &fundamental,
                                                         &sharedMin, &sharedMax);

                if (SUCCEEDED (hr) && sharedMin > 0)
                {
                    // No AUTOCONVERTPCM: IAudioClient3 shared streams do not
                    // accept it, and the reader's nominal ratio already handles
                    // a producer/device rate difference.
                    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

                    if (lowLatencyChoice.convert)
                        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                               | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

                    // Start at the smallest period the previous run sustained,
                    // or at the endpoint minimum on a first run. Escalation
                    // raises it later if this machine still starves.
                    const int chosenPeriod = PeriodPolicy::initialPeriod ((int) sharedMin,
                                                                         (int) fundamental,
                                                                         (int) sharedMax,
                                                                         periodHintFrames);

                    hr = client3->InitializeSharedAudioStream (flags, (UINT32) chosenPeriod,
                                                               (WAVEFORMATEX*) mixFormat,
                                                               nullptr);

                    if (SUCCEEDED (hr))
                    {
                        mode             = OutputMode::sharedLowLatency;
                        kind             = PcmKind::float32;
                        deviceSampleRate = lowLatencyChoice.rate;
                        deviceChannels   = (int) mixFormat->nChannels;
                        frameSize        = (int) mixFormat->nBlockAlign;
                        periodFrames     = chosenPeriod;
                        sharedMinPeriod   = (int) sharedMin;
                        sharedFundamental = (int) fundamental;
                        sharedMaxPeriod   = (int) sharedMax;
                        sharedPeriod      = chosenPeriod;
                        opened            = true;
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
        if (defaultSharedChoice.convert)
            streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        hr = audioClient->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                      streamFlags,
                                      defaultPeriod,
                                      0,
                                      defaultSharedFormat,
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
        deviceSampleRate = defaultSharedChoice.rate;
        deviceChannels   = (int) defaultSharedFormat->nChannels;
        frameSize        = (int) defaultSharedFormat->nBlockAlign;
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

    double periodMs = 1000.0 * periodFrames / (double) deviceSampleRate;

    sampleRate.store (deviceSampleRate);
    numChannels.store (deviceChannels);
    outputMode.store ((int) mode);

    // The hint saved for the next launch: the sustained low-latency period, or
    // the standard-shared sentinel if this machine had to fall back.
    outputPeriodFrames.store (periodHintFor (mode, sharedPeriod),
                              std::memory_order_relaxed);

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
    std::vector<std::vector<float>> tempChannelBuffers;
    std::vector<float*> tempChannelPtrs;

    // Also called after an escalation re-opens the stream with a possibly
    // different buffer size, which happens outside the render loop, so the
    // allocation is allowed there.
    auto resizeTempBuffers = [&]()
    {
        tempChannelBuffers.resize (static_cast<size_t> (deviceChannels));

        for (auto& ch : tempChannelBuffers)
            ch.assign (static_cast<size_t> (bufferSize), 0.0f);

        tempChannelPtrs.assign (static_cast<size_t> (deviceChannels), nullptr);

        for (int ch = 0; ch < deviceChannels; ++ch)
            tempChannelPtrs[static_cast<size_t> (ch)] = tempChannelBuffers[static_cast<size_t> (ch)].data();
    };

    resizeTempBuffers();

    // The engine (ASIO / mic interface clock) produces and this endpoint
    // consumes on its own clock, so read through a drift-compensating reader.
    // Without it the backlog wanders with the clock drift - in practice it
    // grew for the whole session - and it also drops whatever the engine
    // wrote before this stream was live.
    //
    // The backlog target is the largest request of the last few seconds plus
    // a small margin; it grows at once for a large endpoint request and then
    // shrinks back, so one burst no longer costs latency for the session.
    const double initialProducerRate = producerSampleRate.load (std::memory_order_relaxed);
    const double initialRatio = initialProducerRate > 0.0
                                  ? initialProducerRate / (double) deviceSampleRate
                                  : 1.0;

    // BacklogTarget keeps the block in producer frames and converts it to
    // device frames itself; the target as a whole is then converted back to
    // producer frames here, so the block is not scaled twice.
    // The endpoint buffer, not just the period, is the largest request a
    // single event can make: after a stall the render thread is asked to fill
    // the whole buffer, and the ring must already hold that much.
    BacklogTarget backlog;
    backlog.prepare ((double) deviceSampleRate, periodFrames,
                     producerBlockSize.load (std::memory_order_relaxed),
                     1.5, 5.0, initialProducerRate, (int) bufferSize);

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
                      + juce::String (periodMs, 2) + " ms buffer="
                      + juce::String ((int) bufferSize) + " frames ("
                      + juce::String (1000.0 * bufferSize / deviceSampleRate, 2) + " ms)");

    // The engine's real period grid on this machine, so a soak log shows what
    // the escalation had to choose from. Only rung 2 reports one.
    if (mode == OutputMode::sharedLowLatency)
        StartupLog::mark ("output: engine period min=" + juce::String (sharedMinPeriod)
                          + " fundamental=" + juce::String (sharedFundamental)
                          + " max=" + juce::String (sharedMaxPeriod)
                          + " frames, buffer/period="
                          + juce::String ((double) bufferSize / (double) juce::jmax (1, sharedPeriod), 2));

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

    // ---- Register the render thread with MMCSS ----
    // Pro Audio is the class Windows reserves for audio rendering; Audio is the
    // fallback on systems without a Pro Audio profile. AVRT_PRIORITY_HIGH asks
    // for the top of that class: with a one-period endpoint buffer a wake-up a
    // millisecond late is already a dropout.
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW (L"Pro Audio", &mmcssTaskIndex);
    const char* mmcssTask = "Pro Audio";

    if (mmcssHandle == nullptr)
    {
        mmcssTaskIndex = 0;
        mmcssHandle = AvSetMmThreadCharacteristicsW (L"Audio", &mmcssTaskIndex);
        mmcssTask = "Audio";
    }

    const bool mmcssHighPriority = mmcssHandle != nullptr
                                    && AvSetMmThreadPriority (mmcssHandle, AVRT_PRIORITY_HIGH) != FALSE;

    // Published for describeLatency() on the message thread. Only whether the
    // registration succeeded matters there; the class name stays in this log.
    mmcssActive.store (mmcssHandle != nullptr, std::memory_order_relaxed);

    // Before the loop: after this the thread must not build a string or touch
    // the file system again.
    StartupLog::mark (juce::String ("output: mmcss=")
                      + (mmcssHandle != nullptr ? mmcssTask : "failed")
                      + (mmcssHighPriority ? " highPriority"
                                           : " priorityNotRaised"));

    // ---- Render loop state ----
    HANDLE events[] = { stopEvent, renderEvent };

    double ringFillEstimate = 0.0;
    double paddingEstimate  = 0.0;
    constexpr double smoothingSeconds = 0.5;
    int eventsSeen = 0;

    // Deadline diagnostics. QueryPerformanceCounter reads a per-process
    // counter without a syscall, so it is safe inside the loop.
    LARGE_INTEGER qpcFrequency {};
    QueryPerformanceFrequency (&qpcFrequency);

    const auto qpcSeconds = [&qpcFrequency]() noexcept
    {
        LARGE_INTEGER now {};
        QueryPerformanceCounter (&now);

        return qpcFrequency.QuadPart > 0
                 ? (double) now.QuadPart / (double) qpcFrequency.QuadPart
                 : 0.0;
    };

    LARGE_INTEGER previousEventQpc {};
    bool havePreviousEvent = false;

    double maxGapMs = 0.0;
    double biggestRiskyGapMs = 0.0;
    int lateEvents = 0;
    int riskyGaps = 0;
    int bufferErrors = 0;

    // The escalation target: a period in frames, or 0 for the standard shared
    // rung when the low-latency rung is already at its maximum. The cause and
    // the gap are only kept so the log between loops can say why it fired.
    enum class EscalationCause { starved, riskyGap };
    int escalateToPeriod = 0;
    EscalationCause escalationCause = EscalationCause::starved;
    double escalationGapMs = 0.0;

    // Ignore events while a freshly started stream primes its endpoint.
    EscalationWindow starvation;
    starvation.reset (qpcSeconds());

    // Risky wake-up gaps use the same threshold and grace: two stalls the
    // buffer cannot cover within a minute mean the period is too small.
    EscalationWindow riskyGapsWindow;
    riskyGapsWindow.reset (qpcSeconds());

    // ---- Re-open helpers (called outside the render loop) ----
    // Everything here may allocate and take a few milliseconds. The loop is
    // not running, so the only cost is the accepted audible gap.
    auto finishReopen = [&]() -> bool
    {
        // Stale signals from the stopped client must not make the first event
        // look like a wake-up that was late.
        ResetEvent (renderEvent);

        // Prime the new endpoint with silence, exactly like the first open.
        {
            BYTE* data = nullptr;
            hr = renderClient->GetBuffer (bufferSize, &data);

            if (SUCCEEDED (hr) && data != nullptr)
            {
                std::memset (data, 0, static_cast<size_t> (bufferSize) * frameSize);
                renderClient->ReleaseBuffer (bufferSize, 0);
            }
        }

        periodMs = 1000.0 * periodFrames / (double) deviceSampleRate;

        sampleRate.store (deviceSampleRate);
        numChannels.store (deviceChannels);
        outputMode.store ((int) mode);
        devicePeriodMs.store (periodMs);
        outputPeriodFrames.store (periodHintFor (mode, sharedPeriod),
                                  std::memory_order_relaxed);

        REFERENCE_TIME streamLatency = 0;
        if (FAILED (audioClient->GetStreamLatency (&streamLatency)))
            streamLatency = 0;

        streamLatencyMs.store (streamLatency / 10000.0);
        paddingValid.store (false);
        measuredPaddingFrames.store (0.0);

        // Everything the loop touches is prepared before Start(), as in the
        // initial open: the first render event after Start() must not find a
        // half-prepared reader, and allocating here is allowed only because
        // the loop is not running yet.
        resizeTempBuffers();

        const double currentProducerRate = producerSampleRate.load (std::memory_order_relaxed);
        const double ratio = currentProducerRate > 0.0
                               ? currentProducerRate / (double) deviceSampleRate
                               : 1.0;

        backlog.prepare ((double) deviceSampleRate, periodFrames,
                         producerBlockSize.load (std::memory_order_relaxed),
                         1.5, 5.0, currentProducerRate, (int) bufferSize);

        const int target = (int) std::lround (backlog.getTargetFrames() * ratio);

        // The re-open gap let the producer run ahead by however long the
        // endpoint was stopped. Playing that backlog after the re-open would
        // add the whole gap to the output latency for the rest of the session,
        // so cut it to the new target exactly as the initial open does.
        {
            const int staleFrames = buffer->getAvailableForRead() - target;

            if (staleFrames > 0)
                buffer->discard (staleFrames);
        }

        backlogTargetFrames.store (target, std::memory_order_relaxed);
        reader.prepare (buffer->getNumChannels(), static_cast<int> (bufferSize),
                        static_cast<double> (deviceSampleRate), target);

        // The re-open gap itself is deliberate, not a scheduling failure.
        previousEventQpc = {};
        havePreviousEvent = false;
        eventsSeen = 0;
        starvation.reset (qpcSeconds());
        riskyGapsWindow.reset (qpcSeconds());

        hr = audioClient->Start();
        if (FAILED (hr))
            return false;

        return true;
    };

    auto reopenLowLatency = [&] (int requestedPeriod) -> bool
    {
        if (audioClient != nullptr)
            audioClient->Stop();

        SAFE_RELEASE (renderClient);
        SAFE_RELEASE (audioClient);

        hr = activateClient();
        if (FAILED (hr) || audioClient == nullptr)
        {
            if (SUCCEEDED (hr))
                hr = E_POINTER;
            return false;
        }

        IAudioClient3* client3 = nullptr;
        hr = audioClient->QueryInterface (__uuidof (IAudioClient3),
                                          reinterpret_cast<void**> (&client3));

        if (FAILED (hr) || client3 == nullptr)
        {
            // A success with no interface would otherwise reach the caller as
            // hr == S_OK and the error banner would read "0x0".
            if (SUCCEEDED (hr))
                hr = E_NOINTERFACE;

            SAFE_RELEASE (client3);
            return false;
        }

        UINT32 sharedDefault = 0, fundamental = 0, sharedMin = 0, sharedMax = 0;
        hr = client3->GetSharedModeEnginePeriod ((WAVEFORMATEX*) mixFormat,
                                                 &sharedDefault, &fundamental,
                                                 &sharedMin, &sharedMax);
        if (FAILED (hr) || sharedMin == 0)
        {
            if (SUCCEEDED (hr))
                hr = AUDCLNT_E_UNSUPPORTED_FORMAT;

            client3->Release();
            return false;
        }

        const int allowed = PeriodPolicy::initialPeriod ((int) sharedMin, (int) fundamental,
                                                         (int) sharedMax, requestedPeriod);

        hr = client3->InitializeSharedAudioStream (AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                   (UINT32) allowed,
                                                   (WAVEFORMATEX*) mixFormat, nullptr);
        client3->Release();

        if (FAILED (hr))
            return false;

        hr = audioClient->SetEventHandle (renderEvent);
        if (FAILED (hr))
            return false;

        hr = audioClient->GetService (__uuidof (IAudioRenderClient),
                                      reinterpret_cast<void**> (&renderClient));
        if (FAILED (hr) || renderClient == nullptr)
        {
            if (SUCCEEDED (hr))
                hr = E_POINTER;
            return false;
        }

        hr = audioClient->GetBufferSize (&bufferSize);
        if (FAILED (hr) || bufferSize == 0)
        {
            if (SUCCEEDED (hr))
                hr = E_POINTER;
            return false;
        }

        mode              = OutputMode::sharedLowLatency;
        kind              = PcmKind::float32;
        deviceSampleRate  = nativeSampleRate;
        deviceChannels    = (int) mixFormat->nChannels;
        frameSize         = (int) mixFormat->nBlockAlign;
        sharedMinPeriod   = (int) sharedMin;
        sharedFundamental = (int) fundamental;
        sharedMaxPeriod   = (int) sharedMax;
        sharedPeriod      = allowed;
        periodFrames      = allowed;

        return finishReopen();
    };

    auto reopenStandardShared = [&]() -> bool
    {
        if (audioClient != nullptr)
            audioClient->Stop();

        SAFE_RELEASE (renderClient);
        SAFE_RELEASE (audioClient);

        hr = activateClient();
        if (FAILED (hr) || audioClient == nullptr)
        {
            if (SUCCEEDED (hr))
                hr = E_POINTER;
            return false;
        }

        REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
        hr = audioClient->GetDevicePeriod (&defaultPeriod, &minPeriod);
        if (FAILED (hr))
            return false;

        DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (defaultSharedChoice.convert)
            streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                         | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        hr = audioClient->Initialize (AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                      defaultPeriod, 0, defaultSharedFormat, nullptr);
        if (FAILED (hr))
            return false;

        hr = audioClient->SetEventHandle (renderEvent);
        if (FAILED (hr))
            return false;

        hr = audioClient->GetService (__uuidof (IAudioRenderClient),
                                      reinterpret_cast<void**> (&renderClient));
        if (FAILED (hr) || renderClient == nullptr)
        {
            if (SUCCEEDED (hr))
                hr = E_POINTER;
            return false;
        }

        hr = audioClient->GetBufferSize (&bufferSize);
        if (FAILED (hr) || bufferSize == 0)
        {
            if (SUCCEEDED (hr))
                hr = E_POINTER;
            return false;
        }

        mode             = OutputMode::shared;
        kind             = PcmKind::float32;
        deviceSampleRate = defaultSharedChoice.rate;
        deviceChannels   = (int) defaultSharedFormat->nChannels;
        frameSize        = (int) defaultSharedFormat->nBlockAlign;
        periodFrames     = juce::jmax (1, (int) (defaultPeriod * deviceSampleRate
                                                 / 10000000));
        sharedPeriod     = 0;

        return finishReopen();
    };

    // ---- The real-time loop ----
    enum class LoopExit { stopped, failed, escalate };

    auto runRenderLoop = [&]() -> LoopExit
    {
        const double lateThresholdMs = 2.0 * periodMs;

        // The buffer duration, not the period, decides whether a wake-up was
        // audible: a 15 ms stall on a 5.8 ms endpoint buffer glitches the
        // cable even though the next event still finds some padding. Keep
        // half a millisecond of margin for an event that arrives just as the
        // buffer runs dry.
        const double coverMs = juce::jmax (0.0, 1000.0 * (double) bufferSize
                                                  / (double) deviceSampleRate - 0.5);

        // Liveness. A stream can keep running=true while nothing reaches the
        // cable: the events stop, or every GetCurrentPadding/GetBuffer fails.
        // Both are published as "stream stalled" after half a second so the
        // message thread can retry, instead of a banner-free dead output.
        constexpr double stallSeconds = 0.5;
        double lastRenderEventSeconds = qpcSeconds();
        double bufferFailureSinceSeconds = -1.0;

        const auto bufferFailed = [&]() noexcept -> bool
        {
            const double now = qpcSeconds();

            if (bufferFailureSinceSeconds < 0.0)
                bufferFailureSinceSeconds = now;

            return now - bufferFailureSinceSeconds >= stallSeconds;
        };

        while (true)
        {
            // Wake often enough to notice a silent endpoint inside 500 ms. A
            // stop costs at most this wait, which is inaudible for a shutdown.
            DWORD result = WaitForMultipleObjects (2, events, FALSE, 100);

            if (result == WAIT_OBJECT_0)
            {
                // Stop event signalled.
                return LoopExit::stopped;
            }

            if (result == WAIT_OBJECT_0 + 1)
            {
                // Render event: WASAPI wants more audio data.
                LARGE_INTEGER eventQpc {};
                QueryPerformanceCounter (&eventQpc);

                // One timestamp for the liveness check and both escalation
                // windows: a second QPC read would make the two disagree.
                const double eventSeconds = qpcFrequency.QuadPart > 0
                                              ? (double) eventQpc.QuadPart
                                                  / (double) qpcFrequency.QuadPart
                                              : 0.0;
                lastRenderEventSeconds = eventSeconds;

                // A late event is not evidence of the steady request: it can
                // ask for a whole buffer after a stall. Feeding that to the
                // backlog would raise the target for the rest of the session.
                bool eventLate = false;
                bool riskyGapTriggered = false;

                // The first event only starts the clock: there is no previous
                // render event to measure a gap against, and the interval since
                // the loop began includes thread setup.
                if (havePreviousEvent)
                {
                    const double gapMs = qpcFrequency.QuadPart > 0
                                           ? 1000.0 * (double) (eventQpc.QuadPart - previousEventQpc.QuadPart)
                                               / (double) qpcFrequency.QuadPart
                                           : 0.0;

                    if (gapMs > maxGapMs)
                    {
                        maxGapMs = gapMs;
                        maxRenderGapMs.store (maxGapMs, std::memory_order_relaxed);
                    }

                    if (gapMs > lateThresholdMs)
                    {
                        eventLate = true;
                        lateEventCount.store (++lateEvents, std::memory_order_relaxed);
                    }

                    // A risky gap is counted even when the endpoint still has
                    // padding afterwards: the cable already played silence for
                    // the part of the gap the buffer could not hold. Only the
                    // low-latency stream can escalate, so the window is fed
                    // only there; the counters stay honest for every mode.
                    if (gapMs > coverMs)
                    {
                        riskyGapCount.store (++riskyGaps, std::memory_order_relaxed);

                        if (gapMs > biggestRiskyGapMs)
                        {
                            biggestRiskyGapMs = gapMs;
                            maxRiskyGapMs.store (biggestRiskyGapMs,
                                                 std::memory_order_relaxed);
                        }

                        if (mode == OutputMode::sharedLowLatency)
                            riskyGapTriggered = riskyGapsWindow.record (eventSeconds, gapMs);
                    }
                }
                else
                {
                    havePreviousEvent = true;
                }

                previousEventQpc = eventQpc;

                UINT32 padding = 0;
                hr = audioClient->GetCurrentPadding (&padding);

                if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                {
                    setError (ErrorCode::deviceDisconnected);
                    return LoopExit::failed;
                }

                if (FAILED (hr))
                {
                    if (bufferFailed())
                    {
                        setError (ErrorCode::streamStalled);
                        return LoopExit::failed;
                    }

                    continue;
                }

                bufferFailureSinceSeconds = -1.0;

                // The current stream's buffer/period ratio is what turns a
                // wake-up gap into the period that would have covered it.
                const double bufferToPeriodRatio =
                    (double) bufferSize / (double) juce::jmax (1, sharedPeriod);

                // An empty endpoint at a render event means it played everything
                // it had since the last one. This is the glitch signal: the
                // late-event counter misses a gap that still left a little
                // padding, but a dry endpoint is audible.
                if (padding == 0)
                {
                    starvedEventCount.fetch_add (1, std::memory_order_relaxed);

                    if (mode == OutputMode::sharedLowLatency
                        && starvation.record (eventSeconds))
                    {
                        // No gap evidence here: one period step is the old
                        // starvation rule, and 0 at the maximum period means
                        // the standard shared rung.
                        escalateToPeriod = PeriodPolicy::escalationTarget (
                            0.0, (double) deviceSampleRate, bufferToPeriodRatio,
                            sharedPeriod, sharedMinPeriod, sharedFundamental,
                            sharedMaxPeriod);
                        escalationCause = EscalationCause::starved;
                        escalationGapMs = 0.0;
                        return LoopExit::escalate;
                    }
                }

                // Two stalls the buffer could not cover: pick the period from
                // the largest of them so the next launch and the rest of this
                // session stop glitching on the same length of stall.
                if (riskyGapTriggered)
                {
                    escalationGapMs = riskyGapsWindow.largestWithin (eventSeconds);
                    escalateToPeriod = PeriodPolicy::escalationTarget (
                        escalationGapMs, (double) deviceSampleRate, bufferToPeriodRatio,
                        sharedPeriod, sharedMinPeriod, sharedFundamental, sharedMaxPeriod);
                    escalationCause = EscalationCause::riskyGap;
                    return LoopExit::escalate;
                }

                // Exclusive mode is opened at exactly the endpoint buffer
                // length, so one event frees the whole buffer and the whole
                // buffer is written, as JUCE does. Shared streams fill only
                // what the endpoint is not already holding.
                UINT32 available = bufferSize;

                if (mode != OutputMode::exclusive)
                {
                    if (padding >= bufferSize)
                        continue;

                    available = bufferSize - padding;
                }

                if (available == 0)
                    continue;

                BYTE* data = nullptr;
                hr = renderClient->GetBuffer (available, &data);

                if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                {
                    setError (ErrorCode::deviceDisconnected);
                    return LoopExit::failed;
                }

                if (FAILED (hr) || data == nullptr)
                {
                    bufferErrorCount.store (++bufferErrors, std::memory_order_relaxed);

                    if (bufferFailed())
                    {
                        setError (ErrorCode::streamStalled);
                        return LoopExit::failed;
                    }

                    continue;
                }

                bufferFailureSinceSeconds = -1.0;

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
                // holds; the nominal ratio converts from device frames. The block
                // and the ratio are refreshed together in case the engine changed.
                backlog.setProducerFormat (producerBlockSize.load (std::memory_order_relaxed),
                                           currentProducerRate);

                // The first events after Start() can ask for a whole buffer while
                // the endpoint is still priming; treating that as the steady
                // request would raise the target for the whole session. A late
                // event is the same trap for the same reason.
                if (++eventsSeen > 3 && ! eventLate)
                    backlog.onRequest (framesToRead);

                const int targetFrames = (int) std::lround (backlog.getTargetFrames() * nominalRatio);
                reader.setTargetFill (targetFrames);
                backlogTargetFrames.store (targetFrames, std::memory_order_relaxed);

                // Always fills all framesToRead frames: audio, or silence while
                // the ring is priming after start-up or an underrun.
                reader.read (*buffer, tempChannelPtrs.data(), deviceChannels,
                             framesToRead, nominalRatio);
                underrunCount.store (reader.getUnderrunCount(), std::memory_order_relaxed);
                resyncCount.store (reader.getResyncCount(), std::memory_order_relaxed);

                // Interleave the per-channel data into the output buffer, in the
                // format the stream actually accepted.
                // The limiter runs before the reader's interpolation, which
                // can overshoot a full-scale input by about 0.9 dB. This is
                // the last point before the endpoint, so the clamp lives here
                // for float and integer formats alike.
                if (kind == PcmKind::float32)
                {
                    float* outputFloat = reinterpret_cast<float*> (data);

                    for (int i = 0; i < framesToRead; ++i)
                        for (int ch = 0; ch < deviceChannels; ++ch)
                            outputFloat[i * deviceChannels + ch] =
                                OutputCeiling::clamp (tempChannelPtrs[(size_t) ch][i]);
                }
                else if (kind == PcmKind::pcm16)
                {
                    auto* outputPcm = reinterpret_cast<int16_t*> (data);

                    for (int i = 0; i < framesToRead; ++i)
                        for (int ch = 0; ch < deviceChannels; ++ch)
                            outputPcm[i * deviceChannels + ch] =
                                toPcm16 (OutputCeiling::clamp (tempChannelPtrs[(size_t) ch][i]));
                }
                else
                {
                    auto* outputPcm = reinterpret_cast<int32_t*> (data);

                    for (int i = 0; i < framesToRead; ++i)
                        for (int ch = 0; ch < deviceChannels; ++ch)
                            outputPcm[i * deviceChannels + ch] =
                                toPcm24In32 (OutputCeiling::clamp (tempChannelPtrs[(size_t) ch][i]));
                }

                if (FAILED (renderClient->ReleaseBuffer (available, 0)))
                    bufferErrorCount.store (++bufferErrors, std::memory_order_relaxed);
            }
            else if (result == WAIT_TIMEOUT)
            {
                // A stream whose events stopped (audio service restart, another
                // application taking the endpoint, a driver gone quiet) keeps
                // running=true while nothing reaches the cable. Publish the
                // stall so the message thread retries the output.
                if (qpcSeconds() - lastRenderEventSeconds >= stallSeconds)
                {
                    setError (ErrorCode::streamStalled);
                    return LoopExit::failed;
                }
            }
            else
            {
                // WAIT_FAILED or other unexpected return.
                setError (ErrorCode::waitFailed, (int) GetLastError());
                return LoopExit::failed;
            }
        }
    };

    LoopExit exitReason = runRenderLoop();

    while (exitReason == LoopExit::escalate)
    {
        escalationCount.fetch_add (1, std::memory_order_relaxed);

        const int oldPeriod = sharedPeriod;
        const int oldBufferFrames = (int) bufferSize;
        const double oldPeriodMs = periodMs;
        const double oldBufferMs = 1000.0 * (double) oldBufferFrames / deviceSampleRate;
        const bool byRiskyGap = escalationCause == EscalationCause::riskyGap;
        const double triggerGapMs = escalationGapMs;

        // A larger low-latency period first. If even that cannot be opened,
        // the standard shared rung is still better than no audio at all, and
        // the hint saved afterwards keeps the next launch on that rung.
        bool reopened = false;

        if (escalateToPeriod > 0)
            reopened = reopenLowLatency (escalateToPeriod);

        if (! reopened)
            reopened = reopenStandardShared();

        if (! reopened)
        {
            setError (ErrorCode::initializeAudioClient, (int) hr);
            exitReason = LoopExit::failed;
            break;
        }

        // After leaving the loop and before re-entering it: the only place an
        // escalation may touch the log. The new buffer matters as much as the
        // new period, because the buffer duration is what has to cover the
        // next wake-up.
        StartupLog::mark (
            juce::String ("output: escalated (") + (byRiskyGap ? "risky gap" : "starved")
            + "), period " + juce::String (oldPeriod) + " frames ("
            + juce::String (oldPeriodMs, 2) + " ms) -> "
            + (mode == OutputMode::sharedLowLatency
                 ? juce::String (periodFrames) + " frames ("
                     + juce::String (periodMs, 2) + " ms)"
                 : juce::String ("standard shared (")
                     + juce::String (periodMs, 2) + " ms)")
            + ", buffer " + juce::String (oldBufferFrames) + " frames ("
            + juce::String (oldBufferMs, 2) + " ms) -> " + juce::String ((int) bufferSize)
            + " frames (" + juce::String (1000.0 * bufferSize / deviceSampleRate, 2) + " ms)"
            + (byRiskyGap ? ", gap " + juce::String (triggerGapMs, 1) + " ms" : juce::String())
            + " (starved=" + juce::String (starvedEventCount.load (std::memory_order_relaxed))
            + " risky=" + juce::String (riskyGapCount.load (std::memory_order_relaxed)) + ")");

        exitReason = runRenderLoop();
    }

    // ---- Shutdown ----
    if (mmcssHandle != nullptr)
        AvRevertMmThreadCharacteristics (mmcssHandle);

    mmcssActive.store (false, std::memory_order_relaxed);

    if (audioClient != nullptr)
        audioClient->Stop();

    cleanup();
    running.store (false);
}
