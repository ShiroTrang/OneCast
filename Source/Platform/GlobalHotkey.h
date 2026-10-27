#pragma once
#include <JuceHeader.h>
#include <Windows.h>
#include <functional>
#include <optional>

/// A system-wide hotkey (RegisterHotKey), used for the global mute shortcut.
///
/// The key works whichever application has the focus, and is delivered to a
/// hidden message-only window, so the owner needs no visible window and no
/// key handling of its own. MOD_NOREPEAT is always set: holding the keys down
/// presses once, so mute does not flicker.
///
/// Construct, use and destroy it on the message thread: the window belongs to
/// the thread that created it and the hotkey is delivered through its message
/// queue. Windows gives a combination to one owner only, across processes and
/// inside this one, so a second GlobalHotkey asking for the same combination
/// is refused.
class GlobalHotkey
{
public:
    /// modifiers is a mix of MOD_CONTROL, MOD_ALT, MOD_SHIFT and MOD_WIN,
    /// virtualKey a VK_* code (or the character code for A-Z and 0-9).
    struct Combo { unsigned modifiers = 0; unsigned virtualKey = 0; };

    /// Reads "Ctrl+Alt+M" style text: case-insensitive, "+" separated, spaces
    /// around the separators ignored. Needs at least one modifier (Ctrl, Alt,
    /// Shift, Win, each at most once) and exactly one key out of A-Z, 0-9,
    /// F1-F24, Pause, ScrollLock, Insert, Home, End, PageUp, PageDown, in any
    /// order. A bare key is refused on purpose: it would swallow that key in
    /// every application.
    static std::optional<Combo> parse (const juce::String& text);

    /// Canonical text, modifiers in the order Ctrl, Alt, Shift, Win and then
    /// the key ("Ctrl+Shift+F12"). Empty when parse() would not accept the
    /// combo (no modifier, or a key outside the supported set).
    static juce::String format (Combo combo);

    GlobalHotkey();
    ~GlobalHotkey();

    /// Registers the combination, replacing the one held now. Returns false
    /// when the text does not parse or another owner has the combination; the
    /// previous hotkey then stays registered. Setting the combination already
    /// held succeeds without doing anything.
    bool setHotkey (const juce::String& text);

    /// Releases the hotkey, if any.
    void clear();

    /// The registered combination in canonical form, "" when there is none.
    juce::String getHotkey() const;

    /// Called on the message thread each time the hotkey is pressed. It may
    /// destroy this object.
    std::function<void()> onPressed;

private:
    HWND window = nullptr;
    std::optional<Combo> current;

    /// RegisterHotKey id of the current hotkey (1 or 2, 0 when none). A
    /// replacement registers under the other id first, so the old hotkey is
    /// only given up once the new one is secured.
    int activeId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlobalHotkey)
};
