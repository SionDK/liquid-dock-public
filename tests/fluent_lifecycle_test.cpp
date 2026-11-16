// FluentFlyout module (src/fluent_*.cpp) over its life with FluentFlyout, against fakes:
//   - its settings files: the newest of several, the 30 s path cache, files that are not its settings
//     (another file, empty, too big), the file gone for a while and back, non-ASCII app names
//   - the widget's monitor from the file and from where FluentFlyout put the widget, on a layout of a
//     main monitor and another one (the real main monitor and a made-up one)
//   - "hide completely when nothing plays" when the media sessions are first known; its app filter by
//     FluentFlyout's names for the apps (the parts of the id, the running programs, the shell)
//   - the media thread's rounds: no wake-ups while the slot does not follow the sessions, a look at once
//     when it does again
//   - its flyouts next to a shown dock at the real DPI (100% or 150%), closed and opened again (hidden
//     and shown by FluentFlyout's open animation)
//   - FluentFlyout restarted and updated (other processes: this test started again as a fake
//     FluentFlyout), handles kept steady
// Nothing here looks like FluentFlyout or a taskbar to other programs (the running dock included):
// the class names and the title are test names that fluent_*.cpp sees as the real ones through
// the redirections below; the tray host is a stub (HostedState is set by hand); flyout windows are
// cloaked (shown for the module, invisible on screen).
// cl /std:c++20 /EHsc /utf-8 tests\fluent_lifecycle_test.cpp user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib version.lib dwmapi.lib
#include "../src/explorer_taskbar.h"
#include "../src/tray_host.h"
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <atomic>
#include <mutex>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <thread>

// ---------------------------------------------------------------- fakes
// the tray host: its window and what it holds, set by the test
namespace trayhost {
HWND g_stubWindow = nullptr;
Hosted g_stubHosted;
HWND Window() { return g_stubWindow; }
Hosted HostedState() { return g_stubHosted; }
bool ProcessElevated() { return false; }
}

static HWND g_fakeExplorer = nullptr;
static HWND HuntExplorerTaskbar() { return g_fakeExplorer; }

static std::vector<std::wstring> g_settingsPaths;
static int g_settingsLookups = 0;
static std::vector<std::wstring> HuntSettingsFiles() { ++g_settingsLookups; return g_settingsPaths; }

// Test names for the real ones, in both directions: fluent_*.cpp asks for and compares the real
// names, the windows on the desktop carry the test names.
static const wchar_t kHuntTitle[] = L"HuntFluent_TaskbarWindow";            // "TaskbarWindow"
static const wchar_t kHuntSecondary[] = L"HuntFluent_SecondaryTrayWnd";     // "Shell_SecondaryTrayWnd"
static const wchar_t kHuntMainTray[] = L"HuntFluent_ExplorerTrayWnd";       // "Shell_TrayWnd"
static const wchar_t kHuntWrapper[] = L"HuntFluent_HwndWrapper[";           // "HwndWrapper[..."

static HWND HuntFindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR title) {
    if (cls && !IS_INTRESOURCE(cls) && !wcscmp(cls, L"Shell_SecondaryTrayWnd")) cls = kHuntSecondary;
    if (title && !wcscmp(title, L"TaskbarWindow")) title = kHuntTitle;
    return FindWindowExW(parent, after, cls, title);
}
static int HuntGetClassNameW(HWND h, LPWSTR out, int n) {
    wchar_t real[256] = {};
    if (!GetClassNameW(h, real, 256)) return 0;
    std::wstring s = real;
    if (s == kHuntSecondary) s = L"Shell_SecondaryTrayWnd";
    else if (s == kHuntMainTray) s = L"Shell_TrayWnd";
    else if (!s.compare(0, wcslen(kHuntWrapper), kHuntWrapper)) s = L"HwndWrapper[" + s.substr(wcslen(kHuntWrapper));
    wcsncpy_s(out, n, s.c_str(), _TRUNCATE);
    return (int)wcslen(out);
}
static int HuntGetWindowTextW(HWND h, LPWSTR out, int n) {
    wchar_t real[256] = {};
    GetWindowTextW(h, real, 256);
    wcsncpy_s(out, n, !wcscmp(real, kHuntTitle) ? L"TaskbarWindow" : real, _TRUNCATE);
    return (int)wcslen(out);
}

#define ExplorerTaskbar HuntExplorerTaskbar
#define LD_FLUENT_SETTINGS_FILE HuntSettingsFiles
#define LD_FLUENT_NO_MEDIA
#define LD_FLUENT_MEDIA_CHECK_MS 100   // the media thread's rounds (MediaRounds, run below with a fake list): 5 s in the dock
#define LD_FLUENT_EXE L"fluent_lifecycle_test.exe"
#define FindWindowExW HuntFindWindowExW
#define GetClassNameW HuntGetClassNameW
#define GetWindowTextW HuntGetWindowTextW
#include "../src/fluent_flyout.cpp"
#include "../src/fluent_settings_file.cpp"
#include "../src/fluent_media.cpp"
#include "../src/fluent_placement.cpp"
#undef FindWindowExW
#undef GetClassNameW
#undef GetWindowTextW
#undef ExplorerTaskbar

Settings g_settings;

static std::atomic<int> g_playingLogs{ 0 };   // "media playing" lines written
static std::atomic<int> g_mediaLogs{ 0 };     // "media playing" and "nothing playing" lines
static std::atomic<int> g_slotLogs{ 0 };      // "widget slot open / closed" lines

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (!wcscmp(buf, L"fluentflyout: media playing")) ++g_playingLogs;
    if (!wcscmp(buf, L"fluentflyout: media playing") || !wcscmp(buf, L"fluentflyout: nothing playing")) ++g_mediaLogs;
    if (!wcsncmp(buf, L"fluentflyout: widget slot ", 26)) ++g_slotLogs;
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

// ---------------------------------------------------------------- helpers
static int g_failures = 0, g_checks = 0;
static bool Check(bool ok, const char* what) {
    ++g_checks;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
    return ok;
}

static void Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

static bool WaitFor(const std::function<bool()>& cond, int ms) {
    for (int i = 0; i < ms / 20; ++i) { Pump(); if (cond()) return true; Sleep(20); }
    return cond();
}

