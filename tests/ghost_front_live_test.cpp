// A closed Start or Search keeping the front, live (tests\run_ghost_front_live.bat). After the monitors came back on,
// SearchHost's Search window - closed: WS_VISIBLE still, cloaked by the shell - stayed the foreground window with
// nothing on the screen, and the dock took it for an open Search (seen on a test PC, 2026-10-30: six clicks on Chrome
// dropped). mod.25 brings the window up as usual over it; this asks Windows whether that goes through.
//
// It turns the monitors off (SC_MONITORPOWER, as their power timeout would), wakes them with a one-pixel mouse move,
// writes down what holds the front as they come back, then brings up a window of another process (a second copy of
// this program) the dock's way: the unassigned key for the right, AllowSetForegroundWindow for its process,
// SetForegroundWindow, and attached to the front window's thread if that alone did not do (apps_click.cpp ForceForeground),
// tried again twice 0.25 s apart (app_click.h kRefocusTries). Nothing else is sent: no Esc, no click.
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <string>

static double Now() { static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }(); LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
static const double t0 = Now();
static const wchar_t* kTargetClass = L"LiquidDockGhostFrontTarget";

static bool Cloaked(HWND h) { DWORD c = 0; return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c; }
static std::wstring Exe(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"?";
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    std::wstring s = QueryFullProcessImageNameW(p, 0, path, &n) ? path : L"?";
    CloseHandle(p);
    size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? s : s.substr(slash + 1);
}
static std::wstring Front() {
    HWND fg = GetForegroundWindow();
    if (!fg) return L"nothing";
    wchar_t cls[96] = {}; GetClassNameW(fg, cls, 96);
    wchar_t buf[400];
    swprintf_s(buf, L"%ls %ls%ls%ls", Exe(fg).c_str(), cls, IsWindowVisible(fg) ? L"" : L" hidden", Cloaked(fg) ? L" cloaked" : L"");
    return buf;
}
static bool ShellExe(HWND h) {
    const std::wstring e = Exe(h);
    for (const wchar_t* s : { L"StartMenuExperienceHost.exe", L"SearchHost.exe", L"SearchApp.exe", L"ShellExperienceHost.exe", L"ShellHost.exe" })
        if (!_wcsicmp(e.c_str(), s)) return true;
    return false;
}
#define SAY(...) do { wprintf(L"%6.2f  ", Now() - t0); wprintf(__VA_ARGS__); wprintf(L"\n"); fflush(stdout); } while (0)

static int displayState = -1;   // GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed
static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_POWERBROADCAST && w == PBT_POWERSETTINGCHANGE) {
        auto* s = (POWERBROADCAST_SETTING*)l;
        if (s->PowerSetting == GUID_CONSOLE_DISPLAY_STATE) { displayState = *(DWORD*)s->Data; SAY(L"display: %ls", displayState == 0 ? L"off" : displayState == 1 ? L"on" : L"dimmed"); }
    }
    return DefWindowProcW(h, m, w, l);
}
static void Pump(double seconds, HWND watchFront = nullptr) {
    const double end = Now() + seconds;
    std::wstring last = Front();
    while (Now() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (watchFront) { std::wstring f = Front(); if (f != last) { SAY(L"in front: %ls", f.c_str()); last = f; } }
        Sleep(20);
    }
}

static int Target() {   // the second copy: a plain window of another process, shown behind the front one
    WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kTargetClass;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, kTargetClass, L"Liquid Dock ghost-front test", WS_OVERLAPPEDWINDOW, 200, 200, 420, 160, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { if (msg.message == WM_CLOSE) break; TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

static void TakeRight() {
    if (AllowSetForegroundWindow(GetCurrentProcessId())) return;
    INPUT in[2] = {}; in[0].type = in[1].type = INPUT_KEYBOARD; in[0].ki.wVk = in[1].ki.wVk = 0xE8; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}
static unsigned long Force(HWND h) {   // apps_click.cpp ForceForeground
    if (SetForegroundWindow(h) && GetForegroundWindow() == h) return 0;
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    const bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    const unsigned long err = fgThread && fgThread != me && !attached ? GetLastError() : 0;
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    SetForegroundWindow(h);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
    return err;
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && !wcscmp(argv[1], L"--target")) return Target();
    int tries = 1;
    if (argc > 1) tries = _wtoi(argv[1]) > 0 ? _wtoi(argv[1]) : 1;

    WNDCLASSW wc = {}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"LiquidDockGhostFrontProbe";
    RegisterClassW(&wc);
    HWND me = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    RegisterPowerSettingNotification(me, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" --target";
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { wprintf(L"RESULT error: no target process (%lu)\n", GetLastError()); return 2; }
    HWND target = nullptr;
    for (int i = 0; i < 100 && !target; ++i) { Sleep(50); target = FindWindowW(kTargetClass, nullptr); }
    if (!target) { wprintf(L"RESULT error: no target window\n"); TerminateProcess(pi.hProcess, 1); return 2; }
    Pump(0.5);
    SAY(L"start: in front %ls; the test window behind it", Front().c_str());

    int ghosts = 0, cameOverGhost = 0, refusedByGhost = 0;
    for (int run = 1; run <= tries; ++run) {
        SAY(L"---- run %d of %d: monitors off", run, tries);
        PostMessageW(me, WM_SYSCOMMAND, SC_MONITORPOWER, 2);
        Pump(10, me);
        SAY(L"waking them: a one-pixel mouse move");
        INPUT mv[2] = {}; mv[0].type = mv[1].type = INPUT_MOUSE; mv[0].mi.dwFlags = mv[1].mi.dwFlags = MOUSEEVENTF_MOVE; mv[0].mi.dx = 1; mv[1].mi.dx = -1;
        SendInput(2, mv, sizeof(INPUT));
        Pump(6, me);
        if (displayState == 0) { SendInput(2, mv, sizeof(INPUT)); Pump(4, me); }
        HWND fg = GetForegroundWindow();
        const bool ghost = fg && ShellExe(fg) && Cloaked(fg);
        SAY(L"after the wake: in front %ls -> %ls", Front().c_str(), ghost ? L"a closed shell window keeping the front (reproduced)" : L"no closed shell window in front");
        ghosts += ghost;
        // the dock's way (AppModel::Push at a click, then two refocus tries 0.25 s apart)
        DWORD tpid = 0; GetWindowThreadProcessId(target, &tpid);
        TakeRight();
        AllowSetForegroundWindow(tpid);
        unsigned long err = Force(target);
        bool came = false;
        for (int k = 0; k < 3 && !came; ++k) {
            Pump(0.25);
            came = GetForegroundWindow() == target;
            if (!came && k < 2) { TakeRight(); err = Force(target); }
        }
        SAY(L"the dock's way: %ls (in front now: %ls; attach error %lu)", came ? L"the test window came in front" : L"it did NOT come in front", Front().c_str(), err);
        if (ghost) { if (came) ++cameOverGhost; else ++refusedByGhost; }
        // the test window behind again for the next run; the terminal back in front
        if (run < tries) { SetWindowPos(target, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); Pump(1); }
    }
    PostMessageW(target, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    TerminateProcess(pi.hProcess, 0);
    wprintf(L"RESULT runs=%d reproduced=%d came_over_it=%d refused=%d\n", tries, ghosts, cameOverGhost, refusedByGhost);
    return 0;
}
