// Which of Explorer's taskbars the start button presses for a dock's monitor (src/taskbar_start.cpp: TaskbarOn),
// against a fake window list and fake monitors: no real window, monitor or taskbar is looked at, nothing is pressed,
// UI Automation is never started (only TaskbarOn runs; the taskbar's remembered monitor goes into S().entries
// directly, as Remember would leave it). The rules, as they are (a characterization: what the code does now):
//   1. the primary monitor: Explorer's main taskbar (never its window's position);
//   2. a secondary taskbar remembered on this monitor (seen there expanded): that one, wherever its window lies;
//   3. one remembered on no monitor, or on one that is gone: by its window's monitor (the nearest, lying on none);
//   4. one remembered on another monitor that is still there: never, even when its window lies on this one;
//   and only Explorer's own Shell_SecondaryTrayWnd windows (its main taskbar's process) count, at most 16 of them.
// The monitors are a desktop's (2560x1440 primary, 1440x2560 portrait on its left at -1440,-560) and two
// stacked on its right; the taskbars' thickness follows the scale (1.0, 1.25, 1.5), auto-hidden ones lie mostly
// off their monitor (2 px left on it, scaled).
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\taskbar_start_on_test.cpp /link user32.lib ole32.lib oleaut32.lib uuid.lib
#include "../src/common.h"
#include "../src/taskbar_start.h"
#include <uiautomation.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// ---- the fake window list (front to back) and the fake monitors
struct FakeWnd { HWND h; const wchar_t* cls; DWORD pid; bool host; RECT r; };
struct FakeMon { HMONITOR m; RECT r; bool alive; };
static std::vector<FakeWnd> g_wins;
static std::vector<FakeMon> g_mons;
static DWORD g_shellPid = 0;   // 0: no shell window (Explorer restarting)
static int g_findSecondary = 0, g_fromPoint = 0, g_fromWindow = 0, g_monInfo = 0, g_otherCalls = 0;
static HWND Wnd(uintptr_t n) { return reinterpret_cast<HWND>(n); }
static HMONITOR Mon(uintptr_t n) { return reinterpret_cast<HMONITOR>(n); }
static const FakeWnd* FindWnd(HWND h) { for (const auto& w : g_wins) if (w.h == h) return &w; return nullptr; }
static const FakeMon* FindMon(HMONITOR m) { for (const auto& x : g_mons) if (x.m == m) return &x; return nullptr; }
static const HWND kShellWnd = Wnd(0x9000);

