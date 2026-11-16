#include "system.h"
#include <shellapi.h>
#include "native_tray.h"
#include "taskbar_start.h"
#include "taskbar_visibility.h"
#include "explorer_taskbar.h"

NativeTaskbar g_taskbar;

// ---------------------------------------------------------------- native taskbar

static void ForEachTaskbar(const std::function<void(HWND)>& fn) {
    if (HWND h = ExplorerTaskbar()) fn(h);
    HWND s = nullptr;
    while ((s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) fn(s);
}

// The taskbar is made invisible with an empty window region instead of being hidden: Explorer
// still treats it as shown, so its own windows (hidden icons, calendar, network...) keep working
// when the dock opens them, while it draws nothing and receives no clicks.
static bool HasEmptyRegion(HWND h) {
    HRGN r = CreateRectRgn(0, 0, 0, 0);
    int type = GetWindowRgn(h, r);
    DeleteObject(r);
    return type == NULLREGION;
}

static void Vanish(HWND h, bool onUiThread) {
    // Explorer can rebuild the region while activating a native flyout. Keep
    // the entire taskbar transparent as well, independent of that region.
    taskbarvisibility::Protect(h);
    if (!HasEmptyRegion(h) && IsWindowVisible(h)) {   // still expanded: its XAML buttons are still in the automation tree
        // The kept tray buttons and their UI Automation object are the UI thread's alone (a click looks them up and
        // presses them there). The enforcer's thread gets here too - Explorer rebuilding the region between Enforce's
        // look and this one, or laying its taskbar out again after auto-hide is set - and leaves them alone: a click
        // looks up what is not kept.
        if (onUiThread && h == ExplorerTaskbar()) nativetray::CacheTrayButtons(h);
        taskbarstart::Remember(h);   // its Start button, for the dock's start button (taskbar_start.h)
    }
    if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOWNA);   // e.g. hidden by an older version
    if (!HasEmptyRegion(h)) {
        HRGN r = CreateRectRgn(0, 0, 0, 0);
        if (!SetWindowRgn(h, r, TRUE)) DeleteObject(r);   // on success the window owns the region
    }
}

extern HWND g_mainWnd;
constexpr UINT WM_APP_ENFORCE = WM_APP + 30;   // handled by the main window: Enforce() on the UI thread

void NativeTaskbar::StartEnforcer() {
    const bool runs = TryDetach([this] {
        double start = NowSeconds();
        for (int pass = 0;; ++pass) {
            // right after logon explorer keeps (re)building its taskbar and resetting auto-hide: check
            // often, and ask about auto-hide every time, for the first minute. Later the region and the
            // opacity (local and cheap) every second, auto-hide - a round trip to Explorer - every 5 s
            // and at once when the taskbar settings change (main.cpp: WM_SETTINGCHANGE "TraySettings").
            const bool early = NowSeconds() - start < 60;
            Sleep(early ? 250 : 1000);
            const bool askAutoHide = early || pass % 5 == 0 || recheck.exchange(false);
            if (hidden) Enforce(false, askAutoHide);
        }
    });
    // no checks on the UI thread instead: its auto-hide question waits on Explorer
    if (!runs) Log(L"taskbar: no thread to keep it hidden on: hidden again only when Explorer makes a new one (TaskbarCreated)");
}

