// Real windows, restricted to disposable windows in this process: the preview's popup going from monitor to
// monitor when they have different scales (a laptop's 150 % screen and a 100 % one connected to it, say). The popup is
// made once, on the primary monitor, and moved with SetWindowPos; it has no WM_DPICHANGED of its own (it goes to
// DefWindowProc). This checks that on every monitor, coming from every other one, the popup has the place and
// size the preview chose for it (bounds), and prints each monitor's scale. One monitor, or all at one scale: nothing
// to check (exit 0). No keyboard or mouse input, no other program's window, the running dock untouched.
// tests\run_preview_live_dpi.bat builds and runs it.
#include "../src/common.h"
#include "../src/apps.h"
#include <dwmapi.h>
#include <cstdio>
#include <cstdarg>
#define private public
#include "../src/window_preview.h"
#undef private
#include "../src/window_preview.cpp"
Settings g_settings;
AppModel g_apps;
void AppModel::ActivateWindow(HWND) {}
void AppModel::AskToClose(HWND, const std::wstring&) {}
void Log(const wchar_t* fmt, ...) {
    wchar_t w[1024]; va_list ap; va_start(ap, fmt); vswprintf(w, 1024, fmt, ap); va_end(ap);
    char u[3072]; if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), nullptr, nullptr)) printf("LOG %s\n", u);
    fflush(stdout);
}
static int checks = 0, failures = 0;
static void Check(bool ok, const char* msg) { ++checks; if (!ok) { ++failures; printf("FAIL %s\n", msg); fflush(stdout); } }
static void Pump(int ms) {
    auto end = GetTickCount64() + ms;
    do { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(10); } while (GetTickCount64() < end);
}
int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::vector<RECT> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM p) -> BOOL { ((std::vector<RECT>*)p)->push_back(*r); return TRUE; }, (LPARAM)&monitors);
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW; wc.lpszClassName = L"LiquidDockPreviewDpiFixture";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH); RegisterClassW(&wc);
    std::vector<HWND> windows;   // one of the test's own windows on each monitor, the preview's source there
    std::vector<UINT> dpis;
    for (const RECT& m : monitors) {
        HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Preview DPI fixture", WS_OVERLAPPEDWINDOW,
                                 m.left + 120, m.top + 120, 480, 300, nullptr, nullptr, wc.hInstance, nullptr);
        if (!h) { puts("Window creation failed"); return 2; }
        ShowWindow(h, SW_SHOWNOACTIVATE); windows.push_back(h); dpis.push_back(GetDpiForWindow(h));
    }
    Pump(200); DwmFlush();
    bool mixed = false;
    for (size_t i = 0; i < monitors.size(); ++i) {
        const RECT& m = monitors[i];
        printf("MONITOR %zu %ldx%ld at %ld,%ld: %u dpi (%u %%)\n", i, m.right - m.left, m.bottom - m.top, m.left, m.top, dpis[i], dpis[i] * 100 / 96);
        mixed |= dpis[i] != dpis[0];
    }
    if (!mixed) puts("SKIP: every monitor has the same scale; nothing to cross");
    else {
        WindowPreview p;
        double t = 0;
        // every monitor from every other one, twice round
        for (int round = 0; round < 2; ++round)
        for (size_t i = 0; i < monitors.size(); ++i) {
            const RECT& m = monitors[i];
            auto app = std::make_shared<AppItem>(); app->name = L"LiquidDock 미리보기 배율 확인"; app->windows = {windows[i]};
            const int cx = (m.left + m.right) / 2;
            const float scale = std::max(.72f, std::min(m.right - m.left, m.bottom - m.top) / 1440.f);
            const int depth = (int)std::lround(110 * scale), half = (int)std::lround(28 * scale);
            const RECT anchor{cx - half, m.bottom - depth, cx + half, m.bottom};
            const POINT cursor{cx, m.bottom - depth / 2};
            p.Dismiss(); p.candidate.reset();
            p.Update(windows[0], app, anchor, m, Edge::Bottom, scale, cursor, t += 1, true);
            p.Update(windows[0], app, anchor, m, Edge::Bottom, scale, cursor, t += 0.5, true);
            Pump(150);
            RECT real{}; GetWindowRect(p.hwnd, &real);
            RECT client{}; GetClientRect(p.hwnd, &client);
            const UINT dpi = p.hwnd ? GetDpiForWindow(p.hwnd) : 0;
            printf("DPI_MOVE round=%d monitor=%zu dpi=%u popup_dpi=%u bounds=%ld,%ld,%ld,%ld window=%ld,%ld,%ld,%ld client=%ldx%ld\n",
                   round, i, dpis[i], dpi, p.bounds.left, p.bounds.top, p.bounds.right, p.bounds.bottom,
                   real.left, real.top, real.right, real.bottom, client.right, client.bottom);
            fflush(stdout);
            Check(p.Open(), "the preview opens on the monitor");
            Check(EqualRect(&real, &p.bounds) != FALSE, "the popup is where the preview put it, at the size it chose");
            Check(client.right == p.bounds.right - p.bounds.left && client.bottom == p.bounds.bottom - p.bounds.top,
                  "...its cards and DWM pictures drawn in a client area of that size");
        }
        p.Dismiss();
    }
    for (HWND h : windows) DestroyWindow(h);
    Pump(50);
    printf("RESULT live_preview_dpi checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
