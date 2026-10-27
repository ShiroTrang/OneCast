// Unit tests for the Windows platform helpers: the global mute hotkey and the
// start-with-Windows registry entry. Neither needs an audio device.
//
//   cmake --build build --config Release --target OneCastTests
//   build\OneCastTests_artefacts\Release\OneCastTests.exe platform
//
// The registry test only ever touches the value "OneCastTest" under
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run, never the real "OneCast"
// entry, and removes it again whatever the outcome. The hotkey tests register
// Ctrl+Alt+Shift+F22..F24, combinations no application is likely to own; a
// machine that already uses one of them makes the registration tests fail
// (on the development machine another program already holds F13..F21).

#include <JuceHeader.h>

#include "../Source/Platform/GlobalHotkey.h"
#include "../Source/Platform/AutoStart.h"

#include <memory>
#include <vector>

namespace
{
    constexpr unsigned allModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;

    /// The class of the hidden window GlobalHotkey owns. The dispatch test finds
    /// the window by it and sends WM_HOTKEY, since pressing a real key is out
    /// of reach of a unit test.
    constexpr const wchar_t* hotkeyWindowClass = L"OneCastGlobalHotkeyWindow";

    HWND findHotkeyWindow()
    {
        return FindWindowExW (HWND_MESSAGE, nullptr, hotkeyWindowClass, nullptr);
    }

    bool matches (const std::optional<GlobalHotkey::Combo>& combo, unsigned modifiers, unsigned virtualKey)
    {
        return combo.has_value() && combo->modifiers == modifiers && combo->virtualKey == virtualKey;
    }

    const juce::String runKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const juce::String testValueName = "OneCastTest";

    /// Removes the test value in the destructor, so a failed expectation (or a
    /// value left behind by an earlier crashed run) never leaves it in the
    /// registry. Goes through the registry directly, not through AutoStart, so
    /// the clean-up does not depend on the code under test.
    struct TestValueGuard
    {
        TestValueGuard()  { remove(); }
        ~TestValueGuard() { remove(); }

        static juce::String path() { return runKey + "\\" + testValueName; }

        static void remove()
        {
            if (juce::WindowsRegistry::valueExists (path()))
                juce::WindowsRegistry::deleteValue (path());
        }
    };
}

//==============================================================================
class GlobalHotkeyTests final : public juce::UnitTest
{
public:
    GlobalHotkeyTests() : juce::UnitTest ("GlobalHotkey", "platform") {}

