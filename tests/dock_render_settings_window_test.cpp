// The production settings window (settings_window*.cpp) on the real Direct3D / Direct2D device, driven by
// window messages sent straight to its (never shown) window: the mouse wheel, clicks on its controls, a
// slider drag and a move to a monitor of another resolution. The fixture is the one of
// tests/dock_interaction_test.cpp (tests\dock_fixture_*.h: stubs for the shell, OLED, theme, audio, Start and the
// taskbar; fake monitors; a fake shell's app bars), plus stubs for ShowWindow, SetForegroundWindow, SetCapture and TrackPopupMenu: no input is
// sent, no window is shown or activated, the mouse is never captured, and nothing outside this test's
// folder (build\tests\dock-render\data) is written. "g_mainWnd" is a message-only window of this test
// that counts what the settings window posts to the docks. One PASS/FAIL line per check, a summary line;
// the exit code is the number of failures.
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/capture.h"
#include "../src/apps.h"
#include "../src/system.h"
#include "../src/sensor_shared.h"
#include "../src/native_tray.h"
#include <shellscalingapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d2d1effects.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <map>
#include <limits>
#define private public
#include "../src/sensors.h"
#include "../src/dock.h"
#undef private

Settings g_settings;
HWND g_mainWnd = nullptr;
struct MonitorCase { HMONITOR handle; RECT rect; bool primary; };
static std::vector<MonitorCase> monitors;
static BOOL WINAPI MatrixMonitorInfo(HMONITOR m, LPMONITORINFO info) {
    for (const auto& entry : monitors) if (entry.handle == m) { info->rcMonitor = info->rcWork = entry.rect; info->dwFlags = entry.primary ? MONITORINFOF_PRIMARY : 0; return TRUE; }
    return ::GetMonitorInfoW(m, info);
}
// WM_DPICHANGED's rectangle lands on this fake monitor (the window itself stays on the real screen)
static HMONITOR movedTo = nullptr;
static HMONITOR WINAPI MatrixMonitorFromRect(LPCRECT r, DWORD flags) { return movedTo ? movedTo : ::MonitorFromRect(r, flags); }
static HWND WINAPI MatrixForeground() { return nullptr; }
static BOOL WINAPI MatrixShowWindow(HWND, int) { return TRUE; }   // keep fixture windows invisible
static SHORT WINAPI MatrixKeyState(int) { return 0; }
static BOOL WINAPI MatrixForegroundSet(HWND) { return TRUE; }      // never activate a window
static int popupMenus = 0;
// The menu "picks" popupChoice only when it has that item enabled (a real menu cannot pick a greyed item); 0: none.
// popupItems: the last menu's items, "id:text" ("g" after the id when greyed, "-" a separator).
static int popupChoice = 0;
static std::wstring popupItems;
static POINT popupAt{};   // where the last menu was asked to open (screen pixels)
static BOOL WINAPI MatrixPopupMenu(HMENU m, UINT, int px, int py, int, HWND, const RECT*) {
    ++popupMenus;
    popupAt = { px, py };
    popupItems.clear();
    BOOL pick = 0;
    for (int i = 0, n = GetMenuItemCount(m); i < n; ++i) {
        wchar_t text[64] = {};
        MENUITEMINFOW mi = { sizeof(mi) };
        mi.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_STRING; mi.dwTypeData = text; mi.cch = ARRAYSIZE(text);
        GetMenuItemInfoW(m, i, TRUE, &mi);
        if (!popupItems.empty()) popupItems += L",";
        if (mi.fType & MFT_SEPARATOR) { popupItems += L"-"; continue; }
        const bool grey = (mi.fState & MFS_DISABLED) != 0;
        popupItems += std::to_wstring(mi.wID) + (grey ? L"g:" : L":") + text;
        if (popupChoice && (int)mi.wID == popupChoice && !grey) pick = popupChoice;
    }
    return pick;
}
static HWND captured = nullptr;                                    // never take the real mouse
static HWND WINAPI MatrixSetCapture(HWND h) { HWND was = captured; captured = h; return was; }
// captureNotifies: as Windows, ReleaseCapture sends WM_CAPTURECHANGED (lParam: the new owner, none) to the window losing it
static bool captureNotifies = false;
static BOOL WINAPI MatrixReleaseCapture() {
    HWND was = captured; captured = nullptr;
    if (captureNotifies && was) SendMessageW(was, WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}
// the settings window only: where WM_EXITSIZEMOVE finds the window (null: the real screen), and TrackMouseEvent
// (never the real one: it would post WM_MOUSELEAVE to the hidden window at once)
static HMONITOR windowOn = nullptr;
static HMONITOR WINAPI MatrixMonitorFromWindow(HWND h, DWORD f) { return windowOn ? windowOn : ::MonitorFromWindow(h, f); }
static int leaveTracks = 0;
static BOOL WINAPI MatrixTrackMouseEvent(LPTRACKMOUSEEVENT) { ++leaveTracks; return TRUE; }
// OLED mode stubs: never the real wallpaper or desktop icons
static bool oledOn = false;
static HMONITOR oledFor = (HMONITOR)(intptr_t)-1;   // the monitor the last Enable was asked for (-1: never asked)
namespace oled {
bool Active() { return oledOn; }
bool CoversMonitor(HMONITOR) { return oledOn; }
bool Enable(HMONITOR m) { oledOn = true; oledFor = m; return true; }
bool Disable() { oledOn = false; return true; }
void Reapply() {}
}
// the desktop, theme, shell, Recycle Bin, audio and Start stubs that only answer; the taskbar's Start button and app
// buttons; the shell's app bars (no real app bar is registered)
#include "dock_fixture_quiet.h"
#include "dock_fixture_taskbar.h"
#include "dock_fixture_appbar.h"
#define ShellExecuteW MatrixShellExecute
#define SHFileOperationW MatrixFileOperation
#define SHEmptyRecycleBinW MatrixEmptyRecycleBin
#define SHAppBarMessage FakeAppBar
#define GetMonitorInfoW MatrixMonitorInfo
#define MonitorFromRect MatrixMonitorFromRect
#define GetForegroundWindow MatrixForeground
#define ShowWindow MatrixShowWindow
#define GetAsyncKeyState MatrixKeyState
#define SetForegroundWindow MatrixForegroundSet
#define TrackPopupMenu MatrixPopupMenu
#define SetCapture MatrixSetCapture
#define ReleaseCapture MatrixReleaseCapture
#define ToggleDesktop MatrixToggleDesktop
#include "parts_dock.inc"
#include <dwmapi.h>
static int presents = 0;   // frames the settings window has shown (its only Present call)
static void OnPresent();   // below: the frame's pixels read back, when a check asks for them
// presentNow (the characterization checks only, after the checks above): no wait for the screen's next frame, so
// their thousand frames take less time. The frame is read back before Present, so its pixels are the same.
static bool presentNow = false;
#define Present(sync, flags) Present((++presents, OnPresent(), presentNow ? 0u : (sync)), (flags))
// What the settings window draws, as text (recOn): each Direct2D call of settings_window*.cpp with its rectangle (DIPs)
// and the brush's colour. No font or GPU is in it, so it is the same on every PC; the version is written "<version>".
static bool recOn = false;
static std::wstring recOps;
static std::vector<std::wstring> recTexts;
static void RecPut(const wchar_t* op, std::initializer_list<float> v, ID2D1Brush* b) {
    if (!recOn) return;
    recOps += op;
    for (float f : v) { wchar_t t[32]; swprintf_s(t, L" %.3f", f); recOps += t; }
    if (b) {
        const D2D1_COLOR_F c = static_cast<ID2D1SolidColorBrush*>(b)->GetColor();
        wchar_t t[80]; swprintf_s(t, L" #%.3f,%.3f,%.3f,%.3f", c.r, c.g, c.b, c.a); recOps += t;
    }
    recOps += L"\n";
}
static void RecText(const wchar_t* t, UINT32 n, const D2D1_RECT_F& r, ID2D1Brush* b) {
    if (!recOn) return;
    std::wstring s(t, n);
    if (s.rfind(L"버전 ", 0) == 0) s = L"<version>";
    recTexts.push_back(s);
    recOps += L"T[" + s + L"]";
    RecPut(L"", { r.left, r.top, r.right, r.bottom }, b);
}
static void RecFR(const D2D1_RECT_F& r, ID2D1Brush* b) { RecPut(L"FR", { r.left, r.top, r.right, r.bottom }, b); }
static void RecFRR(const D2D1_ROUNDED_RECT& r, ID2D1Brush* b) { RecPut(L"FRR", { r.rect.left, r.rect.top, r.rect.right, r.rect.bottom, r.radiusX, r.radiusY }, b); }
static void RecDRR(const D2D1_ROUNDED_RECT& r, ID2D1Brush* b, float w = 1, ID2D1StrokeStyle* = nullptr) { RecPut(L"DRR", { r.rect.left, r.rect.top, r.rect.right, r.rect.bottom, r.radiusX, r.radiusY, w }, b); }
static void RecFE(const D2D1_ELLIPSE& e, ID2D1Brush* b) { RecPut(L"FE", { e.point.x, e.point.y, e.radiusX, e.radiusY }, b); }
static void RecDE(const D2D1_ELLIPSE& e, ID2D1Brush* b, float w = 1, ID2D1StrokeStyle* = nullptr) { RecPut(L"DE", { e.point.x, e.point.y, e.radiusX, e.radiusY, w }, b); }
static void RecClear(const D2D1_COLOR_F& c) { RecPut(L"CLEAR", { c.r, c.g, c.b, c.a }, nullptr); }
static void RecClip(const D2D1_RECT_F& r, D2D1_ANTIALIAS_MODE m) { RecPut(L"CLIP", { r.left, r.top, r.right, r.bottom, (float)m }, nullptr); }
// The text format of each DrawTextW (recFonts, apart from recOps so the hashes of the drawing calls stay as they
// were): its family (the dock's icon font "<glyph>": the one this PC resolved), size, weight, alignments, wrapping.
static std::wstring recFonts;
static std::wstring FontText(IDWriteTextFormat* f);
static void RecFont(IDWriteTextFormat* f) { if (recOn) recFonts += FontText(f) + L"\n"; }
// These macros evaluate their arguments twice (once for the real call, once for the record): the settings window's
// arguments have no side effects now; one that had (Fill(NextRow(y), ...)) would run twice in this test only.
#define DrawTextW(t, n, f, r, b, o) DrawTextW((RecText((t), (n), (r), (b)), RecFont(f), (t)), (n), (f), (r), (b), (o))
#define FillRectangle(...) FillRectangle(__VA_ARGS__), RecFR(__VA_ARGS__)
#define FillRoundedRectangle(...) FillRoundedRectangle(__VA_ARGS__), RecFRR(__VA_ARGS__)
#define DrawRoundedRectangle(...) DrawRoundedRectangle(__VA_ARGS__), RecDRR(__VA_ARGS__)
#define FillEllipse(...) FillEllipse(__VA_ARGS__), RecFE(__VA_ARGS__)
#define DrawEllipse(...) DrawEllipse(__VA_ARGS__), RecDE(__VA_ARGS__)
#define Clear(...) Clear(__VA_ARGS__), RecClear(__VA_ARGS__)
#define PushAxisAlignedClip(...) PushAxisAlignedClip(__VA_ARGS__), RecClip(__VA_ARGS__)
// The accent colour (ReadAccent) never comes from the real registry: accentValue (ABGR, Windows' default blue unless a
// check sets another), or no value at all (accentFails). A WM_SETTINGCHANGE broadcast by another program during the
// test then reads the same colour too.
static DWORD accentValue = 0x00D77800;
static bool accentFails = false;
static int accentReads = 0;
static LSTATUS WINAPI MatrixRegGet(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID data, LPDWORD) {
    ++accentReads;
    if (accentFails || !data) return ERROR_FILE_NOT_FOUND;
    *(DWORD*)data = accentValue;
    return ERROR_SUCCESS;
}
// What the settings window passes on to DefWindowProc, by message (then the real one)
static std::map<UINT, int> defCalls;
static LRESULT WINAPI MatrixDefProc(HWND h, UINT m, WPARAM w, LPARAM l) { ++defCalls[m]; return ::DefWindowProcW(h, m, w, l); }
#define RegGetValueW MatrixRegGet
#define DefWindowProcW MatrixDefProc
#define MonitorFromWindow MatrixMonitorFromWindow
#define TrackMouseEvent MatrixTrackMouseEvent
#include "../src/settings_window.cpp"
#include "../src/settings_window_pages.cpp"
#include "../src/settings_window_customizer.cpp"
#undef Present
#undef RegGetValueW
#undef DefWindowProcW
#undef DrawTextW
#undef FillRectangle
#undef FillRoundedRectangle
#undef DrawRoundedRectangle
#undef FillEllipse
#undef DrawEllipse
#undef Clear
#undef PushAxisAlignedClip
#undef MonitorFromWindow
#undef TrackMouseEvent
#undef ShellExecuteW
#undef SHFileOperationW
#undef SHEmptyRecycleBinW
#undef SHAppBarMessage
#undef GetMonitorInfoW
#undef MonitorFromRect
#undef GetForegroundWindow
#undef ShowWindow
#undef GetAsyncKeyState
#undef SetForegroundWindow
#undef TrackPopupMenu
#undef SetCapture
#undef ReleaseCapture
#undef ToggleDesktop

#include "portable_check.h"

namespace sw = settingswindow;

static const HMONITOR kLaptop = (HMONITOR)(uintptr_t)0x3001, kDesktop = (HMONITOR)(uintptr_t)0x3002;

// ---------------------------------------------------------------- what reaches the docks

static int rebuilds = 0, relayouts = 0, trayHosts = 0;
static int closes = 0, autostarts = 0, hotkeys = 0;   // counted, never dispatched: "종료" does not close the test's window
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static void Drain() {   // count (and drop) what the settings window posted to the docks
    MSG msg;
    while (PeekMessageW(&msg, g_mainWnd, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_APP_REBUILD) ++rebuilds;
        else if (msg.message == WM_APP_RELAYOUT) ++relayouts;
        else if (msg.message == WM_APP_TRAYHOST) ++trayHosts;
        else if (msg.message == WM_CLOSE) ++closes;
        else if (msg.message == WM_APP_AUTOSTART) ++autostarts;
        else if (msg.message == WM_APP_HOTKEY) ++hotkeys;
    }
}
static void ResetCounts() { Drain(); rebuilds = relayouts = trayHosts = 0; closes = autostarts = hotkeys = 0; }
static std::wstring Ini() { return AppDataDir() + L"\\settings.ini"; }
static bool Saved() { return GetFileAttributesW(Ini().c_str()) != INVALID_FILE_ATTRIBUTES; }
static void Unsave() { DeleteFileW(Ini().c_str()); }

// ---------------------------------------------------------------- driving the window

// A point of the content (DIPs) in screen pixels, for WM_MOUSEWHEEL, and in client pixels for the others.
static POINT ClientPx(float x, float y) { return { (LONG)std::lround(x * sw::g_dpi / 96), (LONG)std::lround(y * sw::g_dpi / 96) }; }
static void Wheel(float x, float y, int delta) {
    POINT p = ClientPx(x, y); ClientToScreen(sw::g_wnd, &p);
    SendMessageW(sw::g_wnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(short)delta), MAKELPARAM((WORD)(short)p.x, (WORD)(short)p.y));
}
static void Mouse(UINT msg, float x, float y) {
    POINT p = ClientPx(x, y);
    SendMessageW(sw::g_wnd, msg, msg == WM_LBUTTONDOWN ? MK_LBUTTON : 0, MAKELPARAM((WORD)(short)p.x, (WORD)(short)p.y));
}
static void Click(float x, float y) { Mouse(WM_LBUTTONDOWN, x, y); Mouse(WM_LBUTTONUP, x, y); }
// The centre of the first hot area of a control (nav item: -10 - page, reset: -2); false if it is not drawn.
static bool HotCentre(int control, int sub, float& x, float& y) {
    for (auto& h : sw::g_hots)
        if (h.control == control && h.sub == sub) { x = (h.r.left + h.r.right) / 2; y = (h.r.top + h.r.bottom) / 2; return true; }
    return false;
}
static int ControlIndex(const wchar_t* label) {
    const auto& cs = sw::g_pages[sw::g_page].controls;
    for (int i = 0; i < (int)cs.size(); ++i) if (!wcscmp(cs[i].label, label)) return i;
    return -1;
}
static void GoToPage(int page) {
    float x, y;
    if (HotCentre(-10 - page, 0, x, y)) Click(x, y);
}
// A place on the page with no control under it: the page title.
static void TitlePoint(float& x, float& y) { x = sw::kNav + sw::kPad + 20; y = 26 + 20 - sw::g_scroll; }

// ---------------------------------------------------------------- checks

static void Opening() {
    sw::Show(kLaptop);
    Check(sw::IsOpen() && sw::g_swap && sw::g_target, "the settings window opens (hidden) with its swap chain");
    // the laptop: 2880x1800 at Windows' 150 %; the window follows the screen (1800 / 1440 = 1.25), not Windows' scale
    Check(std::fabs(sw::g_dpi - 120) < 0.01f, "content scale for the laptop's 2880x1800 screen: 120 dpi");
    RECT rc; GetClientRect(sw::g_wnd, &rc);
    Check(rc.right == (LONG)std::ceil(sw::kW * 1.25f) && rc.bottom == (LONG)std::ceil(sw::kH * 1.25f), "client area 660x600 DIPs in pixels at 1.25");
    Check(g_settingsHold.open, "the docks are told the settings are open");
    LONG_PTR st = GetWindowLongPtrW(sw::g_wnd, GWL_STYLE);
    Check(!(st & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) && (st & WS_SYSMENU), "only a close button: no minimize or maximize box");
}

// Win+D (or the dock's show-desktop button) still minimizes a window without a minimize box: the settings window
// then closes - saved, the docks let go - instead of staying open off the screen and holding every dock up.
static void MinimizeCloses() {
    sw::Show(kLaptop);
    Check(sw::IsOpen() && g_settingsHold.open, "open again for the minimize check");
    if (!sw::IsOpen()) return;
    Unsave();
    SendMessageW(sw::g_wnd, WM_SIZE, SIZE_MINIMIZED, 0);
    Check(!g_settingsHold.open, "minimized: the docks are let go at once");
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Check(!sw::IsOpen() && !g_settingsHold.open && Saved() && !sw::g_swap, "minimized: it closes - saved, its device resources released");
}

// The shape page's sliders sit in a folded group ("독 크기와 위치 조정"): until its header is clicked they are not
// drawn, so neither a click nor the wheel reaches them (a desktop's edgeOffset went from -10 to -30 unnoticed,
// most likely from the wheel landing on that slider while scrolling the page).
static bool AnySliderShown() {
    for (auto& h : sw::g_hots)
        if (h.control >= 0 && sw::g_pages[sw::g_page].controls[h.control].type == sw::Control::Slider) return true;
    return false;
}
static void Folding() {
    GoToPage(0);
    sw::g_scroll = 0; sw::Render();
    const int group = ControlIndex(L"독 크기와 위치 조정"), icon = ControlIndex(L"아이콘 크기"), offset = ControlIndex(L"가장자리 따라 위치");
    if (group < 0 || icon < 0 || offset < 0) { Check(false, "the group and its sliders found"); return; }
    const auto& gc = sw::g_pages[0].controls[group];
    Check(gc.type == sw::Control::Group && gc.count == 5 && offset == group + 5, "the group folds the five sliders after it, the last being the position along the edge");
    float x, y, hx, hy;
    Check(!gc.open && !AnySliderShown() && !HotCentre(icon, 0, x, y), "folded when the window opens: no slider is drawn");
    Check(HotCentre(group, 0, hx, hy), "its header is drawn");
    Check(sw::g_contentH <= sw::ClientH(), "folded, the shape page fits the window");
    // its panel stays clear of the row above: the "기본" marks under 위치 end 19 DIPs below that control's hot areas
    float segBottom = -1, panelTop = -1;
    for (auto& h : sw::g_hots) {
        if (h.control == ControlIndex(L"위치")) segBottom = std::max(segBottom, h.r.bottom);
        if (h.control == group) panelTop = h.r.top;
    }
    Check(segBottom > 0 && panelTop >= segBottom + 19, "the header's panel does not cover the \"기본\" mark under 위치");
    // the wheel over the folded group's place, and just below its header, changes nothing
    g_settings = Settings{}; g_settings.edgeOffset = -10;
    ResetCounts(); Unsave();
    for (float dy : { 0.f, 40.f, 80.f, 120.f }) Wheel(hx + 7, hy + dy, -WHEEL_DELTA);
    Drain();
    const Settings d;
    Check(g_settings.edgeOffset == -10 && g_settings.iconSize == d.iconSize && g_settings.iconGap == d.iconGap &&
          g_settings.dockThickness == d.dockThickness && g_settings.edgeGap == d.edgeGap && rebuilds + relayouts == 0 && !Saved(),
          "the wheel over a folded group changes no setting, rebuilds and saves nothing");
    // a click on the header opens it: the sliders are there, and the page scrolls again
    ResetCounts(); Unsave();
    Click(hx, hy);
    Drain();
    Check(gc.open && AnySliderShown() && HotCentre(icon, 0, x, y) && HotCentre(offset, 0, x, y), "a click on the header opens it: the sliders are drawn");
    Check(rebuilds + relayouts == 0 && !Saved(), "opening it changes nothing on the docks");
    Check(sw::g_contentH > sw::ClientH() + 50, "open, the shape page is taller than the window: it scrolls");
    // folded again while scrolled down: the page, now shorter, goes back to its top
    sw::g_scroll = 100; sw::Render();
    if (!HotCentre(group, 0, hx, hy)) { Check(false, "header shown while scrolled"); return; }
    const int framesBefore = presents;
    Click(hx, hy);
    Check(!gc.open && !AnySliderShown() && sw::g_scroll == 0, "a second click folds it, and the page scrolled past its new end comes back to the top");
    Check(presents - framesBefore == 1, "...in one frame: none is drawn at the old scroll first");
    // opened, a wheel on a slider works it as before (the header is back where it was at the top)
    if (!HotCentre(group, 0, hx, hy)) { Check(false, "header shown at the top"); return; }
    Click(hx, hy);
    if (!HotCentre(offset, 0, x, y)) { Check(false, "the edge offset slider shown"); return; }
    ResetCounts(); Unsave();
    Wheel(x, y, WHEEL_DELTA);
    Drain();
    Check(g_settings.edgeOffset == -5 && relayouts == 1 && Saved(), "open, a notch up on the position along the edge: -10 to -5, laid out in place, saved");
    // closed and opened again: folded again
    sw::Close();
    sw::Show(kLaptop);
    Drain();
    if (!sw::IsOpen()) { Check(false, "the window opens again"); return; }
    sw::Render();
    const auto& again = sw::g_pages[0].controls[ControlIndex(L"독 크기와 위치 조정")];
    Check(!again.open && !AnySliderShown(), "opened again: folded again");
    // the checks below drive the sliders: open it
    if (HotCentre(group, 0, hx, hy)) Click(hx, hy);
    Check(again.open && AnySliderShown(), "opened for the checks that follow");
    g_settings = Settings{};
    ResetCounts(); Unsave();
}