// a hidden window on its own thread with a message loop (a fake FluentFlyout widget, a fake taskbar)
struct WindowThread {
    HWND hwnd = nullptr;
    std::thread t;
    DWORD tid = 0;
    std::wstring cls, title;
    void Run(const std::wstring& c, const std::wstring& ti) {
        cls = c; title = ti;
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        t = std::thread([this, ready] {
            tid = GetCurrentThreadId();
            WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls.c_str();
            RegisterClassExW(&wc);
            hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls.c_str(), title.c_str(), WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
            SetEvent(ready);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
            if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
        });
        WaitForSingleObject(ready, 5000);
        CloseHandle(ready);
    }
    void Quit() {
        if (!t.joinable()) return;
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
        t.join();
        hwnd = nullptr;
    }
    ~WindowThread() { Quit(); }
};

static void MakeChild(HWND w, HWND parent) {
    SetWindowLongPtrW(w, GWL_STYLE, (GetWindowLongPtrW(w, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
    SetParent(w, parent);
}

static void WriteFileBytes(const std::wstring& path, const std::string& bytes, int secondsAgo) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    if (!bytes.empty()) WriteFile(f, bytes.data(), (DWORD)bytes.size(), &w, nullptr);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{ { ft.dwLowDateTime, ft.dwHighDateTime } };
    u.QuadPart -= (ULONGLONG)secondsAgo * 10000000ULL;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SetFileTime(f, nullptr, nullptr, &ft);
    CloseHandle(f);
}

static std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string b(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), b.data(), n, nullptr, nullptr);
    return b;
}

// FluentFlyout's settings.xml as XmlSerializer writes it (bom: as XmlWriter does; StreamWriter does not)
static void WriteSettings(const std::wstring& path, const std::wstring& body, int secondsAgo, bool bom = true) {
    std::string xml = std::string(bom ? "\xEF\xBB\xBF" : "") + "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                      "<UserSettings xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\r\n" +
                      Utf8(body) + "</UserSettings>\r\n";
    WriteFileBytes(path, xml, secondsAgo);
}

static std::wstring Monitor(int n) { return L"  <TaskbarWidgetSelectedMonitor>" + std::to_wstring(n) + L"</TaskbarWidgetSelectedMonitor>\r\n"; }

static void SetSessions(std::vector<std::wstring> ids) {
    std::lock_guard<std::mutex> lk(fluentflyout::g_mediaMu);
    fluentflyout::g_sessions = std::move(ids);
    fluentflyout::g_mediaKnown = true;
    ++fluentflyout::g_mediaVersion;
}

// a flyout of "FluentFlyout": a top-level window of flyout size, shown for the module but cloaked
static HWND MakeFlyout(int x, int y, int w, int h) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"HuntFluent_Flyout";
        RegisterClassExW(&wc);
        registered = true;
    }
    HWND f = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"HuntFluent_Flyout", L"", WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(f, DWMWA_CLOAK, &cloak, sizeof(cloak));
    ShowWindow(f, SW_SHOWNOACTIVATE);
    fluentflyout::OnWinEvent(nullptr, EVENT_OBJECT_SHOW, f, OBJID_WINDOW, CHILDID_SELF, 0, 0);
    return f;
}

static RECT Rect(HWND h) { RECT r{}; GetWindowRect(h, &r); return r; }

// FluentFlyout moves its flyout (its animation): the module hears of it as from its hook
static void FluentMoves(HWND f, int x, int y) {
    SetWindowPos(f, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    fluentflyout::OnWinEvent(nullptr, EVENT_OBJECT_LOCATIONCHANGE, f, OBJID_WINDOW, CHILDID_SELF, 0, 0);
}

static void PlaceFor(const fluentflyout::DockView& v, double seconds) {
    const double until = NowSeconds() + seconds;
    while (NowSeconds() < until) { fluentflyout::PlaceFlyouts(v, NowSeconds()); Pump(); Sleep(8); }
}

// ---------------------------------------------------------------- a fake FluentFlyout process
// This test started again: a hidden widget window ("TaskbarWindow", a WPF class name) until it is closed;
// then it lingers (an app that closes its windows before it exits) and leaves.
static int ChildMain(const wchar_t* id, int lingerMs) {
    std::wstring cls = std::wstring(kHuntWrapper) + L"child-" + id + L"]";
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_TIMER) { DestroyWindow(h); return 0; }
        if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(h, m, w, l);
    };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, cls.c_str(), kHuntTitle, WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) return 1;
    SetTimer(w, 1, 30000, nullptr);   // never outlives the test by much
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    Sleep(lingerMs);
    return 0;
}

struct Child {
    PROCESS_INFORMATION pi{};
    HWND widget = nullptr;
    bool Start(const wchar_t* id, int lingerMs) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --hunt-fluent-widget " + id + L" " + std::to_wstring(lingerMs);
        STARTUPINFOW si = { sizeof(si) };
        if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
        std::wstring cls = std::wstring(kHuntWrapper) + L"child-" + id + L"]";
        for (int i = 0; i < 250 && !widget; ++i) { widget = FindWindowW(cls.c_str(), nullptr); if (!widget) Sleep(20); }
        return widget != nullptr;
    }
    void CloseWidget() { if (widget) PostMessageW(widget, WM_CLOSE, 0, 0); }
    bool WaitExit(int ms) { return pi.hProcess && WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0; }
    void Release() {
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        pi = {};
    }
};

