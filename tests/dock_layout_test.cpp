// Dock windows, layout, monitors, auto-hide and input on fake test screens: a
// laptop's 2880x1800 panel (150%) and a desktop's 2560x1440 primary with a 1440x2560 portrait monitor
// on its left (negative coordinates). Runs the production Dock (dock.cpp, dock_window.cpp, dock_input.cpp,
// dock_layout.cpp, dock_render.cpp, dock_sensors.cpp, dock_panel.cpp, dock_controls.cpp, dock_notify.cpp) with fake monitors, a fake window in front, fake mouse buttons and
// pointer, fake mouse capture and a fake shell that holds app bars like Explorer does. Each dock gets a
// hidden window of its own (never shown). No input is sent, no real app bar is registered, no other
// program's window is touched; settings, pins and the log go to the test's own data folder next to the exe.
// One PASS/FAIL line per check (tests/portable_check.h), exit code = number of failures.
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/capture.h"
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <string_view>
#include <mutex>
#include "../src/system.h"
#include "../src/sensor_shared.h"
#include "../src/native_tray.h"
#include "../src/oled.h"
#include "../src/theme.h"
#include "../src/explorer_taskbar.h"
#include <shellscalingapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d2d1effects.h>
#include <cstdio>
#include <thread>
#include <limits>
#include <map>
#include <dwmapi.h>
#include "portable_check.h"
#define private public
#include "../src/apps.h"      // AppModel::presses: an app click that only counts (nothing is started)
#include "../src/sensors.h"
#include "../src/dock.h"
#undef private
#include "../src/idle_wait.h"   // main.cpp's look at the pointer (LookAtPointer, kNoPointer): what the loop gives the docks

Settings g_settings;
HWND g_mainWnd = nullptr;

// ---- the frame trace (FrameTraceTests, at the end): while a script runs, what the dock asked of the fakes below
// (Windows calls, the stubbed modules, dock.log lines) is collected here and written into the frame's line
static std::string* traceEvents = nullptr;
static void TraceEvent(const std::string& what) {
    if (!traceEvents) return;
    if (!traceEvents->empty()) *traceEvents += ';';
    *traceEvents += what;
}
static std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ---- stubs for the modules the dock calls (as in dock_interaction_test.cpp): nothing real happens
namespace nativetray { static int desktopToggles = 0; static bool HuntToggleDesktop() { ++desktopToggles; TraceEvent("ToggleDesktop"); return true; } }
static bool oledOn = false;
namespace oled {
bool Active() { return oledOn; }
bool CoversMonitor(HMONITOR) { return oledOn; }
bool Enable(HMONITOR) { oledOn = true; return true; }
bool Disable() { oledOn = false; return true; }
void Reapply() {}
}
#include "../src/settings_window.h"
static int settingsWindows = 0;
namespace settingswindow { void Show(HMONITOR) { ++settingsWindows; TraceEvent("settingswindow::Show"); } void Close() {} bool IsOpen() { return false; } bool PreTranslate(MSG*) { return false; } }
static int themeToggles = 0; static bool themeLight = false;
namespace theme { bool IsLight() { return themeLight; } bool Toggle() { ++themeToggles; themeLight = !themeLight; TraceEvent("theme::Toggle"); return true; } }
#include "../src/audio_devices.h"
static std::wstring speakersName = L"Speakers";
namespace audio {
std::vector<Device> Outputs() { return { { L"a", speakersName, true } }; }
bool SetDefault(const std::wstring&) { return true; }
std::wstring DefaultOutputName() { return L"Speakers"; }
}
#include "../src/start_menu.h"
static int startClicks = 0;
namespace startmenu { bool InFront() { return false; } void Click(HMONITOR, bool closing) { ++startClicks; TraceEvent(closing ? "startmenu::Click closing" : "startmenu::Click"); } }
#include "dock_fixture_taskbar.h"

// ---- fake monitors: a laptop's screen and a two-monitor desktop's
struct FakeMonitor { HMONITOR handle; RECT rect; bool primary; const char* name; };
static const FakeMonitor kLaptop = { (HMONITOR)(uintptr_t)0x2001, { 0, 0, 2880, 1800 }, true, "laptop 2880x1800" };
static const FakeMonitor kDesk = { (HMONITOR)(uintptr_t)0x2002, { 0, 0, 2560, 1440 }, true, "desktop 2560x1440" };
static const FakeMonitor kPortrait = { (HMONITOR)(uintptr_t)0x2003, { -1440, -1120, 0, 1440 }, false, "portrait left, bottoms aligned" };
static const FakeMonitor kPortraitTop = { (HMONITOR)(uintptr_t)0x2004, { -1440, 0, 0, 2560 }, false, "portrait left, tops aligned" };
static const FakeMonitor* const kMonitors[] = { &kLaptop, &kDesk, &kPortrait, &kPortraitTop };
// the frame trace's fourth screen: a short side of 2160 px is the dock's scale 1.5 (the laptop's 1800 px is 1.25,
// the desktop's 1440 px 1.0 - the dock sizes itself by the screen, not by Windows' scale setting)
static const FakeMonitor kUhd = { (HMONITOR)(uintptr_t)0x2005, { 0, 0, 3840, 2160 }, true, "uhd 3840x2160" };
static BOOL WINAPI HuntMonitorInfo(HMONITOR m, LPMONITORINFO info) {
    for (auto* f : kMonitors)
        if (f->handle == m) { info->rcMonitor = info->rcWork = f->rect; info->dwFlags = f->primary ? MONITORINFOF_PRIMARY : 0; return TRUE; }
    if (m == kUhd.handle) { info->rcMonitor = info->rcWork = kUhd.rect; info->dwFlags = MONITORINFOF_PRIMARY; return TRUE; }
    return ::GetMonitorInfoW(m, info);
}

// ---- the window in front (a game, Start...) on one of the fake monitors
static bool fgOn = false, fgZoomed = false;
static RECT fgRect{};
static HMONITOR fgMonitor = nullptr;
static LONG_PTR fgStyle = WS_POPUP;
static HWND const kFg = (HWND)(uintptr_t)0xF00DF00D;
// its handle: always kFg but in the frame trace, where another window (kFg2) comes to the front in its place
static HWND const kFg2 = (HWND)(uintptr_t)0xF00DF00E;
static HWND fgWindow = kFg;
static HWND WINAPI HuntForeground() { return fgOn ? fgWindow : nullptr; }
static HMONITOR WINAPI HuntMonitorFromWindow(HWND h, DWORD flags) { return h == fgWindow ? fgMonitor : ::MonitorFromWindow(h, flags); }
static BOOL WINAPI HuntWindowRect(HWND h, LPRECT r) { if (h == fgWindow) { *r = fgRect; return TRUE; } return ::GetWindowRect(h, r); }
static BOOL WINAPI HuntIsZoomed(HWND h) { return h == fgWindow ? fgZoomed : ::IsZoomed(h); }
static LONG_PTR WINAPI HuntWindowLong(HWND h, int i) { return h == fgWindow ? fgStyle : ::GetWindowLongPtrW(h, i); }
// its client area as GetWindowInfo reports it (a maximized window counts as fullscreen only when that covers the
// monitor); empty unless a check sets it, and fgInfoFails makes the call fail
static RECT fgClient{};
static bool fgInfoFails = false;
static BOOL WINAPI HuntWindowInfo(HWND h, PWINDOWINFO wi) {
    if (h != fgWindow) return ::GetWindowInfo(h, wi);
    if (fgInfoFails) return FALSE;
    wi->rcWindow = fgRect; wi->rcClient = fgClient; wi->dwStyle = (DWORD)fgStyle;
    return TRUE;
}
static int WINAPI HuntClassName(HWND h, LPWSTR text, int cap) {
    if (h == fgWindow) { wcsncpy_s(text, cap, L"HuntFullscreen", _TRUNCATE); return (int)wcslen(text); }
    return ::GetClassNameW(h, text, cap);
}

// ---- pointer, mouse buttons (physical, as GetAsyncKeyState reports them) and mouse capture
static POINT cursor{};
static bool physicalLeft = false, physicalRight = false, buttonsSwapped = false;
static BOOL WINAPI HuntCursorPos(LPPOINT p) { *p = cursor; return TRUE; }
static SHORT WINAPI HuntAsyncKey(int vk) {
    if (vk == VK_LBUTTON) return physicalLeft ? (SHORT)0x8000 : 0;
    if (vk == VK_RBUTTON) return physicalRight ? (SHORT)0x8000 : 0;
    return 0;
}
static SHORT WINAPI HuntKeyState(int) { return 0; }   // no Shift held
static int WINAPI HuntSystemMetrics(int i) { return i == SM_SWAPBUTTON ? (buttonsSwapped ? 1 : 0) : ::GetSystemMetrics(i); }
static Dock* captureOwner = nullptr;   // the dock whose window has the (fake) capture
static Dock* currentDock = nullptr;    // the dock the next SetCapture belongs to
static HWND WINAPI HuntSetCapture(HWND) { captureOwner = currentDock; TraceEvent("SetCapture"); return nullptr; }
static BOOL WINAPI HuntReleaseCapture() {
    // as Windows does: the window losing the capture gets WM_CAPTURECHANGED (lParam: nobody) before the call returns
    TraceEvent("ReleaseCapture");
    Dock* d = captureOwner; captureOwner = nullptr;
    if (d) d->WndProc(WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}
static int raises = 0;
static BOOL WINAPI HuntSetWindowPos(HWND, HWND after, int, int, int, int, UINT flags) {
    ++raises; TraceEvent(std::string("SetWindowPos ") + (after == HWND_TOPMOST ? "topmost " : "") + std::to_string(flags)); return TRUE;
}
static BOOL WINAPI HuntShowWindow(HWND, int cmd) { TraceEvent("ShowWindow " + std::to_string(cmd)); return TRUE; }   // the test's windows stay hidden
static BOOL WINAPI HuntSetForeground(HWND) { TraceEvent("SetForegroundWindow"); return TRUE; }
static int menus = 0;
static std::vector<std::wstring> menuItems;   // the last menu's items as Windows would read them (& marks an access key)
static BOOL WINAPI HuntPopupMenu(HMENU m, UINT, int, int, int, HWND, const RECT*) {
    ++menus; menuItems.clear(); TraceEvent("TrackPopupMenu");
    for (int i = 0; i < GetMenuItemCount(m); ++i) {
        wchar_t text[256] = {};
        GetMenuStringW(m, i, text, 256, MF_BYPOSITION);
        menuItems.push_back(text);
    }
    return 0;   // nothing chosen
}
static HINSTANCE WINAPI HuntShellExecute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) { TraceEvent("ShellExecute"); return (HINSTANCE)42; }
static int HuntFileOperation(LPSHFILEOPSTRUCTW) { return 0; }
static HRESULT WINAPI HuntEmptyRecycleBin(HWND, LPCWSTR, DWORD) { return S_OK; }

// ---- the shell's app bar list, as Explorer keeps it (lost when Explorer restarts): shell and FakeAppBar, traced here
#include "dock_fixture_appbar.h"
static UINT_PTR WINAPI HuntAppBar(DWORD msg, PAPPBARDATA abd) {
    if (traceEvents) {
        const char* name = msg == ABM_NEW ? "ABM_NEW" : msg == ABM_REMOVE ? "ABM_REMOVE" : msg == ABM_QUERYPOS ? "ABM_QUERYPOS" : msg == ABM_SETPOS ? "ABM_SETPOS" : "ABM_other";
        TraceEvent(std::string(name) + " " + std::to_string(abd->uEdge) + " " + std::to_string(abd->rc.left) + "," + std::to_string(abd->rc.top) + "," +
                   std::to_string(abd->rc.right) + "," + std::to_string(abd->rc.bottom));
    }
    return FakeAppBar(msg, abd);
}
static HWND HuntExplorerTaskbar() { return shell.running ? shell.taskbar : nullptr; }
static void RestartExplorer() {   // the new Explorer has a new taskbar and no app bars
    shell.bars.clear(); shell.strips.clear();
    shell.taskbar = (HWND)((uintptr_t)shell.taskbar + 1);
}

// ---- dock.log: the dock's lines are kept here too (and still written to the test's own log)
static std::vector<std::wstring> logLines;
static void HuntLog(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    logLines.push_back(buf);
    TraceEvent("log:" + Utf8(buf));
    Log(L"%ls", buf);
}
static int LogCount(const wchar_t* part) {
    int n = 0;
    for (auto& l : logLines) if (l.find(part) != std::wstring::npos) ++n;
    return n;
}

// ---- for the frame trace (FrameTraceTests); with the trace off each passes the call on as it was
// the window region and the DWM cloak of the dock's (hidden, the test's own) window: passed on, and traced
static int WINAPI HuntSetWindowRgn(HWND h, HRGN rg, BOOL redraw) {
    if (traceEvents) {
        RECT box{}; if (rg) GetRgnBox(rg, &box);
        TraceEvent("SetWindowRgn " + std::to_string(box.left) + "," + std::to_string(box.top) + "," + std::to_string(box.right) + "," + std::to_string(box.bottom));
    }
    return ::SetWindowRgn(h, rg, redraw);
}
static HRESULT WINAPI HuntDwmSetWindowAttribute(HWND h, DWORD attr, LPCVOID value, DWORD size) {
    if (attr == DWMWA_CLOAK && value && size == sizeof(BOOL)) TraceEvent("DWMWA_CLOAK " + std::to_string(*(const BOOL*)value));
    return ::DwmSetWindowAttribute(h, attr, value, size);
}
// the z-order over a dock (KeepInFront): with traceAbove the window in front (fgWindow) lies right over traceDockWindow
static bool traceAbove = false;
static HWND traceDockWindow = nullptr;
static HWND WINAPI HuntGetWindow(HWND h, UINT cmd) {
    if (traceAbove && cmd == GW_HWNDPREV && h && h == traceDockWindow) return fgWindow;
    if (traceAbove && cmd == GW_HWNDPREV && h == fgWindow) return nullptr;
    return ::GetWindow(h, cmd);
}
// the date the clock's tooltip shows (the PC's own date and locale otherwise): a fixed one while traceDate is set
static bool traceDate = false;
static const wchar_t* const kTraceDate = L"2026년 10월 28일 수요일";
static void WINAPI HuntLocalTime(LPSYSTEMTIME st) {
    if (!traceDate) { ::GetLocalTime(st); return; }
    *st = {}; st->wYear = 2026; st->wMonth = 10; st->wDay = 28; st->wDayOfWeek = 3; st->wHour = 21; st->wMinute = 30;
}
static int WINAPI HuntDateFormatEx(LPCWSTR locale, DWORD flags, const SYSTEMTIME* date, LPCWSTR format, LPWSTR out, int cap, LPCWSTR calendar) {
    if (!traceDate) return ::GetDateFormatEx(locale, flags, date, format, out, cap, calendar);
    TraceEvent("GetDateFormatEx");
    if (!out || cap <= 0) return (int)wcslen(kTraceDate) + 1;
    wcsncpy_s(out, cap, kTraceDate, _TRUNCATE);
    return (int)wcslen(out) + 1;
}
// what Update hands the modules linked beside it, for the frame trace (filled while traceEvents is set, read and
// cleared frame by frame): FluentFlyout's flyout placement (the DockView), the window preview (the hovered app,
// its anchor and whether it may show), the tray host's widget window (Place) and the tray icons' mouse (Send)
static std::string traceFly, tracePreview, tracePlace;
static std::string TraceRect(const RECT& r) {
    return std::to_string(r.left) + "," + std::to_string(r.top) + "," + std::to_string(r.right) + "," + std::to_string(r.bottom);
}
namespace fluentflyout {
// the view as the dock gives it, traced; then the real one (no flyouts here: it does nothing and says false)
static bool HuntPlaceFlyouts(const DockView& v, double now) {
    if (traceEvents) {
        char sc[32]; sprintf_s(sc, "%.4f", v.scale);
        traceFly = std::string(v.shown ? "shown" : "away") + " bar " + TraceRect(v.bar) + (v.slot ? " slot " : " noslot ") + TraceRect(v.slotRect) +
                   " edge" + std::to_string((int)v.edge) + " s" + sc;
    }
    return PlaceFlyouts(v, now);
}
// the pointer on one of its flyouts next to this dock: as the script says (FluentFlyout has none here otherwise)
static bool traceInFlyout = false;
static bool HuntFlyoutAt(HMONITOR monitor, POINT p) { return traceInFlyout || FlyoutAt(monitor, p); }
// its widget's slot: while traceWidget names a monitor, FluentFlyout shows traceWidget there (else the real answer:
// FluentFlyout is not running here, no slot)
static Widget traceWidget;
static Widget HuntWidgetState() { return traceWidget.monitor ? traceWidget : WidgetState(); }
}
namespace trayhost {
static int sends[7] = {};   // by Mouse
static const char* const kMouseNames[] = { "LeftClick", "LeftDoubleClick", "RightClick", "MiddleClick", "Enter", "Move", "Leave" };
// a tray icon's program told of the mouse: counted and traced, not passed on (no such icon here: the real one does nothing)
static void HuntSend(uint64_t key, Mouse m, POINT cursor, const RECT& anchor) {
    ++sends[(int)m];
    char k[24]; sprintf_s(k, "%llx", (unsigned long long)key);
    TraceEvent(std::string("trayhost::Send ") + k + " " + kMouseNames[(int)m] + " " + std::to_string(cursor.x) + "," + std::to_string(cursor.y) + " " + TraceRect(anchor));
}
// the widget's host window placed: traced, not passed on (no tray host runs here)
static int placeShown = 0, placeHidden = 0;
static void HuntPlace(bool shown, const RECT& rect, bool raise, const RECT& clip, HWND over) {
    ++(shown ? placeShown : placeHidden);
    if (!traceEvents) return;
    if (!tracePlace.empty()) tracePlace += ';';
    tracePlace += std::string(shown ? "show " : "hide ") + TraceRect(rect) + (raise ? " raise" : "") + " clip " + TraceRect(clip) + (over ? " over" : "");
}
}
// the window preview (window_preview.cpp, linked): every call goes to the real one; Update's arguments are traced,
// and the script can say the pointer is on the preview (Contains) or that it shows (Open) - it never opens here (the
// apps have no windows)
static bool traceInPreview = false, tracePreviewOpen = false;
struct PreviewTap {
    WindowPreview& w;
    void Update(HWND owner, const std::shared_ptr<AppItem>& hovered, RECT anchor, RECT monitor, Edge edge, float scale, POINT cursor, double now, bool allowed) {
        if (traceEvents) {
            char sc[32]; sprintf_s(sc, "%.4f", scale);
            tracePreview = (hovered ? "app" + std::to_string(hovered->id) : std::string("-")) + " anchor " + TraceRect(anchor) + (allowed ? " allowed" : " held") +
                           " mon " + TraceRect(monitor) + " edge" + std::to_string((int)edge) + " s" + sc + (owner ? "" : " noowner");
        }
        w.Update(owner, hovered, anchor, monitor, edge, scale, cursor, now, allowed);
    }
    void Dismiss() { TraceEvent("preview::Dismiss"); w.Dismiss(); }
    bool Open() const { return tracePreviewOpen || w.Open(); }
    bool Contains(POINT p) const { return traceInPreview || w.Contains(p); }
};
static PreviewTap TapPreview(WindowPreview& w) { return { w }; }

#define GetMonitorInfoW HuntMonitorInfo
#define GetForegroundWindow HuntForeground
#define MonitorFromWindow HuntMonitorFromWindow
#define GetWindowRect HuntWindowRect
#define IsZoomed HuntIsZoomed
#define GetWindowLongPtrW HuntWindowLong
#define GetWindowInfo HuntWindowInfo
#define GetClassNameW HuntClassName
#define GetCursorPos HuntCursorPos
#define GetAsyncKeyState HuntAsyncKey
#define GetKeyState HuntKeyState
#define GetSystemMetrics HuntSystemMetrics
#define SetCapture HuntSetCapture
#define ReleaseCapture HuntReleaseCapture
#define SetWindowPos HuntSetWindowPos
#define ShowWindow HuntShowWindow
#define SetForegroundWindow HuntSetForeground
#define TrackPopupMenu HuntPopupMenu
#define ShellExecuteW HuntShellExecute
#define SHFileOperationW HuntFileOperation
#define SHEmptyRecycleBinW HuntEmptyRecycleBin
#define SHAppBarMessage HuntAppBar
#define ToggleDesktop HuntToggleDesktop
#define ExplorerTaskbar HuntExplorerTaskbar
#define Log HuntLog
#define SetWindowRgn HuntSetWindowRgn
#define DwmSetWindowAttribute HuntDwmSetWindowAttribute
#define GetWindow HuntGetWindow
#define GetLocalTime HuntLocalTime
#define GetDateFormatEx HuntDateFormatEx
#define PlaceFlyouts HuntPlaceFlyouts
#define FlyoutAt HuntFlyoutAt
#define WidgetState HuntWidgetState
#define Send HuntSend
#define Place HuntPlace
#define windowPreview TapPreview(windowPreview)
#include "parts_dock.inc"
#undef PlaceFlyouts
#undef FlyoutAt
#undef WidgetState
#undef Send
#undef Place
#undef windowPreview
#undef GetMonitorInfoW
#undef GetForegroundWindow
#undef MonitorFromWindow
#undef GetWindowRect
#undef IsZoomed
#undef GetWindowLongPtrW
#undef GetWindowInfo
#undef GetClassNameW
#undef GetCursorPos
#undef GetAsyncKeyState
#undef GetKeyState
#undef GetSystemMetrics
#undef SetCapture
#undef ReleaseCapture
#undef SetWindowPos
#undef ShowWindow
#undef SetForegroundWindow
#undef TrackPopupMenu
#undef ShellExecuteW
#undef SHFileOperationW
#undef SHEmptyRecycleBinW
#undef SHAppBarMessage
#undef ToggleDesktop
#undef ExplorerTaskbar
#undef Log
#undef SetWindowRgn
#undef DwmSetWindowAttribute
#undef GetWindow
#undef GetLocalTime
#undef GetDateFormatEx

