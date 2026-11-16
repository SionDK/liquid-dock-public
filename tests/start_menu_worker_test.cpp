// The start button's worker (src/start_menu.cpp) on Windows or Wine. Two modes:
//   start_menu_worker_test.exe          safe (tests\run_regressions.bat): neither the taskbar's Start button
//                                       nor the shell is touched, and no key reaches Windows (keys are only
//                                       counted). Checks the worker itself: it ends on its own, clears its number,
//                                       logs why Start did not come, takes further clicks without a second key,
//                                       closes Start with one key and no wait (and stops a wait still running),
//                                       hands a click to a waiting worker, starts over after a stuck one.
//   start_menu_worker_test.exe --live   real Windows only (tests\run_start_menu_live.bat): checks that an
//                                       unassigned key gives this background process the right to change the
//                                       front window (what the dock's clicks rely on), then opens and closes the
//                                       real Start menu a few times - by the taskbar's own Start button, then by
//                                       the shell's request as without it. Hands off the mouse and the keyboard
//                                       for about 30 seconds.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\start_menu_worker_test.cpp /link user32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib
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
double NowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}
std::wstring AppDataDir() { return L"."; }   // never asked: the test keeps no verdict of the dock's (g_keepVerdict)
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
// system.cpp's TakeForegroundRight, the same steps; in safe mode nothing goes out
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
static int checks = 0, failures = 0;
static void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
}
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

