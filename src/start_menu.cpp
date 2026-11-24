#include "start_menu.h"
#include "start_menu_logic.h"
#include "system.h"
#include "taskbar_start.h"
#include "app_visibility.h"
#include <dwmapi.h>
#include <objbase.h>
#include <servprov.h>
#include <atomic>
#include <mutex>
#include <thread>

// The start button (start_menu_logic.h says why it no longer only presses the Windows key, and the ladder a
// click goes down: the shell's request, the taskbar's own Start button - taskbar_start.h - the Windows key).
// Nothing a click found out decides what a later click sends: no verdict is kept, in the process or on disk.
// (Kept between clicks, for the log's lines and to save calls only: Start's window and the names below - g_start -,
// when the display came on, IAppVisibility on the UI thread, and each taskbar's Start button in taskbar_start.cpp.)
// (data\taskbar-start.txt, the verdict mod.18 to
// mod.35 kept there, is neither read, written nor removed: a version put back reads its own file again.)
//
// The shell's launcher: Explorer's immersive shell service. Undocumented; the ids and the order of the
// methods are the ones RetroBar (StartMenuMonitor.cs, 2026-10) and ExplorerPatcher (StartMenu.h,
// TwinUIPatches.cpp, 2026-07) use on Windows 10 1607 and later, Windows 11 included. Each table is
// declared only up to the last method called. Should a Windows release change them, QueryService fails
// (the interface id names the table) and the click goes on at once with the taskbar's button, then the key.

namespace startmenu {
namespace {

const CLSID kImmersiveShell = { 0xc2f03a33, 0x21f5, 0x47fa, { 0xb4, 0xbb, 0x15, 0x63, 0x62, 0xa2, 0xf2, 0x39 } };
const GUID kLauncherService = { 0x6f86e01c, 0xc649, 0x4d61, { 0xbe, 0x23, 0xf1, 0x32, 0x2d, 0xde, 0xca, 0x9d } };
const IID kLauncherIid = { 0xd8d60399, 0xa0f1, 0xf987, { 0x55, 0x51, 0x32, 0x1f, 0xd1, 0xb4, 0x98, 0x64 } };
const GUID kMonitorService = { 0x47094e3a, 0x0cf2, 0x430f, { 0x80, 0x6f, 0xcf, 0x9e, 0x4f, 0x0f, 0x12, 0xdd } };
const IID kMonitorServiceIid = { 0x4d4c1e64, 0xe410, 0x4faa, { 0xba, 0xfa, 0x59, 0xca, 0x06, 0x9b, 0xfe, 0xc2 } };
// IAppVisibility: app_visibility.h (shell_front.cpp's LauncherVisible asks the same), under the names this file gave it
using ::kAppVisibility;
using ::kAppVisibilityIid;

struct IMonitorService : IUnknown {   // IImmersiveMonitorService
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT, IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFromHandle(HMONITOR, IUnknown**) = 0;
};

struct ILauncher : IUnknown {         // IImmersiveLauncher (Windows 10 1607+)
    virtual HRESULT STDMETHODCALLTYPE ShowStartView(int method, int flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE Dismiss(int method) = 0;
    virtual HRESULT STDMETHODCALLTYPE DismissToLastDesktopApplication(int method) = 0;
    virtual HRESULT STDMETHODCALLTYPE DismissSynchronouslyWithoutTransition() = 0;
    virtual HRESULT STDMETHODCALLTYPE IsVisible(BOOL* visible) = 0;
    virtual HRESULT STDMETHODCALLTYPE OnStartButtonPressed(int show, int dismiss) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetForeground() = 0;
    virtual HRESULT STDMETHODCALLTYPE ConnectToMonitor(IUnknown* monitor) = 0;
};

using IVisibility = IAppVisibilityDecl;   // IAppVisibility

constexpr int kShowFromStartButton = 11;      // ILSM_STARTBUTTON
constexpr int kIgnoreForegroundError = 4;     // ILSF_IGNORE_SET_FOREGROUND_ERROR (as RetroBar)

// The window in front: which program, and whether it runs as administrator.
struct Front {
    std::wstring exe;      // file name; empty: nothing in front, "?" : a program the dock may not look into
    bool elevated = false; // asked only where it counts (ProgramOf)
    bool start = false;    // it is Start
};

// The program of process pid. elevation: whether it runs as administrator too - two more calls into Windows,
// made only at a click's first look (can the Windows key reach Start; for the log). started (when given): when Start's
// process started (FILETIME; 0: another program, or not to be had), read with the same handle - for the log only
// (StartWindowNow).
Front ProgramOf(DWORD pid, bool elevation, uint64_t* started = nullptr) {
    Front f;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) { f.exe = L"?"; return f; }
    wchar_t path[MAX_PATH * 2];
    DWORD n = ARRAYSIZE(path);
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        f.exe = name ? name + 1 : path;
    }
    HANDLE token = nullptr;
    if (elevation && OpenProcessToken(p, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD got = 0;
        f.elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got) && e.TokenIsElevated;
        CloseHandle(token);
    }
    f.start = !_wcsicmp(f.exe.c_str(), L"StartMenuExperienceHost.exe");
    if (started) {
        FILETIME created{}, exited{}, kernel{}, user{};
        *started = f.start && GetProcessTimes(p, &created, &exited, &kernel, &user) ? (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
    }
    CloseHandle(p);
    return f;
}

// The program of the window in front (elevation: as ProgramOf). window (when given): that window, for the log.
Front InFrontNow(bool elevation = false, HWND* window = nullptr) {
    HWND fg = GetForegroundWindow();
    if (window) *window = fg;
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid) return {};
    return ProgramOf(pid, elevation);
}

// What a wait's last look found in front. A window's program never changes: while the same window stays in front
// its program is not looked into again (no process opened at each look). Kept only once the program's name was read.
struct FrontSeen {
    HWND window = nullptr;
    DWORD pid = 0;
    bool start = false;
    std::wstring exe;   // its program's name: the click's dock.log lines name that window by it (no process opened again)
};

// Start's own window is in front (InFrontNow().start); seen: the last look's, brought up to date.
bool StartInFront(FrontSeen& seen) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid) return false;
    if (fg == seen.window && pid == seen.pid) return seen.start;
    const Front f = ProgramOf(pid, false);
    if (!f.exe.empty() && f.exe != L"?") seen = { fg, pid, f.start, f.exe };
    return f.start;
}

