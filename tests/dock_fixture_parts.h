#pragma once
// The Dock's part files under the fakes of dock_fixture_matrix.h (dock_interaction_test, dock_render_device_test).
// Before this include the test defines: the shell fakes MatrixShellExecute, MatrixFileOperation and
// MatrixEmptyRecycleBin (its own or dock_fixture_quiet.h's), MatrixPopupMenu, nativetray::MatrixToggleDesktop, and
// includes dock_fixture_appbar.h (FakeAppBar). A fake only this test has goes in a #define of its own right before
// this include and an #undef right after it (dock_interaction_test: GetWindow, SetWindowPos).
// The shell fakes stay defined after the parts (both tests use them later).
#define ShellExecuteW MatrixShellExecute
#define SHFileOperationW MatrixFileOperation
#define SHEmptyRecycleBinW MatrixEmptyRecycleBin
#define SHAppBarMessage FakeAppBar
#define GetMonitorInfoW MatrixMonitorInfo
#define GetForegroundWindow MatrixForeground
#define MonitorFromWindow MatrixMonitorFromWindow
#define GetWindowRect MatrixWindowRect
#define IsZoomed MatrixIsZoomed
#define GetWindowLongPtrW MatrixWindowLong
#define GetWindowInfo MatrixWindowInfo
#define GetClassNameW MatrixClassName
#define ShowWindow MatrixShowWindow
#define GetAsyncKeyState MatrixKeyState
#define SetForegroundWindow MatrixForegroundSet
#define TrackPopupMenu MatrixPopupMenu
#define ToggleDesktop MatrixToggleDesktop
#include "parts_dock.inc"
#undef SHAppBarMessage
#undef GetMonitorInfoW
#undef GetForegroundWindow
#undef MonitorFromWindow
#undef GetWindowRect
#undef IsZoomed
#undef GetWindowLongPtrW
#undef GetWindowInfo
#undef GetClassNameW
#undef ShowWindow
#undef GetAsyncKeyState
#undef SetForegroundWindow
#undef TrackPopupMenu
#undef ToggleDesktop
