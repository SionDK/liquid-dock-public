// The start button's worker (src/start_menu.cpp) on Windows or Wine. Two modes:
//   start_menu_worker_test.exe          safe (tests\run_regressions.bat): neither the shell nor the taskbar's Start
//                                       button is touched, and no key reaches Windows (keys are only
//                                       counted). What the click's dock.log lines read stays real, and
//                                       only reads: the shell's CoreWindows looked up (FindWindowExW), the
//                                       programs of those and of the window in front opened for their name, age
//                                       and elevation (OpenProcess, QueryFullProcessImageNameW, GetProcessTimes,
//                                       OpenProcessToken), the windows'
//                                       cloak (DwmGetWindowAttribute), the last input (GetLastInputInfo) and
//                                       the monitors (MonitorFromWindow, GetMonitorInfoW). Checks the worker itself: it ends on its own, clears its number,
//                                       logs why Start did not come, takes further clicks without a second key,
//                                       closes Start with one key and no wait (and stops a wait still running),
//                                       hands a click to a waiting worker, starts over after a stuck one.
//   start_menu_worker_test.exe --live   real Windows only (tests\run_start_menu_live.bat): checks that an
//                                       unassigned key gives this background process the right to change the
//                                       front window (what the dock's clicks rely on), then opens and closes the
//                                       real Start menu a few times - by the shell's request (the dock's first way
//                                       since mod.36), then, the shell taken away, by the taskbar's own Start button
//                                       and by the Windows key, each by itself; Start is closed by the Windows key.
//                                       It prints what was not measured when the ladder was written (no check on
//                                       them): the taskbar button's toggle state while Start is closed, open by the
//                                       shell's request and closed again, and the window in front once the key closed
//                                       Start. Hands off the mouse and the keyboard for about 30 seconds.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\start_menu_worker_test.cpp /link user32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib dwmapi.lib
#define SendKeyCombo CountedKeyCombo
#include "../src/start_menu.cpp"
#include "../src/taskbar_start.cpp"
#undef SendKeyCombo
#include <cstdarg>
#include <cstdio>
#include <vector>

// ---- what the worker needs from the rest of the dock (common.cpp, system.cpp), here observable
static std::mutex g_logMu;
static std::vector<std::wstring> g_log;
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_logMu);
    g_log.push_back(buf);
    char utf8[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    printf("  log: %s\n", utf8);
}
#define TEST_SUPPORT_QPC_NOW
#define TEST_SUPPORT_FOREGROUND_RIGHT_TEXT
#include "test_support.h"
static std::atomic<int> g_dataDirAsked{ 0 };
std::wstring AppDataDir() { ++g_dataDirAsked; return L"."; }   // never asked: the start button keeps nothing on disk (mod.36)
static std::atomic<int> g_keys{ 0 }, g_rights{ 0 };
static bool g_realKeys = false;   // --live: the keys really go out (closing Start needs one)
bool CountedKeyCombo(std::initializer_list<WORD> keys) {
    ++g_keys;
    if (!g_realKeys) return true;
    std::vector<INPUT> in;
    for (WORD k : keys) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.end(); it != keys.begin();) { --it; INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i); }
    return SendInput((UINT)in.size(), in.data(), sizeof(INPUT)) == in.size();
}
// shell_front.cpp's TakeForegroundRight, the same steps; in safe mode nothing goes out
ForegroundRight TakeForegroundRight() {
    ++g_rights;
    if (!g_realKeys) return ForegroundRight::Had;
    const DWORD me = GetCurrentProcessId();
    if (AllowSetForegroundWindow(me)) return ForegroundRight::Had;
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const bool inserted = SendInput(2, in, sizeof(INPUT)) == 2;
    if (AllowSetForegroundWindow(me)) return ForegroundRight::Taken;
    return inserted ? ForegroundRight::Refused : ForegroundRight::NotInserted;
}
static const char* RightName(ForegroundRight r) {
    switch (r) {
    case ForegroundRight::Had: return "had it";
    case ForegroundRight::Taken: return "taken with an unassigned key";
    case ForegroundRight::Refused: return "refused even after an unassigned key";
    case ForegroundRight::NotInserted: return "the key was not inserted";
    }
    return "?";
}
static std::string Utf8(const std::wstring& w) {
    char b[1024] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    return b;
}
static std::string FrontExe() {
    const auto f = startmenu::InFrontNow();
    return f.exe.empty() ? std::string("nothing") : Utf8(f.exe);
}
// Start has the keyboard: the window in front is Start's or its search box's (SearchHost.exe on 25H2)
static bool StartHasKeyboard() {
    const auto f = startmenu::InFrontNow();
    return f.start || !_wcsicmp(f.exe.c_str(), L"SearchHost.exe");
}