bool SelfElevated() {
    static const bool elevated = [] {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION e{};
        DWORD got = 0;
        bool r = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got) && e.TokenIsElevated;
        CloseHandle(token);
        return r;
    }();
    return elevated;
}

// The worker the button waits on: its number (0: none) and since when (GetTickCount64). A worker stuck
// in a call to a hung Explorer is left behind by the next click (StillOpening); only the current one
// takes further clicks, sends and clears the number when done.
std::atomic<uint32_t> g_current{ 0 };
std::atomic<uint64_t> g_since{ 0 };
std::atomic<bool> g_again{ false };    // clicked again meanwhile
std::atomic<bool> g_closed{ false };   // the button closed Start meanwhile: send nothing more
uint32_t g_lastId = 0;                 // UI thread
std::mutex g_mu;                       // a worker's dock.log lines, kept together (a left-behind worker may still finish)
std::atomic<double> g_displayOnAt{ -1 };   // NowSeconds when the display last came on (DisplayCameOn); -1: not seen this run

// The ways a click may use - all of them in the dock. The tests take some away: the worker test's safe mode asks
// no shell and presses no taskbar button (no real Start opens), the live test tries each way by itself.
constexpr int kWayShell = 1, kWayTaskbar = 2, kWayKey = 4;
std::atomic<int> g_ways{ kWayShell | kWayTaskbar | kWayKey };

// The launcher for click id, connected to the dock's monitor so Start comes there (as with the
// taskbar's own button on that monitor). connected: how that went (Start still comes if it failed).
// E_ABORT: a later click took over while Explorer kept this one waiting - nothing more is asked of Explorer,
// and its one launcher is not moved to this click's monitor while that click's Start comes.
HRESULT MakeLauncher(HMONITOR monitor, uint32_t id, ComPtr<ILauncher>& launcher, HRESULT& connected) {
    ComPtr<IServiceProvider> shell;
    HRESULT hr = CoCreateInstance(kImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_NO_CODE_DOWNLOAD, IID_IServiceProvider,
                                  reinterpret_cast<void**>(shell.GetAddressOf()));
    if (FAILED(hr)) return hr;
    if (g_current.load() != id) return E_ABORT;   // a hung Explorer holds a caller here first
    hr = shell->QueryService(kLauncherService, kLauncherIid, reinterpret_cast<void**>(launcher.GetAddressOf()));
    if (FAILED(hr)) return hr;
    if (!launcher) return E_NOINTERFACE;
    ComPtr<IMonitorService> monitors;
    ComPtr<IUnknown> where;
    connected = shell->QueryService(kMonitorService, kMonitorServiceIid, reinterpret_cast<void**>(monitors.GetAddressOf()));
    if (SUCCEEDED(connected) && monitors) connected = monitors->GetFromHandle(monitor, where.GetAddressOf());
    if (g_current.load() != id) { launcher.Reset(); return E_ABORT; }   // those calls may have hung as well
    if (SUCCEEDED(connected) && where) connected = launcher->ConnectToMonitor(where.Get());
    else if (SUCCEEDED(connected)) connected = E_POINTER;
    return S_OK;
}

// Start is up: IAppVisibility says so - the documented answer - or Start's own window is in front. The launcher is
// asked (the undocumented IsVisible) only when given: by a click that could make no IAppVisibility, never at each
// look of a usual wait. seen: a wait's (StartInFront), nullptr for a single look. by (when given): which one said so,
// for the log - kByVisibility, kByLauncher, kByFront (left as it was when none did).
constexpr int kByVisibility = 1, kByLauncher = 2, kByFront = 3;
bool Visible(IVisibility* vis, ILauncher* launcher, FrontSeen* seen = nullptr, int* by = nullptr) {
    BOOL v = FALSE;
    if (vis && SUCCEEDED(vis->IsLauncherVisible(&v)) && v) { if (by) *by = kByVisibility; return true; }
    v = FALSE;
    if (launcher && SUCCEEDED(launcher->IsVisible(&v)) && v) { if (by) *by = kByLauncher; return true; }
    FrontSeen once;
    const bool front = StartInFront(seen ? *seen : once);
    if (front && by) *by = kByFront;
    return front;
}

// Seconds since Explorer - the process of the shell's window - started; -1: no telling. Kernel calls only: nothing
// is sent to Explorer, so a hung one does not hold this up.
double ExplorerAge(HWND shell) {
    DWORD pid = 0;
    if (!shell || !GetWindowThreadProcessId(shell, &pid) || !pid) return -1;
    const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return -1;
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    const BOOL got = GetProcessTimes(p, &created, &exited, &kernel, &user);
    CloseHandle(p);
    if (!got) return -1;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER c, n;
    c.LowPart = created.dwLowDateTime;
    c.HighPart = created.dwHighDateTime;
    n.LowPart = now.dwLowDateTime;
    n.HighPart = now.dwHighDateTime;
    return n.QuadPart >= c.QuadPart ? (n.QuadPart - c.QuadPart) / 1e7 : -1;   // the clock went back since: no telling
}

// What became of the taskbar's own Start button on a click, for the log.
struct TaskbarNote {
    const wchar_t* skipped = nullptr;                        // why it is never looked for on this click; nullptr: it may be
    taskbarstart::Found found = taskbarstart::Found::NoButton;   // (looked for only when its turn came)
    HRESULT hr = S_OK;                                       // the press
    const wchar_t* how = L"";
    int before = -1, after = -1;                             // its toggle state around the press (Pressed), for the log
    double took = -1;                                        // the press call's seconds (Pressed::took), for the log; -1: no press
    bool held = false;                                       // found and not pressed: its state did not read off (Pressed::held)
    double turnAt = 0;                                       // when its turn was over, pressed or not (the click's timeline)
};

// ", state off -> on": what a toggle press saw of the button; empty when nothing was read. (Not read again once a wait
// ran out: the worker still holds the click then, and a click meanwhile would be counted by no one.)
void StateNote(const TaskbarNote& tb, wchar_t* out, size_t n) {
    out[0] = 0;
    if (tb.before < 0 && tb.after < 0) return;
    _snwprintf_s(out, n, _TRUNCATE, L", state %ls -> %ls", ToggleText(tb.before), ToggleText(tb.after));
}

