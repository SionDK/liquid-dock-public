#pragma once
// Which of the stock Windows 11 taskbar's tray buttons is the "show hidden icons" chevron, from what
// UI Automation tells about them (native_tray.cpp). Kept free of Windows headers so it can be tested
// anywhere (tests/tray_buttons_test.cpp, run by tests/run_portable.sh).
//
// The chevron is a "SystemTray.NormalButton". Windows 11 26H2 made the app icons shown on the taskbar
// NormalButtons as well (AutomationId "NotifyItemIcon"; the chevron's is "SystemTrayIcon"), so "the
// first NormalButton" can be an app's icon: pressing it opened no hidden-icons window (seen on a
// 26H2 PC). Older builds give app icons another class, and there the first NormalButton is
// the chevron.
#include <string>
#include <vector>

namespace traybuttons {

struct Facts {
    std::wstring className, automationId;
};

inline constexpr wchar_t kNormal[] = L"SystemTray.NormalButton";
inline constexpr wchar_t kChevronId[] = L"SystemTrayIcon";
inline constexpr wchar_t kAppIconId[] = L"NotifyItemIcon";

// The chevron's position among the buttons (UI Automation tree order), -1 when there is none (no
// hidden icons, or Windows' "hidden icon menu" turned off).
inline int ChevronIndex(const std::vector<Facts>& buttons) {
    bool appIconsAreNormal = false;
    for (const auto& b : buttons)
        if (b.className == kNormal && b.automationId == kAppIconId) appIconsAreNormal = true;
    for (size_t i = 0; i < buttons.size(); ++i)   // the chevron as Windows 11 26H2 names it
        if (buttons[i].className == kNormal && buttons[i].automationId == kChevronId) return (int)i;
    // a NormalButton that is not an app's icon - on older builds (no NormalButton is one) the first NormalButton
    for (size_t i = 0; i < buttons.size(); ++i)
        if (buttons[i].className == kNormal && buttons[i].automationId != kAppIconId &&
            (!appIconsAreNormal || !buttons[i].automationId.empty())) return (int)i;
    return -1;
}

}   // namespace traybuttons