// The behaviour page's three sliders fold the same way ("확대 효과와 속도 조정"); its toggles stay outside.
static void FoldingBehaviour() {
    GoToPage(1);
    sw::g_scroll = 0; sw::Render();
    const int group = ControlIndex(L"확대 효과와 속도 조정"), mag = ControlIndex(L"확대 효과"), hide = ControlIndex(L"숨는 속도"),
              autoHide = ControlIndex(L"자동 숨김");
    if (group < 0 || mag < 0 || hide < 0 || autoHide < 0) { Check(false, "the behaviour group and its sliders found"); return; }
    const auto& gc = sw::g_pages[1].controls[group];
    Check(gc.type == sw::Control::Group && gc.count == 3 && mag == group + 1 && hide == group + 3, "the behaviour group folds the three sliders after it");
    float x, y, hx, hy;
    Check(!gc.open && !AnySliderShown() && HotCentre(autoHide, 0, x, y), "folded when the window opens: no slider drawn, the toggles are");
    Check(sw::g_contentH <= sw::ClientH(), "folded, the behaviour page fits the window");
    if (!HotCentre(group, 0, hx, hy)) { Check(false, "behaviour header drawn"); return; }
    // its panel stays clear of the toggle above it and its "기본 켬/끔" mark
    float above = -1, panelTop = -1;
    for (auto& h : sw::g_hots) { if (h.control == group - 1) above = h.r.bottom; if (h.control == group) panelTop = h.r.top; }
    Check(above > 0 && panelTop >= above, "the header's panel starts below the toggle above it");
    g_settings = Settings{}; g_settings.hideDelay = 0.5f;
    ResetCounts(); Unsave();
    for (float dy : { 0.f, 40.f, 80.f, 120.f }) Wheel(hx + 9, hy + dy, -WHEEL_DELTA);
    Drain();
    const Settings d;
    Check(g_settings.hideDelay == 0.5f && g_settings.magnification == d.magnification && g_settings.showDelay == d.showDelay &&
          rebuilds + relayouts == 0 && !Saved(), "the wheel over the folded behaviour group changes no setting, rebuilds and saves nothing");
    Click(hx, hy);
    Check(gc.open && HotCentre(mag, 0, x, y) && HotCentre(hide, 0, x, y) && sw::g_contentH > sw::ClientH(), "a click opens it: the sliders are drawn, the page scrolls");
    sw::g_scroll = sw::g_contentH; sw::ClampScroll(); sw::Render();
    if (!HotCentre(group, 0, hx, hy)) { Check(false, "behaviour header shown while scrolled"); return; }
    const int framesBefore = presents;
    Click(hx, hy);
    Check(!gc.open && !AnySliderShown() && sw::g_scroll == 0 && presents - framesBefore == 1, "a second click folds it, back at the top in one frame");
    // open it for the checks that follow
    if (HotCentre(group, 0, hx, hy)) Click(hx, hy);
    Check(gc.open && AnySliderShown(), "opened for the checks that follow");
    g_settings = Settings{};
    sw::g_scroll = 0;
    GoToPage(0);
    ResetCounts(); Unsave();
}

static void Scrolling() {
    GoToPage(0);
    float x, y; TitlePoint(x, y);
    sw::g_scroll = 0;
    Wheel(x, y, -WHEEL_DELTA);
    Check(std::fabs(sw::g_scroll - 60) < 0.01f, "one wheel notch down scrolls the page 60 DIPs");
    Wheel(x, y, WHEEL_DELTA);
    Check(std::fabs(sw::g_scroll) < 0.01f, "one notch up scrolls it back");
    for (int i = 0; i < 10; ++i) Wheel(x, y, -WHEEL_DELTA);
    Check(sw::g_scroll > 0 && std::fabs(sw::g_scroll - (sw::g_contentH - sw::ClientH())) < 0.5f, "scrolling stops at the end of the page");
    sw::g_scroll = 0; sw::Render();

    // A precision touchpad (a laptop's) and smooth-scrolling mice send WM_MOUSEWHEEL in parts of a notch
    // (dock_render-6: GET_WHEEL_DELTA_WPARAM / WHEEL_DELTA dropped every part, so a slow two-finger scroll
    // never moved the page). The page follows them in proportion.
    TitlePoint(x, y);
    for (int i = 0; i < 3; ++i) Wheel(x, y, -40);
    Check(std::fabs(sw::g_scroll - 60) < 0.01f, "three thirds of a notch (touchpad) scroll the page as one notch");
    sw::g_scroll = 0; sw::Render();
    TitlePoint(x, y);
    for (int i = 0; i < 3; ++i) Wheel(x, y, -80);   // 240 = two notches, in parts that are each less than one
    Check(std::fabs(sw::g_scroll - 120) < 0.01f, "parts of notches add up (3 x 80 = two notches)");
    sw::g_scroll = 0; sw::Render();
    TitlePoint(x, y);
    Wheel(x, y, -40);
    Check(std::fabs(sw::g_scroll - 20) < 0.01f, "a third of a notch scrolls a third of 60 DIPs: smooth, not in jumps");
    sw::g_scroll = 0; sw::Render();
}

static bool SliderAt(float x, float y) {
    const sw::Hot* h = sw::HotAt(x, y);
    return h && h->control >= 0 && sw::g_pages[sw::g_page].controls[h->control].type == sw::Control::Slider;
}
// A point of the page (at scroll 0) with no slider under it, that a slider's middle reaches once the page has
// scrolled `by` DIPs; its control in `control`.
static bool PointAboveSlider(float by, float& x, float& y, int& control) {
    sw::g_scroll = 0; sw::Render();
    for (auto& h : sw::g_hots) {
        if (h.control < 0 || sw::g_pages[sw::g_page].controls[h.control].type != sw::Control::Slider) continue;
        x = (h.r.left + h.r.right) / 2; y = (h.r.top + h.r.bottom) / 2 - by; control = h.control;
        if (y > 0 && !SliderAt(x, y)) return true;
    }
    return false;
}

// A swipe that scrolls the page keeps scrolling it while the pointer rests, even when a slider comes under the
// pointer: the slider is not changed and the docks are not rebuilt halfway through a scroll.
static void WheelGestures() {
    GoToPage(0);
    g_settings = Settings{};
    float x, y; int control = -1;
    if (!PointAboveSlider(60, x, y, control)) { Check(false, "a point above a slider found"); return; }
    const auto& c = sw::g_pages[sw::g_page].controls[control];
    const float before = c.value(), end = std::min(120.f, sw::g_contentH - sw::ClientH());
    ResetCounts(); Unsave();
    for (int i = 0; i < 8; ++i) Wheel(x, y, -30);   // two notches in quarters: after the first, a slider is under the pointer
    Drain();
    Check(std::fabs(sw::g_scroll - end) < 0.01f && c.value() == before && rebuilds == 0 && relayouts == 0 && !Saved(),
          "a slider the scrolling page brings under the resting pointer does not take the rest of the swipe");
    // the pointer moves onto a slider shown now: the wheel works that slider
    const float startY = y;
    int moved = -1;
    for (auto& h : sw::g_hots) {
        const float cy = (h.r.top + h.r.bottom) / 2;
        if (h.control >= 0 && sw::g_pages[sw::g_page].controls[h.control].type == sw::Control::Slider &&
            cy > 20 && cy < sw::ClientH() - 20 && std::fabs(cy - startY) > 4) {
            moved = h.control; x = (h.r.left + h.r.right) / 2; y = cy; break;
        }
    }
    if (moved < 0) { Check(false, "a shown slider found"); return; }
    const auto& m = sw::g_pages[sw::g_page].controls[moved];
    const float was = m.value(), scroll = sw::g_scroll;
    const bool atBottom = was - m.step < m.lo + m.step * 0.01f;   // then a notch up
    const float expect = atBottom ? was + m.step : was - m.step;
    ResetCounts(); Unsave();
    Wheel(x, y, atBottom ? WHEEL_DELTA : -WHEEL_DELTA);
    Drain();
    Check(std::fabs(m.value() - expect) < m.step * 0.01f && sw::g_scroll == scroll && rebuilds + relayouts == 1 && Saved(),
          "once the pointer moves onto a slider the wheel works that slider, one step a notch");
    // parts of a notch on a slider add up to whole steps; a part the other way is dropped when the wheel turns back
    g_settings = Settings{}; sw::g_scroll = 0; sw::Render();
    const int icon = ControlIndex(L"아이콘 크기");
    if (icon < 0 || !HotCentre(icon, 0, x, y)) { Check(false, "icon size slider found again"); return; }
    x += 20;   // off the last wheel's point: a new gesture whatever slider that was
    ResetCounts();
    for (int i = 0; i < 4; ++i) Wheel(x, y, -30);
    Drain();
    Check(g_settings.iconSize == 42 && rebuilds == 1, "four quarters of a notch on the icon size slider: one step, one rebuild");
    g_settings.iconSize = 44; ResetCounts();
    Wheel(x, y, 60); Wheel(x, y, -WHEEL_DELTA);
    Drain();
    Check(g_settings.iconSize == 42 && rebuilds == 1, "half a notch up, then a notch down: one step down");
    g_settings = Settings{};
    sw::g_scroll = 0; sw::Render();
}

