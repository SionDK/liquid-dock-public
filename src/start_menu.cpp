#include "start_menu.h"
#include "start_menu_logic.h"
#include "system.h"
#include "taskbar_start.h"
#include <objbase.h>
#include <servprov.h>
#include <atomic>
#include <mutex>
#include <thread>

// The start button (start_menu_logic.h says why it no longer only presses the Windows key, and when it
// presses the taskbar's own Start button: taskbar_start.h).
//
// The shell's launcher: Explorer's immersive shell service. Undocumented; the ids and the order of the
// methods are the ones RetroBar (StartMenuMonitor.cs, 2026-10) and ExplorerPatcher (StartMenu.h,
// TwinUIPatches.cpp, 2026-07) use on Windows 10 1607 and later, Windows 11 included. Each table is
// declared only up to the last method called. Should a Windows release change them, QueryService fails
// (the interface id names the table) and the button presses the Windows key as before.

namespace startmenu {
namespace {

const CLSID kImmersiveShell = { 0xc2f03a33, 0x21f5, 0x47fa, { 0xb4, 0xbb, 0x15, 0x63, 0x62, 0xa2, 0xf2, 0x39 } };
const GUID kLauncherService = { 0x6f86e01c, 0xc649, 0x4d61, { 0xbe, 0x23, 0xf1, 0x32, 0x2d, 0xde, 0xca, 0x9d } };
const IID kLauncherIid = { 0xd8d60399, 0xa0f1, 0xf987, { 0x55, 0x51, 0x32, 0x1f, 0xd1, 0xb4, 0x98, 0x64 } };
const GUID kMonitorService = { 0x47094e3a, 0x0cf2, 0x430f, { 0x80, 0x6f, 0xcf, 0x9e, 0x4f, 0x0f, 0x12, 0xdd } };
const IID kMonitorServiceIid = { 0x4d4c1e64, 0xe410, 0x4faa, { 0xba, 0xfa, 0x59, 0xca, 0x06, 0x9b, 0xfe, 0xc2 } };
// IAppVisibility (shobjidl_core.h, documented since Windows 8), declared here so every compiler sees the same
const CLSID kAppVisibility = { 0x7e5fe3d9, 0x985f, 0x4908, { 0x91, 0xf9, 0xee, 0x19, 0xf9, 0xfd, 0x15, 0x14 } };
const IID kAppVisibilityIid = { 0x2246ea2d, 0xcaea, 0x4444, { 0xa3, 0xc4, 0x6d, 0xe8, 0x27, 0xe4, 0x43, 0x13 } };

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

struct IVisibility : IUnknown {       // IAppVisibility
    virtual HRESULT STDMETHODCALLTYPE GetAppVisibilityOnMonitor(HMONITOR, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLauncherVisible(BOOL* visible) = 0;
};

constexpr int kShowFromStartButton = 11;      // ILSM_STARTBUTTON
constexpr int kIgnoreForegroundError = 4;     // ILSF_IGNORE_SET_FOREGROUND_ERROR (as RetroBar)

// The window in front: which program, and whether it runs as administrator.
struct Front {
    std::wstring exe;      // file name; empty: nothing in front, "?" : a program the dock may not look into
    bool elevated = false; // asked only where it counts (ProgramOf)
    bool start = false;    // it is Start
};

// The program of process pid. elevation: whether it runs as administrator too - two more calls into Windows,
// made only at a click's first look (can the Windows key reach Start; for the log).
Front ProgramOf(DWORD pid, bool elevation) {
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
    CloseHandle(p);
    f.start = !_wcsicmp(f.exe.c_str(), L"StartMenuExperienceHost.exe");
    return f;
}

// The program of the window in front (elevation: as ProgramOf).
Front InFrontNow(bool elevation = false) {
    HWND fg = GetForegroundWindow();
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
};

// Start's own window is in front (InFrontNow().start); seen: the last look's, brought up to date.
bool StartInFront(FrontSeen& seen) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid) return false;
    if (fg == seen.window && pid == seen.pid) return seen.start;
    const Front f = ProgramOf(pid, false);
    if (!f.exe.empty() && f.exe != L"?") seen = { fg, pid, f.start };
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
// takes further clicks, sends, learns and clears the number when done.
std::atomic<uint32_t> g_current{ 0 };
std::atomic<uint64_t> g_since{ 0 };
std::atomic<bool> g_again{ false };    // clicked again meanwhile
std::atomic<bool> g_closed{ false };   // the button closed Start meanwhile: send nothing more
uint32_t g_lastId = 0;                 // UI thread
std::mutex g_mu;                       // g_pref, g_reportedFirst, g_verdictLoaded (a left-behind worker may still finish)
Preference g_pref;
bool g_reportedFirst = false;

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

// Start is up: any of the three ways to tell (one of them may say nothing on a given Windows release).
// seen: a wait's (StartInFront), nullptr for a single look.
bool Visible(IVisibility* vis, ILauncher* launcher, FrontSeen* seen = nullptr) {
    BOOL v = FALSE;
    if (vis && SUCCEEDED(vis->IsLauncherVisible(&v)) && v) return true;
    v = FALSE;
    if (launcher && SUCCEEDED(launcher->IsVisible(&v)) && v) return true;
    FrontSeen once;
    return StartInFront(seen ? *seen : once);
}

// The taskbar button's verdict across runs (start_menu_logic.h), one line in data\taskbar-start.txt next to the
// settings. g_keepVerdict false: neither read nor written (the worker test leaves the dock's data alone).
bool g_keepVerdict = true;
wchar_t g_verdictFile[MAX_PATH * 2] = L"";   // set at the first click (plain storage: workers may outlive statics)
bool g_verdictLoaded = false;

std::wstring WindowsBuild() {   // "26200.6899": a Windows update is checked afresh
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = { sizeof(v) };
    if (auto f = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) f(&v);
    DWORD ubr = 0, size = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
    return std::to_wstring(v.dwBuildNumber) + L"." + std::to_wstring(ubr);
}

long Today() {   // days since 1601 (UTC)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long>(u.QuadPart / 864000000000ULL);
}

// Once, at the first click. g_mu held.
void LoadVerdict() {
    if (g_verdictLoaded) return;
    g_verdictLoaded = true;
    if (g_keepVerdict && !g_verdictFile[0]) _snwprintf_s(g_verdictFile, ARRAYSIZE(g_verdictFile), _TRUNCATE, L"%ls\\taskbar-start.txt", AppDataDir().c_str());
    if (!g_verdictFile[0] || g_pref.taskbar != TaskbarState::Unknown) return;
    HANDLE f = CreateFileW(g_verdictFile, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;   // nothing kept: the button is checked
    char line[96] = {};
    DWORD got = 0;
    const BOOL read = ReadFile(f, line, sizeof(line) - 1, &got, nullptr);
    CloseHandle(f);
    if (!read) return;
    std::wstring text;
    for (DWORD i = 0; i < got && line[i] != '\r' && line[i] != '\n'; ++i) text += static_cast<wchar_t>(static_cast<unsigned char>(line[i]));
    long failedDay = -1;
    const TaskbarState s = ParseVerdict(text, WindowsBuild(), Today(), &failedDay);
    if (s == TaskbarState::Unknown) return;
    g_pref.taskbar = s;
    g_pref.taskbarFailedDay = failedDay;   // a failure is tried again kVerdictDays after it was kept, within the run too
    Log(L"start: the taskbar's Start button %ls on this Windows build before (%.60ls): %ls", s == TaskbarState::Works ? L"worked" : L"did not work",
        text.c_str(), s == TaskbarState::Works ? L"pressed and trusted from the first click" : L"the start button asks the shell");
}

void SaveVerdict(TaskbarState s) {   // g_mu held
    if (!g_verdictFile[0]) return;
    const std::wstring v = FormatVerdict(s, WindowsBuild(), Today());
    if (v.empty()) { DeleteFileW(g_verdictFile); return; }
    std::string line;   // digits, dots, letters: plain ASCII
    for (wchar_t c : v) line += static_cast<char>(c);
    HANDLE f = CreateFileW(g_verdictFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &put, nullptr);
    CloseHandle(f);
}

// Seconds since Explorer - the process of the shell's window - started; -1: no telling. Kernel calls only: nothing
// is sent to Explorer, so a hung one does not hold this up.
double ExplorerAge() {
    DWORD pid = 0;
    const HWND shell = GetShellWindow();
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
    const wchar_t* skipped = L"";                            // why it was not looked for; nullptr: it was
    taskbarstart::Found found = taskbarstart::Found::NoButton;
    HRESULT hr = S_OK;                                       // the press
    const wchar_t* how = L"";
};

// One dock.log line for a click that went otherwise than usual. reach: seconds it took to be ready to
// send - to find the taskbar's button, or to reach the shell (before the click's timeline starts). g_mu held.
void Report(const Opening& o, HRESULT made, HRESULT connected, HRESULT lastShell, const Front& front, double reach, const TaskbarNote& tb) {
    if (o.Opened() && o.CameAfter() == Way::None) return;   // already up when the wait began (the user's own key)
    if (o.Opened() && o.CameAfter() == o.First() && o.At() < kShellAfterKey && !o.Clicks() && reach < kShellAfterKey) {
        if (!g_reportedFirst) {   // the usual click: once per run, so the log shows which way works here
            wchar_t note[160] = L"";
            if (o.CameAfter() == Way::Taskbar)
                _snwprintf_s(note, ARRAYSIZE(note), _TRUNCATE, L" (%ls; the button %ls)", tb.how, taskbarstart::FoundText(tb.found));
            else if (o.CameAfter() == Way::Shell && FAILED(connected))
                wcscpy_s(note, L" (not on the dock's monitor: Windows chose)");
            Log(L"start: Start came by %ls in %.0f ms%ls", WayText(o.CameAfter()), (reach + o.At()) * 1000, note);
        }
        g_reportedFirst = true;
        return;
    }
    // truncated, never too long for the buffer: a too long text would end the process (_snwprintf_s, _TRUNCATE)
    wchar_t taskbar[192], shell[96], key[192], took[64] = L"";
    if (tb.skipped) wcscpy_s(taskbar, tb.skipped);
    else if (o.Taskbar() == TaskbarUse::None) _snwprintf_s(taskbar, ARRAYSIZE(taskbar), _TRUNCATE, L"none to press (%ls)", taskbarstart::FoundText(tb.found));
    else if (!o.TaskbarSent()) wcscpy_s(taskbar, L"not pressed");
    else if (!o.TaskbarOk()) _snwprintf_s(taskbar, ARRAYSIZE(taskbar), _TRUNCATE, L"the press did not go through: %ls (%08lx)", tb.how, (unsigned long)tb.hr);
    else if (o.TaskbarUncertain())
        _snwprintf_s(taskbar, ARRAYSIZE(taskbar), _TRUNCATE, L"pressed (%ls), unsure whether it arrived (%08lx): waited on, nothing else sent", tb.how, (unsigned long)tb.hr);
    else _snwprintf_s(taskbar, ARRAYSIZE(taskbar), _TRUNCATE, L"pressed (%ls; the button %ls)%ls", tb.how, taskbarstart::FoundText(tb.found),
                      o.Taskbar() == TaskbarUse::Proven ? L", trusted" : o.ExplorerBusy() ? L", Explorer busy: waited on like a trusted press" : L", checked");
    if (!o.ShellReady()) _snwprintf_s(shell, ARRAYSIZE(shell), _TRUNCATE, L"not available (%08lx)", (unsigned long)made);
    else if (!o.ShellSent()) wcscpy_s(shell, L"not asked");
    else _snwprintf_s(shell, ARRAYSIZE(shell), _TRUNCATE, L"asked %d time%ls, %ls (%08lx)", o.ShellSent(), o.ShellSent() == 1 ? L"" : L"s",
                      o.ShellAccepted() ? L"accepted" : L"refused", (unsigned long)lastShell);
    if (o.KeySent()) wcscpy_s(key, o.KeyOk() ? L"pressed" : L"not inserted (input blocked)");
    else if (o.KeyBlocked()) _snwprintf_s(key, ARRAYSIZE(key), _TRUNCATE, L"not pressed: %ls runs as administrator and Windows drops keys sent to it", front.exe.c_str());
    else if (o.ShellAccepted()) wcscpy_s(key, L"not pressed (the shell took the request; the key would close a Start on its way)");
    else if (o.TaskbarOk() && o.TaskbarTrusted()) wcscpy_s(key, L"not pressed (the taskbar's button took the press; the key would close a Start on its way)");
    else wcscpy_s(key, L"not pressed");
    if (reach >= 0.3) _snwprintf_s(took, ARRAYSIZE(took), _TRUNCATE, L"; getting ready to send took %.1f s", reach);
    if (o.Opened())
        Log(L"start: Start came after %.1f s, by %ls - taskbar: %ls; shell: %ls; Windows key: %ls; %d more click%ls while waiting%ls",
            reach + o.At(), WayText(o.CameAfter()), taskbar, shell, key, o.Clicks(), o.Clicks() == 1 ? L"" : L"s", took);
    else
        Log(L"start: Start did not come in %.1f s - taskbar: %ls; shell: %ls; Windows key: %ls; in front: %.80ls%ls; %d more click%ls; Windows up %.0f s%ls",
            reach + o.At(), taskbar, shell, key, front.exe.empty() ? L"nothing" : front.exe.c_str(), front.elevated ? L" (administrator)" : L"",
            o.Clicks(), o.Clicks() == 1 ? L"" : L"s", GetTickCount64() / 1000.0, took);
}

void Run(HMONITOR monitor, uint32_t id) {
    const double clicked = NowSeconds();
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Preference pref;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            const long today = Today(), failedDay = g_pref.taskbarFailedDay;
            if (RetryTaskbar(g_pref, today)) {   // the dock ran on since the failure was kept (sleep, not a restart)
                wchar_t when[64] = L"on a later day than today (the clock went back)";
                if (today >= failedDay) _snwprintf_s(when, ARRAYSIZE(when), _TRUNCATE, L"%ld days ago", today - failedDay);
                Log(L"start: the taskbar's Start button was left alone %ls (it did not bring Start): it is pressed and checked again, "
                    L"as the next run would", when);
            }
            pref = g_pref;
        }
        const Front front = InFrontNow(true);
        const bool keyBlocked = front.elevated && !SelfElevated();
        // Only a running shell is touched: with Explorer gone, COM could start a new explorer.exe to answer.
        const bool explorerUp = GetShellWindow() != nullptr;
        // The taskbar's own Start button first (taskbar_start.h), unless it did not work here (the kept
        // verdict). Pressing it goes to Explorer as a click would: no injected key, so an administrator's
        // window in front does not stop it.
        TaskbarNote tb;
        tb.skipped = pref.taskbar == TaskbarState::Off ? L"off"
                   : pref.taskbar == TaskbarState::Failed ? L"left alone: it did not bring Start before"
                   : !explorerUp ? L"Explorer is not up" : nullptr;
        if (!tb.skipped) tb.found = taskbarstart::Find(monitor);
        const TaskbarUse use = TaskbarUseFor(pref, !tb.skipped && (tb.found == taskbarstart::Found::Kept || tb.found == taskbarstart::Found::Now));
        ComPtr<ILauncher> launcher;
        ComPtr<IVisibility> vis;
        // With the taskbar's button first, the shell is reached only when it is needed (E_PENDING till then):
        // reaching a busy Explorer can take seconds, which must not hold up the press.
        HRESULT connected = E_FAIL, made = !pref.askShell ? E_ABORT : !explorerUp ? HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE) : E_PENDING;
        if (made == E_PENDING && use == TaskbarUse::None) made = MakeLauncher(monitor, id, launcher, connected);
        // A later click started over while this worker hung on Explorer (kStuck) - reaching the shell, making IAppVisibility
        // or in the loop below: that click's worker sends and learns now; this one sends nothing more - a second request,
        // press or key would stop the Start that one brings, or bring back one closed meanwhile - and touches Explorer no more.
        bool leftBehind = g_current.load() != id;
        const double v0 = NowSeconds();
        if (!leftBehind) CoCreateInstance(kAppVisibility, nullptr, CLSCTX_ALL, kAppVisibilityIid, reinterpret_cast<void**>(vis.GetAddressOf()));
        const double visTook = NowSeconds() - v0;   // one call into Explorer: slow when it is busy
        Opening o(pref.shellFirst, made == E_PENDING || (SUCCEEDED(made) && launcher.Get() != nullptr), keyBlocked, use);
        if (visTook >= kBusyCall) o.Busy();   // Explorer is busy: Start will be slow too
        // The timeline starts here: finding the taskbar's button or reaching a busy Explorer after logon can
        // take seconds, and the wait for Start must not be used up before anything was sent.
        const double t0 = NowSeconds();
        HRESULT lastShell = S_OK;
        FrontSeen seen;
        while (!leftBehind && !o.Done()) {
            if (g_current.load() != id) { leftBehind = true; break; }
            const double t = NowSeconds() - t0;
            if (g_closed.exchange(false)) { o.Closed(t); break; }
            if (g_again.exchange(false)) o.Again(t);
            const bool visible = Visible(vis.Get(), launcher.Get(), &seen);
            if (g_current.load() != id) { leftBehind = true; break; }   // that call may have hung as well
            const Way way = o.Next(t, visible);
            if (way == Way::Taskbar) {
                const taskbarstart::Pressed p = taskbarstart::Press(monitor);
                tb.hr = p.hr;
                tb.how = p.how;
                o.Sent(way, NowSeconds() - t0, p.ok || p.unsure);
                if (p.unsure) o.Uncertain();                  // it may still arrive: nothing else may follow it
                else if (p.took >= kBusyCall) o.Busy();       // the press itself was slow: Explorer is busy
            } else if (way == Way::Shell) {
                if (made == E_PENDING) {   // needed now (the taskbar's button went first)
                    made = MakeLauncher(monitor, id, launcher, connected);
                    if (FAILED(made) || !launcher) o.ShellLost();   // as if it had not been there: the other way
                    continue;   // reaching the shell can take seconds: look again before asking it (Start may be up)
                }
                lastShell = launcher ? launcher->ShowStartView(kShowFromStartButton, kIgnoreForegroundError) : E_POINTER;
                o.Sent(way, NowSeconds() - t0, SUCCEEDED(lastShell));   // when the shell answered: it can take a while
            } else if (way == Way::Key) {
                const bool inserted = SendKeyCombo({ VK_LWIN });
                o.Sent(way, NowSeconds() - t0, inserted);
            } else if (!o.Done()) {
                Sleep(static_cast<DWORD>(kLook * 1000));
            }
        }
        std::lock_guard<std::mutex> lk(g_mu);
        if (leftBehind) {   // nothing is learned from an opening another click took over: no telling what did what
            Log(L"start: the click left behind on a hung Explorer got an answer again after %.1f s: nothing more sent, nothing learned "
                L"(a later click started over; sent before: the taskbar's button %d, the shell %d, the Windows key %d)",
                NowSeconds() - clicked, o.TaskbarSent() ? 1 : 0, o.ShellSent(), o.KeySent());
        } else {
            Report(o, made, connected, lastShell, front, t0 - clicked, tb);
            switch (LearnTaskbar(g_pref, o)) {
            case TaskbarLesson::Works:
                SaveVerdict(TaskbarState::Works);
                Log(L"start: the taskbar's own Start button brought Start: the start button presses it from now on, on this Windows build "
                    L"(it opens and closes Start as the taskbar does)");
                break;
            case TaskbarLesson::Unproven:   // the kept verdict stays: one miss is no proof
                Log(L"start: the taskbar's Start button was pressed and Start did not come: the next click checks it again (the shell after %.0f s)", kTaskbarProof);
                break;
            case TaskbarLesson::Failed: {
                // right after Windows or Explorer started everything is slow: what is learned then is not kept
                const double up = GetTickCount64() / 1000.0, explorer = ExplorerAge();
                const bool early = EarlyForVerdict(up, explorer);
                if (!early) SaveVerdict(TaskbarState::Failed);
                g_pref.taskbarFailedDay = early ? -1 : Today();   // tried again kVerdictDays from now, within this run too
                wchar_t kept[128] = L"(tried again in 3 days, or after a Windows update)", age[48] = L"Explorer's age unknown";
                if (explorer >= 0) _snwprintf_s(age, ARRAYSIZE(age), _TRUNCATE, L"Explorer %.0f s", explorer);
                if (early) _snwprintf_s(kept, ARRAYSIZE(kept), _TRUNCATE, L"for this run (Windows up %.0f s, %ls: started minutes ago, not kept)", up, age);
                Log(L"start: pressing the taskbar's Start button did not bring Start %d clicks in a row: the start button asks the shell %ls", kTaskbarMissesOff, kept);
                break;
            }
            case TaskbarLesson::GaveUp:
                Log(L"start: %d clicks in a row brought no Start, the taskbar's button pressed and the other ways sent: the button is left alone "
                    L"for this run, so the shell and the key are found out as without it", kTaskbarMissesOff);
                break;
            case TaskbarLesson::None: break;
            }
            switch (Learn(g_pref, o)) {
            case Lesson::KeyFirst:
                Log(L"start: the shell's request did not bring Start %d clicks in a row: the Windows key goes first from now on", kMissesKeyFirst);
                break;
            case Lesson::ShellFirst: Log(L"start: the shell's request brought Start: it goes first again"); break;
            case Lesson::None: break;
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    uint32_t mine = id;
    g_current.compare_exchange_strong(mine, 0);   // unless a later click has started over
}

// Start's window belongs to the shell; it may come in front - and take the keyboard, so typing searches -
// only if the dock lets it. The dock needs that right itself first: a click on its own window does not
// always give it (system.h).
void AllowShellForeground() {
    TakeForegroundRight();
    DWORD pid = 0;
    if (HWND shell = GetShellWindow()) GetWindowThreadProcessId(shell, &pid);
    if (pid) AllowSetForegroundWindow(pid);
}

// Start is shown on this monitor: its window - or its search box's, SearchHost.exe on 25H2 - is in front
// there. (Start open on another monitor: that taskbar's button, not this one's, would close it.)
bool StartShownOn(HMONITOR monitor) {
    HWND fg = GetForegroundWindow();
    if (!fg || MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) != monitor) return false;
    const Front f = InFrontNow();
    return f.start || !_wcsicmp(f.exe.c_str(), L"SearchHost.exe");
}

// Start in front, and the taskbar's button has brought it before: that button closes it, as on the taskbar -
// pressed on a worker (UI Automation waits on Explorer). A press that surely did not go through: the Windows
// key, unless Start has gone meanwhile; one that may still arrive (a timeout): nothing more, as the key on
// top of a late press would open Start again. false: no thread to be had (the caller presses the key).
bool CloseWithTaskbar(HMONITOR monitor) {
    try {
        std::thread([monitor] {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const taskbarstart::Pressed p = taskbarstart::Press(monitor);
            if (p.unsure) {
                Log(L"start: closing Start with the taskbar's Start button: unsure whether the press arrived (%ls, %08lx): nothing more sent",
                    p.how, (unsigned long)p.hr);
            } else if (!p.ok) {
                ComPtr<IVisibility> vis;
                CoCreateInstance(kAppVisibility, nullptr, CLSCTX_ALL, kAppVisibilityIid, reinterpret_cast<void**>(vis.GetAddressOf()));
                Sleep(150);
                const bool still = Visible(vis.Get(), nullptr);
                if (still) SendKeyCombo({ VK_LWIN });
                Log(L"start: closing Start with the taskbar's Start button did not go through (%ls, %08lx): %ls", p.how, (unsigned long)p.hr,
                    still ? L"the Windows key closed it" : L"Start had gone by itself");
            }
            if (SUCCEEDED(co)) CoUninitialize();
        }).detach();
        return true;
    } catch (...) {
        return false;
    }
}

}   // namespace

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
    bool taskbarCloses = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        LoadVerdict();   // the first click: what the taskbar's button did on this Windows build before
        taskbarCloses = ClosesWithTaskbar(g_pref);
    }
    const uint32_t running = g_current.load();
    const double age = (GetTickCount64() - g_since.load()) / 1000.0;
    switch (OnPress(startWasInFront, StillOpening(running != 0, age))) {
    case Press::Close:   // Start is in front: the taskbar's button closes it (proven, one kept here), else the Windows key
        if (running) g_closed = true;   // a wait still running must not bring it back
        if (!taskbarCloses || !taskbarstart::Kept(monitor) || !StartShownOn(monitor) || !CloseWithTaskbar(monitor)) SendKeyCombo({ VK_LWIN });
        return;
    case Press::Again:
        g_again = true;
        return;
    case Press::Open:
        break;
    }
    if (running) Log(L"start: the last click's request has waited on Explorer for %.0f s: starting over", age);
    AllowShellForeground();
    if (++g_lastId == 0) ++g_lastId;   // never 0
    const uint32_t id = g_lastId;
    g_again = false;
    g_closed = false;
    g_since = GetTickCount64();
    g_current = id;
    try {
        std::thread(Run, monitor, id).detach();
    } catch (...) {   // no thread to be had: the Windows key, as before
        g_current = 0;
        SendKeyCombo({ VK_LWIN });
    }
}

}   // namespace startmenu
