// Live probe for the settings window's wheel handling (dock_render-6, -7): which WM_MOUSEWHEEL messages this
// PC's touchpad and mouse really send. A window of its own asks the user to scroll over it for 15 s - slowly
// and quickly with two fingers on the touchpad, then with a mouse wheel if there is one - and counts the
// messages: how many carry less than one notch (WHEEL_DELTA = 120), how far the settings window scrolls its
// page for them (in proportion: 60 DIPs a notch) and how far the old code did (each message's delta / 120
// notches, the rest dropped), and whether the pointer stays where it was between the messages of a swipe:
// the settings window keeps a gesture on the page while it does, so a slider that the page brings under the
// pointer does not take the rest of the swipe. No input is sent, no other window is touched, nothing is
// written but the console output.
// Usage: dock_render_live_wheel.exe [seconds]
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>

static int messages = 0, partial = 0, sum = 0, minAbs = 1 << 30, maxAbs = 0;
static int notchesOld = 0;      // the old settings window: GET_WHEEL_DELTA_WPARAM / WHEEL_DELTA per message
static int moved = 0;           // messages whose pointer is more than 2 px from the previous message's
static POINT last{};
static ULONGLONG endAt = 0;

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEWHEEL: {
        const int d = GET_WHEEL_DELTA_WPARAM(w);
        const POINT at = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (messages && (std::abs(at.x - last.x) > 2 || std::abs(at.y - last.y) > 2)) ++moved;
        last = at;
        ++messages; sum += d;
        if (std::abs(d) < WHEEL_DELTA) ++partial;
        minAbs = std::min(minAbs, std::abs(d)); maxAbs = std::max(maxAbs, std::abs(d));
        notchesOld += d / WHEEL_DELTA;
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }
    case WM_TIMER:
        if (GetTickCount64() >= endAt) DestroyWindow(h);
        else InvalidateRect(h, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc); InflateRect(&rc, -16, -16);
        wchar_t t[512];
        swprintf_s(t, L"Scroll over this window: slowly and quickly with two fingers on the touchpad, then with a mouse wheel.\n\n"
                      L"%d s left\n\nwheel messages: %d, less than one notch: %d, with the pointer moved: %d\n"
                      L"settings page scroll now: %.0f DIPs (the old code: %d DIPs)",
                   (int)((endAt - std::min(endAt, GetTickCount64())) / 1000), messages, partial, moved, -sum * 60.0 / WHEEL_DELTA, -notchesOld * 60);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, t, -1, &rc, DT_LEFT | DT_WORDBREAK);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const int seconds = argc > 1 ? std::max(5, _wtoi(argv[1])) : 15;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"dock_render-live-wheel";
    if (!RegisterClassExW(&wc)) { puts("RESULT dock_render_live_wheel: no window class"); return 2; }
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Liquid Dock wheel probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 900, 500, nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) { puts("RESULT dock_render_live_wheel: no window"); return 2; }
    endAt = GetTickCount64() + (ULONGLONG)seconds * 1000;
    SetTimer(h, 1, 250, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    printf("wheel messages %d, less than one notch %d (smallest |delta| %d, largest %d), sum %d, pointer moved between them %d\n",
           messages, partial, messages ? minAbs : 0, maxAbs, sum, moved);
    printf("settings page scroll: now %.0f DIPs, the old code %d DIPs\n", -sum * 60.0 / WHEEL_DELTA, -notchesOld * 60);
    if (!messages) { puts("VERDICT no wheel messages: scroll over the probe window while it runs"); return 1; }
    if (partial) printf("VERDICT %d of %d wheel messages carry less than one notch: the settings window scrolls its page by them in "
                        "proportion and adds them up on a slider (the old code dropped them)\n", partial, messages);
    else puts("VERDICT every wheel message is a whole notch or more on this PC");
    if (moved) printf("VERDICT the pointer moved between %d of %d wheel messages: each move starts a new wheel gesture, so a slider "
                      "under the pointer then takes the wheel (fine if it was the mouse moving between scrolls)\n", moved, messages);
    else puts("VERDICT the pointer stayed put through every wheel message: a swipe that scrolls the page keeps scrolling it");
    return 0;
}
