#pragma once

/** Whether the options callout opens with its invalid-shortcut warning.

    The field deliberately keeps the saved text when launch registration failed
    (invalid, or another application owns the combination), so the text alone
    cannot say whether the mute key really works. The panel has to combine the
    saved text with the registration result, and getting that rule backwards
    would either hide a real failure or show the warning on every healthy
    launch. Keeping the decision here lets the tests cover it without a window
    or an audio device.
*/
namespace HotkeyStatus
{
    inline bool invalidNoticeVisible (bool hasSavedHotkey, bool savedHotkeyRegistered)
    {
        // No saved text means no shortcut at all, which is not an error; text
        // that Windows did not hand back is the failure the label explains.
        return hasSavedHotkey && ! savedHotkeyRegistered;
    }
}
