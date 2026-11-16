#include "oled_internal.h"
#include <dwmapi.h>

namespace oled {

// ---------------------------------------------------------------- black covers over live wallpapers
//
// The black wallpaper only shows while nothing draws over the Windows wallpaper. Live-wallpaper apps
// render their own window inside the desktop (on Windows 11 24H2+ a child of Progman between the
// wallpaper and the icons; before that, a WorkerW behind the icons), so in OLED mode every covered
// monitor also gets a black, click-through top-level window placed right above the desktop: at the
// bottom of the z-order, under every normal window. Clicks pass through to the desktop (its menu still
// opens). "Show desktop" (Win+D, the dock's desktop button) lifts the desktop over the open windows;
// the covers then move right above it, and back to the bottom when it ends - the same z-order dance
// desktop widgets such as Rainmeter's "on desktop" skins rely on.

constexpr wchar_t kCoverClass[] = L"LiquidDockOledCover";
constexpr UINT kZFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;

std::vector<HWND> g_covers;
bool g_coversDirty = true;     // (re)build on the next Tick
double g_nextBuild = 0;        // nothing to cover (monitor unplugged): retry later, not every pass
double g_changedAt = 0;        // the last build after a change (Tick's clock): the retries' 2 s spell starts there
constexpr double kRetry = 2.0, kRetrySpell = 30.0, kRetrySlow = 60.0;
double g_nextCheck = 0;        // z-order check pacing
double g_checkUntil = 0;       // after a foreground change: check every pass until then
int g_fixStreak = 0;           // checks in a row that had to move the covers back down
double g_backoffUntil = 0;     // another program keeps forcing a window to the bottom: stop pushing
bool g_raised = false;         // the covers went up with the desktop ("show desktop")
HWINEVENTHOOK g_fgHook = nullptr;
std::vector<HMONITOR> g_coveredMonitors;   // the monitors the mode applies to, as of the last build
bool g_coverageKnown = false;              // false until the first build after a change: every monitor counts
int g_coversSaid = -1;                     // the cover count dock.log had last
DWORD g_coverFailLast = 0;                 // the cover window error the last build met (0: none)

namespace {

LRESULT CALLBACK CoverProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        FillRect((HDC)w, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DPICHANGED: return 0;   // keep the monitor rectangle; display changes rebuild the covers
    }
    return DefWindowProcW(h, m, w, l);
}

bool IsCover(HWND h) { return std::find(g_covers.begin(), g_covers.end(), h) != g_covers.end(); }

// The window holding the desktop icons: Progman itself on Windows 11 24H2+ (or while no live wallpaper
// has split the desktop), otherwise the top-level WorkerW the icons were moved into.
HWND DesktopHost() {
    HWND shell = GetShellWindow();
    if (!shell) return nullptr;   // Explorer is not running
    if (FindWindowExW(shell, nullptr, L"SHELLDLL_DefView", nullptr)) return shell;
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr;)
        if (FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) return w;
    return shell;
}

