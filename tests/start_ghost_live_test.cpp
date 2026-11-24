// An app icon clicked right after the dock opened Start, live (tests\run_start_ghost_live.bat). A test PC, 2026-11-17
// (a 26H2 build): the dock opened Start with the taskbar's Start button, then eight clicks on Terminal and Chrome came to
// nothing - the window in front was SearchHost's CoreWindow, cloaked, and Windows refused the dock's SetForegroundWindow
// (the right held, attaching refused with 5). Was it the open Start's own window, cloaked, or a closed one keeping the
// front? (First run, 13:5x: an open Start's window in front is NOT cloaked - so a closed one.)
//
// Default: three parts, each with a window of another process (a second copy of this program) behind the console:
//   A  measure: Start opened the dock's way (src\start_menu.cpp: the shell's request since mod.36); what
//      holds the front while it is open (program, class, DWMWA_CLOAKED, IAppVisibility, Start's and Search's
//      CoreWindows); the dock's way to bring the window up while Start is open (apps_click.cpp ForceForeground with
//      both answers); then Esc - only while the window in front is the shell's and Start shows - and the dock's way
//      again once Start has gone.
//   B  Start opened again, then the dock's own code (AppModel::ActivateWindow -> BringUp, CheckWatch looked at every
//      20 ms) brings the window up; its log lines are printed.
//   C  Start closed by a second click on the dock's start button instead - the Windows key since mod.36 (the taskbar's
//      button before, measured on several PCs: no cloaked window left): does a cloaked shell window keep the front then, and
//      does the dock's code bring the window up over it (SwitchToThisWindow tried once only if not, for the record).
// Keys sent: the unassigned key for the right to the front window when it is missing (shell_front.cpp), Esc only as
// above, and the Windows key: once in C (the dock's start button closing Start), and wherever the dock's own ladder or
// the tidying up between parts falls back on it (Start not brought by the shell's request and the taskbar's button
// within 4 s; Start still open with a window in front that is not the shell's). No mouse input. The running dock is not touched (it may come up with Start, as with any Start).
// 'click': that PC's case made with the real mouse (see the click mode below and run_start_ghost_live.bat).
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/admin_helper.h"
#include "../src/system.h"
#include "../src/taskbar_apps.h"
#include <dwmapi.h>
#include <shlobj.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define private public
#include "../src/apps.h"
#undef private

Settings g_settings;
HWND g_mainWnd = nullptr;
Gfx g_gfx;   // never used: no icon is loaded

// ---- the dock's log, kept here to be printed and searched
static std::mutex g_logMu;
static std::vector<std::wstring> g_log;
static void TestLog(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_logMu);
    g_log.push_back(buf);
    char utf8[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    printf("  log: %s\n", utf8);
    fflush(stdout);
}
static size_t LogSize() { std::lock_guard<std::mutex> lk(g_logMu); return g_log.size(); }
static bool LogHas(size_t from, const wchar_t* text) {
    std::lock_guard<std::mutex> lk(g_logMu);
    for (size_t i = from; i < g_log.size(); ++i) if (g_log[i].find(text) != std::wstring::npos) return true;
    return false;
}