// ---------------------------------------------------------------- fixture
static double simTime = 10;

// A hidden window of the test's own that stands in for the dock's window (the dock destroys it).
static HWND NewDockWindow() {
    static const bool registered = [] {
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"dock_layout_test-dock";
        return RegisterClassW(&wc) != 0;
    }();
    if (!registered) return nullptr;
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"dock_layout_test-dock", L"", WS_POPUP, 0, 0, 8, 8,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

static void Advance(Dock& d, POINT p, double seconds, float dt = 1.f / 120.f) {
    cursor = p;
    for (int i = 0; i < (int)std::ceil(seconds / dt); ++i) { simTime += dt; d.Update(simTime, dt, p); }
}
static POINT Outside(const RECT& r) { return { r.left + (r.right - r.left) / 2, r.top - 3000 }; }
static POINT EdgePoint(const RECT& r, Edge edge, float fraction) {
    LONG x = r.left + (LONG)((r.right - r.left - 1) * fraction), y = r.top + (LONG)((r.bottom - r.top - 1) * fraction);
    switch (edge) {
    case Edge::Bottom: return { x, r.bottom - 1 };
    case Edge::Top: return { x, r.top };
    case Edge::Left: return { r.left, y };
    default: return { r.right - 1, y };
    }
}
static void Apps(int count) {
    g_apps.items.clear(); g_apps.attentionEvents.clear(); ++g_apps.structureVersion;
    for (int i = 0; i < count; ++i) {
        auto a = std::make_shared<AppItem>(); a->id = i + 1; a->name = L"Hunt App " + std::to_wstring(i + 1); a->pinned = true;
        g_apps.items.push_back(a);
    }
}
static void Quiet(Dock& d) {
    fgOn = fgZoomed = false; fgStyle = WS_POPUP; fgClient = {}; fgInfoFails = false;
    physicalLeft = physicalRight = buttonsSwapped = false;
    d.reveal.snap(0); d.magAmount.snap(0); d.pinSide = 0; d.pinBlend.snap(0); d.panelAlpha.snap(0); d.tooltipAlpha.snap(0);
    d.edgeSince = d.outsideSince = d.panelOutsideSince = -1; d.menuOpen = d.dragging = false; d.pressSlot = -1; d.pressKind = Dock::HitNone;
    d.nextCaptureTry = std::numeric_limits<double>::max();   // never starts a real screen capture
}
// The dock as BuildDocks makes it, without the GPU window: its own hidden window, laid out and hidden.
static std::unique_ptr<Dock> MakeDock(const FakeMonitor& m, int index, bool withWindow) {
    auto d = std::make_unique<Dock>(m.handle, index, m.primary);
    d->nextCaptureTry = std::numeric_limits<double>::max();
    if (withWindow) d->hwnd = NewDockWindow();
    d->Metrics(); d->SyncSlots();
    for (auto& s : d->slots) { s.appear.snap(1); s.shrink.snap(1); }
    d->trailingGap.snap(0);
    Quiet(*d);
    Advance(*d, Outside(d->mon), 0.5);
    return d;
}
static LPARAM ClientPoint(const Dock& d, POINT screen) { return MAKELPARAM(screen.x - d.winX, screen.y - d.winY); }
static POINT SlotPoint(const Dock& d, int slot) { return d.ScreenPointUV(d.slots[slot].u, d.margin + d.barThick / 2); }

static std::string Label(const char* what, const FakeMonitor& m, int edge) {
    static const char* names[] = { "bottom", "left", "right", "top" };
    return std::string(what) + " (" + m.name + ", " + names[edge] + ")";
}

// ---------------------------------------------------------------- geometry on the test screens
static void GeometryTests() {
    for (auto* m : kMonitors) for (int edge = 0; edge < 4; ++edge) {
        g_settings = Settings{}; g_settings.edge = (Edge)edge; Apps(12);
        auto d = MakeDock(*m, 0, false);
        const RECT& r = m->rect;
        Check(d->winX >= r.left && d->winY >= r.top && d->winX + d->winW <= r.right && d->winY + d->winH <= r.bottom,
              Label("the dock window lies inside its monitor", *m, edge).c_str());
        const float shortSide = (float)std::min(r.right - r.left, r.bottom - r.top);
        Check(std::fabs(d->scale - std::max(0.72f, shortSide / 1440.0f)) < 1e-4f, Label("the dock is sized by the screen's short side", *m, edge).c_str());
        // the outermost pixel row along the edge is "at the edge", one row further in is too, three rows in is not
        bool edgeRows = true, roundTrip = true;
        for (float f : { 0.f, 0.5f, 1.f }) {
            POINT p = EdgePoint(r, (Edge)edge, f);
            float u, v; d->ScreenToUV(p, u, v);
            edgeRows &= v > 0 && v < 2 && u >= 0 && u < d->len;
            POINT in = p;
            switch ((Edge)edge) { case Edge::Bottom: in.y -= 3; break; case Edge::Top: in.y += 3; break; case Edge::Left: in.x += 3; break; default: in.x -= 3; }
            d->ScreenToUV(in, u, v); edgeRows &= v >= 2;
        }
        Check(edgeRows, Label("the screen edge row counts as the edge, three pixels in does not", *m, edge).c_str());
        for (float u : { 10.f, d->len * 0.5f, d->len - 10.f }) for (float v : { d->margin + 1.f, d->margin + d->barThick * 0.5f }) {
            POINT p = d->ScreenPointUV(u, v);
            float u2, v2; d->ScreenToUV(p, u2, v2);
            roundTrip &= std::fabs(u2 - u) <= 1.01f && std::fabs(v2 - v) <= 1.01f && PtInRect(&r, p);
        }
        Check(roundTrip, Label("dock coordinates and screen pixels map back and forth within a pixel", *m, edge).c_str());
        // the edge brings it up, leaving puts it away; the hovered icon is the one under the pointer
        Advance(*d, EdgePoint(r, (Edge)edge, 0.5f), 0.8);
        Check(d->reveal.target == 1 && d->reveal.x > 0.9f, Label("the pointer at the edge brings the dock up", *m, edge).c_str());
        bool hits = true;
        for (int i : { 1, 6, 12 }) {
            POINT p = SlotPoint(*d, i); Advance(*d, p, 0.6); p = SlotPoint(*d, i); Advance(*d, p, 0.3);
            float u, v; d->ScreenToUV(p, u, v); auto h = d->HitTestUV(u, v);
            hits &= h.kind == Dock::HitSlot && h.slot == i && d->hover.slot == i;
        }
        Check(hits, Label("an icon under the pointer is the one hovered and hit", *m, edge).c_str());
        Advance(*d, Outside(r), 1.5);
        Check(d->reveal.x < 0.002f, Label("leaving the monitor puts the dock away", *m, edge).c_str());
    }
}

// ---------------------------------------------------------------- the settings window holds the docks only while seen
// Seen on a test PC, 2026-11-06: the settings window, open but minimized, held the auto-hide dock up with nothing on the
// screen. A real window of the test's own stands in for it: fully transparent, off the screen, never activated.
static void SettingsHoldTests() {
    static const bool registered = [] {
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"dock_layout_test-settings";
        return RegisterClassW(&wc) != 0;
    }();
    HWND w = registered ? CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"dock_layout_test-settings", L"", WS_POPUP | WS_VISIBLE, 30000, 30000, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(w != nullptr, "a stand-in settings window");
    if (!w) return;
    SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA);
    g_settings = Settings{}; Apps(6);
    auto d = MakeDock(kDesk, 0, false);
    const POINT away = Outside(d->mon);
    g_settingsHold.open = true; g_settingsHold.wnd = w;
    Advance(*d, away, 1.5);
    Check(d->reveal.target == 1 && d->reveal.x > 0.9f, "an open settings window on the screen holds the auto-hide dock up");
    ShowWindow(w, SW_SHOWMINNOACTIVE);
    Advance(*d, away, 1.5);
    Check(IsIconic(w) && d->reveal.target == 0 && d->reveal.x < 0.002f, "minimized (Win+D): the dock goes away as usual");
    ShowWindow(w, SW_SHOWNOACTIVATE);
    Advance(*d, away, 1.0);
    Check(!IsIconic(w) && d->reveal.target == 1, "restored: held up again");
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(w, DWMWA_CLOAK, &cloak, sizeof(cloak));
    Advance(*d, away, 1.5);
    Check(d->reveal.target == 0 && d->reveal.x < 0.002f, "cloaked (left on another virtual desktop): the dock goes away");
    cloak = FALSE;
    DwmSetWindowAttribute(w, DWMWA_CLOAK, &cloak, sizeof(cloak));
    ShowWindow(w, SW_HIDE);
    Advance(*d, away, 1.5);
    Check(d->reveal.target == 0 && d->reveal.x < 0.002f, "hidden: the dock goes away");
    g_settingsHold.wnd = nullptr;
    Advance(*d, away, 0.5);
    Check(d->reveal.target == 1, "open with no window known yet (as before): still holds the dock up");
    g_settingsHold.open = false;
    Advance(*d, away, 1.5);
    Check(d->reveal.x < 0.002f, "closed: the dock goes away");
    DestroyWindow(w);
}

// ---------------------------------------------------------------- a two-monitor desktop's two docks and a game
static void TwoMonitorTests() {
    g_settings = Settings{}; g_settings.secondaryAlwaysShown = true; Apps(10);
    auto primary = MakeDock(kDesk, 0, false);
    auto side = MakeDock(kPortrait, 1, false);
    auto both = [&](POINT p, double seconds) {
        cursor = p;
        for (int i = 0; i < (int)std::ceil(seconds * 120); ++i) { simTime += 1.0 / 120; primary->Update(simTime, 1.f / 120, p); side->Update(simTime, 1.f / 120, p); }
    };
    both(Outside(kDesk.rect), 1.0);
    Check(side->reveal.target == 1 && primary->reveal.target == 0, "secondary always shown: the portrait dock is up, the primary one hidden");
    Check(!side->Busy(), "the portrait dock rests (no vsync-paced frames)");
    // a borderless game fills the primary
    fgOn = true; fgMonitor = kDesk.handle; fgRect = kDesk.rect; fgStyle = WS_POPUP;
    both(EdgePoint(kDesk.rect, Edge::Bottom, 0.5f), 1.0);
    Check(primary->FullscreenHere() && !side->FullscreenHere(), "the game is seen on the primary only");
    Check(primary->reveal.target == 0 && simTime - primary->lastCaptureWanted > 0.9, "the pointer at the game's bottom edge: no dock and no capture there");
    Check(side->reveal.target == 1 && !side->Busy() && simTime - side->lastCaptureWanted > 0.9, "the portrait dock stays up with its glass paused");
    // the game moves to the portrait monitor (negative coordinates)
    fgMonitor = kPortrait.handle; fgRect = kPortrait.rect;
    both(Outside(kDesk.rect), 1.0);
    Check(side->reveal.target == 0 && side->FullscreenHere(), "a game on the portrait monitor puts its dock away");
    both(EdgePoint(kDesk.rect, Edge::Bottom, 0.5f), 0.8);
    Check(primary->reveal.target == 1, "...and the primary dock still comes up at its edge");
    // a game window one pixel short of the portrait monitor is a window, not fullscreen
    fgRect = kPortrait.rect; fgRect.bottom -= 1;
    both(Outside(kDesk.rect), 1.0);
    Check(side->reveal.target == 1, "a borderless window short of the screen leaves the always-shown dock up");

    // A borderless game (or video wall) spanned across both monitors fills each of them. Windows names one monitor
    // for it (MonitorFromWindow: the larger share - here both shares are 2560x1440 = 1440x2560 px); the dock of the
    // other monitor must go away too, not sit frozen on top of the game.
    fgRect = { kPortrait.rect.left, kPortrait.rect.top, kDesk.rect.right, kDesk.rect.bottom }; fgMonitor = kDesk.handle;
    both(Outside(kDesk.rect), 1.0);
    Check(side->reveal.target == 0 && side->FullscreenHere(), "a game spanned across both monitors puts the portrait dock away too");
    both(EdgePoint(kDesk.rect, Edge::Bottom, 0.5f), 0.8);
    Check(primary->reveal.target == 0 && primary->FullscreenHere(), "...and the primary dock stays away at its edge");
    both(EdgePoint(kPortrait.rect, Edge::Bottom, 0.5f), 0.8);
    Check(side->reveal.target == 0 && simTime - side->lastCaptureWanted > 0.7, "...and the portrait dock stays away at its edge, capturing nothing of the game");
    // ...but a window that covers the primary and only part of the portrait monitor fills the primary alone
    fgRect = { kPortrait.rect.left + 720, kDesk.rect.top, kDesk.rect.right, kDesk.rect.bottom };
    both(Outside(kDesk.rect), 1.0);
    Check(primary->FullscreenHere() && !side->FullscreenHere() && side->reveal.target == 1,
          "a borderless window over the primary and half the portrait monitor: only the primary counts as fullscreen");
    fgOn = false;
    both(Outside(kDesk.rect), 0.5);
}

// A resting always-shown dock redraws for a sensor sample (every 2 s) only when its cell would look different
// (g_sensors.cellVersion), every sample while its panel shows, and not while nobody sees the screen (then once
// when it is seen again). The production SensorClient::Tick takes the samples (CPU temperature off: no helper).
static void SensorRedrawTests() {
    g_settings = Settings{}; g_settings.secondaryAlwaysShown = true; Apps(10);
    auto primary = MakeDock(kDesk, 0, false);
    auto side = MakeDock(kPortrait, 1, false);
    const POINT away = Outside(kDesk.rect);
    auto both = [&](double seconds) {
        cursor = away;
        for (int i = 0; i < (int)std::ceil(seconds * 120); ++i) { simTime += 1.0 / 120; primary->Update(simTime, 1.f / 120, away); side->Update(simTime, 1.f / 120, away); }
    };
    auto frame = [&] {   // one pass of the main loop; whether the portrait dock would draw
        primary->dirty = side->dirty = false;
        simTime += 1.0 / 120; primary->Update(simTime, 1.f / 120, away); side->Update(simTime, 1.f / 120, away);
        return side->dirty;
    };
    auto sample = [&](float cpuTemp, float cpuLoad) {   // the sensor thread's sample, taken on the main loop's Tick
        LDSensorBlock b{};
        // HWiNFO's CPU temperature, flagged as the sampler flags it (hw_panel.h): the source HWiNFO takes no other
        b.flags = LD_HAS_CPU_TEMP | LD_CPU_TEMP_HWINFO | LD_HAS_GPU; b.cpuTemp = cpuTemp; b.cpuLoad = cpuLoad; b.gpuTemp = 40;
        { std::lock_guard<std::mutex> lk(g_sensors.mu); g_sensors.localBlk = b; }
        g_sensors.localNew = true;
        g_sensors.Tick(simTime);
    };
    g_settings.sensorSource = 1;
    both(1.0);
    Check(side->reveal.target == 1 && primary->reveal.target == 0 && !side->Busy(), "sensors: the portrait dock up and resting, the primary one hidden");
    sample(47.2f, 12);
    frame();
    Check(!frame(), "sensors: nothing new - no redraw");
    int v = g_sensors.version, cv = g_sensors.cellVersion;
    sample(47.4f, 30);
    const bool sameDrawn = frame();
    Check(g_sensors.version == v + 1 && g_sensors.cellVersion == cv && side->lastSensorVersion == g_sensors.version && !sameDrawn,
          "sensors: a sample that leaves the cell as it was (47.2 -> 47.4 C, the load 12 -> 30 %) - no redraw of the resting dock");
    sample(47.6f, 30);
    const bool changedDrawn = frame();
    Check(g_sensors.cellVersion == cv + 1 && changedDrawn, "sensors: 47.4 -> 47.6 C (47 deg -> 48 deg): the resting dock redraws");
    Check(!frame(), "...once");

    // its panel up: every sample (the panel shows the loads, the drives, the memory)
    Dock::debugForcePanel = true;
    both(1.0);
    Check(side->panelAlpha.target == 1 && side->panelAlpha.x > 0.99f && primary->panelAlpha.target == 0, "sensors: the portrait dock's panel up");
    Check(!frame(), "sensors: the panel up, nothing new - no redraw");
    v = g_sensors.version; cv = g_sensors.cellVersion;
    sample(47.6f, 30);
    const bool panelDrawn = frame();
    Check(g_sensors.version == v + 1 && g_sensors.cellVersion == cv && panelDrawn && !primary->dirty,
          "sensors: the panel up - the same values redraw it (the primary dock, without a panel, does not redraw)");
    sample(47.6f, 31);
    Check(frame(), "...and the next sample too");
    Dock::debugForcePanel = false;
    both(1.0);
    Check(side->panelAlpha.target == 0 && side->panelAlpha.x < 0.01f, "sensors: the panel closed");
    sample(47.6f, 32);
    Check(!frame(), "sensors: the panel closed - the same cell, no redraw again");

    // nobody sees the screen (locked, the display off): the cell's change waits, one redraw when it is seen. The
    // loop's look at the pointer (main.cpp: idle_wait.h LookAtPointer) sets Dock::screenUnseen: the display off here
    idlewait::PointerLook look; look.Start(simTime);
    idlewait::LookAtPointer(look, true, away.x, away.y, simTime);   // the pointer read where it rests
    look.displayOff = true;   // "off" (MainProc)
    Dock::screenUnseen = idlewait::LookAtPointer(look, true, away.x, away.y, simTime).unseen;
    sample(48.6f, 32);
    const bool unseen1 = frame(), unseen2 = frame();
    sample(49.6f, 32);
    const bool unseen3 = frame();
    Check(!unseen1 && !unseen2 && !unseen3, "sensors: the screen unseen - 48.6 and 49.6 C (49 deg, 50 deg) draw nothing");
    look.displayOff = false;   // "on" (MainProc)
    Dock::screenUnseen = idlewait::LookAtPointer(look, true, away.x, away.y, simTime).unseen;
    const bool seen1 = frame(), seen2 = frame();
    Check(seen1 && !seen2, "sensors: seen again - one redraw, then none");

    g_sensors.live = false; g_sensors.d = {}; g_sensors.alertPending = false;
    both(0.5);
}

