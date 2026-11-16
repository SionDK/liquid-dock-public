// Dock windows, layout, monitors, auto-hide and input on fake test screens: a
// laptop's 2880x1800 panel (150%) and a desktop's 2560x1440 primary with a 1440x2560 portrait monitor
// on its left (negative coordinates). Runs the production Dock (dock.cpp, dock_window.cpp, dock_input.cpp,
// dock_layout.cpp, dock_render.cpp, dock_sensors.cpp, dock_controls.cpp, dock_notify.cpp) with fake monitors, a fake window in front, fake mouse buttons and
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
#include "portable_check.h"
#define private public
#include "../src/apps.h"      // AppModel::presses: an app click that only counts (nothing is started)
#include "../src/sensors.h"
#include "../src/dock.h"
#undef private

Settings g_settings;
HWND g_mainWnd = nullptr;

// ---- stubs for the modules the dock calls (as in dock_interaction_test.cpp): nothing real happens
namespace nativetray { static int desktopToggles = 0; static bool HuntToggleDesktop() { ++desktopToggles; return true; } }
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
namespace settingswindow { void Show(HMONITOR) { ++settingsWindows; } void Close() {} bool IsOpen() { return false; } bool PreTranslate(MSG*) { return false; } }
static int themeToggles = 0; static bool themeLight = false;
namespace theme { bool IsLight() { return themeLight; } bool Toggle() { ++themeToggles; themeLight = !themeLight; return true; } }
#include "../src/audio_devices.h"
static std::wstring speakersName = L"Speakers";
namespace audio {
std::vector<Device> Outputs() { return { { L"a", speakersName, true } }; }
bool SetDefault(const std::wstring&) { return true; }
std::wstring DefaultOutputName() { return L"Speakers"; }
}
#include "../src/start_menu.h"
static int startClicks = 0;
namespace startmenu { bool InFront() { return false; } void Click(HMONITOR, bool) { ++startClicks; } }
#include "../src/taskbar_start.h"
namespace taskbarstart { void Remember(HWND) {} }
#include "../src/taskbar_apps.h"
namespace taskbarapps {
bool Begin(const Job&) { return false; }
bool Take(uint32_t, Result&) { return false; }
const wchar_t* OutcomeText(Outcome) { return L""; }
}

// ---- fake monitors: a laptop's screen and a two-monitor desktop's
struct FakeMonitor { HMONITOR handle; RECT rect; bool primary; const char* name; };
static const FakeMonitor kLaptop = { (HMONITOR)(uintptr_t)0x2001, { 0, 0, 2880, 1800 }, true, "laptop 2880x1800" };
static const FakeMonitor kDesk = { (HMONITOR)(uintptr_t)0x2002, { 0, 0, 2560, 1440 }, true, "desktop 2560x1440" };
static const FakeMonitor kPortrait = { (HMONITOR)(uintptr_t)0x2003, { -1440, -1120, 0, 1440 }, false, "portrait left, bottoms aligned" };
static const FakeMonitor kPortraitTop = { (HMONITOR)(uintptr_t)0x2004, { -1440, 0, 0, 2560 }, false, "portrait left, tops aligned" };
static const FakeMonitor* const kMonitors[] = { &kLaptop, &kDesk, &kPortrait, &kPortraitTop };
static BOOL WINAPI HuntMonitorInfo(HMONITOR m, LPMONITORINFO info) {
    for (auto* f : kMonitors)
        if (f->handle == m) { info->rcMonitor = info->rcWork = f->rect; info->dwFlags = f->primary ? MONITORINFOF_PRIMARY : 0; return TRUE; }
    return ::GetMonitorInfoW(m, info);
}

