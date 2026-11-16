// FluentFlyout module (src/fluent_*.cpp) with the production tray host (src/tray_host*.cpp):
//   - FluentFlyout's settings.xml: values, defaults, lists, entities; reloaded when it changes
//   - its app filter (block / allow list) and "something plays" from the media sessions
//   - finding its widget window ("TaskbarWindow"), in the host window or on its own
//   - when the dock's slot opens: the widget hosted, visible, "hide completely" with and without media
//   - flyouts next to a shown dock: along the dock to the widget at once, off the glass after their
//     own animation, a stack moved together, left alone for a hidden dock, the far side or a move of
//     FluentFlyout's own
// Everything runs in this process under test names: the "FluentFlyout" process is this test (its
// windows: test class names and titles, seen by the module as FluentFlyout's and Explorer's through the
// redirections below), its settings event has a test name, the settings file is a temporary one, the
// media sessions are set by hand.
// Windows (or Wine): cl /std:c++20 /EHsc tests\fluent_flyout_test.cpp user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib version.lib dwmapi.lib
#include "../src/explorer_taskbar.h"
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <thread>
#include <atomic>

#define LD_TRAY_CLASS L"LiquidDockFluentTest_TrayWnd"
#define LD_TRAY_SETTINGS_KEY L"Software\\LiquidDockFluentTest\\NotifyIconSettings"
#define LD_TRAY_SETTINGS_PARENT L"Software\\LiquidDockFluentTest"
#define LD_TRAY_CREATED_MESSAGE L"LiquidDockFluentTest.TaskbarCreated"
static bool g_testElevated = false;
#define LD_TRAY_ELEVATED g_testElevated
static HWND g_fakeExplorer = nullptr;
static HWND TestExplorerTaskbar() { return g_fakeExplorer; }
#define ExplorerTaskbar TestExplorerTaskbar
#include "../src/tray_host.cpp"
#include "../src/tray_host_icons.cpp"
#include "../src/tray_host_settings.cpp"
#include "../src/tray_host_widget.cpp"
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <mutex>
#include <optional>
static std::wstring g_settingsFile;
static std::wstring TestSettingsFile() { return g_settingsFile; }

// Test names for FluentFlyout's and Explorer's, in both directions: fluent_*.cpp asks for and
// compares the real names, the windows on the desktop carry the test names (a running FluentFlyout or
// dock never takes them for its own: FluentFlyout picks a "Shell_SecondaryTrayWnd" for its widget, the
// dock hides Explorer's taskbars).
static const wchar_t kTestTitle[] = L"LiquidDockFluentTest_TaskbarWindow";          // "TaskbarWindow"
static const wchar_t kTestSecondary[] = L"LiquidDockFluentTest_SecondaryTrayWnd";   // "Shell_SecondaryTrayWnd"
static const wchar_t kTestWrapper[] = L"LiquidDockFluentTest_HwndWrapper[";         // "HwndWrapper[..."
static HWND TestFindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR title) {
    if (cls && !IS_INTRESOURCE(cls) && !wcscmp(cls, L"Shell_SecondaryTrayWnd")) cls = kTestSecondary;
    if (title && !wcscmp(title, L"TaskbarWindow")) title = kTestTitle;
    return FindWindowExW(parent, after, cls, title);
}
static int TestGetClassNameW(HWND h, LPWSTR out, int n) {
    wchar_t real[256] = {};
    if (!GetClassNameW(h, real, 256)) return 0;
    std::wstring s = real;
    if (s == kTestSecondary) s = L"Shell_SecondaryTrayWnd";
    else if (!s.compare(0, wcslen(kTestWrapper), kTestWrapper)) s = L"HwndWrapper[" + s.substr(wcslen(kTestWrapper));
    wcsncpy_s(out, n, s.c_str(), _TRUNCATE);
    return (int)wcslen(out);
}
static int TestGetWindowTextW(HWND h, LPWSTR out, int n) {
    wchar_t real[256] = {};
    GetWindowTextW(h, real, 256);
    wcsncpy_s(out, n, !wcscmp(real, kTestTitle) ? L"TaskbarWindow" : real, _TRUNCATE);
    return (int)wcslen(out);
}