    void runTest() override
    {
        beginTest ("hotkey parse and format");
        {
            auto combo = GlobalHotkey::parse ("ctrl+alt+m");
            expect (matches (combo, MOD_CONTROL | MOD_ALT, 'M'));
            if (combo)
                expectEquals (GlobalHotkey::format (*combo), juce::String ("Ctrl+Alt+M"));

            combo = GlobalHotkey::parse ("Shift+Ctrl+F12");
            expect (matches (combo, MOD_CONTROL | MOD_SHIFT, VK_F12));
            if (combo)
                expectEquals (GlobalHotkey::format (*combo), juce::String ("Ctrl+Shift+F12"));

            // Canonical order is Ctrl, Alt, Shift, Win, whatever the input order.
            combo = GlobalHotkey::parse ("Win+Shift+Alt+Ctrl+Pause");
            expect (matches (combo, allModifiers, VK_PAUSE));
            if (combo)
                expectEquals (GlobalHotkey::format (*combo), juce::String ("Ctrl+Alt+Shift+Win+Pause"));

            // Spaces around the separators are tolerated, the key name is not
            // case sensitive.
            combo = GlobalHotkey::parse ("  CTRL + pagedown ");
            expect (matches (combo, MOD_CONTROL, VK_NEXT));
            if (combo)
                expectEquals (GlobalHotkey::format (*combo), juce::String ("Ctrl+PageDown"));

            expect (matches (GlobalHotkey::parse ("Alt+7"), MOD_ALT, '7'));
            expect (matches (GlobalHotkey::parse ("Alt+F1"), MOD_ALT, VK_F1));
            expect (matches (GlobalHotkey::parse ("Alt+F24"), MOD_ALT, VK_F24));
        }

        beginTest ("hotkey parse rejects what it cannot register");
        {
            const char* const rejected[] =
            {
                "",              // nothing
                "M",             // no modifier
                "F5",            // no modifier
                "Ctrl",          // no key
                "Ctrl+Alt",      // no key
                "Ctrl+",         // trailing separator
                "+M",            // leading separator
                "Ctrl++M",       // empty token
                "Ctrl+Foo",      // unknown key
                "Ctrl+A+B",      // two keys
                "Ctrl+Ctrl+A",   // modifier given twice
                "Ctrl+F0",       // F-keys are F1..F24
                "Ctrl+F25",
                "Ctrl+F01",
                "Ctrl+F-1",
                "Ctrl+AB",
                "Ctrl+Space",    // not in the supported key set
                "Ctrl+Enter",
            };

            for (const char* text : rejected)
                expect (! GlobalHotkey::parse (text).has_value(),
                        "parse should reject \"" + juce::String (text) + "\"");

            // Non-ASCII letters must not be folded onto A-Z.
            expect (! GlobalHotkey::parse (juce::String (juce::CharPointer_UTF8 ("Ctrl+\xC4\xB1"))).has_value());
        }

        beginTest ("hotkey format round-trips every supported key");
        {
            std::vector<unsigned> keys;
            for (unsigned k = 'A'; k <= 'Z'; ++k)       keys.push_back (k);
            for (unsigned k = '0'; k <= '9'; ++k)       keys.push_back (k);
            for (unsigned k = VK_F1; k <= VK_F24; ++k)  keys.push_back (k);
            for (unsigned k : { (unsigned) VK_PAUSE, (unsigned) VK_SCROLL, (unsigned) VK_INSERT,
                                (unsigned) VK_HOME, (unsigned) VK_END, (unsigned) VK_PRIOR,
                                (unsigned) VK_NEXT })
                keys.push_back (k);

            expectEquals ((int) keys.size(), 26 + 10 + 24 + 7);

            for (unsigned key : keys)
            {
                const GlobalHotkey::Combo combo { MOD_CONTROL | MOD_WIN, key };
                const auto text = GlobalHotkey::format (combo);
                expect (text.isNotEmpty(), "format should name key " + juce::String ((int) key));
                expect (matches (GlobalHotkey::parse (text), combo.modifiers, combo.virtualKey),
                        "\"" + text + "\" should parse back to the same combo");
            }

            // Combos parse() would refuse have no text form.
            expect (GlobalHotkey::format ({ 0, 'M' }).isEmpty());
            expect (GlobalHotkey::format ({ MOD_CONTROL, VK_SPACE }).isEmpty());
            expect (GlobalHotkey::format ({}).isEmpty());
        }

        beginTest ("hotkey registration");
        {
            GlobalHotkey first, second;
            expect (first.getHotkey().isEmpty());

            expect (first.setHotkey ("Ctrl+Alt+Shift+F24"));
            expectEquals (first.getHotkey(), juce::String ("Ctrl+Alt+Shift+F24"));

            // Windows hands a combination to one owner only, even inside one process.
            expect (! second.setHotkey ("Ctrl+Alt+Shift+F24"));
            expect (second.getHotkey().isEmpty());

            first.clear();
            expect (first.getHotkey().isEmpty());

            expect (second.setHotkey ("ctrl+alt+shift+f24"));
            expectEquals (second.getHotkey(), juce::String ("Ctrl+Alt+Shift+F24"));
        }

        beginTest ("hotkey replacement keeps the old combo until the new one is taken");
        {
            const juce::String a = "Ctrl+Alt+Shift+F24", b = "Ctrl+Alt+Shift+F23", c = "Ctrl+Alt+Shift+F22";
            GlobalHotkey owner, other;

            expect (owner.setHotkey (a));
            expect (other.setHotkey (c));

            // Unparsable or taken: refused, and the old combo is still ours.
            expect (! owner.setHotkey ("Ctrl+Foo"));
            expect (! owner.setHotkey (""));
            expect (! owner.setHotkey (c));
            expectEquals (owner.getHotkey(), a);

            {
                GlobalHotkey rival;
                expect (! rival.setHotkey (a));   // still held
            }

            // Setting the combo already held is a no-op that succeeds.
            expect (owner.setHotkey (a));
            expectEquals (owner.getHotkey(), a);

            // A successful replacement frees the old combo and takes the new one.
            expect (owner.setHotkey (b));
            expectEquals (owner.getHotkey(), b);
            expect (! other.setHotkey (b));
            expectEquals (other.getHotkey(), c);

            {
                GlobalHotkey rival;
                expect (rival.setHotkey (a));     // freed by the replacement
                expect (! owner.setHotkey (a));   // and now somebody else's
                expectEquals (owner.getHotkey(), b);
            }

            // Destroying a hotkey frees its combo too.
            expect (owner.setHotkey (a));
            expect (other.setHotkey (b));
        }

        beginTest ("hotkey press calls onPressed on the message thread");
        {
            expect (findHotkeyWindow() == nullptr, "no hotkey window should be left over");

            auto hotkey = std::make_unique<GlobalHotkey>();
            const auto window = findHotkeyWindow();
            expect (window != nullptr, "GlobalHotkey should own a message-only window");

            // No callback set: a press is ignored.
            SendMessageW (window, WM_HOTKEY, 1, 0);

            int pressed = 0;
            bool onMessageThread = false;
            hotkey->onPressed = [&]
            {
                ++pressed;
                onMessageThread = juce::MessageManager::getInstance()->isThisTheMessageThread();
            };

            expect (hotkey->setHotkey ("Ctrl+Alt+Shift+F24"));
            SendMessageW (window, WM_HOTKEY, 1, 0);
            SendMessageW (window, WM_HOTKEY, 2, 0);   // the id changes on each replacement
            expectEquals (pressed, 2);
            expect (onMessageThread);

            // The callback may delete its owner (for example when it closes the
            // window that holds it): that must not crash.
            hotkey->onPressed = [&] { hotkey.reset(); };
            SendMessageW (window, WM_HOTKEY, 1, 0);
            expect (hotkey == nullptr);
            expect (findHotkeyWindow() == nullptr, "the window should be gone with its owner");

            // And the combo went with it.
            GlobalHotkey again;
            expect (again.setHotkey ("Ctrl+Alt+Shift+F24"));
        }
    }
};