// ---- the window in front (a game, Start...) on one of the fake monitors
static bool fgOn = false, fgZoomed = false;
static RECT fgRect{};
static HMONITOR fgMonitor = nullptr;
static LONG_PTR fgStyle = WS_POPUP;
static HWND const kFg = (HWND)(uintptr_t)0xF00DF00D;
static HWND WINAPI HuntForeground() { return fgOn ? kFg : nullptr; }
static HMONITOR WINAPI HuntMonitorFromWindow(HWND h, DWORD flags) { return h == kFg ? fgMonitor : ::MonitorFromWindow(h, flags); }
static BOOL WINAPI HuntWindowRect(HWND h, LPRECT r) { if (h == kFg) { *r = fgRect; return TRUE; } return ::GetWindowRect(h, r); }
static BOOL WINAPI HuntIsZoomed(HWND h) { return h == kFg ? fgZoomed : ::IsZoomed(h); }
static LONG_PTR WINAPI HuntWindowLong(HWND h, int i) { return h == kFg ? fgStyle : ::GetWindowLongPtrW(h, i); }
// its client area as GetWindowInfo reports it (a maximized window counts as fullscreen only when that covers the
// monitor); empty unless a check sets it, and fgInfoFails makes the call fail
static RECT fgClient{};
static bool fgInfoFails = false;
static BOOL WINAPI HuntWindowInfo(HWND h, PWINDOWINFO wi) {
    if (h != kFg) return ::GetWindowInfo(h, wi);
    if (fgInfoFails) return FALSE;
    wi->rcWindow = fgRect; wi->rcClient = fgClient; wi->dwStyle = (DWORD)fgStyle;
    return TRUE;
}
static int WINAPI HuntClassName(HWND h, LPWSTR text, int cap) {
    if (h == kFg) { wcsncpy_s(text, cap, L"HuntFullscreen", _TRUNCATE); return (int)wcslen(text); }
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
static HWND WINAPI HuntSetCapture(HWND) { captureOwner = currentDock; return nullptr; }
static BOOL WINAPI HuntReleaseCapture() {
    // as Windows does: the window losing the capture gets WM_CAPTURECHANGED (lParam: nobody) before the call returns
    Dock* d = captureOwner; captureOwner = nullptr;
    if (d) d->WndProc(WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}
static int raises = 0;
static BOOL WINAPI HuntSetWindowPos(HWND, HWND, int, int, int, int, UINT) { ++raises; return TRUE; }
static BOOL WINAPI HuntShowWindow(HWND, int) { return TRUE; }   // the test's windows stay hidden
static BOOL WINAPI HuntSetForeground(HWND) { return TRUE; }
static int menus = 0;
static std::vector<std::wstring> menuItems;   // the last menu's items as Windows would read them (& marks an access key)
static BOOL WINAPI HuntPopupMenu(HMENU m, UINT, int, int, int, HWND, const RECT*) {
    ++menus; menuItems.clear();
    for (int i = 0; i < GetMenuItemCount(m); ++i) {
        wchar_t text[256] = {};
        GetMenuStringW(m, i, text, 256, MF_BYPOSITION);
        menuItems.push_back(text);
    }
    return 0;   // nothing chosen
}
static HINSTANCE WINAPI HuntShellExecute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) { return (HINSTANCE)42; }
static int HuntFileOperation(LPSHFILEOPSTRUCTW) { return 0; }
static HRESULT WINAPI HuntEmptyRecycleBin(HWND, LPCWSTR, DWORD) { return S_OK; }

// ---- the shell's app bar list, as Explorer keeps it (lost when Explorer restarts)
struct FakeShell {
    bool running = true;           // Explorer is there; without it SHAppBarMessage finds no taskbar and fails
    HWND taskbar = (HWND)(uintptr_t)0x7A5C0001;   // Explorer's taskbar window (a new one for every Explorer)
    std::vector<HWND> bars;        // registered app bars
    int news = 0, removes = 0, setPos = 0, unregisteredCalls = 0;   // news: every ABM_NEW call, failed ones too
    RECT lastPos{};
    void Reset() { *this = FakeShell{}; }
    bool Has(HWND h) const { return std::find(bars.begin(), bars.end(), h) != bars.end(); }
} shell;
static UINT_PTR WINAPI HuntAppBar(DWORD msg, PAPPBARDATA abd) {
    if (msg == ABM_NEW) ++shell.news;
    if (!shell.running) return FALSE;
    switch (msg) {
    case ABM_NEW:
        if (shell.Has(abd->hWnd)) return FALSE;   // already registered
        shell.bars.push_back(abd->hWnd); return TRUE;
    case ABM_REMOVE:
        ++shell.removes;
        shell.bars.erase(std::remove(shell.bars.begin(), shell.bars.end(), abd->hWnd), shell.bars.end()); return TRUE;
    case ABM_QUERYPOS:
        if (!shell.Has(abd->hWnd)) { ++shell.unregisteredCalls; return FALSE; }
        return TRUE;
    case ABM_SETPOS:
        if (!shell.Has(abd->hWnd)) { ++shell.unregisteredCalls; return FALSE; }
        ++shell.setPos; shell.lastPos = abd->rc; return TRUE;
    }
    return FALSE;
}
static HWND HuntExplorerTaskbar() { return shell.running ? shell.taskbar : nullptr; }
static void RestartExplorer() {   // the new Explorer has a new taskbar and no app bars
    shell.bars.clear();
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
    Log(L"%ls", buf);
}
static int LogCount(const wchar_t* part) {
    int n = 0;
    for (auto& l : logLines) if (l.find(part) != std::wstring::npos) ++n;
    return n;
}

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
#include "../src/dock.cpp"
#include "../src/dock_window.cpp"
#include "../src/dock_input.cpp"
#include "../src/dock_layout.cpp"
#include "../src/dock_render.cpp"
#include "../src/dock_sensors.cpp"
#include "../src/dock_controls.cpp"
#include "../src/dock_notify.cpp"
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
            hits &= h.kind == Dock::HitSlot && h.slot == i && d->hoverSlot == i;
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
    Dock::settingsOpen = true; Dock::settingsWnd = w;
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
    Dock::settingsWnd = nullptr;
    Advance(*d, away, 0.5);
    Check(d->reveal.target == 1, "open with no window known yet (as before): still holds the dock up");
    Dock::settingsOpen = false;
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

    // nobody sees the screen (locked, the display off): the cell's change waits, one redraw when it is seen
    Dock::screenUnseen = true;
    sample(48.6f, 32);
    const bool unseen1 = frame(), unseen2 = frame();
    sample(49.6f, 32);
    const bool unseen3 = frame();
    Check(!unseen1 && !unseen2 && !unseen3, "sensors: the screen unseen - 48.6 and 49.6 C (49 deg, 50 deg) draw nothing");
    Dock::screenUnseen = false;
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
        d->panelH = std::round(Dock::PanelHeightDip(4, 6, true) * d->scale);
        d->panelVramRoom = d->panelVramTempRoom = true;
        auto rows = d->PanelRowsNow();
        Check(rows.dimmRows == 1 && rows.vram && rows.vramTemp && rows.driveRows == 2 && rows.height == 376,
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
        Check(rows.dimmRows == 1 && rows.vram && rows.vramTemp && rows.driveRows == 0 && rows.drivesHidden == 0 && rows.height == 314,
              "sensors: picture C (CPU 온도, SSD off): 314 DIP, no drive row and none counted as left out");
        items(true, true, true, false, true, true);
        rows = d->PanelRowsNow();
        Check(rows.vram && !rows.vramTemp && rows.height == 349, "sensors: VRAM 온도 off: the block 44 DIP (349)");
        items(true, true, true, true, true, true);
        // the source: off and PawnIO have no module rows (HWiNFO's only) and the VRAM temperature row only from NVML
        for (int source : { 0, 2 }) {
            g_settings.sensorSource = source;
            rows = d->PanelRowsNow();
            const bool without = rows.dimmRows == 0 && rows.vram && !rows.vramTemp && rows.driveRows == 2 && rows.height == 322;
            g_sensors.vramNvmlTempSeen = true;
            rows = d->PanelRowsNow();
            Check(without && rows.vramTemp && rows.height == 349, source == 0 ? "sensors: temperatures off: no module rows; the VRAM temperature row only with NVML's"
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
        // (kNoPointer), and a dock up under the pointer goes away as when it leaves
        p = SlotPoint(*d, 2); Advance(*d, p, 0.5);
        Advance(*d, { -32000, -32000 }, 2.0);
        Check(d->reveal.x < 0.002f && !d->Busy() && d->hoverSlot < 0, Label("no pointer to read: the dock goes away and rests", *m, edge).c_str());

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
        for (int f = 0; f < 96; ++f) { Advance(*d, onClock, 1.0 / 120); track(); hovered &= d->hoverKind == Dock::HitClock; }
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
            for (int f = 0; f < 96; ++f) { Advance(*d, onClock, 1.0 / 120); track(); hovered &= d->hoverKind == Dock::HitClock; }
            const float atRest = d->barU1 - d->barU0;
            g_apps.items.erase(g_apps.items.begin() + 5); ++g_apps.structureVersion;   // AppModel::Refresh: its last window closed
            for (int f = 0; f < 180; ++f) { Advance(*d, onClock, 1.0 / 120); track(); hovered &= d->hoverKind == Dock::HitClock; }
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
    DragTests();
    ClickTests();
    SideExitTests();
    SideExitShorterTests();
    return Summary("dock_layout_test");
}