// Explorer's own desktop windows: Progman (the shell window) and, before 24H2, the visible
// screen-wide WorkerW windows holding the icons and the wallpaper.
bool IsDesktopLayer(HWND h) {
    HWND shell = GetShellWindow();
    if (!shell) return false;
    if (h == shell) return true;
    wchar_t cls[16] = {};
    if (!GetClassNameW(h, cls, 16) || wcscmp(cls, L"WorkerW") != 0 || !IsWindowVisible(h)) return false;
    DWORD a = 0, b = 0;
    GetWindowThreadProcessId(h, &a);
    GetWindowThreadProcessId(shell, &b);
    if (a != b) return false;
    RECT r{};
    GetWindowRect(h, &r);
    return r.left <= GetSystemMetrics(SM_XVIRTUALSCREEN) && r.top <= GetSystemMetrics(SM_YVIRTUALSCREEN) &&
           r.right - r.left >= GetSystemMetrics(SM_CXVIRTUALSCREEN) && r.bottom - r.top >= GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

bool Cloaked(HWND h) {   // other virtual desktops, suspended store apps: in the z-order but not drawn
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c != 0;
}

// A window of another program shown between this cover and the desktop - not minimized, not cloaked,
// overlapping the cover - that the black would hide.
bool WindowBelowCover(HWND cover, HWND host) {
    RECT cr{};
    GetWindowRect(cover, &cr);
    for (HWND w = GetWindow(cover, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == host || IsDesktopLayer(w)) return false;
        if (IsCover(w) || !IsWindowVisible(w) || IsIconic(w) || Cloaked(w)) continue;
        RECT wr{}, both{};
        if (GetWindowRect(w, &wr) && IntersectRect(&both, &wr, &cr)) return true;
    }
    return false;
}

// The desktop lies over visible windows: "show desktop" is still on.
bool DesktopOverWindows(HWND host) {
    for (HWND w = GetWindow(host, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT))
        if (!IsCover(w) && !IsDesktopLayer(w) && IsWindowVisible(w) && !IsIconic(w) && !Cloaked(w)) return true;
    return false;
}

// Resting place: the bottom of the z-order, right above the desktop (Windows keeps the shell's desktop
// windows under HWND_BOTTOM). One check; true when it moved the covers.
bool EnforceZOrder() {
    if (g_covers.empty()) return false;
    for (HWND c : g_covers) if (!IsWindow(c)) { g_coversDirty = true; return false; }
    HWND host = DesktopHost();
    if (!host) return false;   // Explorer is restarting
    // "Show desktop" lifted the desktop over the windows - and over the covers resting at the bottom
    if (FindWindowExW(nullptr, host, kCoverClass, nullptr)) {
        for (HWND c : g_covers) {
            HWND above = GetWindow(host, GW_HWNDPREV);   // insert after it = directly above the desktop
            if (above != c) SetWindowPos(c, above ? above : HWND_TOP, 0, 0, 0, 0, kZFlags);
        }
        g_raised = true;
        return true;
    }
    // Windows that ended up under the covers - the ones "show desktop" brought back, windows restored
    // without being activated, a widget sent to the bottom - go back on top: the covers drop down again.
    if (NowSeconds() >= g_backoffUntil) {
        for (HWND c : g_covers) {
            if (!WindowBelowCover(c, host)) continue;
            for (HWND d : g_covers) SetWindowPos(d, HWND_BOTTOM, 0, 0, 0, 0, kZFlags);
            g_raised = false;
            if (++g_fixStreak >= 20) {   // a program that keeps forcing its window to the very bottom
                g_fixStreak = 0;
                g_backoffUntil = NowSeconds() + 30;
                Log(L"oled: a window keeps taking the bottom of the z-order; covers paused for 30 s");
            }
            return true;
        }
        g_fixStreak = 0;
    }
    if (g_raised && !DesktopOverWindows(host)) g_raised = false;   // went back down with nothing to uncover
    return false;
}

// "Show desktop" and its end both come with a foreground change, and the z-order follows within a few
// ms. Entering or leaving it, wait for that briefly so neither the live wallpaper nor the black flashes.
void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND fg, LONG, LONG, DWORD, DWORD) {
    g_checkUntil = NowSeconds() + 0.5;
    bool toDesktop = fg && (fg == GetShellWindow() || IsDesktopLayer(fg));
    int tries = (toDesktop || g_raised) ? 6 : 1;
    for (int i = 0; i < tries; ++i) {
        if (EnforceZOrder()) break;
        if (i + 1 < tries) Sleep(2);
    }
}

// The monitors the mode applies to: all of them, or the one recorded when it was turned on. looked: the
// record was read and the monitors asked for (for the one monitor, Explorer's wallpaper API answered) -
// nothing found then means none of them is connected.
std::vector<RECT> CoverRects(bool retry, bool& looked) {
    std::vector<RECT> out;
    looked = false;
    State s = Load();
    if (!s.active) return out;
    auto add = [&out](HMONITOR m) {
        MONITORINFO mi = { sizeof(mi) };
        if (m && GetMonitorInfoW(m, &mi)) out.push_back(mi.rcMonitor);
    };
    if (s.allMonitors) {
        std::vector<HMONITOR> mons;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
            ((std::vector<HMONITOR>*)lp)->push_back(m);
            return TRUE;
        }, (LPARAM)&mons);
        for (HMONITOR m : mons) add(m);
        looked = true;
        return out;
    }
    auto w = Wallpaper(retry);
    if (!w) return out;
    looked = true;
    for (auto& [id, original] : s.wallpapers) {
        RECT r{};
        if (SUCCEEDED(w->GetMonitorRECT(id.c_str(), &r)) && r.right > r.left && r.bottom > r.top)
            add(MonitorFromRect(&r, MONITOR_DEFAULTTONULL));   // skip a monitor that is gone
    }
    return out;
}

}   // namespace

void DestroyCovers() {
    for (HWND h : g_covers) DestroyWindow(h);
    g_covers.clear();
    g_coveredMonitors.clear();
    g_coverageKnown = false;
    g_fixStreak = 0;
    g_raised = false;
    if (g_fgHook) { UnhookWinEvent(g_fgHook); g_fgHook = nullptr; }
}

