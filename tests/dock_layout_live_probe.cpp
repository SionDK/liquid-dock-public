// What the dock's window-in-front rules see on the real desktop (tests\run_dock_layout_live.bat). Runs the
// production code of dock.cpp - FullscreenMonitor (a fullscreen app on a monitor: its dock goes away, an
// always-shown dock elsewhere freezes its glass) and the shell pop-up test (Start, search: the dock comes up) -
// against the real window in front, and prints one line whenever something it reads changes:
//   fg:     the window in front: class, program, WS_CAPTION, WS_THICKFRAME (frame), WS_POPUP, maximised, its
//           rectangle and client area (a maximized window counts as fullscreen only without a frame, not a
//           WS_POPUP, both exactly the monitor: F11 from a maximized browser), the monitors it covers,
//           and which monitors' docks the dock's rule puts away (a window spanned across monitors: each one it
//           covers)
//   work:   a monitor's work area (an always-shown dock's strip is cut out of it while its app bar holds;
//           after an Explorer restart it is cut out again as soon as the dock has registered the strip again)
//   cursor: GetCursorPos failing (lock screen, UAC prompt, Ctrl+Alt+Del: the docks then see no pointer)
//   button: the mouse buttons as GetAsyncKeyState reports them, with Windows' "swap buttons" setting
//           (the dock's drag reads the primary one this way)
// It only reads: no input is sent, no window is created, moved or activated, the running dock is not touched.
// Put things in front while it runs (F11 in a browser, a video or a game fullscreen, Win+Tab, Start, Alt+Tab,
// a window maximised; a window spanned across two monitors) and read the lines.
// Usage: dock_layout_live_probe.exe [seconds]   (default 60)
#include "../src/dock.cpp"
#include <cstdio>
#include <cwchar>

Settings g_settings;
HWND g_mainWnd = nullptr;
bool RevealHotkeyFailed() { return false; }