// ---- what apps*.cpp and start_menu.cpp need of system.cpp: the same steps (system.cpp pulls in the rest of the dock)
ForegroundRight TakeForegroundRight() {
    const DWORD me = GetCurrentProcessId();
    if (AllowSetForegroundWindow(me)) return ForegroundRight::Had;
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;   // unassigned
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const bool inserted = SendInput(2, in, sizeof(INPUT)) == 2;
    if (AllowSetForegroundWindow(me)) return ForegroundRight::Taken;
    return inserted ? ForegroundRight::Refused : ForegroundRight::NotInserted;
}
const wchar_t* ForegroundRightText(ForegroundRight r) {
    switch (r) {
    case ForegroundRight::Had: return L"had it";
    case ForegroundRight::Taken: return L"taken with an unassigned key";
    case ForegroundRight::Refused: return L"refused even after an unassigned key";
    case ForegroundRight::NotInserted: return L"not taken: the key was not inserted";
    }
    return L"?";
}
static std::atomic<int> g_escs{ 0 };
bool SendKeyCombo(std::initializer_list<WORD> keys) {
    for (WORD k : keys) if (k == VK_ESCAPE) ++g_escs;
    std::vector<INPUT> in;
    for (WORD k : keys) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.end(); it != keys.begin();) { --it; INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i); }
    return SendInput((UINT)in.size(), in.data(), sizeof(INPUT)) == in.size();
}
static std::wstring ExeOfPid(DWORD pid) {
    HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!p) return L"?";
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    std::wstring s = QueryFullProcessImageNameW(p, 0, path, &n) ? path : L"?";
    CloseHandle(p);
    const size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? s : s.substr(slash + 1);
}
static std::wstring ExeOf(HWND h) { DWORD pid = 0; GetWindowThreadProcessId(h, &pid); return ExeOfPid(pid); }
bool IsShellPopupWindow(HWND w) {   // shell_front.cpp's list (without its cache of the last window: looked at each time)
    const std::wstring e = ExeOf(w);
    for (const wchar_t* s : { L"StartMenuExperienceHost.exe", L"SearchHost.exe", L"SearchApp.exe", L"ShellExperienceHost.exe", L"ShellHost.exe" })
        if (!_wcsicmp(e.c_str(), s)) return true;
    return false;
}
namespace adminhelper {   // no helper: the dock's own calls (as on a PC without it)
bool Activate(HWND, const std::wstring&) { return false; }
bool Minimize(HWND, const std::wstring&) { return false; }
bool Close(HWND, const std::wstring&) { return false; }
bool ActivateOver(HWND, const std::wstring&) { return false; }
}
// the taskbar's app buttons: not pressed (off in the settings below)
#include "stub_taskbar_apps.h"
int LauncherVisible();

#define Log(...) TestLog(__VA_ARGS__)
#include "../src/apps.cpp"
#include "../src/apps_pins.cpp"
#include "../src/apps_click.cpp"
#include "../src/apps_press.cpp"
#include "../src/start_menu.cpp"
#include "../src/taskbar_start.cpp"
#undef Log

// shell_front.cpp's, word for word (its declaration of IAppVisibility: src/app_visibility.h, which start_menu.cpp
// brought in). Copied, not included: shell_front.cpp would bring its own SendKeyCombo and IsShellPopupWindow, and this
// program has others (above: Esc counted, the front looked at each time).
int LauncherVisible() {
    static IAppVisibilityDecl* const vis = [] {
        IAppVisibilityDecl* v = nullptr;
        if (FAILED(CoCreateInstance(kAppVisibility, nullptr, CLSCTX_ALL, kAppVisibilityIid, reinterpret_cast<void**>(&v)))) v = nullptr;
        return v;
    }();
    BOOL shown = FALSE;
    if (!vis || FAILED(vis->IsLauncherVisible(&shown))) return -1;
    return shown ? 1 : 0;
}