// Why a click is slow (Opening::Slow), for the log: Windows or Explorer started minutes ago, and the first call into
// Explorer on this click that took kBusyCall.
struct SlowNote {
    bool early = false;
    double up = 0, explorer = -1;    // Windows' uptime and Explorer's age at the click (seconds; -1: no telling)
    const wchar_t* call = nullptr;   // the first call that slow...
    double took = 0;                 // ...and its seconds
};
std::wstring SlowText(const SlowNote& s) {
    std::wstring t;
    wchar_t b[192];
    if (s.early) {
        wchar_t age[48] = L"Explorer's age unknown";
        if (s.explorer >= 0) _snwprintf_s(age, ARRAYSIZE(age), _TRUNCATE, L"Explorer %.0f s", s.explorer);
        _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"Windows up %.0f s, %ls: started minutes ago", s.up, age);
        t = b;
    }
    if (s.call) {
        _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"%ls%ls took %.0f ms: Explorer busy", t.empty() ? L"" : L"; ", s.call, s.took * 1000);
        t += b;
    }
    return t;
}

// ---- What dock.log tells of an opening click besides what went out (mod.36): the right to come in front as Click
// handed it on, the window in front and Start's own window at the moments that count, which check saw Start and on
// which monitor, when the more clicks came, whether the user gave input before the first way went out. Read only with
// calls that go to no other program's thread - user32 (GetForegroundWindow, GetWindowThreadProcessId, GetClassNameW,
// IsWindowVisible, IsWindow, FindWindowExW, GetLastInputInfo, MonitorFromWindow, MonitorFromPoint, GetMonitorInfoW),
// DWM's cloak (DwmGetWindowAttribute) and the kernel (a process's name and start, the date) - once the click's timeline
// has started (t0), never inside a measure of a busy Explorer (reaching the shell, making IAppVisibility, the request,
// a press's took); nothing is decided by them.
// No window title is read. (2026-11-20, seen on a test PC: a trusted press went through, the button off -> on, and Start
// did not come in 8.3 s - and nothing in the log could tell why.)

// What the UI thread's Click did about the right to come in front (AllowShellForeground), handed to the worker as it was.
struct ClickNote {
    ForegroundRight right = ForegroundRight::Had;   // TakeForegroundRight's answer
    DWORD explorer = 0;     // the shell window's process (0: no shell window)
    bool allowed = false;   // AllowSetForegroundWindow(explorer)
    DWORD error = 0;        // its GetLastError when it failed
    uint64_t tick = 0;      // GetTickCount64 at the click (g_since, after the right was taken): later input is the user's
};

// A window as the lines name it: "<program> <class>[ cloaked][ hidden] <handle>" (apps_click.cpp's front, with the handle).
struct WindowNote {
    HWND window = nullptr;   // nullptr: nothing in front
    DWORD pid = 0;
    DWORD cloak = 0;         // DWMWA_CLOAKED as Windows gives it: 0, 1 the app, 2 the shell, 4 inherited
    bool hidden = false;
    wchar_t cls[96] = L"";
};
WindowNote NoteOf(HWND w) {
    WindowNote n;
    n.window = w;
    if (!w) return n;
    GetWindowThreadProcessId(w, &n.pid);
    GetClassNameW(w, n.cls, ARRAYSIZE(n.cls));
    n.hidden = !IsWindowVisible(w);
    DWORD c = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &c, sizeof(c)))) n.cloak = c;
    return n;
}

constexpr DWORD kNoCloak = 0xFFFFFFFF;   // no window, or its cloak not to be read
DWORD CloakOfWindow(HWND w) {
    DWORD c = 0;
    return w && SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &c, sizeof(c))) ? c : kNoCloak;
}

// Start's own window: StartMenuExperienceHost.exe's CoreWindow (EnumWindows does not list it; FindWindowEx finds it, as
// tests\start_ghost_live_test.cpp does). Remembered across clicks and looked up again only once it is gone (IsWindow, the
// same process), so a click opens no process for it - nor again for the other programs' CoreWindows met on the way.
// The worker that holds g_startBusy alone touches it: a second worker meanwhile (one left behind that finished late)
// does not wait - it reports Start's window as not found - and each takes a copy (StartSeen) for its lines.
// named: the programs of the windows the lines named (the CoreWindows met on the way too) - open Start's front, its search
// box's window on 25H2 and 26H2, is named at every usual click, and its process is opened once a run, not at each click.
// A window and its process id, as read of it now, name the same program as when it was kept (a handle of a window gone is
// not handed out again soon, with the same process id still less).
struct NamedWindow {
    HWND window;
    DWORD pid;
    wchar_t exe[64];   // (plain storage: workers may outlive statics)
};
struct StartWindow {
    HWND window = nullptr;
    DWORD pid = 0;
    uint64_t started = 0;   // its process's start (FILETIME), 0: not to be had
    DWORD others[8] = {};   // processes of CoreWindows that are not Start's
    int nOthers = 0, nextOther = 0;
    NamedWindow named[8] = {};
    int nextNamed = 0;
};
StartWindow g_start;
std::atomic<bool> g_startBusy{ false };
struct StartSeen {
    HWND window = nullptr;
    DWORD pid = 0;
    uint64_t started = 0;
};

// g_startBusy held
void KeepName(StartWindow& s, HWND w, DWORD pid, const std::wstring& exe) {
    if (!w || !pid || exe.empty() || exe == L"?" || exe.size() >= ARRAYSIZE(s.named[0].exe)) return;
    for (const auto& n : s.named) if (n.window == w && n.pid == pid) return;
    NamedWindow& n = s.named[s.nextNamed];
    n.window = w;
    n.pid = pid;
    wcscpy_s(n.exe, exe.c_str());
    s.nextNamed = (s.nextNamed + 1) % static_cast<int>(ARRAYSIZE(s.named));
}