// Wheel messages that wait in the queue together (a touchpad sends one for every part of a notch, and they pile
// up while the loop is busy): the page is drawn once, by the last. Present waits for the screen's next frame once
// a frame is queued, so drawing each one held the docks' loop (the same thread) a frame apiece.
static void PostWheel(float x, float y, int delta) {
    POINT p = ClientPx(x, y); ClientToScreen(sw::g_wnd, &p);
    PostMessageW(sw::g_wnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(short)delta), MAKELPARAM((WORD)(short)p.x, (WORD)(short)p.y));
}
static void PostMouse(UINT msg, float x, float y) {
    POINT p = ClientPx(x, y);
    PostMessageW(sw::g_wnd, msg, msg == WM_LBUTTONDOWN ? MK_LBUTTON : 0, MAKELPARAM((WORD)(short)p.x, (WORD)(short)p.y));
}
static double Pump() {   // as the dock's loop: everything waiting, then back; the time it took
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    MSG msg;
    while (PeekMessageW(&msg, sw::g_wnd, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    QueryPerformanceCounter(&b);
    return double(b.QuadPart - a.QuadPart) / f.QuadPart * 1000;
}
static bool HotTop(int control, int sub, float& top) {
    for (auto& h : sw::g_hots) if (h.control == control && h.sub == sub) { top = h.r.top; return true; }
    return false;
}
static void WheelBursts() {
    GoToPage(0);
    g_settings = Settings{};
    sw::g_scroll = 0; sw::Render();
    const int glass = ControlIndex(L"유리 질감");
    float x, y, top0 = 0, top = 0;
    if (glass < 0 || !HotTop(glass, 2, top0)) { Check(false, "glass texture control found"); return; }
    TitlePoint(x, y);
    // one by one, as the wheel came when the loop kept up: a frame each
    int was = presents;
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    for (int i = 0; i < 12; ++i) Wheel(x, y, -20);   // 12 sixths of a notch: two notches, 120 DIPs
    QueryPerformanceCounter(&b);
    const double oneByOne = double(b.QuadPart - a.QuadPart) / f.QuadPart * 1000;
    const int framesOneByOne = presents - was;
    Check(std::fabs(sw::g_scroll - 120) < 0.01f && framesOneByOne == 12, "12 parts of notches one at a time: 120 DIPs, a frame each");
    sw::g_scroll = 0; sw::Render();
    // the same twelve waiting together
    for (int i = 0; i < 12; ++i) PostWheel(x, y, -20);
    was = presents;
    const double together = Pump();
    printf("  12 wheel messages: one by one %d frames in %.1f ms, waiting together %d frame(s) in %.1f ms\n", framesOneByOne, oneByOne, presents - was, together);
    Check(std::fabs(sw::g_scroll - 120) < 0.01f && presents - was == 1, "12 parts of notches waiting together: the same 120 DIPs, one frame");
    Check(HotTop(glass, 2, top) && std::fabs(top - (top0 - 120)) < 0.01f, "...and the hot areas are those of the page scrolled 120 DIPs");
    // past the end of the page: the messages that scroll nothing still leave the page drawn where it stopped
    sw::g_scroll = 0; sw::Render();
    const float end = sw::g_contentH - sw::ClientH();
    for (int i = 0; i < 30; ++i) PostWheel(x, y, -WHEEL_DELTA);
    was = presents;
    Pump();
    Check(std::fabs(sw::g_scroll - end) < 0.5f && presents - was == 1 && HotTop(glass, 2, top) && std::fabs(top - (top0 - sw::g_scroll)) < 0.01f,
          "a burst past the end of the page: drawn once, where the page stopped");
    // a click between waiting wheel messages lands on what is under the pointer on the page scrolled so far
    sw::g_scroll = 0; sw::Render();
    float gx, gy;
    if (!HotCentre(glass, 2, gx, gy)) { Check(false, "glass texture segment found"); return; }
    g_settings.glassStyle = 1;
    PostWheel(x, y, -WHEEL_DELTA);            // 60 DIPs
    PostMouse(WM_LBUTTONDOWN, gx, gy - 60);   // where the frosted segment is now
    PostMouse(WM_LBUTTONUP, gx, gy - 60);
    PostWheel(x, y, -WHEEL_DELTA);
    ResetCounts();
    Pump();
    Drain();
    Check(g_settings.glassStyle == 2 && std::fabs(sw::g_scroll - 120) < 0.01f, "a click between waiting wheel messages presses what the scrolled page shows under it");
    // a new gesture (the pointer moved) between waiting messages finds the slider on the page as it is now
    sw::g_scroll = 0; sw::Render();
    const int icon = ControlIndex(L"아이콘 크기");
    float ix, iy;
    if (icon < 0 || !HotCentre(icon, 0, ix, iy) || iy - 60 < 10) { Check(false, "icon size slider found for a burst"); return; }
    g_settings.iconSize = 44;
    PostWheel(x, y, -WHEEL_DELTA);           // the page scrolls 60 DIPs
    PostWheel(ix, iy - 60, -WHEEL_DELTA);    // the pointer moved onto the slider where it is now
    ResetCounts();
    Pump();
    Drain();
    Check(std::fabs(sw::g_scroll - 60) < 0.01f && g_settings.iconSize == 42 && rebuilds == 1, "a wheel gesture begun between waiting messages works the slider the scrolled page shows");
    g_settings = Settings{};
    sw::g_scroll = 0; sw::Render();
    ResetCounts(); Unsave();
}

static void WheelOnSliders() {
    GoToPage(0);
    sw::g_scroll = 0; sw::Render();
    const int icon = ControlIndex(L"아이콘 크기"), edgeGap = ControlIndex(L"화면 가장자리와 거리");
    float x = 0, y = 0;
    if (icon < 0 || !HotCentre(icon, 0, x, y)) { Check(false, "icon size slider found"); return; }

    g_settings.iconSize = 44; ResetCounts(); Unsave();
    Wheel(x, y, -WHEEL_DELTA);
    Drain();
    Check(g_settings.iconSize == 42 && rebuilds == 1 && Saved(), "a notch down on the icon size slider: 42, docks rebuilt once, saved");
    g_settings.iconSize = 44; ResetCounts(); Unsave();
    Wheel(x, y, 2 * WHEEL_DELTA);
    Drain();
    Check(g_settings.iconSize == 48 && rebuilds == 1, "two notches at once: two steps, one rebuild");
    if (edgeGap >= 0 && HotCentre(edgeGap, 0, x, y)) {
        g_settings.edgeGap = 6; ResetCounts();
        Wheel(x, y, WHEEL_DELTA);
        Drain();
        Check(g_settings.edgeGap == 8 && relayouts == 1 && rebuilds == 0, "the edge gap slider lays the docks out in place");
    }
    HotCentre(icon, 0, x, y);

    // A wheel message over a slider whose value does not change saves and rebuilds nothing (dock_render-7: it
    // rewrote settings.ini and rebuilt every dock - new windows, swap chains and screen captures - for each one:
    // at the slider's end, or for a part of a notch, which a touchpad sends dozens of per gesture).
    g_settings.iconSize = 64; ResetCounts(); Unsave();   // the slider's maximum
    Wheel(x, y, WHEEL_DELTA);
    Drain();
    Check(g_settings.iconSize == 64 && rebuilds == 0, "a notch up at the icon size maximum does not rebuild the docks");
    Check(!Saved(), "a notch up at the icon size maximum does not rewrite settings.ini");
    g_settings.iconSize = 44; ResetCounts(); Unsave();
    for (int i = 0; i < 2; ++i) Wheel(x, y, 30);   // half a notch from a touchpad: no step yet
    Drain();
    Check(g_settings.iconSize == 44 && rebuilds == 0 && !Saved(), "half a notch (touchpad) changes nothing, so rebuilds and saves nothing");
    g_settings.iconSize = 80; ResetCounts(); Unsave();   // edited by hand: settings.ini takes up to 96, the slider ends at 64
    Wheel(x + 5, y, 30);   // a new gesture, a quarter of a notch
    Drain();
    Check(g_settings.iconSize == 80 && rebuilds == 0 && !Saved(), "a quarter of a notch keeps an icon size beyond the slider, so rebuilds and saves nothing");
    Wheel(x + 5, y, -WHEEL_DELTA);
    Drain();
    Check(g_settings.iconSize == 64 && rebuilds == 1 && Saved(), "a whole notch down brings it to the slider's end");
    g_settings.magnification = 2.1f;
    GoToPage(1);
    ResetCounts();
    const int mag = ControlIndex(L"확대 효과");
    if (mag >= 0 && HotCentre(mag, 0, x, y)) {
        Wheel(x, y, WHEEL_DELTA);
        Drain();
        Check(g_settings.magnification == 2.1f && rebuilds == 0, "a notch up at the magnification maximum does not rebuild the docks");
    }
    g_settings = Settings{};
    sw::g_scroll = 0;
    GoToPage(0);
}

static void Controls() {
    float x, y;
    // a slider drag: the docks change size once, when it is let go
    GoToPage(0);
    sw::g_scroll = 0; sw::Render();
    const int icon = ControlIndex(L"아이콘 크기");
    g_settings.iconSize = 44; ResetCounts(); Unsave();
    if (icon >= 0 && HotCentre(icon, 0, x, y)) {
        const auto track = sw::g_sliderTrack[icon];
        Mouse(WM_LBUTTONDOWN, track.left + (track.right - track.left) * 0.5f, y);
        Check(captured == sw::g_wnd && sw::g_drag == icon, "a press on the slider starts a drag");
        for (int i = 0; i <= 10; ++i) Mouse(WM_MOUSEMOVE, track.left + (track.right - track.left) * (0.5f + i * 0.05f), y);
        Drain();
        const int during = rebuilds;
        Mouse(WM_LBUTTONUP, track.right, y);
        Drain();
        Check(during == 0 && rebuilds == 1 && Saved() && g_settings.iconSize == 64 && sw::g_drag < 0 && !captured,
              "dragged to the end: 64, docks rebuilt once when let go, saved");
    }
    // segmented choices
    const int glass = ControlIndex(L"유리 질감");
    if (glass >= 0) {
        sw::g_scroll = sw::g_contentH; sw::ClampScroll(); sw::Render();   // the last controls are below the fold
        ResetCounts();
        if (HotCentre(glass, 2, x, y)) Click(x, y);
        Drain();
        Check(g_settings.glassStyle == 2 && relayouts == 1 && rebuilds == 0, "glass texture: frosted, laid out in place");
        if (HotCentre(glass, 2, x, y)) Click(x, y);
        Drain();
        Check(relayouts == 1, "choosing the segment already chosen does nothing");
    }
    // the page's reset
    ResetCounts();
    g_settings.iconSize = 60; g_settings.dockThickness = 140; g_settings.glassStyle = 0;
    sw::Render();
    if (HotCentre(-2, 0, x, y)) Click(x, y);
    Drain();
    Check(g_settings.iconSize == 44 && g_settings.dockThickness == 100 && g_settings.glassStyle == 1 && rebuilds == 1,
          "\"기본값으로\" restores the shape page and rebuilds the docks");
    // toggles
    GoToPage(1);
    sw::g_scroll = 0; sw::Render();
    const int autoHide = ControlIndex(L"자동 숨김");
    ResetCounts(); Unsave();
    if (autoHide >= 0 && HotCentre(autoHide, 0, x, y)) Click(x, y);
    Drain();
    Check(!g_settings.autoHide && relayouts == 1 && Saved(), "auto-hide off: laid out in place and saved");
    if (HotCentre(autoHide, 0, x, y)) Click(x, y);
    Check(g_settings.autoHide, "and on again");
    // the button customizer: a click on a listed button adds it at the end
    GoToPage(2);
    sw::g_scroll = 0;
    g_settings.trayButtons = "desktop"; sw::Render();
    ResetCounts();
    if (HotCentre(sw::kHotPalette - (int)ButtonId::Trash, 0, x, y)) Click(x, y);
    Drain();
    Check(g_settings.trayButtons == "desktop,trash" && relayouts == 1 && !captured, "a click on a listed button adds it at the end");
    // a drag of the first button off the preview takes it away
    sw::Render();
    if (HotCentre(sw::kHotDock - 0, 0, x, y)) {
        Mouse(WM_LBUTTONDOWN, x, y);
        Mouse(WM_MOUSEMOVE, x + 10, y);
        Mouse(WM_MOUSEMOVE, x, sw::g_preview.bottom + 120);
        Mouse(WM_LBUTTONUP, x, sw::g_preview.bottom + 120);
        Check(g_settings.trayButtons == "trash" && !sw::g_bd.armed, "a button dragged off the preview is taken away");
    }
    g_settings = Settings{};
    GoToPage(0);
}

// The 센서 page (after 시스템, the pages before it keep their places): the source "온도 읽기" (끄기 / HWiNFO Gadget /
// PawnIO) and the folded group "PC 상태에 보일 항목" with its six switches, each rebuilding the docks; the old PawnIO
// switch is gone from 시스템. PawnIO itself is never chosen here (its UAC prompt).
static bool Reveal(int control, int sub, float& x, float& y);   // (below) a control's hot area scrolled into view
static void SensorPage() {
    float x, y;
    Check(sw::g_pages.size() == 5 && !wcscmp(sw::g_pages[3].title, L"시스템") && !wcscmp(sw::g_pages[4].title, L"센서") &&
          !wcscmp(sw::g_pages[0].title, L"모양") && !wcscmp(sw::g_pages[1].title, L"동작") && sw::g_pages[4].glyph && sw::g_pages[4].glyph[0],
          "five pages: the 센서 page (with its icon) after 시스템, the others where they were");
    bool noOld = true;
    for (const auto& c : sw::g_pages[3].controls) if (wcsstr(c.label, L"PawnIO") || wcsstr(c.label, L"CPU 온도")) noOld = false;
    Check(noOld, "the 시스템 page no longer has the \"CPU 온도 직접 읽기 (PawnIO)\" switch");
    GoToPage(4);
    sw::g_scroll = 0; sw::Render();
    Check(sw::g_page == 4, "the nav item opens the 센서 page");
    const int source = ControlIndex(L"온도 읽기"), group = ControlIndex(L"PC 상태에 보일 항목");
    const wchar_t* labels[] = { L"CPU 온도", L"GPU", L"VRAM", L"VRAM 온도", L"RAM 슬롯 온도", L"SSD" };
    if (source < 0 || group < 0) { Check(false, "the 센서 page's controls found"); return; }
    const auto& page = sw::g_pages[4];
    const auto& sc = page.controls[source];
    Check(source == 0 && sc.type == sw::Control::Segmented && sc.options.size() == 3 && !wcscmp(sc.options[0], L"끄기") &&
          !wcscmp(sc.options[1], L"HWiNFO Gadget") && !wcscmp(sc.options[2], L"PawnIO") && sc.apply == sw::Apply::Rebuild && sc.def == 1,
          "first: the segmented \"온도 읽기\" - 끄기 / HWiNFO Gadget / PawnIO, its default HWiNFO Gadget, rebuilding the docks");
    const auto& gc = page.controls[group];
    bool toggles = gc.type == sw::Control::Group && gc.count == 6 && group == 1 && gc.hint;
    for (int i = 0; i < 6; ++i) {
        const int k = ControlIndex(labels[i]);
        toggles &= k == group + 1 + i && page.controls[k].type == sw::Control::Toggle && page.controls[k].apply == sw::Apply::Rebuild &&
                   page.controls[k].hasDefault && page.controls[k].def == 1;
    }
    Check(toggles, "then the group \"PC 상태에 보일 항목\" folding its six switches (CPU 온도, GPU, VRAM, VRAM 온도, RAM 슬롯 온도, SSD), each on by default, rebuilding");
    Check(page.reset != nullptr, "the 센서 page has \"기본값으로\"");

    // the source: off, and HWiNFO again
    g_settings = Settings{};
    ResetCounts(); Unsave();
    if (HotCentre(source, 0, x, y)) Click(x, y);
    Drain();
    Check(g_settings.sensorSource == 0 && rebuilds == 1 && Saved(), "온도 읽기 끄기: sensorSource 0, the docks rebuilt, saved");
    ResetCounts();
    if (HotCentre(source, 1, x, y)) Click(x, y);
    Drain();
    Check(g_settings.sensorSource == 1 && rebuilds == 1, "HWiNFO Gadget again: sensorSource 1, rebuilt");

    // folded: no switch drawn; opened, a switch rebuilds and saves
    sw::Render();
    const int gpu = ControlIndex(L"GPU"), ssd = ControlIndex(L"SSD");
    Check(!gc.open && !HotCentre(gpu, 0, x, y), "folded when the window opens: the switches are not drawn");
    if (HotCentre(group, 0, x, y)) Click(x, y);
    Check(gc.open && HotCentre(gpu, 0, x, y), "a click on the header opens it");
    ResetCounts(); Unsave();
    if (HotCentre(gpu, 0, x, y)) Click(x, y);
    Drain();
    Check(!g_settings.showGpu && rebuilds == 1 && Saved(), "GPU off: showGpu 0, the docks rebuilt (the window's thickness follows), saved");
    sw::g_scroll = sw::g_contentH; sw::ClampScroll(); sw::Render();
    ResetCounts();
    if (HotCentre(ssd, 0, x, y)) Click(x, y);
    Drain();
    Check(!g_settings.showSsd && rebuilds == 1, "SSD off (below the fold): showSsd 0, rebuilt");

    // the power draw (mod.40): after the group the segmented "전력 소비량" (없음 / 그래프 / 요약, 없음 by default), the docks
    // laid out again in place (the panel's height does not change: no rebuild); under it "CPU 전력" and "GPU 전력",
    // on by default, drawn only while it is not 없음
    const int power = ControlIndex(L"전력 소비량"), cpuPower = ControlIndex(L"CPU 전력"), gpuPower = ControlIndex(L"GPU 전력");
    if (power < 0 || cpuPower < 0 || gpuPower < 0) Check(false, "the 센서 page's power controls found");
    else {
        const auto& pc = page.controls[power];
        bool shape = power == group + 7 && pc.type == sw::Control::Segmented && pc.options.size() == 3 && !wcscmp(pc.options[0], L"없음") &&
                     !wcscmp(pc.options[1], L"그래프") && !wcscmp(pc.options[2], L"요약") && pc.apply == sw::Apply::Live && pc.def == 0 && pc.hint && !pc.shown &&
                     cpuPower == power + 1 && gpuPower == power + 2;
        for (int k : { cpuPower, gpuPower })
            shape &= page.controls[k].type == sw::Control::Toggle && page.controls[k].apply == sw::Apply::Live && page.controls[k].def == 1 && page.controls[k].shown;
        Check(shape, "after the group: \"전력 소비량\" 없음 / 그래프 / 요약 (기본 없음), then the switches \"CPU 전력\" and \"GPU 전력\" (기본 켬), all laying the docks out again");
        g_settings = Settings{};
        Check(!Reveal(cpuPower, 0, x, y) && !Reveal(gpuPower, 0, x, y) && Reveal(power, 1, x, y), "없음: the two switches are not drawn (no hot area)");
        const float pageWithout = sw::g_contentH;
        ResetCounts(); Unsave();
        if (Reveal(power, 1, x, y)) Click(x, y);
        Drain();
        Check(g_settings.powerView == 1 && relayouts == 1 && rebuilds == 0 && Saved(), "그래프: powerView 1, the docks laid out again (not rebuilt), saved");
        sw::Render();
        Check(sw::g_contentH > pageWithout + 100 && Reveal(cpuPower, 0, x, y) && Reveal(gpuPower, 0, x, y), "...the two switches drawn under it, the page longer by them");
        ResetCounts(); Unsave();
        if (Reveal(cpuPower, 0, x, y)) Click(x, y);
        Drain();
        Check(!g_settings.showCpuPower && g_settings.showGpuPower && relayouts == 1 && rebuilds == 0 && Saved(), "CPU 전력 off: showCpuPower 0, laid out again, saved");
        ResetCounts();
        if (Reveal(power, 2, x, y)) Click(x, y);
        Drain();
        Check(g_settings.powerView == 2 && Reveal(gpuPower, 0, x, y), "요약: powerView 2, the switches still drawn");
        // scrolled to the page's end, 없음 hides the two switches: the scroll clamped before the one frame the click
        // draws (no frame at the old scroll with empty room under "기본값으로", no jump a frame later)
        sw::g_scroll = sw::g_contentH; sw::ClampScroll(); sw::Render();
        const float endWith = sw::g_scroll;
        if (!HotCentre(power, 0, x, y)) Check(false, "\"전력 소비량\" drawn at the page's end");
        else {
            const int framesBefore = presents;
            Click(x, y);
            const int frames = presents - framesBefore;
            Drain();
            Check(g_settings.powerView == 0 && endWith > 100 && std::fabs(sw::g_scroll - std::max(0.f, sw::g_contentH - sw::ClientH())) < 0.01f &&
                  frames == 1, "...scrolled to the end, 없음: the page shorter by the switches, at its new end in one frame");
        }
        g_settings.powerView = 2; sw::g_scroll = 0; sw::Render();
        if (Reveal(power, 0, x, y)) Click(x, y);
        Drain();
        Check(g_settings.powerView == 0 && !Reveal(cpuPower, 0, x, y) && !g_settings.showCpuPower, "없음 again: the switches hidden (their values kept)");
        sw::g_scroll = 0; sw::Render();
    }

    // the page's reset: the source, the six switches, the power view and its two switches
    g_settings.sensorSource = 0; g_settings.showCpuTemp = false; g_settings.showVramTemp = false; g_settings.showRamTemp = false; g_settings.showVram = false;
    g_settings.powerView = 2; g_settings.showCpuPower = false; g_settings.showGpuPower = false;
    sw::Render();
    ResetCounts();
    float rx, ry;
    if (Reveal(-2, 0, rx, ry)) Click(rx, ry);
    Drain();
    const Settings d;
    Check(g_settings.sensorSource == 1 && g_settings.showCpuTemp && g_settings.showGpu && g_settings.showVram && g_settings.showVramTemp &&
          g_settings.showRamTemp && g_settings.showSsd && g_settings.powerView == 0 && g_settings.showCpuPower && g_settings.showGpuPower && rebuilds == 1,
          "\"기본값으로\" restores the source HWiNFO Gadget, every switch and the power view 없음 with both its switches, rebuilt");
    sw::g_scroll = 0; sw::Render();
    if (HotCentre(group, 0, x, y)) Click(x, y);   // folded again
    g_settings = Settings{};
    sw::g_scroll = 0;
    GoToPage(0);
    ResetCounts(); Unsave();
}

static void MovingMonitors() {
    // dragged onto a desktop's 2560x1440 monitor (another Windows scale): the content follows its resolution
    RECT r = { 100, 100, 900, 800 };
    movedTo = kDesktop;
    SendMessageW(sw::g_wnd, WM_DPICHANGED, MAKEWPARAM(96, 96), (LPARAM)&r);
    RECT rc; GetClientRect(sw::g_wnd, &rc);
    Check(std::fabs(sw::g_dpi - 96) < 0.01f && rc.right == (LONG)sw::kW && rc.bottom == (LONG)sw::kH && sw::g_target,
          "on the 2560x1440 monitor: 96 dpi, 660x600 px, a new target");
    // the hot areas follow the new scale: a wheel notch over the title still scrolls
    float x, y; TitlePoint(x, y);
    sw::g_scroll = 0; sw::Render();
    Wheel(x, y, -WHEEL_DELTA);
    Check(std::fabs(sw::g_scroll - 60) < 0.01f, "at 96 dpi a notch still scrolls 60 DIPs");
    sw::g_scroll = 0;
    movedTo = kLaptop;
    SendMessageW(sw::g_wnd, WM_DPICHANGED, MAKEWPARAM(144, 144), (LPARAM)&r);
    movedTo = nullptr;
    Check(std::fabs(sw::g_dpi - 120) < 0.01f, "back on the laptop: 120 dpi again");
}

static void Closing() {
    Unsave();
    PostMessageW(sw::g_wnd, WM_KEYDOWN, VK_ESCAPE, 0);
    MSG msg;
    while (PeekMessageW(&msg, sw::g_wnd, 0, 0, PM_REMOVE)) {
        if (sw::PreTranslate(&msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    Check(!sw::IsOpen() && !g_settingsHold.open && Saved() && !sw::g_swap, "Esc closes it: saved, docks told, its device resources released");
}

// ================================================================ characterization: what the code does now
// What the settings window does now, pinned before its Render, Proc and DrawCustomizer are split up: every page's
// controls and hot areas, each control's setter and the page resets, the messages the tests above never send,
// the button customizer, and the pixels. Expectations are this code's own output (right or wrong); long ones are a
// hash, printed as "HASH: <name> <hex>" for comparing builds on one PC (the pixels: only that), the drawing calls
// and hot areas (no font or GPU in them) also against the hashes written here.

static const HMONITOR kBig = (HMONITOR)(uintptr_t)0x3003;   // 3840x2160: content scale 1.5
struct ScaleCase { HMONITOR m; const char* name; float dpi; };
static const ScaleCase kScales[] = { { kDesktop, "s100", 96 }, { kLaptop, "s125", 120 }, { kBig, "s150", 144 } };
static const D2D1_COLOR_F kAccent = { 0.0f, 0.47f, 0.83f, 1.0f };   // fixed: the real one is read from the registry

static uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ULL) {
    for (size_t i = 0; i < n; ++i) { h ^= ((const unsigned char*)p)[i]; h *= 1099511628211ULL; }
    return h;
}
static uint64_t Fnv(const std::wstring& s) { return Fnv(s.data(), s.size() * sizeof(wchar_t)); }

static bool grab = false, grabOk = false;
static uint64_t grabHash = 0;
static void OnPresent() {   // the frame just drawn (still in the back buffer), the version line blanked
    if (!grab) return;
    grab = false; grabOk = false; grabHash = 0;
    if (!sw::g_target) return;
    auto* dc = g_gfx.dc.Get();
    const D2D1_SIZE_U size = sw::g_target->GetPixelSize();
    auto rp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> read;
    D2D1_MAPPED_RECT m{};
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, &rp, &read)) || FAILED(read->CopyFromBitmap(nullptr, sw::g_target.Get(), nullptr)) ||
        FAILED(read->Map(D2D1_MAP_OPTIONS_READ, &m))) return;
    // "버전 0.2.0-mod.N" (settings_window.cpp Render: { 22, 42, kNav, 60 }, clipped to it): blanked, so a new version
    // keeps every hash
    const float s = sw::g_dpi / 96;
    const int vx0 = (int)std::floor(22 * s), vx1 = (int)std::ceil(sw::kNav * s), vy0 = (int)std::floor(42 * s), vy1 = (int)std::ceil(60 * s);
    uint64_t h = Fnv(&size, sizeof(size));
    std::vector<uint32_t> row(size.width);
    for (UINT32 y = 0; y < size.height; ++y) {
        memcpy(row.data(), m.bits + (size_t)y * m.pitch, size.width * 4);
        if ((int)y >= vy0 && (int)y < vy1) for (int x = vx0; x < vx1 && x < (int)size.width; ++x) row[x] = 0;
        h = Fnv(row.data(), row.size() * 4, h);
    }
    read->Unmap();
    grabHash = h; grabOk = true;
}

static std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::string G(float v) { char b[32]; snprintf(b, sizeof b, "%g", v); return b; }
static std::wstring F3(float v) { if (std::isnan(v)) return L"nan"; wchar_t b[32]; swprintf_s(b, L"%.3f", v); return b; }
static std::wstring R4(const D2D1_RECT_F& r) { return F3(r.left) + L"," + F3(r.top) + L"," + F3(r.right) + L"," + F3(r.bottom); }

// A check against the expectation written here; on a mismatch both are printed.
static void Pin(const std::string& what, const std::string& got, const std::string& want) {
    const bool ok = got == want;
    Check(ok, (what + ": " + want).c_str());
    if (!ok) printf("  GOT %s\n", got.c_str());
}

// ---------------------------------------------------------------- the settings and what reached the docks

static std::vector<std::pair<std::string, std::string>> Fields(const Settings& s) {
    auto i = [](long long v) { return std::to_string(v); };
    return {
        { "edge", i((int)s.edge) }, { "iconSize", G(s.iconSize) }, { "magnification", G(s.magnification) }, { "glassStyle", i(s.glassStyle) },
        { "trayButtons", s.trayButtons }, { "showDesktopButton", i(s.showDesktopButton) }, { "showThemeButton", i(s.showThemeButton) },
        { "showOledButton", i(s.showOledButton) }, { "oledOneMonitor", i(s.oledOneMonitor) }, { "trayIcons", i(s.trayIcons) },
        { "fluentFlyout", i(s.fluentFlyout) }, { "clockSize", i(s.clockSize) }, { "clockFormat", i(s.clockFormat) }, { "sensorSize", i(s.sensorSize) }, { "iconSpacing", i(s.iconSpacing) }, { "iconGap", i(s.iconGap) },
        { "dockThickness", i(s.dockThickness) }, { "edgeGap", i(s.edgeGap) }, { "edgeOffset", i(s.edgeOffset) }, { "autoHide", i(s.autoHide) },
        { "secondaryAlwaysShown", i(s.secondaryAlwaysShown) }, { "showWithStart", i(s.showWithStart) }, { "showDelay", G(s.showDelay) },
        { "hideDelay", G(s.hideDelay) }, { "autostart", i(s.autostart) }, { "startMenuExtras", i(s.startMenuExtras) },
        { "shiftRunsAsAdmin", i(s.shiftRunsAsAdmin) }, { "taskbarAppButtons", i(s.taskbarAppButtons) }, { "revealHotkey", i(s.revealHotkey) },
        { "sensorSource", i(s.sensorSource) }, { "cpuTemp", i(s.cpuTemp) }, { "sourceFromCpuTemp", i(s.sourceFromCpuTemp) },
        { "showCpuTemp", i(s.showCpuTemp) }, { "showGpu", i(s.showGpu) }, { "showVram", i(s.showVram) }, { "showVramTemp", i(s.showVramTemp) },
        { "showRamTemp", i(s.showRamTemp) }, { "showSsd", i(s.showSsd) }, { "powerView", i(s.powerView) },
        { "showCpuPower", i(s.showCpuPower) }, { "showGpuPower", i(s.showGpuPower) }, { "adminWindows", i(s.adminWindows) },
        { "hwinfoCpuLabel", s.hwinfoCpuLabel }, { "pinsVersion", i(s.pinsVersion) },
    };
}
struct World { Settings s; bool oled, light; HMONITOR oledMon; };
static World Now() { return { g_settings, oledOn, themeLight, oledFor }; }
static std::string MonName(HMONITOR m) {
    return m == kLaptop ? "laptop" : m == kDesktop ? "desktop" : m == kBig ? "big" : m == nullptr ? "all" : "never";
}
// What changed from a to b: "field=old>new ...", "-" for nothing.
static std::string Diff(const World& a, const World& b) {
    std::string out;
    auto add = [&](const std::string& s) { out += (out.empty() ? "" : " ") + s; };
    const auto fa = Fields(a.s), fb = Fields(b.s);
    for (size_t k = 0; k < fa.size(); ++k) if (fa[k].second != fb[k].second) add(fa[k].first + "=" + fa[k].second + ">" + fb[k].second);
    if (a.oled != b.oled) add(std::string("oled=") + (b.oled ? "on" : "off"));
    if (a.light != b.light) add(std::string("theme=") + (b.light ? "light" : "dark"));
    if (a.oledMon != b.oledMon) add("oledFor=" + MonName(b.oledMon));
    return out.empty() ? "-" : out;
}
// What the settings window posted to the docks since ResetCounts, and whether settings.ini was written.
static std::string Posted() {
    Drain();
    std::string s;
    auto add = [&](const char* n, int v) { if (v) s += (s.empty() ? "" : " ") + std::string(n) + std::to_string(v); };
    add("rebuild", rebuilds); add("relayout", relayouts); add("trayhost", trayHosts); add("close", closes); add("autostart", autostarts); add("hotkey", hotkeys);
    if (Saved()) s += s.empty() ? "saved" : " saved";
    return s.empty() ? "none" : s;
}
static void Fresh() { ResetCounts(); Unsave(); }

// ---------------------------------------------------------------- opening, pages, hover, a frame

static bool OpenAt(HMONITOR m, bool light) {
    if (sw::IsOpen()) sw::Close();
    sw::g_bd = {}; captured = nullptr;
    themeLight = light;
    sw::Show(m);
    Drain();
    if (!sw::IsOpen()) return false;
    sw::g_accent = kAccent; sw::g_acrylic = true;
    sw::Render();
    return true;
}
static bool HasGroup(int p) { for (auto& c : sw::g_pages[p].controls) if (c.type == sw::Control::Group) return true; return false; }
// Opens (or folds) every group of the page shown, by a click on its header at the top of the page.
static void SetGroups(bool open) {
    const auto& cs = sw::g_pages[sw::g_page].controls;
    for (int i = 0; i < (int)cs.size(); ++i) {
        if (cs[i].type != sw::Control::Group || cs[i].open == open) continue;
        sw::g_scroll = 0; sw::Render();
        float x, y;
        if (HotCentre(i, 0, x, y)) Click(x, y);
    }
    sw::g_scroll = 0; sw::Render();
}
static void HoverOff() { SendMessageW(sw::g_wnd, WM_MOUSELEAVE, 0, 0); }
static bool HoverOn(int control, int sub) { float x, y; if (!HotCentre(control, sub, x, y)) return false; Mouse(WM_MOUSEMOVE, x, y); return true; }
static bool Esc() { MSG m = { sw::g_wnd, WM_KEYDOWN, VK_ESCAPE, 0 }; return sw::PreTranslate(&m); }
// The control's hot area brought into view (scrolled when it is below the fold); its centre.
static bool Reveal(int control, int sub, float& x, float& y) {
    sw::g_scroll = 0; sw::Render();
    float top;
    if (!HotTop(control, sub, top)) return false;
    if (top > sw::ClientH() - 80) { sw::g_scroll = top - 200; sw::ClampScroll(); sw::Render(); }
    return HotCentre(control, sub, x, y);
}

static const char* TypeName(int t) {
    static const char* n[] = { "Segmented", "Slider", "Toggle", "Button", "Customizer", "Group" };
    return t >= 0 && t < 6 ? n[t] : "?";
}
static const wchar_t* ApplyName(sw::Apply a) {
    return a == sw::Apply::None ? L"None" : a == sw::Apply::Live ? L"Live" : a == sw::Apply::OnRelease ? L"OnRelease" : L"Rebuild";
}
// Every control of page p as it is now: type, label, hint, how it applies, its default, and its state.
static std::wstring ControlTable(int p) {
    const auto& page = sw::g_pages[p];
    std::wstring o = L"page [" + std::wstring(page.title) + L"] glyph";
    for (const wchar_t* g = page.glyph; g && *g; ++g) { wchar_t b[8]; swprintf_s(b, L" %04X", *g); o += b; }
    o += page.reset ? L" reset\n" : L" no-reset\n";
    for (int i = 0; i < (int)page.controls.size(); ++i) {
        const auto& c = page.controls[i];
        const char* type = TypeName(c.type);
        o += std::to_wstring(i) + L" " + std::wstring(type, type + strlen(type)) +
             L" [" + c.label + L"] hint[" + (c.hint ? c.hint : L"") + L"] apply " + ApplyName(c.apply) + L" def " + F3(c.def) + (c.hasDefault ? L"" : L" no-default");
        switch (c.type) {
        case sw::Control::Segmented: {
            o += L" options";
            for (auto* s : c.options) o += std::wstring(L" |") + s;
            o += L" index " + std::to_wstring(c.index());
            break;
        }
        case sw::Control::Slider:
            o += L" lo " + F3(c.lo) + L" hi " + F3(c.hi) + L" step " + F3(c.step) + L" value " + F3(c.value()) +
                 L" text " + c.text(c.lo) + L"|" + c.text(c.def) + L"|" + c.text(c.hi) + L"|" + c.text(c.value());
            break;
        case sw::Control::Toggle: o += std::wstring(L" on ") + (c.on() ? L"1" : L"0"); break;
        case sw::Control::Group: o += L" count " + std::to_wstring(c.count) + (c.open ? L" open" : L" folded"); break;
        default: break;
        }
        o += L"\n";
    }
    return o;
}
// The hot areas of the last frame, the slider tracks drawn, the page's length and the customizer's drop places.
static std::wstring HotDump() {
    std::wstring o;
    for (auto& h : sw::g_hots) {
        o += std::to_wstring(h.control) + L":" + std::to_wstring(h.sub) + L" " + R4(h.r) + L"\n";
        if (h.control >= 0 && sw::g_pages[sw::g_page].controls[h.control].type == sw::Control::Slider)
            o += L"track " + R4(sw::g_sliderTrack[h.control]) + L"\n";
    }
    o += L"contentH " + F3(sw::g_contentH) + L" clientH " + F3(sw::ClientH()) + L" scroll " + F3(sw::g_scroll) + L"\n";
    if (sw::g_page == 2) {
        o += L"preview " + R4(sw::g_preview) + L" drops";
        for (float c : sw::g_dropCentres) o += L" " + F3(c);
        o += L"\n";
    }
    return o;
}
struct Shot { uint64_t draws = 0, pixels = 0, hots = 0, fonts = 0; bool pixelsOk = false; };
static Shot Shoot() {   // one frame of the page as it is: its drawing calls, pixels and hot areas (and text formats)
    recOps.clear(); recTexts.clear(); recFonts.clear();
    recOn = true; grab = true;
    sw::Render();
    recOn = false; grab = false;
    Shot s;
    s.draws = Fnv(recOps); s.pixels = grabHash; s.pixelsOk = grabOk; s.hots = Fnv(HotDump()); s.fonts = Fnv(recFonts);
    return s;
}
// A text format as text (RecFont): family ("<glyph>": the dock's icon font, which this PC resolved), size, weight,
// text and paragraph alignment, word wrapping.
static std::wstring FontText(IDWriteTextFormat* f) {
    if (!f) return L"none";
    wchar_t fam[64] = {}, glyph[64] = {};
    f->GetFontFamilyName(fam, ARRAYSIZE(fam));
    if (g_gfx.fmtGlyph) g_gfx.fmtGlyph->GetFontFamilyName(glyph, ARRAYSIZE(glyph));
    const std::wstring name = glyph[0] && !wcscmp(fam, glyph) ? std::wstring(L"<glyph>") : std::wstring(fam);
    wchar_t b[96];
    swprintf_s(b, L" %.2f %d %d %d %d", f->GetFontSize(), (int)f->GetFontWeight(), (int)f->GetTextAlignment(), (int)f->GetParagraphAlignment(), (int)f->GetWordWrapping());
    return name + b;
}
static bool Drew(const wchar_t* text) { for (auto& t : recTexts) if (t == text) return true; return false; }
static int DrewCount(const wchar_t* text) { int n = 0; for (auto& t : recTexts) n += t == text; return n; }
static std::string Hex(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%016llx", (unsigned long long)v); return b; }

// ---------------------------------------------------------------- 1 + 5: every page, folded and open, hover, theme, scale

// The hover of each page: folded, its group's header (or a control outside it); open, a control inside.
struct HoverAt { int control, sub; };
static HoverAt HoverFor(int page, bool open) {
    switch (page) {
    case 0: return open ? HoverAt{ 2, 0 } : HoverAt{ 1, 0 };   // the icon size slider / the group
    case 1: return open ? HoverAt{ 4, 0 } : HoverAt{ 0, 0 };   // magnification / auto-hide
    case 2: return { sw::kHotDock - 0, 0 };                     // the preview's first button
    case 3: return { 5, 0 };                                    // "종료"
    default: return open ? HoverAt{ 3, 0 } : HoverAt{ 0, 0 };  // GPU / "끄기" of the source
    }
}

// The expected hashes of the drawing calls and of the control tables with their hot areas, all scenes in order
// (FNV-1a of the HASH values). Machine-independent: no font, GPU or real setting is in them.
static const uint64_t kDrawsAll = 0x2deacf038b307f6cULL, kControlsAll = 0x275e161f6e1c2977ULL, kExtrasAll = 0x931c9eb45e314c64ULL;

static void PageMatrix() {
    // scene -> its shot at each scale: 1.0 and 1.5 are compared with 1.25 once all three are drawn (not with
    // the first scale drawn: 1.0, the "at s100" checks compared 1.0 with itself and "at s150"
    // compared 1.5 with 1.0)
    std::map<std::string, Shot> byScale[3];
    std::string drawsDiffer[3], hotsDiffer[3], noPixels;
    uint64_t drawsAll = 1469598103934665603ULL, controlsAll = 1469598103934665603ULL;
    int hoverSame = 0, scenes = 0;
    for (bool light : { false, true })
        for (int si = 0; si < 3; ++si) {
            g_settings = Settings{};
            if (!OpenAt(kScales[si].m, light)) { Check(false, "the settings window opens for the page matrix"); return; }
            for (int p = 0; p < (int)sw::g_pages.size(); ++p) {
                GoToPage(p); HoverOff();
                const bool grouped = HasGroup(p);
                for (int fold = 0; fold < (grouped ? 2 : 1); ++fold) {
                    SetGroups(fold == 1);
                    uint64_t restDraws = 0;
                    for (int hv = 0; hv < 2; ++hv) {
                        HoverOff();
                        if (hv) { const HoverAt h = HoverFor(p, fold == 1); HoverOn(h.control, h.sub); }
                        const std::string scene = std::string(light ? "light" : "dark") + "-p" + std::to_string(p) + "-" +
                                                  (grouped ? (fold ? "open" : "folded") : "flat") + "-" + (hv ? "hover" : "rest");
                        const Shot s = Shoot();
                        ++scenes;
                        if (!s.pixelsOk) noPixels += " " + scene;
                        printf("HASH: settings_pixels/%s-%s %s\n", kScales[si].name, scene.c_str(), Hex(s.pixels).c_str());
                        if (hv) hoverSame += s.draws == restDraws; else restDraws = s.draws;
                        byScale[si][scene] = s;
                        if (si == 1) {
                            printf("HASH: settings_draws/%s %s\n", scene.c_str(), Hex(s.draws).c_str());
                            drawsAll = Fnv(&s.draws, sizeof s.draws, drawsAll);
                            if (!light && !hv) {
                                const uint64_t c = Fnv(ControlTable(p) + HotDump());
                                printf("HASH: settings_controls/p%d-%s %s\n", p, grouped ? (fold ? "open" : "folded") : "flat", Hex(c).c_str());
                                controlsAll = Fnv(&c, sizeof c, controlsAll);
                            }
                        }
                    }
                }
                SetGroups(false);
            }
        }
    for (int si : { 0, 2 })
        for (const auto& [scene, s] : byScale[si]) {
            const auto it = byScale[1].find(scene);
            if (it == byScale[1].end() || it->second.draws != s.draws) drawsDiffer[si] += " " + scene;
            if (it == byScale[1].end() || it->second.hots != s.hots) hotsDiffer[si] += " " + scene;
        }
    Check(scenes == 96 && noPixels.empty(), ("page matrix: 96 frames (5 pages, folded/open, hover, dark/light, 1.0/1.25/1.5) read back" + noPixels).c_str());
    Check(hoverSame == 0, "page matrix: the hover changes the drawing on every page, folded and open");
    for (int si : { 0, 2 }) {
        Check(drawsDiffer[si].empty(), (std::string("page matrix at ") + kScales[si].name + ": the same drawing calls (DIPs) as at 1.25" + drawsDiffer[si]).c_str());
        Check(hotsDiffer[si].empty(), (std::string("page matrix at ") + kScales[si].name + ": the same hot areas (DIPs) as at 1.25" + hotsDiffer[si]).c_str());
    }
    Pin("page matrix: the drawing calls of all 32 scenes (hash of the settings_draws lines)", Hex(drawsAll), Hex(kDrawsAll));
    Pin("page matrix: the control tables and hot areas of the 8 page states (hash of the settings_controls lines)", Hex(controlsAll), Hex(kControlsAll));
    if (sw::IsOpen()) sw::Close();
    Drain(); Unsave();
}

// More states, at each scale (dark): changed values under a folded header, the page scrolled to its end, the
// customizer with a battery, with every / no button, other thickness, gap and icon size, and while dragging.
struct Extra { const char* name; int page; bool open; void (*setup)(); };
static void DragPreview(int index, float dx, float dyBelow) {   // a press on the preview's button `index`, moved
    float x, y;
    sw::Render();
    if (!HotCentre(sw::kHotDock - index, 0, x, y)) return;
    Mouse(WM_LBUTTONDOWN, x, y);
    Mouse(WM_MOUSEMOVE, x + 10, y);
    Mouse(WM_MOUSEMOVE, dyBelow > 0 ? x : sw::g_preview.right - 2 + dx, dyBelow > 0 ? sw::g_preview.bottom + dyBelow : y);
}
static const Extra kExtras[] = {
    { "p0-folded-changed", 0, false, [] { g_settings.iconSize = 60; g_settings.edgeOffset = -10; g_settings.glassStyle = 0; } },
    { "p0-open-changed", 0, true, [] { g_settings.iconSize = 60; g_settings.edgeOffset = -10; g_settings.glassStyle = 0; } },
    { "p0-open-end", 0, true, [] { sw::g_scroll = 10000; sw::ClampScroll(); } },
    { "p0-open-left", 0, true, [] { g_settings.edge = Edge::Left; g_settings.edgeOffset = -20; g_settings.edgeGap = 0; } },
    { "p0-folded-opaque", 0, false, [] { sw::g_acrylic = false; } },
    { "p1-folded-changed", 1, false, [] { g_settings.magnification = 1.0f; g_settings.showDelay = 0.6f; g_settings.autoHide = false; } },
    { "p1-open-changed", 1, true, [] { g_settings.magnification = 1.0f; g_settings.showDelay = 0.6f; g_settings.autoHide = false; } },
    { "p2-battery", 2, false, [] { g_sys.SetBattery(battery::FromRaw({ 0, 1, 57, 0, 7200 })); } },
    { "p2-battery-charging", 2, false, [] { g_sys.SetBattery(battery::FromRaw({ 1, 8, 80, 0, 0xFFFFFFFFu })); } },
    { "p2-all", 2, false, [] { g_settings.trayButtons = "desktop,theme,oled,awake,screenshot,quiet,trash,downloads,audio,sff"; } },
    { "p2-none", 2, false, [] { g_settings.trayButtons = ""; } },
    { "p2-thick75", 2, false, [] { g_settings.dockThickness = 75; } },
    { "p2-thick150", 2, false, [] { g_settings.dockThickness = 150; } },
    { "p2-gap10", 2, false, [] { g_settings.iconGap = 10; } },
    { "p2-gap60", 2, false, [] { g_settings.iconGap = 60; } },
    { "p2-spacing0", 2, false, [] { g_settings.iconGap = 0; g_settings.iconSpacing = 0; } },
    { "p2-spacing2", 2, false, [] { g_settings.iconGap = 0; g_settings.iconSpacing = 2; } },
    { "p2-size32", 2, false, [] { g_settings.iconSize = 32; } },
    { "p2-size64", 2, false, [] { g_settings.iconSize = 64; g_settings.trayButtons = "desktop,theme,oled,awake,screenshot,quiet,trash"; } },
    { "p2-oled-on", 2, false, [] { oledOn = true; g_settings.oledOneMonitor = true; g_settings.trayIcons = true; g_settings.fluentFlyout = false; } },
    { "p2-drag-over", 2, false, [] { DragPreview(0, 0, 0); } },
    { "p2-drag-off", 2, false, [] { DragPreview(0, 0, 120); } },
    { "p2-drag-palette", 2, false, [] {
          float x, y; sw::Render();
          if (!HotCentre(sw::kHotPalette - (int)ButtonId::Trash, 0, x, y)) return;
          Mouse(WM_LBUTTONDOWN, x, y);
          Mouse(WM_MOUSEMOVE, (sw::g_preview.left + sw::g_preview.right) / 2, (sw::g_preview.top + sw::g_preview.bottom) / 2);
      } },
    { "p2-hover-list", 2, false, [] { HoverOn(sw::kHotPalette - (int)ButtonId::Trash, 0); } },
    { "p2-battery-hover", 2, false, [] { g_sys.SetBattery(battery::FromRaw({ 0, 1, 57, 0, 7200 })); sw::Render(); HoverOn(sw::kHotDock - 2, 0); } },
    { "p3-changed", 3, false, [] { g_settings.autostart = true; g_settings.revealHotkey = 2; g_settings.startMenuExtras = false; g_settings.adminWindows = 1; } },
    { "p4-folded-changed", 4, false, [] { g_settings.sensorSource = 0; g_settings.showGpu = false; g_settings.showSsd = false; } },
    { "p4-open-changed", 4, true, [] { g_settings.sensorSource = 0; g_settings.showGpu = false; g_settings.showSsd = false; } },
};
// The preview's buttons in one line: how many, in how many rows, from where to where.
static std::string PreviewSummary() {
    int n = 0, palette = 0;
    float left = 1e9f, right = -1e9f;
    std::vector<float> tops;
    for (auto& h : sw::g_hots) {
        if (h.control <= sw::kHotPalette) { ++palette; continue; }
        if (h.control > sw::kHotDock) continue;
        ++n; left = std::min(left, h.r.left); right = std::max(right, h.r.right);
        if (std::find(tops.begin(), tops.end(), h.r.top) == tops.end()) tops.push_back(h.r.top);
    }
    char b[160];
    snprintf(b, sizeof b, "preview %d buttons in %d row(s) %.2f..%.2f, list %d", n, (int)tops.size(), n ? left : 0, n ? right : 0, palette);
    std::string s = b;
    if (sw::g_bd.armed)   // while a button is dragged: no hot areas, the list as it would be dropped here
        s += std::string(" | dragging ") + (sw::g_bd.active ? "active" : "armed") + " shown " + FormatButtons(sw::ShownList()) +
             " drops " + std::to_string(sw::g_dropCentres.size());
    return s;
}
static std::map<std::string, std::string> kExtraWant = {
    { "p2-battery", "preview 3 buttons in 2 row(s) 502.49..537.21, list 7" },
    { "p2-battery-charging", "preview 3 buttons in 2 row(s) 502.49..537.21, list 7" },
    { "p2-all", "preview 10 buttons in 2 row(s) 460.69..535.09, list 0" },
    { "p2-none", "preview 0 buttons in 0 row(s) 0.00..0.00, list 10" },
    { "p2-thick75", "preview 3 buttons in 1 row(s) 483.20..527.84, list 7" },
    { "p2-thick150", "preview 3 buttons in 2 row(s) 490.44..525.16, list 7" },
    { "p2-gap10", "preview 3 buttons in 2 row(s) 481.21..515.93, list 7" },
    { "p2-gap60", "preview 3 buttons in 2 row(s) 502.21..536.93, list 7" },
    { "p2-spacing0", "preview 3 buttons in 2 row(s) 486.29..521.01, list 7" },
    { "p2-spacing2", "preview 3 buttons in 2 row(s) 495.77..530.49, list 7" },
    { "p2-size32", "preview 3 buttons in 1 row(s) 469.19..513.83, list 7" },
    { "p2-size64", "preview 7 buttons in 2 row(s) 496.49..567.79, list 3" },
    { "p2-oled-on", "preview 3 buttons in 2 row(s) 490.69..525.41, list 7" },
    { "p2-drag-over", "preview 0 buttons in 0 row(s) 0.00..0.00, list 0 | dragging active shown theme,sff,desktop drops 2" },
    { "p2-drag-off", "preview 0 buttons in 0 row(s) 0.00..0.00, list 0 | dragging active shown theme,sff drops 2" },
    { "p2-drag-palette", "preview 0 buttons in 0 row(s) 0.00..0.00, list 0 | dragging active shown trash,desktop,theme,sff drops 3" },
    { "p2-hover-list", "preview 3 buttons in 2 row(s) 490.69..525.41, list 7" },
    { "p2-battery-hover", "preview 3 buttons in 2 row(s) 502.49..537.21, list 7" },
};
static void Extras() {
    std::map<std::string, Shot> byScale[3];   // compared with 1.25 once all three are drawn (as PageMatrix)
    std::string differ[3];
    uint64_t all = 1469598103934665603ULL;
    for (int si = 0; si < 3; ++si) {
        g_settings = Settings{};
        if (!OpenAt(kScales[si].m, false)) { Check(false, "the settings window opens for the extra states"); return; }
        for (const auto& e : kExtras) {
            g_settings = Settings{}; oledOn = false;
            GoToPage(e.page); HoverOff();
            SetGroups(e.open);
            e.setup();
            const Shot s = Shoot();
            printf("HASH: settings_pixels/%s-dark-%s %s\n", kScales[si].name, e.name, Hex(s.pixels).c_str());
            byScale[si][e.name] = s;
            const std::string name = e.name;
            if (si == 1) {
                printf("HASH: settings_draws/dark-%s %s\n", e.name, Hex(s.draws).c_str());
                printf("HASH: settings_hots/dark-%s %s\n", e.name, Hex(s.hots).c_str());
                all = Fnv(&s.draws, sizeof s.draws, all); all = Fnv(&s.hots, sizeof s.hots, all);
                if (kExtraWant.count(name)) Pin("customizer " + name, PreviewSummary(), kExtraWant[name]);
                // the texts that say what changed
                if (name == "p0-folded-changed") Check(Drew(L"바꾼 값 2개") && !Drew(L"60"), "folded shape group, icon size 60 and position -10: \"바꾼 값 2개\" on its header, no slider drawn");
                if (name == "p0-open-changed") Check(!Drew(L"바꾼 값 2개") && Drew(L"60") && Drew(L"왼쪽으로 10%") && Drew(L"32% (기본)"), "open: no count on the header; the sliders say 60, 왼쪽으로 10%, 32% (기본)");
                if (name == "p0-open-left") Check(Drew(L"위로 20%") && Drew(L"붙임"), "docks on the left: the position along the edge says 위로 20%, the edge gap 0 says 붙임");
                if (name == "p1-folded-changed") Check(Drew(L"바꾼 값 2개"), "folded behaviour group, magnification off and show delay 0.6: \"바꾼 값 2개\" (auto-hide is outside it)");
                if (name == "p1-open-changed") Check(Drew(L"끄기") && Drew(L"0.60초") && Drew(L"0.35초 (기본)"), "open: 끄기, 0.60초, 0.35초 (기본)");
                if (name == "p4-folded-changed") Check(Drew(L"바꾼 값 2개"), "folded sensor group, GPU and SSD off: \"바꾼 값 2개\" (the source is outside it)");
                if (name == "p2-all") Check(Drew(L"모든 버튼이 독에 있습니다"), "every button on the dock: \"모든 버튼이 독에 있습니다\" instead of the list");
                if (name == "p2-battery") Check(Drew(L"57%"), "a laptop on battery: the preview shows the battery cell with 57%");
                if (name == "p3-changed") Check(DrewCount(L"종료") == 1 && Drew(L"Ctrl+Shift+Space"), "the system page: one \"종료\" button, the shortcut segments");
            }
            // leave nothing behind: a drag cancelled with Esc, no battery
            if (sw::g_bd.armed) Esc();
            g_sys.SetBattery({});
            sw::g_acrylic = true; oledOn = false;
        }
    }
    // A scene with a button dragged follows the pointer, whose DIPs are rounded to pixels: it differs between the
    // scales (1.0 and 1.5 happen to round alike, 1.25 not). Those are compared as the first round did, 1.5 with 1.0,
    // and their drawing calls at 1.0 and 1.5 pinned below; every other scene with 1.25.
    auto dragged = [](const std::string& name) { return name.find("drag") != std::string::npos; };
    uint64_t dragAll = 1469598103934665603ULL;
    for (int si : { 0, 2 })
        for (const auto& [name, s] : byScale[si]) {
            const int ref = dragged(name) ? 0 : 1;
            if (ref == si) continue;
            const auto it = byScale[ref].find(name);
            if (it == byScale[ref].end() || it->second.draws != s.draws || it->second.hots != s.hots) differ[si] += " " + name;
        }
    for (int si : { 0, 2 })
        for (const auto& [name, s] : byScale[si])
            if (dragged(name)) { dragAll = Fnv(&s.draws, sizeof s.draws, dragAll); dragAll = Fnv(&s.hots, sizeof s.hots, dragAll); }
    for (int si : { 0, 2 }) Check(differ[si].empty(), (std::string("extra states at ") + kScales[si].name + ": the same drawing calls and hot areas as at 1.25" + differ[si]).c_str());
    Pin("extra states: drawing calls and hot areas of all 28 (hash of the settings_draws / settings_hots lines)", Hex(all), Hex(kExtrasAll));
    Pin("extra states with a button dragged (the pointer rounded to pixels), at 1.0 and 1.5: drawing calls and hot areas (hash)", Hex(dragAll), "afbb44e1f937b8a3");
    g_settings = Settings{};
    if (sw::IsOpen()) sw::Close();
    Drain(); Unsave();
}

// ---------------------------------------------------------------- 1: slider texts and the "바꾼 값" boundary

static void SliderTexts() {
    g_settings = Settings{};
    if (!OpenAt(kLaptop, false)) { Check(false, "open for the slider texts"); return; }
    std::string got;
    for (int p = 0; p < (int)sw::g_pages.size(); ++p)
        for (int i = 0; i < (int)sw::g_pages[p].controls.size(); ++i) {
            const auto& c = sw::g_pages[p].controls[i];
            if (c.type != sw::Control::Slider) continue;
            got += U8(L"[" + std::wstring(c.label) + L"] " + c.text(c.lo) + L"|" + c.text(c.def) + L"|" + c.text(c.hi) + L" ");
        }
    Pin("slider texts lo|default|hi", got,
        "[아이콘 크기] 32|44|64 [아이콘 간격] 10%|32%|60% [독 두께] 75%|100%|150% [화면 가장자리와 거리] 붙임|6 px|40 px [가장자리 따라 위치] 왼쪽으로 100%|가운데|오른쪽으로 100% [확대 효과] 끄기|1.7배|2.1배 [나타나는 속도] 즉시|0.05초|0.60초 [숨는 속도] 0.10초|0.35초|1.50초 ");
    // just under half a step from the default still counts as the default; half a step is a change
    GoToPage(0); SetGroups(false);
    g_settings.iconSize = 44.9f; Shoot();
    const bool under = !Drew(L"바꾼 값 1개");
    g_settings.iconSize = 45; Shoot();
    const bool at = Drew(L"바꾼 값 1개");
    SetGroups(true);
    g_settings.iconSize = 44.9f; Shoot();
    const bool underText = Drew(L"45 (기본)");
    g_settings.iconSize = 45; Shoot();
    const bool atText = Drew(L"45") && !Drew(L"45 (기본)");
    Check(under && at && underText && atText, "icon size 44.9 is the default (\"45 (기본)\", no count), 45 is a change (\"45\", \"바꾼 값 1개\"): half a step");
    // toggles: a changed one counts once its value differs from the default; the marks 기본 켬 / 기본 끔
    GoToPage(1); SetGroups(false);
    g_settings = Settings{}; Shoot();
    Check(DrewCount(L"기본 켬") == 2 && DrewCount(L"기본 끔") == 2 && !Drew(L"바꾼 값 1개"), "behaviour page: 기본 켬 twice, 기본 끔 twice; at the defaults no count");
    GoToPage(2);
    g_settings = Settings{}; Shoot();
    Check(DrewCount(L"기본 켬") == 1 && DrewCount(L"기본 끔") == 1 && DrewCount(L"기본") == 1, "buttons page: the OLED and dark mode switches show no default; one 기본 under the segments");
    g_settings = Settings{};
    sw::Close(); Drain(); Unsave();
}

// ---------------------------------------------------------------- 2: each control's setter, the clicks, the resets

static bool Forbidden(int p, int ci, int sub) {   // PawnIO (a UAC prompt) and the elevated helper's Enable: never
    const auto& c = sw::g_pages[p].controls[ci];
    if (c.type == sw::Control::Segmented && sub >= 0 && sub < (int)c.options.size() && wcsstr(c.options[sub], L"PawnIO")) return true;
    if (c.type == sw::Control::Toggle && (wcsstr(c.label, L"관리자 권한 앱") || (p == 3 && ci == 4)) && sub == 1) return true;
    return false;
}
static std::map<std::string, std::string> kRoundTrip = {
    { "set p0.0 위치 choose(0)", "- | index 0 | none" },
    { "set p0.0 위치 choose(1)", "edge=0>1 | index 1 | none" },
    { "set p0.0 위치 choose(2)", "edge=0>2 | index 2 | none" },
    { "set p0.0 위치 choose(3)", "edge=0>3 | index 3 | none" },
    { "set p0.2 아이콘 크기 set(32)", "iconSize=44>32 | value 32 | none" },
    { "set p0.2 아이콘 크기 set(64)", "iconSize=44>64 | value 64 | none" },
    { "set p0.2 아이콘 크기 set(34)", "iconSize=44>34 | value 34 | none" },
    { "set p0.3 아이콘 간격 set(10)", "iconGap=0>10 | value 10 | none" },
    { "set p0.3 아이콘 간격 set(60)", "iconGap=0>60 | value 60 | none" },
    { "set p0.3 아이콘 간격 set(12)", "iconGap=0>12 | value 12 | none" },
    { "set p0.4 독 두께 set(75)", "dockThickness=100>75 | value 75 | none" },
    { "set p0.4 독 두께 set(150)", "dockThickness=100>150 | value 150 | none" },
    { "set p0.4 독 두께 set(80)", "dockThickness=100>80 | value 80 | none" },
    { "set p0.5 화면 가장자리와 거리 set(0)", "edgeGap=6>0 | value 0 | none" },
    { "set p0.5 화면 가장자리와 거리 set(40)", "edgeGap=6>40 | value 40 | none" },
    { "set p0.5 화면 가장자리와 거리 set(2)", "edgeGap=6>2 | value 2 | none" },
    { "set p0.6 가장자리 따라 위치 set(-100)", "edgeOffset=0>-100 | value -100 | none" },
    { "set p0.6 가장자리 따라 위치 set(100)", "edgeOffset=0>100 | value 100 | none" },
    { "set p0.6 가장자리 따라 위치 set(-95)", "edgeOffset=0>-95 | value -95 | none" },
    { "set p0.7 유리 질감 choose(0)", "glassStyle=1>0 | index 0 | none" },
    { "set p0.7 유리 질감 choose(1)", "- | index 1 | none" },
    { "set p0.7 유리 질감 choose(2)", "glassStyle=1>2 | index 2 | none" },
    { "set p0.8 시계 글자 choose(0)", "- | index 0 | none" },
    { "set p0.8 시계 글자 choose(1)", "clockSize=0>1 | index 1 | none" },
    { "set p0.9 센서 글자 choose(0)", "- | index 0 | none" },
    { "set p0.9 센서 글자 choose(1)", "sensorSize=0>1 | index 1 | none" },
    { "set p1.0 자동 숨김 toggle(0)", "autoHide=1>0 | on 0 | none" },
    { "set p1.0 자동 숨김 toggle(1)", "- | on 1 | none" },
    { "set p1.1 보조 모니터에서는 항상 표시 toggle(0)", "- | on 0 | none" },
    { "set p1.1 보조 모니터에서는 항상 표시 toggle(1)", "secondaryAlwaysShown=0>1 | on 1 | none" },
    { "set p1.2 시작 메뉴와 함께 표시 toggle(0)", "showWithStart=1>0 | on 0 | none" },
    { "set p1.2 시작 메뉴와 함께 표시 toggle(1)", "- | on 1 | none" },
    { "set p1.4 확대 효과 set(1)", "magnification=1.7>1 | value 1 | none" },
    { "set p1.4 확대 효과 set(2.1)", "magnification=1.7>2.1 | value 2.1 | none" },
    { "set p1.4 확대 효과 set(1.1)", "magnification=1.7>1.1 | value 1.1 | none" },
    { "set p1.5 나타나는 속도 set(0)", "showDelay=0.05>0 | value 0 | none" },
    { "set p1.5 나타나는 속도 set(0.6)", "showDelay=0.05>0.6 | value 0.6 | none" },
    { "set p1.5 나타나는 속도 set(0.05)", "- | value 0.05 | none" },
    { "set p1.6 숨는 속도 set(0.1)", "hideDelay=0.35>0.1 | value 0.1 | none" },
    { "set p1.6 숨는 속도 set(1.5)", "hideDelay=0.35>1.5 | value 1.5 | none" },
    { "set p1.6 숨는 속도 set(0.15)", "hideDelay=0.35>0.15 | value 0.15 | none" },
    { "set p1.7 24시간제 시계 toggle(0)", "- | on 0 | none" },
    { "set p1.7 24시간제 시계 toggle(1)", "clockFormat=0>1 | on 1 | none" },
    { "set p2.1 트레이 아이콘 표시 (실험) toggle(0)", "- | on 0 | trayhost1" },
    { "set p2.1 트레이 아이콘 표시 (실험) toggle(1)", "trayIcons=0>1 | on 1 | trayhost1" },
    { "set p2.2 FluentFlyout 연동 toggle(0)", "fluentFlyout=1>0 | on 0 | trayhost1" },
    { "set p2.2 FluentFlyout 연동 toggle(1)", "- | on 1 | trayhost1" },
    { "set p2.3 OLED 모드를 적용할 모니터 choose(0)", "- | index 0 | none" },
    { "set p2.3 OLED 모드를 적용할 모니터 choose(1)", "oledOneMonitor=0>1 | index 1 | none" },
    { "set p2.4 OLED 모드 켜기 toggle(0)", "- | on 0 | none" },
    { "set p2.4 OLED 모드 켜기 toggle(1)", "oled=on oledFor=all | on 1 | none" },
    { "set p2.5 Windows 다크 모드 toggle(0)", "theme=light | on 0 | none" },
    { "set p2.5 Windows 다크 모드 toggle(1)", "- | on 1 | none" },
    { "set p3.0 Windows 시작 시 자동 실행 toggle(0)", "- | on 0 | autostart1" },
    { "set p3.0 Windows 시작 시 자동 실행 toggle(1)", "autostart=0>1 | on 1 | autostart1" },
    { "set p3.1 독 열기 단축키 choose(0)", "- | index 0 | hotkey1" },
    { "set p3.1 독 열기 단축키 choose(1)", "revealHotkey=0>1 | index 1 | hotkey1" },
    { "set p3.1 독 열기 단축키 choose(2)", "revealHotkey=0>2 | index 2 | hotkey1" },
    { "set p3.1 독 열기 단축키 choose(3)", "revealHotkey=0>3 | index 3 | hotkey1" },
    { "set p3.2 시작 버튼 우클릭 메뉴 toggle(0)", "startMenuExtras=1>0 | on 0 | none" },
    { "set p3.2 시작 버튼 우클릭 메뉴 toggle(1)", "- | on 1 | none" },
    { "set p3.3 Shift+클릭으로 관리자 권한 실행 toggle(0)", "shiftRunsAsAdmin=1>0 | on 0 | none" },
    { "set p3.3 Shift+클릭으로 관리자 권한 실행 toggle(1)", "- | on 1 | none" },
    { "set p3.4 관리자 권한 앱 창 제어 toggle(0)", "adminWindows=1>2 | on 0 | none" },
    { "set p3.5 Liquid Dock 종료 press", "- | close1" },
    { "set p4.0 온도 읽기 choose(0)", "sensorSource=1>0 | index 0 | none" },
    { "set p4.0 온도 읽기 choose(1)", "- | index 1 | none" },
    { "set p4.2 CPU 온도 toggle(0)", "showCpuTemp=1>0 | on 0 | none" },
    { "set p4.2 CPU 온도 toggle(1)", "- | on 1 | none" },
    { "set p4.3 GPU toggle(0)", "showGpu=1>0 | on 0 | none" },
    { "set p4.3 GPU toggle(1)", "- | on 1 | none" },
    { "set p4.4 VRAM toggle(0)", "showVram=1>0 | on 0 | none" },
    { "set p4.4 VRAM toggle(1)", "- | on 1 | none" },
    { "set p4.5 VRAM 온도 toggle(0)", "showVramTemp=1>0 | on 0 | none" },
    { "set p4.5 VRAM 온도 toggle(1)", "- | on 1 | none" },
    { "set p4.6 RAM 슬롯 온도 toggle(0)", "showRamTemp=1>0 | on 0 | none" },
    { "set p4.6 RAM 슬롯 온도 toggle(1)", "- | on 1 | none" },
    { "set p4.7 SSD toggle(0)", "showSsd=1>0 | on 0 | none" },
    { "set p4.7 SSD toggle(1)", "- | on 1 | none" },
    { "set p4.8 전력 소비량 choose(0)", "- | index 0 | none" },
    { "set p4.8 전력 소비량 choose(1)", "powerView=0>1 | index 1 | none" },
    { "set p4.8 전력 소비량 choose(2)", "powerView=0>2 | index 2 | none" },
    { "set p4.9 CPU 전력 toggle(0)", "showCpuPower=1>0 | on 0 | none" },
    { "set p4.9 CPU 전력 toggle(1)", "- | on 1 | none" },
    { "set p4.10 GPU 전력 toggle(0)", "showGpuPower=1>0 | on 0 | none" },
    { "set p4.10 GPU 전력 toggle(1)", "- | on 1 | none" },
    { "reset p0", "iconSize=60>44 glassStyle=2>1 clockSize=1>0 sensorSize=1>0 iconSpacing=2>1 iconGap=50>0 dockThickness=140>100 edgeGap=20>6 edgeOffset=30>0 | rebuild1 saved" },
    { "reset p1", "magnification=1.2>1.7 clockFormat=1>0 autoHide=0>1 secondaryAlwaysShown=1>0 showWithStart=0>1 showDelay=0.4>0.05 hideDelay=1.2>0.35 | rebuild1 saved" },
    { "reset p2", "trayButtons=trash>desktop,theme,sff oledOneMonitor=1>0 trayIcons=1>0 fluentFlyout=0>1 | rebuild1 trayhost1 saved" },
    { "reset p2 (tray icons and FluentFlyout at their defaults)", "trayButtons=trash>desktop,theme,sff oledOneMonitor=1>0 | rebuild1 saved" },
    { "reset p3", "no reset button" },
    { "reset p4", "sensorSource=0>1 showCpuTemp=0>1 showGpu=0>1 showVram=0>1 showVramTemp=0>1 showRamTemp=0>1 showSsd=0>1 powerView=2>0 showCpuPower=0>1 showGpuPower=0>1 | rebuild1 saved" },
};
static void PinRT(const std::string& key, const std::string& got) {
    auto it = kRoundTrip.find(key);
    Pin(key, got, it == kRoundTrip.end() ? "?" : it->second);
}
static void SetterRoundTrip() {
    g_settings = Settings{};
    if (!OpenAt(kLaptop, false)) { Check(false, "open for the setters"); return; }
    for (int p = 0; p < (int)sw::g_pages.size(); ++p)
        for (int ci = 0; ci < (int)sw::g_pages[p].controls.size(); ++ci) {
            const auto& c = sw::g_pages[p].controls[ci];
            const std::string key = "set p" + std::to_string(p) + "." + std::to_string(ci) + " " + U8(c.label);
            auto start = [] { g_settings = Settings{}; oledOn = false; themeLight = false; oledFor = (HMONITOR)(intptr_t)-1; Fresh(); };
            switch (c.type) {
            case sw::Control::Segmented:
                for (int i = 0; i < (int)c.options.size(); ++i) {
                    if (Forbidden(p, ci, i)) continue;
                    start(); const World a = Now();
                    c.choose(i);
                    PinRT(key + " choose(" + std::to_string(i) + ")", Diff(a, Now()) + " | index " + std::to_string(c.index()) + " | " + Posted());
                }
                break;
            case sw::Control::Slider:
                for (float v : { c.lo, c.hi, c.lo + c.step }) {
                    start(); const World a = Now();
                    c.set(v);
                    PinRT(key + " set(" + G(v) + ")", Diff(a, Now()) + " | value " + G(c.value()) + " | " + Posted());
                }
                break;
            case sw::Control::Toggle:
                for (int v : { 0, 1 }) {
                    if (Forbidden(p, ci, v)) continue;
                    start();
                    if (p == 3 && ci == 4) g_settings.adminWindows = 1;
                    const World a = Now();
                    c.toggle(v == 1);
                    PinRT(key + " toggle(" + std::to_string(v) + ")", Diff(a, Now()) + " | on " + std::to_string(c.on()) + " | " + Posted());
                }
                break;
            case sw::Control::Button: {
                start(); const World a = Now();
                c.press();
                PinRT(key + " press", Diff(a, Now()) + " | " + Posted());
                break;
            }
            default: break;
            }
        }
    g_settings = Settings{}; oledOn = false; themeLight = false;
    sw::Close(); Drain(); Unsave();
}
// The same controls pressed as a user does (all groups open, scrolled into view): what changes, what is posted.
// A slider is pressed at its track's right end and let go there; a segmented control on its first segment not
// chosen (PawnIO never); a toggle flipped (관리자 권한 앱 창 제어 only from on to off: never Enable).
static const char* const kClickWant =
    "p0.0 위치: rebuild1 saved | edge=0>1\n"
    "p0.2 아이콘 크기: down none, up rebuild1 saved | iconSize=44>64\n"
    "p0.3 아이콘 간격: down relayout1, up relayout1 saved | iconGap=0>60\n"
    "p0.4 독 두께: down relayout1, up relayout1 saved | dockThickness=100>150\n"
    "p0.5 화면 가장자리와 거리: down relayout1, up relayout1 saved | edgeGap=6>40\n"
    "p0.6 가장자리 따라 위치: down relayout1, up relayout1 saved | edgeOffset=0>100\n"
    "p0.7 유리 질감: relayout1 saved | glassStyle=1>0\n"
    "p0.8 시계 글자: relayout1 saved | clockSize=0>1\n"
    "p0.9 센서 글자: relayout1 saved | sensorSize=0>1\n"
    "p1.0 자동 숨김: relayout1 saved | autoHide=1>0\n"
    "p1.1 보조 모니터에서는 항상 표시: relayout1 saved | secondaryAlwaysShown=0>1\n"
    "p1.2 시작 메뉴와 함께 표시: relayout1 saved | showWithStart=1>0\n"
    "p1.4 확대 효과: down none, up rebuild1 saved | magnification=1.7>2.1\n"
    "p1.5 나타나는 속도: down none, up saved | showDelay=0.05>0.6\n"
    "p1.6 숨는 속도: down none, up saved | hideDelay=0.35>1.5\n"
    "p1.7 24시간제 시계: relayout1 saved | clockFormat=0>1\n"
    "p2.1 트레이 아이콘 표시 (실험): trayhost1 saved | trayIcons=0>1\n"
    "p2.2 FluentFlyout 연동: trayhost1 saved | fluentFlyout=1>0\n"
    "p2.3 OLED 모드를 적용할 모니터: saved | oledOneMonitor=0>1\n"
    "p2.4 OLED 모드 켜기: saved | oled=on oledFor=all\n"
    "p2.5 Windows 다크 모드: saved | theme=light\n"
    "p3.0 Windows 시작 시 자동 실행: autostart1 saved | autostart=0>1\n"
    "p3.1 독 열기 단축키: hotkey1 saved | revealHotkey=0>1\n"
    "p3.2 시작 버튼 우클릭 메뉴: saved | startMenuExtras=1>0\n"
    "p3.3 Shift+클릭으로 관리자 권한 실행: saved | shiftRunsAsAdmin=1>0\n"
    "p3.4 관리자 권한 앱 창 제어: saved | adminWindows=1>2\n"
    "p3.5 Liquid Dock 종료: close1 | -\n"
    "p4.0 온도 읽기: rebuild1 saved | sensorSource=1>0\n"
    "p4.2 CPU 온도: rebuild1 saved | showCpuTemp=1>0\n"
    "p4.3 GPU: rebuild1 saved | showGpu=1>0\n"
    "p4.4 VRAM: rebuild1 saved | showVram=1>0\n"
    "p4.5 VRAM 온도: rebuild1 saved | showVramTemp=1>0\n"
    "p4.6 RAM 슬롯 온도: rebuild1 saved | showRamTemp=1>0\n"
    "p4.7 SSD: rebuild1 saved | showSsd=1>0\n"
    "p4.8 전력 소비량: relayout1 saved | powerView=0>1\n"
    "p4.9 CPU 전력: relayout1 saved | showCpuPower=1>0\n"
    "p4.10 GPU 전력: relayout1 saved | showGpuPower=1>0\n";
static void ClickRoundTrip() {
    for (int si = 0; si < 3; ++si) {
        g_settings = Settings{};
        if (!OpenAt(kScales[si].m, false)) { Check(false, "open for the clicks"); return; }
        std::string got;
        for (int p = 0; p < (int)sw::g_pages.size(); ++p) {
            GoToPage(p); SetGroups(true);
            for (int ci = 0; ci < (int)sw::g_pages[p].controls.size(); ++ci) {
                const auto& c = sw::g_pages[p].controls[ci];
                if (c.type == sw::Control::Group || c.type == sw::Control::Customizer) continue;
                g_settings = Settings{}; oledOn = false; themeLight = false; oledFor = (HMONITOR)(intptr_t)-1;
                if (p == 3 && ci == 4) g_settings.adminWindows = 1;   // so the click turns it off, never Enable
                // CPU 전력 / GPU 전력 are drawn only under a power view: 그래프 before their clicks
                const int powerView = c.shown ? sensorlogic::kPowerGraph : sensorlogic::kPowerOff;
                g_settings.powerView = powerView;
                int sub = 0;
                if (c.type == sw::Control::Segmented) { while (sub == c.index() || Forbidden(p, ci, sub)) ++sub; }
                if (c.type == sw::Control::Toggle && Forbidden(p, ci, c.on() ? 0 : 1)) continue;
                float x, y;
                if (!Reveal(ci, sub, x, y)) { got += "p" + std::to_string(p) + "." + std::to_string(ci) + " not drawn\n"; continue; }
                Fresh();
                const World a = Now();
                std::string line = "p" + std::to_string(p) + "." + std::to_string(ci) + " " + U8(c.label) + ": ";
                if (c.type == sw::Control::Slider) {
                    const float right = sw::g_sliderTrack[ci].right;
                    Mouse(WM_LBUTTONDOWN, right, y);
                    line += "down " + Posted();
                    ResetCounts();
                    Mouse(WM_LBUTTONUP, right, y);
                    line += ", up " + Posted();
                } else {
                    Click(x, y);
                    line += Posted();
                }
                line += " | " + Diff(a, Now()) + "\n";
                got += line;
            }
            SetGroups(false);
        }
        Pin(std::string("every control clicked at ") + kScales[si].name + " (what changed, what reached the docks, saved)", got, kClickWant);
        sw::Close(); Drain();
    }
    g_settings = Settings{}; oledOn = false; Unsave();
}
static Settings Scrambled() {   // every field away from its default
    Settings s;
    s.edge = Edge::Top; s.iconSize = 60; s.magnification = 1.2f; s.glassStyle = 2; s.trayButtons = "trash";
    s.showDesktopButton = false; s.showThemeButton = false; s.showOledButton = true; s.oledOneMonitor = true; s.trayIcons = true;
    s.fluentFlyout = false; s.clockSize = 1; s.clockFormat = 1; s.sensorSize = 1; s.iconSpacing = 2; s.iconGap = 50; s.dockThickness = 140; s.edgeGap = 20; s.edgeOffset = 30;
    s.autoHide = false; s.secondaryAlwaysShown = true; s.showWithStart = false; s.showDelay = 0.4f; s.hideDelay = 1.2f; s.autostart = true;
    s.startMenuExtras = false; s.shiftRunsAsAdmin = false; s.taskbarAppButtons = false; s.revealHotkey = 3; s.sensorSource = 0;
    s.cpuTemp = true; s.sourceFromCpuTemp = true; s.showCpuTemp = s.showGpu = s.showVram = s.showVramTemp = s.showRamTemp = s.showSsd = false;
    s.powerView = 2; s.showCpuPower = s.showGpuPower = false;
    s.adminWindows = 2; s.hwinfoCpuLabel = "x"; s.pinsVersion = 2;
    return s;
}
static void PageResets() {
    g_settings = Settings{};
    if (!OpenAt(kLaptop, false)) { Check(false, "open for the resets"); return; }
    for (int p = 0; p < (int)sw::g_pages.size(); ++p) {
        GoToPage(p);
        for (int variant = 0; variant < (p == 2 ? 2 : 1); ++variant) {
            g_settings = Scrambled();
            if (variant == 1) { g_settings.trayIcons = Settings{}.trayIcons; g_settings.fluentFlyout = Settings{}.fluentFlyout; }
            float x, y;
            const bool shown = Reveal(-2, 0, x, y);
            Fresh();
            const World a = Now();
            if (shown) Click(x, y);
            const std::string key = "reset p" + std::to_string(p) + (variant ? " (tray icons and FluentFlyout at their defaults)" : "");
            PinRT(key, shown ? Diff(a, Now()) + " | " + Posted() : "no reset button");
        }
    }
    g_settings = Settings{};
    sw::Close(); Drain(); Unsave();
}

// ---------------------------------------------------------------- 3: the messages the checks above never send

static void InputMessages() {
    g_settings = Settings{};
    if (!OpenAt(kLaptop, false)) { Check(false, "open for the input messages"); return; }
    const HWND w = sw::g_wnd;
    GoToPage(0); SetGroups(true);
    const int icon = ControlIndex(L"아이콘 크기"), gap = ControlIndex(L"화면 가장자리와 거리");
    float x, y;
    // a slider drag that loses the mouse (another window took it): applied once, as a release
    auto pressSlider = [&](int ci) {
        sw::g_scroll = 0; sw::Render();
        if (!HotCentre(ci, 0, x, y)) return false;
        const auto t = sw::g_sliderTrack[ci];
        Mouse(WM_LBUTTONDOWN, t.left + (t.right - t.left) * 0.5f, y);
        Mouse(WM_MOUSEMOVE, t.right, y);
        return true;
    };
    std::string got;
    g_settings.iconSize = 44; Fresh();
    if (pressSlider(icon)) {
        got += "moved: " + Posted() + " drag " + std::to_string(sw::g_drag);
        int f = presents;
        SendMessageW(w, WM_CAPTURECHANGED, 0, (LPARAM)w);
        got += " | to itself: " + Posted() + " drag " + std::to_string(sw::g_drag) + " frames " + std::to_string(presents - f);
        f = presents;
        SendMessageW(w, WM_CAPTURECHANGED, 0, (LPARAM)g_mainWnd);
        got += " | lost: " + Posted() + " drag " + std::to_string(sw::g_drag) + " frames " + std::to_string(presents - f) + " icon " + G(g_settings.iconSize);
        Fresh();
        Mouse(WM_LBUTTONUP, x, y);
        got += " | up after: " + Posted();
    }
    Pin("icon size slider dragged, then WM_CAPTURECHANGED", got,
        "moved: none drag 2 | to itself: none drag 2 frames 0 | lost: rebuild1 saved drag -1 frames 1 icon 64 | up after: none");
    got.clear(); g_settings = Settings{}; Fresh();
    if (pressSlider(gap)) {
        got += "moved: " + Posted();
        ResetCounts();
        SendMessageW(w, WM_CAPTURECHANGED, 0, 0);
        got += " | lost: " + Posted() + " gap " + std::to_string(g_settings.edgeGap);
    }
    Pin("edge gap slider (live) dragged, then the mouse lost", got, "moved: relayout2 | lost: relayout1 saved gap 40");
    // Windows sends WM_CAPTURECHANGED from inside ReleaseCapture: the release is applied once, not twice
    captureNotifies = true;
    got.clear(); g_settings = Settings{}; Fresh();
    if (pressSlider(icon)) {
        ResetCounts();
        const int f = presents;
        Mouse(WM_LBUTTONUP, x, y);
        got += "slider up: " + Posted() + " frames " + std::to_string(presents - f) + " drag " + std::to_string(sw::g_drag);
    }
    GoToPage(2);
    g_settings = Settings{}; sw::Render();
    if (HotCentre(sw::kHotDock - 0, 0, x, y)) {
        Fresh();
        Mouse(WM_LBUTTONDOWN, x, y);
        Mouse(WM_MOUSEMOVE, x + 10, y);
        Mouse(WM_MOUSEMOVE, x, sw::g_preview.bottom + 120);
        const int f = presents;
        Mouse(WM_LBUTTONUP, x, sw::g_preview.bottom + 120);
        got += " | button dropped off: " + Posted() + " frames " + std::to_string(presents - f) + " buttons " + g_settings.trayButtons;
        g_settings = Settings{}; sw::Render(); Fresh();
        Mouse(WM_LBUTTONDOWN, x, y);
        Mouse(WM_MOUSEMOVE, x + 10, y);
        const int f2 = presents;
        const bool taken = Esc();
        got += " | Esc: " + std::to_string(taken) + " " + Posted() + " frames " + std::to_string(presents - f2) + " armed " + std::to_string(sw::g_bd.armed) +
               " open " + std::to_string(sw::IsOpen()) + " captured " + std::to_string(captured != nullptr);
    }
    captureNotifies = false;
    Pin("ReleaseCapture sends WM_CAPTURECHANGED (as Windows)", got, "slider up: rebuild1 saved frames 1 drag -1 | button dropped off: relayout1 saved frames 1 buttons theme,sff | Esc: 1 none frames 1 armed 0 open 1 captured 0");
    // a button drag that loses the mouse: dropped, nothing committed
    got.clear(); g_settings = Settings{}; sw::Render();
    if (HotCentre(sw::kHotDock - 1, 0, x, y)) {
        Fresh();
        Mouse(WM_LBUTTONDOWN, x, y);
        Mouse(WM_MOUSEMOVE, x + 30, y);
        int f = presents;
        SendMessageW(w, WM_CAPTURECHANGED, 0, (LPARAM)w);
        got += "to itself: armed " + std::to_string(sw::g_bd.armed) + " frames " + std::to_string(presents - f);
        f = presents;
        SendMessageW(w, WM_CAPTURECHANGED, 0, (LPARAM)g_mainWnd);
        got += " | lost: armed " + std::to_string(sw::g_bd.armed) + " frames " + std::to_string(presents - f) + " " + Posted() + " buttons " + g_settings.trayButtons;
        Mouse(WM_LBUTTONUP, x + 30, y);
        got += " | up after: " + Posted();
        captured = nullptr;
    }
    Pin("preview button dragged, then WM_CAPTURECHANGED", got, "to itself: armed 1 frames 0 | lost: armed 0 frames 1 none buttons desktop,theme,sff | up after: none");
    // right button: a menu over a button of the preview or the list only
    got.clear(); popupChoice = 0;
    auto rclick = [&](float px, float py) {
        const int menus = popupMenus, f = presents;
        popupItems.clear();
        Mouse(WM_RBUTTONUP, px, py);
        return "menus " + std::to_string(popupMenus - menus) + " frames " + std::to_string(presents - f) + " [" + U8(popupItems) + "]";
    };
    sw::Render();
    if (HotCentre(sw::kHotDock - 0, 0, x, y)) got += "first: " + rclick(x, y);
    if (HotCentre(sw::kHotDock - 2, 0, x, y)) got += " | last: " + rclick(x, y);
    if (HotCentre(sw::kHotDock - 1, 0, x, y)) got += " | middle: " + rclick(x, y);
    if (HotCentre(sw::kHotPalette - (int)ButtonId::Trash, 0, x, y)) got += " | list: " + rclick(x, y);
    if (HotCentre(1, 0, x, y)) got += " | tray icons switch: " + rclick(x, y);
    if (HotCentre(-10 - 0, 0, x, y)) got += " | nav: " + rclick(x, y);
    TitlePoint(x, y); got += " | title: " + rclick(x, y);
    got += " | " + Posted();
    Pin("WM_RBUTTONUP", got, "first: menus 1 frames 1 [2g:왼쪽으로,3:오른쪽으로,-,4:독에서 빼기] | last: menus 1 frames 1 [2:왼쪽으로,3g:오른쪽으로,-,4:독에서 빼기] | middle: menus 1 frames 1 [2:왼쪽으로,3:오른쪽으로,-,4:독에서 빼기] | list: menus 1 frames 1 [1:독에 추가] | tray icons switch: menus 0 frames 0 [] | nav: menus 0 frames 0 [] | title: menus 0 frames 0 [] | none");
    // the pointer leaves: the hover is dropped (one frame), and only once
    got.clear();
    {
        const int tracks = leaveTracks;
        HoverOn(sw::kHotPalette - (int)ButtonId::Trash, 0);
        got += "hover " + std::to_string(sw::g_hover) + " tracked " + std::to_string(leaveTracks - tracks);
        HoverOn(sw::kHotPalette - (int)ButtonId::Trash, 0);
        got += " again: tracked " + std::to_string(leaveTracks - tracks);
        int f = presents;
        HoverOff();
        got += " | leave: hover " + std::to_string(sw::g_hover) + " sub " + std::to_string(sw::g_hoverSub) + " frames " + std::to_string(presents - f);
        f = presents;
        HoverOff();
        got += " | again: frames " + std::to_string(presents - f);
    }
    Pin("WM_MOUSEMOVE / WM_MOUSELEAVE", got, "hover -2008 tracked 1 again: tracked 2 | leave: hover -1 sub -1 frames 1 | again: frames 0");
    // activation and the system's settings: drawn again (the accent read again only for a setting change)
    got.clear();
    {
        // WA_INACTIVE: DefWindowProc then leaves the focus alone (WA_ACTIVE sets it, and how many frames that draws
        // depends on which window Windows lets this process activate)
        int f = presents;
        const LRESULT r = SendMessageW(w, WM_ACTIVATE, WA_INACTIVE, 0);
        got += "deactivate: frames " + std::to_string(presents - f) + " result " + std::to_string((long long)r) + " accent kept " +
               std::to_string(sw::g_accent.r == kAccent.r && sw::g_accent.g == kAccent.g && sw::g_accent.b == kAccent.b);
        f = presents;
        SendMessageW(w, WM_SETTINGCHANGE, 0, 0);
        got += " | setting change: frames " + std::to_string(presents - f);
        f = presents;
        SendMessageW(w, WM_DWMCOLORIZATIONCOLORCHANGED, 0, 0);
        got += " | colorization: frames " + std::to_string(presents - f);
        f = presents;
        InvalidateRect(w, nullptr, FALSE);
        SendMessageW(w, WM_PAINT, 0, 0);
        got += " | paint: frames " + std::to_string(presents - f);
        got += " | erase: " + std::to_string((long long)SendMessageW(w, WM_ERASEBKGND, 0, 0)) + " " + Posted();
        sw::g_accent = kAccent;
    }
    Pin("WM_ACTIVATE, WM_SETTINGCHANGE, WM_DWMCOLORIZATIONCOLORCHANGED, WM_PAINT, WM_ERASEBKGND", got, "deactivate: frames 1 result 0 accent kept 1 | setting change: frames 1 | colorization: frames 1 | paint: frames 1 | erase: 1 none");
    // a move that ends on a monitor of the same scale setting but another resolution
    got.clear();
    for (HMONITOR m : { kLaptop, kBig, kDesktop, kLaptop }) {
        windowOn = m;
        const int f = presents;
        SendMessageW(w, WM_EXITSIZEMOVE, 0, 0);
        RECT rc; GetClientRect(w, &rc);
        got += MonName(m) + ": dpi " + G(sw::g_dpi) + " client " + std::to_string(rc.right) + "x" + std::to_string(rc.bottom) + " frames " + std::to_string(presents - f) + "; ";
    }
    windowOn = nullptr;
    Pin("WM_EXITSIZEMOVE", got, "laptop: dpi 120 client 825x750 frames 0; big: dpi 144 client 990x900 frames 2; desktop: dpi 96 client 660x600 frames 2; laptop: dpi 120 client 825x750 frames 2; ");
    // closed while dragging: the drag is applied as a release, saved
    got.clear();
    GoToPage(0); SetGroups(true);
    g_settings = Settings{}; Fresh();
    if (pressSlider(gap)) {
        ResetCounts();
        sw::Close();
        got += "edge gap: " + Posted() + " gap " + std::to_string(g_settings.edgeGap) + " drag " + std::to_string(sw::g_drag) + " open " + std::to_string(sw::IsOpen());
    }
    g_settings = Settings{};
    if (OpenAt(kLaptop, false)) {
        GoToPage(0); SetGroups(true); Fresh();
        if (pressSlider(icon)) {
            ResetCounts();
            const bool taken = Esc();   // Esc while a slider is held: no button drag to cancel, so it closes
            got += " | icon size, Esc: " + std::to_string(taken) + " " + Posted() + " icon " + G(g_settings.iconSize) + " open " + std::to_string(sw::IsOpen());
        }
    }
    g_settings = Settings{};
    if (OpenAt(kLaptop, false)) {
        GoToPage(2); sw::Render(); Fresh();
        if (HotCentre(sw::kHotDock - 0, 0, x, y)) {
            Mouse(WM_LBUTTONDOWN, x, y);
            Mouse(WM_MOUSEMOVE, x, sw::g_preview.bottom + 120);
            const HWND was = sw::g_wnd;
            sw::Close();
            got += " | button drag: " + Posted() + " buttons " + g_settings.trayButtons + " armed " + std::to_string(sw::g_bd.armed) +
                   " active " + std::to_string(sw::g_bd.active) + " capture kept " + std::to_string(captured == was);
        }
    }
    sw::g_bd = {}; captured = nullptr;
    Pin("closed while dragging", got, "edge gap: relayout1 saved gap 40 drag -1 open 0 | icon size, Esc: 1 rebuild1 saved icon 64 open 0 | button drag: saved buttons desktop,theme,sff armed 1 active 1 capture kept 1");
    // Esc: only the settings window's own WM_KEYDOWN of VK_ESCAPE is taken
    got.clear();
    g_settings = Settings{};
    if (OpenAt(kLaptop, false)) {
        MSG other = { g_mainWnd, WM_KEYDOWN, VK_ESCAPE, 0 }, key = { sw::g_wnd, WM_KEYDOWN, VK_RETURN, 0 }, up = { sw::g_wnd, WM_KEYUP, VK_ESCAPE, 0 };
        got += "other window " + std::to_string(sw::PreTranslate(&other)) + ", Enter " + std::to_string(sw::PreTranslate(&key)) + ", key up " +
               std::to_string(sw::PreTranslate(&up)) + ", open " + std::to_string(sw::IsOpen());
        Fresh();
        const bool taken = Esc();   // before IsOpen: the operands of + are evaluated in no fixed order
        got += " | Esc " + std::to_string(taken) + " open " + std::to_string(sw::IsOpen()) + " " + Posted();
        MSG late = { nullptr, WM_KEYDOWN, VK_ESCAPE, 0 };
        got += " | closed: " + std::to_string(sw::PreTranslate(&late));
    }
    Pin("PreTranslate", got, "other window 0, Enter 0, key up 0, open 1 | Esc 1 open 0 saved | closed: 0");
    g_settings = Settings{}; Drain(); Unsave();
}

// ---------------------------------------------------------------- 4: the button customizer

static bool DropAt(int from, bool palette, float x, float y) {   // a drag from a preview button (or a listed one) to (x, y)
    float sx, sy;
    if (!HotCentre(palette ? sw::kHotPalette - from : sw::kHotDock - from, 0, sx, sy)) return false;
    Mouse(WM_LBUTTONDOWN, sx, sy);
    Mouse(WM_MOUSEMOVE, sx + 10, sy);
    Mouse(WM_MOUSEMOVE, x, y);
    Mouse(WM_LBUTTONUP, x, y);
    return true;
}
// Where a dragged button would land, asked of ShownList at exact DIP points (no mouse: the pixel rounding of a
// message cannot hit a boundary): the listed trash dragged onto the preview (desktop, theme, sff), at each drop
// centre and a thousandth either side, and the four edges of the preview's drop area and just outside them.
static std::string DropProbe() {
    sw::g_bd = {}; sw::g_bd.armed = sw::g_bd.active = true; sw::g_bd.fromDock = false; sw::g_bd.id = ButtonId::Trash;
    sw::g_bd.x = (sw::g_preview.left + sw::g_preview.right) / 2; sw::g_bd.y = (sw::g_preview.top + sw::g_preview.bottom) / 2;
    sw::Render();   // the drop centres of the chosen buttons without the dragged one
    const D2D1_RECT_F p = sw::g_preview;
    const float my = (p.top + p.bottom) / 2, mx = (p.left + p.right) / 2;
    auto at = [&](float x, float y) {
        sw::g_bd.x = x; sw::g_bd.y = y;
        const auto l = sw::ShownList();
        const auto it = std::find(l.begin(), l.end(), ButtonId::Trash);
        return it == l.end() ? std::string("out") : std::to_string(it - l.begin());
    };
    std::string s = "preview " + U8(R4(p)) + " centres";
    for (float c : sw::g_dropCentres) s += " " + U8(F3(c));
    for (float c : std::vector<float>(sw::g_dropCentres)) s += " | " + at(c - 0.001f, my) + "<" + at(c, my) + "<" + at(c + 0.001f, my);
    s += " | top " + at(mx, p.top - 16) + " " + at(mx, p.top - 16.01f) + " bottom " + at(mx, p.bottom + 16) + " " + at(mx, p.bottom + 16.01f) +
         " left " + at(p.left - 12, my) + " " + at(p.left - 12.01f, my) + " right " + at(p.right + 12, my) + " " + at(p.right + 12.01f, my);
    sw::g_bd = {};
    sw::Render();
    return s;
}
// at 1.0, 1.25, 1.5: the steps land on points rounded to whole pixels, so a drop on a cell's edge can differ
static const char* const kCustomizerWant[3] = {
    "first to the end: theme,sff,desktop relayout1 saved; "
        "last to the front: desktop,theme,sff relayout1 saved; "
        "trash between the first two: desktop,trash,theme,sff relayout1 saved; "
        "audio dropped off the preview: desktop,trash,theme,sff relayout1 saved; "
        "audio clicked: desktop,trash,theme,sff,audio relayout1 saved; "
        "a preview button clicked: desktop,trash,theme,sff,audio none; "
        "moved 3 DIPs and let go: desktop,trash,theme,sff,audio none; "
        "dragged along the preview and back: trash,desktop,theme,sff,audio relayout1 saved; "
        "menu 1 on the list's quiet: trash,desktop,theme,sff,audio,quiet relayout1 saved; "
        "menu 2 on the first (greyed): trash,desktop,theme,sff,audio,quiet none; "
        "menu 3 on the first: desktop,trash,theme,sff,audio,quiet relayout1 saved; "
        "menu 3 on the last (greyed): desktop,trash,theme,sff,audio,quiet none; "
        "menu 2 on the last: desktop,trash,theme,sff,quiet,audio relayout1 saved; "
        "menu 4 on the second: desktop,theme,sff,quiet,audio relayout1 saved; "
        "menu 1 on a preview button (not in its menu): desktop,theme,sff,quiet,audio none; "
        "menu 4 on the list (not in its menu): desktop,theme,sff,quiet,audio none; "
        "dragged off, Esc: desktop,theme,sff,quiet,audio none; "
        "",
    "first to the end: theme,sff,desktop relayout1 saved; "
        "last to the front: desktop,theme,sff relayout1 saved; "
        "trash between the first two: desktop,theme,trash,sff relayout1 saved; "
        "audio dropped off the preview: desktop,theme,trash,sff relayout1 saved; "
        "audio clicked: desktop,theme,trash,sff,audio relayout1 saved; "
        "a preview button clicked: desktop,theme,trash,sff,audio none; "
        "moved 3 DIPs and let go: desktop,theme,trash,sff,audio none; "
        "dragged along the preview and back: desktop,trash,theme,sff,audio relayout1 saved; "
        "menu 1 on the list's quiet: desktop,trash,theme,sff,audio,quiet relayout1 saved; "
        "menu 2 on the first (greyed): desktop,trash,theme,sff,audio,quiet none; "
        "menu 3 on the first: trash,desktop,theme,sff,audio,quiet relayout1 saved; "
        "menu 3 on the last (greyed): trash,desktop,theme,sff,audio,quiet none; "
        "menu 2 on the last: trash,desktop,theme,sff,quiet,audio relayout1 saved; "
        "menu 4 on the second: trash,theme,sff,quiet,audio relayout1 saved; "
        "menu 1 on a preview button (not in its menu): trash,theme,sff,quiet,audio none; "
        "menu 4 on the list (not in its menu): trash,theme,sff,quiet,audio none; "
        "dragged off, Esc: trash,theme,sff,quiet,audio none; "
        "",
    "first to the end: theme,sff,desktop relayout1 saved; "
        "last to the front: desktop,theme,sff relayout1 saved; "
        "trash between the first two: desktop,trash,theme,sff relayout1 saved; "
        "audio dropped off the preview: desktop,trash,theme,sff relayout1 saved; "
        "audio clicked: desktop,trash,theme,sff,audio relayout1 saved; "
        "a preview button clicked: desktop,trash,theme,sff,audio none; "
        "moved 3 DIPs and let go: desktop,trash,theme,sff,audio none; "
        "dragged along the preview and back: trash,desktop,theme,sff,audio relayout1 saved; "
        "menu 1 on the list's quiet: trash,desktop,theme,sff,audio,quiet relayout1 saved; "
        "menu 2 on the first (greyed): trash,desktop,theme,sff,audio,quiet none; "
        "menu 3 on the first: desktop,trash,theme,sff,audio,quiet relayout1 saved; "
        "menu 3 on the last (greyed): desktop,trash,theme,sff,audio,quiet none; "
        "menu 2 on the last: desktop,trash,theme,sff,quiet,audio relayout1 saved; "
        "menu 4 on the second: desktop,theme,sff,quiet,audio relayout1 saved; "
        "menu 1 on a preview button (not in its menu): desktop,theme,sff,quiet,audio none; "
        "menu 4 on the list (not in its menu): desktop,theme,sff,quiet,audio none; "
        "dragged off, Esc: desktop,theme,sff,quiet,audio none; "
        "",
};
static const char* const kDropWant =
    "preview 208.000,140.000,628.000,252.000 centres 490.140 505.010 505.020 | 0<0<1 | 1<1<2 | 2<2<3 | "
    "top 0 out bottom 0 out left 0 out right 3 out";   // the same at every scale (DIPs)
static void Customizer() {
    for (int si = 0; si < 3; ++si) {
        std::string got;
        g_settings = Settings{};
        if (!OpenAt(kScales[si].m, false)) { Check(false, "open for the customizer"); return; }
        GoToPage(2);
        sw::Render();
        Pin(std::string("customizer at ") + kScales[si].name + ": where a drop lands (ShownList at exact points)", DropProbe(), kDropWant);
        auto step = [&](const char* what, auto act) {
            sw::Render(); Fresh();
            act();
            got += std::string(what) + ": " + g_settings.trayButtons + " " + Posted() + "; ";
        };
        auto cell = [](int i, D2D1_RECT_F& r) { for (auto& h : sw::g_hots) if (h.control == sw::kHotDock - i) { r = h.r; return true; } return false; };
        D2D1_RECT_F r0{}, r2{};
        step("first to the end", [&] { if (cell(2, r2)) DropAt(0, false, r2.right + 4, (r2.top + r2.bottom) / 2); });
        step("last to the front", [&] { if (cell(0, r0)) DropAt(2, false, r0.left - 4, (r0.top + r0.bottom) / 2); });
        step("trash between the first two", [&] {
            D2D1_RECT_F a{}, b{};
            if (cell(0, a) && cell(1, b)) DropAt((int)ButtonId::Trash, true, (a.left + a.right + b.left + b.right) / 4, (a.top + a.bottom + b.top + b.bottom) / 4);
        });
        step("audio dropped off the preview", [&] { DropAt((int)ButtonId::Audio, true, sw::g_preview.left + 20, sw::g_preview.bottom + 150); });
        step("audio clicked", [&] { float x, y; if (HotCentre(sw::kHotPalette - (int)ButtonId::Audio, 0, x, y)) Click(x, y); });
        step("a preview button clicked", [&] { float x, y; if (HotCentre(sw::kHotDock - 0, 0, x, y)) Click(x, y); });
        step("moved 3 DIPs and let go", [&] {
            float x, y;
            if (!HotCentre(sw::kHotDock - 1, 0, x, y)) return;
            Mouse(WM_LBUTTONDOWN, x, y); Mouse(WM_MOUSEMOVE, x + 3, y); Mouse(WM_LBUTTONUP, x + 3, y);
        });
        step("dragged along the preview and back", [&] {
            float x, y;
            if (!HotCentre(sw::kHotDock - 1, 0, x, y)) return;
            Mouse(WM_LBUTTONDOWN, x, y); Mouse(WM_MOUSEMOVE, x + 30, y); Mouse(WM_MOUSEMOVE, x, y); Mouse(WM_LBUTTONUP, x, y);
        });
        // the right-click menu's commands (the fake menu picks only an item it has enabled)
        auto menu = [&](int hot, int choice) {
            float x, y;
            if (!HotCentre(hot, 0, x, y)) return;
            popupChoice = choice;
            Mouse(WM_RBUTTONUP, x, y);
            popupChoice = 0;
        };
        const int n = (int)ChosenButtons().size();
        step("menu 1 on the list's quiet", [&] { menu(sw::kHotPalette - (int)ButtonId::Quiet, 1); });
        step("menu 2 on the first (greyed)", [&] { menu(sw::kHotDock - 0, 2); });
        step("menu 3 on the first", [&] { menu(sw::kHotDock - 0, 3); });
        step("menu 3 on the last (greyed)", [&] { menu(sw::kHotDock - n, 3); });
        step("menu 2 on the last", [&] { menu(sw::kHotDock - n, 2); });
        step("menu 4 on the second", [&] { menu(sw::kHotDock - 1, 4); });
        step("menu 1 on a preview button (not in its menu)", [&] { menu(sw::kHotDock - 0, 1); });
        step("menu 4 on the list (not in its menu)", [&] { menu(sw::kHotPalette - (int)ButtonId::Downloads, 4); });
        // a drag cancelled with Esc (one whose capture is lost: InputMessages)
        step("dragged off, Esc", [&] {
            float x, y;
            if (!HotCentre(sw::kHotDock - 0, 0, x, y)) return;
            Mouse(WM_LBUTTONDOWN, x, y); Mouse(WM_MOUSEMOVE, x, sw::g_preview.bottom + 120);
            Esc();
            Mouse(WM_LBUTTONUP, x, sw::g_preview.bottom + 120);
        });
        Pin(std::string("customizer at ") + kScales[si].name, got, kCustomizerWant[si]);
        g_settings = Settings{};
        sw::Close(); Drain();
    }
    Unsave();
}

// The preview's proportions over the shape page's sliders: for every icon size, the dock thickness from which its
// buttons stack two to a cell (2 rows), and a hash of the preview's lay-out (PreviewSummary) at every pair; the icon
// gap and spacing at a few sizes. DIPs: the same at every scale.
static const char* const kStackWant =
    "32:110 34:105 36:100 38:95 40:90 42:85 44:80 46:75 48:75 50:75 52:75 54:75 56:75 58:75 60:75 62:75 64:75 ";
static const char* const kStackLayoutWant = "642494b807398588";
static void StackSweep() {
    for (int si = 0; si < 3; ++si) {
        g_settings = Settings{};
        if (!OpenAt(kScales[si].m, false)) { Check(false, "open for the preview sweep"); return; }
        GoToPage(2);
        std::string from, all;
        for (int size = 32; size <= 64; size += 2) {
            int first = 0;
            for (int t = 75; t <= 150; t += 5) {
                g_settings.iconSize = (float)size; g_settings.dockThickness = t;
                sw::Render();
                const std::string s = PreviewSummary();
                all += s + "\n";
                if (!first && s.find("in 2 row(s)") != std::string::npos) first = t;
            }
            from += std::to_string(size) + ":" + std::to_string(first) + " ";
        }
        for (int gap : { 0, 10, 24, 60 })
            for (int spacing : { 0, 1, 2 })
                for (int size : { 32, 44, 64 }) {
                    g_settings = Settings{}; g_settings.iconGap = gap; g_settings.iconSpacing = spacing; g_settings.iconSize = (float)size;
                    sw::Render();
                    all += PreviewSummary() + "\n";
                }
        Pin(std::string("preview sweep at ") + kScales[si].name + ": the dock thickness from which the buttons stack, per icon size", from, kStackWant);
        Pin(std::string("preview sweep at ") + kScales[si].name + ": the preview's lay-out at 272 size/thickness and 36 gap/spacing pairs (hash)",
            Hex(Fnv(all.data(), all.size())), kStackLayoutWant);
        g_settings = Settings{};
        sw::Close(); Drain();
    }
    Unsave();
}

// ================================================================ characterization, second round
// What the first round left loose: the shared device context put back after drawing, a slider's value from where it
// is pressed, what Show sets up, the input paths of Proc that the checks above never take, the accent and what goes
// on to DefWindowProc, branches of the pages' lambdas, drawing states no scene had, and the preview shrunk to fit.

static const sw::Control* Ctl(int p, const wchar_t* label, int* index = nullptr) {
    const auto& cs = sw::g_pages[p].controls;
    for (int i = 0; i < (int)cs.size(); ++i) if (!wcscmp(cs[i].label, label)) { if (index) *index = i; return &cs[i]; }
    return nullptr;
}

// Render puts the docks' context back as it found it (unit mode, dpi): the docks draw in pixels at their own dpi on
// the same context. Poisoned before each way the window draws: opening, a click, a move to another monitor.
static void RenderKeepsContext() {
    auto* dc = g_gfx.dc.Get();
    float ox, oy; dc->GetDpi(&ox, &oy);
    const auto om = dc->GetUnitMode();
    auto poison = [&] { dc->SetUnitMode(D2D1_UNIT_MODE_PIXELS); dc->SetDpi(111, 111); };
    auto state = [&] { float x, y; dc->GetDpi(&x, &y); return std::to_string((int)dc->GetUnitMode()) + " " + G(x) + "," + G(y); };
    std::string got;
    for (int si = 0; si < 3; ++si) {
        g_settings = Settings{};
        poison();
        if (!OpenAt(kScales[si].m, false)) { Check(false, "open for the device context"); dc->SetUnitMode(om); dc->SetDpi(ox, oy); return; }
        got += std::string(kScales[si].name) + " open " + state();
        poison(); GoToPage(2); got += ", click " + state();
        poison(); windowOn = kScales[(si + 1) % 3].m; SendMessageW(sw::g_wnd, WM_EXITSIZEMOVE, 0, 0); windowOn = nullptr;
        got += ", moved " + state() + " (dpi " + G(sw::g_dpi) + ")";
        RECT r = { 100, 100, 900, 800 };
        poison(); movedTo = kScales[si].m; SendMessageW(sw::g_wnd, WM_DPICHANGED, 0, (LPARAM)&r); movedTo = nullptr;
        got += ", dpi changed " + state() + " (dpi " + G(sw::g_dpi) + "); ";
    }
    dc->SetUnitMode(om); dc->SetDpi(ox, oy);
    Pin("the docks' device context after the settings window drew (unit mode, dpi; set to pixels, 111 before)", got,
        "s100 open 1 111,111, click 1 111,111, moved 1 111,111 (dpi 120), dpi changed 1 111,111 (dpi 96); s125 open 1 111,111, click 1 111,111, moved 1 111,111 (dpi 144), dpi changed 1 111,111 (dpi 120); "
        "s150 open 1 111,111, click 1 111,111, moved 1 111,111 (dpi 96), dpi changed 1 111,111 (dpi 144); ");
    if (sw::IsOpen()) sw::Close();
    Drain(); Unsave();
}

// A slider pressed off its steps: the value is rounded to a step (1/3, 2/3 and 7 % along the track); a Live slider
// held where a move changes no value posts nothing, a move that does posts one relayout.
static void SliderValues() {
    std::string got;
    for (int si = 0; si < 3; ++si) {
        g_settings = Settings{};
        if (!OpenAt(kScales[si].m, false)) { Check(false, "open for the slider values"); return; }
        got += std::string(kScales[si].name) + ":";
        struct Which { int page; const wchar_t* label; };
        for (const Which w : { Which{ 0, L"아이콘 크기" }, Which{ 0, L"독 두께" }, Which{ 1, L"확대 효과" }, Which{ 1, L"나타나는 속도" } }) {
            GoToPage(w.page); SetGroups(true);
            int ci = -1; const sw::Control* c = Ctl(w.page, w.label, &ci);
            float x, y;
            if (!c || !HotCentre(ci, 0, x, y)) { got += " missing"; continue; }
            const auto t = sw::g_sliderTrack[ci];
            got += " [" + U8(w.label) + "]";
            for (float f : { 1 / 3.f, 2 / 3.f, 0.07f }) {
                g_settings = Settings{}; sw::Render(); Fresh();
                const float px = t.left + (t.right - t.left) * f;
                Mouse(WM_LBUTTONDOWN, px, y);
                got += " " + G(c->value());
                Mouse(WM_LBUTTONUP, px, y);
            }
            SetGroups(false);
        }
        GoToPage(0); SetGroups(true);
        int gap = -1; Ctl(0, L"화면 가장자리와 거리", &gap);
        float x, y;
        g_settings = Settings{}; sw::Render();
        if (gap >= 0 && HotCentre(gap, 0, x, y)) {
            const auto t = sw::g_sliderTrack[gap];
            const float mx = t.left + (t.right - t.left) * 0.5f;
            Fresh();
            Mouse(WM_LBUTTONDOWN, mx, y);
            got += " | live down " + Posted() + " gap " + std::to_string(g_settings.edgeGap);
            ResetCounts();
            Mouse(WM_MOUSEMOVE, mx, y); Mouse(WM_MOUSEMOVE, mx, y + 3);
            got += ", unchanged moves " + Posted();
            ResetCounts();
            Mouse(WM_MOUSEMOVE, t.left + (t.right - t.left) * 0.75f, y);
            got += ", a move " + Posted() + " gap " + std::to_string(g_settings.edgeGap);
            ResetCounts();
            Mouse(WM_LBUTTONUP, t.left + (t.right - t.left) * 0.75f, y);
            got += ", up " + Posted();
        }
        SetGroups(false);
        got += "; ";
        sw::Close(); Drain();
    }
    g_settings = Settings{}; Unsave();
    Pin("sliders pressed at 1/3, 2/3 and 7 % of the track (icon size, thickness, magnification, show delay), a live slider held", got,
        "s100: [아이콘 크기] 42 54 34 [독 두께] 100 125 80 [확대 효과] 1.4 1.7 1.1 [나타나는 속도] 0.2 0.4 0.05 | live down relayout1 gap 20, unchanged moves none, a move relayout1 gap 30, up relayout1 saved; s125: [아이콘 크기] 42 54 34 [독 두께] 100 125 80 [확대 효과] 1.4 1.7 1.1 [나타나는 속도] 0.2 0.4 0.05 | live down relayout1 gap 20, unchanged moves none, a move relayout1 gap 30, up relayout1 saved; "
        "s150: [아이콘 크기] 42 54 34 [독 두께] 100 125 80 [확대 효과] 1.4 1.7 1.1 [나타나는 속도] 0.2 0.4 0.05 | live down relayout1 gap 20, unchanged moves none, a move relayout1 gap 30, up relayout1 saved; ");
}

// What Show sets up: the docks told which window holds them up (g_settingsHold.wnd, SettingsHold), the window centred
// on the work area of its monitor, its title and class, and a fresh state - first page, top, no hover, no drag, no
// wheel gesture - whatever the window it replaces was left at. Moved by the user, it keeps its top-left.
static void ShowState() {
    std::string got;
    for (int si = 0; si < 3; ++si) {
        const HMONITOR m = kScales[si].m;
        g_settings = Settings{};
        if (!OpenAt(m, false)) { Check(false, "open for Show"); return; }
        got += std::string(kScales[si].name) + ": held by " + std::to_string(g_settingsHold.wnd == sw::g_wnd && sw::g_wnd);
        RECT r; GetWindowRect(sw::g_wnd, &r);
        MONITORINFO mi = { sizeof(mi) }; MatrixMonitorInfo(m, &mi);
        const int dx = (r.left + r.right) / 2 - (mi.rcWork.left + mi.rcWork.right) / 2, dy = (r.top + r.bottom) / 2 - (mi.rcWork.top + mi.rcWork.bottom) / 2;
        got += " centred " + std::to_string(std::abs(dx) <= 1 && std::abs(dy) <= 1);
        wchar_t title[64] = {}, cls[64] = {};
        GetWindowTextW(sw::g_wnd, title, 64); GetClassNameW(sw::g_wnd, cls, 64);
        got += " [" + U8(title) + "] [" + U8(cls) + "]";
        // moved by the user: WM_EXITSIZEMOVE on another monitor keeps the top-left, WM_DPICHANGED takes the suggested one
        RECT a; GetWindowRect(sw::g_wnd, &a);
        windowOn = kScales[(si + 1) % 3].m; SendMessageW(sw::g_wnd, WM_EXITSIZEMOVE, 0, 0); windowOn = nullptr;
        RECT b; GetWindowRect(sw::g_wnd, &b);
        RECT sug = { a.left + 50, a.top + 30, a.left + 850, a.top + 730 };
        movedTo = m; SendMessageW(sw::g_wnd, WM_DPICHANGED, 0, (LPARAM)&sug); movedTo = nullptr;
        RECT c; GetWindowRect(sw::g_wnd, &c);
        got += " | moved: top-left kept " + std::to_string(b.left == a.left && b.top == a.top) + ", suggested taken " + std::to_string(c.left == sug.left && c.top == sug.top);
        // left on the 시스템 page, scrolled, hovering a segment, after a wheel gesture there
        GoToPage(3); sw::Render();
        float x, y;
        if (HotCentre(1, 2, x, y)) { Wheel(x, y, -WHEEL_DELTA); HoverOn(1, 2); }
        sw::g_scroll = 37;
        got += " | left at page " + std::to_string(sw::g_page) + " hover " + std::to_string(sw::g_hover) + "/" + std::to_string(sw::g_hoverSub) + " wheel page " + std::to_string(sw::g_wheel.page);
        sw::Close();
        got += " | closed: held by " + std::to_string(g_settingsHold.wnd != nullptr) + " open " + std::to_string(g_settingsHold.open);
        recOps.clear(); recTexts.clear(); recFonts.clear(); recOn = true;
        sw::Show(m);
        recOn = false;
        Drain();
        got += " | shown again: page " + std::to_string(sw::g_page) + " scroll " + G(sw::g_scroll) + " hover " + std::to_string(sw::g_hover) + "/" + std::to_string(sw::g_hoverSub) +
               " drag " + std::to_string(sw::g_drag) + " wheel page " + std::to_string(sw::g_wheel.page) + " held by " + std::to_string(g_settingsHold.wnd == sw::g_wnd && sw::g_wnd) +
               ", its first frame: 모양 " + std::to_string(DrewCount(L"모양")) + " 시스템 " + std::to_string(DrewCount(L"시스템")) + " 위치 " + std::to_string(DrewCount(L"위치")) + "; ";
        sw::Close(); Drain();
    }
    g_settings = Settings{}; Unsave();
    Pin("Show: the docks held by it, centred, title and class, moved, and a fresh state when opened again", got,
        "s100: held by 1 centred 1 [Liquid Dock 설정] [LiquidDockSettings] | moved: top-left kept 1, suggested taken 1 | left at page 3 hover 1/2 wheel page 3 | closed: held by 0 open 0 | shown again: page 0 scroll 0 hover -1/-1 drag -1 wheel page -1 held by 1, its first frame: 모양 2 시스템 1 위치 1; s125: held by 1 centred 1 [Liquid Dock 설정] [LiquidDockSettings] | moved: top-left kept 1, suggested taken 1 | left at page 3 hover 1/2 wheel page 3 | closed: held by 0 open 0 | shown again: page 0 scroll 0 hover -1/-1 drag -1 wheel page -1 held by 1, its first frame: 모양 2 시스템 1 위치 1; "
        "s150: held by 1 centred 1 [Liquid Dock 설정] [LiquidDockSettings] | moved: top-left kept 1, suggested taken 1 | left at page 3 hover 1/2 wheel page 3 | closed: held by 0 open 0 | shown again: page 0 scroll 0 hover -1/-1 drag -1 wheel page -1 held by 1, its first frame: 모양 2 시스템 1 위치 1; ");
}

// Proc's input paths the checks above never take: a button dropped where it is let go (not where it last moved);
// the drag starting past 4 DIPs; a group's header starting a new wheel gesture; another page starting at its top;
// the hover moving between segments of one control; the menu at the click's screen pixel; Esc as the window's own
// WM_KEYDOWN (without PreTranslate).
static void InputPaths() {
    std::string got;
    for (int si = 0; si < 3; ++si) {
        g_settings = Settings{};
        if (!OpenAt(kScales[si].m, false)) { Check(false, "open for the input paths"); return; }
        const HWND w = sw::g_wnd;
        got += std::string(kScales[si].name) + ":";
        float x, y;
        GoToPage(2); sw::Render();
        if (HotCentre(sw::kHotDock - 0, 0, x, y)) {
            D2D1_RECT_F last{};
            for (auto& h : sw::g_hots) if (h.control == sw::kHotDock - 2) last = h.r;
            const float ex = last.right + 4, ey = (last.top + last.bottom) / 2;
            Fresh();
            Mouse(WM_LBUTTONDOWN, x, y); Mouse(WM_MOUSEMOVE, x + 10, y); Mouse(WM_MOUSEMOVE, x, sw::g_preview.bottom + 120);
            Mouse(WM_LBUTTONUP, ex, ey);
            got += " moved off, let go over the end: " + g_settings.trayButtons + " " + Posted();
            g_settings = Settings{}; sw::Render(); Fresh();
            Mouse(WM_LBUTTONDOWN, x, y); Mouse(WM_MOUSEMOVE, x + 10, y); Mouse(WM_MOUSEMOVE, ex, ey);
            Mouse(WM_LBUTTONUP, x, sw::g_preview.bottom + 120);
            got += ", moved over the end, let go off: " + g_settings.trayButtons + " " + Posted();
            // the drag starts once the pointer is more than 4 DIPs from the press
            std::string act;
            for (const POINT d : { POINT{ 3, 0 }, POINT{ 5, 0 }, POINT{ 0, 3 }, POINT{ 0, 5 }, POINT{ 3, 3 } }) {
                g_settings = Settings{}; sw::Render(); Fresh();
                Mouse(WM_LBUTTONDOWN, x, y); Mouse(WM_MOUSEMOVE, x + d.x, y + d.y);
                act += std::to_string(sw::g_bd.active);
                Esc();
            }
            got += ", active after 3/5 across, 3/5 down, 3+3: " + act + " " + Posted();
        }
        // a click on a group's header: the next wheel at the same point is a new gesture (the slider now under it)
        GoToPage(0); SetGroups(true);
        int icon = -1; Ctl(0, L"아이콘 크기", &icon);
        float px, py, hx, hy;
        if (icon >= 0 && HotCentre(icon, 0, px, py)) {
            SetGroups(false);
            g_settings = Settings{}; Fresh();
            Wheel(px, py, -WHEEL_DELTA);
            got += " | wheel, header, wheel at one point: " + G(g_settings.iconSize) + "/" + G(sw::g_scroll);
            if (HotCentre(1, 0, hx, hy)) Click(hx, hy);
            Wheel(px, py, -WHEEL_DELTA);
            got += " -> " + G(g_settings.iconSize) + "/" + G(sw::g_scroll) + " " + Posted();
        }
        // another page: at its top
        GoToPage(1); SetGroups(true); GoToPage(0); SetGroups(true);
        sw::g_scroll = 100; sw::Render();
        GoToPage(1);
        got += " | page 0 scrolled 100, then page " + std::to_string(sw::g_page) + ": scroll " + G(sw::g_scroll);
        SetGroups(false); GoToPage(0); SetGroups(false);
        // the hover from one segment to another of one control: drawn again
        HoverOff();
        int glass = -1; Ctl(0, L"유리 질감", &glass);
        if (glass >= 0 && HoverOn(glass, 0)) {
            int f = presents;
            HoverOn(glass, 2);
            got += " | hover segment 0 -> 2: frames " + std::to_string(presents - f) + " sub " + std::to_string(sw::g_hoverSub);
            f = presents;
            HoverOn(glass, 2);
            got += ", again: frames " + std::to_string(presents - f);
        }
        HoverOff();
        // the menu opens at the click, in screen pixels
        GoToPage(2); sw::Render();
        auto menuAt = [&](int hot) {
            float mx, my;
            if (!HotCentre(hot, 0, mx, my)) return std::string("missing");
            popupAt = { -99999, -99999 }; popupChoice = 0;
            Mouse(WM_RBUTTONUP, mx, my);
            POINT o = { 0, 0 }; ClientToScreen(w, &o);
            const POINT c = ClientPx(mx, my);
            return std::to_string(popupAt.x - o.x - c.x) + "," + std::to_string(popupAt.y - o.y - c.y);
        };
        got += " | menu off the click (px): preview " + menuAt(sw::kHotDock - 1) + " list " + menuAt(sw::kHotPalette - (int)ButtonId::Trash);
        // Esc and Enter sent to the window itself
        Fresh();
        SendMessageW(w, WM_KEYDOWN, VK_RETURN, 0);
        got += " | Enter: open " + std::to_string(sw::IsOpen());
        SendMessageW(w, WM_KEYDOWN, VK_ESCAPE, 0);
        got += ", Esc: open " + std::to_string(sw::IsOpen()) + " docks held " + std::to_string(g_settingsHold.open) + " " + Posted() + "; ";
        if (sw::IsOpen()) sw::Close();
        Drain();
    }
    g_settings = Settings{}; Unsave();
    Pin("input paths: drop point, drag start, a new wheel gesture after a header, a new page's scroll, segment hover, menu point, Esc", got,
        "s100: moved off, let go over the end: theme,sff,desktop relayout1 saved, moved over the end, let go off: theme,sff relayout1 saved, active after 3/5 across, 3/5 down, 3+3: 01011 none | wheel, header, wheel at one point: 44/0 -> 42/0 rebuild1 saved | page 0 scrolled 100, then page 1: scroll 0 | hover segment 0 -> 2: frames 1 sub 2, again: frames 0 | menu off the click (px): preview 0,0 list 0,0 | Enter: open 1, Esc: open 0 docks held 0 saved; s125: moved off, let go over the end: theme,sff,desktop relayout1 saved, moved over the end, let go off: theme,sff relayout1 saved, active after 3/5 across, 3/5 down, 3+3: 01010 none | wheel, header, wheel at one point: 44/0 -> 42/0 rebuild1 saved | page 0 scrolled 100, then page 1: scroll 0 | hover segment 0 -> 2: frames 1 sub 2, again: frames 0 | menu off the click (px): preview 0,0 list 0,0 | Enter: open 1, Esc: open 0 docks held 0 saved; "
        "s150: moved off, let go over the end: theme,sff,desktop relayout1 saved, moved over the end, let go off: theme,sff relayout1 saved, active after 3/5 across, 3/5 down, 3+3: 01011 none | wheel, header, wheel at one point: 44/0 -> 42/0 rebuild1 saved | page 0 scrolled 100, then page 1: scroll 0 | hover segment 0 -> 2: frames 1 sub 2, again: frames 0 | menu off the click (px): preview 0,0 list 0,0 | Enter: open 1, Esc: open 0 docks held 0 saved; ");
}

// The accent: read from the registry (ABGR) when the window opens and on WM_SETTINGCHANGE / WM_DWMCOLORIZATIONCOLORCHANGED,
// not on WM_ACTIVATE; on the dark backdrop a dark one is lifted (also when there is no value to read). The three
// messages go on to DefWindowProc (WM_ACTIVATE: the focus); the input messages the window handles do not.
static void AccentAndDefProc() {
    auto acc = [] { char b[64]; snprintf(b, sizeof b, "%.3f,%.3f,%.3f", sw::g_accent.r, sw::g_accent.g, sw::g_accent.b); return std::string(b); };
    const DWORD bright = 0x0000D7FF, darkOne = 0x00102030, was = accentValue;
    std::string got;
    for (bool light : { false, true }) {
        if (sw::IsOpen()) sw::Close();
        g_settings = Settings{}; themeLight = light;
        accentValue = darkOne; accentReads = 0;
        sw::Show(kLaptop);
        Drain();
        if (!sw::IsOpen()) { Check(false, "open for the accent"); accentValue = was; themeLight = false; return; }
        const HWND w = sw::g_wnd;
        got += std::string(light ? "light" : "dark") + ": opened " + acc() + " reads " + std::to_string(accentReads);
        accentValue = bright; defCalls.clear();
        SendMessageW(w, WM_ACTIVATE, WA_INACTIVE, 0);
        got += ", activate " + acc();
        SendMessageW(w, WM_SETTINGCHANGE, 0, 0);
        got += ", setting change " + acc();
        accentValue = darkOne;
        SendMessageW(w, WM_DWMCOLORIZATIONCOLORCHANGED, 0, 0);
        got += ", colorization " + acc();
        accentFails = true; sw::g_accent = { 0.1f, 0.1f, 0.1f, 1 };
        SendMessageW(w, WM_SETTINGCHANGE, 0, 0);
        accentFails = false;
        got += ", no value (from 0.1) " + acc() + " reads " + std::to_string(accentReads);
        SendMessageW(w, WM_MOUSEMOVE, 0, MAKELPARAM(5, 5));
        SendMessageW(w, WM_KEYDOWN, VK_RETURN, 0);
        InvalidateRect(w, nullptr, FALSE); SendMessageW(w, WM_PAINT, 0, 0);
        got += " | DefWindowProc: activate " + std::to_string(defCalls[WM_ACTIVATE]) + " setting " + std::to_string(defCalls[WM_SETTINGCHANGE]) +
               " colorization " + std::to_string(defCalls[WM_DWMCOLORIZATIONCOLORCHANGED]) + " move " + std::to_string(defCalls[WM_MOUSEMOVE]) +
               " key " + std::to_string(defCalls[WM_KEYDOWN]) + " paint " + std::to_string(defCalls[WM_PAINT]) + "; ";
        sw::Close(); Drain();
    }
    accentValue = was; themeLight = false; g_settings = Settings{}; Unsave();
    Pin("the accent read (ABGR, lifted on the dark backdrop) and the messages passed on to DefWindowProc", got,
        "dark: opened 0.484,0.444,0.405 reads 1, activate 0.484,0.444,0.405, setting change 1.000,0.843,0.000, colorization 0.484,0.444,0.405, no value (from 0.1) 0.450,0.450,0.450 reads 4 | DefWindowProc: activate 1 setting 2 colorization 1 move 0 key 0 paint 0; "
        "light: opened 0.188,0.125,0.063 reads 1, activate 0.188,0.125,0.063, setting change 1.000,0.843,0.000, colorization 0.188,0.125,0.063, no value (from 0.1) 0.100,0.100,0.100 reads 4 | DefWindowProc: activate 1 setting 2 colorization 1 move 0 key 0 paint 0; ");
}

// The pages' lambdas off their defaults: the icon gap's presets (iconGap 0), the position's words on every edge,
// each slider's text a step in from either end and at its boundaries, and the OLED switch on one monitor.
static void PageLambdas() {
    g_settings = Settings{};
    if (!OpenAt(kLaptop, false)) { Check(false, "open for the lambdas"); return; }
    std::string got;
    const sw::Control* gapC = Ctl(0, L"아이콘 간격");
    const sw::Control* posC = Ctl(0, L"가장자리 따라 위치");
    if (!gapC || !posC) { Check(false, "the icon gap and position sliders found"); return; }
    for (int sp : { -1, 0, 1, 2, 3 }) { g_settings = Settings{}; g_settings.iconGap = 0; g_settings.iconSpacing = sp; got += G(gapC->value()) + " "; }
    g_settings = Settings{}; g_settings.iconGap = 18; got += G(gapC->value()) + " |";
    for (Edge e : { Edge::Bottom, Edge::Left, Edge::Right, Edge::Top }) {
        g_settings = Settings{}; g_settings.edge = e;
        got += " " + U8(posC->text(20)) + "/" + U8(posC->text(-20)) + "/" + U8(posC->text(0));
    }
    g_settings = Settings{};
    got += " |";
    for (auto& pg : sw::g_pages)
        for (auto& c : pg.controls)
            if (c.type == sw::Control::Slider) got += " [" + U8(c.text(c.lo + c.step)) + "|" + U8(c.text(c.hi - c.step)) + "]";
    const sw::Control* magC = Ctl(1, L"확대 효과");
    const sw::Control* showC = Ctl(1, L"나타나는 속도");
    const sw::Control* edgeC = Ctl(0, L"화면 가장자리와 거리");
    if (magC && showC && edgeC)
        got += " | " + U8(magC->text(1.04f)) + " " + U8(magC->text(1.05f)) + " " + U8(magC->text(1.06f)) + " " + U8(showC->text(0.024f)) + " " +
               U8(showC->text(0.025f)) + " " + U8(edgeC->text(0)) + " " + U8(edgeC->text(1));
    Pin("the pages' lambdas: icon gap presets, the position's words per edge, texts a step in, boundaries", got,
        "22 22 32 44 44 18 | 오른쪽으로 20%/왼쪽으로 20%/가운데 아래로 20%/위로 20%/가운데 아래로 20%/위로 20%/가운데 오른쪽으로 20%/왼쪽으로 20%/가운데 | [34|62] [12%|58%] [80%|145%] [2 px|38 px] [왼쪽으로 95%|오른쪽으로 95%] [1.1배|2.0배] [0.05초|0.55초] [0.15초|1.45초] | 끄기 1.0배 1.1배 즉시 0.03초 붙임 1 px");
    // OLED on: all monitors, or the one the settings were opened on
    got.clear();
    const HMONITOR opened[] = { kLaptop, kDesktop };
    for (HMONITOR m : opened) {
        if (!OpenAt(m, false)) { Check(false, "open for OLED"); return; }
        const sw::Control* oledC = Ctl(2, L"OLED 모드 켜기");
        if (!oledC) { Check(false, "the OLED switch found"); return; }
        for (bool one : { false, true }) {
            g_settings = Settings{}; g_settings.oledOneMonitor = one; oledOn = false; oledFor = (HMONITOR)(intptr_t)-1;
            oledC->toggle(true);
            got += "opened on " + MonName(m) + (one ? " one monitor" : " all monitors") + ": " + (oledOn ? "on" : "off") + " for " + MonName(oledFor);
            oledC->toggle(false);
            got += ", off " + std::to_string(!oledOn) + "; ";
        }
    }
    oledOn = false; g_settings = Settings{};
    Pin("OLED on from the settings window: all monitors, or the one it was opened on", got,
        "opened on laptop all monitors: on for all, off 1; "
        "opened on laptop one monitor: on for laptop, off 1; "
        "opened on desktop all monitors: on for all, off 1; "
        "opened on desktop one monitor: on for desktop, off 1; ");
    if (sw::IsOpen()) sw::Close();
    Drain(); Unsave();
}

// Drawing states no scene above had (each scale; dark, and light for the branches only the light theme takes): a nav
// item, "기본값으로" and a switch that is off under the pointer, a slider held, the widest preview (shrunk once to fit
// the panel); light: no backdrop, a button dragged over the preview, a slider held, an off switch hovered.
struct Extra2 { const char* name; int page; bool open; bool light; void (*setup)(); };
static void HoldIconSlider() {
    int ci = -1; Ctl(0, L"아이콘 크기", &ci);
    float x, y;
    if (ci < 0 || !HotCentre(ci, 0, x, y)) return;
    const auto t = sw::g_sliderTrack[ci];
    Mouse(WM_LBUTTONDOWN, t.left + (t.right - t.left) / 3, y);
}
static void Widest() {
    g_settings.trayButtons = "desktop,theme,oled,awake,screenshot,quiet,trash,downloads,audio,sff";
    g_settings.iconSize = 64; g_settings.iconGap = 60; g_settings.dockThickness = 150;
    g_sys.SetBattery(battery::FromRaw({ 0, 1, 57, 0, 7200 }));
}
static const Extra2 kExtras2[] = {
    { "dark-p0-folded-nav-hover", 0, false, false, [] { HoverOn(-10 - 3, 0); } },
    { "dark-p0-folded-reset-hover", 0, false, false, [] { HoverOn(-2, 0); } },
    { "dark-p1-folded-off-toggle-hover", 1, false, false, [] { g_settings.autoHide = false; sw::Render(); HoverOn(0, 0); } },
    { "dark-p0-open-slider-held", 0, true, false, HoldIconSlider },
    { "dark-p2-widest", 2, false, false, Widest },
    { "dark-p2-widest-drag", 2, false, false, [] { Widest(); DragPreview(3, 0, 0); } },
    { "light-p0-folded-opaque", 0, false, true, [] { sw::g_acrylic = false; } },
    { "light-p2-drag-over", 2, false, true, [] { DragPreview(0, 0, 0); } },
    { "light-p0-open-slider-held", 0, true, true, HoldIconSlider },
    { "light-p1-folded-off-toggle-hover", 1, false, true, [] { g_settings.autoHide = false; sw::Render(); HoverOn(0, 0); } },
};
static const uint64_t kExtras2All = 0xefb9bfee77015e0cULL;
static std::map<std::string, std::string> kExtra2Want = {
    { "dark-p2-widest", "preview 10 buttons in 2 row(s) 501.09..578.09, list 0" },
    { "dark-p2-widest-drag", "preview 0 buttons in 0 row(s) 0.00..0.00, list 0 | dragging active shown desktop,theme,oled,screenshot,quiet,trash,downloads,audio,sff,awake drops 9" },
    { "light-p2-drag-over", "preview 0 buttons in 0 row(s) 0.00..0.00, list 0 | dragging active shown theme,sff,desktop drops 2" },
};
static void MoreScenes() {
    std::map<std::string, Shot> byScale[3];
    uint64_t all = 1469598103934665603ULL;
    for (int si = 0; si < 3; ++si)
        for (bool light : { false, true }) {
            g_settings = Settings{};
            if (!OpenAt(kScales[si].m, light)) { Check(false, "the settings window opens for the more scenes"); return; }
            for (const auto& e : kExtras2) {
                if (e.light != light) continue;
                g_settings = Settings{}; oledOn = false;
                GoToPage(e.page); HoverOff();
                SetGroups(e.open);
                e.setup();
                const Shot s = Shoot();
                byScale[si][e.name] = s;
                printf("HASH: settings_pixels/%s-%s %s\n", kScales[si].name, e.name, Hex(s.pixels).c_str());
                if (si == 1) {
                    printf("HASH: settings_draws/%s %s\n", e.name, Hex(s.draws).c_str());
                    printf("HASH: settings_hots/%s %s\n", e.name, Hex(s.hots).c_str());
                    all = Fnv(&s.draws, sizeof s.draws, all); all = Fnv(&s.hots, sizeof s.hots, all);
                    if (kExtra2Want.count(e.name)) Pin(std::string("customizer ") + e.name, PreviewSummary(), kExtra2Want[e.name]);
                }
                // leave nothing behind: a slider let go, a drag cancelled, no battery
                if (sw::g_drag >= 0) { Mouse(WM_LBUTTONUP, 300, 300); }
                if (sw::g_bd.armed) Esc();
                g_sys.SetBattery({});
                sw::g_acrylic = true; oledOn = false;
                Drain();
            }
        }
    // a button dragged follows the pointer rounded to pixels (Extras): those scenes' drawing calls at 1.0 and 1.5 go
    // into the hash too; the others are the same as at 1.25
    std::string differ[3];
    for (int si : { 0, 2 })
        for (const auto& [name, s] : byScale[si]) {
            if (name.find("drag") != std::string::npos) { all = Fnv(&s.draws, sizeof s.draws, all); all = Fnv(&s.hots, sizeof s.hots, all); continue; }
            const auto it = byScale[1].find(name);
            if (it == byScale[1].end() || it->second.draws != s.draws || it->second.hots != s.hots) differ[si] += " " + name;
        }
    for (int si : { 0, 2 }) Check(differ[si].empty(), (std::string("more scenes at ") + kScales[si].name + ": the same drawing calls and hot areas as at 1.25 (but a button dragged)" + differ[si]).c_str());
    Pin("more scenes: drawing calls and hot areas of all 10 at 1.25, and of the 2 with a button dragged at 1.0 and 1.5 (hash)", Hex(all), Hex(kExtras2All));
    g_settings = Settings{};
    if (sw::IsOpen()) sw::Close();
    Drain(); Unsave();
}

// The text formats: each one's family, size, weight and alignments as created, and which one each text of every page
// (folded and open) is drawn with - the same at every scale. Not in the drawing calls' hashes (they were pinned first).
static const uint64_t kFontsAll = 0x4fb6536fe8e9346bULL;
static void TextFormats() {
    g_settings = Settings{};
    if (!OpenAt(kLaptop, false)) { Check(false, "open for the text formats"); return; }
    const std::pair<const char*, IDWriteTextFormat*> fs[] = {
        { "title", sw::g_fTitle.Get() }, { "navTitle", sw::g_fNavTitle.Get() }, { "nav", sw::g_fNav.Get() }, { "label", sw::g_fLabel.Get() },
        { "value", sw::g_fValue.Get() }, { "hint", sw::g_fHint.Get() }, { "glyph", sw::g_fGlyph.Get() }, { "seg", sw::g_fSeg.Get() },
        { "segSmall", sw::g_fSegSmall.Get() }, { "chip", sw::g_fChip.Get() }, { "tiny", sw::g_fTiny.Get() },
    };
    std::string got;
    for (const auto& [n, f] : fs) got += std::string(n) + ": " + U8(FontText(f)) + "; ";
    Pin("the settings window's text formats (family, size, weight, text / paragraph alignment, wrapping)", got,
        "title: Segoe UI Variable Display 26.00 600 0 2 1; navTitle: Segoe UI Variable Text 15.00 600 0 2 1; nav: Segoe UI Variable Text 15.00 400 0 2 1; label: Segoe UI Variable Text 15.00 400 0 2 1; value: Segoe UI Variable Text 15.00 600 1 2 1; hint: Segoe UI Variable Text 12.50 400 0 2 1; glyph: <glyph> 16.00 400 0 2 1; seg: Segoe UI Variable Text 14.00 400 2 2 1; segSmall: Segoe UI Variable Text 12.00 400 2 2 1; chip: Segoe UI Variable Text 13.50 400 0 2 1; tiny: Segoe UI Variable Text 11.50 400 2 2 1; ");
    std::string differ;
    uint64_t all = 1469598103934665603ULL;
    std::map<std::string, uint64_t> at125;
    for (int si : { 1, 0, 2 }) {
        g_settings = Settings{};
        if (!OpenAt(kScales[si].m, false)) { Check(false, "open for the text formats"); return; }
        for (int p = 0; p < (int)sw::g_pages.size(); ++p) {
            GoToPage(p); HoverOff();
            for (int fold = 0; fold < (HasGroup(p) ? 2 : 1); ++fold) {
                SetGroups(fold == 1);
                if (p == 2) g_settings.trayButtons = fold ? "desktop,theme,oled,awake,screenshot,quiet,trash,downloads,audio,sff" : "desktop";
                const Shot s = Shoot();
                g_settings = Settings{};
                const std::string scene = "p" + std::to_string(p) + "-" + std::to_string(fold);
                if (si == 1) { at125[scene] = s.fonts; all = Fnv(&s.fonts, sizeof s.fonts, all); }
                else if (at125[scene] != s.fonts) differ += std::string(" ") + kScales[si].name + "-" + scene;
            }
            SetGroups(false);
        }
    }
    Check(differ.empty(), ("text formats of every page: the same at 1.0 and 1.5 as at 1.25" + differ).c_str());
    Pin("text formats of every page, folded and open (hash of the format of each text drawn)", Hex(all), Hex(kFontsAll));
    g_settings = Settings{};
    if (sw::IsOpen()) sw::Close();
    Drain(); Unsave();
}

// The preview with every button, at a few icon sizes, thicknesses and gaps, with and without a battery: wide enough
// that DrawCustomizer's second pass shrinks it (k scaled to the panel). DIPs: the same hash at every scale.
static const char* const kStackAllWant = "6950d55f6de433d6";
static void StackSweepAll() {
    for (int si = 0; si < 3; ++si) {
        g_settings = Settings{};
        if (!OpenAt(kScales[si].m, false)) { Check(false, "open for the widest preview sweep"); return; }
        GoToPage(2);
        std::string all;
        for (int bat = 0; bat < 2; ++bat) {
            g_sys.SetBattery(bat ? battery::FromRaw({ 0, 1, 57, 0, 7200 }) : battery::State{});
            for (int size : { 32, 40, 48, 56, 64 })
                for (int t : { 75, 110, 150 })
                    for (int gap : { 0, 30, 60 }) {
                        g_settings = Settings{};
                        g_settings.trayButtons = "desktop,theme,oled,awake,screenshot,quiet,trash,downloads,audio,sff";
                        g_settings.iconSize = (float)size; g_settings.dockThickness = t; g_settings.iconGap = gap;
                        sw::Render();
                        all += PreviewSummary() + "\n";
                    }
        }
        g_sys.SetBattery({});
        Pin(std::string("preview sweep with every button at ") + kScales[si].name + ": the lay-out at 90 size/thickness/gap/battery sets (hash)",
            Hex(Fnv(all.data(), all.size())), kStackAllWant);
        g_settings = Settings{};
        sw::Close(); Drain();
    }
    Unsave();
}

static void Characterization() {
    monitors.push_back({ kBig, { 0, 0, 3840, 2160 }, false });
    const LARGE_INTEGER t0 = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t; }();
    presentNow = true;
    PageMatrix();
    Extras();
    SliderTexts();
    SetterRoundTrip();
    ClickRoundTrip();
    PageResets();
    InputMessages();
    Customizer();
    StackSweep();
    // the second round
    RenderKeepsContext();
    SliderValues();
    ShowState();
    InputPaths();
    AccentAndDefProc();
    PageLambdas();
    MoreScenes();
    TextFormats();
    StackSweepAll();
    presentNow = false;
    g_settings = Settings{}; oledOn = false; themeLight = false;
    if (sw::IsOpen()) sw::Close();
    Drain(); Unsave();
    LARGE_INTEGER t1, f; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    fflush(stdout);   // stderr is unbuffered: without this the time lands in the middle of a buffered PASS line
    fprintf(stderr, "  characterization: %.1f s\n", double(t1.QuadPart - t0.QuadPart) / f.QuadPart);
}

int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    if (AppDataDir().find(L"\\tests\\dock-render\\") == std::wstring::npos) { puts("not in the fixture folder"); return 100; }
    if (!g_gfx.Init()) { puts("GPU initialization failed"); return 100; }
    g_gfx.EnsureFonts(1.25f);   // the docks resolve the icon font first
    g_sys.timeText = L"21:30"; g_sys.timeShort = L"21:30"; g_sys.dateText = L"2026-10-28";
    monitors.push_back({ kLaptop, { 0, 0, 2880, 1800 }, true });
    monitors.push_back({ kDesktop, { 0, 0, 2560, 1440 }, false });   // its own fake handle; placed where the laptop is
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"dock_render-settings-main";
    RegisterClassExW(&wc);
    g_mainWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_mainWnd) { puts("no message window"); return 100; }
    Opening();
    if (sw::IsOpen()) Folding();
    if (sw::IsOpen()) FoldingBehaviour();
    if (sw::IsOpen()) {
        Scrolling();
        WheelOnSliders();
        WheelGestures();
        WheelBursts();
        Controls();
        SensorPage();
        MovingMonitors();
        Closing();
        MinimizeCloses();
    }
    if (sw::IsOpen()) sw::Close();
    Characterization();
    Unsave();
    DestroyWindow(g_mainWnd);
    return Summary("dock_render_settings_window");
}
