// The production settings window (settings_window*.cpp) on the real Direct3D / Direct2D device, driven by
// window messages sent straight to its (never shown) window: the mouse wheel, clicks on its controls, a
// slider drag and a move to a monitor of another resolution. The fixture is the one of
// tests/dock_interaction_test.cpp (stubs for the shell, OLED, theme, audio, Start and the taskbar; fake
// monitors), plus stubs for ShowWindow, SetForegroundWindow, SetCapture and TrackPopupMenu: no input is
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
static BOOL WINAPI MatrixPopupMenu(HMENU, UINT, int, int, int, HWND, const RECT*) { ++popupMenus; return 0; }
static HWND captured = nullptr;                                    // never take the real mouse
static HWND WINAPI MatrixSetCapture(HWND h) { HWND was = captured; captured = h; return was; }
static BOOL WINAPI MatrixReleaseCapture() { captured = nullptr; return TRUE; }
namespace nativetray { static bool MatrixToggleDesktop() { return true; } }
// OLED mode stubs: never the real wallpaper or desktop icons
static bool oledOn = false;
namespace oled {
bool Active() { return oledOn; }
bool CoversMonitor(HMONITOR) { return oledOn; }
bool Enable(HMONITOR) { oledOn = true; return true; }
bool Disable() { oledOn = false; return true; }
void Reapply() {}
}
static bool themeLight = false;
namespace theme {
bool IsLight() { return themeLight; }
bool Toggle() { themeLight = !themeLight; return true; }
}
static HINSTANCE WINAPI MatrixShellExecute(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) { return (HINSTANCE)42; }
static int MatrixFileOperation(LPSHFILEOPSTRUCTW) { return 0; }
static HRESULT WINAPI MatrixEmptyRecycleBin(HWND, LPCWSTR, DWORD) { return S_OK; }
#include "../src/audio_devices.h"
namespace audio {
std::vector<Device> Outputs() { return { { L"a", L"Speakers", true }, { L"b", L"Headset", false } }; }
bool SetDefault(const std::wstring&) { return true; }
std::wstring DefaultOutputName() { return L"Speakers"; }
}
#include "../src/start_menu.h"
namespace startmenu {
bool InFront() { return false; }
void Click(HMONITOR, bool) {}
}
#include "../src/taskbar_start.h"
namespace taskbarstart { void Remember(HWND) {} }
#include "../src/taskbar_apps.h"
namespace taskbarapps {
bool Begin(const Job&) { return false; }
bool Take(uint32_t, Result&) { return false; }
const wchar_t* OutcomeText(Outcome) { return L""; }
}
#define ShellExecuteW MatrixShellExecute
#define SHFileOperationW MatrixFileOperation
#define SHEmptyRecycleBinW MatrixEmptyRecycleBin
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
#include "../src/dock.cpp"
#include "../src/dock_window.cpp"
#include "../src/dock_input.cpp"
#include "../src/dock_layout.cpp"
#include "../src/dock_render.cpp"
#include "../src/dock_sensors.cpp"
#include "../src/dock_controls.cpp"
#include "../src/dock_notify.cpp"
static int presents = 0;   // frames the settings window has shown (its only Present call)
#define Present(sync, flags) Present((++presents, (sync)), (flags))
#include "../src/settings_window.cpp"
#include "../src/settings_window_pages.cpp"
#include "../src/settings_window_customizer.cpp"
#undef Present
#undef ShellExecuteW
#undef SHFileOperationW
#undef SHEmptyRecycleBinW
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
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static void Drain() {   // count (and drop) what the settings window posted to the docks
    MSG msg;
    while (PeekMessageW(&msg, g_mainWnd, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_APP_REBUILD) ++rebuilds;
        else if (msg.message == WM_APP_RELAYOUT) ++relayouts;
        else if (msg.message == WM_APP_TRAYHOST) ++trayHosts;
    }
}
static void ResetCounts() { Drain(); rebuilds = relayouts = trayHosts = 0; }
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
    Check(Dock::settingsOpen, "the docks are told the settings are open");
    LONG_PTR st = GetWindowLongPtrW(sw::g_wnd, GWL_STYLE);
    Check(!(st & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) && (st & WS_SYSMENU), "only a close button: no minimize or maximize box");
}

// Win+D (or the dock's show-desktop button) still minimizes a window without a minimize box: the settings window
// then closes - saved, the docks let go - instead of staying open off the screen and holding every dock up.
static void MinimizeCloses() {
    sw::Show(kLaptop);
    Check(sw::IsOpen() && Dock::settingsOpen, "open again for the minimize check");
    if (!sw::IsOpen()) return;
    Unsave();
    SendMessageW(sw::g_wnd, WM_SIZE, SIZE_MINIMIZED, 0);
    Check(!Dock::settingsOpen, "minimized: the docks are let go at once");
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Check(!sw::IsOpen() && !Dock::settingsOpen && Saved() && !sw::g_swap, "minimized: it closes - saved, its device resources released");
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

    // the page's reset: the source and the six switches
    g_settings.sensorSource = 0; g_settings.showCpuTemp = false; g_settings.showVramTemp = false; g_settings.showRamTemp = false; g_settings.showVram = false;
    sw::Render();
    ResetCounts();
    if (HotCentre(-2, 0, x, y)) Click(x, y);
    Drain();
    const Settings d;
    Check(g_settings.sensorSource == 1 && g_settings.showCpuTemp && g_settings.showGpu && g_settings.showVram && g_settings.showVramTemp &&
          g_settings.showRamTemp && g_settings.showSsd && rebuilds == 1, "\"기본값으로\" restores the source HWiNFO Gadget and every switch, rebuilt");
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
    Check(!sw::IsOpen() && !Dock::settingsOpen && Saved() && !sw::g_swap, "Esc closes it: saved, docks told, its device resources released");
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
    Unsave();
    DestroyWindow(g_mainWnd);
    return Summary("dock_render_settings_window");
}