bool NativeTaskbar::Hide() {
    std::lock_guard<std::recursive_mutex> lk(mu);
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = ExplorerTaskbar();
    if (!abd.hWnd) return false;
    if (!stateSaved) {   // do not hide anything until the recovery state has been saved
        originalState = SHAppBarMessage(ABM_GETSTATE, &abd);
        // remember across crashes so a later run can restore it
        HKEY k = nullptr;
        LSTATUS saved = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\LiquidDock", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
        if (saved == ERROR_SUCCESS) {
            DWORD v = (DWORD)originalState;
            DWORD existing = 0, sz = sizeof(existing);
            saved = RegGetValueW(HKEY_CURRENT_USER, L"Software\\LiquidDock", L"TaskbarState", RRF_RT_REG_DWORD, nullptr, &existing, &sz);
            if (saved == ERROR_FILE_NOT_FOUND || saved == ERROR_PATH_NOT_FOUND)
                saved = RegSetValueExW(k, L"TaskbarState", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
            else if (saved == ERROR_SUCCESS) originalState = existing;
            RegCloseKey(k);
        }
        if (saved != ERROR_SUCCESS) { Log(L"taskbar backup failed: %ld; keeping native taskbar", saved); return false; }
        stateSaved = true;
    }
    hidden = true;
    Enforce();
    return true;
}

void NativeTaskbar::Enforce(bool onUiThread, bool checkAutoHide) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    if (!hidden) return;
    HWND tray = ExplorerTaskbar();
    if (!tray) return;   // explorer not up yet (early logon) - try again shortly
    // a new / re-shown taskbar must have its tray buttons cached (UI Automation, UI thread) before it vanishes
    if (!onUiThread && (!stateSaved || !HasEmptyRegion(tray))) { PostMessageW(g_mainWnd, WM_APP_ENFORCE, 0, 0); return; }
    if (!stateSaved) { Hide(); return; }
    // auto-hide frees the reserved work area so maximised windows use the full screen;
    // explorer may reset it while it starts, so keep checking
    ForEachTaskbar([&](HWND h) { Vanish(h, onUiThread); });
    if (!checkAutoHide) return;
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = tray;
    UINT_PTR st = SHAppBarMessage(ABM_GETSTATE, &abd);
    if (!(st & ABS_AUTOHIDE)) {
        abd.lParam = ABS_AUTOHIDE | (originalState & ABS_ALWAYSONTOP);
        SHAppBarMessage(ABM_SETSTATE, &abd);
        ForEachTaskbar([&](HWND h) { Vanish(h, onUiThread); });   // Explorer lays its taskbar out again for the new state
    }
}

bool NativeTaskbar::WithInvisibleTaskbars(const std::function<bool()>& action) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    if (!hidden || !stateSaved) return false;
    bool protectedAll = true; int count = 0;
    ForEachTaskbar([&](HWND h) { ++count; if (!taskbarvisibility::Protect(h)) protectedAll = false; });
    // Never activate Explorer's controls on the strength of an empty region alone.
    if (!count || !protectedAll) return false;
    bool result = action();
    Enforce();
    ForEachTaskbar([&](HWND h) { if (!taskbarvisibility::IsProtected(h)) result = false; });
    return result;
}

bool NativeTaskbar::Restore() {
    std::lock_guard<std::recursive_mutex> lk(mu);
    hidden = false;
    DWORD st = 0, sz = sizeof(st);
    LSTATUS read = RegGetValueW(HKEY_CURRENT_USER, L"Software\\LiquidDock", L"TaskbarState", RRF_RT_REG_DWORD, nullptr, &st, &sz);
    bool fromReg = read == ERROR_SUCCESS;
    if (!stateSaved && !fromReg && read != ERROR_FILE_NOT_FOUND && read != ERROR_PATH_NOT_FOUND) return false;
    if (!stateSaved && fromReg) { originalState = st; stateSaved = true; }
    HWND primary = ExplorerTaskbar();
    if (!primary) return false;   // Explorer is restarting: retain the recovery record
    std::vector<HWND> bars;
    ForEachTaskbar([&](HWND h) { bars.push_back(h); });
    bool restored = true;
    for (HWND h : bars) {
        if (!taskbarvisibility::Restore(h)) restored = false;
        if (!SetWindowRgn(h, nullptr, TRUE)) restored = false;
        ShowWindow(h, SW_SHOWNA);
        if (!IsWindow(h) || !IsWindowVisible(h) || HasEmptyRegion(h)) restored = false;
    }
    if (stateSaved) {
        APPBARDATA abd = { sizeof(abd) };
        abd.hWnd = primary;
        abd.lParam = originalState;
        SHAppBarMessage(ABM_SETSTATE, &abd);
        // ABM_SETSTATE always returns TRUE. Read back the setting instead.
        // ABS_ALWAYSONTOP is no longer meaningful on supported modern Windows versions.
        if ((SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != (originalState & ABS_AUTOHIDE)) restored = false;
    }
    std::vector<HWND> current;
    ForEachTaskbar([&](HWND h) { current.push_back(h); });
    if (!restored || primary != ExplorerTaskbar() || current != bars) return false;
    LSTATUS removed = RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\LiquidDock", L"TaskbarState");
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND && removed != ERROR_PATH_NOT_FOUND) return false;
    stateSaved = false;
    return true;
}