#define LD_FLUENT_SETTINGS_FILE TestSettingsFile
#define LD_FLUENT_NO_MEDIA
#define LD_FLUENT_EXE L"fluent_flyout_test.exe"
#define LD_FLUENT_SETTINGS_EVENT L"LiquidDockFluentTest_OpenSettings"   // FluentFlyout's own one opens its settings window
#define FindWindowExW TestFindWindowExW
#define GetClassNameW TestGetClassNameW
#define GetWindowTextW TestGetWindowTextW
#include "../src/fluent_flyout.cpp"
#include "../src/fluent_settings_file.cpp"
#include "../src/fluent_media.cpp"
#include "../src/fluent_placement.cpp"
#undef FindWindowExW
#undef GetClassNameW
#undef GetWindowTextW
#undef ExplorerTaskbar

Settings g_settings;

static std::mutex g_logMu;
static std::vector<std::wstring> g_logLines;   // the host thread logs too
static bool Logged(const wchar_t* part) {
    std::lock_guard<std::mutex> lk(g_logMu);
    for (auto& l : g_logLines) if (l.find(part) != std::wstring::npos) return true;
    return false;
}
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lk(g_logMu);
        g_logLines.push_back(buf);
    }
    char utf8[4096];   // printf only: wide and narrow output on one stream do not mix
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    std::printf("  log: %s\n", utf8);
}
double NowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}
std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static int g_failures = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static void Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

static bool WaitFor(const std::function<bool()>& cond, int ms) {
    for (int i = 0; i < ms / 20; ++i) { Pump(); if (cond()) return true; Sleep(20); }
    return cond();
}

// a window on its own thread with a message loop (FluentFlyout's widget window, the fake Explorer)
struct WindowThread {
    HWND hwnd = nullptr;
    std::thread t;
    DWORD tid = 0;
    void Run(const wchar_t* cls, const wchar_t* title) {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        t = std::thread([this, cls, title, ready] {
            tid = GetCurrentThreadId();
            WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls;
            RegisterClassExW(&wc);
            hwnd = CreateWindowExW(0, cls, title, WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
            SetEvent(ready);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
        });
        WaitForSingleObject(ready, 5000);
        CloseHandle(ready);
    }
    void Quit() {
        if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
        if (t.joinable()) t.join();
        hwnd = nullptr;
    }
};

static void WriteSettings(const std::wstring& body, int secondsAgo) {
    std::string utf8 = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<UserSettings xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, body.c_str(), (int)body.size(), nullptr, 0, nullptr, nullptr);
    std::string b(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, body.c_str(), (int)body.size(), b.data(), n, nullptr, nullptr);
    utf8 += b + "</UserSettings>\r\n";
    HANDLE f = CreateFileW(g_settingsFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &w, nullptr);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{ { ft.dwLowDateTime, ft.dwHighDateTime } };
    u.QuadPart -= (ULONGLONG)secondsAgo * 10000000ULL;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SetFileTime(f, nullptr, nullptr, &ft);
    CloseHandle(f);
}

static void SetSessions(std::vector<std::wstring> ids) {
    std::lock_guard<std::mutex> lk(fluentflyout::g_mediaMu);
    fluentflyout::g_sessions = std::move(ids);
    fluentflyout::g_mediaKnown = true;
    ++fluentflyout::g_mediaVersion;
}

static HWND MakeFlyout(int x, int y, int w, int h) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"TestFlyout";
        RegisterClassExW(&wc);
        registered = true;
    }
    HWND f = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"TestFlyout", L"", WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(f, SW_SHOWNOACTIVATE);
    fluentflyout::OnWinEvent(nullptr, EVENT_OBJECT_SHOW, f, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    return f;
}