static bool TickUntil(const std::function<bool()>& cond, int ms) {
    return WaitFor([&] { fluentflyout::Tick(NowSeconds()); return cond(); }, ms);
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && !wcscmp(argv[1], L"--hunt-fluent-widget")) return ChildMain(argv[2], _wtoi(argv[3]));
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // as the dock
    g_settings.fluentFlyout = true;
    g_settings.edge = Edge::Bottom;

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\') + 1);   // build\tests\hunt-fluent\ (temp files next to the test)
    const std::wstring fileA = dir + L"hunt-fluent-settings-a.xml", fileB = dir + L"hunt-fluent-settings-b.xml";
    DeleteFileW(fileA.c_str()); DeleteFileW(fileB.c_str());
    g_settingsPaths = { fileA, fileB };

    // ================================================================ the settings files
    {
        using namespace fluentflyout;
        const double t = NowSeconds() + 1000;   // a clock of its own (reset below)
        WriteSettings(fileA, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n" + Monitor(1), 120);
        g_settingsFilesAt = -1; g_settingsLookups = 0;
        LoadSettings(t, true);
        Check(g_ff.loaded && g_ff.widgetEnabled && g_ff.widgetMonitor == 1 && g_ffPath == fileA && g_fileSetting == 1 && g_fileAt == t,
              "one settings file: read, its monitor setting noted as new");
        int version = g_ffVersion;
        WriteSettings(fileB, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n" + Monitor(0), 60);
        LoadSettings(t + 2, false);
        Check(g_ffPath == fileB && g_ff.widgetMonitor == 0 && g_ffVersion != version && g_fileAt == t + 2,
              "two of them (classic and Store): the newer one counts");
        WriteSettings(fileA, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n" + Monitor(1), 10);
        LoadSettings(t + 3, false);
        Check(g_ffPath == fileB, "not looked at again before 2 s");
        LoadSettings(t + 4, false);
        Check(g_ffPath == fileA && g_ff.widgetMonitor == 1, "the other one saved since: it counts now");
        version = g_ffVersion;
        for (double s = 6; s <= 28; s += 2) LoadSettings(t + s, false);
        Check(g_ffVersion == version && g_settingsLookups == 1, "unchanged: not read again; where the files are looked up once in 30 s");
        LoadSettings(t + 31, false);
        Check(g_settingsLookups == 2, "...and again after 30 s");

        // files that are not its settings: the values read last stay
        WriteFileBytes(fileB, "<html><body>not FluentFlyout's</body></html>", 5);
        LoadSettings(t + 34, false);
        Check(g_ffPath == fileA && g_ff.widgetMonitor == 1 && g_ffVersion == version, "a newer file that is not its settings is not taken");
        WriteFileBytes(fileB, "", 4);
        LoadSettings(t + 36, false);
        Check(g_ffPath == fileA && g_ffVersion == version, "an empty file is not taken");
        {
            std::string big = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<UserSettings>\r\n<TaskbarWidgetEnabled>false</TaskbarWidgetEnabled>\r\n";
            big.append((8u << 20) - big.size(), ' ');
            big += "</UserSettings>\r\n";
            WriteFileBytes(fileB, big, 3);
        }
        LoadSettings(t + 38, false);
        Check(g_ffPath == fileA && g_ff.widgetEnabled && g_ffVersion == version, "a file of 8 MB or more is not read");

        // UTF-8 without a byte order mark (StreamWriter's default), Korean app names
        WriteSettings(fileB, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n  <AppFilteringEnabled>true</AppFilteringEnabled>\r\n"
                             L"  <BlockedApps>\r\n    <string>멜론</string>\r\n    <string>Tom &amp; Jerry</string>\r\n  </BlockedApps>\r\n" + Monitor(1), 2, false);
        LoadSettings(t + 40, false);
        Check(g_ffPath == fileB && g_ff.filtering && g_ff.blocked.size() == 2 && g_ff.blocked[0] == L"멜론" && g_ff.blocked[1] == L"Tom & Jerry",
              "UTF-8 without a byte order mark; Korean names and entities in the lists");
        Check(g_fileAt == t + 4, "the same monitor setting in another file: not noted as new (it decides only when it changes)");
        version = g_ffVersion;
        LoadSettings(t + 42, false);
        Check(g_fileAt == t + 4 && g_ffVersion == version, "...and nothing changes while the file stays");

        // the file gone (FluentFlyout replaces it when saving: a moment without it), then for good
        DeleteFileW(fileA.c_str()); DeleteFileW(fileB.c_str());
        LoadSettings(t + 44, false);
        const int lookups = g_settingsLookups;
        LoadSettings(t + 46, false);
        LoadSettings(t + 48, false);
        Check(g_ff.loaded && g_ff.filtering, "gone for a moment: its values are kept");
        Check(g_settingsLookups == lookups + 2, "while no file exists the folders are looked up at every check (the Store folder may appear)");
        LoadSettings(t + 54.5, false);
        Check(!g_ff.loaded && g_fileSetting == -1 && g_ffPath.empty() && g_choiceDirty, "gone for more than 10 s: FluentFlyout's defaults, its monitor no longer known");
        WriteSettings(fileA, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n" + Monitor(1), 1);
        LoadSettings(t + 56.5, false);
        Check(g_ff.loaded && g_fileSetting == 1 && g_fileAt == t + 56.5, "back: read at once, its monitor noted as new");
        // back on the real clock for the rest, nothing read yet
        g_nextSettingsCheck = 0; g_settingsFilesAt = -1; g_ffMissingSince = -1;
        g_ff = {}; g_ffPath.clear(); g_ffTime = {}; g_fileSetting = -1; g_fileAt = -1; g_choiceDirty = true;
    }

    // ================================================================ the widget's monitor
    // The real main monitor and a made-up one (only its number matters to the module): a two-monitor
    // layout, a portrait monitor left of the main one at negative coordinates.
    HMONITOR realMain = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(realMain, &mi);
    const RECT mon = mi.rcMonitor;
    const HMONITOR portrait = (HMONITOR)(UINT_PTR)0x48F1;
    auto useLayout = [&] {
        fluentflyout::g_monitorHandles = { portrait, realMain };   // the system lists the portrait one first
        fluentflyout::g_monitorDescs = { { false, mon.left - 1440 }, { true, mon.left } };
        fluentflyout::g_monitorsAt = NowSeconds() + 1e6;   // kept: RefreshMonitors would read the real ones
        fluentflyout::g_choiceDirty = true;
    };
    WindowThread mainTray, widget;
    mainTray.Run(kHuntMainTray, L"");
    g_fakeExplorer = mainTray.hwnd;
    widget.Run(std::wstring(kHuntWrapper) + L"in-process]", kHuntTitle);
    {
        useLayout();
        WriteSettings(fileA, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n" + Monitor(1), 1);
        fluentflyout::g_nextScan = 0;
        fluentflyout::Tick(NowSeconds());
        Check(fluentflyout::g_widget == widget.hwnd && fluentflyout::g_pid == GetCurrentProcessId(), "its widget window on its own (starting): found");
        Check(fluentflyout::WidgetMonitor() == portrait && fluentflyout::WidgetMonitorNumber() == 2,
              "its setting \"2\" with the portrait monitor listed first: the portrait monitor (main first, then left to right)");
        WriteSettings(fileA, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n" + Monitor(0), 0);
        fluentflyout::g_nextSettingsCheck = 0;
        fluentflyout::Tick(NowSeconds());
        Check(fluentflyout::WidgetMonitor() == realMain && fluentflyout::WidgetMonitorNumber() == 1, "setting \"1\": the main monitor");
        Sleep(30);   // a later write time
        WriteSettings(fileA, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n" + Monitor(7), 0);
        fluentflyout::g_nextSettingsCheck = 0;
        fluentflyout::Tick(NowSeconds());
        Check(fluentflyout::WidgetMonitor() == portrait, "a setting beyond the monitors: the last one (FluentFlyout clamps)");

        // Explorer shows its taskbar on the main display only (no secondary taskbar): FluentFlyout, set to
        // the portrait monitor, falls back to the first Shell_TrayWnd - Explorer's main one while the dock's
        // host window is not up (the dock starting). That is no sign of FluentFlyout's monitor.
        fluentflyout::Forget(L"test: next case");
        Sleep(30);
        WriteSettings(fileA, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n" + Monitor(1), 0);
        MakeChild(widget.hwnd, mainTray.hwnd);
        useLayout();
        fluentflyout::g_nextScan = 0;
        fluentflyout::Tick(NowSeconds());
        Check(fluentflyout::g_widget == widget.hwnd && fluentflyout::g_parentKind == fluentflyout::ParentKind::Explorer,
              "its widget in Explorer's main taskbar: found there");
        Check(fluentflyout::WidgetMonitor() == portrait,
              "no secondary taskbar: its widget in Explorer's main taskbar is FluentFlyout's fallback - the file's monitor (\"2\") holds the slot");
        // Explorer's taskbar on the other display too: set to that one, FluentFlyout would have put its widget
        // into the secondary taskbar - in Explorer's main taskbar it is a sure answer (looked at again when the
        // displays change, the widget not moving)
        WindowThread secondary;
        secondary.Run(kHuntSecondary, L"");
        fluentflyout::g_choiceDirty = true;   // as RefreshMonitors marks a change of the displays
        fluentflyout::Tick(NowSeconds());
        Check(fluentflyout::WidgetMonitor() == realMain && fluentflyout::g_shownMonitor == realMain,
              "taskbars on all displays: its widget in Explorer's main taskbar means the main monitor");
        // in a secondary taskbar: always a sure answer (FluentFlyout takes one only on its monitor)
        fluentflyout::g_shownMonitor = nullptr;
        SetParent(widget.hwnd, secondary.hwnd);
        fluentflyout::Tick(NowSeconds());
        Check(fluentflyout::g_parentKind == fluentflyout::ParentKind::Explorer && fluentflyout::g_shownMonitor == realMain,
              "its widget in a secondary taskbar: that taskbar's monitor");
        SetParent(widget.hwnd, mainTray.hwnd);   // not destroyed with that window
        secondary.Quit();
        fluentflyout::Forget(L"test: next case");
        fluentflyout::g_monitorsAt = -1;   // the real monitors again
    }

    // ================================================================ hide completely, media first known
    {
        using namespace fluentflyout;
        g_nextScan = 0;
        Tick(NowSeconds());
        g_ff.loaded = true; g_ff.widgetEnabled = true; g_ff.hideCompletely = true; g_ff.filtering = false;
        ++g_ffVersion;
        trayhost::g_stubHosted.window = g_widget;
        trayhost::g_stubHosted.box = { 20, 15, 210, 55 };
        trayhost::g_stubHosted.fitted = true;
        g_mediaKnown = false;
        g_media = {};
        const double t = NowSeconds();
        UpdateHasMedia(t);
        Check(g_widget && WidgetState().open, "media sessions not known yet: the slot may open (nothing hidden that may be there)");
        SetSessions({});   // the media thread's first list, a moment after FluentFlyout was found
        UpdateHasMedia(t + 0.05);
        Check(!g_media.value && !WidgetState().open,
              "the first session list says nothing plays: the slot closes at once (no empty slot for 1.5 s while FluentFlyout hides its widget)");
        SetSessions({ L"Spotify.exe" });
        UpdateHasMedia(t + 0.1);
        Check(g_media.value && WidgetState().open, "something plays: open at once");
        SetSessions({});
        UpdateHasMedia(t + 0.2);
        Check(g_media.value && g_media.Waiting() && WidgetState().open, "a session replaced (a gap): held back, the slot stays");
        UpdateHasMedia(t + 1.8);
        Check(!g_media.value && !WidgetState().open, "nothing for 1.5 s: closed");
        // the filter as FluentFlyout matches it: a name in full, any part of the id, entries not trimmed
        // (Spotify not running, the shell not knowing the id: FluentFlyout names it by its id)
        const std::wstring spotify = L"Spotify.exe";
        g_ff.filtering = true; g_ff.filterMode = 0;
        g_ff.blocked = { L" Spotify " };
        Check(SessionAllowed(spotify, &spotify), "an entry with spaces around it does not match (FluentFlyout does not trim)");
        g_ff.blocked = { L"SPOTIFY" };
        Check(!SessionAllowed(spotify, &spotify), "...matching ignores case");
        g_ff.filterMode = 1; g_ff.allowed = { L"\t" };
        Check(!SessionAllowed(spotify, &spotify) && !SessionAllowed(spotify, nullptr), "allow list of a blank entry only: nothing counts");

        // FluentFlyout's name for the app (not a part of its id): Firefox's id is a hash, FluentFlyout names
        // it from the shell; a Store player by its program's description
        const std::wstring firefoxId = L"308046B0AF4A39CB", firefox = L"Firefox";
        g_ff.allowed = { L"Firefox" };
        Check(SessionAllowed(firefoxId, &firefox), "allow list by FluentFlyout's name for the app (Firefox, not in its id): it counts");
        g_ff.allowed = { L"FIREFOX" };
        Check(SessionAllowed(firefoxId, &firefox), "...ignoring case");
        g_ff.allowed = { L"Spotify" };
        Check(!SessionAllowed(firefoxId, &firefox), "...another app's entry: it does not count");
        Check(SessionAllowed(firefoxId, nullptr), "the app not named yet: an entry may be its name, it counts (nothing hidden that may be shown)");
        const std::wstring melon = L"멜론", ecoute = L"écoute";
        g_ff.allowed = { L"멜론" };
        Check(SessionAllowed(L"Kakao.Melon_abc!App", &melon), "a Korean name in full");
        g_ff.allowed = { L"ÉCOUTE" };
        Check(SessionAllowed(L"Ecoute.App", &ecoute), "the case of letters beyond ASCII is ignored as FluentFlyout ignores it (OrdinalIgnoreCase)");
        g_ff.filterMode = 0; g_ff.blocked = { L"Firefox" };
        Check(!SessionAllowed(firefoxId, &firefox), "block list by FluentFlyout's name: left out");
        Check(SessionAllowed(firefoxId, nullptr), "...not named yet: it counts");

        // how FluentFlyout names apps (MediaPlayerData): the parts of the id it looks for
        auto variants = [](const wchar_t* id) { return NameVariants(id); };
        Check(variants(L"com.squirrel.Discord.Discord") == std::vector<std::wstring>({ L"squirrel", L"Discord", L"Discord", L"com.squirrel.Discord.Discord" }),
              "the id's parts without \"com\", the id itself");
        Check(variants(L"Spotify.exe") == std::vector<std::wstring>({ L"Spotify", L"Spotify.exe" }), "\"exe\" taken out");
        Check(variants(L"COMPANY.ccomom") == std::vector<std::wstring>({ L"PANY", L"com", L"COMPANY.ccomom" }),
              "taken out ignoring case, in one pass (String.Replace)");
        Check(variants(L"Microsoft.MicrosoftEdge.Stable_8wekyb3d8bbwe!App").back() == L"msedge" && variants(L"MSEdge").back() == L"msedge" &&
              variants(L"Contoso.App!msedge").back() == L"msedge" && variants(L"Chrome").back() == L"Chrome",
              "Edge's ids: \"msedge\" too");
        // its pick among the running programs
        const HWND someWindow = (HWND)(UINT_PTR)0x1234;
        const std::vector<std::wstring> player = { L"Player", L"Contoso.Player" };
        Check(PickProgram(player, { { L"host", L"C:\\Apps\\Contoso\\Player\\host.exe", someWindow }, { L"player", L"C:\\Apps\\player.exe", nullptr } }) == 1,
              "a program named like a part first (no window needed), before an earlier one whose path has a part");
        Check(PickProgram(player, { { L"host", L"C:\\APPS\\CONTOSO\\PLAYER\\host.exe", someWindow } }) == 0, "else one with a window whose path has a part, ignoring case");
        Check(PickProgram(player, { { L"host", L"C:\\Apps\\Contoso\\Player\\host.exe", nullptr } }) == -1, "...not one without a window");
        Check(PickProgram(player, { { L"player", L"", someWindow } }) == -1, "a program FluentFlyout cannot read is skipped");

        // the real programs: this test with a (cloaked) window of its own, and an id nothing knows
        HWND own = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"HuntFluent Player", WS_POPUP, 0, 0, 40, 20, nullptr, nullptr, nullptr, nullptr);
        BOOL cloak = TRUE;
        DwmSetWindowAttribute(own, DWMWA_CLOAK, &cloak, sizeof(cloak));
        ShowWindow(own, SW_SHOWNOACTIVATE);
        Check(AppName(L"fluent_lifecycle_test.exe") == L"HuntFluent Player", "a running program without a description: named by its main window's title");
        Check(AppName(L"LiquidDockHunt.NoSuchPlayer_0000!App") == L"LiquidDockHunt.NoSuchPlayer_0000!App", "nothing knows the id: the id");
        const std::wstring edge = AppName(L"MSEdge");
        Check(!edge.empty() && edge != L"MSEdge", "Edge's id: named by its program or the shell (shell:AppsFolder)");

        // the media thread names the apps with the list while the filter is on; the UI thread uses those names
        g_ff.filterMode = 1; g_ff.allowed = { L"HuntFluent Player" };
        g_ff.hideCompletely = true;
        ++g_ffVersion;
        WantMedia(true);   // as Tick does now: the slot follows the sessions
        g_namesWanted = true;
        PublishSessions({ L"fluent_lifecycle_test.exe" });
        {
            std::lock_guard<std::mutex> lk(g_mediaMu);
            Check(g_appNames[L"fluent_lifecycle_test.exe"] == L"HuntFluent Player", "a new list: its apps named first");
        }
        const double t2 = NowSeconds();
        UpdateHasMedia(t2);
        Check(g_rawMedia && g_media.value && WidgetState().open, "allowed by FluentFlyout's name for it: playing, the slot open (not cut while FluentFlyout shows it)");
        int published = g_mediaVersion;
        PublishSessions({ L"fluent_lifecycle_test.exe" });
        Check(g_mediaVersion == published, "the same list again, its apps named: nothing new");
        g_ff.allowed = { L"Spotify" };
        ++g_ffVersion;
        UpdateHasMedia(t2 + 0.1);
        UpdateHasMedia(t2 + 1.7);
        Check(!g_rawMedia && !g_media.value && !WidgetState().open, "not on the allow list: nothing playing, the slot closed");
        g_namesWanted = false;
        PublishSessions({ L"Other.Player" });
        {
            std::lock_guard<std::mutex> lk(g_mediaMu);
            Check(g_mediaVersion == published + 1 && g_sessions == std::vector<std::wstring>({ L"Other.Player" }) && !g_appNames.count(L"Other.Player"),
                  "filter off: the list without naming its apps");
        }
        // FluentFlyout gone, its filter on: the media thread (asleep) names no apps; found again with its
        // settings file unchanged (LoadSettings reads nothing): named again
        g_namesWanted = true;
        Forget(L"test: next case");
        Check(!g_namesWanted, "FluentFlyout gone: no apps named for its filter");
        g_nextScan = 0;
        Tick(NowSeconds());
        Check(g_widget == widget.hwnd && g_ff.filtering && g_namesWanted, "found again, its settings file unchanged: its filter's apps named again");
        g_namesWanted = false;
        DestroyWindow(own);
        g_ff.filtering = false; g_ff.blocked.clear(); g_ff.allowed.clear(); g_ff.hideCompletely = false;
        ++g_ffVersion;
        trayhost::g_stubHosted = {};
        g_media = {};
    }

    // ================================================================ the media thread while its list is not used
    // Its rounds (MediaRounds) with a fake manager, a check every 100 ms here (5 s in the dock): it looks
    // at the sessions only while "hide completely" makes the slot follow them. Before, it looked at every
    // check from the moment FluentFlyout was first found until the dock exited (720 an hour).
    {
        using namespace fluentflyout;
        g_nextScan = 0;
        Tick(NowSeconds());
        g_ff.loaded = true; g_ff.widgetEnabled = true; g_ff.hideCompletely = false; g_ff.filtering = false;
        g_namesWanted = false;
        ++g_ffVersion;
        trayhost::g_stubHosted.window = g_widget;
        trayhost::g_stubHosted.box = { 20, 15, 210, 55 };
        trayhost::g_stubHosted.fitted = true;
        Tick(NowSeconds());
        Check(g_widget && !g_mediaWanted, "FluentFlyout found, \"hide completely\" off: its session list is not used");
        Check(MediaEvents(), "(the media thread's events)");
        std::atomic<int> checks{ 0 }, lists{ 0 };
        std::atomic<bool> spotify{ false };   // what the fake manager lists
        auto list = [&] { ++lists; return spotify ? std::vector<std::wstring>{ L"Spotify.exe" } : std::vector<std::wstring>{}; };
        std::thread media([&] { MediaRounds([&] { ++checks; Refresh(list); }); });
        Sleep(550);
        std::printf("  (not used: %d wake-ups, %d lists in 0.55 s)\n", checks.load(), lists.load());
        Check(checks == 0 && lists == 0, "...the media thread sleeps: no wake-ups, nothing asked");

        g_ff.hideCompletely = true;
        ++g_ffVersion;
        const double on = NowSeconds();
        Tick(on);
        Check(g_mediaWanted && WaitFor([&] { return g_mediaKnown.load(); }, 1000) && NowSeconds() - on < 0.1,
              "\"hide completely\" on: the media thread looks at once");
        const int playingLogs = g_playingLogs;
        Tick(NowSeconds());
        Check(!g_media.value && !WidgetState().open, "...a first list: nothing plays, the slot closes at once");
        lists = 0; checks = 0;
        Sleep(550);
        std::printf("  (used: %d wake-ups, %d lists in 0.55 s)\n", checks.load(), lists.load());
        Check(lists >= 3 && lists + 1 >= checks, "while it is used: a look at every check");
        spotify = true;
        Check(TickUntil([&] { return g_media.value && WidgetState().open; }, 1000), "...a player found by the next check: the slot opens");

        g_ff.hideCompletely = false;
        ++g_ffVersion;
        Tick(NowSeconds());
        Check(!g_mediaWanted && !g_mediaKnown && g_media.value && WidgetState().open, "\"hide completely\" off again: the list dropped, the slot as FluentFlyout shows it");
        Sleep(50);   // the media thread hears of it
        lists = 0; checks = 0;
        Sleep(550);
        std::printf("  (not used again: %d wake-ups, %d lists in 0.55 s)\n", checks.load(), lists.load());
        Check(checks == 0 && lists == 0, "...the media thread sleeps again");
        Refresh(list);   // Windows reports a change meanwhile (its callback)
        PublishSessions({ L"Late.Player" });   // a list read just before the switch
        Check(lists == 0 && !g_mediaKnown, "...a change reported meanwhile: nothing asked, nothing kept");

        spotify = false;
        g_ff.hideCompletely = true;
        ++g_ffVersion;
        WantMedia(true);   // as Tick does; its list may come before the UI thread looks at the sessions again
        Check(WaitFor([&] { return g_mediaKnown.load(); }, 1000), "on again: the media thread looks at once");
        Tick(NowSeconds());
        Check(!g_media.value && !WidgetState().open, "...a first list again: nothing plays, closed at once (not held back as a replaced session)");
        Forget(L"test: next case");
        Check(!g_mediaWanted && !g_mediaKnown && !g_media.value,
              "FluentFlyout gone: its list is not used nor kept; what counted last (nothing plays) stays for the next first list");
        Sleep(50);
        lists = 0; checks = 0;
        Sleep(550);
        Check(checks == 0 && lists == 0, "...the media thread sleeps");
        Check(g_playingLogs == playingLogs + 1, "\"media playing\" written only for the player (not for the list being dropped)");

        const double stop = NowSeconds();
        SetEvent(g_mediaStop);
        media.join();
        ResetEvent(g_mediaStop);
        Check(NowSeconds() - stop < 0.2, "the dock exits: the sleeping media thread ends at once");

        // The list wanted again, its first look not in yet (the lists are handed over by hand below): the
        // slot stays as the dock showed it - before, the list was always followed and known by then
        int slotLogs = g_slotLogs, mediaLogs = g_mediaLogs;
        g_nextScan = 0;
        Tick(NowSeconds());   // FluentFlyout restarted: found again, its widget already in the dock's window
        Check(g_widget == widget.hwnd && g_mediaWanted && !g_listSeen && !WidgetState().open && g_slotLogs == slotLogs,
              "found again, nothing playing before, the list not in yet: the slot stays closed (not opened for a moment)");
        PublishSessions({});
        Tick(NowSeconds());
        Check(g_listSeen && !g_media.value && !WidgetState().open && g_slotLogs == slotLogs && g_mediaLogs == mediaLogs,
              "...its first list, nothing plays: still closed, no line written");
        PublishSessions({ L"Spotify.exe" });
        Tick(NowSeconds());
        Check(WidgetState().open && g_slotLogs == slotLogs + 1, "...a player: the slot opens");

        slotLogs = g_slotLogs; mediaLogs = g_mediaLogs;
        g_settings.edge = Edge::Left;   // a dock on a side edge takes no widget: the list is not used
        Tick(NowSeconds());
        Check(!g_mediaWanted && !WidgetState().open, "a side edge: no slot, the list not used");
        g_settings.edge = Edge::Bottom;
        Tick(NowSeconds());
        Check(g_mediaWanted && !WidgetState().open, "the bottom again, the list not in yet: no slot yet (none was shown)");
        PublishSessions({ L"Spotify.exe" });
        Tick(NowSeconds());
        Check(WidgetState().open && g_slotLogs == slotLogs && g_mediaLogs == mediaLogs,
              "...its first list, still playing: the slot opens, no line written (it was open before)");
        g_settings.edge = Edge::Left;
        Tick(NowSeconds());
        WidgetState();
        g_settings.edge = Edge::Bottom;
        Tick(NowSeconds());
        const bool openWaiting = WidgetState().open;
        PublishSessions({});   // stopped while the dock was on the side edge
        Tick(NowSeconds());
        Check(!openWaiting && !WidgetState().open && g_slotLogs == slotLogs + 1,
              "...again, the player stopped meanwhile: never opened for a moment, closed once");

        g_ff.hideCompletely = false;
        ++g_ffVersion;
        Tick(NowSeconds());
        Check(WidgetState().open, "\"hide completely\" off: the slot as FluentFlyout shows it");
        slotLogs = g_slotLogs; mediaLogs = g_mediaLogs;
        g_ff.hideCompletely = true;
        ++g_ffVersion;
        Tick(NowSeconds());
        const bool keptOpen = WidgetState().open;
        PublishSessions({});
        Tick(NowSeconds());
        Check(keptOpen && !WidgetState().open && g_slotLogs == slotLogs + 1 && g_mediaLogs == mediaLogs,
              "on again, nothing playing: open until the first list (not closed and opened), then closed once");

        PublishSessions({ L"Spotify.exe" });
        Tick(NowSeconds());
        Check(WidgetState().open, "a player: open");
        Forget(L"test: next case");   // found again before the dock looked at the slot
        g_nextScan = 0;
        Tick(NowSeconds());
        const double found = NowSeconds();
        Check(!WidgetState().open, "FluentFlyout found again at once, the list not in yet: no slot yet (the old one went with it)");
        mediaLogs = g_mediaLogs;
        Check(TickUntil([&] { return WidgetState().open; }, 2500) && NowSeconds() - found > 0.9 && g_mediaLogs == mediaLogs,
              "...no list at all (a media service that does not answer): after a second not known, counted as playing - open");
        g_ff.hideCompletely = false;
        ++g_ffVersion;
        trayhost::g_stubHosted = {};
        g_media = {};
        g_nextScan = 0;
        Tick(NowSeconds());   // found again for the flyouts below
    }

    // ================================================================ HostWidth
    {
        fluentflyout::g_ff.manualPadding = 0;
        const int w100 = fluentflyout::HostWidth(1.0f), w150 = fluentflyout::HostWidth(1.5f);
        fluentflyout::g_ff.manualPadding = -12;
        const int padded = fluentflyout::HostWidth(1.5f);
        fluentflyout::g_ff.manualPadding = 0;
        Check(w100 == 480 && w150 == 720 && padded == 732, "host window: 480 px at 100%, 720 at 150%, plus FluentFlyout's padding (its own px)");
    }

    // ================================================================ flyouts at the real DPI
    {
        using namespace fluentflyout;
        Unhook();   // events are fed by hand (deterministic)
        g_ff.animationSpeed = 1;   // 0.15 s
        HWND probe = MakeFlyout(mon.left + 16, mon.bottom - 200, 100, 50);
        const float s = GetDpiForWindow(probe) / 96.0f;
        OnWinEvent(nullptr, EVENT_OBJECT_HIDE, probe, OBJID_WINDOW, CHILDID_SELF, 0, 0);
        DestroyWindow(probe);
        auto S = [&](int px) { return (int)std::lround(px * s); };
        std::printf("  (flyouts at %.0f%%)\n", s * 100);
        DockView v;
        v.monitor = realMain; v.edge = Edge::Bottom; v.shown = true; v.scale = s;
        v.bar = { mon.left + S(150), mon.bottom - S(76), mon.right - S(150), mon.bottom - S(6) };
        v.slot = true;
        v.slotRect = { mon.left + S(170), v.bar.top, mon.left + S(370), v.bar.bottom };
        const int slotMid = (v.slotRect.left + v.slotRect.right) / 2, gap = S(12);
        const int w = S(310), h = S(116), ownLeft = mon.left + S(16), ownTop = mon.bottom - S(16) - h;
        HWND f = MakeFlyout(ownLeft, ownTop, w, h);   // FluentFlyout's bottom-left place: across the glass
        Check(IsFlyoutWindow(f), "a flyout of FluentFlyout's size at this DPI counts as a flyout");
        PlaceFor(v, 1.2);
        RECT p = Rect(f);
        Check(p.bottom == v.bar.top - gap && (p.left + p.right) / 2 == slotMid && FlyoutAt(realMain, { (p.left + p.right) / 2, (p.top + p.bottom) / 2 }),
              "placed clear of the glass above the widget, the gap scaled");
        Sleep(250);
        FluentMoves(f, p.left, p.top + S(20));   // its close animation: 20 px towards the edge while it fades
        PlaceFor(v, 0.1);
        Check(Rect(f).top == p.top + S(20) && !FlyoutAt(realMain, { (p.left + p.right) / 2, p.top + S(30) }), "its closing slide is left to it");
        // Opened again before it hid (a media key during the close animation): FluentFlyout runs its open
        // animation again (MainWindow.ShowMediaFlyout, OpenAnimation), which hides the window first and shows
        // it again once the animation has begun (WindowHelper.SetVisibility: SWP_HIDEWINDOW, SWP_SHOWWINDOW) -
        // back at its own place, from 20 px lower up to it. The hook hears the hide and the show.
        const int from = mon.bottom - h + S(4);
        ShowWindow(f, SW_HIDE);
        OnWinEvent(nullptr, EVENT_OBJECT_HIDE, f, OBJID_WINDOW, CHILDID_SELF, 0, 0);
        FluentMoves(f, ownLeft, from);   // hidden meanwhile: not a flyout the module follows
        ShowWindow(f, SW_SHOWNOACTIVATE);
        OnWinEvent(nullptr, EVENT_OBJECT_SHOW, f, OBJID_WINDOW, CHILDID_SELF, 0, 0);
        for (int i = 1; i <= 6; ++i) {
            PlaceFor(v, 0.025);
            FluentMoves(f, Rect(f).left, from + (ownTop - from) * i / 6);   // its animation sets Top only
        }
        PlaceFor(v, 1.2);
        RECT r = Rect(f);
        Check(r.bottom == v.bar.top - gap && (r.left + r.right) / 2 == slotMid,
              "opened again while closing: placed clear of the glass again (not left across the dock)");
        OnWinEvent(nullptr, EVENT_OBJECT_HIDE, f, OBJID_WINDOW, CHILDID_SELF, 0, 0);
        DestroyWindow(f);
        PlaceFlyouts(v, NowSeconds());
        Check(g_flyouts.empty() && !Busy() && g_lift.empty(), "hidden: forgotten, nothing keeps the frames at display rate");

        // a dock at the top: a flyout FluentFlyout puts at the top goes below the glass
        v.edge = Edge::Top;
        v.bar = { mon.left + S(150), mon.top + S(6), mon.right - S(150), mon.top + S(76) };
        v.slotRect = { mon.left + S(170), v.bar.top, mon.left + S(370), v.bar.bottom };
        HWND t = MakeFlyout(ownLeft, mon.top + S(16), w, h);
        PlaceFor(v, 1.0);
        r = Rect(t);
        Check(r.top == v.bar.bottom + gap && (r.left + r.right) / 2 == slotMid, "dock at the top: below the glass, under the widget");
        OnWinEvent(nullptr, EVENT_OBJECT_HIDE, t, OBJID_WINDOW, CHILDID_SELF, 0, 0);
        DestroyWindow(t);
        g_ff.animationSpeed = 2;
    }
    fluentflyout::Forget(L"test: next case");
    widget.Quit();
    mainTray.Quit();
    g_fakeExplorer = nullptr;

    // ================================================================ FluentFlyout restarted, updated
    {
        using namespace fluentflyout;
        // the module's handle for the process has SYNCHRONIZE only (GetProcessId needs more): compared as objects
        using Compare = BOOL(WINAPI*)(HANDLE, HANDLE);
        static const Compare compare = (Compare)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles");
        auto watches = [&](const Child& ch) { return g_proc && (!compare || compare(g_proc, ch.pi.hProcess)); };
        Child a, b, c;
        Check(a.Start(L"a", 1000), "a fake FluentFlyout (another process) with its widget window");
        g_nextScan = 0;
        Check(TickUntil([&] { return g_widget == a.widget; }, 3000) && g_pid == a.pi.dwProcessId && g_hooks[0] && watches(a),
              "found: its process watched, its windows hooked");
        a.CloseWidget();   // it quits: its window first, the process a second later
        Check(TickUntil([&] { return !g_widget && g_lostWidget; }, 2000) && Present(), "its widget window gone, the process still there: a new window is awaited");
        Check(TickUntil([&] { return !Present(); }, 3000) && a.WaitExit(0) && !g_proc && !g_hooks[0], "the process gone: forgotten, its handle closed, unhooked");
        Check(b.Start(L"b", 1500), "FluentFlyout started again (a new process)");
        const double startedAt = NowSeconds();
        Check(TickUntil([&] { return g_widget == b.widget; }, 3500) && g_pid == b.pi.dwProcessId && watches(b),
              "the new process is taken (within the 2 s scan)");
        std::printf("  (after %.1f s)\n", NowSeconds() - startedAt);
        // an update: the new version starts while the old one closes its widget window and lingers
        Check(c.Start(L"c", 0), "the updated FluentFlyout starts while the old one runs");
        b.CloseWidget();
        Check(TickUntil([&] { return g_widget == c.widget; }, 2000) && g_pid == c.pi.dwProcessId && watches(c),
              "the old one's widget window gone: the new one's window is taken at once, its process watched instead");
        Check(b.WaitExit(5000), "(the old process left)");
        TickUntil([] { return false; }, 300);
        Check(g_widget == c.widget && Present(), "the old process leaving changes nothing");
        c.CloseWidget();
        Check(c.WaitExit(5000) && TickUntil([&] { return !Present(); }, 4000), "the last one quits: no longer present");
        a.Release(); b.Release(); c.Release();

        // handles: what the first rounds set up once (process creation, hooks) aside, rounds of FluentFlyout
        // found, its window made again and the process gone leave nothing behind
        auto round = [&](const wchar_t* id) {
            Child r;
            if (!r.Start(id, 300)) return false;
            g_nextScan = 0;
            bool ok = TickUntil([&] { return g_widget == r.widget; }, 3000);
            r.CloseWidget();
            ok = ok && TickUntil([&] { return !Present(); }, 4000) && r.WaitExit(5000);
            r.Release();
            return ok;
        };
        bool rounds = round(L"warm-1") && round(L"warm-2");
        DWORD handlesBefore = 0, handlesAfter = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
        for (int i = 0; i < 4; ++i) rounds = round((L"round-" + std::to_wstring(i)).c_str()) && rounds;
        GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
        std::printf("  (handles %lu before four more rounds, %lu after)\n", handlesBefore, handlesAfter);
        Check(rounds && handlesAfter <= handlesBefore + 1, "no handles left behind by FluentFlyout processes coming and going");
    }

    fluentflyout::Shutdown();
    DeleteFileW(fileA.c_str()); DeleteFileW(fileB.c_str());
    std::printf("fluent_lifecycle_test: %s (%d checks, %d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}