// ---- checks
#include "portable_check.h"
static const bool kFlushEachCheck = (portable_check::FlushEach() = true);
static bool LogHas(size_t from, const wchar_t* text) {
    std::lock_guard<std::mutex> lk(g_logMu);
    for (size_t i = from; i < g_log.size(); ++i) if (g_log[i].find(text) != std::wstring::npos) return true;
    return false;
}
static size_t LogSize() { std::lock_guard<std::mutex> lk(g_logMu); return g_log.size(); }
static bool WaitIdle(double seconds) {
    const double until = NowSeconds() + seconds;
    while (NowSeconds() < until) {
        if (startmenu::g_current.load() == 0) return true;
        Sleep(20);
    }
    return startmenu::g_current.load() == 0;
}
static const int kAllWays = startmenu::kWayShell | startmenu::kWayTaskbar | startmenu::kWayKey;

static void SafeTests(HMONITOR primary) {
    // neither the shell nor the taskbar's Start button is touched here: no real Start can open (the key is only counted)
    startmenu::g_ways = startmenu::kWayKey;
    const double wait = startmenu::kGiveUp + 2;

    int keys = g_keys; size_t log = LogSize();
    startmenu::Click(primary, false);
    Check(startmenu::g_current.load() != 0, "a click starts a worker (the UI thread does not wait)");
    Check(WaitIdle(wait), "the worker ends by itself (nothing brings Start here: it gives up)");
    Check(g_keys - keys == 1, "...after pressing the Windows key once");
    Check(LogHas(log, L"start: Start did not come in") && LogHas(log, L"Windows key: pressed"),
          "dock.log says Start did not come, and that the key was pressed");
    Check(LogHas(log, L"shell: not available"), "...and that the shell was not asked");
    Check(LogHas(log, L"taskbar: off"), "...nor the taskbar's Start button");
    Check(!taskbarstart::TaskbarOn(nullptr), "no monitor: no taskbar to press (the click goes the other ways)");

    keys = g_keys; log = LogSize();
    startmenu::Click(primary, false); Sleep(150);
    startmenu::Click(primary, false); Sleep(150);
    startmenu::Click(primary, false);
    Check(WaitIdle(wait), "three clicks: the first one's worker ends");
    Check(g_keys - keys == 1, "three clicks while Start is on its way: one key (a second would close it again)");
    Check(LogHas(log, L"2 more clicks"), "dock.log counts the two clicks made while waiting");

    keys = g_keys; log = LogSize();
    startmenu::Click(primary, true);
    Check(g_keys - keys == 1 && startmenu::g_current.load() == 0, "Start in front at the press: one key closes it, no worker");
    Check(LogHas(log, L"start: closing Start (the start button pressed while it was open; in front: ") && LogHas(log, L") by the Windows key"),
          "...and dock.log says Start was closed, how, and what was in front");
    Check(g_rights.load() >= 1, "an opening click takes the right to change the front window before it hands it to the shell");

    {   // Start closed with the button while a wait still runs: that wait must not bring it back
        keys = g_keys;
        startmenu::Click(primary, false);
        Sleep(200);
        const double t0 = NowSeconds();
        startmenu::Click(primary, true);
        const bool ended = WaitIdle(1.5);
        Check(ended && NowSeconds() - t0 < 1.0, "Start closed with the button while a wait runs: that wait ends at once");
        Check(g_keys - keys == 2, "...one key for the click, one to close Start, none after");
    }

    keys = g_keys;
    startmenu::g_current = 0xFFFF0001u;   // a worker that is waiting, not stuck
    startmenu::g_since = GetTickCount64();
    startmenu::g_again = false;
    startmenu::Click(primary, false);
    Check(startmenu::g_current.load() == 0xFFFF0001u && g_keys == keys && startmenu::g_again.load(),
          "a click while a worker waits goes to that worker (no new worker, no key)");

    log = LogSize();
    startmenu::g_since = GetTickCount64() - static_cast<uint64_t>((startmenu::kStuck + 1) * 1000);   // stuck on Explorer
    startmenu::Click(primary, false);
    Check(LogHas(log, L"starting over") && startmenu::g_current.load() != 0xFFFF0001u, "a click after kStuck leaves the stuck worker behind and starts over");
    Check(WaitIdle(wait), "the new worker ends");
    Check(g_dataDirAsked == 0, "in all of it the dock's data folder was never asked for: no verdict is kept on disk (data\\taskbar-start.txt is left as it is)");
    startmenu::g_ways = kAllWays;
}