// ---------------------------------------------------------------- looking
static double T0 = 0;
#define SAY(...) do { printf("%6.2f  ", NowSeconds() - T0); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
// Waits handling this thread's messages (its windows answer, never 'not responding'), instead of Sleep.
static void Pump(double seconds) {
    const double end = NowSeconds() + seconds;
    do {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(10);
    } while (NowSeconds() < end);
}
static std::string U8(const std::wstring& w) {
    char b[1024] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    return b;
}
static DWORD Cloak(HWND h) { DWORD c = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c)); return c; }
static HWND CoreWindowOf(const wchar_t* exe) {   // not in EnumWindows: found by class
    for (HWND w = FindWindowExW(nullptr, nullptr, L"Windows.UI.Core.CoreWindow", nullptr); w;
         w = FindWindowExW(nullptr, w, L"Windows.UI.Core.CoreWindow", nullptr))
        if (!_wcsicmp(ExeOf(w).c_str(), exe)) return w;
    return nullptr;
}
static std::wstring State() {
    HWND fg = GetForegroundWindow();
    wchar_t cls[96] = L"";
    if (fg) GetClassNameW(fg, cls, ARRAYSIZE(cls));
    HWND start = CoreWindowOf(L"StartMenuExperienceHost.exe"), search = CoreWindowOf(L"SearchHost.exe");
    const int l = LauncherVisible();
    wchar_t buf[512];
    swprintf_s(buf, L"front %ls %ls%ls cloak=%lu | Start shown=%ls | Start's CoreWindow cloak=%ls, Search's=%ls",
               fg ? ExeOf(fg).c_str() : L"nothing", cls, fg && !IsWindowVisible(fg) ? L" hidden" : L"", fg ? Cloak(fg) : 0,
               l == 1 ? L"yes" : l == 0 ? L"no" : L"?",
               start ? std::to_wstring(Cloak(start)).c_str() : L"-", search ? std::to_wstring(Cloak(search)).c_str() : L"-");
    return buf;
}
static void Watch(double seconds, double step = 0.05) {   // prints each change
    const double end = NowSeconds() + seconds;
    std::wstring last;
    while (NowSeconds() < end) {
        const std::wstring s = State();
        if (s != last) { SAY("%s", U8(s).c_str()); last = s; }
        Pump(step);
    }
}
static bool ShellFrontCloaked() { HWND fg = GetForegroundWindow(); return fg && IsShellPopupWindow(fg) && Cloak(fg); }

// ---------------------------------------------------------------- the window to bring up: another process
static const wchar_t* kTargetClass = L"LiquidDockStartGhostTarget";
static LRESULT CALLBACK TargetProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);   // closed by hand too: the process ends
    return DefWindowProcW(h, m, w, l);
}
static int Target() {
    WNDCLASSW wc = {}; wc.lpfnWndProc = TargetProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kTargetClass;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, kTargetClass, L"Liquid Dock start-ghost test", WS_OVERLAPPEDWINDOW, 240, 240, 460, 180, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { if (msg.message == WM_CLOSE) break; TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
static bool InFrontIs(HWND h) { HWND fg = GetForegroundWindow(); return fg && (fg == h || GetAncestor(fg, GA_ROOTOWNER) == h); }
// The dock's raw way at a click and its two refocus tries 0.25 s apart (AppModel::Push, CheckWatch)
static bool DockWay(HWND target, const char* what) {
    DWORD tpid = 0; GetWindowThreadProcessId(target, &tpid);
    for (int k = 0; k < 3; ++k) {
        const ForegroundRight r = TakeForegroundRight();
        AllowSetForegroundWindow(tpid);
        const Forced f = ForceForeground(target);
        Pump(0.25);
        const bool came = InFrontIs(target);
        SAY("%s, try %d: right %s; SetForegroundWindow said %d, then %d (error %lu); attach error %lu -> %s", what, k + 1,
            U8(ForegroundRightText(r)).c_str(), f.first, f.second, f.error, f.attachError, came ? "IN FRONT" : "not in front");
        if (came) return true;
    }
    return false;
}

// ---------------------------------------------------------------- click mode: the ghost made as on that PC
// A window like the dock's (WS_EX_NOACTIVATE, MA_NOACTIVATE) clicked with the real mouse while Start is open: Start goes
// at the press, and nothing takes the activation. Its look at the press and at the release, for the record.
static const wchar_t* kPadClass = L"LiquidDockStartGhostPad";
static bool g_padActivates = false;   // the 'activate' way: the click activates it (as the taskbar's does)
static std::string g_atDown, g_atUp;
static LRESULT CALLBACK PadProc(HWND h, UINT m, WPARAM w, LPARAM l);
// The real mouse on the middle of the window - only if the pointer is really there and that point is the window's,
// checked again before the press; a press sent is always followed by its release. false: no click sent.
static bool ClickAt(HWND w) {
    RECT r; GetWindowRect(w, &r);
    const POINT p = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    auto there = [&] { POINT c{}; return GetCursorPos(&c) && c.x == p.x && c.y == p.y && WindowFromPoint(p) == w; };
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) { SAY("click: NOT sent - the left button is held"); return false; }
    if (WindowFromPoint(p) != w) { SAY("click: NOT sent - the point is not this window's (something lies over it)"); return false; }
    POINT was{}; GetCursorPos(&was);
    if (!SetCursorPos(p.x, p.y) || !there()) { SAY("click: NOT sent - the pointer did not get there"); SetCursorPos(was.x, was.y); return false; }
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (SendInput(1, &in[0], sizeof(INPUT)) != 1) { SAY("click: NOT sent - the press was not inserted"); SetCursorPos(was.x, was.y); return false; }
    Pump(0.08);   // as a real click: a moment between press and release
    if (SendInput(1, &in[1], sizeof(INPUT)) != 1 && SendInput(1, &in[1], sizeof(INPUT)) != 1)
        SAY("click: WARNING - the release was not inserted: the left button may be held - click once to free it");
    Pump(0.05);
    SetCursorPos(was.x, was.y);
    return true;
}
static LRESULT CALLBACK PadProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE: return GetWindowLongPtrW(h, GWLP_USERDATA) || g_padActivates ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_LBUTTONDOWN: g_atDown = U8(State()); break;
    case WM_LBUTTONUP: g_atUp = U8(State()); break;
    }
    return DefWindowProcW(h, m, w, l);
}

