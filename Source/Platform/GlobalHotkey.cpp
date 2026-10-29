#include "GlobalHotkey.h"

namespace
{
    constexpr unsigned allModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;

    /// Keys with a name of their own. A-Z, 0-9 and F1-F24 are derived instead.
    struct NamedKey { const char* name; unsigned virtualKey; };

    constexpr NamedKey namedKeys[] =
    {
        { "Pause",      VK_PAUSE  },
        { "ScrollLock", VK_SCROLL },
        { "Insert",     VK_INSERT },
        { "Home",       VK_HOME   },
        { "End",        VK_END    },
        { "PageUp",     VK_PRIOR  },
        { "PageDown",   VK_NEXT   },
    };

    /// Canonical modifier order, shared by parse() and format().
    struct NamedModifier { const char* name; unsigned flag; };

    constexpr NamedModifier namedModifiers[] =
    {
        { "Ctrl",  MOD_CONTROL },
        { "Alt",   MOD_ALT     },
        { "Shift", MOD_SHIFT   },
        { "Win",   MOD_WIN     },
    };

    constexpr int numFunctionKeys = 24;

    bool isAsciiLetter (juce::juce_wchar c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
    bool isAsciiDigit  (juce::juce_wchar c) { return c >= '0' && c <= '9'; }

    /// Splits at every '+', keeping empty pieces: "Ctrl+" and "Ctrl++A" are
    /// mistakes to report, not spacing to skip (StringArray::fromTokens would
    /// swallow them).
    juce::StringArray splitOnPlus (const juce::String& text)
    {
        juce::StringArray pieces;
        int start = 0;

        for (;;)
        {
            const int plus = text.indexOfChar (start, '+');

            if (plus < 0)
            {
                pieces.add (text.substring (start).trim());
                return pieces;
            }

            pieces.add (text.substring (start, plus).trim());
            start = plus + 1;
        }
    }

    std::optional<unsigned> parseModifier (const juce::String& token)
    {
        for (const auto& modifier : namedModifiers)
            if (token.equalsIgnoreCase (modifier.name))
                return modifier.flag;

        return std::nullopt;
    }

    std::optional<unsigned> parseKey (const juce::String& token)
    {
        // The ASCII checks come first: toUpperCase() would fold some non-ASCII
        // letters (a dotless i, say) onto A-Z.
        if (token.length() == 1)
        {
            const auto c = token[0];

            if (isAsciiLetter (c))  return (unsigned) juce::CharacterFunctions::toUpperCase (c);
            if (isAsciiDigit (c))   return (unsigned) c;
        }

        // F1..F24, written without a leading zero.
        if (token.length() >= 2 && token.length() <= 3 && (token[0] == 'F' || token[0] == 'f'))
        {
            const auto digits = token.substring (1);

            if (digits.containsOnly ("0123456789") && digits[0] != '0')
            {
                const int number = digits.getIntValue();

                if (number >= 1 && number <= numFunctionKeys)
                    return (unsigned) (VK_F1 + number - 1);
            }
        }

        for (const auto& key : namedKeys)
            if (token.equalsIgnoreCase (key.name))
                return key.virtualKey;

        return std::nullopt;
    }

    /// Empty for a key parseKey() would not produce.
    juce::String keyName (unsigned virtualKey)
    {
        if ((virtualKey >= 'A' && virtualKey <= 'Z') || (virtualKey >= '0' && virtualKey <= '9'))
            return juce::String::charToString ((juce::juce_wchar) virtualKey);

        if (virtualKey >= VK_F1 && virtualKey < VK_F1 + numFunctionKeys)
            return "F" + juce::String ((int) (virtualKey - VK_F1) + 1);

        for (const auto& key : namedKeys)
            if (key.virtualKey == virtualKey)
                return key.name;

        return {};
    }

    bool sameCombo (const GlobalHotkey::Combo& a, const GlobalHotkey::Combo& b)
    {
        return a.modifiers == b.modifiers && a.virtualKey == b.virtualKey;
    }

    /// Window procedure of the hidden window. Only WM_HOTKEY matters, and every
    /// WM_HOTKEY this window gets is ours (it registers nothing else), so the
    /// id is not checked: one queued just before a replacement is still a
    /// press of the same shortcut.
    LRESULT CALLBACK hotkeyWindowProc (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*> (lParam);
            SetWindowLongPtrW (hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (create->lpCreateParams));
        }
        else if (message == WM_HOTKEY)
        {
            if (auto* self = reinterpret_cast<GlobalHotkey*> (GetWindowLongPtrW (hwnd, GWLP_USERDATA)))
            {
                // Call a copy: the callback is allowed to destroy the
                // GlobalHotkey, and with it the function it is running from.
                if (auto callback = self->onPressed)
                    callback();
            }

            return 0;
        }

        return DefWindowProcW (hwnd, message, wParam, lParam);
    }