StartSeen StartWindowNow() {
    if (g_startBusy.exchange(true)) return {};
    StartWindow& s = g_start;
    StartSeen seen;
    if (s.window) {
        DWORD pid = 0;
        if (IsWindow(s.window) && GetWindowThreadProcessId(s.window, &pid) && pid == s.pid) seen = { s.window, s.pid, s.started };
        else {
            s.window = nullptr;   // Start's process went (Start or Explorer restarted): found again
            s.pid = 0;
            s.started = 0;
        }
    }
    HWND w = nullptr;
    for (int guard = 0; !seen.window && guard < 16; ++guard) {
        w = FindWindowExW(nullptr, w, L"Windows.UI.Core.CoreWindow", nullptr);
        if (!w) break;
        DWORD pid = 0;
        if (!GetWindowThreadProcessId(w, &pid) || !pid) continue;
        bool other = false;
        for (int i = 0; i < s.nOthers && !other; ++i) other = s.others[i] == pid;
        if (other) continue;
        uint64_t started = 0;
        const Front f = ProgramOf(pid, false, &started);
        KeepName(s, w, pid, f.exe);
        if (f.start) {
            s.window = w;
            s.pid = pid;
            s.started = started;
            seen = { w, pid, started };
            break;
        }
        s.others[s.nextOther] = pid;
        s.nextOther = (s.nextOther + 1) % ARRAYSIZE(s.others);
        if (s.nOthers < static_cast<int>(ARRAYSIZE(s.others))) ++s.nOthers;
    }
    g_startBusy = false;
    return seen;
}

// A way that went out: when it had (the click's timeline), the window in front right after it, and Start's cloak then.
struct SentNote {
    Way way = Way::None;
    double at = 0;
    WindowNote front;
    DWORD startCloak = kNoCloak;
};

// What a click's worker noted on its way (Run), for its lines.
struct Notes {
    ClickNote click;
    HWND frontWindow = nullptr;   // the window in front at the click (Run's front, InFrontNow) ...
    std::wstring frontExe;        // ...and its program
    WindowNote before, end;       // in front: at the click; at the end
    SentNote sent[3];             // the ways in the order they went out (each at most once a click)
    int nSent = 0;
    double requestTook = -1;      // the shell's request: the call's seconds (-1: not asked)
    StartSeen start;                                 // Start's own window (StartWindowNow): at the click, else at the end
    DWORD startBefore = kNoCloak, startEnd = kNoCloak;   // its cloak at the click and at the end
    bool inputTaken = false, inputRead = false;      // GetLastInputInfo asked before the first way went out; it answered
    DWORD input = 0;                                 // ...its tick
    int seenBy = 0;                                  // which check saw Start (Visible's by); 0: none did
    static constexpr int kTimes = 16;
    double againAt[kTimes] = {};                     // the more clicks: when the worker took them in (seconds after the click)
    int agains = 0;
};

std::wstring Hex8(HWND w) {
    wchar_t b[16];
    _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"%08lX", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(w) & 0xFFFFFFFFu));
    return b;
}
std::wstring MonitorText(HMONITOR m) {
    MONITORINFO mi = { sizeof(mi) };
    if (!m || !GetMonitorInfoW(m, &mi)) return L"?";
    wchar_t b[64];
    _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"%ld,%ld %ldx%ld", mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top);
    return b;
}
// which taskbar's button the dock presses for this monitor: taskbar_start.cpp's TaskbarOn tells them apart the same way
std::wstring DockMonitorText(HMONITOR m) {
    const bool primary = m == MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    return MonitorText(m) + (primary ? L" (the primary monitor: the main taskbar)" : L" (another monitor: a secondary taskbar)");
}
std::wstring CloakText(DWORD c) { return c == kNoCloak ? std::wstring(L"?") : std::to_wstring(c); }
const wchar_t* SentText(Way w) { return w == Way::Taskbar ? L"the press" : w == Way::Shell ? L"the shell's request" : L"the Windows key"; }

// The programs of the noted windows: from what the click read of them anyway (the wait's last look, the window in front
// at the click - "?" too: a program refused then is not asked again), Start's process (StartWindowNow: checked at this
// click), what earlier clicks named (g_start.named), else looked into now - once a window, and kept for the next clicks.
class Programs {
public:
    explicit Programs(DWORD startPid) : startPid_(startPid) {}
    void Know(HWND w, DWORD pid, const std::wstring& exe) {
        if (w && !exe.empty()) known_.push_back({ w, pid, exe });
    }
    std::wstring Of(const WindowNote& n) {
        for (const auto& k : known_) if (k.window == n.window && k.pid == n.pid) return k.exe;
        std::wstring exe;
        if (n.pid && n.pid == startPid_) exe = L"StartMenuExperienceHost.exe";
        else if (n.pid && !g_startBusy.exchange(true)) {   // (another worker in it: looked into, as without it)
            for (const auto& k : g_start.named) if (k.window == n.window && k.pid == n.pid) { exe = k.exe; break; }
            g_startBusy = false;
        }
        if (exe.empty()) {
            exe = n.pid ? ProgramOf(n.pid, false).exe : std::wstring(L"?");
            if (n.pid && !g_startBusy.exchange(true)) {
                KeepName(g_start, n.window, n.pid, exe);
                g_startBusy = false;
            }
        }
        known_.push_back({ n.window, n.pid, exe });   // (a refused one too: not asked twice for one line)
        return exe;
    }
private:
    struct Known { HWND window; DWORD pid; std::wstring exe; };
    std::vector<Known> known_;
    DWORD startPid_;
};

// Start's own window, or its search box's (SearchHost.exe on 25H2 and 26H2: the window in front while Start is open)
bool StartsProgram(const std::wstring& exe) {
    return !_wcsicmp(exe.c_str(), L"StartMenuExperienceHost.exe") || !_wcsicmp(exe.c_str(), L"SearchHost.exe");
}

std::wstring WindowText(const WindowNote& n, Programs& programs) {
    if (!n.window) return L"nothing";
    const std::wstring exe = programs.Of(n);
    std::wstring s = (exe.empty() ? std::wstring(L"?") : exe) + L" " + (n.cls[0] ? n.cls : L"?");
    if (n.cloak) s += L" cloaked";
    if (n.hidden) s += L" hidden";
    s += L" " + Hex8(n.window);
    if (!_wcsicmp(exe.c_str(), L"SearchHost.exe")) s += L" cloak " + std::to_wstring(n.cloak);   // open Start's front on 25H2 and 26H2
    return s;
}

