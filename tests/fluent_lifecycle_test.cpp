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
#include "../src/elevation.h"   // before the stub's name below: its ProcessElevated stays the real one, unused
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
bool g_stubElevated = false;   // the dock as administrator (the log's line for the widget not moved in)
bool ProcessElevated() { return g_stubElevated; }
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

// A made-up monitor fluent_flyout.cpp can describe in its log (the real ones pass through): left of the
// main one at -1440,-560, as on a two-monitor desktop. Only fluent_flyout.cpp sees it (placement reads the real ones).
static const HMONITOR kHuntOther = (HMONITOR)(UINT_PTR)0x48F3;
static BOOL HuntGetMonitorInfoW(HMONITOR m, LPMONITORINFO mi) {
    if (m != kHuntOther) return GetMonitorInfoW(m, mi);
    mi->rcMonitor = mi->rcWork = { -1440, -560, 0, 2000 };
    mi->dwFlags = 0;
    return TRUE;
}

// fluent_flyout.cpp's and fluent_media.cpp's waits with a timeout (Shutdown's for the media thread, in StopMedia)
// are noted, then made as asked
static DWORD g_waitedMs = 0;
static DWORD HuntWaitForSingleObject(HANDLE h, DWORD ms) {
    if (ms) g_waitedMs = ms;
    return WaitForSingleObject(h, ms);
}

#define ExplorerTaskbar HuntExplorerTaskbar
#define LD_FLUENT_SETTINGS_FILE HuntSettingsFiles
#define LD_FLUENT_NO_MEDIA
#define LD_FLUENT_MEDIA_CHECK_MS 100   // the media thread's rounds (MediaRounds, run below with a fake list): 5 s in the dock
#define LD_FLUENT_EXE L"fluent_lifecycle_test.exe"
#define FindWindowExW HuntFindWindowExW
#define GetClassNameW HuntGetClassNameW
#define GetWindowTextW HuntGetWindowTextW
#define GetMonitorInfoW HuntGetMonitorInfoW
#define WaitForSingleObject HuntWaitForSingleObject
#define ProcessElevated trayhost::ProcessElevated   // the stub above (the dock as administrator or not)
#include "../src/fluent_flyout.cpp"
#undef ProcessElevated
#undef GetMonitorInfoW
#include "../src/fluent_settings_file.cpp"
#include "../src/fluent_media.cpp"   // its StopMedia: Shutdown's wait for the media thread
#undef WaitForSingleObject
#include "../src/fluent_placement.cpp"
#undef FindWindowExW
#undef GetClassNameW
#undef GetWindowTextW
#undef ExplorerTaskbar

Settings g_settings;

static std::atomic<int> g_playingLogs{ 0 };   // "media playing" lines written
static std::atomic<int> g_mediaLogs{ 0 };     // "media playing" and "nothing playing" lines
static std::atomic<int> g_slotLogs{ 0 };      // "widget slot open / closed" lines
static std::mutex g_logMu;
static std::vector<std::wstring> g_logLines;   // every line, in order (the media thread's too)

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    {
        std::lock_guard<std::mutex> lk(g_logMu);
        g_logLines.push_back(buf);
    }
    if (!wcscmp(buf, L"fluentflyout: media playing")) ++g_playingLogs;
    if (!wcscmp(buf, L"fluentflyout: media playing") || !wcscmp(buf, L"fluentflyout: nothing playing")) ++g_mediaLogs;
    if (!wcsncmp(buf, L"fluentflyout: widget slot ", 26)) ++g_slotLogs;
    char utf8[4096];   // printf only: wide and narrow output on one stream do not mix
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    std::printf("  log: %s\n", utf8);
}
static double g_frozenNow = -1;   // >= 0: the module's clock held there (a boundary looked at exactly)
double NowSeconds() {
    if (g_frozenNow >= 0) return g_frozenNow;
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}
#define TEST_SUPPORT_FROM_UTF8
#include "test_support.h"

// ---------------------------------------------------------------- helpers
#include "portable_check.h"

// The log's lines, word for word (characterization: what the module writes now). A mark, then the
// module's lines ("fluentflyout: ...") written since, in order.
static size_t LogMark() {
    std::lock_guard<std::mutex> lk(g_logMu);
    return g_logLines.size();
}
static std::vector<std::wstring> FluentLines(size_t from) {
    std::lock_guard<std::mutex> lk(g_logMu);
    std::vector<std::wstring> out;
    for (size_t i = from; i < g_logLines.size(); ++i)
        if (!g_logLines[i].compare(0, 14, L"fluentflyout: ")) out.push_back(g_logLines[i]);
    return out;
}
static void PrintLines(const char* what, const std::vector<std::wstring>& lines) {
    std::printf("  %s (%zu):\n", what, lines.size());
    for (auto& l : lines) {
        char utf8[4096];
        WideCharToMultiByte(CP_UTF8, 0, l.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
        std::printf("    %s\n", utf8);
    }
}
static bool SameLines(size_t from, const std::vector<std::wstring>& want) {
    const auto got = FluentLines(from);
    if (got == want) return true;
    PrintLines("lines expected", want);
    PrintLines("lines written", got);
    return false;
}
static std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}
static std::wstring SettingsLine(const std::wstring& path, int widget, int hide, int monitor, int visualizer, int filter, const wchar_t* mode, int animation) {
    return Fmt(L"fluentflyout: settings %s - widget %d, hide when nothing plays %d, monitor %d, visualizer %d, app filter %d (%s), animation %d",
               path.c_str(), widget, hide, monitor, visualizer, filter, mode, animation);
}
// ParentKind by name: Explorer's main and secondary taskbars by one name (ParentKind tells them apart; the
// checks that tell them apart compare the kind itself)
static std::string KindName(fluentflyout::ParentKind k) {
    switch (k) {
    case fluentflyout::ParentKind::None: return "None";
    case fluentflyout::ParentKind::Host: return "Host";
    case fluentflyout::ParentKind::ExplorerMain: return "Explorer";
    case fluentflyout::ParentKind::ExplorerSecondary: return "Explorer";
    case fluentflyout::ParentKind::Other: return "Other";
    }
    return "?";
}

#include "fluent_harness.h"

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

// ---------------------------------------------------------------- a fake media thread for Shutdown
// Its rounds (MediaRounds) as the media thread runs them, its look a fake one (no media sessions are
// asked): one that hangs (a media service that does not answer) notes what Shutdown had done by the
// time the stop came, and returns only when released.
struct FakeMedia {
    bool hang = false;
    HANDLE inLook = nullptr, release = nullptr;
    size_t mark = 0;                              // the log's lines from here on
    std::atomic<int> looks{ 0 };
    std::atomic<bool> sawStop{ false }, forgottenFirst{ false };
};
static FakeMedia g_fakeMedia;