// The sensor settings (the 센서 page: sensorSource and the six items; sensor_logic.h ShowItems) on the production
// Dock: the panel's rows and heights for the approved pictures (PanelAll / A / B / C), the header's VRAM part, the
// window's room - a horizontal dock's thickness T shrinks with what is hidden, a vertical one's stays - and the
// cell's key with the GPU or the CPU temperature hidden.
static void SensorSettingsTests() {
    // a desktop's panel as in the pictures: two modules (one row), the VRAM block with its temperature,
    // three drives (two rows)
    auto fill = [](int drives, int dimms) {
        g_sensors.live = true;
        g_sensors.d = {};
        g_sensors.d.flags = LD_HAS_CPU_TEMP | LD_CPU_TEMP_HWINFO | LD_HWINFO | LD_HAS_GPU | LD_HAS_GPU_LOAD;
        g_sensors.d.cpuTemp = 47; g_sensors.d.cpuLoad = 8; g_sensors.d.gpuTemp = 41; g_sensors.d.gpuLoad = 3;
        PanelData p;
        for (int i = 0; i < drives; ++i) { sensorlogic::DriveRow r; r.letter = wchar_t(L'C' + i); r.model = L"WD SN850X"; r.temp = 40.f + i; p.drives.push_back(r); }
        for (int i = 0; i < dimms; ++i) { sensorlogic::DimmRow r; r.label = L"RAM " + std::to_wstring(i + 1); r.temp = 36; p.dimms.push_back(r); }
        p.drivesWithTemp = drives; p.modules = dimms; p.dimmsWithTemp = dimms; p.sampled = true;
        p.vram.present = true; p.vram.totalGB = 15.9f; p.vram.usedGB = 2.4f; p.vram.load = 15; p.vram.temp = 52;
        g_sensors.panel = p;
    };
    auto items = [](bool cpuTemp, bool gpu, bool vram, bool vramTemp, bool ramTemp, bool ssd) {
        g_settings.showCpuTemp = cpuTemp; g_settings.showGpu = gpu; g_settings.showVram = vram;
        g_settings.showVramTemp = vramTemp; g_settings.showRamTemp = ramTemp; g_settings.showSsd = ssd;
    };
    g_settings = Settings{}; Apps(8);
    fill(3, 2);
    g_sensors.vramNvmlTempSeen = false;
    {
        auto d = MakeDock(kDesk, 0, false);
        // a room for everything: the rows each picture shows, and its height
        d->panelH = std::round(sensorlogic::PanelHeightDip(4, 6, 2) * d->scale);   // 2: the VRAM block with its temperature row
        d->panelVramRoom = d->panelVramTempRoom = true;
        auto rows = d->PanelRowsNow();
        Check(rows.dimmRows == 1 && rows.vram && rows.vram == 2 && rows.driveRows == 2 && rows.height == 376,
              "sensors: everything shown (a desktop PC): one module row, VRAM with its temperature, two drive rows - 376 DIP");
        items(true, true, false, false, false, true);
        rows = d->PanelRowsNow();
        Check(rows.dimmRows == 0 && !rows.vram && rows.driveRows == 2 && rows.dimmsHidden == 0 && rows.height == 270,
              "sensors: picture A (VRAM, VRAM 온도, RAM 슬롯 온도 off): 270 DIP, the modules not counted as left out");
        items(true, false, false, true, true, true);
        rows = d->PanelRowsNow();
        Check(rows.dimmRows == 1 && !rows.vram && rows.driveRows == 2 && rows.height == 305, "sensors: picture B (GPU, VRAM off): 305 DIP");
        items(false, true, true, true, true, false);
        rows = d->PanelRowsNow();
        Check(rows.dimmRows == 1 && rows.vram && rows.vram == 2 && rows.driveRows == 0 && rows.drivesHidden == 0 && rows.height == 314,
              "sensors: picture C (CPU 온도, SSD off): 314 DIP, no drive row and none counted as left out");
        items(true, true, true, false, true, true);
        rows = d->PanelRowsNow();
        Check(rows.vram && !(rows.vram == 2) && rows.height == 349, "sensors: VRAM 온도 off: the block 44 DIP (349)");
        items(true, true, true, true, true, true);
        // the source: off and PawnIO have no module rows (HWiNFO's only) and the VRAM temperature row only from NVML
        for (int source : { 0, 2 }) {
            g_settings.sensorSource = source;
            rows = d->PanelRowsNow();
            const bool without = rows.dimmRows == 0 && rows.vram && !(rows.vram == 2) && rows.driveRows == 2 && rows.height == 322;
            g_sensors.vramNvmlTempSeen = true;
            rows = d->PanelRowsNow();
            Check(without && rows.vram == 2 && rows.height == 349, source == 0 ? "sensors: temperatures off: no module rows; the VRAM temperature row only with NVML's"
                                                                             : "sensors: PawnIO: no module rows; the VRAM temperature row only with NVML's");
            g_sensors.vramNvmlTempSeen = false;
        }
        g_settings.sensorSource = 1;
        // the header's VRAM part follows the block shown (DrawPanel: rows.vram)
        Check(sensorlogic::PanelHeader(g_settings.showGpu, d->PanelRowsNow().vram, g_settings.showSsd) == L"CPU · GPU · RAM · VRAM · SSD",
              "sensors: the header with everything shown");
        // the cell's key: the GPU hidden - no GPU line; the CPU temperature hidden - its load
        g_settings.showGpu = false;
        auto key = g_sensors.CellKeyNow();
        Check(!key.gpuLine && key.gpu.kind == 0 && key.cpu.kind == 1 && key.cpu.value == 47, "sensors: the GPU hidden: the cell's key has the CPU's line only (47°)");
        g_settings.showGpu = true; g_settings.showCpuTemp = false;
        key = g_sensors.CellKeyNow();
        Check(key.gpuLine && key.gpu.kind == 1 && key.gpu.value == 41 && key.cpu.kind == 2 && key.cpu.value == 8, "sensors: the CPU temperature hidden: the cell shows its load (8%), the GPU its 41°");
        g_settings.showCpuTemp = true; g_settings.sensorSource = 0;
        key = g_sensors.CellKeyNow();
        Check(key.cpu.kind == 2 && key.gpu.kind == 1, "sensors: temperatures off: the CPU's load, the GPU's NVML temperature stays");
        g_settings.sensorSource = 1;
    }

    // the window's room: what is hidden keeps none - a horizontal dock thinner, a vertical one as it was
    for (auto* m : { &kDesk, &kLaptop }) for (Edge edge : { Edge::Bottom, Edge::Left }) {
        g_settings = Settings{}; g_settings.edge = edge; Apps(8);
        fill(3, 2);
        auto d = MakeDock(*m, 0, false);
        const bool vert = IsVertical(edge);
        auto metrics = [&] {
            d->Metrics();
            const auto room = g_sensors.PanelRoomHint();
            const auto rr = sensorlogic::RoomRows(room, g_settings.showSsd, Dock::PanelColumns(vert), Dock::kPanelMaxDimmRows, Dock::kPanelMaxDriveRows);
            return d->panelH == std::round(sensorlogic::PanelHeightDip(rr.dimmRows, rr.driveRows, rr.vram) * d->scale) && d->panelVramRoom == room.vram &&
                   d->panelVramTempRoom == (room.vram && room.vramTemp);
        };
        bool consistent = metrics();
        const auto all = g_sensors.PanelRoomHint();
        const float tAll = d->T, hAll = d->panelH;
        consistent &= all.drives >= 3 && all.dimms >= 2 && all.vram && all.vramTemp;
        struct Step { const char* what; bool cpuTemp, gpu, vram, vramTemp, ramTemp, ssd; int source; };
        const Step steps[] = {
            { "SSD off", true, true, true, true, true, false, 1 },
            { "VRAM 온도 off", true, true, true, false, true, true, 1 },
            { "VRAM off", true, true, false, true, true, true, 1 },
            { "RAM 슬롯 온도 off", true, true, true, true, false, true, 1 },
            { "temperatures off (no module rows; the VRAM temperature row kept only on an NVIDIA adapter)", true, true, true, true, true, true, 0 },
            { "PawnIO (no module rows; the VRAM temperature row kept only on an NVIDIA adapter)", true, true, true, true, true, true, 2 },
        };
        bool thinner = true, same = true;
        for (const Step& st : steps) {
            items(st.cpuTemp, st.gpu, st.vram, st.vramTemp, st.ramTemp, st.ssd);
            g_settings.sensorSource = st.source;
            consistent &= metrics();
            if (!vert) thinner &= d->T < tAll && d->panelH < hAll && std::fabs((tAll - d->T) - (hAll - d->panelH)) <= 1;
            else same &= d->T == tAll;
            if (!vert && !(d->T < tAll)) std::printf("  %s: T %.0f -> %.0f\n", st.what, tAll, d->T);
        }
        // GPU and the CPU temperature hidden: the cards' row stays (the CPU card the whole width) - T as with all shown
        items(false, false, true, true, true, true); g_settings.sensorSource = 1;
        consistent &= metrics();
        const bool cardsKept = d->T == tAll;
        // shown again: the room as before (what was counted is not forgotten)
        items(true, true, true, true, true, true);
        consistent &= metrics();
        const auto back = g_sensors.PanelRoomHint();
        Check(consistent, Label("sensors: the window's panel room follows the hint with the settings applied", *m, (int)edge).c_str());
        if (!vert) Check(thinner, Label("sensors: each item hidden makes a horizontal dock's window thinner by the panel's height it took", *m, (int)edge).c_str());
        else Check(same, Label("sensors: a vertical dock's window keeps its thickness (the panel's width sets it)", *m, (int)edge).c_str());
        Check(cardsKept, Label("sensors: the GPU and the CPU temperature hidden: the cards' row stays, T as before", *m, (int)edge).c_str());
        Check(back == all && d->T == tAll, Label("sensors: shown again: the room and T as before", *m, (int)edge).c_str());
        {
            items(true, true, true, true, true, false);
            const auto noSsd = g_sensors.PanelRoomHint();
            items(true, true, true, true, true, true);
            Check(noSsd.drives == 0 && noSsd.dimms == all.dimms && noSsd.vram, Label("sensors: SSD hidden: no drive kept room for", *m, (int)edge).c_str());
        }
    }
    g_sensors.live = false; g_sensors.d = {}; g_sensors.panel = PanelData{}; g_sensors.vramNvmlTempSeen = false;
    g_settings = Settings{};
}

// ---------------------------------------------------------------- a maximized browser going fullscreen (mod.24)
// Chrome maximized, then F11 or a page's fullscreen video: Windows keeps the window maximized, Chrome takes its
// caption and resize frame away and puts the window and its client area exactly on the monitor (probe on a 150 % laptop screen:
// caption 0 maximised 1 rect 0,0-2880,1800). Before mod.24 every maximized window was a window, and the dock came
// up over the video. Maximized windows that keep their frame hang over the monitor by it (Chrome -11 at 150 %,
// -8 at 100 %); a WS_POPUP, a window with a resize frame, or one whose client area or rectangle is a pixel or two
// off stays a window the dock comes up over.
static const LONG_PTR kChromeF11 = WS_OVERLAPPED | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE | WS_MAXIMIZE;
static const LONG_PTR kChromeMaximized = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_MAXIMIZE;
static RECT Grown(const RECT& r, int by) { return { r.left - by, r.top - by, r.right + by, r.bottom + by }; }
// the window in front on m: its style, window rectangle and client area (GetWindowInfo); maximized unless told
static void Front(const FakeMonitor& m, LONG_PTR style, const RECT& window, const RECT& client, bool zoomed = true) {
    fgOn = true; fgMonitor = m.handle; fgZoomed = zoomed; fgStyle = style; fgRect = window; fgClient = client; fgInfoFails = false;
}

static void ZoomedFullscreenTests() {
    for (auto* m : kMonitors) {
        const RECT& r = m->rect;
        const int frame = m == &kLaptop ? 11 : 8;   // how far a maximized window's resize frame hangs over (150 % / 100 %)
        g_settings = Settings{}; Apps(8);
        auto d = MakeDock(*m, 0, false);
        // an auto-hide dock, hidden, then the pointer at its edge for 0.8 s: did it come up, was a capture pre-warmed?
        struct Seen { bool up, here, captured; };
        auto atEdge = [&]() {
            d->reveal.snap(0); d->edgeSince = d->outsideSince = -1;
            Advance(*d, Outside(r), 0.5);
            Advance(*d, EdgePoint(r, Edge::Bottom, 0.5f), 0.8);
            Seen s{ d->reveal.target == 1, d->FullscreenHere(), simTime - d->lastCaptureWanted < 0.7 };
            Advance(*d, Outside(r), 1.5);
            return s;
        };
        Front(*m, kChromeF11, r, r);
        Seen s = atEdge();
        Check(!s.up && s.here, Label("Chrome maximized, then F11: the pointer at the edge brings no dock", *m, 0).c_str());
        Check(!s.captured, Label("...and pre-warms no capture of the video", *m, 0).c_str());
        // Esc: maximized again with its caption and frame
        Front(*m, kChromeMaximized, Grown(r, frame), r);
        s = atEdge();
        Check(s.up && !s.here && s.captured, Label("leaving F11 (maximized again, over the monitor by its frame): the edge brings the dock up", *m, 0).c_str());
        Front(*m, (WS_OVERLAPPEDWINDOW & ~WS_CAPTION) | WS_VISIBLE | WS_MAXIMIZE, Grown(r, frame), r);
        s = atEdge();
        Check(s.up && !s.here, Label("a caption-less maximized window over the monitor by its frame is a window", *m, 0).c_str());
        Front(*m, kChromeF11 | WS_THICKFRAME, r, r);
        s = atEdge();
        Check(s.up && !s.here, Label("a caption-less maximized window with a resize frame, exactly the monitor, is a window", *m, 0).c_str());
        Front(*m, WS_POPUP | WS_VISIBLE | WS_MAXIMIZE, r, r);
        s = atEdge();
        Check(s.up && !s.here, Label("a maximized WS_POPUP exactly on the monitor (Qt, WPF frameless) is a window", *m, 0).c_str());
        Front(*m, kChromeF11, r, { r.left, r.top, r.right, r.bottom - 2 });
        s = atEdge();
        Check(s.up && !s.here, Label("caption-less, no frame, but the client area 2 px short of the monitor: a window", *m, 0).c_str());
        {
            RECT shortBy1 = r; shortBy1.bottom -= 1;
            Front(*m, kChromeF11, shortBy1, shortBy1);
            s = atEdge();
            Check(s.up && !s.here, Label("caption-less, no frame, the window 1 px short of the monitor: a window", *m, 0).c_str());
        }
        Front(*m, kChromeF11, Grown(r, 1), r);
        s = atEdge();
        Check(s.up && !s.here, Label("caption-less, no frame, the window 1 px over the monitor: a window", *m, 0).c_str());
        Front(*m, kChromeF11, r, r); fgInfoFails = true;
        s = atEdge();
        Check(s.up && !s.here, Label("GetWindowInfo failing on the maximized window: a window", *m, 0).c_str());
        // not maximized (F11 over a restored window): the same as before mod.24
        Front(*m, kChromeF11 & ~WS_MAXIMIZE, r, r, false);
        s = atEdge();
        Check(!s.up && s.here, Label("a caption-less window exactly on the monitor, not maximized, is still fullscreen", *m, 0).c_str());

        // an always-shown dock: F11 puts it away, leaving F11 brings it back
        Quiet(*d);
        g_settings.autoHide = false;
        Front(*m, kChromeMaximized, Grown(r, frame), r);
        Advance(*d, Outside(r), 1.0);
        Check(d->reveal.target == 1, Label("auto-hide off: the dock stays up over the maximized browser", *m, 0).c_str());
        Front(*m, kChromeF11, r, r);
        Advance(*d, Outside(r), 1.0);
        Check(d->reveal.target == 0 && d->FullscreenHere(), Label("auto-hide off: F11 in the maximized browser puts the dock away", *m, 0).c_str());
        Front(*m, kChromeMaximized, Grown(r, frame), r);
        Advance(*d, Outside(r), 1.0);
        Check(d->reveal.target == 1 && !d->FullscreenHere(), Label("auto-hide off: leaving F11 brings the dock back", *m, 0).c_str());
        Quiet(*d);
        Advance(*d, Outside(r), 0.2);
    }

    // a desktop's two monitors, both docks always shown: F11 on one puts only its dock away, the other one
    // rests with its glass paused (no frames, no capture) until the video's fullscreen ends
    g_settings = Settings{}; g_settings.autoHide = false; Apps(10);
    auto primary = MakeDock(kDesk, 0, false);
    auto side = MakeDock(kPortrait, 1, false);
    auto both = [&](POINT p, double seconds) {
        cursor = p;
        for (int i = 0; i < (int)std::ceil(seconds * 120); ++i) { simTime += 1.0 / 120; primary->Update(simTime, 1.f / 120, p); side->Update(simTime, 1.f / 120, p); }
    };
    const POINT away = Outside(kDesk.rect);
    Front(kPortrait, kChromeMaximized, Grown(kPortrait.rect, 8), kPortrait.rect);
    both(away, 1.0);
    Check(primary->reveal.target == 1 && side->reveal.target == 1 && !side->FullscreenHere(),
          "auto-hide off, Chrome maximized on the portrait monitor: both docks up");
    Front(kPortrait, kChromeF11, kPortrait.rect, kPortrait.rect);
    both(away, 1.0);
    Check(side->reveal.target == 0 && side->FullscreenHere() && !primary->FullscreenHere(),
          "F11 in the maximized browser on the portrait monitor (negative coordinates) puts only that dock away");
    Check(primary->reveal.target == 1 && !primary->Busy() && simTime - primary->lastCaptureWanted > 0.9,
          "...and the primary dock stays up with its glass paused");
    Front(kPortrait, kChromeMaximized, Grown(kPortrait.rect, 8), kPortrait.rect);
    both(away, 1.0);
    Check(side->reveal.target == 1 && !side->FullscreenHere(), "leaving F11 brings the portrait dock back");
    Check(simTime - primary->lastCaptureWanted < 0.05, "...and the primary dock's glass follows the screen again");
    // the other way round: a page's fullscreen video in the browser maximized on the primary
    Front(kDesk, kChromeF11, kDesk.rect, kDesk.rect);
    both(away, 1.0);
    Check(primary->reveal.target == 0 && primary->FullscreenHere() && !side->FullscreenHere(),
          "a fullscreen video in the browser maximized on the primary puts only the primary dock away");
    Check(side->reveal.target == 1 && !side->Busy() && simTime - side->lastCaptureWanted > 0.9,
          "...and the portrait dock stays up with its glass paused");
    Front(kDesk, kChromeMaximized, Grown(kDesk.rect, 8), kDesk.rect);
    both(away, 1.0);
    Check(primary->reveal.target == 1 && side->reveal.target == 1 && simTime - side->lastCaptureWanted < 0.05,
          "the video's fullscreen over: both docks up, the glass live again");
    Quiet(*primary); Quiet(*side);
    both(away, 0.5);
}

// ---------------------------------------------------------------- app bars (always-shown docks)
// (TaskbarCreatedMessage: dock_window.cpp's)
static void AppBarTests() {
    for (auto* m : { &kLaptop, &kPortrait }) for (int edge = 0; edge < 4; ++edge) {
        shell.Reset();
        g_settings = Settings{}; g_settings.edge = (Edge)edge; g_settings.autoHide = false; Apps(8);
        logLines.clear();
        auto d = MakeDock(*m, 0, true);
        if (!d->hwnd) { Check(false, "the test's dock window was created"); return; }
        Advance(*d, Outside(d->mon), 1.0);
        Check(shell.Has(d->hwnd) && shell.news == 1, Label("auto-hide off: the dock registers one app bar", *m, edge).c_str());
        Check(LogCount(L"appbar:") == 0, Label("...and a registration that works at once writes no dock.log line", *m, edge).c_str());
        Check(shell.setPos == 1, Label("a resting dock claims its strip once, not every frame", *m, edge).c_str());
        const RECT p = shell.lastPos, r = m->rect;
        const int band = IsVertical((Edge)edge) ? p.right - p.left : p.bottom - p.top;
        bool onEdge = false;
        switch ((Edge)edge) {
        case Edge::Bottom: onEdge = p.bottom == r.bottom && p.left == r.left && p.right == r.right; break;
        case Edge::Top: onEdge = p.top == r.top && p.left == r.left && p.right == r.right; break;
        case Edge::Left: onEdge = p.left == r.left && p.top == r.top && p.bottom == r.bottom; break;
        case Edge::Right: onEdge = p.right == r.right && p.top == r.top && p.bottom == r.bottom; break;
        }
        Check(onEdge && band % 4 == 0 && band >= d->margin + d->barThick && band < d->margin + d->barThick + 4,
              Label("the strip runs along the dock's edge of its monitor, as deep as the glass in 4 px steps", *m, edge).c_str());
        // another app bar moved: claim the strip again, once
        d->WndProc(WM_APP_APPBAR, ABN_POSCHANGED, 0);
        Advance(*d, Outside(d->mon), 0.5);
        Check(shell.setPos == 2, Label("another app bar moved: the strip is claimed again once", *m, edge).c_str());
        {   // a thicker glass (the settings slider lays the dock out in place): the strip follows it
            g_settings.dockThickness = 150;
            const bool inPlace = d->Relayout();
            Advance(*d, Outside(d->mon), 1.0);
            const RECT q = shell.lastPos;
            const int thick = IsVertical((Edge)edge) ? q.right - q.left : q.bottom - q.top;
            Check(inPlace && shell.setPos >= 3 && thick % 4 == 0 && thick >= d->margin + d->barThick && thick < d->margin + d->barThick + 4,
                  Label("a thicker glass laid out in place: the strip grows with it", *m, edge).c_str());
            g_settings.dockThickness = 100; d->Relayout(); Advance(*d, Outside(d->mon), 1.0);
        }

        // The tray host (tray_host.cpp) broadcasts TaskbarCreated itself when it starts, so programs re-add their
        // icons: Explorer is the same and still holds the strip - nothing to ask again.
        {
            const int news = shell.news, setPos = shell.setPos;
            d->WndProc(TaskbarCreatedMessage(), 0, 0);
            Advance(*d, Outside(d->mon), 0.5);
            Check(shell.Has(d->hwnd) && shell.news == news && shell.setPos == setPos,
                  Label("the tray host's own TaskbarCreated leaves the app bar as it is", *m, edge).c_str());
        }

        // Explorer restarts: the new Explorer has no app bars and announces itself with TaskbarCreated (broadcast to
        // every top-level window, the dock's included). The always-shown dock must register its strip again.
        const int claimed = shell.setPos;
        RestartExplorer();
        d->WndProc(TaskbarCreatedMessage(), 0, 0);
        Advance(*d, Outside(d->mon), 0.5);
        Check(shell.Has(d->hwnd) && shell.unregisteredCalls == 0 && shell.setPos == claimed + 1,
              Label("after an Explorer restart the always-shown dock registers its app bar again", *m, edge).c_str());
        {   // ...and the tray host's broadcast 2 s later (with that new Explorer) asks nothing again
            const int news = shell.news, setPos = shell.setPos;
            d->WndProc(TaskbarCreatedMessage(), 0, 0);
            Advance(*d, Outside(d->mon), 0.5);
            Check(shell.news == news && shell.setPos == setPos, Label("...and the tray host's broadcast after it changes nothing", *m, edge).c_str());
        }
        {   // a new taskbar window while the shell still holds the strip (never seen, but ABM_NEW would then fail):
            // the dock gives the strip back first, so it is registered again rather than lost
            shell.taskbar = (HWND)((uintptr_t)shell.taskbar + 1);
            const int setPos = shell.setPos, removes = shell.removes;
            d->WndProc(TaskbarCreatedMessage(), 0, 0);
            Advance(*d, Outside(d->mon), 0.5);
            Check(shell.Has(d->hwnd) && d->appBar && shell.removes == removes + 1 && shell.setPos == setPos + 1,
                  Label("a new taskbar while the shell still holds the strip: registered again, not lost", *m, edge).c_str());
        }

        // auto-hide back on: the strip is given back
        g_settings.autoHide = true;
        Advance(*d, Outside(d->mon), 0.2);
        Check(!shell.Has(d->hwnd), Label("auto-hide on again: the app bar is removed", *m, edge).c_str());
        // OLED mode on its monitor hides the always-shown dock and gives the strip back
        g_settings.autoHide = false; Advance(*d, Outside(d->mon), 0.2);
        oledOn = true; Advance(*d, Outside(d->mon), 0.5);
        Check(!d->appBar && d->reveal.target == 0, Label("OLED mode: no app bar and the dock hidden", *m, edge).c_str());
        oledOn = false; Advance(*d, Outside(d->mon), 0.5);
        // the docks are rebuilt (a monitor added or removed): the old dock gives its strip back
        const HWND w = d->hwnd;
        const bool had = shell.Has(w);
        d.reset();
        Check(had && !shell.Has(w), Label("a dock that goes away gives its strip back", *m, edge).c_str());
    }


    // Explorer is not there (crashed, restarting): ABM_NEW fails. The dock must not ask again on every frame
    // (each failure is a dock.log line, and 20000 lines end the log) - and must register once Explorer is back.
    shell.Reset(); shell.running = false;
    g_settings = Settings{}; g_settings.autoHide = false; Apps(8);
    {
        logLines.clear();
        auto d = MakeDock(kLaptop, 0, true);   // its first half second of frames went to a missing Explorer
        shell.news = 0;
        Advance(*d, Outside(d->mon), 1.0);
        Check(shell.news <= 2, "Explorer gone: a failed app bar registration is not retried on every frame");
        shell.running = true;
        Advance(*d, Outside(d->mon), 3.0);
        Check(shell.Has(d->hwnd), "Explorer back: the always-shown dock registers its app bar within a few seconds");
        // docks on PCs that are not tested directly are seen through their dock.log: the end of the failure must be there
        Check(LogCount(L"register failed") == 1 && LogCount(L"registered after all") == 1,
              "...and dock.log has the failure once and the registration that ended it once");
    }
    shell.Reset(); shell.running = false;
    {   // Explorer comes up and announces itself (TaskbarCreated): registered on the next frame, not up to 2 s later
        logLines.clear();
        auto d = MakeDock(kLaptop, 0, true);
        Advance(*d, Outside(d->mon), 0.3);
        // a TaskbarCreated while there is still no Explorer taskbar (the tray host's broadcast): asked again at once,
        // and that failure is the same one, not a second dock.log line
        const int news = shell.news;
        d->WndProc(TaskbarCreatedMessage(), 0, 0);
        Advance(*d, Outside(d->mon), 2.0 / 120);
        Check(shell.news == news + 1 && LogCount(L"register failed") == 1,
              "a TaskbarCreated with no Explorer taskbar yet: asked again at once, the failure logged only once");
        shell.running = true;
        d->WndProc(TaskbarCreatedMessage(), 0, 0);
        Advance(*d, Outside(d->mon), 2.0 / 120);
        Check(shell.Has(d->hwnd) && shell.setPos == 1, "Explorer announcing its taskbar: the waiting dock registers its app bar at once");
        Check(LogCount(L"registered after all") == 1, "...and dock.log tells that the strip is there again");
    }
    shell.Reset();
}