// The Windows build and whether this run is elevated: the two runs (as the user, as administrator) differ
// (an elevated process may not reach Explorer's shell objects; the dock itself runs as the user).
static void PrintRun() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = { sizeof(v) };
    if (auto f = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) f(&v);
    printf("  Windows build %lu; this test runs %s\n", v.dwBuildNumber,
           startmenu::SelfElevated() ? "as administrator (the dock itself runs as the user: the run without administrator is the one that counts)" : "as the user, as the dock does");
}

// The taskbar's Start button for this monitor, as the worker looks it up (on a thread of the MTA, as there).
static taskbarstart::Found FindTaskbarButton(HMONITOR m) {
    taskbarstart::Found f = taskbarstart::Found::NoButton;
    std::thread([&] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        f = taskbarstart::Find(m);
        if (SUCCEEDED(co)) CoUninitialize();
    }).join();
    return f;
}

// ---- measures (--live; printed, never a check). The taskbar Start button's toggle state right now, read only -
// nothing is pressed: "off", "on", "indeterminate", or why it could not be read. (The ladder presses the button only
// when it reads off; whether it reads on while Start is open by the shell's request was not measured when it was written.)
static std::string StartToggleNow(HMONITOR m) {
    std::string text = "?";
    std::thread([&] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const taskbarstart::Found f = taskbarstart::Find(m);
        if (f != taskbarstart::Found::Kept && f != taskbarstart::Found::Now) {
            text = "no button to read (" + Utf8(taskbarstart::FoundText(f)) + ")";
        } else {
            ComPtr<IUIAutomationElement> button;
            {
                std::lock_guard<std::mutex> lk(taskbarstart::S().book);
                button = taskbarstart::At(taskbarstart::TaskbarOn(m)).button;
            }
            if (!button) text = "the button went away meanwhile";
            else {
                std::lock_guard<std::mutex> call(taskbarstart::S().calls);
                ComPtr<IUIAutomationTogglePattern> toggle;
                const HRESULT hr = button->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle));
                if (FAILED(hr) || !toggle) { char b[64]; snprintf(b, sizeof(b), "no Toggle pattern (%08lx)", (unsigned long)hr); text = b; }
                else {
                    double took = 0; bool timedOut = false;
                    const int state = taskbarstart::StateOf(toggle.Get(), &took, &timedOut);
                    char b[96];
                    snprintf(b, sizeof(b), "%s (read in %.0f ms%s)", Utf8(startmenu::ToggleText(state)).c_str(), took * 1000, timedOut ? ", timed out" : "");
                    text = b;
                }
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).join();
    return text;
}
// The window in front as dock.log names one: program, class, whether it is cloaked (DWM) or hidden, its monitor.
static std::string FrontNow() {
    const HWND fg = GetForegroundWindow();
    if (!fg) return "nothing";
    wchar_t cls[96] = L"";
    GetClassNameW(fg, cls, ARRAYSIZE(cls));
    DWORD cloak = 0;
    DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloak, sizeof(cloak));
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi);
    char b[400];
    snprintf(b, sizeof(b), "%s %s, cloak %lu%s, %s, on monitor %ld,%ld", FrontExe().c_str(), Utf8(cls).c_str(), cloak,
             cloak ? " (cloaked)" : "", IsWindowVisible(fg) ? "visible" : "hidden", mi.rcMonitor.left, mi.rcMonitor.top);
    return b;
}

