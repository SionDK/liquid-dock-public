#pragma once
// The window preview's two tests on an in-memory window/DWM model (window_preview_test, preview_rules_test):
// the simple fakes they share and their #define list.
// Include it right before window_preview.cpp, after the test's own fakes and after window_preview.h (WindowPreview).
// Each test keeps its own Source model, thumbnail and DWM fakes (window_preview_test hands out the source's handle,
// preview_rules_test numbers them and counts strays) and its counters; window_preview_test adds the fakes that catch
// a source window being touched (OpenIcon ... SendMessageW), both add `#define Log FakeLog` after this.
static WindowPreview* current = nullptr;   // the popup's window data (GetWindowLongPtrW)
static HWND captured = nullptr;            // the mouse capture
static LONG_PTR WINAPI FakeUserData(HWND, int) { return reinterpret_cast<LONG_PTR>(current); }
static HWND WINAPI FakeCapture(HWND h) { HWND old = captured; captured = h; return old; }
static HWND WINAPI FakeCaptured() { return captured; }
static BOOL WINAPI FakeRelease() { captured = nullptr; return TRUE; }
static HRESULT WINAPI FakeAttribute(HWND, DWORD, PVOID value, DWORD) { *static_cast<DWORD*>(value) = 0; return S_OK; }
static int WINAPI FakeClassName(HWND, LPWSTR name, int count) { wcscpy_s(name, count, L"FixtureClass"); return 12; }
static int WINAPI FakeText(HWND, LPWSTR text, int count) { wcscpy_s(text, count, L"Fixture window"); return 14; }
static BOOL WINAPI FakeDestroy(HWND) { return TRUE; }

// the test defines these: FakeIsWindow, FakeVisible, FakeShow, FakePid, FakeIconic, FakeAffinity, FakeRegister,
// FakeUnregister, FakeSize, FakeUpdate, FakeWindowRect, FakePos, FakeRegion, FakeInvalidate, FakeForeground, FakeKey
#define IsWindow FakeIsWindow
#define IsWindowVisible FakeVisible
#define ShowWindow FakeShow
#define GetWindowThreadProcessId FakePid
#define IsIconic FakeIconic
#define GetWindowDisplayAffinity FakeAffinity
#define DwmGetWindowAttribute FakeAttribute
#define DwmRegisterThumbnail FakeRegister
#define DwmUnregisterThumbnail FakeUnregister
#define DwmQueryThumbnailSourceSize FakeSize
#define DwmUpdateThumbnailProperties FakeUpdate
#define GetWindowRect FakeWindowRect
#define GetClassNameW FakeClassName
#define SetWindowPos FakePos
#define SetWindowRgn FakeRegion
#define InvalidateRect FakeInvalidate
#define GetWindowTextW FakeText
#define GetForegroundWindow FakeForeground
#define GetAsyncKeyState FakeKey
#define DestroyWindow FakeDestroy
#define GetWindowLongPtrW FakeUserData
#define SetCapture FakeCapture
#define GetCapture FakeCaptured
#define ReleaseCapture FakeRelease
