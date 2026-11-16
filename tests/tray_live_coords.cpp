// Live, by hand (tests\run_tray_live.bat): the coordinates a tray program gets with a click on its icon -
// from Windows' own hidden-icons window and from the dock - at the screen's real scale (150% on a
// laptop screen, say). The dock passes them on as version-4 programs expect (wParam = x, y); whether Windows hands
// a program that is not DPI aware its own (scaled) coordinates or the screen's can only be seen on a real
// screen. Two test icons: one of a window that is not DPI aware ("i"), one of a per-monitor aware window
// ("!"). Each left / right click logs what came (wParam), where the pointer is for that window
// (GetCursorPos in its own DPI context), and the icon's rectangle (Shell_NotifyIconGetRect). The verdict
// compares the dock's answers with Windows' own for the same window.
// Nothing is pressed or typed by the test. The person clicks the icons; prompts go to the console window
// (also when the output goes to the log). The icons are removed when it ends.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace {
constexpr UINT kCallback = WM_APP + 1;
constexpr int kPhases = 2;   // 0: Windows' hidden-icons window, 1: the dock

struct Probe {
    const wchar_t* name; DPI_AWARENESS_CONTEXT ctx; UINT uid; LPCWSTR icon;
    HWND hwnd = nullptr; DWORD tid = 0; std::thread t;
};
struct Hit { int phase, probe; UINT event; POINT wp, cursor; bool rectOk; RECT rect; };

std::atomic<int> g_phase{ -1 };
std::mutex g_mu;
std::vector<Hit> g_hits;
Probe g_probes[2] = { { L"not DPI aware", DPI_AWARENESS_CONTEXT_UNAWARE, 1, IDI_INFORMATION },
                      { L"per-monitor aware", DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, 2, IDI_WARNING } };

void Say(const wchar_t* fmt, ...) {   // the console window, even when stdout goes to a log file
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    HANDLE con = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (con == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteConsoleW(con, buf, (DWORD)wcslen(buf), &n, nullptr);
    CloseHandle(con);
}

LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m != kCallback) return DefWindowProcW(h, m, w, l);
    const int which = (int)GetWindowLongPtrW(h, GWLP_USERDATA);
    const UINT event = LOWORD(l);
    if ((event != NIN_SELECT && event != WM_CONTEXTMENU) || g_phase < 0) return 0;
    Hit hit{ g_phase, which, event, { GET_X_LPARAM(w), GET_Y_LPARAM(w) }, {}, false, {} };
    GetCursorPos(&hit.cursor);   // in this window's DPI context
    NOTIFYICONIDENTIFIER id = { sizeof(id) };
    id.hWnd = h; id.uID = g_probes[which].uid;
    hit.rectOk = SUCCEEDED(Shell_NotifyIconGetRect(&id, &hit.rect));
    std::printf("CLICK %s icon=\"%ls\" event=%s wparam=%ld,%ld cursor=%ld,%ld rect=%s%ld,%ld-%ld,%ld\n",
                hit.phase ? "dock" : "windows", g_probes[which].name, event == NIN_SELECT ? "NIN_SELECT" : "WM_CONTEXTMENU",
                hit.wp.x, hit.wp.y, hit.cursor.x, hit.cursor.y, hit.rectOk ? "" : "(none) ", hit.rect.left, hit.rect.top, hit.rect.right, hit.rect.bottom);
    std::fflush(stdout);
    std::lock_guard<std::mutex> lk(g_mu);
    g_hits.push_back(hit);
    return 0;
}