namespace {
struct Mon { HMONITOR h; RECT rect, work; };
std::vector<Mon> Monitors() {
    std::vector<Mon> out;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(m, &mi)) ((std::vector<Mon>*)lp)->push_back({ m, mi.rcMonitor, mi.rcWork });
        return TRUE;
    }, (LPARAM)&out);
    return out;
}
int IndexOf(const std::vector<Mon>& mons, HMONITOR h) {
    for (size_t i = 0; i < mons.size(); ++i) if (mons[i].h == h) return (int)i;
    return -1;
}
bool Covers(const RECT& r, const RECT& m) { return r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom; }
std::wstring ProgramOf(HWND w) {
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!p) return L"?";
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    std::wstring name = L"?";
    if (QueryFullProcessImageNameW(p, 0, path, &n)) { const wchar_t* s = wcsrchr(path, L'\\'); name = s ? s + 1 : path; }
    CloseHandle(p);
    return name;
}
double Now() { return GetTickCount64() / 1000.0; }
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // as the dock: physical pixels
    const double seconds = argc > 1 ? _wtof(argv[1]) : 60;
    auto mons = Monitors();
    std::printf("monitors: %d\n", (int)mons.size());
    for (size_t i = 0; i < mons.size(); ++i)
        std::printf("  %d: %ld,%ld %ldx%ld%s, work %ld,%ld-%ld,%ld\n", (int)i, mons[i].rect.left, mons[i].rect.top,
                    mons[i].rect.right - mons[i].rect.left, mons[i].rect.bottom - mons[i].rect.top,
                    mons[i].h == MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY) ? " primary" : "",
                    mons[i].work.left, mons[i].work.top, mons[i].work.right, mons[i].work.bottom);
    std::printf("swap buttons: %d\n", GetSystemMetrics(SM_SWAPBUTTON));
    std::printf("watching for %.0f s ...\n", seconds);
    std::fflush(stdout);

    std::wstring lastFg;
    std::vector<RECT> lastWork;
    for (auto& m : mons) lastWork.push_back(m.work);
    bool lastCursorOk = true; int lastButtons = -1;
    const double start = Now();
    while (Now() - start < seconds) {
        const double t = Now() - start;
        // the window in front, as FullscreenMonitor and the shell pop-up rule see it
        HWND fg = GetForegroundWindow();
        wchar_t line[1024] = L"(none)";
        if (fg) {
            wchar_t cls[64] = {}; GetClassNameW(fg, cls, 64);
            RECT r{}; GetWindowRect(fg, &r);
            const LONG_PTR st = GetWindowLongPtrW(fg, GWL_STYLE);
            RECT fsRect{};
            const HMONITOR fs = FullscreenMonitor(nullptr, fg, &fsRect);   // the production rule (dock.cpp, Dock::Update)
            std::wstring covers, away;
            for (size_t i = 0; i < mons.size(); ++i) {
                if (Covers(r, mons[i].rect)) covers += std::to_wstring(i) + L" ";
                if (fs && (mons[i].h == fs || RectCovers(fsRect, mons[i].rect))) away += std::to_wstring(i) + L" ";
            }
            WINDOWINFO wi = { sizeof(wi) };   // the client area and the frame: what tells F11 from maximized (dock.cpp ZoomedFullscreen)
            if (!GetWindowInfo(fg, &wi)) wi.rcClient = RECT{};
            swprintf_s(line, L"%ls (%ls) caption %d frame %d popup %d maximised %d rect %ld,%ld-%ld,%ld client %ld,%ld-%ld,%ld on %d covers [%ls] -> docks away on [%ls] (named %d) shell pop-up %d",
                       cls, ProgramOf(fg).c_str(), (st & WS_CAPTION) == WS_CAPTION, (st & WS_THICKFRAME) != 0, (st & WS_POPUP) != 0,
                       IsZoomed(fg) != FALSE, r.left, r.top, r.right, r.bottom,
                       wi.rcClient.left, wi.rcClient.top, wi.rcClient.right, wi.rcClient.bottom,
                       IndexOf(mons, MonitorFromWindow(fg, MONITOR_DEFAULTTONULL)), covers.c_str(), away.c_str(), IndexOf(mons, fs),
                       (int)ShellPopupWindow(fg));
        }
        if (lastFg != line) { lastFg = line; std::printf("%7.2f fg: %ls\n", t, line); }
        // work areas: the app bars of always-shown docks
        auto now = Monitors();
        if (now.size() != mons.size()) { std::printf("%7.2f monitors changed: %d\n", t, (int)now.size()); mons = now; lastWork.clear(); for (auto& m : mons) lastWork.push_back(m.work); }
        for (size_t i = 0; i < now.size() && i < lastWork.size(); ++i)
            if (!EqualRect(&now[i].work, &lastWork[i])) {
                std::printf("%7.2f work: monitor %d %ld,%ld-%ld,%ld (was %ld,%ld-%ld,%ld)\n", t, (int)i, now[i].work.left, now[i].work.top,
                            now[i].work.right, now[i].work.bottom, lastWork[i].left, lastWork[i].top, lastWork[i].right, lastWork[i].bottom);
                lastWork[i] = now[i].work;
            }
        // the pointer
        POINT c{};
        const bool ok = GetCursorPos(&c) != FALSE;
        if (ok != lastCursorOk) {
            lastCursorOk = ok;
            if (ok) std::printf("%7.2f cursor: readable again (%ld,%ld)\n", t, c.x, c.y);
            else std::printf("%7.2f cursor: GetCursorPos failed (error %lu)\n", t, GetLastError());
        }
        const int buttons = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0) | ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 2 : 0);
        if (buttons != lastButtons) {
            if (lastButtons >= 0)
                std::printf("%7.2f button: physical left %d right %d (swap buttons %d: the primary button is the %ls one)\n", t, buttons & 1,
                            (buttons >> 1) & 1, GetSystemMetrics(SM_SWAPBUTTON), GetSystemMetrics(SM_SWAPBUTTON) ? L"right" : L"left");
            lastButtons = buttons;
        }
        std::fflush(stdout);
        Sleep(100);
    }
    std::printf("done\n");
    return 0;
}