static DWORD WINAPI FakeMediaThread(void*) {
    fluentflyout::MediaRounds([] {
        ++g_fakeMedia.looks;
        SetEvent(g_fakeMedia.inLook);
        if (!g_fakeMedia.hang) return;
        g_fakeMedia.sawStop = WaitForSingleObject(fluentflyout::g_mediaStop, 5000) == WAIT_OBJECT_0;
        const auto lines = FluentLines(g_fakeMedia.mark);
        g_fakeMedia.forgottenFirst = std::find(lines.begin(), lines.end(), L"fluentflyout: shutting down") != lines.end() && !fluentflyout::g_mediaWanted;
        WaitForSingleObject(g_fakeMedia.release, 5000);
    });
    return 0;
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
        const size_t settingsMark = LogMark();
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
        Check(g_namesWanted, "...its app filter on: the apps are named for it (g_namesWanted, set with each file read)");
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
        LoadSettings(t + 52, false);
        Check(g_ff.loaded && g_ff.filtering && g_fileSetting == 1 && g_ffPath == fileB && g_namesWanted, "gone for 8 s: its values still kept (FluentFlyout's defaults only after 10 s)");
        const int versionGone = g_ffVersion;
        g_choiceDirty = false;   // as UpdateMonitor leaves it: only the file gone sets it again
        LoadSettings(t + 54.5, false);
        Check(!g_ff.loaded && g_fileSetting == -1 && g_ffPath.empty() && g_choiceDirty, "gone for more than 10 s: FluentFlyout's defaults, its monitor no longer known");
        Check(!g_namesWanted && g_fileAt == t + 54.5 && g_ffVersion == versionGone + 1,
              "...and no apps named for its filter, the setting noted as new now (\"not known\"), the settings' version one on");
        WriteSettings(fileA, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n" + Monitor(1), 1);
        LoadSettings(t + 56.5, false);
        Check(g_ff.loaded && g_fileSetting == 1 && g_fileAt == t + 56.5, "back: read at once, its monitor noted as new");
        // the log: one line for each file read (its path and values), one for the file gone; none for what is not taken
        Check(SameLines(settingsMark, {
                  SettingsLine(fileA, 1, 0, 1, 0, 0, L"block", 2),
                  SettingsLine(fileB, 1, 0, 0, 0, 0, L"block", 2),
                  SettingsLine(fileA, 1, 0, 1, 0, 0, L"block", 2),
                  SettingsLine(fileB, 1, 0, 1, 0, 1, L"block", 2),
                  L"fluentflyout: settings file gone - its defaults assumed",
                  SettingsLine(fileA, 1, 0, 1, 0, 0, L"block", 2) }),
              "the log: \"settings <path> - widget ..., animation N\" for each file read, \"settings file gone - its defaults assumed\" after 10 s, nothing for a file not taken");
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
        Check(fluentflyout::g_widget == widget.hwnd && fluentflyout::g_parentKind == fluentflyout::ParentKind::ExplorerMain,
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
        Check(fluentflyout::g_parentKind == fluentflyout::ParentKind::ExplorerSecondary && fluentflyout::g_shownMonitor == realMain,
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
        const size_t namedMark = LogMark();
        PublishSessions({ L"fluent_lifecycle_test.exe" });
        {
            std::lock_guard<std::mutex> lk(g_mediaMu);
            Check(g_appNames[L"fluent_lifecycle_test.exe"] == L"HuntFluent Player", "a new list: its apps named first");
        }
        Check(SameLines(namedMark, { L"fluentflyout: media app fluent_lifecycle_test.exe - FluentFlyout's name for it \"HuntFluent Player\" (its app filter)" }),
              "the log: \"media app <id> - FluentFlyout's name for it \"<name>\" (its app filter)\" for a new name");
        const double t2 = NowSeconds();
        UpdateHasMedia(t2);
        Check(g_rawMedia && g_media.value && WidgetState().open, "allowed by FluentFlyout's name for it: playing, the slot open (not cut while FluentFlyout shows it)");
        int published = g_mediaVersion;
        const size_t sameListMark = LogMark();
        PublishSessions({ L"fluent_lifecycle_test.exe" });
        Check(g_mediaVersion == published, "the same list again, its apps named: nothing new");
        Check(SameLines(sameListMark, {}), "...and no line (a name once found is not written again)");
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
        // an app FluentFlyout names by its id (nothing knows it): looked up again when the list changes -
        // the same name again, no line
        g_namesWanted = true;
        const std::wstring noSuch = L"LiquidDockHunt.NoSuchPlayer_0000!App";
        size_t idMark = LogMark();
        PublishSessions({ noSuch });
        Check(SameLines(idMark, { L"fluentflyout: media app " + noSuch + L" - FluentFlyout's name for it \"" + noSuch + L"\" (its app filter)" }),
              "the log: \"media app <id> - FluentFlyout's name for it \"<id>\"\" for an app named by its id");
        idMark = LogMark();
        PublishSessions({ noSuch, L"fluent_lifecycle_test.exe" });
        {
            std::lock_guard<std::mutex> lk(g_mediaMu);
            Check(g_appNames[noSuch] == noSuch && g_appNames[L"fluent_lifecycle_test.exe"] == L"HuntFluent Player" &&
                      g_sessions == std::vector<std::wstring>({ noSuch, L"fluent_lifecycle_test.exe" }),
                  "...the list changed: the one named by its id looked up again (the same), the other's name kept");
        }
        Check(SameLines(idMark, {}), "...and no line (its name did not change)");
        const int versionSame = g_mediaVersion;
        PublishSessions({ noSuch, L"fluent_lifecycle_test.exe" });
        Check(g_mediaVersion == versionSame, "...the same list again: nothing looked up again, nothing new (an app named by its id is tried again only when the list changes)");
        g_namesWanted = false;
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
        const size_t updateMark = LogMark();
        b.CloseWidget();
        Check(TickUntil([&] { return g_widget == c.widget; }, 2000) && g_pid == c.pi.dwProcessId && watches(c),
              "the old one's widget window gone: the new one's window is taken at once, its process watched instead");
        Check(SameLines(updateMark, { L"fluentflyout: its widget window closed - keeping its place for the new one",
                                      Fmt(L"fluentflyout: running (process %lu)", c.pi.dwProcessId),
                                      L"fluentflyout: widget window is in a window of its own (not moved into a taskbar)" }),
              "the log: the old window closed, then the new process \"running (process <pid>)\" - no \"new widget window is here\" for another process");
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

    // ================================================================ as it is now (a characterization)
    // Characterization: the module's log lines word for word on each path, what each path forgets and
    // keeps, the kind of each parent, Shutdown with a media thread. Made-up monitors only (kHuntOther,
    // which the GetMonitorInfoW fake describes, and the real main one, at 0,0 on every PC); the 6 s and
    // 3 s waits by moving back the times they count from, not by a clock ahead.
    {
        using namespace fluentflyout;
        const std::wstring running = Fmt(L"fluentflyout: running (process %lu)", GetCurrentProcessId());
        MONITORINFO mainInfo = { sizeof(mainInfo) };
        GetMonitorInfoW(realMain, &mainInfo);
        const std::wstring mainText = Fmt(L"main monitor at %ld,%ld (FluentFlyout's \"1\")", mainInfo.rcMonitor.left, mainInfo.rcMonitor.top);
        const std::wstring otherText = L"other monitor at -1440,-560 (FluentFlyout's \"2\")";
        const std::wstring own = L"a window of its own (not moved into a taskbar)", dock = L"the dock's window",
                           mainBar = L"Explorer's taskbar (hidden by the dock)", secondBar = L"a secondary Explorer taskbar (hidden by the dock)";
        auto in = [](const std::wstring& where) { return L"fluentflyout: widget window is in " + where; };
        auto noBars = [](int secondaries, int monitors) {
            return Fmt(L"fluentflyout: Explorer has no taskbar on every display (%d secondary, %d monitors): FluentFlyout puts its widget into the main one when its monitor has none - its settings decide",
                       secondaries, monitors);
        };
        const std::wstring tookMain = L"fluentflyout: FluentFlyout took that taskbar: its widget belongs on the " + mainText;
        auto goesTo = [](const std::wstring& monitor, const wchar_t* why, int setting) {
            return L"fluentflyout: its widget goes to the dock on the " + monitor + L" - " + why + (setting > 0 ? Fmt(L", its setting \"%d\"", setting) : std::wstring());
        };
        const std::wstring notMoved = L"fluentflyout: FluentFlyout has not moved its widget into the dock's window (it is in ";
        auto layout = [&](std::vector<HMONITOR> handles, std::vector<fluentlogic::Monitor> descs) {
            g_monitorHandles = std::move(handles);
            g_monitorDescs = std::move(descs);
            g_monitorsAt = NowSeconds() + 1e6;   // kept: RefreshMonitors would read the real ones
            g_choiceDirty = true;
        };
        auto twoMonitors = [&] { layout({ kHuntOther, realMain }, { { false, -1440 }, { true, mon.left } }); };
        auto makeOwn = [](HWND w) {   // a window of its own again
            SetWindowLongPtrW(w, GWL_STYLE, (GetWindowLongPtrW(w, GWL_STYLE) & ~WS_CHILD) | WS_POPUP);
            SetParent(w, nullptr);
        };
        WindowThread tray, host, stale, other, second;
        tray.Run(kHuntMainTray, L"");     // Explorer's main taskbar
        host.Run(kHuntMainTray, L"");     // the dock's host window: a "Shell_TrayWnd" with the dock's mark
        stale.Run(kHuntMainTray, L"");    // one with the mark that is not the host window now (an earlier one)
        other.Run(L"HuntFluent_OtherParent", L"");
        second.Run(kHuntSecondary, L"");
        SetPropW(host.hwnd, kTrayHostProp, (HANDLE)1);
        SetPropW(stale.hwnd, kTrayHostProp, (HANDLE)1);
        g_fakeExplorer = tray.hwnd;
        trayhost::g_stubHosted = {};

        // ---- the kind of each parent (Classify) and its name in the log (ParentName)
        trayhost::g_stubWindow = host.hwnd;
        Check(KindName(Classify(nullptr)) == "None" && KindName(Classify(GetDesktopWindow())) == "None" && ParentName(nullptr) == own && ParentName(GetDesktopWindow()) == own,
              "parent: none or the desktop - None, \"a window of its own (not moved into a taskbar)\"");
        Check(KindName(Classify(host.hwnd)) == "Host" && ParentName(host.hwnd) == dock, "parent: the dock's host window (a Shell_TrayWnd with the dock's mark) - Host, \"the dock's window\"");
        Check(KindName(Classify(tray.hwnd)) == "Explorer" && ParentName(tray.hwnd) == mainBar, "parent: Explorer's main taskbar - Explorer, \"Explorer's taskbar (hidden by the dock)\"");
        Check(KindName(Classify(second.hwnd)) == "Explorer" && ParentName(second.hwnd) == secondBar,
              "parent: a secondary taskbar - Explorer, \"a secondary Explorer taskbar (hidden by the dock)\"");
        Check(KindName(Classify(stale.hwnd)) == "Other" && ParentName(stale.hwnd) == mainBar,
              "parent: a Shell_TrayWnd with the dock's mark that is not the host window now - Other, yet named \"Explorer's taskbar (hidden by the dock)\"");
        Check(KindName(Classify(other.hwnd)) == "Other" && ParentName(other.hwnd) == L"a window of class HuntFluent_OtherParent",
              "parent: any other window - Other, \"a window of class <its class>\"");
        trayhost::g_stubWindow = nullptr;
        Check(KindName(Classify(host.hwnd)) == "Other" && ParentName(host.hwnd) == mainBar, "...the dock's window while the host is not up: as that one");
        second.Quit();

        // ---- found with no settings file: the dock's monitor "not known yet", no setting in the line
        Forget(L"test: next case");
        DeleteFileW(fileA.c_str()); DeleteFileW(fileB.c_str());
        g_nextSettingsCheck = 0; g_settingsFilesAt = -1; g_ffMissingSince = -1;
        g_ff = {}; g_ffPath.clear(); g_ffTime = {}; g_fileSetting = -1; g_fileAt = -1;
        twoMonitors();
        {
            WindowThread w0;
            w0.Run(std::wstring(kHuntWrapper) + L"lines-0]", kHuntTitle);
            const size_t mark0 = LogMark();
            g_nextScan = 0;
            Tick(NowSeconds());
            Check(g_widget == w0.hwnd && !g_ff.loaded && g_fileSetting == -1 && g_monitor == realMain &&
                      SameLines(mark0, { running, in(own), goesTo(mainText, L"not known yet", 0) }),
                  "the log, found with no settings file: \"running\", where, \"its widget goes to the dock on the main monitor at <x>,<y> (FluentFlyout's \"1\") - not known yet\" (no setting)");
            w0.Quit();
        }
        // ---- found with its setting "1" (0 in the file): "from its settings, its setting \"1\""
        Forget(L"test: next case");
        WriteSettings(fileA, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n" + Monitor(0), 1);
        g_nextSettingsCheck = 0; g_settingsFilesAt = -1; g_ffMissingSince = -1; g_ffPath.clear(); g_ffTime = {};
        twoMonitors();
        {
            WindowThread wz;
            wz.Run(std::wstring(kHuntWrapper) + L"lines-z]", kHuntTitle);
            const size_t markZ = LogMark();
            g_nextScan = 0;
            Tick(NowSeconds());
            Check(g_widget == wz.hwnd && g_fileSetting == 0 && g_fileMonitor == 1 && g_monitor == realMain &&
                      SameLines(markZ, { running, SettingsLine(fileA, 1, 0, 0, 0, 0, L"block", 2), in(own), goesTo(mainText, L"from its settings", 1) }),
                  "the log, found with its setting \"1\": \"its widget goes to the dock on the main monitor ... - from its settings, its setting \"1\"\"");
            wz.Quit();
        }

        // ---- found, then moved between Explorer's taskbars: the lines of each step
        Forget(L"test: next case");
        WriteSettings(fileA, L"  <TaskbarWidgetEnabled>true</TaskbarWidgetEnabled>\r\n  <TaskbarVisualizerEnabled>true</TaskbarVisualizerEnabled>\r\n"
                             L"  <AppFilteringMode>1</AppFilteringMode>\r\n  <FlyoutAnimationSpeed>3</FlyoutAnimationSpeed>\r\n" + Monitor(1), 1);
        g_nextSettingsCheck = 0; g_settingsFilesAt = -1; g_ffMissingSince = -1; g_ffPath.clear(); g_ffTime = {};
        twoMonitors();
        WindowThread w1;
        w1.Run(std::wstring(kHuntWrapper) + L"lines-1]", kHuntTitle);
        size_t mark = LogMark();
        g_nextScan = 0;
        Tick(NowSeconds());
        Check(g_widget == w1.hwnd && SameLines(mark, { running, SettingsLine(fileA, 1, 0, 1, 1, 0, L"allow", 3), in(own), goesTo(otherText, L"from its settings", 2) }),
              "the log, found: \"running (process <pid>)\", its settings file, \"widget window is in a window of its own ...\", \"its widget goes to the dock on the other monitor at -1440,-560 (FluentFlyout's \"2\") - from its settings, its setting \"2\"\"");
        // the same setting, the monitors listed in another order: its position worked out again - the same
        // monitor, nothing written
        layout({ realMain, kHuntOther }, { { true, mon.left }, { false, -1440 } });
        mark = LogMark();
        Tick(NowSeconds());
        Check(g_fileMonitor == 1 && g_monitor == kHuntOther && WidgetMonitor() == kHuntOther && SameLines(mark, {}),
              "the monitors listed in another order, the same setting \"2\": worked out again, the same monitor - nothing written");
        // a monitor the system does not describe (GetMonitorInfo fails) in its place: "(no monitor)"
        const HMONITOR undescribed = (HMONITOR)(UINT_PTR)0x48F7;
        layout({ undescribed, realMain }, { { false, -1440 }, { true, mon.left } });
        mark = LogMark();
        Tick(NowSeconds());
        Check(g_monitor == undescribed && SameLines(mark, { goesTo(L"(no monitor)", L"from its settings", 2) }),
              "the log, the dock's monitor not described by the system: \"its widget goes to the dock on the (no monitor) - from its settings, its setting \"2\"\"");
        twoMonitors();
        mark = LogMark();
        Tick(NowSeconds());
        Check(g_monitor == kHuntOther && SameLines(mark, { goesTo(otherText, L"from its settings", 2) }), "...the layout back: the other monitor again");
        MakeChild(w1.hwnd, tray.hwnd);   // Explorer's main taskbar, no secondary one
        mark = LogMark();
        Tick(NowSeconds());
        Check(KindName(g_parentKind) == "Explorer" && !g_parentTells && SameLines(mark, { in(mainBar), noBars(0, 2) }),
              "the log, into Explorer's main taskbar, no secondary one: where, then \"Explorer has no taskbar on every display (0 secondary, 2 monitors): ...\"");
        Check(!trayhost::Window() && g_notInHostSince == -1 && !g_notInHostLogged, "...the dock's window not up: no wait for the widget to move into it");
        g_choiceDirty = true;   // the displays looked at again: the same
        mark = LogMark();
        Tick(NowSeconds());
        Check(SameLines(mark, {}), "...looked at again, the same: nothing written");
        second.Run(kHuntSecondary, L"");   // Explorer's taskbar on the other display too
        g_choiceDirty = true;
        mark = LogMark();
        Tick(NowSeconds());
        Check(g_parentTells && g_shownMonitor == realMain && SameLines(mark, { tookMain, goesTo(mainText, L"where FluentFlyout put it", 2) }),
              "the log, a taskbar on every display: \"FluentFlyout took that taskbar: its widget belongs on the main monitor ...\", the dock \"- where FluentFlyout put it, its setting \"2\"\"");
        {
            const double s1 = NowSeconds(), s2 = s1 + 0.25;
            mark = LogMark();
            Tick(s1);
            const double shownAt1 = g_shownAt;
            Tick(s2);
            Check(shownAt1 == s1 && g_shownAt == s2 && g_shownMonitor == realMain && SameLines(mark, {}),
                  "...while it stays in that taskbar: when it was seen there noted at every Tick (the same monitor: nothing written)");
        }
        second.Quit();
        g_choiceDirty = true;
        mark = LogMark();
        Tick(NowSeconds());
        Check(!g_parentTells && g_shownMonitor == realMain && SameLines(mark, { noBars(0, 2) }),
              "...the secondary taskbar gone: \"no taskbar on every display\" again (it told before); the dock stays where FluentFlyout put it");
        // three displays, one secondary taskbar: in it a sure answer, in the main one not
        second.Run(kHuntSecondary, L"");
        layout({ kHuntOther, realMain, (HMONITOR)(UINT_PTR)0x48F5 }, { { false, -1440 }, { true, mon.left }, { false, mon.left + 2560 } });
        SetParent(w1.hwnd, second.hwnd);
        mark = LogMark();
        Tick(NowSeconds());
        Check(KindName(g_parentKind) == "Explorer" && g_parentTells && SameLines(mark, { in(secondBar) }),
              "the log, into a secondary taskbar (1 of 3 displays): where only - it tells, the same monitor as before (no \"took that taskbar\" again)");
        SetParent(w1.hwnd, tray.hwnd);
        mark = LogMark();
        Tick(NowSeconds());
        Check(!g_parentTells && SameLines(mark, { in(mainBar), noBars(1, 3) }), "...back into the main one: \"(1 secondary, 3 monitors)\"");
        // secondary taskbars are counted up to 16
        std::vector<HWND> more;
        for (int i = 0; i < 17; ++i)
            more.push_back(CreateWindowExW(WS_EX_TOOLWINDOW, kHuntSecondary, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
        std::vector<HMONITOR> many = { kHuntOther, realMain };
        std::vector<fluentlogic::Monitor> manyDescs = { { false, -1440 }, { true, mon.left } };
        for (int i = 2; i < 18; ++i) { many.push_back((HMONITOR)(UINT_PTR)(0x4900 + i)); manyDescs.push_back({ false, mon.left + 2560 * (i - 1) }); }
        layout(many, manyDescs);
        SetParent(w1.hwnd, other.hwnd);
        mark = LogMark();
        Tick(NowSeconds());
        Check(KindName(g_parentKind) == "Other" && SameLines(mark, { in(L"a window of class HuntFluent_OtherParent") }), "the log, into another window: \"widget window is in a window of class <its class>\"");
        SetParent(w1.hwnd, tray.hwnd);
        mark = LogMark();
        Tick(NowSeconds());
        Check(SameLines(mark, { in(mainBar), noBars(16, 18) }), "...back into the main one, 18 secondary taskbars on 18 displays: counted up to 16 (\"16 secondary, 18 monitors\")");
        for (HWND h : more) DestroyWindow(h);
        twoMonitors();
        SetParent(w1.hwnd, stale.hwnd);
        mark = LogMark();
        Tick(NowSeconds());
        Check(KindName(g_parentKind) == "Other" && !g_parentTells && SameLines(mark, { in(mainBar) }),
              "the log, into a marked Shell_TrayWnd that is not the host window: \"Explorer's taskbar\" by its name, nothing about its monitor (Other)");

        // ---- the dock's window up, the widget not in it: one line after 6 s
        trayhost::g_stubWindow = host.hwnd;
        SetParent(w1.hwnd, tray.hwnd);
        mark = LogMark();
        Tick(NowSeconds());
        Check(g_notInHostSince >= 0 && !g_notInHostLogged && SameLines(mark, { in(mainBar) }),
              "...the dock's window up, its widget in Explorer's main taskbar (a taskbar on every display, the same monitor): where only; the wait begins");
        g_notInHostSince = NowSeconds() - 5.5;
        mark = LogMark();
        Tick(NowSeconds());
        Check(!g_notInHostLogged && SameLines(mark, {}), "...nothing after 5.5 s");
        g_notInHostSince = NowSeconds() - 6.5;
        mark = LogMark();
        Tick(NowSeconds());
        Check(g_notInHostLogged && SameLines(mark, { notMoved + mainBar + L") - is FluentFlyout's taskbar widget on, with Premium?" }),
              "the log, after 6 s: \"FluentFlyout has not moved its widget into the dock's window (it is in <where>) - is FluentFlyout's taskbar widget on, with Premium?\"");
        mark = LogMark();
        Tick(NowSeconds());
        Check(SameLines(mark, {}), "...written once");
        makeOwn(w1.hwnd);
        mark = LogMark();
        Tick(NowSeconds());
        Check(g_notInHostLogged && SameLines(mark, { in(own) }), "...moved elsewhere outside the dock's window: not written again");
        MakeChild(w1.hwnd, host.hwnd);
        mark = LogMark();
        Tick(NowSeconds());
        Check(KindName(g_parentKind) == "Host" && g_notInHostSince == -1 && !g_notInHostLogged && SameLines(mark, { in(dock) }),
              "the log, into the dock's window: \"widget window is in the dock's window\"; the wait and its line reset");
        makeOwn(w1.hwnd);
        Tick(NowSeconds());
        trayhost::g_stubElevated = true;
        g_notInHostSince = NowSeconds() - 6.5;
        mark = LogMark();
        Tick(NowSeconds());
        trayhost::g_stubElevated = false;
        Check(SameLines(mark, { notMoved + own +
                                L") - the dock runs as administrator, and Windows does not let an ordinary program put a window into an elevated one: start Liquid Dock normally" }),
              "the log, out of it for 6 s again, the dock as administrator: \"... - the dock runs as administrator, and Windows does not let an ordinary program put a window into an elevated one: start Liquid Dock normally\"");
        // the dock's window gone meanwhile (the host stopped): the wait and its line reset; up again, it begins
        // again and the line comes once more after 6 s
        trayhost::g_stubWindow = nullptr;
        mark = LogMark();
        Tick(NowSeconds());
        Check(g_notInHostLogged == false && g_notInHostSince == -1 && SameLines(mark, {}), "...the dock's window gone: the wait and its line reset, nothing written");
        trayhost::g_stubWindow = host.hwnd;
        Tick(NowSeconds());
        const bool waitsAgain = g_notInHostSince >= 0 && !g_notInHostLogged;
        g_notInHostSince = NowSeconds() - 6.5;
        mark = LogMark();
        Tick(NowSeconds());
        Check(waitsAgain && g_notInHostLogged && SameLines(mark, { notMoved + own + L") - is FluentFlyout's taskbar widget on, with Premium?" }),
              "...up again: the wait begins again, the line once more after 6 s");

        // ---- the slot's four lines
        MakeChild(w1.hwnd, host.hwnd);
        Tick(NowSeconds());
        trayhost::g_stubHosted.window = w1.hwnd;
        trayhost::g_stubHosted.box = { 20, 15, 210, 55 };
        trayhost::g_stubHosted.fitted = true;
        mark = LogMark();
        Check(WidgetState().open && SameLines(mark, { L"fluentflyout: widget slot open (showing)" }), "the log: \"widget slot open (showing)\"");
        trayhost::g_stubHosted.window = nullptr;
        g_adoptedAt = NowSeconds() - 1.0;
        mark = LogMark();
        Check(WidgetState().open && SameLines(mark, {}), "...the widget not in the host window within 1.6 s of being found: kept open, nothing written");
        g_frozenNow = NowSeconds();   // the module's clock held: the 1.6 s boundary looked at exactly
        g_adoptedAt = g_frozenNow - 1.55;
        mark = LogMark();
        Check(WidgetState().open && SameLines(mark, {}), "...1.55 s after being found: still kept open");
        g_settings.edge = Edge::Left;   // a side edge for a moment: no slot answered (g_slotShown), its last state (g_decidedOpen) kept
        WidgetState();
        g_settings.edge = Edge::Bottom;
        const bool sideMeanwhile = !g_slotShown && g_decidedOpen;
        g_adoptedAt = g_frozenNow - 1.0;
        mark = LogMark();
        Check(sideMeanwhile && WidgetState().open && g_slotShown && SameLines(mark, {}),
              "...kept by its last state open (not by what was last answered, closed on the side edge): open again, nothing written");
        g_frozenNow = -1;
        g_adoptedAt = NowSeconds() - 2.0;
        mark = LogMark();
        Check(!WidgetState().open && SameLines(mark, { L"fluentflyout: widget slot closed (widget hidden or not in the dock's window)" }),
              "...later: \"widget slot closed (widget hidden or not in the dock's window)\"");
        trayhost::g_stubHosted.window = w1.hwnd;
        WidgetState();   // open again
        trayhost::g_stubHosted.window = nullptr;
        g_frozenNow = NowSeconds();
        g_adoptedAt = g_frozenNow - 1.65;
        mark = LogMark();
        Check(!WidgetState().open && SameLines(mark, { L"fluentflyout: widget slot closed (widget hidden or not in the dock's window)" }),
              "...1.65 s after being found: closed");
        g_frozenNow = -1;
        trayhost::g_stubHosted.window = w1.hwnd;
        WidgetState();
        g_ff.hideCompletely = true;
        g_media.value = false;
        mark = LogMark();
        Check(!WidgetState().open && SameLines(mark, { L"fluentflyout: widget slot closed (nothing playing, hide completely)" }), "the log: \"widget slot closed (nothing playing, hide completely)\"");
        g_media.value = true;
        WidgetState();
        trayhost::g_stubHosted.box = {};
        mark = LogMark();
        Check(!WidgetState().open && SameLines(mark, { L"fluentflyout: widget slot closed (empty)" }), "the log: \"widget slot closed (empty)\"");
        // not in the host window and nothing playing with "hide completely": the first reason is the one written
        trayhost::g_stubHosted.box = { 20, 15, 210, 55 };
        WidgetState();   // open again
        trayhost::g_stubHosted.window = nullptr;
        g_media.value = false;
        g_adoptedAt = NowSeconds() - 2.0;
        mark = LogMark();
        Check(!WidgetState().open && SameLines(mark, { L"fluentflyout: widget slot closed (widget hidden or not in the dock's window)" }),
              "the log, not in the host window and nothing playing (hide completely): \"widget slot closed (widget hidden or not in the dock's window)\" - that reason first");
        trayhost::g_stubHosted.window = w1.hwnd;
        g_media.value = true;
        g_ff.hideCompletely = false;
        g_media = {};
        trayhost::g_stubHosted.box = { 20, 15, 210, 55 };
        WidgetState();

        // ---- "nothing playing" and "media playing" (UpdateHasMedia), the lists handed over by hand
        g_media = {};
        SetSessions({});
        mark = LogMark();
        const double tMedia = NowSeconds();
        UpdateHasMedia(tMedia);
        const bool nothingAtOnce = !g_media.value;
        SetSessions({ L"Spotify.exe" });
        UpdateHasMedia(tMedia + 0.1);
        Check(!g_mediaWanted && nothingAtOnce && g_media.value && SameLines(mark, { L"fluentflyout: nothing playing", L"fluentflyout: media playing" }),
              "the log: \"nothing playing\" (a first list, empty), then \"media playing\"");
        {
            std::lock_guard<std::mutex> lk(g_mediaMu);
            g_mediaKnown = false;
        }
        g_listSeen = false; g_rawMedia = true; g_rawMediaFor.sessions = g_rawMediaFor.settings = -1;
        g_media = {};

        // ---- its flyouts gone without the hook hearing of it (hidden, destroyed): dropped by the next Tick
        Unhook();   // no events from the windows below: Tick's own look only
        const bool noFlyouts = g_flyouts.empty();
        HWND shownFly = MakeFlyout(mon.left + 16, mon.bottom - 200, 100, 50);
        HWND hiddenFly = MakeFlyout(mon.left + 16, mon.bottom - 300, 100, 50);
        HWND goneFly = MakeFlyout(mon.left + 16, mon.bottom - 400, 100, 50);
        const bool threeFlyouts = g_flyouts.size() == 3;
        ShowWindow(hiddenFly, SW_HIDE);
        DestroyWindow(goneFly);
        mark = LogMark();
        Tick(NowSeconds());
        Check(noFlyouts && threeFlyouts && g_flyouts.size() == 1 && g_flyouts[0].h == shownFly && SameLines(mark, {}),
              "Tick: a flyout hidden and one destroyed without a hook event dropped, the shown one kept (nothing written)");
        DestroyWindow(hiddenFly);
        DestroyWindow(shownFly);
        g_flyouts.clear(); g_lift.clear();
        Hook();

        // ---- its widget window closed (Tick): what is forgotten and what is kept
        g_parentTells = true; g_notInHostSince = 12.5; g_notInHostLogged = true;
        const HMONITOR shownMonitor = g_shownMonitor, monitor = g_monitor;
        const double shownAt = g_shownAt, adoptedAt = g_adoptedAt;
        const bool lastOpen = g_decidedOpen, slotShown = g_slotShown;
        const RECT lastBox = g_lastBox;
        const DWORD pidBefore = g_pid;
        const HANDLE procBefore = g_proc;
        const HWND first = w1.hwnd;
        Check(shownMonitor == realMain && monitor == realMain && lastOpen && slotShown && lastBox.right == 210 && pidBefore && procBefore, "fixture: the widget shown, its process watched");
        w1.Quit();
        mark = LogMark();
        double t = NowSeconds();
        Tick(t);
        Check(SameLines(mark, { L"fluentflyout: its widget window closed - keeping its place for the new one" }), "the log: \"its widget window closed - keeping its place for the new one\"");
        Check(!g_widget && g_lostWidget == first && g_lostAt == t && g_parent == (HWND)-1 && KindName(g_parentKind) == "None" && g_nextScan == t + 0.1,
              "...forgotten: the widget (kept as the lost one, since then), its parent and its kind; the next look in 0.1 s");
        Check(g_parentTells && g_notInHostSince == 12.5 && g_notInHostLogged && g_shownMonitor == shownMonitor && g_shownAt == shownAt && g_monitor == monitor &&
              g_decidedOpen == lastOpen && g_slotShown == slotShown && EqualRect(&g_lastBox, &lastBox) && g_adoptedAt == adoptedAt && g_pid == pidBefore && g_proc == procBefore,
              "...kept: whether its parent told the monitor, the 6 s wait and its line, where FluentFlyout showed it, the dock's monitor, the slot's last state and box, when it was found, its process");
        WindowThread w2;   // the same FluentFlyout's new widget window
        w2.Run(std::wstring(kHuntWrapper) + L"lines-2]", kHuntTitle);
        MakeChild(w2.hwnd, host.hwnd);
        trayhost::g_stubHosted.window = w2.hwnd;
        g_nextScan = 0;
        mark = LogMark();
        const double t2 = NowSeconds();
        Tick(t2);
        Check(SameLines(mark, { Fmt(L"fluentflyout: its new widget window is here after %.1f s", t2 - t), in(dock) }),
              "the log, the same FluentFlyout's new widget window: \"its new widget window is here after <s> s\" (no \"running\"), where it is");
        Check(g_widget == w2.hwnd && !g_lostWidget && g_lostAt == -1 && g_adoptedAt == t2 && g_pid == pidBefore && g_proc == procBefore &&
              g_shownMonitor == shownMonitor && g_shownAt == shownAt,
              "...taken: the lost one forgotten, found now; its process and where FluentFlyout showed it kept");
        w2.Quit();
        Tick(NowSeconds());
        g_lostAt = NowSeconds() - 4;   // closed more than 3 s ago
        g_nextScan = 0;
        mark = LogMark();
        const double t3 = NowSeconds();
        Tick(t3);
        Check(!Present() && g_nextScan == t3 + 0.5 && SameLines(mark, { L"fluentflyout: its widget window did not come back" }),
              "the log, none within 3 s: \"its widget window did not come back\" (the next look in 0.5 s)");
        {
            // its process ended and its widget window closed by the same Tick: the process first - "no longer
            // running" only (no "keeping its place")
            WindowThread w4;
            w4.Run(std::wstring(kHuntWrapper) + L"lines-4]", kHuntTitle);
            MakeChild(w4.hwnd, host.hwnd);
            g_nextScan = 0;
            Tick(NowSeconds());
            const bool found4 = g_widget == w4.hwnd;
            CloseHandle(g_proc);
            g_proc = CreateEventW(nullptr, TRUE, TRUE, nullptr);   // its process ended: a set event in its place
            w4.Quit();
            mark = LogMark();
            const double t6 = NowSeconds();
            Tick(t6);
            Check(found4 && !Present() && !g_lostWidget && g_nextScan == t6 + 2.0 && SameLines(mark, { L"fluentflyout: no longer running" }),
                  "the log, its process ended and its widget window closed at once: \"no longer running\" only (the process looked at first)");
            // a look that finds nothing: the next one in 2 s
            const double t7 = t6 + 2.5;
            mark = LogMark();
            Tick(t7);
            Check(!Present() && g_nextScan == t7 + 2.0 && SameLines(mark, {}), "a look that finds no widget window: the next look in 2 s, nothing written");
        }

        // ---- its process ended, the integration switched off
        WindowThread w3;
        w3.Run(std::wstring(kHuntWrapper) + L"lines-3]", kHuntTitle);
        MakeChild(w3.hwnd, host.hwnd);
        trayhost::g_stubHosted.window = w3.hwnd;
        g_nextScan = 0;
        mark = LogMark();
        Tick(NowSeconds());
        Check(g_widget == w3.hwnd && SameLines(mark, { running, in(dock), goesTo(otherText, L"from its settings", 2) }),
              "the log, found again: \"running\", where, the dock again (its settings file unchanged: no line)");
        CloseHandle(g_proc);
        g_proc = CreateEventW(nullptr, TRUE, TRUE, nullptr);   // its process ended: a set event in its place
        mark = LogMark();
        const double t4 = NowSeconds();
        Tick(t4);
        Check(!Present() && !g_proc && !g_pid && g_nextScan == t4 + 2.0 && SameLines(mark, { L"fluentflyout: no longer running" }),
              "the log, its process ended: \"no longer running\" (forgotten, the next look in 2 s)");
        g_nextScan = 0;
        Tick(NowSeconds());
        g_settings.fluentFlyout = false;
        mark = LogMark();
        Tick(NowSeconds());
        Check(!Present() && SameLines(mark, { L"fluentflyout: integration switched off" }), "the log, the integration switched off: \"integration switched off\"");
        mark = LogMark();
        Tick(NowSeconds());
        g_settings.fluentFlyout = true;
        Check(SameLines(mark, {}), "...once");
        mark = LogMark();
        Forget(L"test: nothing to forget");
        Check(SameLines(mark, {}), "Forget with nothing found: no line");

        // ---- Forget: what it clears and what it keeps (everything set first)
        g_nextScan = 0;
        Tick(NowSeconds());   // found again: its process watched, its windows hooked
        g_parent = other.hwnd; g_parentKind = ParentKind::Other; g_parentTells = true;
        g_notInHostSince = 12.5; g_notInHostLogged = true;
        g_monitor = realMain; g_lostWidget = first; g_lostAt = 3.5;
        g_shownMonitor = realMain; g_shownAt = 4.5;
        g_decidedOpen = g_slotShown = true; g_lastBox = { 1, 2, 3, 4 };
        g_adoptedAt = 5.5; g_nextScan = 6.5;
        g_namesWanted = true;
        WantMedia(true);
        {
            std::lock_guard<std::mutex> lk(g_mediaMu);
            g_mediaKnown = true;
        }
        g_media.value = false;
        g_rawMedia = false; g_rawMediaFor.sessions = 3; g_rawMediaFor.settings = 4; g_listSeen = true;   // worked out from a list
        {
            std::lock_guard<std::mutex> lk(g_mediaMu);
            g_sessions = { L"Fixture.Player" };
            g_appNames[L"Fixture.Player"] = L"Fixture Player";
        }
        const int mediaVersion = g_mediaVersion;
        const double mediaWantedAt = g_mediaWantedAt;
        Flyout flyout;
        flyout.h = other.hwnd;
        g_flyouts.push_back(flyout);
        g_lift[realMain] = 7;
        const bool dirty = g_choiceDirty;
        const int fileSetting = g_fileSetting, fileMonitor = g_fileMonitor, ffVersion = g_ffVersion;
        const double fileAt = g_fileAt, monitorsAt = g_monitorsAt;
        const size_t monitorCount = g_monitorHandles.size();
        const HANDLE proc = g_proc;
        Check(g_widget == w3.hwnd && g_pid && proc && g_hooks[0] && g_hooks[1] && g_mediaWanted, "fixture: everything set before Forget");
        mark = LogMark();
        Forget(L"test: forget");
        DWORD flags = 0;
        const RECT none{};
        Check(SameLines(mark, { L"fluentflyout: test: forget" }), "Forget: \"fluentflyout: <why>\" while something was found");
        Check(g_parent == (HWND)-1 && KindName(g_parentKind) == "None" && !g_parentTells, "Forget clears: its parent, the kind, whether it told the monitor");
        Check(g_notInHostSince == -1 && !g_notInHostLogged, "Forget clears: the 6 s wait and its line");
        Check(!g_widget && !g_lostWidget && g_lostAt == -1 && !g_monitor, "Forget clears: the widget, the lost one and when, the dock's monitor");
        Check(!g_shownMonitor && g_shownAt == -1, "Forget clears: where FluentFlyout showed it");
        Check(!g_decidedOpen && !g_slotShown && EqualRect(&g_lastBox, &none), "Forget clears: the slot's last state and box");
        Check(!g_namesWanted && !g_mediaWanted && !g_mediaKnown, "Forget clears: apps named, the session list wanted and known");
        Check(!g_pid && !g_proc && !GetHandleInformation(proc, &flags) && !g_hooks[0] && !g_hooks[1] && g_flyouts.empty() && g_lift.empty(),
              "Forget clears: its process (the handle closed), the hooks, the flyouts and their lifts");
        Check(g_adoptedAt == 5.5 && g_nextScan == 6.5 && g_choiceDirty == dirty && g_fileSetting == fileSetting && g_fileMonitor == fileMonitor && g_fileAt == fileAt &&
              g_monitorsAt == monitorsAt && g_monitorHandles.size() == monitorCount && g_ffVersion == ffVersion && g_ff.loaded && !g_media.value,
              "Forget keeps: when it was found, the next look, the monitors and the file's setting, FluentFlyout's settings, what counted last (nothing plays)");
        Check(g_rawMedia && g_rawMediaFor.sessions == -1 && g_rawMediaFor.settings == -1 && !g_listSeen,
              "Forget clears (the list no longer wanted): what was worked out from the sessions - something may play, for no version, no list seen");
        {
            std::lock_guard<std::mutex> lk(g_mediaMu);
            Check(g_sessions == std::vector<std::wstring>({ L"Fixture.Player" }) && g_appNames[L"Fixture.Player"] == L"Fixture Player" &&
                      g_mediaVersion == mediaVersion && g_mediaWantedAt == mediaWantedAt,
                  "Forget keeps: the last session list, the apps' names, the list's version, when it was last wanted");
        }

        // ---- Adopt: a FluentFlyout found anew, another one than the one known, the same one's new window
        g_parent = other.hwnd; g_parentKind = ParentKind::Other; g_parentTells = true;
        g_notInHostSince = 12.5; g_notInHostLogged = true; g_monitor = realMain;
        g_decidedOpen = g_slotShown = true; g_lastBox = { 1, 2, 3, 4 };
        g_shownMonitor = realMain; g_shownAt = 4.5;
        g_lostWidget = first; g_lostAt = 3.5;   // a lost window, no FluentFlyout known (g_pid 0)
        g_flyouts.push_back(flyout);
        mark = LogMark();
        t = NowSeconds();
        Adopt(w3.hwnd, t);
        Check(SameLines(mark, { running }) && g_widget == w3.hwnd && g_adoptedAt == t && g_pid == GetCurrentProcessId() && g_proc && g_hooks[0] &&
              !g_shownMonitor && g_shownAt == -1 && !g_lostWidget && g_lostAt == -1,
              "Adopt, a FluentFlyout found anew: \"running\"; where it showed and the lost window forgotten, its process watched and hooked");
        Check(g_parent == other.hwnd && KindName(g_parentKind) == "Other" && g_parentTells && g_notInHostSince == 12.5 && g_notInHostLogged && g_monitor == realMain &&
              g_decidedOpen && g_slotShown && g_lastBox.right == 3 && g_flyouts.size() == 1,
              "...kept (Tick looks at them next): its parent and kind, the 6 s wait, the dock's monitor, the slot, the flyouts");
        g_pid = GetCurrentProcessId() + 4;   // another FluentFlyout was known
        g_shownMonitor = realMain; g_shownAt = 4.5;
        g_lift[realMain] = 7;
        const HANDLE procOther = g_proc;   // the handle for the one known
        mark = LogMark();
        Adopt(w3.hwnd, t = NowSeconds());
        Check(SameLines(mark, { running }) && g_pid == GetCurrentProcessId() && g_proc && g_hooks[0] && g_flyouts.empty() && g_lift.empty() && !g_shownMonitor && g_shownAt == -1,
              "Adopt, another FluentFlyout than the one known: \"running\"; the last one's process forgotten first (its flyouts and lifts gone)");
        Check((!GetHandleInformation(procOther, &flags) || procOther == g_proc) && g_parent == other.hwnd && KindName(g_parentKind) == "Other" && g_parentTells &&
                  g_notInHostSince == 12.5 && g_notInHostLogged && g_monitor == realMain && g_decidedOpen && g_slotShown && g_lastBox.right == 3,
              "...the last one's handle closed (or its slot taken by the new one); kept: its parent and kind, whether it told, the 6 s wait, the dock's monitor, the slot");
        g_widget = nullptr; g_lostWidget = first; g_lostAt = t - 1.5;
        g_shownMonitor = realMain; g_shownAt = 4.5;
        g_flyouts.push_back(flyout);
        const HANDLE procSame = g_proc;
        mark = LogMark();
        const double t5 = t;
        Adopt(w3.hwnd, t5);
        Check(SameLines(mark, { L"fluentflyout: its new widget window is here after 1.5 s" }) && g_widget == w3.hwnd && !g_lostWidget && g_lostAt == -1 && g_adoptedAt == t5 &&
              g_proc == procSame && g_shownMonitor == realMain && g_shownAt == 4.5 && g_flyouts.size() == 1,
              "Adopt, the same FluentFlyout's new window: \"its new widget window is here after 1.5 s\"; its process, where it showed and the flyouts kept");
        Check(g_pid == GetCurrentProcessId() && g_parent == other.hwnd && KindName(g_parentKind) == "Other" && g_parentTells && g_notInHostSince == 12.5 &&
                  g_notInHostLogged && g_monitor == realMain && g_decidedOpen && g_slotShown && g_lastBox.right == 3,
              "...kept too: its parent and kind, whether it told, the 6 s wait and its line, the dock's monitor, the slot");
        g_flyouts.clear(); g_lift.clear();
        g_media = {};

        // ---- Shutdown with the media thread: FluentFlyout forgotten first, then the stop, 1.5 s at most
        Check(MediaEvents(), "fixture: the media thread's events");
        ResetEvent(g_mediaStop); ResetEvent(g_mediaWake);
        g_fakeMedia.hang = true;
        g_fakeMedia.inLook = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_fakeMedia.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_fakeMedia.mark = LogMark();
        HANDLE th = CreateThread(nullptr, 0, FakeMediaThread, nullptr, 0, nullptr);
        HANDLE watch = nullptr;
        DuplicateHandle(GetCurrentProcess(), th, GetCurrentProcess(), &watch, SYNCHRONIZE, FALSE, 0);
        g_mediaThread = th;
        WantMedia(true);   // the list wanted: the media thread looks at once (and hangs in that look)
        Check(th && watch && WaitForSingleObject(g_fakeMedia.inLook, 3000) == WAIT_OBJECT_0, "fixture: the media thread hangs in a look");
        mark = LogMark();
        g_waitedMs = 0;
        double from = NowSeconds();
        Shutdown();
        const double tookHanging = NowSeconds() - from;
        Check(g_waitedMs == 1500 && g_mediaWake && WaitForSingleObject(g_mediaWake, 0) != WAIT_FAILED,
              "...Shutdown waits for the media thread with a timeout of 1500 ms; its wake-up event left open");
        Check(SameLines(mark, { L"fluentflyout: shutting down" }), "the log, Shutdown: \"shutting down\"");
        Check(tookHanging >= 1.4 && tookHanging < 3.0 && !g_mediaThread && WaitForSingleObject(watch, 0) == WAIT_TIMEOUT && !GetHandleInformation(th, &flags),
              "Shutdown, the media thread hanging in a look: back after 1.5 s, the thread left running, its handle closed and let go");
        Check(g_fakeMedia.sawStop && g_fakeMedia.forgottenFirst, "...FluentFlyout forgotten (\"shutting down\", the list not wanted) before the stop is set");
        Check(g_mediaStop && WaitForSingleObject(g_mediaStop, 0) == WAIT_OBJECT_0, "...the stop event left set and open");
        SetEvent(g_fakeMedia.release);
        Check(WaitForSingleObject(watch, 3000) == WAIT_OBJECT_0 && g_fakeMedia.looks == 1, "...its look over, the thread ends (no other look)");
        CloseHandle(watch);
        ResetEvent(g_mediaStop); ResetEvent(g_mediaWake);
        g_fakeMedia.hang = false;
        g_fakeMedia.looks = 0;
        g_nextScan = 0;
        Tick(NowSeconds());   // found again
        th = CreateThread(nullptr, 0, FakeMediaThread, nullptr, 0, nullptr);
        watch = nullptr;
        DuplicateHandle(GetCurrentProcess(), th, GetCurrentProcess(), &watch, SYNCHRONIZE, FALSE, 0);
        g_mediaThread = th;
        mark = LogMark();
        g_waitedMs = 0;
        from = NowSeconds();
        Shutdown();
        const double tookAsleep = NowSeconds() - from;
        Check(g_waitedMs == 1500, "...Shutdown, the media thread asleep: the same wait (1500 ms at most)");
        Check(th && watch && !g_widget && SameLines(mark, { L"fluentflyout: shutting down" }) && tookAsleep < 1.0 && !g_mediaThread &&
              WaitForSingleObject(watch, 0) == WAIT_OBJECT_0 && g_fakeMedia.looks == 0,
              "Shutdown, the media thread asleep: \"shutting down\", the thread ended and waited for at once, no look");
        CloseHandle(watch);
        CloseHandle(g_fakeMedia.inLook);
        CloseHandle(g_fakeMedia.release);
        g_fakeMedia.inLook = g_fakeMedia.release = nullptr;

        RemovePropW(host.hwnd, kTrayHostProp);
        RemovePropW(stale.hwnd, kTrayHostProp);
        trayhost::g_stubWindow = nullptr;
        trayhost::g_stubHosted = {};
        g_fakeExplorer = nullptr;
    }

    fluentflyout::Shutdown();
    DeleteFileW(fileA.c_str()); DeleteFileW(fileB.c_str());
    return Summary("fluent_lifecycle_test");
}