    /// The window class is registered once per process and kept until it ends.
    const wchar_t* registerWindowClass()
    {
        static const wchar_t* const className = []() -> const wchar_t*
        {
            WNDCLASSEXW windowClass {};
            windowClass.cbSize        = sizeof (windowClass);
            windowClass.lpfnWndProc   = hotkeyWindowProc;
            windowClass.hInstance     = GetModuleHandleW (nullptr);
            windowClass.lpszClassName = L"OneCastGlobalHotkeyWindow";

            return RegisterClassExW (&windowClass) != 0 ? windowClass.lpszClassName : nullptr;
        }();

        return className;
    }
}

//==============================================================================
std::optional<GlobalHotkey::Combo> GlobalHotkey::parse (const juce::String& text)
{
    Combo combo;
    bool haveKey = false;

    for (const auto& token : splitOnPlus (text))
    {
        if (const auto modifier = parseModifier (token))
        {
            if ((combo.modifiers & *modifier) != 0)
                return std::nullopt;   // the same modifier twice

            combo.modifiers |= *modifier;
        }
        else if (const auto key = parseKey (token))
        {
            if (haveKey)
                return std::nullopt;   // two keys

            combo.virtualKey = *key;
            haveKey = true;
        }
        else
        {
            return std::nullopt;       // empty or unknown token
        }
    }

    if (! haveKey || combo.modifiers == 0)
        return std::nullopt;

    return combo;
}

juce::String GlobalHotkey::format (Combo combo)
{
    const auto key = keyName (combo.virtualKey);

    if (key.isEmpty() || (combo.modifiers & allModifiers) == 0)
        return {};

    juce::String text;

    for (const auto& modifier : namedModifiers)
        if ((combo.modifiers & modifier.flag) != 0)
            text << modifier.name << '+';

    return text + key;
}

//==============================================================================
GlobalHotkey::GlobalHotkey()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (const auto* className = registerWindowClass())
    {
        // HWND_MESSAGE: no visible window, no z-order, only a message queue.
        window = CreateWindowExW (0, className, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, GetModuleHandleW (nullptr), this);
    }

    jassert (window != nullptr);
}

GlobalHotkey::~GlobalHotkey()
{
    JUCE_ASSERT_MESSAGE_THREAD

    clear();

    if (window != nullptr)
        DestroyWindow (window);
}

bool GlobalHotkey::setHotkey (const juce::String& text)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto combo = parse (text);

    if (! combo.has_value() || window == nullptr)
        return false;

    if (current.has_value() && sameCombo (*current, *combo))
        return true;

    // The old hotkey stays registered until the new one is secured, so a
    // refused replacement loses nothing.
    const int newId = (activeId == 1) ? 2 : 1;

    if (! RegisterHotKey (window, newId, combo->modifiers | MOD_NOREPEAT, combo->virtualKey))
        return false;

    if (activeId != 0)
        UnregisterHotKey (window, activeId);

    activeId = newId;
    current = combo;
    return true;
}

void GlobalHotkey::clear()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (activeId != 0)
        UnregisterHotKey (window, activeId);

    activeId = 0;
    current.reset();
}

juce::String GlobalHotkey::getHotkey() const
{
    return current.has_value() ? format (*current) : juce::String();
}