// The work area must leave the strip out (Dock::CheckAppBar). Until mod.37 the dock's own ABM_QUERYPOS/ABM_SETPOS went
// through its tray host, Explorer could not read them and took no strip while answering TRUE: maximised windows on the
// always-shown secondary dock's monitor went under the glass, and turning the setting off and on (or restarting the
// dock) did not help. The cause is gone (tray_host.cpp, ForwardAppBar); the dock now also sees such a strip and asks again.
static void AppBarWorkAreaTests() {
    for (auto* m : { &kLaptop, &kPortrait }) for (int edge = 0; edge < 4; ++edge) {
        shell.Reset();
        g_settings = Settings{}; g_settings.edge = (Edge)edge; g_settings.autoHide = false; Apps(8);
        logLines.clear();
        auto d = MakeDock(*m, 0, true);
        if (!d->hwnd) { Check(false, "the test's dock window was created"); return; }
        Advance(*d, Outside(d->mon), 2.0);
        const RECT work = shell.Work(m->rect);
        Check(shell.setPos == 1 && LogCount(L"appbar:") == 0 && !EqualRect(&work, &m->rect),
              Label("a strip the shell took: looked at once, nothing asked again or logged", *m, edge).c_str());
        {   // the work area changes and still leaves it out (another program's app bar, a taskbar setting)
            d->WndProc(WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
            Advance(*d, Outside(d->mon), 1.0);
            Check(shell.setPos == 1 && shell.removes == 0 && LogCount(L"appbar:") == 0,
                  Label("the work area changed and leaves the strip out: nothing asked", *m, edge).c_str());
        }
        {   // Explorer works the work area out again without the strip (it lost it, no notification): registered again
            shell.strips.erase(d->hwnd);
            d->WndProc(WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
            Advance(*d, Outside(d->mon), 0.9);
            Check(shell.removes == 0, Label("...not looked at before a second has passed (Explorer puts a program's own work area back)", *m, edge).c_str());
            Advance(*d, Outside(d->mon), 0.5);
            Check(shell.strips.count(d->hwnd) && shell.removes == 1 && shell.setPos == 2 && LogCount(L"reaches into the strip") == 1,
                  Label("the work area lost the strip: registered again a second later, one dock.log line", *m, edge).c_str());
            Advance(*d, Outside(d->mon), 2.0);
            Check(LogCount(L"leaves the strip out again") == 1 && d->appBarMisses == 0 && shell.setPos == 2,
                  Label("...and the next look finds it left out again (logged once), nothing more asked", *m, edge).c_str());
        }
        d.reset();
    }

    // The fault itself: the shell says TRUE to ABM_SETPOS and takes no strip, every time. Three more tries (at once,
    // 2 s and 4 s later), then the dock waits for a new Explorer or the strip to be wanted anew - windows moving
    // every few seconds would be worse than a strip not kept.
    shell.Reset(); shell.takesNoStrip = true;
    g_settings = Settings{}; g_settings.autoHide = false; Apps(8);
    {
        logLines.clear();
        auto d = MakeDock(kPortrait, 0, true);
        Advance(*d, Outside(d->mon), 1.6);
        Check(shell.news == 2 && LogCount(L"registering it again (1 of 3)") == 1,
              "a strip the shell did not take: a second later registered again, logged");
        Advance(*d, Outside(d->mon), 15.0);
        Check(shell.news == 4 && LogCount(L"reaches into the strip") == 3 && LogCount(L"the last try") == 1,
              "...three tries in all, the last one said so in dock.log");
        const int news = shell.news;
        Advance(*d, Outside(d->mon), 30.0);
        Check(shell.news == news && LogCount(L"reaches into the strip") == 3, "...then no more tries and no more lines");
        shell.takesNoStrip = false;   // fixed (a new Explorer): it announces itself, and the strip is taken
        RestartExplorer();
        d->WndProc(TaskbarCreatedMessage(), 0, 0);
        Advance(*d, Outside(d->mon), 2.0);
        const RECT work = shell.Work(kPortrait.rect);
        Check(shell.strips.count(d->hwnd) && work.bottom < kPortrait.rect.bottom && LogCount(L"reaches into the strip") == 3,
              "a new Explorer after the tries: the strip is registered and kept, no more lines");
    }
    shell.Reset(); shell.takesNoStrip = true;
    {   // the shell takes the strip when asked again: one line for the miss, one for the end of it
        logLines.clear();
        auto d = MakeDock(kPortrait, 0, true);
        Advance(*d, Outside(d->mon), 0.5);   // claimed, not taken
        shell.takesNoStrip = false;
        Advance(*d, Outside(d->mon), 3.0);
        Check(shell.strips.count(d->hwnd) && shell.news == 2 && LogCount(L"reaches into the strip") == 1 &&
              LogCount(L"leaves the strip out again") == 1 && d->appBarMisses == 0,
              "taken when asked again: one line for the miss and one for its end");
        // the tries start again for a strip that is wanted anew (auto-hide on and off)
        g_settings.autoHide = true; Advance(*d, Outside(d->mon), 0.2);
        g_settings.autoHide = false; Advance(*d, Outside(d->mon), 0.2);
        Check(shell.strips.count(d->hwnd) && d->appBarMisses == 0, "wanted anew: registered and taken");
    }
    shell.Reset();
    {   // the shell lost the registration itself (no TaskbarCreated): QUERYPOS and SETPOS say FALSE when another bar
        // moves - looked at at once and registered again
        logLines.clear();
        auto d = MakeDock(kPortrait, 0, true);
        Advance(*d, Outside(d->mon), 1.5);
        shell.bars.clear(); shell.strips.clear();
        d->WndProc(WM_APP_APPBAR, ABN_POSCHANGED, 0);
        Advance(*d, Outside(d->mon), 0.5);
        Check(shell.Has(d->hwnd) && shell.strips.count(d->hwnd) && LogCount(L"registering it again (1 of 3)") == 1,
              "a registration the shell lost (SETPOS FALSE): registered again at once");
    }
    shell.Reset();
}

// ---------------------------------------------------------------- clicks and drag reorder
static int IndexOf(const std::shared_ptr<AppItem>& a) {
    for (size_t i = 0; i < g_apps.items.size(); ++i) if (g_apps.items[i] == a) return (int)i;
    return -1;
}
static POINT ButtonPoint(const Dock& d, ButtonId b) {
    auto r = d.ButtonRect(b);
    return { d.winX + (LONG)((r.left + r.right) * 0.5f), d.winY + (LONG)((r.top + r.bottom) * 0.5f) };
}
static POINT EndOfApps(const Dock& d) { return d.ScreenPointUV(d.sepU, d.margin + d.barThick / 2); }

static void DragTests() {
    for (auto* m : { &kLaptop, &kPortrait }) for (int edge : { 0, 1 }) {
        g_settings = Settings{}; g_settings.edge = (Edge)edge; Apps(6);
        auto d = MakeDock(*m, 0, true);
        currentDock = d.get();
        Advance(*d, EdgePoint(d->mon, (Edge)edge, 0.5f), 0.8);

        // a press and release on a button clicks it once
        POINT b = ButtonPoint(*d, ButtonId::Theme); Advance(*d, b, 0.5); b = ButtonPoint(*d, ButtonId::Theme);
        int toggles = themeToggles;
        physicalLeft = true; d->WndProc(WM_LBUTTONDOWN, MK_LBUTTON, ClientPoint(*d, b));
        physicalLeft = false; d->WndProc(WM_LBUTTONUP, 0, ClientPoint(*d, b));
        Check(themeToggles == toggles + 1 && captureOwner == nullptr, Label("press and release on a button: one click", *m, edge).c_str());

        // a plain drag: the drop is applied once and is no click
        auto first = g_apps.items.front();
        POINT p = SlotPoint(*d, 1); Advance(*d, p, 0.5); p = SlotPoint(*d, 1);
        physicalLeft = true; d->WndProc(WM_LBUTTONDOWN, MK_LBUTTON, ClientPoint(*d, p));
        Advance(*d, EndOfApps(*d), 0.6);
        Check(d->dragging, Label("moving with the button held drags the icon", *m, edge).c_str());
        {   // the drop place holds still while the pointer does (the opening gap must not push it back and forth)
            const POINT mid = d->ScreenPointUV((d->slots[3].u + d->slots[4].u) * 0.5f, d->margin + d->barThick / 2);
            Advance(*d, mid, 0.4);
            int changes = 0, last = d->dropIndex;
            for (int i = 0; i < 120; ++i) { Advance(*d, mid, 1.0 / 120); if (d->dropIndex != last) { ++changes; last = d->dropIndex; } }
            Check(changes == 0, Label("a pointer held between two icons keeps one drop place", *m, edge).c_str());
        }
        Advance(*d, EndOfApps(*d), 0.6);
        POINT end = EndOfApps(*d); Advance(*d, end, 0.2);
        int version = g_apps.structureVersion, clicks = startClicks; toggles = themeToggles;
        physicalLeft = false; d->WndProc(WM_LBUTTONUP, 0, ClientPoint(*d, end));
        Check(!d->dragging && IndexOf(first) == 5 && g_apps.structureVersion == version + 1 && startClicks == clicks && themeToggles == toggles,
              Label("the drop moves the app once to the end of the pins and clicks nothing", *m, edge).c_str());

        // an app opens a window during the drag: AppModel::Refresh adds it to the dock (structureVersion moves)
        Advance(*d, EdgePoint(d->mon, (Edge)edge, 0.5f), 0.5);
        first = g_apps.items.front();
        p = SlotPoint(*d, 1); Advance(*d, p, 0.5); p = SlotPoint(*d, 1);
        physicalLeft = true; d->WndProc(WM_LBUTTONDOWN, MK_LBUTTON, ClientPoint(*d, p));
        Advance(*d, EndOfApps(*d), 0.6);
        auto newcomer = std::make_shared<AppItem>(); newcomer->id = 99; newcomer->name = L"Hunt Newcomer";
        g_apps.items.push_back(newcomer); ++g_apps.structureVersion;
        Advance(*d, EndOfApps(*d), 0.3);
        end = EndOfApps(*d); Advance(*d, end, 0.6);
        end = EndOfApps(*d); Advance(*d, end, 0.1);
        Check(d->dragging && d->pressSlot >= 0 && d->pressSlot < (int)d->slots.size() && d->slots[d->pressSlot].app == first,
              Label("an app opening during a drag keeps the dragged icon in hand", *m, edge).c_str());
        physicalLeft = false; d->WndProc(WM_LBUTTONUP, 0, ClientPoint(*d, end));
        Check(!d->dragging && IndexOf(first) == 5 && IndexOf(newcomer) == 6,
              Label("...and its drop still moves it (to the end of the pins)", *m, edge).c_str());

        // the dragged app itself leaves the dock during the drag (its last window closed): the drag ends, nothing moves
        Advance(*d, EdgePoint(d->mon, (Edge)edge, 0.5f), 0.5);
        p = SlotPoint(*d, 7); Advance(*d, p, 0.5); p = SlotPoint(*d, 7);   // the newcomer, unpinned, last
        physicalLeft = true; d->WndProc(WM_LBUTTONDOWN, MK_LBUTTON, ClientPoint(*d, p));
        Advance(*d, SlotPoint(*d, 3), 0.4);
        const bool draggedNewcomer = d->dragging && d->pressSlot == 7;
        {
            g_apps.items.erase(g_apps.items.begin() + IndexOf(newcomer)); ++g_apps.structureVersion;
            Advance(*d, SlotPoint(*d, 3), 0.2);
            const bool ended = !d->dragging && d->pressSlot < 0;
            const int version = g_apps.structureVersion;
            physicalLeft = false; d->WndProc(WM_LBUTTONUP, 0, ClientPoint(*d, SlotPoint(*d, 3)));
            Check(draggedNewcomer && ended && g_apps.structureVersion == version && IndexOf(first) == 5 && captureOwner == nullptr,
                  Label("the dragged app leaving the dock ends the drag without moving anything", *m, edge).c_str());
        }

        // a left-handed mouse (buttons swapped in Windows' mouse settings): Windows sends WM_LBUTTONDOWN for the
        // physical right button, and GetAsyncKeyState(VK_LBUTTON) reports the physical left one
        Advance(*d, EdgePoint(d->mon, (Edge)edge, 0.5f), 0.5);
        for (bool swapped : { false, true }) {
            buttonsSwapped = swapped;
            p = SlotPoint(*d, 2); Advance(*d, p, 0.5); p = SlotPoint(*d, 2);
            if (swapped) physicalRight = true; else physicalLeft = true;
            d->WndProc(WM_LBUTTONDOWN, MK_LBUTTON, ClientPoint(*d, p));
            end = EndOfApps(*d); Advance(*d, end, 0.3);
            const bool dragged = d->dragging;
            physicalLeft = physicalRight = false; d->WndProc(WM_LBUTTONUP, 0, ClientPoint(*d, end));
            if (!swapped) Check(dragged, Label("usual buttons: holding the primary button and moving drags", *m, edge).c_str());
            else Check(dragged, Label("swapped buttons: holding the primary button and moving drags too", *m, edge).c_str());
            Advance(*d, EdgePoint(d->mon, (Edge)edge, 0.5f), 0.5);
        }
        buttonsSwapped = false;
        currentDock = nullptr; captureOwner = nullptr;
    }
}

// ---------------------------------------------------------------- clicks while the apps change
// A click on an app only counts here: an app with a press under way (AppModel::presses) takes further clicks as
// counts, so nothing is started, activated or pressed on the taskbar.
static void CountClicks(const std::shared_ptr<AppItem>& a) {
    g_apps.presses.clear();
    AppModel::Press p; p.item = a->id; p.clicks = 0;
    g_apps.presses.push_back(p);
}
static int Clicks() { return g_apps.presses.empty() ? -1 : g_apps.presses.back().clicks; }
// An app opens its first window (AppModel::Refresh adds it to the dock), and later closes it again.
static void NewcomerOpens() {
    auto a = std::make_shared<AppItem>(); a->id = 77; a->name = L"Hunt Newcomer";
    g_apps.items.push_back(a); ++g_apps.structureVersion;
}
static void NewcomerCloses() {
    g_apps.items.erase(std::remove_if(g_apps.items.begin(), g_apps.items.end(), [](auto& a) { return a->id == 77; }), g_apps.items.end());
    ++g_apps.structureVersion;
}

static void ClickTests() {
    for (auto* m : { &kLaptop, &kPortrait }) for (int edge : { 0, 2 }) {
        g_settings = Settings{}; g_settings.edge = (Edge)edge; Apps(6);
        auto d = MakeDock(*m, 0, true);
        currentDock = d.get();
        const POINT edgePt = EdgePoint(d->mon, (Edge)edge, 0.5f);
        Advance(*d, edgePt, 0.8);
        // the button goes down and up where the pointer is (the dock reads the pointer at the press)
        auto press = [&](POINT p) { cursor = p; physicalLeft = true; d->WndProc(WM_LBUTTONDOWN, MK_LBUTTON, ClientPoint(*d, p)); };
        auto release = [&](POINT p) { cursor = p; physicalLeft = false; d->WndProc(WM_LBUTTONUP, 0, ClientPoint(*d, p)); };

        // a plain click on an icon: one click of its app
        auto second = g_apps.items[1];   // slot 2
        CountClicks(second);
        POINT p = SlotPoint(*d, 2); Advance(*d, p, 0.5); p = SlotPoint(*d, 2);
        press(p); Advance(*d, p, 0.05); release(p);
        Check(Clicks() == 1 && captureOwner == nullptr, Label("a click on an icon clicks its app once", *m, edge).c_str());

        // another app opens its first window (or an unpinned app closes its last one) between the press and the
        // release - a quick click lasts a few frames: the click still counts
        CountClicks(second);
        press(p); NewcomerOpens(); Advance(*d, p, 2.0 / 120); release(p);
        Check(Clicks() == 1, Label("an app opening during a click on an icon: the click still counts", *m, edge).c_str());
        NewcomerCloses(); Advance(*d, edgePt, 1.0);
        int starts = startClicks;
        p = SlotPoint(*d, 0); Advance(*d, p, 0.5); p = SlotPoint(*d, 0);
        press(p); NewcomerOpens(); Advance(*d, p, 2.0 / 120); release(p);
        Check(startClicks == starts + 1, Label("an app opening during a click on the start button: Start still opens", *m, edge).c_str());
        NewcomerCloses(); Advance(*d, edgePt, 1.0);

        // a button-up that reaches the dock a few frames after the button went up (a busy moment): still a click
        CountClicks(second);
        p = SlotPoint(*d, 2); Advance(*d, p, 0.5); p = SlotPoint(*d, 2);
        press(p); Advance(*d, p, 0.05); physicalLeft = false; Advance(*d, p, 0.25); release(p);
        Check(Clicks() == 1 && !d->dragging, Label("a button-up a quarter second late still clicks", *m, edge).c_str());

        // The mouse capture goes to another window between the press and the release (a menu opened with the right
        // button while the left one is still down takes it): the button-up never comes to the dock.
        p = SlotPoint(*d, 2); Advance(*d, p, 0.5); p = SlotPoint(*d, 2);
        press(p); Advance(*d, p, 0.05);
        captureOwner = nullptr; d->WndProc(WM_CAPTURECHANGED, 0, (LPARAM)(uintptr_t)0x0BADCAFE);
        physicalLeft = false;
        Advance(*d, Outside(d->mon), 1.5);
        Check(d->reveal.x < 0.002f && !d->Busy(), Label("a press whose button-up went elsewhere does not keep the dock up and busy", *m, edge).c_str());
        // later the button is held for something else on the screen (selecting text, moving a window)
        const POINT mid = { (d->mon.left + d->mon.right) / 2, (d->mon.top + d->mon.bottom) / 2 };
        physicalLeft = true;
        Advance(*d, mid, 0.2); Advance(*d, { mid.x + 80, mid.y + 80 }, 0.2);
        bool phantom = d->dragging;
        physicalLeft = false; Advance(*d, Outside(d->mon), 0.3);
        Check(!phantom, Label("...a drag elsewhere on the screen later drags no dock icon", *m, edge).c_str());
        auto third = g_apps.items[2];   // slot 3
        CountClicks(third);
        Advance(*d, edgePt, 0.8);
        p = SlotPoint(*d, 3); Advance(*d, p, 0.5); p = SlotPoint(*d, 3);
        int version = g_apps.structureVersion;
        press(p); Advance(*d, p, 0.05); release(p);
        Check(Clicks() == 1 && g_apps.structureVersion == version && !d->dragging,
              Label("...and the next click on an icon is a click, not a move", *m, edge).c_str());
        d->EndDrag(false); captureOwner = nullptr;   // (a failed run must not leak into the next checks)

        // The capture goes without a word (another desktop: the lock screen, a UAC prompt - neither the button-up nor a
        // WM_CAPTURECHANGED comes): once the primary button has been up for a while the press is over all the same.
        Advance(*d, edgePt, 0.8);
        p = SlotPoint(*d, 2); Advance(*d, p, 0.5); p = SlotPoint(*d, 2);
        press(p); Advance(*d, p, 0.05);
        captureOwner = nullptr; physicalLeft = false;
        Advance(*d, Outside(d->mon), 1.9);
        const bool stillPressed = d->pressKind == Dock::HitSlot && d->reveal.target == 1;
        Advance(*d, Outside(d->mon), 2.1);
        Check(stillPressed && d->pressKind == Dock::HitNone && d->pressSlot < 0 && d->reveal.x < 0.002f && !d->Busy(),
              Label("a press whose button-up never came: over after 2 s, the dock away and resting", *m, edge).c_str());
        Advance(*d, edgePt, 0.8);
        p = SlotPoint(*d, 2); Advance(*d, p, 0.5); p = SlotPoint(*d, 2);
        press(p); Advance(*d, p, 0.05);   // ...and a drag elsewhere on the screen before those 2 s are up
        captureOwner = nullptr; physicalLeft = false; Advance(*d, mid, 0.2);
        physicalLeft = true; Advance(*d, mid, 0.1); Advance(*d, { mid.x + 80, mid.y + 80 }, 0.3);
        phantom = d->dragging;
        physicalLeft = false; Advance(*d, Outside(d->mon), 4.0);
        Check(!phantom && d->pressKind == Dock::HitNone && d->reveal.x < 0.002f && !d->Busy(),
              Label("...and a drag elsewhere before then drags no dock icon", *m, edge).c_str());
        d->EndDrag(false); d->ClearPress(); captureOwner = nullptr;

        // Right-click on a running app: its windows by title. An & in a title (Q&A, R&D, Tom & Jerry) is text, not
        // the menu's access-key mark; a tab is no column break. (The windows are set only for the right-click: no
        // preview is ever shown.)
        Advance(*d, edgePt, 0.5);
        p = SlotPoint(*d, 1); Advance(*d, p, 0.5); p = SlotPoint(*d, 1);
        HWND titled = NewDockWindow(), tabbed = NewDockWindow();
        SetWindowTextW(titled, L"Q&A notes - Hunt");
        SetWindowTextW(tabbed, L"a\ttab - Hunt");
        g_apps.items[0]->windows = { titled, tabbed };
        d->WndProc(WM_RBUTTONUP, 0, ClientPoint(*d, p));
        g_apps.items[0]->windows.clear();
        DestroyWindow(titled); DestroyWindow(tabbed);
        Check(menuItems.size() > 2 && menuItems[0] == L"Q&&A notes - Hunt" && menuItems[1] == L"a tab - Hunt",
              Label("the app menu shows a window title with & (or a tab) as it is", *m, edge).c_str());
        // an output device named with an & (the audio button's menu): the same
        speakersName = L"B&O Speakers";
        d->ClickAudio();
        speakersName = L"Speakers";
        Check(!menuItems.empty() && menuItems[0] == L"B&&O Speakers", Label("the output device menu shows a name with & as it is", *m, edge).c_str());

        // the wheel over the volume button (WM_MOUSEWHEEL carries screen coordinates, negative left of the primary)
        Advance(*d, edgePt, 0.5);
        POINT vol = ButtonPoint(*d, ButtonId::Volume); Advance(*d, vol, 0.5); vol = ButtonPoint(*d, ButtonId::Volume);
        float u, v; d->ScreenToUV({ (LONG)(short)vol.x, (LONG)(short)vol.y }, u, v);
        Check(d->HitTestUV(u, v).Is(ButtonId::Volume), Label("the wheel's screen point over the volume button is the volume button", *m, edge).c_str());

        // the pointer unreadable (the lock screen, a UAC prompt): main.cpp gives the docks a point off every monitor
        // (kNoPointer, idle_wait.h), and a dock up under the pointer goes away as when it leaves
        p = SlotPoint(*d, 2); Advance(*d, p, 0.5);
        Advance(*d, { idlewait::kNoPointerX, idlewait::kNoPointerY }, 2.0);
        Check(d->reveal.x < 0.002f && !d->Busy() && d->hover.slot < 0, Label("no pointer to read: the dock goes away and rests", *m, edge).c_str());

        g_apps.presses.clear();
        captureOwner = nullptr; currentDock = nullptr;
    }
}

// ---------------------------------------------------------------- leaving the magnified apps sideways
// Seen on a test PC, 2026-11-12 (mod.25): over the tray, or FluentFlyout's widget, the end icon stayed enlarged - the
// dock held its whole layout there so the clock would not slide away from the pointer. Now the apps shrink back
// and only the end of the dock on that side is held; dock.log tells it once the dock is put away.
static void SideExitTests() {
    for (auto* m : { &kDesk, &kLaptop }) {
        g_settings = Settings{}; Apps(12);   // 44 px icons, magnification 1.7, auto-hide: that PC's settings
        auto d = MakeDock(*m, 0, false);
        const float v = d->margin + d->barThick / 2;
        Advance(*d, EdgePoint(d->mon, Edge::Bottom, 0.5f), 0.8);
        POINT icon{};
        for (int i = 0; i < 6; ++i) { icon = SlotPoint(*d, 12); Advance(*d, icon, 0.3); }
        const float big = d->slots[12].size, held = d->barU1, clock = d->cells[Dock::HitClock].u0, start = d->barU0;
        const POINT onClock = d->ScreenPointUV(d->cells[Dock::HitClock].Mid(), v);
        logLines.clear();
        float drift = 0; bool hovered = true;
        auto track = [&] { drift = std::max({ drift, std::fabs(d->barU1 - held), std::fabs(d->cells[Dock::HitClock].u0 - clock) }); };
        for (int f = 1; f <= 24; ++f) { Advance(*d, { icon.x + (onClock.x - icon.x) * f / 24, icon.y }, 1.0 / 120); track(); }
        for (int f = 0; f < 96; ++f) { Advance(*d, onClock, 1.0 / 120); track(); hovered &= d->hover.kind == Dock::HitClock; }
        Check(big > d->base * 1.6f && d->slots[12].size <= d->base + 0.01f && d->magAmount.target == 0,
              Label("off the apps onto the clock: the enlarged end icon is back to its size", *m, 0).c_str());
        Check(drift <= 0.1f && hovered, Label("...the clock stays under the pointer on every frame", *m, 0).c_str());
        Check(d->pinSide == 1 && d->barU0 - start > d->base * (d->mag - 1), Label("...the other end of the dock comes in instead", *m, 0).c_str());
        Check(LogCount(L"magnify:") == 0, Label("...nothing in dock.log while the end is held", *m, 0).c_str());
        // On its way out, called back by the pointer at the screen edge under the clock: on the frame the dock is told
        // to come up it does not know yet that the pointer is on it - the end stays held all the same (eased for
        // that frame it twitched under the pointer, 0.5 px here and more at 60 Hz, and dock.log said "the pointer gone").
        for (int f = 0; f < 240 && !(d->reveal.target < 0.5f && d->reveal.x < 0.5f); ++f) Advance(*d, Outside(d->mon), 1.0 / 120);
        const bool onItsWayOut = d->reveal.target == 0 && d->reveal.x > 0.1f && d->pinSide == 1;
        drift = 0;
        for (int f = 0; f < 120; ++f) { Advance(*d, { onClock.x, d->mon.bottom - 1 }, 1.0 / 120); track(); }
        Check(onItsWayOut && d->reveal.target == 1 && d->reveal.x > 0.95f && d->pinSide == 1 && drift <= 0.1f && LogCount(L"magnify:") == 0,
              Label("called back on its way out by the pointer at the edge under the clock: the end stays held", *m, 0).c_str());
        Advance(*d, Outside(d->mon), 1.5);
        Check(d->reveal.x < 0.002f && d->pinSide == 0 && LogCount(L"held its tray end") == 1 && LogCount(L"let go: hidden") == 1,
              Label("put away: the end is let go, one line in dock.log", *m, 0).c_str());
    }
}

// The dock gets shorter while an end is held (mod.28 review): an app closes its last window while the pointer is on
// the clock, or FluentFlyout's slot shuts under a pointer on the widget. The held end stays where it is; back on the
// apps and fully grown, a shorter dock no longer reaches its centred place by growing - it stayed half the lost
// length off centre for as long as the pointer was on the apps (22 px for one app, 106 px for the widget). It eases
// back to where a dock of that length that never had an end held is.
static void SideExitShorterTests() {
    for (auto* m : { &kDesk, &kLaptop }) {
        const POINT edge = EdgePoint(m->rect, Edge::Bottom, 0.5f);
        // where a dock with these apps and no end ever held lies, magnified with the pointer at p
        struct Place { float u0, u1; bool grown, held; };
        auto neverHeld = [&](POINT p) {
            auto fresh = MakeDock(*m, 0, false);
            Advance(*fresh, edge, 0.8);
            bool held = false;
            for (int f = 0; f < 180; ++f) { Advance(*fresh, p, 1.0 / 120); held |= fresh->pinSide != 0; }
            return Place{ fresh->barU0, fresh->barU1, fresh->magAmount.target == 1 && fresh->magAmount.x > 0.999f, held };
        };

        // ---- the tray end: an app closes while the pointer is on the clock
        g_settings = Settings{}; Apps(12);
        {
            auto d = MakeDock(*m, 0, false);
            const float v = d->margin + d->barThick / 2;
            Advance(*d, edge, 0.8);
            POINT icon{};
            for (int i = 0; i < 6; ++i) { icon = SlotPoint(*d, 12); Advance(*d, icon, 0.3); }
            const float held = d->barU1, clock = d->cells[Dock::HitClock].u0, length = d->barU1 - d->barU0;
            const POINT onClock = d->ScreenPointUV(d->cells[Dock::HitClock].Mid(), v);
            float drift = 0; bool hovered = true;
            auto track = [&] { drift = std::max({ drift, std::fabs(d->barU1 - held), std::fabs(d->cells[Dock::HitClock].u0 - clock) }); };
            for (int f = 1; f <= 24; ++f) { Advance(*d, { icon.x + (onClock.x - icon.x) * f / 24, icon.y }, 1.0 / 120); track(); }
            for (int f = 0; f < 96; ++f) { Advance(*d, onClock, 1.0 / 120); track(); hovered &= d->hover.kind == Dock::HitClock; }
            const float atRest = d->barU1 - d->barU0;
            g_apps.items.erase(g_apps.items.begin() + 5); ++g_apps.structureVersion;   // AppModel::Refresh: its last window closed
            for (int f = 0; f < 180; ++f) { Advance(*d, onClock, 1.0 / 120); track(); hovered &= d->hover.kind == Dock::HitClock; }
            Check(d->pinSide == 1 && d->slots.size() == 12 && atRest - (d->barU1 - d->barU0) > d->base && d->magAmount.target == 0,
                  Label("an app closes while the pointer is on the clock: the dock is shorter, its tray end still held", *m, 0).c_str());
            Check(drift <= 0.1f && hovered, Label("...and the clock stays under the pointer on every frame of it", *m, 0).c_str());
            const POINT middle = SlotPoint(*d, 6);
            Advance(*d, middle, 1.5);
            const Place plain = neverHeld(middle);
            Check(d->magAmount.target == 1 && d->magAmount.x > 0.999f && length - (d->barU1 - d->barU0) > d->base && plain.grown && !plain.held,
                  Label("...back on an app in the middle the shorter dock is fully magnified", *m, 0).c_str());
            Check(d->pinSide == 0 && std::fabs(d->barU0 - plain.u0) <= 0.1f && std::fabs(d->barU1 - plain.u1) <= 0.1f &&
                  std::fabs((d->barU0 + d->barU1) * 0.5f - d->len * 0.5f) <= 0.1f,
                  Label("...and centred where a dock of that length never held is, nothing held any more", *m, 0).c_str());
            if (d->pinSide != 0 || std::fabs(d->barU0 - plain.u0) > 0.1f)
                printf("  (pinSide %d, the dock %.2f px off its centred place)\n", d->pinSide, d->barU0 - plain.u0);
        }

        // ---- the start end: FluentFlyout's slot shuts under a pointer on the widget. There is no FluentFlyout here:
        // the slot is opened by hand (as dock_interaction_test.cpp's PinnedLayoutTests does), before every frame -
        // Update finds no widget and shuts it, which is also how it shuts for good once the test stops opening it.
        g_settings = Settings{}; Apps(12);
        {
            auto d = MakeDock(*m, 0, false);
            const float v = d->margin + d->barThick / 2, step = 12 * d->scale, lead = std::ceil(200 * d->scale / step) * step + d->gap;
            auto withSlot = [&](POINT p, int frames) { for (int f = 0; f < frames; ++f) { d->widgetWidth.snap(lead); Advance(*d, p, 1.0 / 120); } };
            withSlot(edge, 96);
            POINT icon{};
            for (int i = 0; i < 6; ++i) { icon = SlotPoint(*d, 0); withSlot(icon, 36); }
            const float held = d->barU0, widget = d->widgetU0, length = d->barU1 - d->barU0, big = d->slots[0].size;
            const POINT onWidget = d->ScreenPointUV((d->widgetU0 + d->widgetU1) * 0.5f, v);
            float drift = 0;
            auto track = [&] { drift = std::max({ drift, std::fabs(d->barU0 - held), std::fabs(d->widgetU0 - widget) }); };
            for (int f = 1; f <= 24; ++f) { withSlot({ icon.x + (onWidget.x - icon.x) * f / 24, icon.y }, 1); track(); }
            for (int f = 0; f < 96; ++f) { withSlot(onWidget, 1); track(); }
            Check(big > d->base * 1.6f && d->widgetU1 - d->widgetU0 > 150 * d->scale && d->pinSide == -1 && d->magAmount.target == 0 &&
                  d->slots[0].size <= d->base + 0.01f && drift <= 0.1f,
                  Label("off the start button onto FluentFlyout's widget: the start of the dock is held, the widget stays under the pointer", *m, 0).c_str());
            // the slot shuts (0.4 s): the apps come to the pointer, which is on them after some frames - the dock
            // magnifies again, out from the start it still holds; only fully grown it eases to its place
            int beside = 0, growing = 0; bool still = true;
            for (int f = 0; f < 240; ++f) {
                Advance(*d, onWidget, 1.0 / 120);
                still = still && !(d->magAmount.target == 1 && d->magAmount.settled());
                if (still) { track(); ++(d->magAmount.target == 0 ? beside : growing); }
            }
            Check(beside >= 5 && growing >= 30 && drift <= 0.1f && d->widgetWidth.x == 0 && d->magAmount.target == 1,
                  Label("the slot shuts under the pointer: the start of the dock stays put while it does and while the apps, now under the pointer, grow again", *m, 0).c_str());
            const POINT middle = SlotPoint(*d, 6);
            Advance(*d, middle, 1.5);
            const Place plain = neverHeld(middle);
            Check(d->magAmount.target == 1 && d->magAmount.x > 0.999f && length - (d->barU1 - d->barU0) > 150 * d->scale && plain.grown && !plain.held,
                  Label("...back on an app in the middle the shorter dock is fully magnified", *m, 0).c_str());
            Check(d->pinSide == 0 && std::fabs(d->barU0 - plain.u0) <= 0.1f && std::fabs(d->barU1 - plain.u1) <= 0.1f &&
                  std::fabs((d->barU0 + d->barU1) * 0.5f - d->len * 0.5f) <= 0.1f,
                  Label("...and centred where a dock without the slot never held is, nothing held any more", *m, 0).c_str());
            if (d->pinSide != 0 || std::fabs(d->barU0 - plain.u0) > 0.1f)
                printf("  (pinSide %d, the dock %.2f px off its centred place)\n", d->pinSide, d->barU0 - plain.u0);
        }
    }
}

// ---------------------------------------------------------------- the frame trace (a characterization)
// What Dock::Update does frame by frame, written down as text: a fixed input script (the pointer comes in, runs
// along the apps, goes out sideways onto the tray, the clock, the sensor cell and its panel, presses, drags off the
// dock and back, a button-up that never comes, the wheel, a fullscreen game, a window over the bar, Start, the
// settings window, the shortcut, bounces...) on the fake screens - the dock's scale 1.0 (desktop, portrait left),
// 1.25 (laptop) and 1.5 (3840x2160) - at each of the four edges, auto-hide on and off, on a fake clock (1/60 s a frame;
// safety64, the press safety alone, 1/64 s: its 2 s come out exact). Every frame's line holds the window and its region,
// the reveal and magnification springs, the held end, every icon's place and size, what is hovered and pressed, the tooltip, the panel, what wakes the main loop
// (Busy, LiveGlass, resting) and what the dock asked of Windows and the stubbed modules in that frame (the fakes
// above: SetWindowPos, SetWindowRgn, the cloak, app bar messages, SetCapture, dock.log lines...).
// One "HASH: dock_trace/<screen>-<edge>-<script>-<hide|shown>" line per scene: the same build gives the same
// hashes; a refactoring of Update that changes nothing gives them too. Rounded as written (pixels and rectangles
// as integers, places along the dock to 0.01 px, springs to 4 places), so the hashes say what the dock does; a
// last HASH line, dock_trace/exact-all, hashes the same lines with every float written as its bits - it moves
// when a refactoring reorders float arithmetic (it must not). The text of each scene goes to
// trace-<scene>.txt next to the exe (only what changed from the frame before); LD_TRACE_EXACT=1 writes the
// exact lines there instead. The date of the clock's tooltip is fixed (HuntLocalTime, HuntDateFormatEx).
namespace frametrace {
constexpr float kDt = 1.f / 60;

// a field of a frame's line, as written (rounded) and to the bit (for the exact hash: every float as its bits in hex)
struct Out {
    std::string c, e;
    Out& operator<<(const char* t) { c += t; e += t; return *this; }
    Out& operator<<(char t) { c += t; e += t; return *this; }
    Out& operator<<(const std::string& t) { c += t; e += t; return *this; }
    void clear() { c.clear(); e.clear(); }
};
static void Hex(std::string& s, uint64_t v, int digits) {
    static const char kHex[] = "0123456789abcdef";
    char b[16];
    for (int i = digits - 1; i >= 0; --i) { b[i] = kHex[v & 15]; v >>= 4; }
    s.append(b, digits);
}
// fixed places, rounded half away from zero (no sign on a value that rounds to 0)
static void Fixed(std::string& s, double v, int places) {
    static const double kScale[] = { 1, 10, 100, 1000, 10000 };
    if (!(std::fabs(v) < 1e12)) { s += v != v ? "nan" : v > 0 ? "big" : "-big"; return; }
    long long n = std::llround(v * kScale[places]);
    if (n < 0) { s += '-'; n = -n; }
    const long long whole = n / (long long)kScale[places], part = n % (long long)kScale[places];
    s += std::to_string(whole);
    if (places) {
        char b[8]; long long p = part;
        for (int i = places - 1; i >= 0; --i) { b[i] = char('0' + p % 10); p /= 10; }
        s += '.'; s.append(b, places);
    }
}
static void F(Out& s, float v, int places) {
    Fixed(s.c, v, places);
    uint32_t bits; memcpy(&bits, &v, 4); Hex(s.e, bits, 8);
}
static void Dbl(Out& s, double v) {
    Fixed(s.c, v, 4);
    uint64_t bits; memcpy(&bits, &v, 8); Hex(s.e, bits, 16);
}
static void I(Out& s, long long v) { s << std::to_string(v); }
static void R(Out& s, const RECT& r) { I(s, r.left); s << ','; I(s, r.top); s << ','; I(s, r.right); s << ','; I(s, r.bottom); }
static void Since(Out& s, double now, double t) { if (t < 0) s << '-'; else Dbl(s, now - t); }

static int version0 = 0;   // AppModel::structureVersion at the start of the scene (it only grows, run after run)
struct Field { std::string key; Out v; };
using Fields = std::vector<Field>;
static void Describe(Fields& f, Dock& d, POINT cur, double now) {
    f.clear(); Out s;
    auto put = [&](std::string k) { f.push_back({ std::move(k), std::move(s) }); s.clear(); };
    I(s, cur.x); s << ','; I(s, cur.y); s << ' '; F(s, d.cursorU, 2); s << ',';
    if (d.cursorV > 1e8f) s << "off"; else F(s, d.cursorV, 2);
    put("cur");
    I(s, d.winX); s << ','; I(s, d.winY); s << ','; I(s, d.winW); s << ','; I(s, d.winH); s << " T"; F(s, d.T, 2); put("win");
    R(s, d.curRegion); if (d.cloaked) s << " cloaked"; put("rgn");
    F(s, d.reveal.x, 4); s << '/'; F(s, d.reveal.target, 4); s << " v"; F(s, d.reveal.v, 4); put("rev");
    F(s, d.magAmount.x, 4); s << '/'; F(s, d.magAmount.target, 4); s << " v"; F(s, d.magAmount.v, 4); s << " hu"; F(s, d.hoverU, 2); put("mag");
    I(s, d.pinSide); s << ' '; F(s, d.pinU, 2); s << ' '; F(s, d.pinBlend.x, 4); s << '/'; F(s, d.pinBlend.target, 4);
    s << " off"; F(s, d.pinOff, 2); s << (d.pinTold ? " told" : ""); put("pin");
    F(s, d.fit, 4); s << ' '; F(s, d.base, 2); s << ' '; F(s, d.pad, 2); s << ' '; F(s, d.gap, 2); s << ' '; F(s, d.barThick, 2);
    s << ' '; F(s, d.margin, 2); s << ' '; F(s, d.scale, 4); s << " len"; F(s, d.len, 2); put("metrics");
    F(s, d.hideOff, 2); put("hideOff");
    F(s, d.barU0, 2); s << '-'; F(s, d.barU1, 2); s << " sep"; F(s, d.sepU, 2); s << " apps"; F(s, d.appsU0, 2); s << " trayW"; F(s, d.trayW, 2); put("bar");
    R(s, d.BarScreenRect()); put("barRect");
    // the tray's cells and small buttons, from the separator (they move with it)
    for (int k : { (int)Dock::HitTray, (int)Dock::HitClock, (int)Dock::HitSensor, (int)Dock::HitBattery }) {
        F(s, d.cells[k].u0 - d.sepU, 2); s << '-'; F(s, d.cells[k].u1 - d.sepU, 2); s << ' ';
    }
    for (int i = 0; i < kButtonCount; ++i) {
        const auto& c = d.buttonCells[i];
        if (c.u1 <= c.u0) continue;
        s << 'b'; I(s, i); s << ':'; F(s, c.u0 - d.sepU, 2); s << '-'; F(s, c.u1 - d.sepU, 2); s << '/'; I(s, c.half); s << ' ';
    }
    for (auto& c : d.notifyCells) { s << 'n'; F(s, c.u0 - d.sepU, 2); s << '-'; F(s, c.u1 - d.sepU, 2); s << '/'; I(s, c.half); s << ' '; }
    put("cells");
    for (size_t i = 0; i < d.slots.size(); ++i) {
        const auto& sl = d.slots[i];
        const std::string k = "s" + std::to_string(i);
        I(s, sl.app ? sl.app->id : 0); if (sl.ghost) s << 'g';
        s << " u"; F(s, sl.u, 2); s << " sz"; F(s, sl.size, 2); s << " w"; F(s, sl.weight, 4);
        if (sl.bounceOff != 0 || sl.bounceCount || sl.bounceLoop) {
            s << " bo"; F(s, sl.bounceOff, 2); s << ' '; I(s, sl.bounceCount); s << (sl.bounceLoop ? "L" : "") << (sl.bounceHiddenMode ? "H" : "");
        }
        put(k);
        s << "ap"; F(s, sl.appear.x, 4); s << " sh"; F(s, sl.shrink.x, 4); s << " pr"; F(s, sl.press.x, 4);
        s << " gb"; F(s, sl.gapBefore.x, 4); s << " pk"; F(s, sl.peek.x, 4);
        put(k + "~");
    }
    F(s, d.trailingGap.x, 4); s << " widget "; F(s, d.widgetWidth.x, 2); s << '/'; F(s, d.widgetWidth.target, 2); s << ' ';
    F(s, d.widgetU0, 2); s << '-'; F(s, d.widgetU1, 2); s << (d.widgetShowing ? " showing" : ""); put("gaps");
    I(s, d.hover.kind); s << ','; I(s, d.hover.slot); s << ','; I(s, (int)d.hover.button); s << ','; I(s, d.hover.notify); put("hov");
    I(s, d.pressKind); s << ','; I(s, d.pressSlot); s << ','; I(s, (int)d.pressButton); s << " up"; Since(s, now, d.pressUpSince);
    s << (d.dragging ? " drag" : "") << " drop"; I(s, d.dropIndex); s << (d.dropRemove ? " remove" : "") << (captureOwner == &d ? " capture" : "");
    put("press");
    s << Utf8(d.tooltipText) << " | "; F(s, d.tooltipAlpha.x, 4); s << '/'; F(s, d.tooltipAlpha.target, 4);
    if (d.tooltipAlpha.x > 0.001f || d.tooltipAlpha.target > 0.5f) { s << ' '; F(s, d.tooltipU, 2); s << ','; F(s, d.tooltipV, 2); }
    put("tip");
    F(s, d.panelAlpha.x, 4); s << '/'; F(s, d.panelAlpha.target, 4);
    if (d.panelAlpha.x > 0.001f || d.panelAlpha.target > 0.5f) {
        s << ' '; F(s, d.panelRect.left, 2); s << ','; F(s, d.panelRect.top, 2); s << ','; F(s, d.panelRect.right, 2); s << ','; F(s, d.panelRect.bottom, 2);
    }
    s << " out"; Since(s, now, d.panelOutsideSince); put("panel");
    s << (d.Busy() ? "busy" : "idle") << (d.LiveGlass() ? " live" : "") << (d.restingPrev ? " resting" : "") << (d.FullscreenHere() ? " fullscreen" : "")
      << (now - d.lastCaptureWanted < 0.05 ? " capture-wanted" : "");
    put("wake");
    s << "edge"; Since(s, now, d.edgeSince); s << " outside"; Since(s, now, d.outsideSince);
    s << " hotkey"; if (d.hotkeyHoldUntil > now) Dbl(s, d.hotkeyHoldUntil - now); else s << '-';
    put("since");
    I(s, d.frontRaises); s << " recheck"; if (d.frontRecheckAt < 0) s << '-'; else Dbl(s, d.frontRecheckAt - now); s << (d.lastFront ? " fg" : ""); put("front");
    if (d.appBar) R(s, d.appBarRect); else s << '-';
    s << (d.appBarFailing ? " failing" : "") << " bars"; I(s, (long long)shell.bars.size()); put("appbar");
    I(s, d.sbCount); s << ' '; F(s, d.sbOff, 2); s << ' '; F(s, d.sbPeek.x, 4); s << '/'; F(s, d.sbPeek.target, 4); put("sb");
    int clicks = 0;
    for (auto& p : g_apps.presses) clicks += p.clicks;
    for (auto& a : g_apps.items) { I(s, a->id); s << (a->pinned ? "p " : " "); }
    s << "v"; I(s, g_apps.structureVersion - version0); s << " clicks"; I(s, clicks); put("apps");
    s << "start"; I(s, startClicks); s << " theme"; I(s, themeToggles); s << (themeLight ? "L" : "D") << " settings"; I(s, settingsWindows);
    s << " menus"; I(s, menus); s << " raises"; I(s, raises); s << " desktop"; I(s, nativetray::desktopToggles); put("calls");
    // what Update handed the linked modules in this frame (the fakes above), and whether something asked for a redraw
    // since the frame before (dirty: read only by Render, which does not run here - Script::Step clears it each frame)
    s << (traceFly.empty() ? "-" : traceFly); put("fly");
    s << (tracePreview.empty() ? "-" : tracePreview); put("pv");
    s << (tracePlace.empty() ? "-" : tracePlace); put("place");
    s << (d.dirty ? "dirty" : "clean"); put("dirty");
}

struct Recorder {
    std::string name, events, file;
    Fields cur, prev;
    uint64_t hash = 1469598103934665603ull;
    int frame = 0;
};
static uint64_t exactAll = 1469598103934665603ull;
static bool writeExact = false;   // LD_TRACE_EXACT=1: the files get the exact lines (floats as bits)
static void Fnv(uint64_t& h, const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } }

static void Frame(Recorder& r, Dock& d, POINT cur, double now) {
    Describe(r.cur, d, cur, now);
    Out ev; ev << r.events;
    r.cur.push_back({ "ev", std::move(ev) });
    std::string line = "f" + std::to_string(r.frame), delta = line, exactLine = line;
    for (size_t i = 0; i < r.cur.size(); ++i) {
        const Field& f = r.cur[i];
        line += ' '; line += f.key; line += '='; line += f.v.c;
        exactLine += ' '; exactLine += f.key; exactLine += '='; exactLine += f.v.e;
        const std::string& shown = writeExact ? f.v.e : f.v.c;
        const Field* was = i < r.prev.size() && r.prev[i].key == f.key ? &r.prev[i] : nullptr;
        if (!was) for (auto& p : r.prev) if (p.key == f.key) { was = &p; break; }
        if (!was || (writeExact ? was->v.e : was->v.c) != shown) { delta += ' '; delta += f.key; delta += '='; delta += shown; }
    }
    for (auto& p : r.prev) {
        bool still = false;
        for (auto& f : r.cur) if (f.key == p.key) { still = true; break; }
        if (!still) delta += ' ' + p.key + "=<gone>";
    }
    line += '\n'; exactLine += '\n'; delta += '\n';
    Fnv(r.hash, line);
    Fnv(exactAll, exactLine);
    r.file += delta;
    r.prev.swap(r.cur);
    r.events.clear();
    ++r.frame;
}

// the script's steps: frames on the fake clock with the pointer at, or gliding to, a place worked out from the
// dock's layout of the frame before (as a hand would follow what it sees)
struct Script {
    Dock& d; Recorder& r;
    std::function<void()> before;   // run before every frame's Update (FluentFlyout's slot held open by hand)
    float dt = kDt;                 // the frame time (safety64: 1/64 s, a binary fraction: 2 s on is exactly 2 s on)
    void Act(const std::string& what) { TraceEvent("act:" + what); }
    void Step(POINT p) {
        if (before) before();
        cursor = p; simTime += dt;
        d.Update(simTime, dt, p);
        Frame(r, d, p, simTime);
        traceFly.clear(); tracePreview.clear(); tracePlace.clear();
        d.dirty = false;   // the next frame's line says whether anything (Update, a message) asked for a redraw since this one
    }
    void Hold(int n, const std::function<POINT()>& at) { for (int i = 0; i < n; ++i) Step(at()); }
    void Hold(int n, POINT p) { for (int i = 0; i < n; ++i) Step(p); }
    void Glide(int n, const std::function<POINT()>& to) {
        const POINT from = cursor;
        for (int i = 1; i <= n; ++i) { const POINT t = to(); Step({ from.x + (t.x - from.x) * i / n, from.y + (t.y - from.y) * i / n }); }
    }
    void Glide(int n, POINT to) { Glide(n, [to] { return to; }); }
    void Mouse(UINT msg, POINT p) {
        const char* name = msg == WM_LBUTTONDOWN ? "WM_LBUTTONDOWN" : msg == WM_LBUTTONUP ? "WM_LBUTTONUP" : msg == WM_MOUSEWHEEL ? "WM_MOUSEWHEEL" :
                           msg == WM_RBUTTONUP ? "WM_RBUTTONUP" : msg == WM_MBUTTONUP ? "WM_MBUTTONUP" : "WM_other";
        Act(std::string(name) + " " + std::to_string(p.x) + "," + std::to_string(p.y));
        cursor = p; currentDock = &d;
        if (msg == WM_LBUTTONDOWN) { physicalLeft = true; d.WndProc(msg, MK_LBUTTON, ClientPoint(d, p)); }
        else if (msg == WM_LBUTTONUP) { physicalLeft = false; d.WndProc(msg, 0, ClientPoint(d, p)); }
        else if (msg == WM_RBUTTONUP || msg == WM_MBUTTONUP) d.WndProc(msg, 0, ClientPoint(d, p));
        else d.WndProc(msg, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(p.x, p.y));   // the wheel: screen coordinates
    }
};

static bool tracePopup = false;   // the window in front (fgWindow) is a shell pop-up (Start) that shows
static bool FakeShellPopup(HWND h) { return tracePopup && h == fgWindow; }
static bool FakeRaiseOver(HWND h) { return h == fgWindow; }   // an app's window, not Explorer's or the dock's own

static const FakeMonitor* const kScreens[] = { &kLaptop, &kDesk, &kPortrait, &kUhd };
static const char* const kScreenNames[] = { "laptop", "desk", "portrait", "uhd" };
static const char* const kEdgeNames[] = { "bottom", "left", "right", "top" };
enum class Kind { Pass, Press, Front, Safety, Edges };
static const char* const kScriptNames[] = { "pass", "press", "front", "safety64", "edges64" };
constexpr int kKinds = 5;

// A middle click is sent only where neither reading of its point - as the dock's client point (WndProc) or as a
// screen point - lies on an app (WM_MBUTTONUP starts it: AppModel::Launch) or the volume button (it mutes the PC):
// whatever a change to the dock's code does with the point, the test never reaches those
static bool MiddleClickSafe(Dock& d, POINT screen) {
    const LPARAM lp = ClientPoint(d, screen);
    float u, v;
    d.ClientToUV(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), u, v);
    const Dock::Hit a = d.HitTestUV(u, v);
    d.ScreenToUV(POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, u, v);
    const Dock::Hit b = d.HitTestUV(u, v);
    for (const Dock::Hit& h : { a, b })
        if ((h.kind == Dock::HitSlot && h.slot >= 0 && h.slot < (int)d.slots.size() && d.slots[h.slot].app) || h.Is(ButtonId::Volume)) return false;
    return true;
}

// what the script saw on the way (checked after it: that it did what it says)
struct Seen {
    bool up = false, magnified = false, trayEnd = false, notifyTip = false, startEnd = false, dateTip = false, panel = false, panelHit = false,
         panelShut = false, widget = false, batteryTip = false, hiddenAtEnd = false, upAtEnd = false;
    bool dragOff = false, keptUpOffDock = false, ghostShift = false, dropped = false, buttonClick = false, startClick = false;
    bool gameAway = false, gameEdgeNo = false, raised = false, raisedAtOnce = false, popupUp = false, settingsUp = false, hotkeyUp = false,
         hotkeyAway = false, peek = false, sensorHop = false, launchHop = false;
    int givenUp = 0, lostCapture = 0, upFrames = -1;
    // edges64
    std::string prewarm, frontRound;
    int showFrames = -1, panelFrames = -1, hideFrames = -1, enterFrames = -1, dragPx = -1, bounceExtended = -1;
    bool panelOpen = false, onButton = false, buttonCleared = false, onNotify = false, notifyCleared = false, trayClicks = false, otherSlot = false,
         otherButton = false, unpinnedKept = false, sbNoPeek = false, quietNoHop = false, menuKept = false, previewKept = false, flyoutKept = false,
         hidAfter = false, tipBefore = false, previewNoTip = false, widgetPlaced = false, launchExtended = false;
};

static void Reset(const FakeMonitor& m, int edge, bool autoHide) {
    g_settings = Settings{}; g_settings.edge = (Edge)edge; g_settings.autoHide = autoHide; g_settings.trayIcons = true;
    Apps(8);
    g_apps.presses.clear();   // every app click only counts (as CountClicks): nothing is started or activated
    for (auto& a : g_apps.items) { AppModel::Press p; p.item = a->id; p.clicks = 0; g_apps.presses.push_back(p); }
    shell.Reset();
    fgOn = fgZoomed = false; fgWindow = kFg; fgStyle = WS_POPUP; fgRect = {}; fgClient = {}; fgMonitor = nullptr; fgInfoFails = false;
    physicalLeft = physicalRight = buttonsSwapped = false;
    traceAbove = false; tracePopup = false; traceDate = true;
    Dock::debugForceShow = Dock::debugForcePanel = Dock::screenUnseen = false;
    g_settingsHold.open = false; g_settingsHold.wnd = nullptr;
    features::SetQuiet(0);
    g_sys.volume = 0.5f; g_sys.muted = false; g_sys.net = 1;
    battery::State bat;   // the laptop screen's: on battery, 76 %, no estimate; none on the other screens
    if (&m == &kLaptop) { bat.present = true; bat.percent = 76; }
    g_sys.SetBattery(bat);
    g_sensors.live = false; g_sensors.d = {}; g_sensors.panel = PanelData{}; g_sensors.vramNvmlTempSeen = false; g_sensors.alertPending = false;
    themeLight = false; oledOn = false;
    startClicks = themeToggles = settingsWindows = menus = raises = 0; nativetray::desktopToggles = 0;
    logLines.clear();
    captureOwner = currentDock = nullptr;
    fluentflyout::traceWidget = {}; fluentflyout::traceInFlyout = false; traceInPreview = tracePreviewOpen = false;
    for (int& n : trayhost::sends) n = 0;
    trayhost::placeShown = trayhost::placeHidden = 0;
    traceFly.clear(); tracePreview.clear(); tracePlace.clear();
    simTime = 1000;
}
static AppItem* Item(int id) { for (auto& a : g_apps.items) if (a->id == id) return a.get(); return nullptr; }
static int SlotOf(const Dock& d, int id) { for (size_t i = 0; i < d.slots.size(); ++i) if (d.slots[i].app && d.slots[i].app->id == id) return (int)i; return -1; }
static int Clicks() { int n = 0; for (auto& p : g_apps.presses) n += p.clicks; return n; }

static uint64_t RunScene(int screen, int edge, Kind kind, bool autoHide, Seen& seen, bool write) {
    const FakeMonitor& m = *kScreens[screen];
    const std::string name = std::string(kScreenNames[screen]) + "-" + kEdgeNames[edge] + "-" + kScriptNames[(int)kind] + (autoHide ? "-hide" : "-shown");
    Reset(m, edge, autoHide);
    // edges64: delays that come out exact on its clock of 1/64 s - showing 4 frames, hiding 24 frames on
    if (kind == Kind::Edges) { g_settings.showDelay = 0.0625f; g_settings.hideDelay = 0.375f; }
    version0 = g_apps.structureVersion;
    auto keepPopup = Dock::isShellPopup; auto keepRaise = Dock::raiseOver;
    Dock::isShellPopup = FakeShellPopup; Dock::raiseOver = FakeRaiseOver;
    auto dock = MakeDock(m, 0, true);   // its own hidden window: the region, the cloak and the app bar are real calls for it
    Dock& d = *dock;
    // two of the taskbar's tray icons (Settings::trayIcons), as SyncNotifyIcons would take them from the tray host; no host
    // runs here (its version stays where it is), so they stay; the dock's hover messages to them go nowhere (no such icon)
    d.notifyCells.resize(2);
    d.notifyCells[0].key = 0x7A11; d.notifyCells[0].tip = L"Hunt Tray One";
    d.notifyCells[1].key = 0x7A12;
    d.notifyVersion = trayhost::Version() * 2 + 1;
    traceDockWindow = d.hwnd;
    Recorder r; r.name = name;
    r.file = "# dock_trace " + name + " (dock_layout_test.cpp FrameTraceTests): screen " + m.name + ", edge " + kEdgeNames[edge] +
             ", auto-hide " + (autoHide ? "on" : "off") + (kind == Kind::Safety || kind == Kind::Edges ? ", 1/64 s a frame from t=" : ", 1/60 s a frame from t=") + std::to_string(simTime) +
             "; each line: what changed since the frame before\n";
    traceEvents = &r.events;
    Script s{ d, r };
    const bool vert = IsVertical((Edge)edge);
    auto v = [&d] { return d.margin + d.barThick / 2; };
    auto slot = [&d, v](int i) { return [&d, v, i] { const int k = std::min(i, (int)d.slots.size() - 1); return d.ScreenPointUV(d.slots[k].u, v()); }; };
    auto cell = [&d, v](int k) { return [&d, v, k] { return d.ScreenPointUV(d.cells[k].Mid(), v()); }; };
    auto notify = [&d](int i) { return [&d, i] { const auto q = d.NotifyRect(i); return POINT{ d.winX + (LONG)((q.left + q.right) * 0.5f), d.winY + (LONG)((q.top + q.bottom) * 0.5f) }; }; };
    auto under = [&d](int k) { return [&d, k] { return d.ScreenPointUV(d.cells[k].Mid(), 0.5f); }; };
    auto edgeAt = [&d, edge](float fr) { return [&d, edge, fr] { return EdgePoint(d.mon, (Edge)edge, fr); }; };
    auto button = [&d](ButtonId b) { return [&d, b] { return ButtonPoint(d, b); }; };
    auto underButton = [&d](ButtonId b) { return [&d, b] { return d.ScreenPointUV(d.Cell_(b).Mid(), 0.5f); }; };
    const POINT middle = { (m.rect.left + m.rect.right) / 2, (m.rect.top + m.rect.bottom) / 2 };   // on the screen, far from the dock
    const POINT away = Outside(m.rect);                                                            // off the screen
    auto panelPoint = [&d] { return POINT{ d.winX + (LONG)((d.panelRect.left + d.panelRect.right) * 0.5f), d.winY + (LONG)((d.panelRect.top + d.panelRect.bottom) * 0.5f) }; };
    const int last = (int)d.slots.size() - 1;

    if (kind == Kind::Pass) {
        s.Hold(10, away);
        s.Glide(6, edgeAt(0.5f)); s.Hold(24, edgeAt(0.5f));                     // at the edge: it comes up
        seen.up = d.reveal.target == 1;
        s.Glide(8, slot(1)); s.Hold(6, slot(1));
        s.Glide(40, slot(last)); s.Hold(12, slot(last));                         // along the apps, magnified
        seen.magnified = d.magAmount.target == 1 && d.hover.kind == Dock::HitSlot;
        s.Glide(10, cell(Dock::HitTray)); s.Hold(20, cell(Dock::HitTray));      // off the apps onto the tray: its end held
        seen.trayEnd = d.pinSide == 1 && d.hover.kind == Dock::HitTray;
        s.Glide(6, notify(0)); s.Hold(30, notify(0));                         // a tray icon of the taskbar: its tip
        seen.notifyTip = d.hover.kind == Dock::HitNotify && d.hover.notify == 0 && d.tooltipText == L"Hunt Tray One";
        s.Glide(4, notify(1)); s.Hold(12, notify(1));                         // one without a tip (it opens its own pop-up)
        s.Glide(10, cell(Dock::HitClock)); s.Hold(30, cell(Dock::HitClock));    // the clock's tooltip: the (fixed) date
        seen.dateTip = d.tooltipText == kTraceDate && d.tooltipAlpha.target == 1;
        s.Glide(6, cell(Dock::HitSensor)); s.Hold(30, cell(Dock::HitSensor));   // the sensor cell opens the panel
        seen.panel = d.panelAlpha.target == 1;
        s.Glide(10, panelPoint); s.Hold(20, panelPoint);
        seen.panelHit = d.hover.kind == Dock::HitPanel && d.panelAlpha.target == 1;
        if (d.cells[Dock::HitBattery].u1 > d.cells[Dock::HitBattery].u0) {    // the laptop's battery cell
            s.Glide(6, cell(Dock::HitBattery)); s.Hold(20, cell(Dock::HitBattery));
            seen.batteryTip = d.hover.kind == Dock::HitBattery && !d.tooltipText.empty();
        }
        s.Glide(12, slot(4)); s.Hold(30, slot(4));                               // back on the apps: the panel shuts 0.25 s on
        seen.panelShut = d.panelAlpha.target == 0;
        s.Mouse(WM_MOUSEWHEEL, slot(4)()); s.Hold(2, slot(4));                   // the wheel on an app: nothing
        s.Glide(10, slot(0)); s.Hold(10, slot(0));
        s.Glide(6, [&d, v] { return d.ScreenPointUV(d.barU0 - 8 * d.scale, v()); }); s.Hold(20, [&d, v] { return d.ScreenPointUV(d.barU0 - 8 * d.scale, v()); });
        seen.startEnd = d.pinSide == -1;
        if (!vert) {   // FluentFlyout's slot before the start button, held open by hand (as SideExitShorterTests)
            const float step = 12 * d.scale, lead = std::ceil(200 * d.scale / step) * step + d.gap;
            s.before = [&d, lead] { d.widgetWidth.snap(lead); };
            s.Act("widget slot open");
            s.Glide(8, slot(0)); s.Hold(20, slot(0));
            auto onWidget = [&d, v] { return d.ScreenPointUV((d.widgetU0 + d.widgetU1) * 0.5f, v()); };
            s.Glide(12, onWidget); s.Hold(20, onWidget);
            seen.widget = d.widgetU1 - d.widgetU0 > 150 * d.scale && d.pinSide == -1;
            s.before = nullptr;
            s.Act("widget slot shut");
            s.Hold(40, onWidget);
        } else seen.widget = true;
        s.Glide(10, middle); s.Hold(26, middle);                                 // off the dock: put away (auto-hide)
        s.Glide(4, under(Dock::HitClock)); s.Hold(30, under(Dock::HitClock));    // called back at the edge on its way out
        s.Glide(6, away); s.Hold(80, away);
        seen.hiddenAtEnd = d.reveal.x < 0.002f && d.reveal.target == 0;
        seen.upAtEnd = d.reveal.target == 1 && d.reveal.x > 0.99f;
    } else if (kind == Kind::Press) {
        s.Hold(6, away);
        s.Glide(6, edgeAt(0.5f)); s.Hold(24, edgeAt(0.5f));
        s.Glide(8, slot(2)); s.Hold(12, slot(2));
        POINT p = slot(2)();
        s.Mouse(WM_LBUTTONDOWN, p); s.Hold(3, p);                                // press an app
        const POINT p5 = slot(5)();
        s.Glide(14, p5); s.Hold(10, p5);                                         // moved with the button held: a drag
        s.Glide(14, middle); s.Hold(24, middle);                                 // off the dock (past the unpin line)
        seen.dragOff = d.dragging && d.dropRemove;
        s.Glide(8, away); s.Hold(16, away);                                      // off the screen
        seen.keptUpOffDock = d.dragging && d.reveal.target == 1;
        const int pressedBefore = d.pressSlot;
        s.Act("app 1 closes its last window");
        g_apps.items.erase(g_apps.items.begin()); ++g_apps.structureVersion;     // a ghost before the dragged icon
        s.Hold(40, away);
        seen.ghostShift = pressedBefore == 2 && d.pressSlot == 1 && d.dragging;
        const POINT back = slot(4)();
        s.Glide(16, back); s.Hold(14, back);                                     // back onto the dock, dropped there
        const int version = g_apps.structureVersion;
        s.Mouse(WM_LBUTTONUP, back); s.Hold(30, back);
        seen.dropped = !d.dragging && g_apps.structureVersion == version + 1;
        s.Glide(8, slot(0)); s.Hold(12, slot(0));                                // a click on the start button
        p = slot(0)();
        const int starts = startClicks;
        s.Mouse(WM_LBUTTONDOWN, p); s.Hold(2, p); s.Mouse(WM_LBUTTONUP, p); s.Hold(16, p);
        seen.startClick = startClicks == starts + 1;
        s.Glide(10, button(ButtonId::Theme)); s.Hold(14, button(ButtonId::Theme));
        p = ButtonPoint(d, ButtonId::Theme);
        const int toggles = themeToggles;
        s.Mouse(WM_LBUTTONDOWN, p);                                              // a button pressed, carried off and back
        s.Glide(10, middle); s.Hold(40, middle);
        s.Glide(6, underButton(ButtonId::Theme)); s.Hold(24, underButton(ButtonId::Theme));
        s.Glide(6, button(ButtonId::Theme)); s.Hold(12, button(ButtonId::Theme));
        p = ButtonPoint(d, ButtonId::Theme);
        s.Mouse(WM_LBUTTONUP, p); s.Hold(16, p);
        seen.buttonClick = themeToggles == toggles + 1;
        s.Glide(8, slot(3)); s.Hold(10, slot(3));                                // a press whose button-up never comes
        p = slot(3)();
        s.Mouse(WM_LBUTTONDOWN, p); s.Hold(4, p);
        physicalLeft = false; s.Act("the button is up, no WM_LBUTTONUP");
        s.Hold(130, p);                                                          // given up 2 s on
        s.Glide(6, slot(2)); s.Hold(10, slot(2));                                // ...and a drag's, off the dock
        p = slot(2)();
        s.Mouse(WM_LBUTTONDOWN, p);
        s.Glide(14, middle); s.Hold(10, middle);
        physicalLeft = false; s.Act("the button is up, no WM_LBUTTONUP");
        s.Hold(130, middle);
        seen.givenUp = LogCount(L"never came: given up");
        s.Glide(10, edgeAt(0.5f)); s.Hold(24, edgeAt(0.5f));                     // another window takes the capture
        s.Glide(8, slot(2)); s.Hold(10, slot(2));
        p = slot(2)();
        s.Mouse(WM_LBUTTONDOWN, p); s.Hold(3, p);
        s.Act("WM_CAPTURECHANGED"); captureOwner = nullptr; d.WndProc(WM_CAPTURECHANGED, 0, 0);
        physicalLeft = false; s.Hold(10, p);
        seen.lostCapture = LogCount(L"lost the mouse capture during a press");
        s.Glide(8, away); s.Hold(80, away);
        seen.hiddenAtEnd = d.reveal.x < 0.002f && d.reveal.target == 0;
        seen.upAtEnd = d.reveal.target == 1 && d.reveal.x > 0.99f;
    } else if (kind == Kind::Front) {
        s.Hold(10, away);
        s.Glide(6, edgeAt(0.5f)); s.Hold(24, edgeAt(0.5f));
        s.Glide(8, slot(3)); s.Hold(10, slot(3));
        s.Act("a fullscreen game in front");                                      // a game on this screen
        fgOn = true; fgMonitor = m.handle; fgRect = m.rect; fgStyle = WS_POPUP; fgZoomed = false;
        s.Hold(40, slot(3));
        seen.gameAway = d.FullscreenHere() && d.reveal.target == 0;
        s.Glide(6, edgeAt(0.5f)); s.Hold(30, edgeAt(0.5f));
        seen.gameEdgeNo = d.reveal.target == 0;
        // the game goes and an app's maximized window comes to the front in the same frame: an always-shown dock
        // comes back up and, in that frame, looks at the new window over its bar (KeepInFront after the show/hide)
        s.Act("the game gone, another window in front, maximized over the bar");
        fgWindow = kFg2; fgStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_MAXIMIZE; fgZoomed = true; traceAbove = true;
        fgRect = { m.rect.left - 8, m.rect.top - 8, m.rect.right + 8, m.rect.bottom + 8 };
        s.Hold(1, edgeAt(0.5f));
        seen.raisedAtOnce = d.frontRaises == 1;   // in that frame (auto-hide too: the pointer waited at the edge)
        s.Hold(29, edgeAt(0.5f));
        seen.raisedAtOnce = seen.raisedAtOnce && d.frontRaises == 2;   // and again at the second look
        s.Act("that window gone"); fgOn = false; traceAbove = false; fgWindow = kFg;
        s.Hold(10, edgeAt(0.5f));
        s.Glide(8, slot(3)); s.Hold(10, slot(3));
        s.Act("a maximized window over the bar");                                 // KeepInFront: raised over it
        fgOn = true; fgStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_MAXIMIZE; fgZoomed = true; traceAbove = true;
        fgRect = { m.rect.left - 8, m.rect.top - 8, m.rect.right + 8, m.rect.bottom + 8 };
        const int raisedBefore = d.frontRaises;
        s.Hold(40, slot(3));
        seen.raised = d.frontRaises == raisedBefore + 2;
        s.Act("the window gone"); fgOn = false; traceAbove = false;
        s.Glide(8, away); s.Hold(40, away);
        s.Act("Start opens");                                                     // a shell pop-up in front
        fgOn = true; tracePopup = true; fgStyle = WS_POPUP; fgZoomed = false;
        fgRect = { m.rect.left + 10, m.rect.bottom - 700, m.rect.left + 600, m.rect.bottom - 100 };
        s.Hold(30, away);
        seen.popupUp = d.reveal.target == 1;
        s.Act("Start closes"); fgOn = false; tracePopup = false;
        s.Hold(40, away);
        s.Act("settings window open"); g_settingsHold.open = true;                // the settings window holds it up
        s.Hold(30, away);
        seen.settingsUp = d.reveal.target == 1;
        s.Act("settings window closed"); g_settingsHold.open = false;
        s.Hold(40, away);
        s.Act("HotkeyReveal"); d.HotkeyReveal(simTime);                           // the shortcut
        s.Hold(30, away);
        seen.hotkeyUp = d.reveal.target == 1;
        s.Act("HotkeyReveal"); d.HotkeyReveal(simTime);
        s.Hold(30, away);
        seen.hotkeyAway = d.reveal.target == 0;
        s.Act("attention: app 2");                                                // a notification bounce (the primary dock)
        g_apps.attentionEvents.push_back({ 2 });
        s.Hold(1, away);
        g_apps.attentionEvents.clear();
        bool peek = false;
        for (int i = 0; i < 100; ++i) { s.Hold(1, away); peek |= d.slots[2].peek.x > 0.5f; }
        seen.peek = peek || !d.primary || !autoHide;
        s.Act("overheat"); g_sensors.alertPending = true;                         // the sensor cell's hops
        s.Hold(1, away);
        g_sensors.alertPending = false;
        bool hop = false;
        for (int i = 0; i < 110; ++i) { s.Hold(1, away); hop |= d.sbOff > 1; }
        seen.sensorHop = hop || !d.primary;
        s.Act("app 3 launching");                                                 // launch hops until a window shows
        g_apps.items[2]->launchStarted = simTime; g_apps.items[2]->launchDock = 0;
        s.Glide(6, edgeAt(0.5f)); s.Hold(24, edgeAt(0.5f));
        hop = false;
        for (int i = 0; i < 40; ++i) { s.Hold(1, edgeAt(0.5f)); hop |= d.slots[3].bounceOff > 1; }
        seen.launchHop = hop;
        s.Act("app 3 has its window"); g_apps.items[2]->launchStarted = 0;
        s.Hold(50, edgeAt(0.5f));
        s.Glide(6, away); s.Hold(80, away);
        seen.hiddenAtEnd = d.reveal.x < 0.002f && d.reveal.target == 0;
        seen.upAtEnd = d.reveal.target == 1 && d.reveal.x > 0.99f;
    } else if (kind == Kind::Edges) {
        // the edges of what the other scripts pass through, on a clock of 1/64 s (the delays come out exact): when the
        // dock shows, hides and shuts its panel, the pre-warm distance, a drag's start 1 px at a time, hover cleared
        // in one frame, the tray icons' mouse, presses let go elsewhere, an unpinned app's drag, bounces started again,
        // quiet, what holds a hidden dock up, FluentFlyout's widget slot (WidgetState), KeepInFront's rounding
        s.dt = 1.f / 64;
        using trayhost::Mouse;
        auto inward = [&d, edge](int px) { return [&d, edge, px] {
            POINT q = EdgePoint(d.mon, (Edge)edge, 0.5f);
            switch ((Edge)edge) { case Edge::Bottom: q.y -= px; break; case Edge::Top: q.y += px; break; case Edge::Left: q.x += px; break; default: q.x -= px; }
            return q; }; };
        auto wanted = [&d] { return simTime - d.lastCaptureWanted < 0.05; };
        auto upAgain = [&] { s.Glide(4, edgeAt(0.5f)); s.Hold(24, edgeAt(0.5f)); };
        auto slotOf = [&d, v](int id) { return [&d, v, id] { const int k = std::max(0, SlotOf(d, id)); return d.ScreenPointUV(d.slots[k].u, v()); }; };
        auto attention = [&](int id, POINT at) {
            s.Act("attention: app " + std::to_string(id));
            g_apps.attentionEvents.push_back({ id }); s.Hold(1, at); g_apps.attentionEvents.clear();
        };
        s.Hold(6, away);
        // 1. pre-warm: the hidden dock wants the capture within 60 px (times the scale) of its edge
        for (int px : { 50, 70, 85, 95 }) { s.Hold(6, inward(px)); seen.prewarm += wanted() ? '1' : '0'; }
        // 2. at the edge: up when showDelay has passed (>=), 4 frames after the first frame there
        for (int i = 0; i < 12; ++i) { s.Hold(1, edgeAt(0.5f)); if (autoHide && seen.showFrames < 0 && d.reveal.target > 0.5f) seen.showFrames = i; }
        s.Hold(20, edgeAt(0.5f));
        // 3. the panel shuts more than 0.25 s after the pointer left it: on the 17th frame
        s.Glide(6, cell(Dock::HitSensor)); s.Hold(30, cell(Dock::HitSensor));
        seen.panelOpen = d.panelAlpha.target == 1;
        for (int i = 0, first = -1; i < 30; ++i) {
            s.Hold(1, slot(4));
            if (first < 0 && d.panelOutsideSince >= 0) first = i;
            if (first >= 0 && seen.panelFrames < 0 && d.panelAlpha.target == 0) seen.panelFrames = i - first;
        }
        // 4. off the dock: hidden when hideDelay has passed (>=), 24 frames on
        s.Hold(10, slot(4));
        for (int i = 0, first = -1; i < 40; ++i) {
            s.Hold(1, middle);
            if (first < 0 && d.outsideSince >= 0) first = i;
            if (autoHide && first >= 0 && seen.hideFrames < 0 && d.reveal.target == 0) seen.hideFrames = i - first;
        }
        upAgain();
        // 5. from a small button straight off the screen in one frame: nothing hovered (hover.button too)
        s.Glide(8, button(ButtonId::Theme)); s.Hold(16, button(ButtonId::Theme));
        seen.onButton = d.hover.kind == Dock::HitButton && d.hover.button == ButtonId::Theme;
        s.Hold(1, away);
        seen.buttonCleared = d.hover.kind == Dock::HitNone && d.hover.button == ButtonId::Count && d.hover.slot == -1;
        s.Hold(8, button(ButtonId::Theme));
        // 6. onto a tray icon in one frame: Move at once, Enter 0.4 s on (26 frames), Leave when the pointer leaves
        const int enters = trayhost::sends[(int)Mouse::Enter];
        for (int i = 0, first = -1; i < 40; ++i) {
            s.Hold(1, notify(0));
            if (first < 0 && d.hover.kind == Dock::HitNotify) first = i;
            if (first >= 0 && seen.enterFrames < 0 && trayhost::sends[(int)Mouse::Enter] > enters) seen.enterFrames = i - first;
        }
        seen.onNotify = d.hover.kind == Dock::HitNotify && d.hover.notify == 0;
        const int leaves = trayhost::sends[(int)Mouse::Leave];
        s.Hold(1, away);
        seen.notifyCleared = d.hover.kind == Dock::HitNone && d.hover.notify == -1 && trayhost::sends[(int)Mouse::Leave] == leaves + 1;
        s.Hold(8, notify(0));
        // 7. the tray icons' clicks go to their programs: left (one: a second would depend on the real double-click
        // time), right, middle
        const int l0 = trayhost::sends[(int)Mouse::LeftClick], r0 = trayhost::sends[(int)Mouse::RightClick], m0 = trayhost::sends[(int)Mouse::MiddleClick];
        POINT p = notify(0)();
        s.Mouse(WM_LBUTTONDOWN, p); s.Hold(2, p); s.Mouse(WM_LBUTTONUP, p); s.Hold(6, p);
        s.Glide(4, notify(1)); s.Hold(8, notify(1));
        p = notify(1)(); s.Mouse(WM_RBUTTONUP, p); s.Hold(6, p);
        s.Glide(4, notify(0)); s.Hold(8, notify(0));
        p = notify(0)();
        const bool midOk = MiddleClickSafe(d, p);
        if (midOk) s.Mouse(WM_MBUTTONUP, p); else s.Act("no middle click here");
        s.Hold(6, p);
        seen.trayClicks = trayhost::sends[(int)Mouse::LeftClick] == l0 + 1 && trayhost::sends[(int)Mouse::RightClick] == r0 + 1 &&
                          trayhost::sends[(int)Mouse::MiddleClick] == m0 + (midOk ? 1 : 0) && trayhost::sends[(int)Mouse::LeftDoubleClick] == 0;
        // 8. a press let go over something else: the start button over an app, a button over another button - no click
        s.Glide(10, slot(0)); s.Hold(10, slot(0));
        const int starts = startClicks, clicks0 = Clicks();
        p = slot(0)(); s.Mouse(WM_LBUTTONDOWN, p); s.Glide(8, slot(1)); s.Hold(4, slot(1));
        p = slot(1)(); s.Mouse(WM_LBUTTONUP, p); s.Hold(10, p);
        seen.otherSlot = startClicks == starts && Clicks() == clicks0 && !d.dragging;
        s.Glide(10, button(ButtonId::Theme)); s.Hold(10, button(ButtonId::Theme));
        const int toggles = themeToggles, desks = nativetray::desktopToggles;
        p = ButtonPoint(d, ButtonId::Theme); s.Mouse(WM_LBUTTONDOWN, p);
        s.Glide(6, button(ButtonId::Desktop)); s.Hold(6, button(ButtonId::Desktop));
        p = ButtonPoint(d, ButtonId::Desktop); s.Mouse(WM_LBUTTONUP, p); s.Hold(10, p);
        seen.otherButton = themeToggles == toggles && nativetray::desktopToggles == desks && d.hover.button == ButtonId::Desktop;
        // 9. a drag starts more than 6 px (times the scale) from the press: 1 px a frame along the dock
        s.Glide(10, slot(2)); s.Hold(10, slot(2));
        p = slot(2)(); s.Mouse(WM_LBUTTONDOWN, p); s.Hold(2, p);
        for (int i = 1; i <= 12; ++i) {
            s.Hold(1, vert ? POINT{ p.x, p.y + i } : POINT{ p.x + i, p.y });
            if (seen.dragPx < 0 && d.dragging) seen.dragPx = i;
        }
        p = cursor; s.Mouse(WM_LBUTTONUP, p); s.Hold(20, p);
        // 10. an app that is not pinned, dragged past the unpin line: nothing to unpin (no remove)
        s.Act("app 6 not pinned"); Item(6)->pinned = false; ++g_apps.structureVersion;
        s.Hold(4, cursor);
        s.Glide(8, slotOf(6)); s.Hold(10, slotOf(6));
        p = slotOf(6)(); s.Mouse(WM_LBUTTONDOWN, p); s.Glide(14, middle); s.Hold(10, middle);
        seen.unpinnedKept = d.dragging && !d.dropRemove;
        s.Glide(14, slot(3)); s.Hold(6, slot(3));
        p = slot(3)(); s.Mouse(WM_LBUTTONUP, p); s.Hold(20, p);
        // 11. a notification while the app's hops still play: extended (4 hops in all, not 3 again); one while an app launches
        s.Glide(8, slot(4)); s.Hold(10, slot(4));
        attention(2, slot(4)());
        s.Hold(50, slot(4));
        attention(2, slot(4)());
        if (SlotOf(d, 2) >= 0) seen.bounceExtended = d.slots[SlotOf(d, 2)].bounceCount;
        s.Act("app 4 launching"); Item(4)->launchStarted = simTime; Item(4)->launchDock = 0;
        s.Hold(20, slot(4));
        attention(4, slot(4)());
        if (SlotOf(d, 4) >= 0) { const auto& sl = d.slots[SlotOf(d, 4)]; seen.launchExtended = sl.bounceLoop && (d.primary ? sl.bounceCount == 3 : sl.bounceCount == 0); }
        s.Hold(20, slot(4));
        s.Act("app 4 has its window"); Item(4)->launchStarted = 0;
        s.Hold(150, slot(4));
        // 12. the sensor cell's hops start with the dock up: it hides under them without a peek
        s.Act("overheat"); g_sensors.alertPending = true; s.Hold(1, slot(4)); g_sensors.alertPending = false;
        const bool hopping = d.sbCount == 3;
        float peekMax = 0;
        for (int i = 0; i < 100; ++i) { s.Hold(1, middle); peekMax = std::max(peekMax, d.sbPeek.target); }
        seen.sbNoPeek = (hopping || !d.primary) && peekMax == 0 && (!autoHide || d.reveal.target == 0);
        upAgain();
        // 13. quiet: neither a notification nor an overheat hops
        s.Act("quiet on"); features::SetQuiet(1e12);
        g_sensors.alertPending = true; attention(3, edgeAt(0.5f)()); g_sensors.alertPending = false;
        s.Hold(30, edgeAt(0.5f));
        seen.quietNoHop = SlotOf(d, 3) >= 0 && d.slots[SlotOf(d, 3)].bounceCount == 0 && !d.slots[SlotOf(d, 3)].bounceLoop && d.sbCount == 0;
        s.Act("quiet off"); features::SetQuiet(0);
        // 14. the pointer off the dock, an auto-hide dock still held up: by its menu, by a window preview, by a flyout
        s.Act("menu open"); d.menuOpen = true; s.Hold(40, middle);
        seen.menuKept = d.reveal.target == 1;
        s.Act("menu closed"); d.menuOpen = false; s.Hold(2, middle);
        s.Act("the pointer on a window preview"); traceInPreview = true; s.Hold(40, middle);
        seen.previewKept = d.reveal.target == 1;
        traceInPreview = false;
        s.Act("the pointer on a FluentFlyout flyout"); fluentflyout::traceInFlyout = true; s.Hold(40, middle);
        seen.flyoutKept = d.reveal.target == 1;
        fluentflyout::traceInFlyout = false; s.Act("nothing holds it");
        s.Hold(40, middle);
        seen.hidAfter = autoHide ? d.reveal.target == 0 : d.reveal.target == 1;
        upAgain();
        // 15. a window preview showing over an app: no tooltip
        s.Glide(8, slot(2)); s.Hold(24, slot(2));
        seen.tipBefore = d.tooltipAlpha.target == 1;
        s.Act("window preview open"); tracePreviewOpen = true; s.Hold(10, slot(2));
        seen.previewNoTip = d.tooltipAlpha.x == 0 && d.tooltipAlpha.target == 0;
        tracePreviewOpen = false; s.Act("window preview closed"); s.Hold(20, slot(2));
        // 16. FluentFlyout's widget (WidgetState): its slot opens, the host window is placed over it, slides away with
        // the glass and waits where the slot is (an auto-hide dock put away with it open), the slot shuts
        if (!vert) {
            s.Act("FluentFlyout's widget shows");
            fluentflyout::traceWidget.monitor = d.monitor; fluentflyout::traceWidget.open = true; fluentflyout::traceWidget.fitted = true;
            fluentflyout::traceWidget.box = { 20, 2, 20 + (LONG)std::lround(260 * d.scale), (LONG)std::lround(d.barThick) - 2 };
            const int shown0 = trayhost::placeShown, hidden0 = trayhost::placeHidden;
            s.Hold(40, slot(1));
            auto onWidget = [&d, v] { return d.ScreenPointUV((d.widgetU0 + d.widgetU1) * 0.5f, v()); };
            s.Glide(10, onWidget); s.Hold(20, onWidget);
            const bool showing = d.widgetShowing;
            s.Glide(6, middle); s.Hold(50, middle);
            s.Act("its widget hides"); fluentflyout::traceWidget.open = false; s.Hold(30, middle);
            fluentflyout::traceWidget = {};
            seen.widgetPlaced = showing && trayhost::placeShown > shown0 && (autoHide ? trayhost::placeHidden > hidden0 : true) && !d.widgetShowing;
            upAgain();
        } else seen.widgetPlaced = trayhost::placeShown == 0 && trayhost::placeHidden == 0;   // side docks hold no widget
        // 17. KeepInFront looks at the bar rounded outward: a window in front that ends on the bar's outermost pixel
        // covers it (raised over, twice), one that ends a pixel short does not - for each of the bar's four sides
        s.Glide(8, slot(3)); s.Hold(16, slot(3));
        const auto b = d.RectUV(d.barU0, d.barU1, d.margin, d.margin + d.barThick);
        const RECT bar = { d.winX + (LONG)std::floor(b.left), d.winY + (LONG)std::floor(b.top), d.winX + (LONG)std::ceil(b.right), d.winY + (LONG)std::ceil(b.bottom) };
        const RECT big = { m.rect.left - 8, m.rect.top - 8, m.rect.right + 8, m.rect.bottom + 8 };
        auto overBar = [&](const char* what, RECT r) {
            s.Act(std::string("a window in front, ") + what + " " + TraceRect(r));
            fgOn = true; fgWindow = kFg2; fgStyle = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_MAXIMIZE; fgZoomed = true; traceAbove = true; fgRect = r;
            const int before = d.frontRaises;
            s.Hold(30, slot(3));
            const int n = d.frontRaises - before;
            s.Act("that window gone"); fgOn = false; traceAbove = false; fgWindow = kFg;
            s.Hold(4, slot(3));
            seen.frontRound += char('0' + std::min(n, 9));
        };
        overBar("right on the bar's", { big.left, big.top, bar.right, big.bottom });
        overBar("right a pixel short", { big.left, big.top, bar.right - 1, big.bottom });
        overBar("bottom on the bar's", { big.left, big.top, big.right, bar.bottom });
        overBar("bottom a pixel short", { big.left, big.top, big.right, bar.bottom - 1 });
        overBar("left on the bar's", { bar.left, big.top, big.right, big.bottom });
        overBar("left a pixel short", { bar.left + 1, big.top, big.right, big.bottom });
        overBar("top on the bar's", { big.left, bar.top, big.right, big.bottom });
        overBar("top a pixel short", { big.left, bar.top + 1, big.right, big.bottom });
        s.Glide(6, away); s.Hold(80, away);
        seen.hiddenAtEnd = d.reveal.x < 0.002f && d.reveal.target == 0;
        seen.upAtEnd = d.reveal.target == 1 && d.reveal.x > 0.99f;
    } else {
        // the press safety's 2 s on a clock where they come out exact (1/64 s frames): given up after it, not at it
        s.dt = 1.f / 64;
        s.Hold(6, away);
        s.Glide(6, edgeAt(0.5f)); s.Hold(24, edgeAt(0.5f));
        s.Glide(8, slot(3)); s.Hold(10, slot(3));
        const POINT p = slot(3)();
        s.Mouse(WM_LBUTTONDOWN, p); s.Hold(4, p);
        physicalLeft = false; s.Act("the button is up, no WM_LBUTTONUP");
        for (int i = 0; i < 160; ++i) {
            s.Hold(1, p);
            if (seen.upFrames < 0 && LogCount(L"never came: given up")) seen.upFrames = i;   // frames after the first one that saw it up
        }
        seen.givenUp = LogCount(L"never came: given up");
        s.Glide(6, away); s.Hold(80, away);
        seen.hiddenAtEnd = d.reveal.x < 0.002f && d.reveal.target == 0;
        seen.upAtEnd = d.reveal.target == 1 && d.reveal.x > 0.99f;
    }
    traceEvents = nullptr;
    traceDockWindow = nullptr;
    dock.reset();
    Dock::isShellPopup = keepPopup; Dock::raiseOver = keepRaise;
    g_settingsHold.open = false; fgOn = false; fgWindow = kFg; traceAbove = tracePopup = false; traceDate = false;
    captureOwner = currentDock = nullptr; physicalLeft = false;
    g_sys.SetBattery({});
    fluentflyout::traceWidget = {}; fluentflyout::traceInFlyout = false; traceInPreview = tracePreviewOpen = false; features::SetQuiet(0);
    traceFly.clear(); tracePreview.clear(); tracePlace.clear();
    if (write) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe; path = path.substr(0, path.find_last_of(L'\\')) + L"\\trace-" + std::wstring(name.begin(), name.end()) + L".txt";
        FILE* fp = nullptr;
        if (!_wfopen_s(&fp, path.c_str(), L"wb") && fp) { fwrite(r.file.data(), 1, r.file.size(), fp); fclose(fp); }
        printf("HASH: dock_trace/%s %016llx\n", name.c_str(), (unsigned long long)r.hash);
    }
    return r.hash;
}
}