static void SafeTests(HMONITOR primary) {
    {   // neither the taskbar's Start button nor the shell is touched here: no real Start can open
        std::lock_guard<std::mutex> lk(startmenu::g_mu);
        startmenu::g_pref.askShell = false;
        startmenu::g_pref.shellFirst = false;
        startmenu::g_pref.taskbar = startmenu::TaskbarState::Off;
    }
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

    {   // the taskbar's button has proved itself, but this process keeps none for the monitor (it never looked one
        // up) and nothing of Start is in front: closing goes by the Windows key at once, no lookup, no press
        { std::lock_guard<std::mutex> lk(startmenu::g_mu); startmenu::g_pref.taskbar = startmenu::TaskbarState::Works; }
        keys = g_keys;
        startmenu::Click(primary, true);
        Check(g_keys - keys == 1 && startmenu::g_current.load() == 0 && !taskbarstart::Kept(primary),
              "the button trusted, but none kept for this monitor (and Start's window not in front there): the Windows key closes Start");
        { std::lock_guard<std::mutex> lk(startmenu::g_mu); startmenu::g_pref.taskbar = startmenu::TaskbarState::Off; }
    }

    {   // the taskbar button's verdict, kept across runs - here in a file of the test's own, removed after
        wchar_t dir[MAX_PATH] = L"", file[MAX_PATH + 64] = L"";
        GetTempPathW(ARRAYSIZE(dir), dir);
        _snwprintf_s(file, ARRAYSIZE(file), _TRUNCATE, L"%lsliquid-dock-test-%lu-taskbar-start.txt", dir, GetCurrentProcessId());
        std::lock_guard<std::mutex> lk(startmenu::g_mu);
        const startmenu::Preference saved = startmenu::g_pref;
        wcscpy_s(startmenu::g_verdictFile, file);
        startmenu::SaveVerdict(startmenu::TaskbarState::Works);
        const bool written = GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES;
        startmenu::g_pref.taskbar = startmenu::TaskbarState::Unknown;
        startmenu::g_verdictLoaded = false;
        startmenu::LoadVerdict();
        const bool works = startmenu::g_pref.taskbar == startmenu::TaskbarState::Works;
        startmenu::SaveVerdict(startmenu::TaskbarState::Unknown);   // cleared (nothing proven either way)
        const bool removed = GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES;
        startmenu::g_pref.taskbar = startmenu::TaskbarState::Unknown;
        startmenu::g_verdictLoaded = false;
        startmenu::LoadVerdict();
        const bool cleared = startmenu::g_pref.taskbar == startmenu::TaskbarState::Unknown;
        DeleteFileW(file);
        startmenu::g_verdictFile[0] = L'\0';
        startmenu::g_pref = saved;
        Check(written && works, "a verdict kept (\"works\" on this Windows build) is read back at the next run's first click");
        Check(removed && cleared, "...and once cleared nothing is kept: the next run checks the button again");
    }

    keys = g_keys; log = LogSize();
    startmenu::Click(primary, false); Sleep(150);
    startmenu::Click(primary, false); Sleep(150);
    startmenu::Click(primary, false);
    Check(WaitIdle(wait), "three clicks: the first one's worker ends");
    Check(g_keys - keys == 1, "three clicks while Start is on its way: one key (a second would close it again)");
    Check(LogHas(log, L"2 more clicks"), "dock.log counts the two clicks made while waiting");

    keys = g_keys;
    startmenu::Click(primary, true);
    Check(g_keys - keys == 1 && startmenu::g_current.load() == 0, "Start in front at the press: one key closes it, no worker");
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
    // as the dock starts; taskbar: how it may use the taskbar's own Start button
    auto fresh = [](startmenu::TaskbarState taskbar) {
        std::lock_guard<std::mutex> lk(startmenu::g_mu);
        startmenu::g_pref = startmenu::Preference{};
        startmenu::g_pref.taskbar = taskbar;
        startmenu::g_reportedFirst = false;   // so the first usual click of each part is logged
    };
    auto state = [] { std::lock_guard<std::mutex> lk(startmenu::g_mu); return startmenu::g_pref.taskbar; };
    ComPtr<startmenu::IVisibility> vis;
    CoCreateInstance(startmenu::kAppVisibility, nullptr, CLSCTX_ALL, startmenu::kAppVisibilityIid, reinterpret_cast<void**>(vis.GetAddressOf()));
    auto open = [&] { return startmenu::Visible(vis.Get(), nullptr); };
    auto settle = [&](bool want, double seconds) {
        const double until = NowSeconds() + seconds;
        while (NowSeconds() < until && open() != want) Sleep(30);
        return open() == want;
    };
    fresh(startmenu::TaskbarState::Off);
    if (open()) { startmenu::Click(primary, true); settle(false, 2); }
    Check(!open(), "Start closed before the test");

    // ---- the taskbar's own Start button (src/taskbar_start.cpp), what the dock presses first since mod.18.
    // With the dock running, its taskbar is hidden: this shows whether the button is found then.
    fresh(startmenu::TaskbarState::Unknown);
    const double lookT = NowSeconds();
    const taskbarstart::Found found = FindTaskbarButton(primary);
    printf("  the taskbar's Start button: %s, in %.0f ms (%s)\n", Utf8(taskbarstart::FoundText(found)).c_str(), (NowSeconds() - lookT) * 1000,
           Utf8(taskbarstart::Describe()).c_str());
    if (found == taskbarstart::Found::Kept || found == taskbarstart::Found::Now) {
        int keys = g_keys; size_t log = LogSize();
        startmenu::Click(primary, false);
        WaitIdle(8);
        Check(settle(true, 1), "a click opens Start");
        Check(LogHas(log, L"Start came by the taskbar's Start button"), "...by the taskbar's own Start button (dock.log names it)");
        Check(g_keys == keys, "...without the Windows key");
        Sleep(200);
        Check(StartHasKeyboard(), "...and Start has the keyboard (typing goes to its search box)");
        Check(state() == startmenu::TaskbarState::Works, "...and from now on the taskbar's button is trusted (it closes Start too)");

        Check(startmenu::InFront(), "Start open: the press-time check sees it (what decides that the button closes it)");
        keys = g_keys; log = LogSize();
        startmenu::Click(primary, startmenu::InFront());
        Check(settle(false, 2), "a click with Start open closes it");
        Sleep(300);   // a press that did not go through would be logged by now
        Check(g_keys == keys && !LogHas(log, L"did not go through"), "...by the taskbar's button (no Windows key)");
        printf("  in front once Start closed: %s\n", FrontExe().c_str());

        keys = g_keys;
        for (int i = 0; i < 3; ++i) { startmenu::Click(primary, false); Sleep(80); }
        WaitIdle(8);
        Check(settle(true, 1), "three quick clicks: Start is open (the extra clicks did not toggle it shut)");
        Check(g_keys == keys, "...and no Windows key was pressed");
        startmenu::Click(primary, startmenu::InFront());
        settle(false, 2);
        // A test PC, 2026-10-27 14:17: the shell's request sent about half a second after Start had been closed by
        // the taskbar's button was taken and brought no Start (8 s); the same request nine seconds later did. The
        // dock does not mix the two that way (once the button works, it opens and closes Start); here the shell's
        // part waits a little first, so it checks the shell and not that hand-over.
        Sleep(2000);
    } else {
        printf("  not checked: no Start button to press on this taskbar - the dock then asks the shell, checked below\n");
    }

    // ---- without the taskbar's button: the shell's request, as before mod.18 (the dock's fallback)
    printf("  in front before the shell's part: %s\n", FrontExe().c_str());
    fresh(startmenu::TaskbarState::Off);
    int keys = g_keys; size_t log = LogSize();
    startmenu::Click(primary, false);
    WaitIdle(8);
    Check(settle(true, 1), "without the taskbar's button: a click opens Start");
    Check(g_keys == keys, "...without the Windows key (the shell's request brought it)");
    Check(LogHas(log, L"Start came by the shell's request"), "dock.log names the shell's request");
    Sleep(200);
    Check(StartHasKeyboard(), "...and Start has the keyboard (typing goes to its search box)");

    Check(startmenu::InFront(), "Start open: the press-time check sees it (what decides that the button closes it)");
    if (startmenu::InFront()) {   // (a click without Start open would open it)
        startmenu::Click(primary, true);
        Check(settle(false, 2), "a click with Start open closes it (the Windows key)");
    }
    Sleep(500);

    keys = g_keys;
    for (int i = 0; i < 3; ++i) { startmenu::Click(primary, false); Sleep(80); }
    WaitIdle(8);
    Check(settle(true, 1), "three quick clicks: Start is open (the extra clicks did not toggle it shut)");
    Check(g_keys == keys, "...and no Windows key was pressed");
    startmenu::Click(primary, true);
    settle(false, 2);

    fresh(startmenu::TaskbarState::Unknown);   // as the dock starts: that monitor's taskbar button, if it has one
    std::vector<HMONITOR> mons;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(p)->push_back(m); return TRUE;
    }, reinterpret_cast<LPARAM>(&mons));
    for (HMONITOR m : mons) {
        if (m == primary) continue;
        startmenu::Click(m, false);
        WaitIdle(8);
        bool up = settle(true, 1);
        HWND fg = GetForegroundWindow();
        Check(up && fg && MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == m, "the start button of another monitor's dock opens Start on that monitor");
        startmenu::Click(m, true);
        settle(false, 2);
        break;
    }
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    startmenu::g_keepVerdict = false;   // the dock's kept verdict (data\taskbar-start.txt) is neither read nor written
    const bool live = argc > 1 && !wcscmp(argv[1], L"--live");
    HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (live) LiveTests(primary);
    else SafeTests(primary);
    printf("start_menu_worker_test%s: %s (%d checks, %d failure%s)\n", live ? " --live" : "", failures ? "FAILED" : "ALL PASSED",
           checks, failures, failures == 1 ? "" : "s");
    fflush(stdout);
    return failures;
}