HWND WINAPI FakeShellWindow() { return g_shellPid ? kShellWnd : nullptr; }
static HWND NextOf(HWND after, LPCWSTR cls) {
    bool past = after == nullptr;
    for (const auto& w : g_wins) {
        if (past && !wcscmp(w.cls, cls)) return w.h;
        if (w.h == after) past = true;
    }
    return nullptr;
}
HWND WINAPI FakeFindWindowW(LPCWSTR cls, LPCWSTR) { return cls ? NextOf(nullptr, cls) : nullptr; }
HWND WINAPI FakeFindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR) {
    if (parent || !cls) { ++g_otherCalls; return nullptr; }   // a window's children: none here
    if (!wcscmp(cls, L"Shell_SecondaryTrayWnd")) ++g_findSecondary;
    return NextOf(after, cls);
}
HANDLE WINAPI FakeGetPropW(HWND h, LPCWSTR name) {
    const FakeWnd* w = FindWnd(h);
    return w && w->host && !wcscmp(name, L"LiquidDock.TrayHost") ? reinterpret_cast<HANDLE>(1) : nullptr;
}
DWORD WINAPI FakeGetWindowThreadProcessId(HWND h, LPDWORD pid) {
    DWORD p = h == kShellWnd ? g_shellPid : 0;
    if (const FakeWnd* w = FindWnd(h)) p = w->pid;
    if (pid) *pid = p;
    return p ? 1 : 0;
}
static bool Contains(const RECT& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
HMONITOR WINAPI FakeMonitorFromPoint(POINT p, DWORD flags) {
    ++g_fromPoint;
    for (const auto& x : g_mons) if (x.alive && Contains(x.r, p)) return x.m;
    if (flags == MONITOR_DEFAULTTOPRIMARY)
        for (const auto& x : g_mons) if (x.alive && Contains(x.r, { 0, 0 })) return x.m;
    return nullptr;
}
// as Windows: the monitor the window's rectangle overlaps most; none: the nearest
HMONITOR WINAPI FakeMonitorFromWindow(HWND h, DWORD flags) {
    ++g_fromWindow;
    const FakeWnd* w = FindWnd(h);
    if (!w) return nullptr;
    HMONITOR best = nullptr;
    long long bestArea = 0;
    for (const auto& x : g_mons) {
        if (!x.alive) continue;
        RECT in{};
        if (!IntersectRect(&in, &w->r, &x.r)) continue;
        const long long a = 1LL * (in.right - in.left) * (in.bottom - in.top);
        if (a > bestArea) { bestArea = a; best = x.m; }
    }
    if (best || flags != MONITOR_DEFAULTTONEAREST) return best;
    long long bestD = -1;
    for (const auto& x : g_mons) {
        if (!x.alive) continue;
        const long long dx = (std::max)({ 0L, x.r.left - w->r.right, w->r.left - x.r.right });
        const long long dy = (std::max)({ 0L, x.r.top - w->r.bottom, w->r.top - x.r.bottom });
        const long long d = dx * dx + dy * dy;
        if (bestD < 0 || d < bestD) { bestD = d; best = x.m; }
    }
    return best;
}
BOOL WINAPI FakeGetMonitorInfoW(HMONITOR m, LPMONITORINFO mi) {
    ++g_monInfo;
    const FakeMon* x = FindMon(m);
    if (!x || !x->alive || !mi) return FALSE;
    mi->rcMonitor = x->r;
    mi->rcWork = x->r;
    mi->dwFlags = Contains(x->r, { 0, 0 }) ? MONITORINFOF_PRIMARY : 0;
    return TRUE;
}
#define GetShellWindow FakeShellWindow
#define FindWindowW FakeFindWindowW
#define FindWindowExW FakeFindWindowExW
#define GetPropW FakeGetPropW
#define GetWindowThreadProcessId FakeGetWindowThreadProcessId
#define MonitorFromPoint FakeMonitorFromPoint
#define MonitorFromWindow FakeMonitorFromWindow
#define GetMonitorInfoW FakeGetMonitorInfoW
#include "../src/taskbar_start.cpp"
#undef GetShellWindow
#undef FindWindowW
#undef FindWindowExW
#undef GetPropW
#undef GetWindowThreadProcessId
#undef MonitorFromPoint
#undef MonitorFromWindow
#undef GetMonitorInfoW

// ---- what taskbar_start.cpp needs from the rest of the dock (TaskbarOn uses neither)
void Log(const wchar_t*, ...) {}
double NowSeconds() { return 0; }

// ---- checks
#include "portable_check.h"
static const bool kFlushEachCheck = (portable_check::FlushEach() = true);

using namespace taskbarstart;

// ---- the scene
constexpr DWORD kExplorer = 100, kDock = 200, kRetroBar = 300;
static const HMONITOR kA = Mon(0x100), kB = Mon(0x200), kE = Mon(0x300), kF = Mon(0x400), kGone = Mon(0x500);
static const HWND kHost = Wnd(0x10), kMain = Wnd(0x20), kBarB = Wnd(0x30), kBarE = Wnd(0x40), kBarF = Wnd(0x50), kOther = Wnd(0x60);
static double g_scale = 1.0;
static long Px(double dip) { return static_cast<long>(dip * g_scale + 0.5); }
// a taskbar along a monitor's bottom edge; hidden: auto-hidden, 2 px of it left on its monitor
static RECT BarOn(const RECT& m, bool hidden) {
    const long h = Px(48);
    RECT r = { m.left, m.bottom - h, m.right, m.bottom };
    if (hidden) OffsetRect(&r, 0, h - Px(2));
    return r;
}
static RECT MonRect(HMONITOR m) { const FakeMon* x = FindMon(m); return x ? x->r : RECT{}; }

// A: primary; B: portrait on A's left (a desktop's two monitors); E above F, both on A's right (stacked)
static void Monitors() {
    g_mons = {
        { kA, { 0, 0, 2560, 1440 }, true },
        { kB, { -1440, -560, 0, 2000 }, true },
        { kE, { 2560, -1440, 5120, 0 }, true },
        { kF, { 2560, 0, 5120, 1440 }, true },
        { kGone, { 9000, 0, 10000, 1000 }, false },   // a monitor that left: its handle no longer answers
    };
}
static void Remembered(HWND taskbar, HMONITOR monitor) {
    std::lock_guard<std::mutex> lk(S().book);
    Entry& e = At(taskbar);
    e.monitor = monitor;
}
static void Reset() {
    { std::lock_guard<std::mutex> lk(S().book); S().entries.clear(); }
    Monitors();
    g_shellPid = kExplorer;
    // the dock's own tray host in front of Explorer's main taskbar, as while the dock runs
    g_wins = {
        { kHost, L"Shell_TrayWnd", kDock, true, BarOn(MonRect(kA), false) },
        { kMain, L"Shell_TrayWnd", kExplorer, false, BarOn(MonRect(kA), true) },
        { kBarB, L"Shell_SecondaryTrayWnd", kExplorer, false, BarOn(MonRect(kB), true) },
    };
    g_findSecondary = g_fromPoint = g_fromWindow = g_monInfo = g_otherCalls = 0;
}
static std::string BarName(HWND h) {
    if (!h) return "none";
    if (h == kMain) return "main";
    if (h == kHost) return "host";
    if (h == kBarB) return "B";
    if (h == kBarE) return "E";
    if (h == kBarF) return "F";
    if (h == kOther) return "other";
    const uintptr_t n = reinterpret_cast<uintptr_t>(h);
    if (n >= 0x1000 && n < 0x1100) return "S" + std::to_string(n - 0x1000);
    return "?";
}
// one call: the taskbar and what it asked (secondary windows walked, monitors asked from a window, monitor infos)
static std::string On(HMONITOR m) {
    g_findSecondary = g_fromPoint = g_fromWindow = g_monInfo = g_otherCalls = 0;
    const HWND h = TaskbarOn(m);
    char b[128];
    snprintf(b, sizeof(b), "%s [walk %d, point %d, window %d, info %d]", BarName(h).c_str(), g_findSecondary, g_fromPoint, g_fromWindow, g_monInfo);
    return b;
}
static void Expect(const std::string& got, const char* want, const std::string& what) {
    const bool ok = got == want;
    std::string line = what + " -> " + want;
    Check(ok, line.c_str());
    if (!ok) printf("  found: %s\n", got.c_str());
}

static void Rules(const char* sc) {
    const std::string at = std::string(" (scale ") + sc + ")";

    // ---- nothing to go by
    Reset();
    Expect(On(nullptr), "none [walk 0, point 0, window 0, info 0]", "no monitor: none, nothing asked" + at);
    g_wins.erase(g_wins.begin() + 1);   // Explorer's main taskbar gone (only the dock's tray host left)
    Expect(On(kB), "none [walk 0, point 0, window 0, info 0]", "Explorer's main taskbar not there: none, its secondary ones not looked at" + at);

    // ---- rule 1: the primary monitor is the main taskbar's, whatever lies there
    Reset();
    Expect(On(kA), "main [walk 0, point 1, window 0, info 0]", "rule 1: the primary monitor: Explorer's main taskbar (never the dock's tray host)" + at);
    Remembered(kBarB, kA);   // even a secondary one remembered on it
    g_wins.push_back({ kBarF, L"Shell_SecondaryTrayWnd", kExplorer, false, BarOn(MonRect(kA), false) });
    Expect(On(kA), "main [walk 0, point 1, window 0, info 0]", "...even with a secondary one remembered there and another lying on it" + at);

    // ---- rule 3: nothing remembered: by the window's monitor (the portrait monitor, auto-hidden)
    Reset();
    Expect(On(kB), "B [walk 2, point 1, window 1, info 0]", "rule 3: nothing remembered: the secondary taskbar whose window lies on the monitor" + at);
    Expect(On(kF), "none [walk 2, point 1, window 1, info 0]", "...a monitor without a taskbar of its own: none" + at);

    // ---- rule 2: remembered on this monitor: that one, wherever its window lies
    Reset();
    g_wins.push_back({ kBarE, L"Shell_SecondaryTrayWnd", kExplorer, false, BarOn(MonRect(kE), true) });   // lies mostly on F
    g_wins.push_back({ kBarF, L"Shell_SecondaryTrayWnd", kExplorer, false, BarOn(MonRect(kF), true) });
    Expect(On(kF), "E [walk 4, point 1, window 2, info 0]",
           "stacked, nothing remembered: the upper monitor's auto-hidden taskbar lies on the lower one and is taken for it (first in the list)" + at);
    Expect(On(kE), "none [walk 4, point 1, window 3, info 0]", "...and the upper monitor finds none" + at);
    Remembered(kBarE, kE);
    Remembered(kBarF, kF);
    Expect(On(kE), "E [walk 4, point 1, window 0, info 0]", "rule 2: each remembered on its own monitor: the upper one's, its window's place not asked" + at);
    Expect(On(kF), "F [walk 4, point 1, window 0, info 0]", "...and the lower one's" + at);

    // ---- rule 4: remembered on another monitor still there: never, even when its window lies on this one
    Reset();
    g_wins.push_back({ kBarE, L"Shell_SecondaryTrayWnd", kExplorer, false, BarOn(MonRect(kE), true) });
    g_wins.push_back({ kBarF, L"Shell_SecondaryTrayWnd", kExplorer, false, BarOn(MonRect(kF), true) });
    Remembered(kBarE, kE);   // only the upper one remembered
    Expect(On(kF), "F [walk 4, point 1, window 2, info 1]",
           "rule 4: the upper taskbar remembered on its monitor (still there) is not taken for the lower one its window lies on: the lower one's" + at);
    g_wins.pop_back();   // the lower monitor's own taskbar gone ("show on all displays" changed)
    Expect(On(kF), "none [walk 3, point 1, window 1, info 1]", "...and with no other: none, not the one remembered elsewhere" + at);

    // ---- rule 3 again: remembered on a monitor that is gone: by its window
    Reset();
    Remembered(kBarB, kGone);
    Expect(On(kB), "B [walk 2, point 1, window 1, info 1]", "rule 3: remembered on a monitor that is gone: by its window's monitor" + at);
    Expect(On(kF), "none [walk 2, point 1, window 1, info 1]", "...which is not another monitor's" + at);

    // ---- a taskbar window lying wholly off every monitor (past its own one's outer edge, not even 2 px left on it): by
    // the nearest monitor
    Reset();
    OffsetRect(&g_wins[2].r, 0, Px(2));
    Expect(On(kB), "B [walk 2, point 1, window 1, info 0]", "a secondary taskbar lying wholly off every monitor: by the nearest, its own" + at);
    Expect(On(kF), "none [walk 2, point 1, window 1, info 0]", "...not another one" + at);

    // ---- only Explorer's own secondary taskbars (its main taskbar's process)
    Reset();
    g_wins.insert(g_wins.begin() + 2, { kOther, L"Shell_SecondaryTrayWnd", kRetroBar, false, BarOn(MonRect(kB), false) });
    Expect(On(kB), "B [walk 3, point 1, window 1, info 0]", "another program's secondary taskbar in front on the monitor: skipped, Explorer's" + at);
    Remembered(kOther, kB);
    Expect(On(kB), "B [walk 3, point 1, window 1, info 0]", "...even remembered on that monitor" + at);
    g_wins.erase(g_wins.begin() + 3);   // Explorer's own gone: the other program's alone
    Expect(On(kB), "none [walk 2, point 1, window 0, info 0]", "...and alone: none" + at);
    Reset();
    g_shellPid = 0;   // Explorer restarting (no desktop yet): its main taskbar is the first not the dock's, its process counts
    Expect(On(kB), "B [walk 2, point 1, window 1, info 0]", "no shell window: the main taskbar is still the first not the dock's, its secondary ones count" + at);

    // ---- at most 16 secondary windows walked
    for (int place : { 15, 16 }) {
        Reset();
        g_wins.pop_back();
        for (int i = 0; i < 20; ++i)
            g_wins.push_back({ Wnd(0x1000 + i), L"Shell_SecondaryTrayWnd", kExplorer, false,
                               BarOn(MonRect(i == place ? kB : kF), false) });
        const std::string got = On(kB);
        if (place == 15) Expect(got, "S15 [walk 17, point 1, window 16, info 0]", "twenty secondary windows, the monitor's 16th: found" + at);
        else Expect(got, "none [walk 17, point 1, window 16, info 0]", "...the monitor's 17th: never looked at (16 at most)" + at);
    }
}

int wmain() {
    for (double s : { 1.0, 1.25, 1.5 }) {
        g_scale = s;
        char sc[16];
        snprintf(sc, sizeof(sc), "%.2f", s);
        Rules(sc);
    }
    Summary("taskbar_start_on_test");
    fflush(stdout);
    return portable_check::Failures();
}