void RunProbe(Probe& p, int which, HANDLE ready) {
    p.tid = GetCurrentThreadId();
    SetThreadDpiAwarenessContext(p.ctx);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = ProbeProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"LiquidDockTrayLiveCoords";
    RegisterClassExW(&wc);
    p.hwnd = CreateWindowExW(0, wc.lpszClassName, p.name, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowLongPtrW(p.hwnd, GWLP_USERDATA, which);
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = p.hwnd; nid.uID = p.uid; nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kCallback; nid.hIcon = LoadIconW(nullptr, p.icon);
    swprintf_s(nid.szTip, L"Liquid Dock test icon: %ls", p.name);
    bool added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4;
    added = added && Shell_NotifyIconW(NIM_SETVERSION, &nid) != FALSE;
    std::printf("ICON \"%ls\" added=%d\n", p.name, (int)added);
    SetEvent(ready);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(p.hwnd);
}

int Count(int phase) {
    std::lock_guard<std::mutex> lk(g_mu);
    int seen[2][2] = {};
    for (auto& h : g_hits) if (h.phase == phase) seen[h.probe][h.event == WM_CONTEXTMENU] = 1;
    return seen[0][0] + seen[0][1] + seen[1][0] + seen[1][1];
}

void WaitPhase(int phase, int seconds) {
    g_phase = phase;
    for (int i = 0; i < seconds * 10 && Count(phase) < 4; ++i) Sleep(100);
    Say(L"  %d of 4 clicks seen\r\n", Count(phase));
}
}   // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi = GetDpiForSystem();
    std::printf("SCREEN system dpi %u (%u%%), primary monitor %p\n", dpi, dpi * 100 / 96, (void*)mon);
    for (int i = 0; i < 2; ++i) {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_probes[i].t = std::thread(RunProbe, std::ref(g_probes[i]), i, ready);
        WaitForSingleObject(ready, 5000);
        CloseHandle(ready);
    }
    Say(L"\r\n[1/2] Windows' own hidden-icons window: open it (the dock's hidden-icons button), then LEFT-click and\r\n"
        L"      RIGHT-click each of the two test icons (blue \"i\" and yellow \"!\") once. 90 seconds.\r\n");
    WaitPhase(0, 90);
    Say(L"\r\n[2/2] The dock: Settings > Personalization > Taskbar > Other system tray icons: turn on both\r\n"
        L"      \"tray_live_coords\" entries (the dock shows them with its tray icons on), then LEFT-click and\r\n"
        L"      RIGHT-click each of the two icons on the dock once. 120 seconds.\r\n");
    WaitPhase(1, 120);
    g_phase = -1;

    // for each window and event: does wParam stand where the pointer is (in that window's own context), and
    // inside the icon's rectangle - and is that the same from the dock as from Windows?
    int mismatches = 0, missing = 0;
    for (int probe = 0; probe < 2; ++probe)
        for (UINT event : { (UINT)NIN_SELECT, (UINT)WM_CONTEXTMENU }) {
            const Hit* got[kPhases] = {};
            std::lock_guard<std::mutex> lk(g_mu);
            for (auto& h : g_hits) if (h.probe == probe && h.event == event && !got[h.phase]) got[h.phase] = &h;
            auto closeTo = [](POINT a, POINT b) { return std::abs(a.x - b.x) <= 3 && std::abs(a.y - b.y) <= 3; };
            if (!got[0] || !got[1]) { ++missing; continue; }
            bool atCursor[kPhases], inRect[kPhases];
            for (int ph = 0; ph < kPhases; ++ph) {
                atCursor[ph] = closeTo(got[ph]->wp, got[ph]->cursor);
                inRect[ph] = got[ph]->rectOk && PtInRect(&got[ph]->rect, got[ph]->cursor);
            }
            bool same = atCursor[0] == atCursor[1] && inRect[0] == inRect[1];
            if (!same) ++mismatches;
            std::printf("COMPARE icon=\"%ls\" %s: wparam at the pointer windows=%d dock=%d, pointer inside the icon rect windows=%d dock=%d -> %s\n",
                        g_probes[probe].name, event == NIN_SELECT ? "left" : "right", atCursor[0], atCursor[1], inRect[0], inRect[1],
                        same ? "same" : "DIFFERENT");
        }
    for (auto& p : g_probes) { PostThreadMessageW(p.tid, WM_QUIT, 0, 0); if (p.t.joinable()) p.t.join(); }
    std::printf("VERDICT %s (%d different, %d not compared)\n", mismatches ? "the dock differs from Windows" : missing ? "incomplete" : "the dock matches Windows",
                mismatches, missing);
    Say(L"\r\nDone. The test icons are removed (turn their entries off again in the taskbar settings if you like).\r\n");
    return mismatches ? 1 : missing ? 2 : 0;
}