static RECT Rect(HWND h) { RECT r{}; GetWindowRect(h, &r); return r; }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    g_settingsFile = std::wstring(tmp) + L"liquiddock_fluent_test_settings.xml";
    g_settings.fluentFlyout = true;
    g_settings.edge = Edge::Bottom;

    // ---- FluentFlyout's settings
    {
        auto s = fluentflyout::ParseSettings(LR"(<?xml version="1.0" encoding="utf-8"?>
<UserSettings xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xsd="http://www.w3.org/2001/XMLSchema">
  <Position>0</Position>
  <FlyoutAnimationSpeed>1</FlyoutAnimationSpeed>
  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>
  <TaskbarWidgetSelectedMonitor>3</TaskbarWidgetSelectedMonitor>
  <TaskbarWidgetHideCompletely>true</TaskbarWidgetHideCompletely>
  <TaskbarWidgetManualPadding>-12</TaskbarWidgetManualPadding>
  <TaskbarVisualizerEnabled>false</TaskbarVisualizerEnabled>
  <AppFilteringEnabled>true</AppFilteringEnabled>
  <AppFilteringMode>0</AppFilteringMode>
  <AllowedApps />
  <BlockedApps>
    <string>Spotify</string>
    <string>Tom &amp; Jerry</string>
  </BlockedApps>
</UserSettings>)");
        Check(s.loaded && s.widgetEnabled && s.hideCompletely && !s.visualizer && s.widgetMonitor == 3 && s.manualPadding == -12 && s.animationSpeed == 1,
              "settings.xml: switches and numbers");
        Check(s.filtering && s.filterMode == 0 && s.allowed.empty() && s.blocked.size() == 2 && s.blocked[0] == L"Spotify" && s.blocked[1] == L"Tom & Jerry",
              "settings.xml: app lists, an empty one and an escaped name");
        auto d = fluentflyout::ParseSettings(L"<UserSettings>\n</UserSettings>");
        Check(d.loaded && !d.widgetEnabled && !d.hideCompletely && d.animationSpeed == 2 && d.widgetMonitor == 0, "missing values: FluentFlyout's defaults");
        Check(!fluentflyout::ParseSettings(L"<html>no</html>").loaded, "another file is not taken for its settings");
    }
    WriteSettings(L"<TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n<TaskbarWidgetHideCompletely>true</TaskbarWidgetHideCompletely>\r\n"
                  L"<FlyoutAnimationSpeed>1</FlyoutAnimationSpeed>\r\n", 60);
    fluentflyout::LoadSettings(NowSeconds(), true);
    Check(fluentflyout::g_ff.loaded && fluentflyout::g_ff.widgetEnabled && fluentflyout::g_ff.hideCompletely, "the settings file is read (UTF-8 with BOM)");
    int version = fluentflyout::g_ffVersion;
    fluentflyout::LoadSettings(NowSeconds() + 5, false);
    Check(fluentflyout::g_ffVersion == version, "an unchanged file is not read again");
    WriteSettings(L"<TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n<TaskbarWidgetHideCompletely>false</TaskbarWidgetHideCompletely>\r\n"
                  L"<FlyoutAnimationSpeed>1</FlyoutAnimationSpeed>\r\n", 30);
    fluentflyout::LoadSettings(NowSeconds() + 10, false);
    Check(fluentflyout::g_ffVersion != version && !fluentflyout::g_ff.hideCompletely, "a changed file is read again");

    // ---- app filter and "something plays"
    {
        auto& ff = fluentflyout::g_ff;
        // named as FluentFlyout names them (the media thread, AppName): Spotify not running, by its id
        const std::wstring spotify = L"Spotify.exe", chrome = L"Google Chrome";
        ff.filtering = true; ff.filterMode = 0; ff.blocked = { L"Spotify", L"  " };
        Check(!fluentflyout::SessionAllowed(L"Spotify.exe", &spotify) && fluentflyout::SessionAllowed(L"Chrome", &chrome), "block list: a listed app is ignored, others count");
        ff.filterMode = 1; ff.allowed = { L"chrome" };
        Check(fluentflyout::SessionAllowed(L"Chrome", &chrome) && !fluentflyout::SessionAllowed(L"Spotify.exe", &spotify), "allow list: only a listed app counts");
        Check(!fluentflyout::AppName(L"fluent_flyout_test.exe").empty(), "an app's name from its running program");
        ff.filtering = false; ff.allowed.clear(); ff.blocked.clear();
        ++fluentflyout::g_ffVersion;
        fluentflyout::g_mediaKnown = false;
        const double t = NowSeconds();
        fluentflyout::UpdateHasMedia(t);
        Check(fluentflyout::g_media.value, "media sessions unknown: counted as playing (nothing hidden that may be there)");
        SetSessions({ L"Spotify.exe" });   // a player's session, then replaced by its next one (a gap)
        fluentflyout::UpdateHasMedia(t + 0.05);
        SetSessions({});
        fluentflyout::UpdateHasMedia(t + 0.1);
        Check(!fluentflyout::g_rawMedia && fluentflyout::g_media.value && fluentflyout::g_media.Waiting(),
              "no media session: held back for a moment (a player replacing its session)");
        SetSessions({ L"Spotify.exe" });
        fluentflyout::UpdateHasMedia(t + 0.3);
        Check(fluentflyout::g_media.value && !fluentflyout::g_media.Waiting(), "a session again within that moment: no flicker");
        SetSessions({});
        fluentflyout::UpdateHasMedia(t + 0.4);
        fluentflyout::UpdateHasMedia(t + 2.0);
        Check(!fluentflyout::g_media.value, "no media session for 1.5 s: nothing plays");
        SetSessions({ L"Spotify.exe" });
        fluentflyout::UpdateHasMedia(t + 2.1);
        Check(fluentflyout::g_media.value, "a media session: something plays at once");
        ff.filtering = true; ff.filterMode = 0; ff.blocked = { L"Spotify" };
        ++fluentflyout::g_ffVersion;
        fluentflyout::UpdateHasMedia(t + 2.2);
        fluentflyout::UpdateHasMedia(t + 3.8);
        Check(!fluentflyout::g_media.value, "...unless FluentFlyout's filter leaves it out");
        ff.filtering = false; ff.blocked.clear();
        ++fluentflyout::g_ffVersion;
        fluentflyout::g_media = {};   // back on the real clock for the rest
    }

    // ---- finding the widget, the slot opening and closing
    WindowThread explorer, widget;
    explorer.Run(L"FakeExplorerTaskbar", L"");
    g_fakeExplorer = explorer.hwnd;
    double now = NowSeconds();
    fluentflyout::Tick(now);
    Check(!fluentflyout::Present() && !fluentflyout::WantsHost(), "not running: nothing to do");
    widget.Run(L"LiquidDockFluentTest_HwndWrapper[FluentFlyout;;test]", kTestTitle);
    fluentflyout::g_nextScan = 0;
    fluentflyout::Tick(NowSeconds());
    Check(fluentflyout::Present() && fluentflyout::g_widget == widget.hwnd, "its widget window is found");
    Check(fluentflyout::WantsHost(), "widget on: the tray host must hold it");
    Check(trayhost::Start(false) && trayhost::Running(), "the host starts without asking for icons");
    trayhost::SetHosting(true, fluentflyout::WidgetWindow());
    HWND host = trayhost::Window();
    const int hostW = fluentflyout::HostWidth(1), hostH = 70;
    RECT place = { 200, 600, 200 + hostW, 600 + hostH };
    trayhost::Place(false, place);
    Check(WaitFor([&] { RECT r = Rect(host); return EqualRect(&r, &place); }, 2000), "the host window waits over the slot");
    // what FluentFlyout does on its timer: into the first Shell_TrayWnd, the taskbar's size, cut to the widget
    SetWindowLongPtrW(widget.hwnd, GWL_STYLE, (GetWindowLongPtrW(widget.hwnd, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
    SetParent(widget.hwnd, FindWindowW(LD_TRAY_CLASS, nullptr));
    SetWindowPos(widget.hwnd, HWND_TOP, 0, 0, hostW, hostH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowRgn(widget.hwnd, CreateRectRgn(20, 15, 210, 55), TRUE);
    SetSessions({});
    fluentflyout::g_ff.hideCompletely = true;
    ++fluentflyout::g_ffVersion;
    fluentflyout::Tick(NowSeconds());
    Check(WaitFor([&] { auto h = trayhost::HostedState(); return h.window == widget.hwnd && h.fitted; }, 2000), "the widget is in the host window, laid out for it");
    auto childAt = [&] {   // the widget's place in the host window
        RECT c = Rect(widget.hwnd), hr = Rect(host);
        return POINT{ c.left - hr.left, c.top - hr.top };
    };
    {   // the host moved while the widget drew (WPF puts it where the host was): put back at the corner,
        // and the dock is told where it goes back to - not where it drifted (it chased that, 2026-11-12)
        bool steady = true;
        SetWindowPos(widget.hwnd, nullptr, -60, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Check(WaitFor([&] {
                  auto h = trayhost::HostedState();
                  if (h.window == widget.hwnd && (h.box.left != 20 || h.box.right != 210)) steady = false;
                  POINT p = childAt();
                  return p.x == 0 && p.y == 0;
              }, 2000), "a widget that drifted off the host's corner is put back");
        Check(steady, "...and meanwhile the dock is told the widget's box where it goes back to");
        Check(Logged(L"the widget was -60,0 off this window's corner"), "...and the log says so");
        auto h = trayhost::HostedState();
        Check(h.fitted && h.box.left == 20 && h.box.right == 210, "...laid out for the host again");
        SetWindowPos(widget.hwnd, nullptr, 40, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Check(WaitFor([&] { POINT p = childAt(); return p.x == 0 && p.y == 0; }, 2000), "...the other way too");
    }
    {   // FluentFlyout's thread busy right after its widget drifted: putting it back must not hold the host
        // (the widget is the host's child, so the host thread's input is joined to the widget's thread and
        // SWP_ASYNCWINDOWPOS from there would wait for it)
        static constexpr UINT kDriftAndBusy = WM_APP + 77;
        SetWindowLongPtrW(widget.hwnd, GWLP_WNDPROC, (LONG_PTR)+[](HWND h, UINT m, WPARAM wp, LPARAM lp) -> LRESULT {
            if (m == kDriftAndBusy) {
                SetWindowPos(h, nullptr, -50, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                Sleep((DWORD)wp);
                return 0;
            }
            return DefWindowProcW(h, m, wp, lp);
        });
        PostMessageW(widget.hwnd, kDriftAndBusy, 900, 0);
        Check(WaitFor([&] { return childAt().x == -50; }, 2000), "fixture: the widget drifted, its thread busy");
        Sleep(150);   // the host has seen it off its place meanwhile
        RECT moved = place;
        OffsetRect(&moved, 30, 0);
        const double asked = NowSeconds();
        trayhost::Place(false, moved);
        const bool followed = WaitFor([&] { RECT r = Rect(host); return EqualRect(&r, &moved); }, 2000);
        const double took = NowSeconds() - asked;
        std::printf("note: the host followed the dock after %.0f ms with the widget's thread busy\n", took * 1000);
        Check(followed && took < 0.4, "...the host still follows the dock at once");
        Check(WaitFor([&] { POINT p = childAt(); return p.x == 0 && p.y == 0; }, 3000), "...and the widget is put back once its thread is free");
        trayhost::Place(false, place);
        Check(WaitFor([&] { RECT r = Rect(host); POINT p = childAt(); return EqualRect(&r, &place) && p.x == 0 && p.y == 0; }, 2000),
              "fixture: back over the slot");
        SetWindowLongPtrW(widget.hwnd, GWLP_WNDPROC, (LONG_PTR)DefWindowProcW);
    }
    {   // the host window's region followed the widget where it drifted: once the widget is back, the region
        // follows at once - not at the host's next check (0.1 s), which left the widget cut meanwhile
        trayhost::Place(true, place);
        auto rgnBox = [&] {
            HRGN r = CreateRectRgn(0, 0, 0, 0);
            RECT b{};
            if (GetWindowRgn(host, r) != ERROR) GetRgnBox(r, &b);
            DeleteObject(r);
            return b;
        };
        auto settled = [&] { RECT b = rgnBox(); return b.left == 20 && b.right == 210; };
        Check(WaitFor(settled, 2000), "fixture: the widget is seen, the host cut to it");
        double worst = 0;
        bool allBack = true;
        for (int round = 0; round < 8; ++round) {
            // drifted (made on the widget's thread at once); only the dock puts it back, after it saw it off
            // and cut the host to where it drifted
            SetWindowPos(widget.hwnd, nullptr, -60, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (childAt().x != -60) { allBack = false; break; }
            const double from = NowSeconds();
            while (childAt().x != 0 && NowSeconds() - from < 2) Sleep(0);
            if (childAt().x != 0) { allBack = false; break; }
            const double back = NowSeconds();
            while (!settled() && NowSeconds() - back < 1) Sleep(0);
            if (!settled()) { allBack = false; break; }
            worst = std::max(worst, NowSeconds() - back);
        }
        std::printf("note: the host's region followed the widget put back after %.0f ms at worst\n", worst * 1000);
        Check(allBack, "fixture: each drift seen and put back");
        // the dock looks 10 ms after a move (up to ~26 ms at the default timer resolution); waiting for its
        // 0.1 s check instead took 95 to 112 ms
        Check(worst < 0.07, "once the widget is put back, the host's region follows at once (not at its next 0.1 s check)");
        trayhost::Place(false, place);
    }
    {   // FluentFlyout's region reads as none for a moment: the region last read stands, not the whole window
        SetWindowRgn(widget.hwnd, nullptr, TRUE);
        bool kept = true;
        const double from = NowSeconds();
        while (NowSeconds() - from < 0.6) {
            auto h = trayhost::HostedState();
            if (h.box.left != 20 || h.box.right != 210) kept = false;
            Sleep(20);
        }
        Check(kept, "a widget whose region is gone for a moment keeps its box");
        Check(WaitFor([&] { auto h = trayhost::HostedState(); return h.box.left == 0 && h.box.right == hostW; }, 2000),
              "...and a widget without a region for longer is all window");
        SetWindowRgn(widget.hwnd, CreateRectRgn(20, 15, 210, 55), TRUE);
        Check(WaitFor([&] { auto h = trayhost::HostedState(); return h.box.left == 20 && h.box.right == 210; }, 2000), "...its region again");
    }
    {   // another program's taskbar widget moves in too: it stays cut away
        WindowThread other;
        other.Run(L"OtherTaskbarWidget", L"Lyrics");
        SetWindowLongPtrW(other.hwnd, GWL_STYLE, (GetWindowLongPtrW(other.hwnd, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
        SetParent(other.hwnd, host);
        SetWindowPos(other.hwnd, HWND_TOP, 400, 0, 200, 70, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Sleep(300);
        auto h = trayhost::HostedState();
        Check(h.window == widget.hwnd && h.box.left == 20 && h.box.right == 210, "another program's widget in the host window is not shown");
        other.Quit();
    }
    Check(WaitFor([&] { fluentflyout::Tick(NowSeconds()); return !fluentflyout::g_media.value; }, 3000), "nothing playing for 1.5 s");
    auto w = fluentflyout::WidgetState();
    Check(!w.open && w.monitor != nullptr, "hide completely, nothing playing: the slot stays closed");
    Check(fluentflyout::WidgetMonitor() == w.monitor && fluentflyout::WidgetMonitorNumber() == 1, "the menu names FluentFlyout's monitor \"1\"");
    SetSessions({ L"Spotify.exe" });
    fluentflyout::Tick(NowSeconds());
    w = fluentflyout::WidgetState();
    Check(w.open && w.fitted && w.box.left == 20 && w.box.right == 210 && w.box.top == 15 && w.box.bottom == 55, "media playing: the slot opens as wide as the widget");
    fluentflyout::g_ff.hideCompletely = false;
    SetSessions({});
    ++fluentflyout::g_ffVersion;
    fluentflyout::Tick(NowSeconds());
    Check(fluentflyout::WidgetState().open, "without hide completely the idle widget keeps its slot");
    ShowWindow(widget.hwnd, SW_HIDE);   // FluentFlyout's "hide when paused", or no Premium
    Check(WaitFor([&] { return !fluentflyout::WidgetState().open; }, 2000), "a hidden widget closes the slot");
    ShowWindow(widget.hwnd, SW_SHOWNA);
    Check(WaitFor([&] { return fluentflyout::WidgetState().open; }, 2000), "...and it opens again");
    fluentflyout::g_ff.widgetEnabled = false;
    Check(!fluentflyout::WantsHost() && !fluentflyout::WidgetState().open, "widget switched off in FluentFlyout: no host needed");
    fluentflyout::g_ff.widgetEnabled = true;
    g_settings.edge = Edge::Left;
    Check(!fluentflyout::WantsHost(), "a dock on a side edge does not take the widget");
    g_settings.edge = Edge::Bottom;
    g_settings.fluentFlyout = false;
    fluentflyout::Tick(NowSeconds());
    Check(!fluentflyout::Present() && !fluentflyout::WantsHost(), "integration off: as if it were not running");
    g_settings.fluentFlyout = true;
    fluentflyout::g_nextScan = 0;
    fluentflyout::Tick(NowSeconds());
    Check(fluentflyout::Present() && fluentflyout::g_widget == widget.hwnd, "integration on again: found inside the host window");

    // ---- flyouts next to a shown dock
    fluentflyout::Unhook();   // events are fed by hand below (deterministic)
    fluentflyout::g_ff.animationSpeed = 1;   // 0.15 s
    MONITORINFO mi = { sizeof(mi) };
    HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(primary, &mi);
    const RECT mon = mi.rcMonitor;
    fluentflyout::DockView v;
    v.monitor = primary; v.edge = Edge::Bottom; v.shown = true; v.scale = 1;
    v.bar = { mon.left + 150, mon.bottom - 76, mon.right - 150, mon.bottom - 6 };
    v.slot = true;
    v.slotRect = { mon.left + 170, v.bar.top, mon.left + 370, v.bar.bottom };
    const int slotMid = (v.slotRect.left + v.slotRect.right) / 2;
    HWND media = MakeFlyout(mon.left + 16, mon.bottom - 16 - 116, 310, 116);   // FluentFlyout's bottom-left place: across the glass
    bool raised = fluentflyout::PlaceFlyouts(v, NowSeconds());
    Pump();
    RECT r = Rect(media);
    Check(raised, "a flyout opening across the glass: the dock goes on top of it");
    Check((r.left + r.right) / 2 == slotMid && r.bottom == mon.bottom - 16, "at once along the dock to the widget, its own height kept while it animates");
    Check(fluentflyout::Busy(), "frames at display rate meanwhile");
    double until = NowSeconds() + 1.5;
    while (NowSeconds() < until) { fluentflyout::PlaceFlyouts(v, NowSeconds()); Pump(); Sleep(8); }
    r = Rect(media);
    Check(r.bottom == v.bar.top - 12 && (r.left + r.right) / 2 == slotMid, "after its animation: clear of the glass, above the widget");
    Check(!fluentflyout::Busy() && fluentflyout::FlyoutAt(primary, { (r.left + r.right) / 2, (r.top + r.bottom) / 2 }),
          "settled; the pointer on it keeps the dock up");
    // FluentFlyout opens its volume flyout above where it put the media flyout: the stack moves together
    HWND volume = MakeFlyout(mon.left + 16 + 35, mon.bottom - 16 - 116 - 8 - 50, 240, 50);
    until = NowSeconds() + 1.2;
    while (NowSeconds() < until) { fluentflyout::PlaceFlyouts(v, NowSeconds()); Pump(); Sleep(8); }
    RECT rv = Rect(volume);
    r = Rect(media);
    Check(rv.bottom == r.top - 8 && (rv.left + rv.right) / 2 == slotMid, "a flyout stacked above it moves as far, keeping the stack");
    // FluentFlyout closes it (moves it itself): its business from then on
    Sleep(250);
    SetWindowPos(media, nullptr, r.left, r.top + 20, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    fluentflyout::OnWinEvent(nullptr, EVENT_OBJECT_LOCATIONCHANGE, media, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    fluentflyout::PlaceFlyouts(v, NowSeconds());
    Check(!fluentflyout::FlyoutAt(primary, { (r.left + r.right) / 2, r.top + 40 }) && Rect(media).top == r.top + 20, "a flyout FluentFlyout moves again is left to it");
    fluentflyout::OnWinEvent(nullptr, EVENT_OBJECT_HIDE, media, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    fluentflyout::OnWinEvent(nullptr, EVENT_OBJECT_HIDE, volume, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    DestroyWindow(media); DestroyWindow(volume);
    fluentflyout::PlaceFlyouts(v, NowSeconds());
    Check(fluentflyout::g_lift[primary] == 0, "with the stack gone the next flyout starts afresh");
    // a hidden dock: FluentFlyout's own place
    v.shown = false;
    HWND next = MakeFlyout(mon.left + 16, mon.bottom - 16 - 50, 300, 50);
    until = NowSeconds() + 0.6;
    while (NowSeconds() < until) { fluentflyout::PlaceFlyouts(v, NowSeconds()); Pump(); Sleep(8); }
    Check(Rect(next).left == mon.left + 16 && Rect(next).bottom == mon.bottom - 16 && !fluentflyout::Busy(), "hidden dock: the flyout stays where FluentFlyout put it");
    // the dock comes up while it is open: it settles next to the dock then
    v.shown = true;
    until = NowSeconds() + 0.8;
    while (NowSeconds() < until) { fluentflyout::PlaceFlyouts(v, NowSeconds()); Pump(); Sleep(8); }
    r = Rect(next);
    Check(r.bottom == v.bar.top - 12 && (r.left + r.right) / 2 == slotMid, "...and moves next to the dock when it comes up");
    fluentflyout::OnWinEvent(nullptr, EVENT_OBJECT_HIDE, next, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    DestroyWindow(next);
    // a flyout set to the top of the screen stays there
    HWND top = MakeFlyout(mon.left + 16, mon.top + 16, 310, 116);
    until = NowSeconds() + 0.6;
    while (NowSeconds() < until) { fluentflyout::PlaceFlyouts(v, NowSeconds()); Pump(); Sleep(8); }
    Check(Rect(top).left == mon.left + 16 && Rect(top).top == mon.top + 16, "a flyout at the top of the screen is not moved");
    fluentflyout::OnWinEvent(nullptr, EVENT_OBJECT_HIDE, top, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    DestroyWindow(top);
    // menus and tooltips (layered pop-ups) are not flyouts
    {
        HWND menu = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"TestFlyout", L"", WS_POPUP, mon.left + 300, mon.bottom - 60, 200, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetLayeredWindowAttributes(menu, 0, 255, LWA_ALPHA);
        ShowWindow(menu, SW_SHOWNOACTIVATE);
        Check(!fluentflyout::IsFlyoutWindow(menu), "a layered pop-up (menu, tooltip) is not a flyout");
        DestroyWindow(menu);
    }

    // ---- its settings window, from the dock's menu (an event of the test's name: a running FluentFlyout
    // would open its settings window on its own one, and take the signal before this test sees it)
    {
        Check(!wcscmp(fluentflyout::kSettingsEvent, L"FluentFlyout_OpenSettings"), "the dock sets FluentFlyout's event (its name in FluentFlyout's MainWindow)");
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, LD_FLUENT_SETTINGS_EVENT);   // what FluentFlyout waits on, under the test's name
        // without the event OpenSettings would start "FluentFlyout" again: this test
        Check(ev && fluentflyout::OpenSettings() && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "\"open its settings\" sets FluentFlyout's event");
        if (ev) CloseHandle(ev);
    }

    // ---- FluentFlyout puts its widget into one of Explorer's taskbars: that is the monitor it is set to
    {
        WindowThread secondary;
        secondary.Run(kTestSecondary, L"");
        fluentflyout::g_fileSetting = 0; fluentflyout::g_fileAt = NowSeconds() - 5;   // the file was read earlier
        SetParent(widget.hwnd, secondary.hwnd);
        fluentflyout::Tick(NowSeconds());
        HMONITOR there = MonitorFromWindow(secondary.hwnd, MONITOR_DEFAULTTONEAREST);
        Check(fluentflyout::g_parentKind == fluentflyout::ParentKind::Explorer && fluentflyout::g_shownMonitor == there &&
              fluentflyout::g_shownAt > fluentflyout::g_fileAt, "an Explorer taskbar as its parent tells FluentFlyout's monitor");
        Check(fluentflyout::WidgetMonitor() == there && fluentflyout::FindWidget() == widget.hwnd, "...the slot goes there; the widget is still found");
        SetParent(widget.hwnd, FindWindowW(LD_TRAY_CLASS, nullptr));   // FluentFlyout's next tick: into the dock's window
        SetWindowPos(widget.hwnd, HWND_TOP, 0, 0, hostW, hostH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        fluentflyout::Tick(NowSeconds());
        Check(fluentflyout::g_parentKind == fluentflyout::ParentKind::Host, "back in the dock's window");
        secondary.Quit();
    }

    // ---- FluentFlyout makes its widget window again (displays or work area changed): the slot keeps its place
    Check(WaitFor([&] {
              fluentflyout::Tick(NowSeconds());
              trayhost::SetHosting(fluentflyout::WantsHost(), fluentflyout::WidgetWindow());   // as the main loop does
              return fluentflyout::WidgetState().open;
          }, 3000), "the slot is open before");
    HWND oldWidget = widget.hwnd;
    widget.Quit();   // FluentFlyout closes its widget window...
    fluentflyout::Tick(NowSeconds());
    Check(fluentflyout::Present() && fluentflyout::WantsHost() && fluentflyout::WidgetState().open && fluentflyout::WidgetWindow() == oldWidget,
          "its widget window closed while FluentFlyout runs: the slot keeps its place (nothing shows meanwhile)");
    WindowThread widget2;   // ...and makes a new one, into the first Shell_TrayWnd
    widget2.Run(L"LiquidDockFluentTest_HwndWrapper[FluentFlyout;;test2]", kTestTitle);
    SetWindowLongPtrW(widget2.hwnd, GWL_STYLE, (GetWindowLongPtrW(widget2.hwnd, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
    SetParent(widget2.hwnd, FindWindowW(LD_TRAY_CLASS, nullptr));
    SetWindowPos(widget2.hwnd, HWND_TOP, 0, 0, hostW, hostH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowRgn(widget2.hwnd, CreateRectRgn(20, 15, 210, 55), TRUE);
    bool stayedOpen = true;
    const double madeAt = NowSeconds();
    bool adopted = WaitFor([&] {
        fluentflyout::Tick(NowSeconds());
        trayhost::SetHosting(fluentflyout::WantsHost(), fluentflyout::WidgetWindow());
        stayedOpen = stayedOpen && fluentflyout::WidgetState().open;
        return fluentflyout::g_widget == widget2.hwnd && trayhost::HostedState().window == widget2.hwnd;
    }, 2000);
    Check(adopted && NowSeconds() - madeAt < 1.0, "the new widget window is taken at once and shown in the host window");
    Check(stayedOpen, "...and the slot never closed in between");

    // ---- FluentFlyout's widget does not come back, or it quits
    trayhost::Stop();
    Check(IsWindow(widget2.hwnd) && GetParent(widget2.hwnd) == explorer.hwnd, "the host stopping hands the widget back to Explorer's taskbar");
    widget2.Quit();
    fluentflyout::Tick(NowSeconds());
    Check(fluentflyout::Present() && !fluentflyout::g_widget, "its widget window gone: a new one is awaited for a moment");
    Check(WaitFor([&] { fluentflyout::Tick(NowSeconds()); return !fluentflyout::Present(); }, 5000), "none within 3 s: no longer present");
    fluentflyout::Shutdown();
    explorer.Quit();
    DeleteFileW(g_settingsFile.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, LD_TRAY_SETTINGS_PARENT);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