static int checks = 0, failures = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) ++failures; printf("%s: %s\n", ok ? "PASS" : "FAIL", what); fflush(stdout); }

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && !wcscmp(argv[1], L"--target")) return Target();
    const bool dpiAware = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    T0 = NowSeconds();
    g_settings.taskbarAppButtons = false;   // the dock's own way to bring a window up
    const HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    const HWND home = GetForegroundWindow();   // the console: in front again between the parts
    SAY("in front at the start: %s; per-monitor DPI aware: %s (physical pixels %s)", U8(ExeOf(home)).c_str(), dpiAware ? "yes" : "no",
        dpiAware ? "throughout" : "scaled alike for windows and the pointer");

    g_demoMode = true;   // AppModel::Refresh makes no item (and no icon: g_gfx has no device) for the windows it sees

    // the target process in a job that ends it with this one, whatever happens here
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    }
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" --target";
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) { printf("RESULT error: no target process (%lu)\n", GetLastError()); return 2; }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    struct Find { DWORD pid; HWND h; } find = { pi.dwProcessId, nullptr };   // its own window, not one an earlier run left
    for (int i = 0; i < 100 && !find.h; ++i) {
        Sleep(50);
        EnumWindows([](HWND h, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Find*>(p);
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            wchar_t cls[64] = L""; GetClassNameW(h, cls, ARRAYSIZE(cls));
            if (pid == f->pid && !wcscmp(cls, kTargetClass)) { f->h = h; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&find));
    }
    HWND target = find.h;
    if (!target) { printf("RESULT error: no target window\n"); TerminateProcess(pi.hProcess, 1); return 2; }

    // as the dock starts: every way open to a click (nothing is kept between clicks: there is nothing else to reset)
    auto fresh = [] { startmenu::g_ways = startmenu::kWayShell | startmenu::kWayTaskbar | startmenu::kWayKey; };
    auto idle = [](double s) { const double until = NowSeconds() + s; while (NowSeconds() < until && startmenu::g_current.load()) Pump(0.02); };
    auto settle = [](int want, double s) { const double until = NowSeconds() + s; while (NowSeconds() < until && LauncherVisible() != want) Pump(0.03); return LauncherVisible() == want; };
    // the worker's whole timetable (start_menu.cpp kStuck): none left to press anything once a part goes on
    auto openStart = [&] { startmenu::Click(primary, false); idle(startmenu::kStuck); return settle(1, 1.5); };
    // Esc only as the fix sends it: the shell's window in front and Start shown
    auto escIfOpen = [&] {
        HWND fg = GetForegroundWindow();
        if (fg && IsShellPopupWindow(fg) && LauncherVisible() == 1) { SendKeyCombo({ VK_ESCAPE }); return true; }
        return false;
    };
    auto homeAgain = [&](const char* why) {
        idle(startmenu::kStuck);
        for (int i = 0; i < 2 && LauncherVisible() == 1; ++i) { escIfOpen(); settle(0, 1); }
        for (int i = 0; i < 2 && LauncherVisible() == 1; ++i) { SendKeyCombo({ VK_LWIN }); settle(0, 1.5); }   // the front not the shell's: the Windows key
        SetWindowPos(target, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (home && IsWindow(home)) { TakeForegroundRight(); SetForegroundWindow(home); }
        Pump(0.6);
        SAY("%s: %s", why, U8(State()).c_str());
    };

    fresh();
    if (LauncherVisible() == 1) homeAgain("Start was open before the test, closed");
    Check(LauncherVisible() != 1, "Start closed before the test");
    Check(LauncherVisible() != -1, "IAppVisibility answers on this PC (what the fix asks)");

    if (argc > 1 && !wcscmp(argv[1], L"click")) {
        if (LauncherVisible() == -1) {   // every round would wait on a Start it cannot see, and could leave one open
            printf("RESULT error: IAppVisibility does not answer - click mode not run\n");
            TerminateProcess(pi.hProcess, 0);
            return 2;
        }
        // two windows of this process at the top left, away from Start, the dock and its pop-ups: one like the dock's
        // (no activation), and a pad that activates when clicked - a click on it clears the front after each round
        WNDCLASSW pc = {}; pc.lpfnWndProc = PadProc; pc.hInstance = GetModuleHandleW(nullptr); pc.lpszClassName = kPadClass;
        pc.hbrBackground = (HBRUSH)(COLOR_HIGHLIGHT + 1); pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&pc);
        const LONG_PTR dockEx = WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
        HWND dockLike = CreateWindowExW(dockEx, kPadClass, L"", WS_POPUP, 60, 60, 320, 100, nullptr, nullptr, pc.hInstance, nullptr);
        HWND pad = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPadClass, L"", WS_POPUP, 60, 220, 320, 100, nullptr, nullptr, pc.hInstance, nullptr);
        SetWindowLongPtrW(pad, GWLP_USERDATA, 1);
        ShowWindow(dockLike, SW_SHOWNOACTIVATE);
        ShowWindow(pad, SW_SHOWNOACTIVATE);
        Pump(0.3);
        const char* ways[] = { "none", "esc", "switch", "activate" };
        int rounds = 0, ghosts = 0;
        for (const char* way : ways) {
            printf("---- click, then '%s'\n", way);
            fresh();
            if (!openStart()) { SAY("Start did not open: round skipped"); homeAgain("skipped"); continue; }
            Pump(0.4);
            SAY("before the click: %s", U8(State()).c_str());
            const bool activate = !strcmp(way, "activate");
            const LONG_PTR exNow = GetWindowLongPtrW(dockLike, GWL_EXSTYLE);
            if (activate) { SetWindowLongPtrW(dockLike, GWL_EXSTYLE, exNow & ~WS_EX_NOACTIVATE); g_padActivates = true; }
            g_atDown.clear(); g_atUp.clear();
            const bool clicked = ClickAt(dockLike);
            const bool activated = GetForegroundWindow() == dockLike;
            if (!clicked) {
                if (activate) { SetWindowLongPtrW(dockLike, GWL_EXSTYLE, exNow); g_padActivates = false; }
                printf("RESULT start_ghost_click way=%s clicked=0 (round invalid)\n", way);
                homeAgain("round invalid");
                continue;
            }
            ++rounds;
            Watch(1.0, 0.1);   // a few looks before deciding
            SAY("at the press:   %s", g_atDown.c_str());
            SAY("at the release: %s", g_atUp.c_str());
            const bool frontNull = GetForegroundWindow() == nullptr;
            const bool ghost = ShellFrontCloaked() && LauncherVisible() == 0;
            ghosts += ghost;
            SAY("%s%s", ghost ? "GHOST: Start went at the click, its cloaked window keeps the front (that PC's case)" : "no ghost",
                frontNull ? " (nothing in front)" : "");
            bool came = false;
            if (!strcmp(way, "esc")) {
                HWND fg = GetForegroundWindow();
                const bool sent = fg && IsShellPopupWindow(fg) && SendKeyCombo({ VK_ESCAPE });   // to the shell's window in front only
                Pump(0.5);
                SAY("Esc %s; now %s", sent ? "sent to the shell's window in front" : "NOT sent (the front is not the shell's)", U8(State()).c_str());
            }
            if (!strcmp(way, "switch")) {
                SwitchToThisWindow(target, TRUE);
                Pump(0.4);
                came = InFrontIs(target);
                SAY("SwitchToThisWindow: %s", came ? "IN FRONT" : "not in front");
            } else {
                came = DockWay(target, way);
            }
            if (activate) { SetWindowLongPtrW(dockLike, GWL_EXSTYLE, exNow); g_padActivates = false; }
            printf("RESULT start_ghost_click way=%s clicked=1 pressed=%d ghost=%d front_null=%d activated=%d came=%d\n", way, !g_atDown.empty(),
                   ghost, frontNull, activated, came);
            fflush(stdout);
            // the front cleared: a click on the pad (the system activates it), then the console
            for (int i = 0; i < 2 && ShellFrontCloaked(); ++i) { ClickAt(pad); Pump(0.3); }
            homeAgain("round done");
        }
        Check(rounds == 4, "click: all four rounds ran (Start opened and the click sent each time)");
        printf("RESULT start_ghost_click rounds=%d ghosts=%d\n", rounds, ghosts);
        idle(startmenu::kStuck);
        // nothing left as that PC had it: still the pad to click while it exists, then Esc, then the console
        for (int i = 0; i < 2 && ShellFrontCloaked(); ++i) { ClickAt(pad); Pump(0.3); }
        if (ShellFrontCloaked()) { escIfOpen(); HWND fg = GetForegroundWindow(); if (fg && IsShellPopupWindow(fg)) SendKeyCombo({ VK_ESCAPE }); Pump(0.3); }
        homeAgain("the end");
        Check(LauncherVisible() != 1 && startmenu::g_current.load() == 0, "Start closed at the end, no worker of the start button left");
        Check(!ShellFrontCloaked(), "no cloaked shell window left in front at the end");
        if (ShellFrontCloaked()) printf("GHOST LEFT: click once anywhere on a window of your own (that clears it)\n");
        DestroyWindow(dockLike);
        DestroyWindow(pad);
        PostMessageW(target, WM_CLOSE, 0, 0);
        WaitForSingleObject(pi.hProcess, 3000);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (job) CloseHandle(job);
        printf("start_ghost_live_test click: %s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", checks, failures, failures == 1 ? "" : "s");
        fflush(stdout);
        return failures;
    }

    // ---- A: measure
    printf("---- A: what holds the front while the dock's Start is open\n");
    const bool openedA = openStart();
    Check(openedA, "A: the dock's way opens Start");
    Watch(0.8);
    const bool aCloaked = openedA && ShellFrontCloaked() && LauncherVisible() == 1;
    SAY("A: with Start open the window in front is %s", aCloaked ? "a CLOAKED shell window (that PC's case: an open Start the old rule took for a closed one)"
                                                                 : "not a cloaked shell window");
    const bool refusedOpen = openedA && !DockWay(target, "A: the dock's way while Start is open");
    SAY("A: %s", refusedOpen ? "Windows REFUSED the dock while Start was open (as on that PC)" : "Windows let the window come while Start was open");
    Watch(0.3);
    bool escClosed = false, cameAfterEsc = false;
    if (LauncherVisible() == 1) {
        const bool sent = escIfOpen();
        SAY("A: Esc %s", sent ? "sent (the shell's window in front, Start shown)" : "NOT sent (the window in front is not the shell's)");
        Watch(1.0, 0.1);
        escClosed = sent && LauncherVisible() == 0;
        SAY("A: after the Esc Start is %s", escClosed ? "closed" : "still open");
        if (escClosed) cameAfterEsc = DockWay(target, "A: the dock's way once Start has gone");
    }
    printf("RESULT start_ghost part=A open_front_cloaked=%d refused_while_open=%d esc_closed=%d came_after_esc=%d\n", aCloaked, refusedOpen, escClosed, cameAfterEsc);
    homeAgain("A: done");

    // ---- B: the fix, the dock's own code
    printf("---- B: the dock's code (BringUp, CheckWatch) with the dock's Start open\n");
    fresh();
    const bool openedB = openStart();
    Check(openedB, "B: the dock's way opens Start");
    Watch(0.6);
    const bool bCloaked = openedB && ShellFrontCloaked() && LauncherVisible() == 1;
    bool bCame = false;
    {
        const size_t log = LogSize();
        const int escs = g_escs;
        AppModel m;
        m.Refresh();
        m.ActivateWindow(target);
        const double until = NowSeconds() + 3.0;
        while (NowSeconds() < until && m.watch.window) { m.Tick(NowSeconds()); Sleep(20); }
        Sleep(200);
        bCame = InFrontIs(target);
        // which way the dock's code took, by its own line at the click
        const bool ghostPath = LogHas(log, L"is in front but not shown (a closed Start");
        SAY("B: %s; %d Esc sent; the click took %s; %s", bCame ? "the window came IN FRONT" : "the window did NOT come", g_escs - escs,
            ghostPath ? "mod.25's way (a closed Start keeping the front)" : "the way for a shown pop-up (Esc first) or none", U8(State()).c_str());
        if (openedB && !ghostPath) Check(g_escs - escs >= 1, "B: Start shown in front at the click: closed with Esc first");
        if (openedB) Check(bCame, "B: the dock's code brings the window up with its Start open at the click");
        Check(!LogHas(log, L"could not bring up"), "B: no give-up line");
        printf("RESULT start_ghost part=B open_front_cloaked=%d ghost_path=%d came=%d esc=%d\n", bCloaked, ghostPath, bCame, g_escs - escs);
    }
    homeAgain("B: done");

    // ---- C: Start closed by a second click on the dock's start button (the Windows key since mod.36; the taskbar's
    // button before - not measured with the key until this runs)
    printf("---- C: Start closed by the dock's start button (the Windows key), then the dock's code\n");
    fresh();
    bool ghostLeft = false, cCame = false, switched = false;
    if (openStart()) {
        Sleep(300);
        startmenu::Click(primary, startmenu::InFront());
        settle(0, 2);
        Watch(1.0, 0.1);
        ghostLeft = ShellFrontCloaked() && LauncherVisible() == 0;
        SAY("C: after the close by the Windows key %s", ghostLeft ? "a CLOSED shell window keeps the front (cloaked, Start not shown)" : "no cloaked shell window keeps the front");
        const size_t log = LogSize();
        AppModel m;
        m.Refresh();
        m.ActivateWindow(target);
        const double until = NowSeconds() + 3.0;
        while (NowSeconds() < until && m.watch.window) { m.Tick(NowSeconds()); Sleep(20); }
        Sleep(200);
        cCame = InFrontIs(target);
        SAY("C: the dock's code: %s", cCame ? "the window came IN FRONT" : "the window did NOT come");
        if (!cCame && ghostLeft) {   // for the record only: not what the dock does
            SwitchToThisWindow(target, TRUE);
            Sleep(400);
            switched = InFrontIs(target);
            SAY("C: SwitchToThisWindow: %s", switched ? "IN FRONT" : "not in front");
        }
        if (!ghostLeft) Check(cCame, "C: no closed shell window left in front: the dock's code brings the window up");
        (void)log;
    } else {
        Check(false, "C: the dock's way opens Start");
    }
    printf("RESULT start_ghost part=C closed_shell_window_left=%d came=%d switch=%d\n", ghostLeft, cCame, switched);
    homeAgain("C: done");

    idle(startmenu::kStuck);
    Check(LauncherVisible() != 1 && startmenu::g_current.load() == 0, "Start closed at the end, no worker of the start button left");
    PostMessageW(target, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (job) CloseHandle(job);
    printf("start_ghost_live_test: %s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", checks, failures, failures == 1 ? "" : "s");
    fflush(stdout);
    return failures;
}
