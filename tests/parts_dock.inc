// The Dock's part files (src\dock*.cpp), in the order the dock tests include them under their fakes:
// dock_interaction_test, dock_layout_test, dock_render_device_test, dock_render_settings_window_test.
// Each test defines its fakes (#define RealName FakeName) before this include and #undefs them after it.
// A new part of the Dock goes here and into tests\lists.bat's PROBE_MODULES (dock_layout_live_probe
// includes dock.cpp only and links the other parts), never into DOCK_MODULES (it would be defined twice).
//
// A fake works only on the text included after its #define. These must be faked by every dock test, or the real
// call runs from the test: a page opened, files deleted, the Recycle Bin emptied, the desktop shown, an app bar
// registered (the user's work area shrinks), a window shown or brought to the front, a real menu.
#if !defined(ShellExecuteW) || !defined(SHFileOperationW) || !defined(SHEmptyRecycleBinW) || !defined(SHAppBarMessage) || \
    !defined(ToggleDesktop) || !defined(ShowWindow) || !defined(SetForegroundWindow) || !defined(TrackPopupMenu)
#error "a dock test includes parts_dock.inc without a fake for one of the calls listed above it"
#endif
// The power menu's calls fail the test if a part ever reaches them (tests\dock_fixture_guard.h). The calls below them
// are not used by the parts now: a part that starts using one does not compile in the tests until the tests have a
// fake for it (input sent, programs started, Windows shut down, the registry or files changed, system settings,
// hooks, tray icons, other processes ended, messages to every window).
#include "dock_fixture_guard.h"
#define ExitWindowsEx DockFixtureExitWindowsEx
#define LockWorkStation DockFixtureLockWorkStation
#define LoadLibraryExW DockFixtureLoadLibraryExW
#define SendInput DO_NOT_CALL_SendInput
#define keybd_event DO_NOT_CALL_keybd_event
#define mouse_event DO_NOT_CALL_mouse_event
#define SetCursorPos DO_NOT_CALL_SetCursorPos
#define BlockInput DO_NOT_CALL_BlockInput
#define ClipCursor DO_NOT_CALL_ClipCursor
#define ShellExecuteExW DO_NOT_CALL_ShellExecuteExW
#define CreateProcessW DO_NOT_CALL_CreateProcessW
#define CreateProcessAsUserW DO_NOT_CALL_CreateProcessAsUserW
#define WinExec DO_NOT_CALL_WinExec
#define InitiateSystemShutdownExW DO_NOT_CALL_InitiateSystemShutdownExW
#define InitiateShutdownW DO_NOT_CALL_InitiateShutdownW
#define SetSuspendState DO_NOT_CALL_SetSuspendState
#define SetWindowsHookExW DO_NOT_CALL_SetWindowsHookExW
#define RegSetValueExW DO_NOT_CALL_RegSetValueExW
#define RegSetKeyValueW DO_NOT_CALL_RegSetKeyValueW
#define RegCreateKeyExW DO_NOT_CALL_RegCreateKeyExW
#define RegDeleteKeyW DO_NOT_CALL_RegDeleteKeyW
#define RegDeleteKeyExW DO_NOT_CALL_RegDeleteKeyExW
#define RegDeleteValueW DO_NOT_CALL_RegDeleteValueW
#define RegDeleteTreeW DO_NOT_CALL_RegDeleteTreeW
#define DeleteFileW DO_NOT_CALL_DeleteFileW
#define MoveFileExW DO_NOT_CALL_MoveFileExW
#define RemoveDirectoryW DO_NOT_CALL_RemoveDirectoryW
#define SystemParametersInfoW DO_NOT_CALL_SystemParametersInfoW
#define ChangeDisplaySettingsExW DO_NOT_CALL_ChangeDisplaySettingsExW
#define Shell_NotifyIconW DO_NOT_CALL_Shell_NotifyIconW
#define TerminateProcess DO_NOT_CALL_TerminateProcess
#define BroadcastSystemMessageW DO_NOT_CALL_BroadcastSystemMessageW
#include "../src/dock.cpp"
#include "../src/dock_window.cpp"
#include "../src/dock_input.cpp"
#include "../src/dock_layout.cpp"
#include "../src/dock_render.cpp"
#include "../src/dock_sensors.cpp"
#include "../src/dock_panel.cpp"
#include "../src/dock_controls.cpp"
#include "../src/dock_notify.cpp"
#undef ExitWindowsEx
#undef LockWorkStation
#undef LoadLibraryExW
#undef SendInput
#undef keybd_event
#undef mouse_event
#undef SetCursorPos
#undef BlockInput
#undef ClipCursor
#undef ShellExecuteExW
#undef CreateProcessW
#undef CreateProcessAsUserW
#undef WinExec
#undef InitiateSystemShutdownExW
#undef InitiateShutdownW
#undef SetSuspendState
#undef SetWindowsHookExW
#undef RegSetValueExW
#undef RegSetKeyValueW
#undef RegCreateKeyExW
#undef RegDeleteKeyW
#undef RegDeleteKeyExW
#undef RegDeleteValueW
#undef RegDeleteTreeW
#undef DeleteFileW
#undef MoveFileExW
#undef RemoveDirectoryW
#undef SystemParametersInfoW
#undef ChangeDisplaySettingsExW
#undef Shell_NotifyIconW
#undef TerminateProcess
#undef BroadcastSystemMessageW
