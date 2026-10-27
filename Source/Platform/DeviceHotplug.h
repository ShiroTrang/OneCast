#pragma once
#include <JuceHeader.h>
#include <Windows.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <memory>

/// Windows audio endpoint notifications (IMMNotificationClient), delivered to
/// the listeners on the message thread.
///
/// A USB unplug or plug usually arrives as deviceStateChanged() for each of
/// the interface's endpoints (speakers, microphone...), often with
/// defaultDeviceChanged() alongside, so one physical event is a burst of
/// several notifications: listeners should debounce. ASIO drivers do not
/// appear here, but the same interface's Windows endpoints do.
class DeviceHotplug : private IMMNotificationClient
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void deviceAdded (const juce::String& deviceId) {}
        virtual void deviceRemoved (const juce::String& deviceId) {}
        virtual void deviceStateChanged (const juce::String& deviceId, DWORD newState) {}
        virtual void defaultDeviceChanged (EDataFlow flow, ERole role, const juce::String& newDeviceId) {}
    };

    DeviceHotplug();
    ~DeviceHotplug();

    /// Register for notifications. Call on the message thread (COM is already
    /// initialised there by JUCE). Returns false if Windows refused.
    bool start();
    void stop();

    void addListener (Listener* listener);
    void removeListener (Listener* listener);

private:
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged (LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded (LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved (LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged (EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged (LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;

    /// Hand a notification to the listeners on the message thread.
    template <typename Notify>
    void post (Notify&& notify);

    IMMDeviceEnumerator* enumerator = nullptr;
    std::atomic<LONG> refCount { 1 };
    juce::ListenerList<Listener> listeners;
    bool registered = false;

    /// True between start() and stop(). Shared with the posted messages, which
    /// can run after this object is gone: they check it before touching it.
    const std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (false);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceHotplug)
};