// The pieces the click's lines add (Report): the usual line's tail, the line after one that went otherwise, the more
// clicks' times. endAt: NowSeconds at the end of the wait (t0 + o.At(): no clock read more).
struct Around {
    std::wstring tail, line, clicks;
};
Around Describe(const Notes& m, const Opening& o, const FrontSeen& seen, HMONITOR monitor, double reach, double endAt, const TaskbarNote& tb,
                const std::wstring& slow) {
    Around a;
    Programs programs(m.start.pid);
    programs.Know(seen.window, seen.pid, seen.exe);   // first: a name read later than a refusal at the click
    programs.Know(m.frontWindow, m.before.pid, m.frontExe);
    wchar_t b[192];
    const std::wstring right = std::wstring(L"the dock's right to change the front window: ") + ForegroundRightText(m.click.right);
    std::wstring allowed = L"yes";
    if (!m.click.explorer) allowed = L"no (no shell window)";
    else if (!m.click.allowed) { _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"no (error %lu)", m.click.error); allowed = b; }
    // the calls' milliseconds: the shell's request; the press - or, a button left alone, the read of its state
    std::wstring took;
    if (m.requestTook >= 0) { _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"; the request took %.0f ms", m.requestTook * 1000); took = b; }
    if (tb.took >= 0) {
        _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"; %ls took %.0f ms", tb.held ? L"reading the taskbar button's state" : L"the press", tb.took * 1000);
        took += b;
    }
    const std::wstring slowly = slow.empty() ? std::wstring() : L"; slow - " + slow;
    const std::wstring before = WindowText(m.before, programs), end = WindowText(m.end, programs), dock = DockMonitorText(monitor);
    std::wstring seenBy, came;
    bool cameOnDock = false, dockTold = false;   // came names the dock's monitor as Start's; ...names the dock's at all
    if (o.Opened()) {
        seenBy = m.seenBy == kByVisibility ? L"; seen by IAppVisibility" : m.seenBy == kByLauncher ? L"; seen by the launcher"
               : m.seenBy == kByFront ? L"; seen by the window in front (Start's own)" : L"; not seen: the start button closed it meanwhile";
        // where Start came: the monitor of the window in front at the end - only when that window is Start's or its search
        // box's (IAppVisibility or the launcher may tell Start up while another program's window is still in front)
        if (m.seenBy && m.end.window) {
            if (!StartsProgram(programs.Of(m.end))) came = L"; the window in front at the end is not Start's";
            else if (const HMONITOR on = MonitorFromWindow(m.end.window, MONITOR_DEFAULTTONEAREST); on == monitor) {
                came = L"; came on the dock's monitor " + dock;
                cameOnDock = dockTold = true;
            } else {
                came = L"; came on monitor " + MonitorText(on) + L", not the dock's " + dock;
                dockTold = true;
            }
        }
    }
    // the usual line's tail (every usual click: the last one before a miss tells what was usual)
    a.tail = L" [" + right + L"; Explorer allowed to come in front: " + allowed + took + L"; in front: " + before + L" -> " + end +
             (m.start.window ? L"; Start's window cloak " + CloakText(m.startBefore) + L" -> " + CloakText(m.startEnd) : std::wstring(L"; Start's window not found")) +
             seenBy + came + (dockTold ? std::wstring() : L"; the dock's monitor " + dock) + slowly + L"]";
    // the line after a click that went otherwise
    std::wstring& l = a.line;
    l = right + L"; Explorer";
    if (m.click.explorer) { _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L" (pid %lu)", m.click.explorer); l += b; }
    l += L" allowed to come in front: " + allowed;
    if (m.inputTaken) {
        const int32_t after = static_cast<int32_t>(m.input - static_cast<DWORD>(m.click.tick));   // GetLastInputInfo's tick is GetTickCount's (wraps)
        if (!m.inputRead) wcscpy_s(b, L"? (not to be read)");
        else if (after > 0) _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"yes, %.2f s after the click", after / 1000.0);
        else wcscpy_s(b, L"no");
        l += std::wstring(L"; input after the click before the first way went out: ") + b;
    }
    l += L"; in front before: " + before;
    for (int i = 0; i < m.nSent; ++i) {   // each way that went out, in order: when, and the window in front right after it
        _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"; right after %ls (%.1f s): ", SentText(m.sent[i].way), reach + m.sent[i].at);
        l += b + WindowText(m.sent[i].front, programs);
    }
    l += L"; at the end: " + end;
    if (m.start.window) {
        _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"; Start's window %ls (StartMenuExperienceHost.exe pid %lu", Hex8(m.start.window).c_str(), m.start.pid);
        l += b;
        FILETIME now{};
        if (m.start.started) GetSystemTimeAsFileTime(&now);
        const uint64_t n = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        if (m.start.started && n >= m.start.started) { _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L", up %.0f s", (n - m.start.started) / 1e7); l += b; }
        l += L") cloak " + CloakText(m.startBefore);   // at the click, right after each way but the first, at the end
        for (int i = 1; i < m.nSent; ++i) l += L" -> " + CloakText(m.sent[i].startCloak);
        l += L" -> " + CloakText(m.startEnd);
    } else {
        l += L"; Start's window not found";
    }
    l += seenBy + came + took;
    if (!cameOnDock) l += L"; the dock's monitor " + dock;
    const double on = g_displayOnAt.load();
    if (on >= 0 && endAt >= on) { _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"; the display came on %.0f s ago", endAt - on); l += b; }
    else l += L"; the display not seen going off in this run";
    // the more clicks' times
    if (m.agains) {
        a.clicks = L" (at ";
        for (int i = 0; i < m.agains && i < Notes::kTimes; ++i) {
            _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"%ls%.1f", i ? L", " : L"", m.againAt[i]);
            a.clicks += b;
        }
        a.clicks += m.agains > Notes::kTimes ? L", ... s)" : L" s)";
    }
    return a;
}

