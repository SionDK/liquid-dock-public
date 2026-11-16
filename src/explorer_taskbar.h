#pragma once
#include "common.h"

// With tray icons on the dock (tray_host.h) the dock owns a hidden "Shell_TrayWnd" window of its
// own, kept above Explorer's so programs send it their tray icons first. Code that means Explorer's
// taskbar asks for it here instead of FindWindow("Shell_TrayWnd"), which may return the dock's.
// The mark is a window property, so other processes (the watchdog) can tell the windows apart too.
constexpr wchar_t kTrayHostProp[] = L"LiquidDock.TrayHost";

// Explorer's: the one of the shell's process (the desktop's), else the first that is not a tray host.
// (Another program running a tray window of its own, such as RetroBar, is never mistaken for it.)
inline HWND ExplorerTaskbar() {
    DWORD shellPid = 0;
    if (HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &shellPid);
    HWND fallback = nullptr;
    HWND h = FindWindowW(L"Shell_TrayWnd", nullptr);
    for (int guard = 0; h && guard < 16; ++guard, h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)) {
        if (GetPropW(h, kTrayHostProp)) continue;
        if (!shellPid) return h;   // Explorer is (re)starting: no desktop yet
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == shellPid) return h;
        if (!fallback) fallback = h;
    }
    return fallback;
}
