#pragma once
// The dock's main window (main.cpp, MainProc): its handle and the messages the other parts post to it.
// WM_APP_APPBAR goes to the dock windows instead (dock_window.cpp, dock_input.cpp). WM_APP_ENFORCE, from
// the taskbar enforcer thread, is in native_taskbar.h; the developer commands' messages are named in main.cpp.
#include "common.h"

extern HWND g_mainWnd;
constexpr UINT WM_APP_REBUILD = WM_APP + 1;
constexpr UINT WM_APP_AUTOSTART = WM_APP + 2;
constexpr UINT WM_APP_RELAYOUT = WM_APP + 3;    // shape settings changed: lay the docks out again in place
constexpr UINT WM_APP_SETTINGS = WM_APP + 4;    // open the settings window
constexpr UINT WM_APP_HOTKEY = WM_APP + 5;      // the reveal shortcut setting changed
constexpr UINT WM_APP_TRAYHOST = WM_APP + 6;    // the tray icons setting changed: start or stop the tray host
constexpr UINT WM_APP_APPBAR = WM_APP + 40;     // shell notifications for an always-shown dock