// The dock.log lines of a click. reach: seconds it took to be ready to send - to reach the shell (before the
// click's timeline starts). g_mu held. Start by the shell's request, soon and with nothing else going on: the usual
// line, every time, with the tail (a); any other click: the long line that tells of each way, and the line after it.
// keyOff: the tests took the Windows key away (g_ways). slow: why the click was slow (SlowText; empty: it was not).
void Report(const Opening& o, HRESULT made, HRESULT connected, HRESULT lastShell, const Front& front, double reach, const TaskbarNote& tb, bool keyOff,
            const std::wstring& slow, const Around& a) {
    if (o.Opened() && o.First() == Way::None) return;   // already up when the wait began (the user's own key): nothing went out
    if (o.Opened() && o.CameAfter() == Way::Shell && o.At() < kUsual && !o.Clicks() && reach < kUsual) {
        Log(L"start: Start came by %ls in %.0f ms%ls%ls", WayText(o.CameAfter()), (reach + o.At()) * 1000,
            FAILED(connected) ? L" (not on the dock's monitor: Windows chose)" : L"", a.tail.c_str());
        return;
    }
    // truncated, never too long for the buffer: a too long text would end the process (_snwprintf_s, _TRUNCATE)
    wchar_t taskbar[352], shell[96], key[192], state[96];
    StateNote(tb, state, ARRAYSIZE(state));
    if (tb.skipped) wcscpy_s(taskbar, tb.skipped);
    else if (o.TaskbarSent()) {
        if (!o.TaskbarOk()) _snwprintf_s(taskbar, ARRAYSIZE(taskbar), _TRUNCATE, L"the press did not go through: %ls (%08lx)", tb.how, (unsigned long)tb.hr);
        else if (o.TaskbarUncertain())
            _snwprintf_s(taskbar, ARRAYSIZE(taskbar), _TRUNCATE, L"pressed (%ls), unsure whether it arrived (%08lx): waited on, nothing else sent", tb.how, (unsigned long)tb.hr);
        else _snwprintf_s(taskbar, ARRAYSIZE(taskbar), _TRUNCATE, L"pressed (%ls%ls; the button %ls)%ls", tb.how, state, taskbarstart::FoundText(tb.found),
                          o.WaitsAlone() ? L", slow: waited on alone" : L"");
    } else if (!o.TaskbarTurn()) {   // its turn did not come: Start came first, or the request the shell took is waited on alone
        wcscpy_s(taskbar, o.WaitsAlone() ? L"not pressed (slow: the shell's request is waited on alone)" : L"not pressed");
    } else if (tb.held) {            // found, and left alone: only a button that reads off is pressed
        wchar_t why[160];
        if (tb.before == 1) wcscpy_s(why, L"its toggle state was on - Explorer holds Start open, and a press would close it");
        else if (!wcscmp(tb.how, L"toggle")) _snwprintf_s(why, ARRAYSIZE(why), _TRUNCATE, L"its toggle state was not to be read (%ls)", ToggleText(tb.before));
        else _snwprintf_s(why, ARRAYSIZE(why), _TRUNCATE, L"%ls", tb.how);
        _snwprintf_s(taskbar, ARRAYSIZE(taskbar), _TRUNCATE, L"left alone at %.1f s: %ls (the button %ls)", reach + tb.turnAt, why, taskbarstart::FoundText(tb.found));
    } else {
        _snwprintf_s(taskbar, ARRAYSIZE(taskbar), _TRUNCATE, L"none to press (%ls)", taskbarstart::FoundText(tb.found));
    }
    if (!o.ShellReady()) _snwprintf_s(shell, ARRAYSIZE(shell), _TRUNCATE, L"not available (%08lx)", (unsigned long)made);
    else if (!o.ShellSent()) wcscpy_s(shell, L"not asked");
    else _snwprintf_s(shell, ARRAYSIZE(shell), _TRUNCATE, L"asked 1 time, %ls (%08lx)", o.ShellAccepted() ? L"accepted" : L"refused", (unsigned long)lastShell);
    if (o.KeySent()) wcscpy_s(key, o.KeyOk() ? L"pressed" : L"not inserted (input blocked)");
    else if (keyOff) wcscpy_s(key, L"off");
    else if (o.KeyBlocked()) _snwprintf_s(key, ARRAYSIZE(key), _TRUNCATE, L"not pressed: %ls runs as administrator and Windows drops keys sent to it", front.exe.c_str());
    else if (o.WaitsAlone() && o.TaskbarSent() && o.TaskbarOk()) wcscpy_s(key, L"not pressed (the taskbar's button took the press; the key would close a Start on its way)");
    else if (o.ShellAccepted()) wcscpy_s(key, L"not pressed (the shell took the request; the key would close a Start on its way)");
    else wcscpy_s(key, L"not pressed");
    std::wstring more;   // the line's end: how long getting ready took, why the click was slow
    if (reach >= 0.3) {
        wchar_t took[64];
        _snwprintf_s(took, ARRAYSIZE(took), _TRUNCATE, L"; getting ready to send took %.1f s", reach);
        more = took;
    }
    if (!slow.empty()) more += L"; slow - " + slow;
    if (o.Opened())
        Log(L"start: Start came after %.1f s, by %ls - taskbar: %ls; shell: %ls; Windows key: %ls; %d more click%ls while waiting%ls%ls",
            reach + o.At(), WayText(o.CameAfter()), taskbar, shell, key, o.Clicks(), o.Clicks() == 1 ? L"" : L"s", a.clicks.c_str(), more.c_str());
    else
        Log(L"start: Start did not come in %.1f s - taskbar: %ls; shell: %ls; Windows key: %ls; in front: %.80ls%ls; %d more click%ls%ls; Windows up %.0f s%ls",
            reach + o.At(), taskbar, shell, key, front.exe.empty() ? L"nothing" : front.exe.c_str(), front.elevated ? L" (administrator)" : L"",
            o.Clicks(), o.Clicks() == 1 ? L"" : L"s", a.clicks.c_str(), GetTickCount64() / 1000.0, more.c_str());
    Log(L"start: around that click - %ls", a.line.c_str());
}

// A click's worker (Run), in three steps on its thread: Ready - the first reads, the shell reached and IAppVisibility
// made, before the click's timeline starts (t0); Wait - the looks and the ways that go out; End - the end's notes and
// the lines, under g_mu, taken there once. What the steps share is kept here, each value read once by the step that
// reads it (clicked, t0 and reach too: no later step reads the clock for them again). Run makes it inside its apartment;
// its COM pointers (launcher, vis) go with it, at Run's inner brace - before Run leaves the apartment (CoUninitialize).
struct Worker {
    const HMONITOR monitor;
    const uint32_t id;
    const double clicked;   // NowSeconds at the click, as Run read it
    Notes m;   // for the log only (the notes above): taken after t0, but for the window in front at the click
    Front front;
    bool keyOff = false;
    TaskbarNote tb;
    SlowNote slow;
    ComPtr<ILauncher> launcher;
    ComPtr<IVisibility> vis;
    HRESULT connected = E_FAIL, made = S_OK;
    bool leftBehind = false;
    double t0 = 0, reach = 0;
    Opening o{ false, false };   // (made in Ready, once the shell is reached)
    HRESULT lastShell = S_OK;
    FrontSeen seen;