//==============================================================================
class AutoStartTests final : public juce::UnitTest
{
public:
    AutoStartTests() : juce::UnitTest ("AutoStart", "platform") {}

    void runTest() override
    {
        beginTest ("command line quotes the executable and starts minimised");
        {
            const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
            expectEquals (AutoStart::commandLine(), "\"" + exe + "\" --minimized");
        }

        beginTest ("autostart");
        {
            // Every call below names OneCastTest: the real "OneCast" entry is
            // never read or written here.
            const TestValueGuard guard;

            expect (! AutoStart::isEnabled (testValueName));

            expect (AutoStart::setEnabled (true, testValueName));
            expect (AutoStart::isEnabled (testValueName));
            expect (juce::WindowsRegistry::valueExists (TestValueGuard::path()));
            expectEquals (juce::WindowsRegistry::getValue (TestValueGuard::path()), AutoStart::commandLine());

            // Enabling again just rewrites the value.
            expect (AutoStart::setEnabled (true, testValueName));
            expectEquals (juce::WindowsRegistry::getValue (TestValueGuard::path()), AutoStart::commandLine());

            expect (AutoStart::setEnabled (false, testValueName));
            expect (! AutoStart::isEnabled (testValueName));
            expect (! juce::WindowsRegistry::valueExists (TestValueGuard::path()));

            // Disabling what is not there is not an error.
            expect (AutoStart::setEnabled (false, testValueName));
            expect (! AutoStart::isEnabled (testValueName));
        }

        beginTest ("autostart leaves the test registry value removed");
        {
            expect (! juce::WindowsRegistry::valueExists (TestValueGuard::path()));
        }
    }
};

static GlobalHotkeyTests globalHotkeyTests;
static AutoStartTests    autoStartTests;