namespace {

// retry: Tick's retry while nothing is covered (a monitor gone, Explorer away), not a change.
// true: nothing to cover because the mode's monitors are not connected (unplugged, switched off)
bool BuildCovers(bool retry) {
    // which monitors are in the mode (their docks stay hidden), even if no cover can be made
    bool looked = false;
    const std::vector<RECT> rects = CoverRects(retry, looked);
    const bool absent = looked && rects.empty();
    g_coveredMonitors.clear();
    for (const RECT& r : rects) g_coveredMonitors.push_back(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST));
    g_coverageKnown = true;
    // a cover that cannot be made (no USER objects or desktop heap left) leaves nothing covered, so the
    // 2 s retry meets it again and again: said when it starts or its error changes, and on every other build
    static bool registered = false;
    static DWORD classFailLast = 0;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = CoverProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = kCoverClass;
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) {
            const DWORD e = GetLastError();
            if (!retry || e != classFailLast) Log(L"oled: cover class failed %lu", e);
            classFailLast = e;
            return absent;
        }
    }
    std::vector<HWND> fresh;
    DWORD failed = 0;
    for (const RECT& r : rects) {
        int x = r.left, y = r.top, cx = r.right - r.left, cy = r.bottom - r.top;
        // layered + transparent: drawn black, but every click goes to whatever lies below
        HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 kCoverClass, L"Liquid Dock OLED", WS_POPUP, x, y, cx, cy,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!h) {
            failed = GetLastError();
            if (!retry || failed != g_coverFailLast) Log(L"oled: cover window failed %lu", failed);
            continue;
        }
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        BOOL on = TRUE;
        DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof(on));   // no fade in or out
        DwmSetWindowAttribute(h, DWMWA_EXCLUDED_FROM_PEEK, &on, sizeof(on));          // stays during "peek at desktop"
        // shown straight at the bottom of the z-order: it never flashes over the open windows
        SetWindowPos(h, HWND_BOTTOM, x, y, cx, cy, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        fresh.push_back(h);
    }
    g_coverFailLast = failed;
    // the new covers are up before the old ones go: no glimpse of the live wallpaper on a rebuild
    for (HWND h : g_covers) DestroyWindow(h);
    g_covers = std::move(fresh);
    g_raised = false;   // fresh covers rest at the bottom
    if (!g_covers.empty() && !g_fgHook)
        g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground,
                                   0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (g_covers.empty() && g_fgHook) { UnhookWinEvent(g_fgHook); g_fgHook = nullptr; }
    // a line for every build but a retry that found what the last one said: dock.log stops for good at 20000 lines
    if (!retry || (int)g_covers.size() != g_coversSaid) Log(L"oled: %d cover(s)", (int)g_covers.size());
    g_coversSaid = (int)g_covers.size();
    EnforceZOrder();
    return absent;
}

}   // namespace

void InvalidateCovers() {
    g_coversDirty = true;
    g_nextBuild = 0;
    g_coverageKnown = false;   // monitor handles may have changed: worked out again on the next Tick
}

bool CoversMonitor(HMONITOR monitor) {
    if (!Active()) return false;
    if (!g_coverageKnown) return true;   // the mode just came on or the monitors changed: hide everywhere until known
    return std::find(g_coveredMonitors.begin(), g_coveredMonitors.end(), monitor) != g_coveredMonitors.end();
}

void Tick(double now) {
    if (!Active()) {
        if (!g_covers.empty()) DestroyCovers();
        g_coversDirty = true;
        return;
    }
    if ((g_coversDirty || g_covers.empty()) && now >= g_nextBuild) {
        const bool retry = !g_coversDirty;
        g_coversDirty = false;
        if (!retry) g_changedAt = now;
        const bool absent = BuildCovers(retry);
        // Nothing to cover right now: looked for again in 2 s. The mode's monitors not connected come back
        // with a display change, which rebuilds the covers at once (InvalidateCovers): after 30 s of that,
        // once a minute - Explorer is not asked every 2 s while a monitor stays switched off. Explorer away
        // or a cover that cannot be made come back with no such event: every 2 s, however long it lasts.
        if (g_covers.empty())
            g_nextBuild = now + (absent && now - g_changedAt >= kRetrySpell ? kRetrySlow : kRetry);
        g_nextCheck = now + 0.1;
        return;
    }
    if (g_covers.empty()) return;
    if (now < g_checkUntil || now >= g_nextCheck) {
        g_nextCheck = now + 0.1;
        EnforceZOrder();
    }
}

}   // namespace oled