    void Ready(ClickNote click);
    void Wait();
    void End();
    // (lower case: the steps' helpers, called from Run's steps by these names)
    bool busy(const wchar_t* call, double took);
    void beforeSend();
    void afterSend(Way w, double at);
};

void Worker::Ready(ClickNote click) {
    m.click = click;
    front = InFrontNow(true, &m.frontWindow);
    m.frontExe = front.exe;
    const int ways = g_ways.load();
    keyOff = !(ways & kWayKey);
    const bool keyBlocked = front.elevated && !SelfElevated();
    // Only a running shell is touched: with Explorer gone, COM could start a new explorer.exe to answer.
    const HWND shellWindow = GetShellWindow();
    const bool explorerUp = shellWindow != nullptr;
    // The taskbar's own Start button (taskbar_start.h) is looked for only when its turn comes, behind the shell.
    tb.skipped = !(ways & kWayTaskbar) ? L"off" : !explorerUp ? L"Explorer is not up" : nullptr;
    // Slow from the start: Windows or Explorer started minutes ago (kernel calls only: nothing is sent to Explorer)
    slow.up = GetTickCount64() / 1000.0;
    slow.explorer = ExplorerAge(shellWindow);
    slow.early = EarlyAfterStart(slow.up, slow.explorer);
    // The shell first: reached before the click's timeline starts (reaching a busy Explorer can take seconds).
    made = !(ways & kWayShell) ? E_ABORT : !explorerUp ? HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE) : S_OK;
    const double r0 = NowSeconds();
    if (SUCCEEDED(made)) made = MakeLauncher(monitor, id, launcher, connected);
    // A later click started over while this worker hung on Explorer (kStuck) - reaching the shell, making IAppVisibility
    // or in the loop below: that click's worker sends now; this one sends nothing more - a second request,
    // press or key would stop the Start that one brings, or bring back one closed meanwhile - and touches Explorer no more.
    leftBehind = g_current.load() != id;
    const double v0 = NowSeconds();   // (reaching the shell: r0 to here)
    if (!leftBehind) CoCreateInstance(kAppVisibility, nullptr, CLSCTX_ALL, kAppVisibilityIid, reinterpret_cast<void**>(vis.GetAddressOf()));
    // The timeline starts here: reaching a busy Explorer after logon can take seconds, and the wait for Start
    // must not be used up before anything was sent. (Making IAppVisibility - one call into Explorer: v0 to here.)
    t0 = NowSeconds();
    reach = t0 - clicked;
    o = Opening(SUCCEEDED(made) && launcher.Get() != nullptr, keyBlocked || keyOff);
    if (slow.early) o.Slow();
    busy(L"reaching the shell", v0 - r0);
    busy(L"making IAppVisibility", t0 - v0);
    // The log's notes (none for a worker already left behind): the window in front at the click, Start's own window -
    // user32 and DWM only, a few microseconds; Start's window looked up through its processes once a run (StartWindowNow).
    if (!leftBehind) {
        m.before = NoteOf(m.frontWindow);
        m.start = StartWindowNow();
        m.startBefore = CloakOfWindow(m.start.window);
    }
}

// a single call into Explorer that took kBusyCall: it is busy, Start will be slow too (the first such call is noted)
bool Worker::busy(const wchar_t* call, double took) {
    if (took < kBusyCall) return false;
    if (!slow.call) { slow.call = call; slow.took = took; }
    o.Slow();
    return true;
}

// just before a way goes out while none has: the user's input since the click (one user32 call); right after a
// way went out, once its time was taken: the window in front (and, but for the first, Start's cloak)
void Worker::beforeSend() {
    if (m.nSent) return;
    m.inputTaken = true;
    LASTINPUTINFO li = { sizeof(li) };
    m.inputRead = GetLastInputInfo(&li) != FALSE;
    m.input = li.dwTime;
}
void Worker::afterSend(Way w, double at) {
    if (m.nSent >= static_cast<int>(ARRAYSIZE(m.sent))) return;
    SentNote& s = m.sent[m.nSent];
    s.way = w;
    s.at = at;
    s.front = NoteOf(GetForegroundWindow());
    if (m.nSent) s.startCloak = CloakOfWindow(m.start.window);
    ++m.nSent;
}

void Worker::Wait() {
    bool looked = false;   // the taskbar's button was looked for (at its turn: a kept one costs no call into Explorer)
    while (!leftBehind && !o.Done()) {
        if (g_current.load() != id) { leftBehind = true; break; }
        const double t = NowSeconds() - t0;
        if (g_closed.exchange(false)) { o.Closed(t); break; }
        if (g_again.exchange(false)) {
            o.Again(t);
            if (m.agains < Notes::kTimes) m.againAt[m.agains] = reach + t;
            ++m.agains;
        }
        int by = 0;
        // (the launcher is asked whether Start is up only by a click that has no IAppVisibility)
        const bool visible = Visible(vis.Get(), vis.Get() ? nullptr : launcher.Get(), &seen, &by);
        if (visible) m.seenBy = by;
        if (g_current.load() != id) { leftBehind = true; break; }   // that call may have hung as well
        const Way way = o.Next(t, visible);
        if (way == Way::Shell) {
            beforeSend();
            const double s0 = NowSeconds();
            lastShell = launcher ? launcher->ShowStartView(kShowFromStartButton, kIgnoreForegroundError) : E_POINTER;
            const double at = NowSeconds() - t0;
            o.Sent(way, at, SUCCEEDED(lastShell));   // when the shell answered: it can take a while
            m.requestTook = at - (s0 - t0);
            busy(L"the shell's request", m.requestTook);
            afterSend(way, at);
        } else if (way == Way::Taskbar) {
            // Its turn: kConfirm after a request the shell took, at once behind one it did not take. Pressing it goes to
            // Explorer as a click would: no injected key, so an administrator's window in front does not stop it.
            if (!looked) {
                looked = true;
                if (!tb.skipped) {
                    const double f0 = NowSeconds();
                    tb.found = taskbarstart::Find(monitor);
                    // the lookup itself was slow: a request the shell took is waited on alone after all (Next says)
                    if (busy(L"finding the taskbar's button", NowSeconds() - f0)) continue;
                }
            }
            if (tb.skipped || (tb.found != taskbarstart::Found::Kept && tb.found != taskbarstart::Found::Now)) {
                tb.turnAt = t;
                o.NoPress(t);   // none to press
                continue;
            }
            beforeSend();
            // pressed only when its state, read right before, is off: on, Explorer holds Start open and a press would close it
            const taskbarstart::Pressed p = taskbarstart::Press(monitor, true);
            tb.hr = p.hr;
            tb.how = p.how;
            tb.before = p.before;
            tb.after = p.after;
            tb.took = p.took;
            const double at = NowSeconds() - t0;
            tb.turnAt = at;
            if (p.held) {   // nothing was pressed
                tb.held = true;
                busy(L"reading the taskbar button's state", p.took);
                o.NoPress(at);
                continue;
            }
            o.Sent(way, at, p.ok || p.unsure);
            if (p.unsure) o.Uncertain();                         // it may still arrive: nothing else may follow it
            else busy(L"pressing the taskbar's button", p.took);   // the press itself was slow: Explorer is busy
            afterSend(way, at);
        } else if (way == Way::Key) {
            beforeSend();
            const bool inserted = SendKeyCombo({ VK_LWIN });
            const double at = NowSeconds() - t0;
            o.Sent(way, at, inserted);
            afterSend(way, at);
        } else if (!o.Done()) {
            Sleep(static_cast<DWORD>(kLook * 1000));
        }
    }
}

