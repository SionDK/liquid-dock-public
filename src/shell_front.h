#pragma once
#include "common.h"
#include <initializer_list>

// Presses the keys and lets them go (SendInput). true: every event was inserted - which does not mean
// they arrived: Windows drops injected input bound for a program that runs as administrator (UIPI).
bool SendKeyCombo(std::initializer_list<WORD> keys);

// The right to put another window in front (SetForegroundWindow, and to pass it on with
// AllowSetForegroundWindow). The dock's windows never take activation (WS_EX_NOACTIVATE), and a click on
// such a window does not always give the dock that right: SetForegroundWindow then only flashes the
// window (2026-10-27: nine clicks on two app icons, the windows stayed behind until
// Alt+Tab). Input this process sends itself gives it the right: an unassigned key (0xE8) that no program
// acts on - RetroBar does the same for its taskbar. Probed first (AllowSetForegroundWindow on this very
// process fails without the right), so the key goes out only when the right is missing.
enum class ForegroundRight { Had, Taken, Refused, NotInserted };
ForegroundRight TakeForegroundRight();
const wchar_t* ForegroundRightText(ForegroundRight r);

// The shell's own pop-ups (Start, Search, quick settings, the notification centre), by the program of the
// window: the dock comes up with them (Dock::ShellPopupOpen), and a click on an app icon closes the one in
// front before it brings the app up (AppModel::BringUp), as a click on the taskbar does.
bool IsShellPopupWindow(HWND w);

// Start is shown (IAppVisibility::IsLauncherVisible, documented since Windows 8): 1 shown, 0 not, -1 could not
// be asked. Its window in front need not tell: with Start open on 26H2 the front is SearchHost's CoreWindow,
// which can be cloaked all the same (seen 2026-11-17). UI thread; the object is made once and kept.
int LauncherVisible();