static void FrameTraceTests() {
    using namespace frametrace;
    wchar_t env[8] = {};
    writeExact = GetEnvironmentVariableW(L"LD_TRACE_EXACT", env, 8) > 0 && env[0] == L'1';
    struct Tally { int scenes = 0, ok = 0; std::string failed; };
    Tally pass[2], press[2], front[2], safety[2], edges[2];
    uint64_t hashes[4][4][kKinds][2] = {};
    // edges64's numbers, written out: where a drag starts (px from the press, by screen: the dock's scale 1.25, 1.0, 1.0,
    // 1.5 - more than 6 px times the scale), when the dock shows, shuts its panel, hides, tells a tray icon Enter (frames
    // on the 1/64 s clock), what a hidden dock pre-warms (50, 70, 85, 95 px from its edge: within 60 px times the scale)
    static const int kDragPx[4] = { 8, 7, 7, 10 };
    static const char* const kPrewarm[4] = { "1100", "1000", "1000", "1110" };
    struct Numbers { int scenes = 0, ok = 0; std::string failed; void Add(bool ok_, const std::string& where, const std::string& got) {
        ++scenes; if (ok_) ++ok; else failed += " " + where + "(" + got + ")"; } };
    Numbers dragAt[4], showAt, panelAt, hideAt, enterAt, prewarmAt, roundAt;
    for (int screen = 0; screen < 4; ++screen) for (int edge = 0; edge < 4; ++edge) for (int k = 0; k < kKinds; ++k) for (int hide = 1; hide >= 0; --hide) {
        Seen seen;
        hashes[screen][edge][k][hide] = RunScene(screen, edge, (Kind)k, hide == 1, seen, true);
        const std::string where = std::string(kScreenNames[screen]) + "-" + kEdgeNames[edge];
        const bool parked = hide ? seen.hiddenAtEnd : seen.upAtEnd;   // auto-hide: away at the end; else still up
        std::vector<std::pair<const char*, bool>> want;
        Tally* t = nullptr;
        if ((Kind)k == Kind::Pass) {
            t = &pass[hide];
            want = { { "up", seen.up }, { "magnified", seen.magnified }, { "trayEnd", seen.trayEnd }, { "notifyTip", seen.notifyTip }, { "dateTip", seen.dateTip },
                     { "panel", seen.panel }, { "panelHit", seen.panelHit }, { "panelShut", seen.panelShut }, { "startEnd", seen.startEnd },
                     { "widget", seen.widget }, { "batteryTip", seen.batteryTip || screen != 0 }, { "parked", parked } };
        } else if ((Kind)k == Kind::Press) {
            t = &press[hide];
            want = { { "dragOff", seen.dragOff }, { "keptUpOffDock", seen.keptUpOffDock }, { "ghostShift", seen.ghostShift },
                     { "dropped", seen.dropped }, { "startClick", seen.startClick }, { "buttonClick", seen.buttonClick },
                     { "givenUp", seen.givenUp == 2 }, { "lostCapture", seen.lostCapture == 1 }, { "parked", parked } };
        } else if ((Kind)k == Kind::Safety) {
            t = &safety[hide];
            want = { { "upFrames", seen.upFrames == 129 }, { "givenUp", seen.givenUp == 1 }, { "parked", parked } };
        } else if ((Kind)k == Kind::Edges) {
            t = &edges[hide];
            const bool primary = kScreens[screen]->primary;
            want = { { "panelOpen", seen.panelOpen }, { "onButton", seen.onButton }, { "buttonCleared", seen.buttonCleared }, { "onNotify", seen.onNotify },
                     { "notifyCleared", seen.notifyCleared }, { "trayClicks", seen.trayClicks }, { "otherSlot", seen.otherSlot }, { "otherButton", seen.otherButton },
                     { "unpinnedKept", seen.unpinnedKept }, { "bounceExtended", seen.bounceExtended == (primary ? 4 : 0) }, { "launchExtended", seen.launchExtended },
                     { "sbNoPeek", seen.sbNoPeek }, { "quietNoHop", seen.quietNoHop }, { "menuKept", seen.menuKept }, { "previewKept", seen.previewKept },
                     { "flyoutKept", seen.flyoutKept }, { "hidAfter", seen.hidAfter }, { "tipBefore", seen.tipBefore }, { "previewNoTip", seen.previewNoTip },
                     { "widgetPlaced", seen.widgetPlaced }, { "parked", parked } };
            dragAt[screen].Add(seen.dragPx == kDragPx[screen], where + (hide ? "-hide" : "-shown"), std::to_string(seen.dragPx));
            panelAt.Add(seen.panelFrames == 17, where + (hide ? "-hide" : "-shown"), std::to_string(seen.panelFrames));
            enterAt.Add(seen.enterFrames == 26, where + (hide ? "-hide" : "-shown"), std::to_string(seen.enterFrames));
            roundAt.Add(seen.frontRound == "20202020", where + (hide ? "-hide" : "-shown"), seen.frontRound);
            if (hide) {
                showAt.Add(seen.showFrames == 4, where, std::to_string(seen.showFrames));
                hideAt.Add(seen.hideFrames == 24, where, std::to_string(seen.hideFrames));
                prewarmAt.Add(seen.prewarm == kPrewarm[screen], where, seen.prewarm);
            }
        } else {
            t = &front[hide];
            want = { { "gameAway", seen.gameAway }, { "gameEdgeNo", seen.gameEdgeNo }, { "raisedAtOnce", seen.raisedAtOnce }, { "raised", seen.raised }, { "popupUp", seen.popupUp },
                     { "settingsUp", seen.settingsUp }, { "hotkeyUp", seen.hotkeyUp }, { "hotkeyAway", seen.hotkeyAway || !hide },
                     { "peek", seen.peek }, { "sensorHop", seen.sensorHop }, { "launchHop", seen.launchHop }, { "parked", parked } };
        }
        std::string missing;
        for (auto& [what, ok] : want) if (!ok) missing += (missing.empty() ? "" : ",") + std::string(what);
        ++t->scenes; if (missing.empty()) ++t->ok; else t->failed += " " + where + "(" + missing + ")";
    }
    const char* modes[2] = { "auto-hide off", "auto-hide on" };
    for (int hide = 1; hide >= 0; --hide) {
        Check(pass[hide].ok == pass[hide].scenes && pass[hide].scenes == 16, (std::string("frame trace, ") + modes[hide] +
              ": the pointer brings the dock up, magnifies the apps, holds the tray end and the start end, shows a tray icon's tip and the clock's date, opens the panel and shuts it, on every screen and edge" +
              pass[hide].failed).c_str());
        Check(press[hide].ok == press[hide].scenes && press[hide].scenes == 16, (std::string("frame trace, ") + modes[hide] +
              ": a drag off the dock keeps it up and drops back on it, a ghost before the dragged icon moves its number, a start and a button click count once, two button-ups that never came are given up, a lost capture is logged" +
              press[hide].failed).c_str());
        Check(front[hide].ok == front[hide].scenes && front[hide].scenes == 16, (std::string("frame trace, ") + modes[hide] +
              ": a fullscreen game puts the dock away, a window over the bar is raised over (at once where the dock comes back up with it), twice, Start, the settings window and the shortcut bring it up, the hops play" +
              front[hide].failed).c_str());
        Check(safety[hide].ok == safety[hide].scenes && safety[hide].scenes == 16, (std::string("frame trace, ") + modes[hide] +
              ": on a clock of 1/64 s a press whose button-up never comes is given up on the frame after exactly 2 s, not on it" +
              safety[hide].failed).c_str());
    }
    printf("HASH: dock_trace/exact-all %016llx\n", (unsigned long long)exactAll);
    // two scenes again, after other scenes than in the loop: the same traces (nothing left over from the scene before)
    Seen again;
    const uint64_t a = RunScene(0, 0, Kind::Press, true, again, false), c = RunScene(3, 2, Kind::Front, false, again, false);
    Check(a == hashes[0][0][(int)Kind::Press][1] && c == hashes[3][2][(int)Kind::Front][0],
          "frame trace: a scene run again after other scenes gives the same trace");
    // edges64
    for (int hide = 1; hide >= 0; --hide)
        Check(edges[hide].ok == edges[hide].scenes && edges[hide].scenes == 16, (std::string("frame trace, ") + modes[hide] +
              ": edges64 - hover cleared in one frame, the tray icons' mouse goes to their programs, a press let go over another app or button clicks nothing, "
              "an unpinned app's drag removes nothing, a notification during hops extends them, quiet holds hops back, a menu, a preview and a flyout hold the dock up, "
              "a preview takes the tooltip, FluentFlyout's widget slot opens and its window is placed" + edges[hide].failed).c_str());
    for (int screen = 0; screen < 4; ++screen)
        Check(dragAt[screen].ok == dragAt[screen].scenes && dragAt[screen].scenes == 8, (std::string("frame trace, edges64: on ") + kScreens[screen]->name +
              " a pressed app becomes a drag " + std::to_string(kDragPx[screen]) + " px from the press (1 px a frame along the dock), at every edge" + dragAt[screen].failed).c_str());
    Check(showAt.ok == showAt.scenes && showAt.scenes == 16, ("frame trace, edges64: showDelay 0.0625 s - an auto-hide dock comes up on the 4th frame (1/64 s) at its edge, not the 5th" + showAt.failed).c_str());
    Check(hideAt.ok == hideAt.scenes && hideAt.scenes == 16, ("frame trace, edges64: hideDelay 0.375 s - put away on the 24th frame off the dock, not the 25th" + hideAt.failed).c_str());
    Check(panelAt.ok == panelAt.scenes && panelAt.scenes == 32, ("frame trace, edges64: the panel shuts on the 17th frame (more than 0.25 s) after the pointer left it, not the 16th" + panelAt.failed).c_str());
    Check(enterAt.ok == enterAt.scenes && enterAt.scenes == 32, ("frame trace, edges64: a tray icon hears Enter 26 frames (0.40625 s, at least 0.4 s) after the pointer came onto it" + enterAt.failed).c_str());
    Check(prewarmAt.ok == prewarmAt.scenes && prewarmAt.scenes == 16, ("frame trace, edges64: a hidden dock pre-warms its capture within 60 px times its scale of the edge (50/70/85/95 px: laptop 1100, desktop and portrait 1000, 3840x2160 1110)" + prewarmAt.failed).c_str());
    Check(roundAt.ok == roundAt.scenes && roundAt.scenes == 32, ("frame trace, edges64: KeepInFront takes the bar rounded outward - a window ending on its outermost pixel is raised over (twice), one a pixel short is not, on all four sides" + roundAt.failed).c_str());
    const uint64_t e = RunScene(1, 1, Kind::Edges, true, again, false);
    Check(e == hashes[1][1][(int)Kind::Edges][1], "frame trace: an edges64 scene run again after other scenes gives the same trace");
}

int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    if (!g_gfx.Init()) { puts("GPU initialization failed"); return 2; }   // text measuring (DirectWrite) for the layout
    g_sys.timeText = L"21:30"; g_sys.timeShort = L"21:30"; g_sys.dateText = L"2026-10-28";
    GeometryTests();
    SettingsHoldTests();
    TwoMonitorTests();
    SensorRedrawTests();
    SensorSettingsTests();
    ZoomedFullscreenTests();
    AppBarTests();
    AppBarWorkAreaTests();
    DragTests();
    ClickTests();
    SideExitTests();
    SideExitShorterTests();
    FrameTraceTests();
    return Summary("dock_layout_test");
}