void Worker::End() {
    // the end's notes: the window in front, Start's cloak - and the lines' pieces, where Start came among them (a process
    // looked into only for a window neither this click nor an earlier one named), all before g_mu
    Around around;
    std::wstring slowly;
    if (!leftBehind && !(o.Opened() && o.First() == Way::None)) {   // (Start up at the first look: no line, nothing read)
        m.end = NoteOf(GetForegroundWindow());
        if (!m.start.window) m.start = StartWindowNow();   // Start's process may have come with the click
        m.startEnd = CloakOfWindow(m.start.window);
        if (o.IsSlow()) slowly = SlowText(slow);
        around = Describe(m, o, seen, monitor, reach, t0 + o.At(), tb, slowly);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (leftBehind) {   // an opening another click took over: no telling what did what
        Log(L"start: the click left behind on a hung Explorer got an answer again after %.1f s: nothing more sent, nothing learned "
            L"(a later click started over; sent before: the taskbar's button %d, the shell %d, the Windows key %d)",
            NowSeconds() - clicked, o.TaskbarSent() ? 1 : 0, o.ShellSent() ? 1 : 0, o.KeySent() ? 1 : 0);
    } else {
        Report(o, made, connected, lastShell, front, reach, tb, keyOff, slowly, around);
    }
}

void Run(HMONITOR monitor, uint32_t id, ClickNote click) {
    const double clicked = NowSeconds();
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Worker w{ monitor, id, clicked };   // gone at this brace, its COM pointers with it: before CoUninitialize
        w.Ready(click);
        w.Wait();
        w.End();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    uint32_t mine = id;
    g_current.compare_exchange_strong(mine, 0);   // unless a later click has started over
}

// Start's window belongs to the shell; it may come in front - and take the keyboard, so typing searches -
// only if the dock lets it. The dock needs that right itself first: a click on its own window does not
// always give it (shell_front.h). What each step answered goes to the worker's lines (ClickNote; no call more).
ClickNote AllowShellForeground() {
    ClickNote n;
    n.right = TakeForegroundRight();
    DWORD pid = 0;
    if (HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &pid);
    n.explorer = pid;
    if (pid) {
        n.allowed = AllowSetForegroundWindow(pid) != FALSE;
        if (!n.allowed) n.error = GetLastError();
    }
    return n;
}

}   // namespace

void DisplayCameOn(double now) { g_displayOnAt = now; }

// Start is up: IAppVisibility says so - the documented answer; on Windows 11 25H2 the window in front
// while Start is open can be SearchHost.exe's (Start's search box), not StartMenuExperienceHost.exe's -
// or Start's own window is in front. Asked on the UI thread at a press, as RetroBar asks it every 0.1 s;
// the object is made there once and kept for the life of the process.
bool InFront() {
    static IVisibility* const vis = [] {
        IVisibility* v = nullptr;
        if (FAILED(CoCreateInstance(kAppVisibility, nullptr, CLSCTX_ALL, kAppVisibilityIid, reinterpret_cast<void**>(&v)))) v = nullptr;
        return v;
    }();
    BOOL shown = FALSE;
    if (vis && SUCCEEDED(vis->IsLauncherVisible(&shown)) && shown) return true;
    return InFrontNow().start;
}

void Click(HMONITOR monitor, bool startWasInFront) {
    const uint32_t running = g_current.load();
    const double age = (GetTickCount64() - g_since.load()) / 1000.0;
    switch (OnPress(startWasInFront, StillOpening(running != 0, age))) {
    case Press::Close: {   // Start is in front: the Windows key closes it
        // (Never the taskbar's button: Start closed by it, the shell's request half a second later was taken and brought
        // no Start for 8 s - a test PC, 2026-10-27 14:17. Closed by the key, the next request brings it.)
        if (running) g_closed = true;   // a wait still running must not bring it back
        SendKeyCombo({ VK_LWIN });
        // once per close, which no other line tells: a closed Start's window
        // may keep the front after it (seen 2026-11-17: app clicks right after Start refused - apps_click.cpp's
        // 'is in front but not shown' line then follows this one)
        const Front f = InFrontNow();
        Log(L"start: closing Start (the start button pressed while it was open; in front: %ls) by the Windows key", f.exe.empty() ? L"nothing" : f.exe.c_str());
        return;
    }
    case Press::Again:
        g_again = true;
        return;
    case Press::Open:
        break;
    }
    if (running) Log(L"start: the last click's request has waited on Explorer for %.0f s: starting over", age);
    ClickNote note = AllowShellForeground();
    if (++g_lastId == 0) ++g_lastId;   // never 0
    const uint32_t id = g_lastId;
    g_again = false;
    g_closed = false;
    g_since = GetTickCount64();
    note.tick = g_since.load();
    g_current = id;
    try {
        std::thread(Run, monitor, id, note).detach();
    } catch (...) {   // no thread to be had: the Windows key, as before
        g_current = 0;
        SendKeyCombo({ VK_LWIN });
    }
}

}   // namespace startmenu