static void LiveTests(HMONITOR primary) {
    PrintRun();
    g_realKeys = true;
    {   // what the dock's app icons and start button rely on: this process is not the one in front (its
        // window belongs to the terminal), and an unassigned key gives it the right to change that
        HWND before = GetForegroundWindow();
        HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"Liquid Dock test", WS_POPUP | WS_BORDER,
                                 60, 60, 260, 60, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(w, SW_SHOWNOACTIVATE);
        const ForegroundRight right = TakeForegroundRight();
        SetForegroundWindow(w);
        Sleep(150);
        const bool came = GetForegroundWindow() == w;
        printf("  the right to change the front window: %s\n", RightName(right));
        Check(came && (right == ForegroundRight::Had || right == ForegroundRight::Taken),
              "an unassigned key gives a background process the right to put its window in front");
        DestroyWindow(w);
        if (before && IsWindow(before)) { TakeForegroundRight(); SetForegroundWindow(before); }
        Sleep(150);
    }
    ComPtr<startmenu::IVisibility> vis;
    CoCreateInstance(startmenu::kAppVisibility, nullptr, CLSCTX_ALL, startmenu::kAppVisibilityIid, reinterpret_cast<void**>(vis.GetAddressOf()));
    auto open = [&] { return startmenu::Visible(vis.Get(), nullptr); };
    auto settle = [&](bool want, double seconds) {
        const double until = NowSeconds() + seconds;
        while (NowSeconds() < until && open() != want) Sleep(30);
        return open() == want;
    };
    // a click with Start open closes it: the Windows key (mod.36: never the taskbar's button)
    auto close = [&] {
        if (!startmenu::InFront()) return false;   // (a click without Start open would open it)
        const int keys = g_keys;
        const size_t log = LogSize();
        startmenu::Click(primary, true);
        return settle(false, 2) && g_keys == keys + 1 && LogHas(log, L") by the Windows key");
    };
    startmenu::g_ways = kAllWays;
    if (open()) { startmenu::Click(primary, true); settle(false, 2); }
    Check(!open(), "Start closed before the test");

    // The taskbar's own Start button (src/taskbar_start.cpp): the ladder's second way, behind the shell's request. With
    // the dock running, its taskbar is hidden: this shows whether the button is found then, and what its state reads.
    const double lookT = NowSeconds();
    const taskbarstart::Found found = FindTaskbarButton(primary);
    const bool button = found == taskbarstart::Found::Kept || found == taskbarstart::Found::Now;
    printf("  the taskbar's Start button: %s, in %.0f ms (%s)\n", Utf8(taskbarstart::FoundText(found)).c_str(), (NowSeconds() - lookT) * 1000,
           Utf8(taskbarstart::Describe()).c_str());
    printf("  measure: Start closed - the taskbar button's toggle state reads %s\n", StartToggleNow(primary).c_str());

    // ---- 1. as the dock starts: the shell's request (the first way since mod.36)
    printf("  in front before the shell's part: %s\n", FrontExe().c_str());
    int keys = g_keys; size_t log = LogSize();
    startmenu::Click(primary, false);
    WaitIdle(10);
    Check(settle(true, 1), "a click opens Start");
    Check(LogHas(log, L"Start came by the shell's request"), "...by the shell's request (dock.log names it)");
    Check(g_keys == keys, "...without the Windows key");
    Sleep(200);
    Check(StartHasKeyboard(), "...and Start has the keyboard (typing goes to its search box)");
    printf("  measure: Start open by the shell's request - the taskbar button's toggle state reads %s\n", StartToggleNow(primary).c_str());
    printf("  measure: Start open by the shell's request - in front: %s\n", FrontNow().c_str());

    Check(startmenu::InFront(), "Start open: the press-time check sees it (what decides that the button closes it)");
    Check(close(), "a click with Start open closes it, by the Windows key (dock.log says so, with what was in front)");
    Sleep(300);
    printf("  measure: Start closed by the Windows key, 0.3 s later - in front: %s\n", FrontNow().c_str());
    printf("  measure: Start closed by the Windows key - the taskbar button's toggle state reads %s\n", StartToggleNow(primary).c_str());
    Sleep(200);   // (Start closed by the key, the next request half a second later: what the dock does on a quick reopen)

    keys = g_keys;
    for (int i = 0; i < 3; ++i) { startmenu::Click(primary, false); Sleep(80); }
    WaitIdle(10);
    Check(settle(true, 1), "three quick clicks: Start is open (the extra clicks did not toggle it shut)");
    Check(g_keys == keys, "...and no Windows key was pressed");
    Check(close(), "...and a click closes it again");
    Sleep(500);

    // ---- 2. the shell taken away: the taskbar's own Start button by itself (pressed only when its state reads off)
    if (button) {
        startmenu::g_ways = startmenu::kWayTaskbar;
        keys = g_keys; log = LogSize();
        startmenu::Click(primary, false);
        WaitIdle(10);
        Check(settle(true, 1), "the shell taken away: a click opens Start");
        Check(LogHas(log, L"by the taskbar's Start button - taskbar: pressed (toggle, state off -> "), "...by the taskbar's own Start button, its state off before the press (dock.log names both)");
        Check(g_keys == keys, "...without the Windows key");
        Sleep(200);
        Check(StartHasKeyboard(), "...and Start has the keyboard (typing goes to its search box)");
        printf("  measure: Start open by the taskbar's button - its toggle state reads %s\n", StartToggleNow(primary).c_str());
        startmenu::g_ways = kAllWays;
        Check(close(), "...and a click closes it, by the Windows key");
        Sleep(300);
        printf("  measure: Start opened by the taskbar's button, closed by the Windows key - the button's toggle state reads %s; in front: %s\n",
               StartToggleNow(primary).c_str(), FrontNow().c_str());
        Sleep(700);
    } else {
        printf("  not checked: no Start button to press on this taskbar - the ladder then goes from the shell to the Windows key, checked below\n");
    }

    // ---- 3. the shell and the button taken away: the Windows key by itself (the last way)
    startmenu::g_ways = startmenu::kWayKey;
    keys = g_keys; log = LogSize();
    startmenu::Click(primary, false);
    WaitIdle(10);
    Check(settle(true, 1), "the shell and the button taken away: a click opens Start");
    Check(g_keys == keys + 1 && LogHas(log, L"by the Windows key"), "...by the Windows key, pressed once (dock.log names it)");
    startmenu::g_ways = kAllWays;
    Check(close(), "...and a click closes it");
    Sleep(1000);

    // ---- 4. another monitor's dock: the shell's request puts Start on that monitor (ConnectToMonitor - not measured before
    // mod.36: the dock pressed that monitor's taskbar button); then that taskbar's own button, the shell taken away
    std::vector<HMONITOR> mons;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(p)->push_back(m); return TRUE;
    }, reinterpret_cast<LPARAM>(&mons));
    for (HMONITOR m : mons) {
        if (m == primary) continue;
        log = LogSize();
        startmenu::Click(m, false);
        WaitIdle(10);
        bool up = settle(true, 1);
        HWND fg = GetForegroundWindow();
        Check(up && fg && MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == m, "the start button of another monitor's dock opens Start on that monitor (as the dock starts: the shell's request)");
        printf("  measure: another monitor's dock - %s%s; in front: %s\n",
               LogHas(log, L"Start came by the shell's request") ? "the shell's request brought Start" : "the shell's request alone did not bring Start (see the lines above)",
               LogHas(log, L"not on the dock's monitor") ? ", NOT on the dock's monitor (Windows chose)" : "", FrontNow().c_str());
        startmenu::Click(m, true);
        settle(false, 2);
        Sleep(1000);
        // Asked once: the first answer can be Now (found, and kept from then on) and the second Kept.
        const taskbarstart::Found there = FindTaskbarButton(m);
        if (there == taskbarstart::Found::Kept || there == taskbarstart::Found::Now) {
            startmenu::g_ways = startmenu::kWayTaskbar;
            startmenu::Click(m, false);
            WaitIdle(10);
            up = settle(true, 1);
            fg = GetForegroundWindow();
            Check(up && fg && MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == m, "the start button of another monitor's dock opens Start on that monitor (the shell taken away: that taskbar's own button)");
            startmenu::g_ways = kAllWays;
            startmenu::Click(m, true);
            settle(false, 2);
        } else {
            printf("  not checked: no Start button to press on that monitor's taskbar\n");
        }
        break;
    }
    startmenu::g_ways = kAllWays;
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool live = argc > 1 && !wcscmp(argv[1], L"--live");
    HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (live) LiveTests(primary);
    else SafeTests(primary);
    Summary(live ? "start_menu_worker_test --live" : "start_menu_worker_test");
    fflush(stdout);
    return portable_check::Failures();
}
