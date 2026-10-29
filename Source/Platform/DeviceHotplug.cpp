#include "DeviceHotplug.h"

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if ((p) != nullptr) { (p)->Release(); (p) = nullptr; } } while (0)
#endif

//==============================================================================
DeviceHotplug::DeviceHotplug() {}

DeviceHotplug::~DeviceHotplug()
{
    stop();
}

//==============================================================================
bool DeviceHotplug::start()
{
    if (registered)
        return true;

    HRESULT hr = CoCreateInstance (__uuidof (MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  __uuidof (IMMDeviceEnumerator),
                                  reinterpret_cast<void**> (&enumerator));

    if (FAILED (hr) || enumerator == nullptr)
    {
        DBG ("DeviceHotplug: failed to create IMMDeviceEnumerator (HRESULT 0x"
             + juce::String::toHexString (static_cast<int> (hr)) + ")");
        return false;
    }

    // Before registering: a notification can arrive as soon as it is in, and
    // a stop() -> start() must not keep swallowing them.
    alive->store (true, std::memory_order_release);

    hr = enumerator->RegisterEndpointNotificationCallback (this);

    if (FAILED (hr))
    {
        DBG ("DeviceHotplug: failed to register notification callback (HRESULT 0x"
             + juce::String::toHexString (static_cast<int> (hr)) + ")");
        alive->store (false, std::memory_order_release);
        SAFE_RELEASE (enumerator);
        return false;
    }

    registered = true;
    return true;
}

void DeviceHotplug::stop()
{
    // Messages already posted must not reach the listeners any more.
    alive->store (false, std::memory_order_release);

    if (registered)
    {
        enumerator->UnregisterEndpointNotificationCallback (this);
        registered = false;
    }

    SAFE_RELEASE (enumerator);
}

//==============================================================================
void DeviceHotplug::addListener (Listener* listener)
{
    listeners.add (listener);
}

void DeviceHotplug::removeListener (Listener* listener)
{
    listeners.remove (listener);
}

//==============================================================================
// IUnknown
//==============================================================================
HRESULT STDMETHODCALLTYPE DeviceHotplug::QueryInterface (REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr)
        return E_POINTER;

    if (riid == IID_IUnknown || riid == __uuidof (IMMNotificationClient))
    {
        *ppvObject = static_cast<IMMNotificationClient*> (this);
        AddRef();
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeviceHotplug::AddRef()
{
    return static_cast<ULONG> (++refCount);
}

ULONG STDMETHODCALLTYPE DeviceHotplug::Release()
{
    LONG count = --refCount;
    // Do NOT delete this - the object is owned by the application, not by COM.
    return static_cast<ULONG> (count);
}

//==============================================================================
// IMMNotificationClient callbacks
// These are called on a Windows thread-pool thread. All listener dispatch
// is posted to the JUCE message thread via MessageManager::callAsync.
//==============================================================================
template <typename Notify>
void DeviceHotplug::post (Notify&& notify)
{
    // The message carries its own reference to the alive flag: it may be
    // delivered after this object was destroyed, and must not read it then.
    juce::MessageManager::callAsync ([this, token = alive, notify = std::forward<Notify> (notify)]()
    {
        if (token->load (std::memory_order_acquire))
            listeners.call (notify);
    });
}

static juce::String idToString (LPCWSTR id)
{
    return juce::String (id != nullptr ? id : L"");
}

HRESULT STDMETHODCALLTYPE DeviceHotplug::OnDeviceAdded (LPCWSTR pwstrDeviceId)
{
    post ([id = idToString (pwstrDeviceId)] (Listener& l) { l.deviceAdded (id); });
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceHotplug::OnDeviceRemoved (LPCWSTR pwstrDeviceId)
{
    post ([id = idToString (pwstrDeviceId)] (Listener& l) { l.deviceRemoved (id); });
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceHotplug::OnDeviceStateChanged (LPCWSTR pwstrDeviceId, DWORD dwNewState)
{
    post ([id = idToString (pwstrDeviceId), dwNewState] (Listener& l) { l.deviceStateChanged (id, dwNewState); });
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceHotplug::OnDefaultDeviceChanged (EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId)
{
    // pwstrDefaultDeviceId can be null when no device is available for the given flow/role.
    post ([id = idToString (pwstrDefaultDeviceId), flow, role] (Listener& l) { l.defaultDeviceChanged (flow, role, id); });
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceHotplug::OnPropertyValueChanged (LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/)
{
    // Property changes are frequent and noisy (volume, format changes, etc.).
    // No action needed for device hot-plug detection.
    return S_OK;
}
