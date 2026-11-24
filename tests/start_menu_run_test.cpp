// The start button's worker (src/start_menu.cpp: Click, Run) against a simulated Explorer: the shell's
// launcher, IAppVisibility, the taskbar's Start button (taskbarstart::), the Windows key, the window in front
// and the clocks are fakes, so every path of the worker runs here - the ladder of mod.36 (the shell's request,
// kConfirm later the taskbar's button when its state reads off, kGiveUp after the request the Windows key): the
// shell reached, not to be had, refusing, taking the request and bringing Start late or never; the button found
// or not, its state off, on or not to be read, its press going through, not, timing out, slow; a slow click
// (minutes after Windows or Explorer started, a call into Explorer that took kBusyCall) waiting alone; Explorer
// gone; a worker stuck on a hung Explorer and left behind; Start closed while a wait runs; the window in front
// as a wait's looks read it (the processes and tokens opened, counted). Nothing is kept between clicks: the old
// verdict file data\taskbar-start.txt, put into data-start_menu_run\ next to the test, is neither read, written
// nor removed (the file calls start_menu.cpp makes are counted: none). Nothing reaches Windows: no key,
// no press, no call into Explorer.
// Time is virtual: the worker's looks (Sleep) move a clock of the test's own, so an 8 s wait takes a few
// milliseconds and every run is the same.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\start_menu_run_test.cpp /link user32.lib ole32.lib uuid.lib advapi32.lib dwmapi.lib
#include "../src/common.h"
#include "../src/system.h"
#include "../src/taskbar_start.h"
#include "../src/start_menu.h"
#include "../src/start_menu_logic.h"
#include <dwmapi.h>
#include <objbase.h>
#include <servprov.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

// ---- g_mu as start_menu.cpp takes it: its std::mutex is this one (a std::mutex that tells the trace below when it
// is locked and unlocked). The test's own locks of g_mu (std::lock_guard<std::mutex>) take the plain std::mutex and
// are not traced.
void TraceMuLock(bool locked);
namespace std {
struct CountedMutex : mutex {
    void lock() { mutex::lock(); TraceMuLock(true); }
    void unlock() { TraceMuLock(false); mutex::unlock(); }
};
}   // namespace std

// ---- the Windows calls start_menu.cpp makes, redirected to the fakes below
HRESULT WINAPI FakeCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* out);
void WINAPI FakeCoUninitialize();
HWND WINAPI FakeShellWindow();
HWND WINAPI FakeForegroundWindow();
BOOL WINAPI FakeQueryFullProcessImageNameW(HANDLE process, DWORD flags, LPWSTR name, PDWORD size);
BOOL WINAPI FakeAllowSetForegroundWindow(DWORD pid);
ULONGLONG WINAPI FakeTickCount64();
void WINAPI FakeSystemTimeAsFileTime(LPFILETIME ft);
BOOL WINAPI FakeProcessTimes(HANDLE process, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel, LPFILETIME user);
HANDLE WINAPI FakeOpenProcess(DWORD access, BOOL inherit, DWORD pid);
BOOL WINAPI FakeOpenProcessToken(HANDLE process, DWORD access, PHANDLE token);
void WINAPI FakeSleep(DWORD ms);
bool FakeKeyCombo(std::initializer_list<WORD> keys);
HRESULT WINAPI FakeCoInitializeEx(LPVOID reserved, DWORD model);
BOOL WINAPI FakeGetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS what, LPVOID out, DWORD size, PDWORD got);
DWORD WINAPI FakeGetWindowThreadProcessId(HWND window, LPDWORD pid);
// what the click's dock.log lines read of the windows and the input (user32, DWM) - traced "win" (MonitorFromWindow
// not: only a closing click's StartShownOn asked it, before mod.36)
BOOL WINAPI FakeIsWindow(HWND w);
int WINAPI FakeGetClassNameW(HWND w, LPWSTR name, int size);
BOOL WINAPI FakeIsWindowVisible(HWND w);
HWND WINAPI FakeFindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR title);
HRESULT WINAPI FakeDwmGetWindowAttribute(HWND w, DWORD what, PVOID out, DWORD size);
BOOL WINAPI FakeGetLastInputInfo(PLASTINPUTINFO li);
HMONITOR WINAPI FakeMonitorFromPoint(POINT p, DWORD flags);
HMONITOR WINAPI FakeMonitorFromWindow(HWND w, DWORD flags);
BOOL WINAPI FakeGetMonitorInfoW(HMONITOR m, LPMONITORINFO mi);
// the file and registry calls start_menu.cpp made for the kept verdict (mod.18 to mod.35): none is left - counted,
// so that one coming back shows (the old verdict file is neither read, written nor removed)
HANDLE WINAPI FakeCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD how, DWORD flags, HANDLE tmpl);
BOOL WINAPI FakeReadFile(HANDLE f, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED o);
BOOL WINAPI FakeWriteFile(HANDLE f, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED o);
BOOL WINAPI FakeDeleteFileW(LPCWSTR name);
DWORD WINAPI FakeGetFileAttributesW(LPCWSTR name);
BOOL WINAPI FakeMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
LSTATUS WINAPI FakeRegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD size);
#define CreateFileW FakeCreateFileW
#define ReadFile FakeReadFile
#define WriteFile FakeWriteFile
#define DeleteFileW FakeDeleteFileW
#define GetFileAttributesW FakeGetFileAttributesW
#define MoveFileExW FakeMoveFileExW
#define RegGetValueW FakeRegGetValueW
#define IsWindow FakeIsWindow
#define GetClassNameW FakeGetClassNameW
#define IsWindowVisible FakeIsWindowVisible
#define FindWindowExW FakeFindWindowExW
#define DwmGetWindowAttribute FakeDwmGetWindowAttribute
#define GetLastInputInfo FakeGetLastInputInfo
#define MonitorFromPoint FakeMonitorFromPoint
#define MonitorFromWindow FakeMonitorFromWindow
#define GetMonitorInfoW FakeGetMonitorInfoW
#define GetWindowThreadProcessId FakeGetWindowThreadProcessId
#define CoInitializeEx FakeCoInitializeEx
#define GetTokenInformation FakeGetTokenInformation
#define CoCreateInstance FakeCoCreateInstance
#define CoUninitialize FakeCoUninitialize
#define GetShellWindow FakeShellWindow
#define GetForegroundWindow FakeForegroundWindow
#define QueryFullProcessImageNameW FakeQueryFullProcessImageNameW
#define AllowSetForegroundWindow FakeAllowSetForegroundWindow
#define GetTickCount64 FakeTickCount64
#define GetSystemTimeAsFileTime FakeSystemTimeAsFileTime
#define GetProcessTimes FakeProcessTimes
#define OpenProcess FakeOpenProcess
#define OpenProcessToken FakeOpenProcessToken
#define Sleep FakeSleep
#define SendKeyCombo FakeKeyCombo
#define mutex CountedMutex
#include "../src/start_menu.cpp"
#undef mutex
#undef CreateFileW
#undef ReadFile
#undef WriteFile
#undef DeleteFileW
#undef GetFileAttributesW
#undef MoveFileExW
#undef RegGetValueW
#undef IsWindow
#undef GetClassNameW
#undef IsWindowVisible
#undef FindWindowExW
#undef DwmGetWindowAttribute
#undef GetLastInputInfo
#undef MonitorFromPoint
#undef MonitorFromWindow
#undef GetMonitorInfoW
#undef GetWindowThreadProcessId
#undef CoInitializeEx
#undef GetTokenInformation
#undef CoCreateInstance
#undef CoUninitialize
#undef GetShellWindow
#undef GetForegroundWindow
#undef QueryFullProcessImageNameW
#undef AllowSetForegroundWindow
#undef GetTickCount64
#undef GetSystemTimeAsFileTime
#undef GetProcessTimes
#undef OpenProcess
#undef OpenProcessToken
#undef Sleep
#undef SendKeyCombo

using namespace startmenu;

// ---- the trace (CharacterTests only): what the worker, Click and the closing thread ask of the fakes, in order -
// g_mu's locks, the clock read, Explorer's calls, the looks, sleeps, log lines; a press, request or key with its
// time (microseconds since the scene began). Off otherwise: every token is pushed only while g_tracing.
// g_traceOff, g_traceMain: the same, of the threads other than the test's own (the closing thread runs beside the UI thread), and of its own.
static std::atomic<bool> g_tracing{ false };
static std::mutex g_traceMu;
static std::vector<std::string> g_trace, g_traceOff, g_traceMain;
static DWORD g_mainTid = 0;
static int64_t SceneUs();
static void Trace(const std::string& token) {
    if (!g_tracing) return;
    std::lock_guard<std::mutex> lk(g_traceMu);
    g_trace.push_back(token);
    (GetCurrentThreadId() != g_mainTid ? g_traceOff : g_traceMain).push_back(token);
}
static void TraceAt(const char* token) { if (g_tracing) Trace(std::string(token) + "@" + std::to_string(SceneUs())); }
void TraceMuLock(bool locked) { Trace(locked ? "lock" : "unlock"); }
// the monitor a fake was asked about, by name - a handle differs from PC to PC: the primary monitor (the
// dock's, as a rule), kOtherMon (a made-up handle standing for a secondary monitor's: the fakes never look into it),
// none, or another
static HMONITOR g_primaryMon = nullptr;
static const HMONITOR kOtherMon = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x7e570001));
static std::string MonName(HMONITOR m) { return m == g_primaryMon ? "primary" : m == kOtherMon ? "other" : !m ? "null" : "?"; }

// ---- the virtual clock: NowSeconds, GetTickCount64 and the worker's Sleep
static std::atomic<int64_t> g_virtUs{ 1000000000 };   // microseconds
static std::atomic<int64_t> g_tickBaseMs{ 10LL * 86400 * 1000 };   // Windows up 10 days (a failure is kept)
static std::atomic<long> g_dayShift{ 0 };              // days added to the real date (Today)
static std::atomic<int64_t> g_explorerAgeS{ 10LL * 86400 };   // Explorer (the shell window's process) up 10 days; -1: no telling
static std::atomic<int64_t> g_pauseAtUs{ -1 };         // the next Sleep that reaches this time waits for the test
static HANDLE g_paused = nullptr, g_resume = nullptr;
static std::atomic<int64_t> g_sceneBaseUs{ 0 };      // CharacterTests: when the scene began
static int64_t SceneUs() { return g_virtUs.load() - g_sceneBaseUs.load(); }
double NowSeconds() { Trace("now"); return g_virtUs.load() / 1e6; }
static void Advance(double s) { g_virtUs += static_cast<int64_t>(s * 1e6); }
ULONGLONG WINAPI FakeTickCount64() { Trace("tick"); return static_cast<ULONGLONG>(g_tickBaseMs.load() + g_virtUs.load() / 1000); }
static void ShiftedSystemTime(LPFILETIME ft) {
    GetSystemTimeAsFileTime(ft);
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime; u.HighPart = ft->dwHighDateTime;
    u.QuadPart += static_cast<ULONGLONG>(g_dayShift.load()) * 864000000000ULL;
    ft->dwLowDateTime = u.LowPart; ft->dwHighDateTime = u.HighPart;
}
void WINAPI FakeSystemTimeAsFileTime(LPFILETIME ft) { Trace("date"); ShiftedSystemTime(ft); }
// the shell window's process (here the test's own): started g_explorerAgeS before the (shifted) date; Start's
// (StartMenuExperienceHost.exe, the test's own too) g_startAgeS before it
static std::atomic<int64_t> g_startAgeS{ 23400 };
static const DWORD kStartPid = 0x7ffffff4;
static DWORD OpenedPid(HANDLE process);   // the process id FakeOpenProcess was asked for, for that handle
BOOL WINAPI FakeProcessTimes(HANDLE process, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel, LPFILETIME user) {
    Trace("ptimes");
    const int64_t age = OpenedPid(process) == kStartPid ? g_startAgeS.load() : g_explorerAgeS.load();
    if (age < 0) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    FILETIME now;
    ShiftedSystemTime(&now);
    ULARGE_INTEGER u;
    u.LowPart = now.dwLowDateTime; u.HighPart = now.dwHighDateTime;
    u.QuadPart -= static_cast<ULONGLONG>(age) * 10000000ULL;
    created->dwLowDateTime = u.LowPart; created->dwHighDateTime = u.HighPart;
    *exited = *kernel = *user = FILETIME{};
    return TRUE;
}
void WINAPI FakeSleep(DWORD ms) {
    Trace(ms == static_cast<DWORD>(kLook * 1000) ? "sleep" : "sleep" + std::to_string(ms));
    g_virtUs += static_cast<int64_t>(ms) * 1000;
    int64_t at = g_pauseAtUs.load();
    if (at >= 0 && g_virtUs.load() >= at && g_pauseAtUs.compare_exchange_strong(at, -1)) {
        SetEvent(g_paused);
        WaitForSingleObject(g_resume, 20000);
    }
    Sleep(0);
}

// ---- what the worker needs from the rest of the dock
static std::mutex g_logMu;
static std::vector<std::wstring> g_log;
static bool g_printLog = false;
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    Trace("log");
    std::lock_guard<std::mutex> lk(g_logMu);
    g_log.push_back(buf);
    if (g_printLog) {
        char utf8[4096] = {};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
        printf("  log: %s\n", utf8);
    }
}
static wchar_t g_dataDir[MAX_PATH] = L"";
// data-start_menu_run\ next to the test, where the old verdict file would be - and no one asks for it any more
static std::atomic<int> g_dataDirAsked{ 0 }, g_fileCalls{ 0 };
std::wstring AppDataDir() { ++g_dataDirAsked; return g_dataDir; }
HANDLE WINAPI FakeCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD how, DWORD flags, HANDLE tmpl) {
    ++g_fileCalls;
    return CreateFileW(name, access, share, sa, how, flags, tmpl);
}
BOOL WINAPI FakeReadFile(HANDLE f, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED o) { ++g_fileCalls; return ReadFile(f, buf, n, got, o); }
BOOL WINAPI FakeWriteFile(HANDLE f, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED o) { ++g_fileCalls; return WriteFile(f, buf, n, put, o); }
BOOL WINAPI FakeDeleteFileW(LPCWSTR name) { ++g_fileCalls; return DeleteFileW(name); }
DWORD WINAPI FakeGetFileAttributesW(LPCWSTR name) { ++g_fileCalls; return GetFileAttributesW(name); }
BOOL WINAPI FakeMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) { ++g_fileCalls; return MoveFileExW(from, to, flags); }
LSTATUS WINAPI FakeRegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD size) {
    ++g_fileCalls;
    return RegGetValueW(key, sub, value, flags, type, data, size);
}
static ForegroundRight g_right = ForegroundRight::Had;   // what the right's probe answers, for the click's lines
ForegroundRight TakeForegroundRight() { return g_right; }   // nothing goes out
#define TEST_SUPPORT_FOREGROUND_RIGHT_TEXT
#include "test_support.h"

// ---- simulated Start: visible from a virtual time on (-1: not coming)
static std::atomic<int64_t> g_visibleAtUs{ -1 };
static std::atomic<int> g_cancelled{ 0 };   // a Start on its way stopped by a second request
static bool StartUp() { const int64_t at = g_visibleAtUs.load(); return at >= 0 && g_virtUs.load() >= at; }
static bool StartComing() { const int64_t at = g_visibleAtUs.load(); return at >= 0 && g_virtUs.load() < at; }   // on its way
static void BringStart(double after) {
    if (g_visibleAtUs.load() < 0) g_visibleAtUs = g_virtUs.load() + static_cast<int64_t>(after * 1e6);
}
static void CloseStart() { g_visibleAtUs = -1; }
// A toggle (a press on the taskbar's button, the Windows key): Start up is closed, a Start on its way stopped
// (counted: g_cancelled), else Start is brought after `after` seconds when the toggle brings it
static void ToggleStart(bool brings, double after) {
    if (StartUp()) CloseStart();
    else if (StartComing()) { CloseStart(); ++g_cancelled; }
    else if (brings) BringStart(after);
}

// ---- the Windows key
// when each thing last went out (the virtual clock's microseconds; -1: it did not): the request, the lookup of the
// button, its press, a press held back, the key
static std::atomic<int64_t> g_showAtUs{ -1 }, g_findAtUs{ -1 }, g_pressAtUs{ -1 }, g_holdAtUs{ -1 }, g_keyAtUs{ -1 };
static std::atomic<int> g_keys{ 0 };
static bool g_keyBrings = true;
static bool g_keyInserted = true;   // false: SendInput inserts nothing (input blocked) - counted, nothing happens
bool FakeKeyCombo(std::initializer_list<WORD> keys) {
    std::string k;   // the keys sent, in hex: "key(5B)" is the left Windows key
    for (WORD w : keys) { char b[8]; snprintf(b, sizeof(b), "%s%02X", k.empty() ? "" : "+", w); k += b; }
    TraceAt(("key(" + k + ")").c_str());
    g_keyAtUs = g_virtUs.load();
    ++g_keys;
    if (!g_keyInserted) return false;
    ToggleStart(g_keyBrings, 0.08);
    return true;
}

// ---- the window in front: a hidden window of the test's own, "named" like the program the test wants
// (g_otherWnd: another one of the test's own, as another program's window coming in front)
static HWND g_shellWnd = nullptr, g_firstWnd = nullptr, g_otherWnd = nullptr;
static std::atomic<HWND> g_frontWnd{ nullptr };
static std::atomic<bool> g_explorerUp{ true };
static wchar_t g_frontExe[MAX_PATH] = L"C:\\Program Files\\Test\\notepad.exe";
HWND WINAPI FakeShellWindow() { Trace("shellwnd"); return g_explorerUp ? g_shellWnd : nullptr; }
// g_frontWhenUp: the window in front while Start is up (its search box's, as on 25H2 and 26H2), when set
static std::atomic<HWND> g_frontWhenUp{ nullptr };
static bool StartUp();
HWND WINAPI FakeForegroundWindow() { Trace("fg"); const HWND up = g_frontWhenUp.load(); return up && StartUp() ? up : g_frontWnd.load(); }
// the shell's window answers a process id of its own (kShellPid, standing for Explorer's), so a call
// that hands the right to come in front to another process than the shell's shows in the trace; opening that process
// opens the test's own (its times are the fake Explorer's). Every other window: as it is.
static const DWORD kShellPid = 0x7ffffff0;
// CoreWindows of the shell's programs, hidden windows of the test's own with process ids of their own: Start's
// (StartMenuExperienceHost.exe), its search box's (SearchHost.exe) and another one (ShellExperienceHost.exe). FindWindowExW
// lists them (other, search, Start) only while g_coreWindows; as the window in front they are what they say.
static HWND g_startCoreWnd = nullptr, g_searchWnd = nullptr, g_otherCoreWnd = nullptr;
static const DWORD kSearchPid = 0x7ffffff8, kOtherCorePid = 0x7ffffffc;
DWORD WINAPI FakeGetWindowThreadProcessId(HWND window, LPDWORD pid) {
    const DWORD thread = GetWindowThreadProcessId(window, pid);
    if (window && thread && pid) {
        if (window == g_shellWnd) *pid = kShellPid;
        else if (window == g_startCoreWnd) *pid = kStartPid;
        else if (window == g_searchWnd) *pid = kSearchPid;
        else if (window == g_otherCoreWnd) *pid = kOtherCorePid;
    }
    return thread;
}
// the processes and tokens start_menu.cpp opens (the real ones, of the test's own process), counted;
// g_openRefused: the program in front may not be looked into (as some system processes)
static std::atomic<int> g_processesOpened{ 0 }, g_tokensOpened{ 0 };
static std::atomic<bool> g_openRefused{ false };
static std::mutex g_openedMu;
static std::map<HANDLE, DWORD> g_openedPid;   // each handle FakeOpenProcess gave, with the id it was asked for
static DWORD OpenedPid(HANDLE process) {
    std::lock_guard<std::mutex> lk(g_openedMu);
    const auto it = g_openedPid.find(process);
    return it == g_openedPid.end() ? 0 : it->second;
}
HANDLE WINAPI FakeOpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    Trace("open");
    ++g_processesOpened;
    if (g_openRefused) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    const bool ours = pid == kShellPid || pid == kStartPid || pid == kSearchPid || pid == kOtherCorePid;
    const HANDLE h = OpenProcess(access, inherit, ours ? GetCurrentProcessId() : pid);
    if (h) { std::lock_guard<std::mutex> lk(g_openedMu); g_openedPid[h] = pid; }
    return h;
}
BOOL WINAPI FakeOpenProcessToken(HANDLE process, DWORD access, PHANDLE token) {
    Trace("token");
    ++g_tokensOpened;
    return OpenProcessToken(process, access, token);
}
// g_frontElevated: the program in front runs as administrator (its token says so). The test's own token, which
// SelfElevated reads once per process (before any test sets this), is read as it is.
static std::atomic<bool> g_frontElevated{ false };
BOOL WINAPI FakeGetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS what, LPVOID out, DWORD size, PDWORD got) {
    if (g_frontElevated && what == TokenElevation && out && size >= sizeof(TOKEN_ELEVATION)) {
        static_cast<TOKEN_ELEVATION*>(out)->TokenIsElevated = 1;
        if (got) *got = sizeof(TOKEN_ELEVATION);
        return TRUE;
    }
    return GetTokenInformation(token, what, out, size, got);
}
static const wchar_t kStartExePath[] = L"C:\\Windows\\SystemApps\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\StartMenuExperienceHost.exe";
static const wchar_t kSearchExePath[] = L"C:\\Windows\\SystemApps\\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\\SearchHost.exe";
static const wchar_t kOtherCoreExePath[] = L"C:\\Windows\\SystemApps\\ShellExperienceHost_cw5n1h2txyewy\\ShellExperienceHost.exe";
BOOL WINAPI FakeQueryFullProcessImageNameW(HANDLE process, DWORD, LPWSTR name, PDWORD size) {
    const DWORD pid = OpenedPid(process);   // the shell's CoreWindows' programs; any other process: g_frontExe
    const wchar_t* exe = pid == kStartPid ? kStartExePath : pid == kSearchPid ? kSearchExePath : pid == kOtherCorePid ? kOtherCoreExePath : g_frontExe;
    if (wcslen(exe) + 1 > *size) return FALSE;
    wcscpy_s(name, *size, exe);
    *size = static_cast<DWORD>(wcslen(exe));
    return TRUE;
}
static std::atomic<int> g_rightsGiven{ 0 };
static std::atomic<bool> g_allowRefused{ false };   // handing the right to the shell's process fails (error 5)
BOOL WINAPI FakeAllowSetForegroundWindow(DWORD pid) {   // to whom: the shell's process, the dock's own, or another
    Trace(std::string("allowfg(") + (pid == kShellPid ? "shell" : pid == GetCurrentProcessId() ? "self" : "other") + ")");
    ++g_rightsGiven;
    if (g_allowRefused) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}

// ---- the windows and the input as the click's dock.log lines read them. Every window is visible but g_hiddenWnd;
// cloaked: Start's and its search box's as Start is (2 closed, 0 up) unless g_startCloak / g_searchCloak say otherwise,
// any other window g_frontCloak. The window classes are the real ones (the test's: "hunt-start_menu-front"), the
// CoreWindows' "Windows.UI.Core.CoreWindow", g_firstWnd's g_firstClass when set. The input: g_lastInputTick.
// Monitors: the primary one 0,0 2560x1440 and kOtherMon -1440,-560 1440x2560 (a two-monitor layout), whatever this PC has.
static std::atomic<bool> g_coreWindows{ false };
static std::atomic<int> g_startCloak{ -1 }, g_searchCloak{ -1 };
static std::atomic<DWORD> g_frontCloak{ 0 };
static std::atomic<HWND> g_hiddenWnd{ nullptr };
static wchar_t g_firstClass[64] = L"";
static std::atomic<DWORD> g_lastInputTick{ 0 };
static std::atomic<HMONITOR> g_searchMon{ nullptr };   // the search box's monitor (nullptr: as Windows says)
static bool IsCore(HWND w) { return w && (w == g_startCoreWnd || w == g_searchWnd || w == g_otherCoreWnd); }
BOOL WINAPI FakeIsWindow(HWND w) { Trace("win"); return IsWindow(w); }
int WINAPI FakeGetClassNameW(HWND w, LPWSTR name, int size) {
    Trace("win");
    const wchar_t* cls = IsCore(w) ? L"Windows.UI.Core.CoreWindow" : w && w == g_firstWnd && g_firstClass[0] ? g_firstClass : nullptr;
    if (!cls) return GetClassNameW(w, name, size);
    wcsncpy_s(name, size, cls, _TRUNCATE);
    return static_cast<int>(wcslen(name));
}
BOOL WINAPI FakeIsWindowVisible(HWND w) { Trace("win"); return w && w != g_hiddenWnd.load(); }
HWND WINAPI FakeFindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR) {
    Trace("win");
    if (parent || !cls || wcscmp(cls, L"Windows.UI.Core.CoreWindow") || !g_coreWindows) return nullptr;
    const HWND list[] = { g_otherCoreWnd, g_searchWnd, g_startCoreWnd };
    if (!after) return list[0];
    for (size_t i = 0; i + 1 < ARRAYSIZE(list); ++i) if (list[i] == after) return list[i + 1];
    return nullptr;
}
static std::atomic<int> g_cloakReads{ 0 };
HRESULT WINAPI FakeDwmGetWindowAttribute(HWND w, DWORD what, PVOID out, DWORD size) {
    Trace("win");
    if (what != DWMWA_CLOAKED || !out || size < sizeof(DWORD)) return E_INVALIDARG;
    ++g_cloakReads;
    const int start = g_startCloak.load(), search = g_searchCloak.load();
    *static_cast<DWORD*>(out) = w == g_startCoreWnd ? (start >= 0 ? start : StartUp() ? 0 : 2) : w == g_searchWnd ? (search >= 0 ? search : StartUp() ? 0 : 2)
                              : w == g_otherCoreWnd ? 2 : g_frontCloak.load();
    return S_OK;
}
BOOL WINAPI FakeGetLastInputInfo(PLASTINPUTINFO li) { Trace("win"); li->dwTime = g_lastInputTick.load(); return TRUE; }
HMONITOR WINAPI FakeMonitorFromPoint(POINT p, DWORD flags) { Trace("win"); return MonitorFromPoint(p, flags); }
static std::atomic<HMONITOR> g_frontMon{ nullptr };   // g_firstWnd's monitor (nullptr: as Windows says)
HMONITOR WINAPI FakeMonitorFromWindow(HWND w, DWORD flags) {
    const HMONITOR m = g_searchMon.load(), f = g_frontMon.load();
    if (w && w == g_firstWnd && f) return f;
    return w && w == g_searchWnd && m ? m : MonitorFromWindow(w, flags);
}
BOOL WINAPI FakeGetMonitorInfoW(HMONITOR m, LPMONITORINFO mi) {
    Trace("win");
    if (m != g_primaryMon && m != kOtherMon) return GetMonitorInfoW(m, mi);
    mi->rcMonitor = m == g_primaryMon ? RECT{ 0, 0, 2560, 1440 } : RECT{ -1440, -560, 0, 2000 };
    mi->rcWork = mi->rcMonitor;
    mi->dwFlags = m == g_primaryMon ? MONITORINFOF_PRIMARY : 0;
    return TRUE;
}

// ---- COM objects of the simulated Explorer, counted (a reference left over is a leak)
template <class I> struct Counted : I {
    std::atomic<long> refs{ 0 };
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == Iid()) { *out = static_cast<I*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(--refs); }
    virtual const IID& Iid() const = 0;
};

// ---- Explorer hanging: the next n calls of a kind wait until g_unblock (g_hanging: how many wait now)
static HANDLE g_unblock = nullptr;
static std::atomic<int> g_hanging{ 0 };
static std::atomic<int> g_hangShellMakes{ 0 }, g_hangVisMakes{ 0 }, g_hangPresses{ 0 }, g_hangVisLooks{ 0 }, g_hangWheres{ 0 };
static void MaybeHang(std::atomic<int>& next) {
    for (int n = next.load(); n > 0;)
        if (next.compare_exchange_weak(n, n - 1)) {
            ++g_hanging;
            WaitForSingleObject(g_unblock, 20000);
            --g_hanging;
            return;
        }
}

// g_blind: IAppVisibility and the launcher say nothing of Start (as one of them may on a given Windows release):
// only Start's window in front tells. g_looks: the worker's looks (IsLauncherVisible asked).
static std::atomic<bool> g_blind{ false }, g_launcherSees{ false };
static std::atomic<int> g_looks{ 0 };
struct FakeVis : Counted<IVisibility> {
    const IID& Iid() const override { return kAppVisibilityIid; }
    HRESULT STDMETHODCALLTYPE GetAppVisibilityOnMonitor(HMONITOR, int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsLauncherVisible(BOOL* v) override { Trace("look"); ++g_looks; MaybeHang(g_hangVisLooks); *v = !g_blind && StartUp(); return S_OK; }
} g_vis;
static HRESULT g_whereHr = S_OK;   // the dock's monitor as the shell's monitor service answers it (failed: Windows chooses)
static bool g_whereNull = false;   // ...S_OK, and no monitor object with it

struct FakeWhere : Counted<IUnknown> {
    const IID& Iid() const override { return IID_IUnknown; }
} g_where;

struct FakeMonitors : Counted<IMonitorService> {
    const IID& Iid() const override { return kMonitorServiceIid; }
    HRESULT STDMETHODCALLTYPE GetCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetConnectedCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAt(UINT, IUnknown**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFromHandle(HMONITOR m, IUnknown** out) override {
        Trace("where(" + MonName(m) + ")");
        MaybeHang(g_hangWheres);
        if (FAILED(g_whereHr) || g_whereNull) { *out = nullptr; return g_whereHr; }
        g_where.AddRef(); *out = &g_where; return S_OK;
    }
} g_monitors;

static std::atomic<int> g_shows{ 0 }, g_connects{ 0 }, g_services{ 0 };   // requests; launchers moved to a monitor; services asked for
static std::atomic<HWND> g_frontAfterShow{ nullptr };   // in front once the shell answered a request (nullptr: as it was)
static HRESULT g_showHr = S_OK;
static bool g_showBrings = true;
static double g_showDelay = 0.12;
static std::atomic<int64_t> g_showTookUs{ 0 };   // the request itself takes this long (virtual microseconds)
static std::atomic<int> g_launcherLooks{ 0 };    // ILauncher::IsVisible asked
struct FakeLauncher : Counted<ILauncher> {
    const IID& Iid() const override { return kLauncherIid; }
    HRESULT STDMETHODCALLTYPE ShowStartView(int method, int flags) override {
        TraceAt(("show(" + std::to_string(method) + "," + std::to_string(flags) + ")").c_str());
        g_showAtUs = g_virtUs.load();
        ++g_shows;
        g_virtUs += g_showTookUs.load();
        if (HWND w = g_frontAfterShow.load()) g_frontWnd = w;   // in front once the request returned
        if (FAILED(g_showHr)) return g_showHr;
        // as seen on a test PC: a request while a Start is on its way stops it
        const int64_t at = g_visibleAtUs.load();
        if (at >= 0 && g_virtUs.load() < at) { CloseStart(); ++g_cancelled; return S_OK; }
        if (g_showBrings && g_cancelled == 0) BringStart(g_showDelay);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Dismiss(int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DismissToLastDesktopApplication(int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DismissSynchronouslyWithoutTransition() override { return E_NOTIMPL; }
    // (g_launcherSees: the launcher tells Start up even while g_blind - a click that has no IAppVisibility asks it)
    HRESULT STDMETHODCALLTYPE IsVisible(BOOL* v) override { Trace("isvis"); ++g_launcherLooks; *v = (!g_blind || g_launcherSees) && StartUp(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStartButtonPressed(int, int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetForeground() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ConnectToMonitor(IUnknown*) override { Trace("connect"); ++g_connects; return S_OK; }
} g_launcher;

struct FakeShell : Counted<IServiceProvider> {
    const IID& Iid() const override { return IID_IServiceProvider; }
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID iid, void** out) override {
        Trace(service == kLauncherService ? "qs-launcher" : service == kMonitorService ? "qs-monitors" : "qs");
        ++g_services;
        *out = nullptr;
        if (service == kLauncherService && iid == kLauncherIid) { g_launcher.AddRef(); *out = static_cast<ILauncher*>(&g_launcher); return S_OK; }
        if (service == kMonitorService && iid == kMonitorServiceIid) { g_monitors.AddRef(); *out = static_cast<IMonitorService*>(&g_monitors); return S_OK; }
        return E_NOINTERFACE;
    }
} g_shell;

static std::atomic<int> g_shellMade{ 0 }, g_shellMadeWithoutExplorer{ 0 }, g_visMade{ 0 };
static HRESULT g_shellMakeHr = S_OK;
static HRESULT g_visMakeHr = S_OK;   // failed: no IAppVisibility to be had (the worker's; InFront's was made before)
static double g_visMakeTook = 0;
static std::atomic<int64_t> g_shellMakeTookUs{ 0 };   // reaching the shell takes this long (virtual microseconds)
HRESULT WINAPI FakeCoCreateInstance(REFCLSID clsid, LPUNKNOWN, DWORD, REFIID, LPVOID* out) {
    *out = nullptr;
    if (clsid == kAppVisibility) {
        Trace("mkvis");
        ++g_visMade;
        MaybeHang(g_hangVisMakes);
        Advance(g_visMakeTook);
        if (FAILED(g_visMakeHr)) return g_visMakeHr;
        g_vis.AddRef(); *out = static_cast<IVisibility*>(&g_vis); return S_OK;
    }
    if (clsid == kImmersiveShell) {
        Trace("mkshell");
        ++g_shellMade;
        if (!g_explorerUp) ++g_shellMadeWithoutExplorer;
        MaybeHang(g_hangShellMakes);
        g_virtUs += g_shellMakeTookUs.load();
        if (FAILED(g_shellMakeHr)) return g_shellMakeHr;
        g_shell.AddRef(); *out = static_cast<IServiceProvider*>(&g_shell); return S_OK;
    }
    return REGDB_E_CLASSNOTREG;
}
static std::atomic<int> g_threadsDone{ 0 };   // a worker (or the closing thread) left its apartment: it is through
// The button's worker number as a thread entered its apartment (a worker's own: Click sets it before the thread starts;
// the closing thread's: 0 as a rule).
static thread_local uint32_t t_number = 0;
HRESULT WINAPI FakeCoInitializeEx(LPVOID reserved, DWORD model) { Trace("coinit"); t_number = g_current.load(); return CoInitializeEx(reserved, model); }
// what was still held of the simulated Explorer's objects (every thread's, InFront's IAppVisibility among them) as a
// thread left its apartment, and the button's worker number then: still this worker's ("mine"), none, or another's
// (CharacterTests)
static std::string ComHeld() {
    const uint32_t cur = g_current.load();
    return "couninit[shell " + std::to_string(g_shell.refs.load()) + ", launcher " + std::to_string(g_launcher.refs.load()) +
           ", monitors " + std::to_string(g_monitors.refs.load()) + ", where " + std::to_string(g_where.refs.load()) +
           ", vis " + std::to_string(g_vis.refs.load()) + ", current " + (cur == 0 ? "0" : cur == t_number ? "mine" : "another") + "]";
}
void WINAPI FakeCoUninitialize() { if (g_tracing) Trace(ComHeld()); CoUninitialize(); ++g_threadsDone; }

// ---- the taskbar's own Start button (taskbar_start.h)
static std::atomic<int> g_finds{ 0 }, g_presses{ 0 };
static std::atomic<HWND> g_frontAfterPress{ nullptr };   // in front once the press returned (nullptr: as it was)
static std::atomic<int> g_cloakAfterPress{ -1 };         // Start's window's cloak from the press on (-1: as Start is)
static taskbarstart::Found g_found = taskbarstart::Found::NoButton;
static taskbarstart::Pressed g_pressed;   // what a press answers
static bool g_pressBrings = true;
static double g_pressDelay = 0.1, g_pressTook = 0.01;
static std::atomic<int64_t> g_findTookUs{ 0 };   // finding the button takes this long (virtual microseconds)
// the button's toggle state as a press reads it right before (0 off, 1 on, 2 indeterminate, -1 not read);
// kStateFollows: on while a Start is on its way or up, whoever brought it, off otherwise. g_noToggle: the button has no
// Toggle pattern (no state to read). g_holds: presses asked only of an off button that did not go out. g_pressesAnyState:
// presses asked whatever the state (the worker makes none).
static const int kStateFollows = -2;
static std::atomic<int> g_buttonState{ kStateFollows }, g_holds{ 0 }, g_pressesAnyState{ 0 }, g_keptAsked{ 0 };
static std::atomic<bool> g_noToggle{ false };
namespace taskbarstart {
HWND TaskbarOn(HMONITOR) { return nullptr; }
void Remember(HWND) {}
const wchar_t* FoundText(Found f) {
    return f == Found::Kept ? L"kept" : f == Found::Now ? L"found at the click" : f == Found::NoTaskbar ? L"no taskbar" : f == Found::Classic ? L"classic" : L"none";
}
Found Find(HMONITOR m) { Trace("find(" + MonName(m) + ")"); g_findAtUs = g_virtUs.load(); ++g_finds; g_virtUs += g_findTookUs.load(); return g_found; }
bool Kept(HMONITOR) { ++g_keptAsked; return false; }   // (the worker does not ask - Start is closed by the key)
bool Classic() { return false; }
std::wstring Describe() { return L""; }
Pressed Press(HMONITOR m, bool onlyWhenOff) {
    if (!onlyWhenOff) ++g_pressesAnyState;
    Pressed p = g_pressed;
    p.took = g_pressTook;
    int state = g_buttonState.load();
    if (state == kStateFollows) state = StartUp() || StartComing() ? 1 : 0;
    p.before = g_noToggle ? -1 : state;
    p.after = -1;
    if (onlyWhenOff && (g_noToggle || state != 0)) {   // not off, or no state to read: nothing is pressed
        TraceAt(("hold(" + MonName(m) + ")").c_str());
        g_holdAtUs = g_virtUs.load();
        ++g_holds;
        Advance(g_pressTook);   // (the read of the state)
        p.ok = p.unsure = false;
        p.held = true;
        p.hr = S_OK;
        p.how = g_noToggle ? L"no toggle state to read" : L"toggle";
        return p;
    }
    TraceAt(("press(" + MonName(m) + ")").c_str());
    g_pressAtUs = g_virtUs.load();
    ++g_presses;
    MaybeHang(g_hangPresses);
    Advance(g_pressTook);
    if (p.ok || p.unsure) ToggleStart(g_pressBrings, g_pressDelay);   // like a click on the taskbar's button: it toggles Start
    if (p.ok) p.after = state == 0 ? 1 : 0;
    // what comes of the press besides Start, for the click's lines: another window in front, Start's window uncloaked
    if (HWND w = g_frontAfterPress.load()) g_frontWnd = w;
    if (g_cloakAfterPress >= 0) g_startCloak = g_cloakAfterPress.load();
    return p;
}
}   // namespace taskbarstart

// ---- checks
#include "portable_check.h"
static const bool kFlushEachCheck = (portable_check::FlushEach() = true);
static size_t LogSize() { std::lock_guard<std::mutex> lk(g_logMu); return g_log.size(); }
static bool LogHas(size_t from, const wchar_t* text) {
    std::lock_guard<std::mutex> lk(g_logMu);
    for (size_t i = from; i < g_log.size(); ++i) if (g_log[i].find(text) != std::wstring::npos) return true;
    return false;
}
static bool WaitFor(const std::function<bool()>& done, double seconds = 20) {   // real seconds
    const ULONGLONG until = ::GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000);
    while (!done()) {
        if (::GetTickCount64() > until) return false;
        Sleep(1);
    }
    return true;
}
static bool WaitIdle() { return WaitFor([] { return g_current.load() == 0; }); }
// (IAppVisibility: one reference is InFront's, kept for the life of the process)
static bool NoLeak() { return g_shell.refs == 0 && g_launcher.refs == 0 && g_monitors.refs == 0 && g_where.refs == 0 && g_vis.refs <= 1; }

// the old verdict file (mod.18 to mod.35), where the dock kept it: the test's own reads and writes of it (not counted)
static std::wstring VerdictPath() { return std::wstring(g_dataDir) + L"\\taskbar-start.txt"; }
static std::string ReadVerdict() {
    std::string s;
    HANDLE f = CreateFileW(VerdictPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "(none)";
    char b[256]; DWORD got = 0;
    if (ReadFile(f, b, sizeof(b), &got, nullptr)) s.assign(b, got);
    CloseHandle(f);
    return s;
}
static void WriteVerdict(const std::string& text) {
    HANDLE f = CreateFileW(VerdictPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD put = 0;
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr); CloseHandle(f); }
}
static uint64_t VerdictWritten() {   // the file's last write (FILETIME; 0: no file)
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(VerdictPath().c_str(), GetFileExInfoStandard, &a)) return 0;
    return (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
}

// As the dock starts: Explorer up for days, a plain program in front, every fake answering as on a test PC where all
// of it works (the shell taking requests and bringing Start, the taskbar's button found, its state following Start, its press
// going through). Nothing is kept between clicks, so there is nothing of the button's to reset.
static void Fresh() {
    WaitIdle();
    g_ways = kWayShell | kWayTaskbar | kWayKey;
    // what the click's lines read: Start's window not known (none listed), the display not seen going off, every
    // window visible and not cloaked, no input since, the right had and handed on
    g_start = StartWindow{};
    g_startBusy = false;
    g_displayOnAt = -1;
    g_coreWindows = false; g_startCloak = -1; g_searchCloak = -1; g_frontCloak = 0; g_hiddenWnd = nullptr; g_firstClass[0] = 0;
    g_lastInputTick = 0; g_searchMon = nullptr; g_frontMon = nullptr; g_startAgeS = 23400;
    g_right = ForegroundRight::Had; g_allowRefused = false;
    g_frontAfterPress = nullptr; g_cloakAfterPress = -1; g_frontAfterShow = nullptr; g_frontWhenUp = nullptr;
    ResetEvent(g_unblock);
    g_hangShellMakes = 0; g_hangVisMakes = 0; g_hangPresses = 0; g_hangVisLooks = 0; g_hangWheres = 0;
    g_pauseAtUs = -1;
    g_dayShift = 0;
    g_tickBaseMs = 10LL * 86400 * 1000;
    g_explorerAgeS = 10LL * 86400;
    g_explorerUp = true;
    g_frontWnd = g_firstWnd;
    wcscpy_s(g_frontExe, L"C:\\Program Files\\Test\\notepad.exe");
    g_openRefused = false;
    g_blind = false; g_launcherSees = false;
    CloseStart();
    g_cancelled = 0;
    g_keys = 0; g_keyBrings = true; g_keyInserted = true; g_frontElevated = false;
    g_shows = 0; g_showHr = S_OK; g_showBrings = true; g_showDelay = 0.12; g_showTookUs = 0; g_connects = 0; g_services = 0;
    g_shellMade = 0; g_shellMadeWithoutExplorer = 0; g_shellMakeHr = S_OK; g_visMakeHr = S_OK; g_visMakeTook = 0; g_visMade = 0;
    g_finds = 0; g_presses = 0; g_holds = 0; g_found = taskbarstart::Found::Now;
    g_pressed = taskbarstart::Pressed{}; g_pressed.ok = true; g_pressed.hr = S_OK; g_pressed.how = L"toggle";
    g_pressBrings = true; g_pressDelay = 0.1; g_pressTook = 0.01;
    g_buttonState = kStateFollows; g_noToggle = false;
    g_whereHr = S_OK; g_whereNull = false; g_shellMakeTookUs = 0; g_findTookUs = 0;
    g_showAtUs = -1; g_findAtUs = -1; g_pressAtUs = -1; g_holdAtUs = -1; g_keyAtUs = -1;
}
static bool ClickOpen(HMONITOR m) { Click(m, false); return WaitIdle(); }

// ---- the trace of a scene, and its pins (the ladder of mod.36). Per pinned scene:
//   lines  - its dock.log lines, word for word;
//   calls  - its trace without the reads of the window in front: g_mu's locks, the clock reads (where the timeline
//            and the busy measures start and end), every call into the simulated Explorer, the looks, the sleeps, the
//            log lines, a request, press (hold: a press held back, the button's state not off) or key with its virtual
//            time (microseconds since the scene began), and what was still held as each thread left its apartment (with
//            the button's worker number then);
//   counts - the looks, the launcher's looks, the processes and tokens opened, the front reads, sleeps, clock reads,
//            g_mu's locks and the window reads of the click's dock.log lines ("win").
// A log line that reads more of the window in front (GetForegroundWindow, its process, its class, its cloak) changes
// counts only; one that reads the clock, takes g_mu, sleeps or calls Explorer changes calls too. Each scene starts at
// virtual time 0 (so 2.0 s and 4.0 s are exact to a look) with Windows up 3600 s and Explorer for days: its numbers do
// not depend on the scenes before it.
static std::string Utf8(const std::wstring& w) {
    char b[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    return b;
}
static size_t Begin(bool fresh = true) {
    if (fresh) Fresh(); else WaitIdle();
    CloseStart();
    g_tracing = false;
    g_virtUs = 0;
    g_sceneBaseUs = 0;
    g_tickBaseMs = 3600LL * 1000;   // Windows up 3600 s at the scene's start
    g_showAtUs = -1; g_findAtUs = -1; g_pressAtUs = -1; g_holdAtUs = -1; g_keyAtUs = -1;
    { std::lock_guard<std::mutex> lk(g_traceMu); g_trace.clear(); g_traceOff.clear(); g_traceMain.clear(); }
    g_tracing = true;
    return LogSize();
}
struct Traced { std::vector<std::string> all, off, main; };
static Traced EndTrace() {
    g_tracing = false;
    std::lock_guard<std::mutex> lk(g_traceMu);
    return Traced{ g_trace, g_traceOff, g_traceMain };
}
// a run of a repeated group of up to 8 tokens is written "(a b)x5" (one token: "ax5")
static std::string Compress(const std::vector<std::string>& t) {
    std::string out;
    for (size_t i = 0; i < t.size();) {
        size_t bestP = 1, bestK = 1;
        for (size_t p = 1; p <= 8 && i + 2 * p <= t.size(); ++p) {
            size_t k = 1;
            while (i + (k + 1) * p <= t.size() && std::equal(t.begin() + i, t.begin() + i + p, t.begin() + i + k * p)) ++k;
            if (k >= 2 && k * p > bestK * bestP) { bestP = p; bestK = k; }
        }
        if (!out.empty()) out += ' ';
        if (bestK == 1) { out += t[i]; ++i; continue; }
        if (bestP > 1) out += '(';
        for (size_t j = 0; j < bestP; ++j) { if (j) out += ' '; out += t[i + j]; }
        if (bestP > 1) out += ')';
        out += 'x' + std::to_string(bestK);
        i += bestP * bestK;
    }
    return out;
}
// "win": what the click's dock.log lines read of the windows and the input (user32, DWM) - reads of the window in front
// as the others, so they change counts only
static bool FrontRead(const std::string& x) { return x == "fg" || x == "open" || x == "token" || x == "win"; }
static std::string Calls(const std::vector<std::string>& t) {
    std::vector<std::string> c;
    for (const auto& x : t) if (!FrontRead(x)) c.push_back(x);
    return Compress(c);
}
static int Count(const std::vector<std::string>& t, const char* token) { return static_cast<int>(std::count(t.begin(), t.end(), std::string(token))); }
static std::string Counts(const std::vector<std::string>& t) {
    return std::to_string(Count(t, "look")) + " looks, " + std::to_string(Count(t, "isvis")) + " launcher looks, " +
           std::to_string(Count(t, "open")) + " processes, " + std::to_string(Count(t, "token")) + " tokens, " +
           std::to_string(Count(t, "fg")) + " front reads, " + std::to_string(Count(t, "sleep")) + " sleeps, " +
           std::to_string(Count(t, "now")) + " clock reads, " + std::to_string(Count(t, "lock")) + " locks, " +
           std::to_string(Count(t, "win")) + " window reads";
}
static std::string Held(const std::vector<std::string>& t) {   // what was held as each thread left its apartment
    std::string out;
    for (const auto& x : t) if (x.rfind("couninit", 0) == 0) out += (out.empty() ? "" : " ") + x;
    return out;
}
static std::vector<std::wstring> LinesSince(size_t from) {
    std::lock_guard<std::mutex> lk(g_logMu);
    return std::vector<std::wstring>(g_log.begin() + (std::min)(from, g_log.size()), g_log.end());
}
// the windows' handles differ from run to run: an expected line names them {np} (the window in front of most scenes,
// "notepad.exe hunt-start_menu-front <handle>"), {front} (its handle), {other}, {start}, {search}, {core} (the others'),
// and {adm} stands for " (administrator)" when the test itself runs as administrator (else for nothing)
static std::wstring Handle8(HWND w) {   // (as the lines write a handle: 8 hexadecimal digits)
    wchar_t b[16];
    _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"%08lX", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(w) & 0xFFFFFFFFu));
    return b;
}
static std::wstring Adm() { return SelfElevated() ? L" (administrator)" : L""; }   // the test's own token (not elevated, as a rule)
static std::wstring Sub(std::wstring s) {
    const std::pair<const wchar_t*, std::wstring> names[] = { { L"{np}", L"notepad.exe hunt-start_menu-front " + Handle8(g_firstWnd) },
        { L"{front}", Handle8(g_firstWnd) }, { L"{other}", Handle8(g_otherWnd) }, { L"{start}", Handle8(g_startCoreWnd) }, { L"{search}", Handle8(g_searchWnd) },
        { L"{core}", Handle8(g_otherCoreWnd) }, { L"{adm}", Adm() } };
    for (const auto& n : names)
        for (size_t at; (at = s.find(n.first)) != std::wstring::npos;) s.replace(at, wcslen(n.first), n.second);
    return s;
}
static void ExpectLines(size_t from, std::vector<std::wstring> want, const std::string& what) {
    for (auto& w : want) w = Sub(w);
    const std::vector<std::wstring> got = LinesSince(from);
    const bool ok = got == want;
    Check(ok, what.c_str());
    if (!ok) for (const auto& l : got) printf("  found line: %s\n", Utf8(l).c_str());
}
static void ExpectText(const std::string& got, const std::string& want, const std::string& what) {
    const bool ok = got == want;
    Check(ok, what.c_str());
    if (!ok) printf("  found: %s\n", got.c_str());
}
static const wchar_t kStartExe[] = L"C:\\Windows\\SystemApps\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\StartMenuExperienceHost.exe";
static const wchar_t kSearchExe[] = L"C:\\Windows\\SystemApps\\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\\SearchHost.exe";
static const wchar_t kChromeExe[] = L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
static const char kHeldNone[] = "couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]";

// One opening click (the UI thread's Click, then its worker: never both at once), pinned.
struct Pin { const char* name; std::vector<std::wstring> lines; const char* calls; const char* counts; const char* held = kHeldNone; };
static void ExpectClick(size_t log, const Traced& t, const Pin& p) {
    const std::string n = std::string("[char] ") + p.name;
    ExpectLines(log, p.lines, n + ": dock.log word for word");
    ExpectText(Calls(t.all), p.calls, n + ": its calls in order (g_mu, the clock, Explorer, looks, sleeps, log lines, when things went out)");
    ExpectText(Counts(t.all), p.counts, n + ": counted (looks, processes and tokens opened, front reads, sleeps, clock reads, g_mu's locks)");
    ExpectText(Held(t.all), p.held, n + ": what was held as each worker left its apartment (nothing of Explorer's but InFront's IAppVisibility)");
}
// One opening click on a dock as it starts (fresh; or not: the same run goes on), pinned: set - the fakes for it; the click
// on m and its worker to its end; ok - what went out and what came of it.
static void Scene(const Pin& p, HMONITOR m, const std::function<void()>& set, const std::function<bool()>& ok, const char* what, bool fresh = true) {
    const size_t log = Begin(fresh);
    if (set) set();
    const bool ended = ClickOpen(m);
    const Traced t = EndTrace();
    Check(ended && ok(), (std::string("[char] ") + p.name + ": " + what).c_str());
    ExpectClick(log, t, p);
}
// when something went out in the scene (seconds since it began): at the look that reached `s`
static bool At(const std::atomic<int64_t>& us, double s) { const double t = us.load() / 1e6; return us.load() >= 0 && t >= s - 1e-9 && t < s + 0.07; }
static bool Sent(int shows, int presses, int keys) { return g_shows == shows && g_presses == presses && g_keys == keys; }
static double NowScene() { return g_virtUs.load() / 1e6; }

// ---- the ladder, behaviour first: what goes out, when, and what dock.log says of it (the pins come after)
static void LadderTests(HMONITOR primary) {
    size_t log = 0;

    // ---- the usual click: the shell asked once, Start comes by it
    log = Begin();
    Check(ClickOpen(primary), "a click's worker ends by itself");
    Check(Sent(1, 0, 0) && StartUp(), "the shell's request brings Start: asked once, nothing else sent");
    Check(g_finds == 0 && g_holds == 0, "...and the taskbar's button is not even looked for (its turn did not come)");
    Check(g_shellMade == 1 && g_connects == 1, "...the shell reached once, its launcher put on the dock's monitor");
    Check(LogHas(log, L"start: Start came by the shell's request in 120 ms"), "dock.log names the way Start came, and how long it took");
    Check(g_rightsGiven >= 1, "an opening click hands the right to come in front to the shell");
    Check(NoLeak(), "no reference to the shell's objects is left over");

    // ---- Start open (its search box in front, as on 25H2): the Windows key closes it, on the UI thread - no thread, no press
    wcscpy_s(g_frontExe, kSearchExe);
    Check(InFront(), "Start up: the press-time check sees it (IAppVisibility)");
    {
        const int done = g_threadsDone, finds = g_finds;
        log = LogSize();
        Click(primary, true);
        Check(g_keys == 1 && !StartUp() && g_presses == 0 && g_holds == 0 && g_finds == finds && g_current.load() == 0 && g_threadsDone == done,
              "a click with Start open: the Windows key closes it at once - no worker, no thread, the taskbar's button neither looked for nor pressed");
        Check(LogHas(log, L"start: closing Start (the start button pressed while it was open; in front: SearchHost.exe) by the Windows key"),
              "...and dock.log says Start was closed, how, and what was in front");
    }

    // ---- every usual click has its line, not the run's first only
    Fresh();
    log = LogSize();
    ClickOpen(primary); CloseStart();
    ClickOpen(primary); CloseStart();
    ClickOpen(primary); CloseStart();
    {
        int usual = 0;
        for (const auto& l : LinesSince(log)) usual += l.rfind(L"start: Start came by the shell's request in ", 0) == 0;
        Check(usual == 3 && g_shows == 3 && g_presses == 0 && g_keys == 0, "three usual clicks: three usual lines, one request each");
    }

    // ---- 2026-11-20 the other way round: the shell takes the request and Start does not come - kConfirm later the taskbar's
    // button, its state off, is pressed and brings it
    log = Begin();
    g_showBrings = false;
    Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 0), "the shell takes the request and brings nothing: the taskbar's button brings Start - one request, one press, no key");
    Check(At(g_pressAtUs, kConfirm) && g_finds == 1 && At(g_findAtUs, kConfirm), "...the button looked for and pressed kConfirm after the shell took the request, not before");
    Check(NowScene() < kConfirm + 0.3, "...Start is there a little over kConfirm after the click (not kWaitAccepted)");
    Check(LogHas(log, L"start: Start came after 2.1 s, by the taskbar's Start button - taskbar: pressed (toggle, state off -> on; the button found at the click); "
                      L"shell: asked 1 time, accepted (00000000); Windows key: not pressed"),
          "...dock.log tells each way: the press with the button's state around it, the request the shell took, no key");
    Check(g_pressesAnyState == 0, "...the worker presses the button only through 'only when off'");

    // ---- the button's state reads on: left alone (a press would close the Start Explorer holds open); the key at kGiveUp
    log = Begin();
    g_showBrings = false;
    g_buttonState = 1;
    Check(ClickOpen(primary) && StartUp() && Sent(1, 0, 1) && g_holds == 1, "the button's state reads on: not pressed; the Windows key brings Start");
    Check(At(g_holdAtUs, kConfirm) && At(g_keyAtUs, kGiveUp), "...its turn at kConfirm, the key at kGiveUp after the shell took the request - not brought forward");
    Check(LogHas(log, L"taskbar: left alone at 2.0 s: its toggle state was on - Explorer holds Start open, and a press would close it (the button found at the click)"),
          "...and dock.log says why the button was left alone, and when");

    // ---- no button to press (not in the tree; no taskbar on the monitor; Windows' classic taskbar): the key waits for kGiveUp
    for (taskbarstart::Found none : { taskbarstart::Found::NoButton, taskbarstart::Found::NoTaskbar, taskbarstart::Found::Classic }) {
        log = Begin();
        g_showBrings = false;
        g_found = none;
        Check(ClickOpen(primary) && StartUp() && Sent(1, 0, 1) && g_holds == 0 && g_finds == 1 && At(g_findAtUs, kConfirm) && At(g_keyAtUs, kGiveUp),
              "no button to press: looked for at kConfirm, and the key at kGiveUp after the shell took the request, not at kConfirm (it toggles: mod.15's rule)");
        Check(LogHas(log, (std::wstring(L"taskbar: none to press (") + taskbarstart::FoundText(none) + L"); shell: asked 1 time, accepted (00000000); Windows key: pressed").c_str()),
              "...dock.log says there was none, and why");
    }

    // ---- the press does not go through: the key still at kGiveUp
    log = Begin();
    g_showBrings = false;
    g_pressed.ok = false; g_pressed.hr = E_FAIL;
    Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 1) && At(g_pressAtUs, kConfirm) && At(g_keyAtUs, kGiveUp),
          "a press that did not go through, behind a request the shell took: the key at kGiveUp, not sooner");
    Check(LogHas(log, L"taskbar: the press did not go through: toggle (80004005)"), "...and dock.log says so");

    // ---- the press goes through and brings nothing: the key kConfirm after it
    log = Begin();
    g_showBrings = false; g_pressBrings = false;
    Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 1) && g_keyAtUs.load() - g_pressAtUs.load() >= static_cast<int64_t>(kConfirm * 1e6) && At(g_keyAtUs, kGiveUp),
          "the shell and the button both bring nothing: the key at kGiveUp, kConfirm after the press - Start on the first click");
    Check(LogHas(log, L"Start came after 4.1 s, by the Windows key"), "...dock.log: by the Windows key, about kGiveUp in");

    // ---- nothing brings Start: each way once, given up kWaitAccepted after the shell took the request
    log = Begin();
    g_showBrings = false; g_pressBrings = false; g_keyBrings = false;
    Check(ClickOpen(primary) && !StartUp() && Sent(1, 1, 1) && NowScene() >= kWaitAccepted && NowScene() < kWaitAccepted + 0.1,
          "nothing brings Start: the shell, the button, the key - each once - and the dock gives up kWaitAccepted after the shell took the request");
    Check(LogHas(log, L"start: Start did not come in 8.0 s - taskbar: pressed (toggle, state off -> on; the button found at the click); "
                      L"shell: asked 1 time, accepted (00000000); Windows key: pressed; in front: notepad.exe"),
          "...and dock.log tells what went out");

    // ---- the press timed out: it may still arrive, so nothing else is sent
    log = Begin();
    g_showBrings = false; g_pressBrings = false;
    g_pressed.ok = false; g_pressed.unsure = true; g_pressed.hr = static_cast<HRESULT>(0x80131505);
    Check(ClickOpen(primary), "a press that timed out: the worker ends");
    Check(!StartUp() && Sent(1, 1, 0), "...no key after it (a late press plus the key would toggle Start twice)");
    Check(NowScene() >= kWaitAccepted && NowScene() < kWaitAccepted + 0.1, "...waited on until kWaitAccepted after the shell took the request");
    Check(LogHas(log, L"unsure whether it arrived (80131505): waited on, nothing else sent") &&
          LogHas(log, L"Windows key: not pressed (the taskbar's button took the press; the key would close a Start on its way)"), "...and dock.log says so");

    // ---- slow: minutes after Windows or Explorer started, the request the shell took is waited on alone for kWaitAccepted
    log = Begin();
    g_explorerAgeS = 60;   // Windows up for an hour (fast startup: its uptime runs on), Explorer started a minute ago - a logon
    g_showBrings = false;
    Check(ClickOpen(primary) && !StartUp() && Sent(1, 0, 0) && g_finds == 0 && g_holds == 0,
          "Explorer started a minute ago, the shell takes the request, Start does not come: nothing else is sent, the button not even looked for");
    Check(NowScene() >= kWaitAccepted && NowScene() < kWaitAccepted + 0.1, "...the dock waits kWaitAccepted, then logs it");
    Check(LogHas(log, L"taskbar: not pressed (slow: the shell's request is waited on alone); shell: asked 1 time, accepted (00000000); "
                      L"Windows key: not pressed (the shell took the request; the key would close a Start on its way)") &&
          LogHas(log, L"; slow - Windows up 3600 s, Explorer 60 s: started minutes ago"), "...dock.log says the click was slow, and why (Windows' uptime, Explorer's age)");
    log = Begin();
    g_explorerAgeS = 60;
    g_showDelay = 5.0;
    Check(ClickOpen(primary) && StartUp() && Sent(1, 0, 0) && g_cancelled == 0 && g_finds == 0,
          "...a Start that takes 5 s then comes by the shell alone, nothing toggles it shut");
    log = Begin();
    g_tickBaseMs = 60 * 1000;   // Windows up one minute
    g_showBrings = false;
    Check(ClickOpen(primary) && !StartUp() && Sent(1, 0, 0) && g_finds == 0 && LogHas(log, L"; slow - Windows up 60 s, Explorer 864000 s: started minutes ago"),
          "Windows started a minute ago: the same - waited on alone");
    log = Begin();
    g_tickBaseMs = 60 * 1000;
    g_explorerAgeS = -1;
    g_showBrings = false;
    Check(ClickOpen(primary) && Sent(1, 0, 0) && LogHas(log, L"; slow - Windows up 60 s, Explorer's age unknown: started minutes ago") && !LogHas(log, L"Explorer -1"),
          "...Explorer's age not to be had, Windows up one minute: slow, and dock.log says the age is unknown");
    log = Begin();
    g_explorerAgeS = -1;   // Windows up an hour (Begin), Explorer's age not to be had: Windows' uptime alone - not slow
    g_showBrings = false;
    Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 0) && !LogHas(log, L"slow"), "Explorer's age not to be had, Windows up an hour: not slow - the button at kConfirm");
    {   // the bounds, exact: kEarlyUptime of Windows' uptime and of Explorer's age
        struct { int64_t upMs, explorer; bool slow; const char* what; } early[] = {
            { 299999, 864000, true, "Windows up 299.999 s: slow" }, { 300000, 864000, false, "Windows up 300 s (kEarlyUptime): not slow" },
            { 3600000, 299, true, "Explorer up 299 s: slow" }, { 3600000, 300, false, "Explorer up 300 s (kEarlyUptime): not slow" } };
        for (const auto& e : early) {
            log = Begin();
            g_tickBaseMs = e.upMs; g_explorerAgeS = e.explorer;
            g_showBrings = false;
            Check(ClickOpen(primary) && (e.slow ? Sent(1, 0, 0) && !StartUp() : Sent(1, 1, 0) && StartUp()) && LogHas(log, L"; slow - ") == e.slow, e.what);
        }
    }

    // ---- slow: a single call into Explorer took kBusyCall on this click - each of them, exact to the microsecond
    {
        struct Busy { const char* what; const wchar_t* line; void (*set)(int64_t us); } calls[] = {
            { "reaching the shell", L"; slow - reaching the shell took 250 ms: Explorer busy", [](int64_t us) { g_shellMakeTookUs = us; } },
            { "making IAppVisibility", L"; slow - making IAppVisibility took 250 ms: Explorer busy", [](int64_t us) { g_visMakeTook = us / 1e6; } },
            { "the shell's request", L"; slow - the shell's request took 250 ms: Explorer busy", [](int64_t us) { g_showTookUs = us; } },
            { "finding the taskbar's button", L"; slow - finding the taskbar's button took 250 ms: Explorer busy", [](int64_t us) { g_findTookUs = us; } },
        };
        for (const Busy& c : calls) {
            log = Begin();
            c.set(250000);
            g_showBrings = false;
            const bool ended = ClickOpen(primary);
            Check(ended && !StartUp() && Sent(1, 0, 0) && g_holds == 0 && LogHas(log, c.line),
                  (std::string(c.what) + " took kBusyCall (0.25 s): Explorer is busy - the request the shell took is waited on alone, no press, no key; dock.log names the call").c_str());
            log = Begin();
            c.set(249999);
            g_showBrings = false;
            Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 0) && !LogHas(log, L"; slow - "),
                  (std::string(c.what) + " took just under kBusyCall: not slow - the button pressed at kConfirm").c_str());
        }
        // the press itself: nothing after it
        log = Begin();
        g_showBrings = false; g_pressBrings = false;
        g_pressTook = 0.25;
        Check(ClickOpen(primary) && !StartUp() && Sent(1, 1, 0) && LogHas(log, L"; slow - pressing the taskbar's button took 250 ms: Explorer busy") &&
              LogHas(log, L"taskbar: pressed (toggle, state off -> on; the button found at the click), slow: waited on alone"),
              "the press itself took kBusyCall: no key after it (it would close a Start on its way), and dock.log says so");
        log = Begin();
        g_showBrings = false; g_pressBrings = false;
        g_pressTook = std::nextafter(0.25, 0.0);
        Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 1) && !LogHas(log, L"; slow - "), "the press took just under kBusyCall: not slow - the key kConfirm after it");
        // the read of the button's state (the button left alone): the same
        log = Begin();
        g_showBrings = false;
        g_buttonState = 1;
        g_pressTook = 0.25;
        Check(ClickOpen(primary) && !StartUp() && Sent(1, 0, 0) && g_holds == 1 && LogHas(log, L"; slow - reading the taskbar button's state took 250 ms: Explorer busy"),
              "reading the button's state took kBusyCall (the button left alone, on): slow - no key");
    }
    {   // slow, and the shell did not take a request: nothing is on its way, the button at once - and its press waited on alone
        log = Begin();
        g_explorerAgeS = 60;
        g_showHr = E_FAIL;
        g_pressBrings = false;
        Check(ClickOpen(primary) && !StartUp() && Sent(1, 1, 0) && At(g_pressAtUs, 0) && NowScene() >= kWaitAccepted && NowScene() < kWaitAccepted + 0.1,
              "slow, the shell refuses: the button at once (nothing is on its way), and that press is waited on alone for kWaitAccepted - no key");
        // ...the lookup of the button itself slow: behind a refused request it is pressed all the same (nothing is on its
        // way to wait for) - and then waited on alone
        log = Begin();
        g_showHr = E_FAIL;
        g_findTookUs = 250000;
        g_pressBrings = false;
        Check(ClickOpen(primary) && !StartUp() && Sent(1, 1, 0) && g_finds == 1 && At(g_pressAtUs, 0.25) && LogHas(log, L"; slow - finding the taskbar's button took 250 ms: Explorer busy") &&
              LogHas(log, L"pressed (toggle, state off -> on; the button found at the click), slow: waited on alone"),
              "the shell refuses, finding the button took kBusyCall: pressed all the same (looked for once), then waited on alone - no key");
    }

    // ---- no taskbar button at all (Windows 10's taskbar, none on the monitor): the usual click is the same
    log = Begin();
    g_found = taskbarstart::Found::NoButton;
    Check(ClickOpen(primary) && Sent(1, 0, 0) && StartUp() && g_finds == 0, "no taskbar button: one request to the shell brings Start, no key");
    Check(LogHas(log, L"Start came by the shell's request"), "...and dock.log names the shell's request");
    Check(NoLeak(), "...no reference left over");

    // ---- the shell does not take a request: nothing is on its way - the next way at once
    log = Begin();
    g_shellMakeHr = CO_E_SERVER_EXEC_FAILURE;
    Check(ClickOpen(primary) && StartUp() && Sent(0, 1, 0) && At(g_pressAtUs, 0) && g_shellMade == 1,
          "the shell cannot be had (its interface gone on a new Windows): the taskbar's button at once brings Start");
    Check(LogHas(log, L"by the taskbar's Start button - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: not available (80080005)"),
          "...and dock.log says the shell was not available, with the error");
    log = Begin();
    g_showHr = E_FAIL;
    Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 0) && At(g_pressAtUs, 0) && LogHas(log, L"shell: asked 1 time, refused (80004005)"),
          "the shell refuses the request: the taskbar's button at once; the request is not repeated");
    log = Begin();
    g_shellMakeHr = CO_E_SERVER_EXEC_FAILURE;
    g_found = taskbarstart::Found::NoButton;
    Check(ClickOpen(primary) && StartUp() && Sent(0, 0, 1) && At(g_keyAtUs, 0), "no shell and no button: the Windows key at once, as before");
    log = Begin();
    g_shellMakeHr = CO_E_SERVER_EXEC_FAILURE;
    g_buttonState = 1;
    Check(ClickOpen(primary) && StartUp() && Sent(0, 0, 1) && g_holds == 1 && At(g_keyAtUs, 0), "no shell, the button's state on: left alone, the key at once");
    log = Begin();
    g_shellMakeHr = CO_E_SERVER_EXEC_FAILURE;
    g_pressBrings = false;
    Check(ClickOpen(primary) && StartUp() && Sent(0, 1, 1) && At(g_pressAtUs, 0) && At(g_keyAtUs, kConfirm),
          "no shell, the press goes through and brings nothing: the key kConfirm after the press");
    log = Begin();
    g_shellMakeHr = CO_E_SERVER_EXEC_FAILURE;
    g_pressed.ok = false; g_pressed.hr = E_FAIL;
    Check(ClickOpen(primary) && g_shellMade == 1 && Sent(0, 1, 1) && At(g_keyAtUs, 0) && StartUp(), "the press did not go through and the shell cannot be had: the Windows key");

    // ---- Explorer not up (restarting): nothing of it is touched, the key goes out once
    log = Begin();
    g_explorerUp = false;
    g_keyBrings = false;   // so the click logs what it saw
    Check(ClickOpen(primary), "Explorer not up: the worker ends");
    Check(g_finds == 0 && g_presses == 0 && g_holds == 0 && g_shellMade == 0 && g_keys == 1 && At(g_keyAtUs, 0), "...without looking for the taskbar's button or reaching the shell; one key");
    Check(NowScene() >= kGiveUp && NowScene() < kGiveUp + 0.1, "...given up kGiveUp after the key");
    Check(LogHas(log, L"taskbar: Explorer is not up") && LogHas(log, L"shell: not available (80070426)"), "...and dock.log says why");

    // ---- a program that runs as administrator in front (the dock does not): no key at any time - the shell and the button are no keys
    if (SelfElevated()) {
        for (int i = 0; i < 3; ++i) Check(true, "administrator in front: not run (the test itself runs as administrator)");
    } else {
        log = Begin();
        g_frontElevated = true;
        g_showBrings = false; g_pressBrings = false;
        Check(ClickOpen(primary) && !StartUp() && Sent(1, 1, 0) && NowScene() >= kWaitAccepted && NowScene() < kWaitAccepted + 0.1,
              "an administrator's window in front, nothing brings Start: the shell and the button, never the key");
        Check(LogHas(log, L"Windows key: not pressed: notepad.exe runs as administrator and Windows drops keys sent to it; in front: notepad.exe (administrator)"),
              "...and dock.log says why no key");
        log = Begin();
        g_frontElevated = true;
        g_showBrings = false;
        Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 0), "...the button's press, no injected key, brings Start past the administrator's window");
    }

    // ---- Start closed with the button while a wait runs: the wait ends, nothing more is sent
    log = Begin();
    g_showBrings = false;   // the request brings nothing; Start is opened meanwhile by the user's own key
    g_pauseAtUs = 300000;
    Click(primary, false);
    WaitForSingleObject(g_paused, 20000);
    BringStart(0); Advance(0.001);
    wcscpy_s(g_frontExe, kSearchExe);
    Click(primary, true);
    SetEvent(g_resume);
    Check(WaitIdle() && Sent(1, 0, 1) && !StartUp() && g_finds == 0, "Start closed while a wait runs: the wait ends at once, nothing sent after the key that closed it");

    // ---- more clicks while waiting are counted, nothing is sent for them: the ladder goes on as without them
    log = Begin();
    g_showBrings = false;
    g_pauseAtUs = 300000;
    Click(primary, false);
    for (int i = 0; i < 4; ++i) {
        WaitForSingleObject(g_paused, 20000);
        Click(primary, false);   // joins the wait
        if (i + 1 < 4) g_pauseAtUs = g_virtUs.load() + 500000;
        SetEvent(g_resume);
    }
    Check(WaitIdle() && StartUp() && Sent(1, 1, 0) && At(g_pressAtUs, kConfirm), "four more clicks while the shell's request is waited on: the same press at kConfirm, nothing sent for them");
    Check(LogHas(log, L"4 more clicks while waiting (at 0.3, 0.8, 1.3, 1.8 s)"), "...dock.log counts them, with their times");

    // ---- the ways taken away (the tests' switch): each is left alone, and dock.log says 'off'
    log = Begin();
    g_ways = kWayKey;   // the worker test's safe mode: no shell, no taskbar button
    g_keyBrings = false;
    Check(ClickOpen(primary) && Sent(0, 0, 1) && g_shellMade == 0 && g_finds == 0 && LogHas(log, L"taskbar: off; shell: not available (80004004); Windows key: pressed"),
          "the shell and the button taken away: neither is touched, the key goes out");
    log = Begin();
    g_ways = kWayTaskbar;
    Check(ClickOpen(primary) && StartUp() && Sent(0, 1, 0) && g_shellMade == 0, "the button alone: pressed at once, the shell not reached, no key");
    log = Begin();
    g_ways = kWayShell;
    g_showBrings = false;
    Check(ClickOpen(primary) && !StartUp() && Sent(1, 0, 0) && g_finds == 0 && LogHas(log, L"taskbar: off; shell: asked 1 time, accepted (00000000); Windows key: off"),
          "the shell alone: asked once, neither the button nor the key");
}

// ---- the old verdict file data\taskbar-start.txt (mod.18 to mod.35: "<works|failed> <Windows build> <day>"): left as it is,
// whatever it says - neither read, written nor removed, and nothing of it decides anything. (A version put back reads it again.)
static void VerdictFileTests(HMONITOR primary) {
    for (const char* kept : { "works 26100.1742 155552", "failed 26100.1742 155552", "(none)" }) {
        const bool none = !strcmp(kept, "(none)");
        if (none) DeleteFileW(VerdictPath().c_str()); else WriteVerdict(kept);
        const uint64_t written = VerdictWritten();
        const int files = g_fileCalls, asked = g_dataDirAsked;
        const std::string k = none ? std::string("no verdict file") : std::string("the old verdict file says \"") + kept + "\"";
        size_t log = Begin();
        const bool usual = ClickOpen(primary) && StartUp() && Sent(1, 0, 0) && g_finds == 0;
        Check(usual, (k + ": the usual click asks the shell, as with any other file (the button neither trusted nor left alone)").c_str());
        wcscpy_s(g_frontExe, kSearchExe);
        Click(primary, true);
        Check(g_keys == 1 && g_presses == 0 && !StartUp(), (k + ": Start is closed by the Windows key").c_str());
        Begin();
        g_showBrings = false;
        const bool ladder = ClickOpen(primary) && StartUp() && Sent(1, 1, 0) && At(g_pressAtUs, kConfirm);
        Check(ladder, (k + ": the shell bringing nothing, the button is pressed at kConfirm").c_str());
        Begin();
        g_showBrings = false; g_pressBrings = false; g_keyBrings = false;
        const bool dud = ClickOpen(primary) && !StartUp() && Sent(1, 1, 1);
        Begin();
        const bool again = ClickOpen(primary) && StartUp() && Sent(1, 0, 0);
        Check(dud && again, (k + ": a click that brought nothing changes nothing for the next one (nothing is learned)").c_str());
        Check(g_fileCalls == files && g_dataDirAsked == asked, (k + ": no file or registry call made, the data folder not even asked for").c_str());
        Check(ReadVerdict() == kept && VerdictWritten() == written, (k + ": the file is as it was - not written, not removed, not made").c_str());
        Check(!LogHas(log, L"on this Windows build") && !LogHas(log, L"left alone ") && !LogHas(log, L"from now on") && !LogHas(log, L"in a row"),
              (k + ": no line about a verdict, a lesson or a button left alone for days").c_str());
    }
    DeleteFileW(VerdictPath().c_str());
}

// ---- a worker stuck on a hung Explorer is left behind by the next click (mod.23); once Explorer answers again it must
// send nothing more: the new click's request is on its way (a second request stops it - seen on a test PC, mod.17)
static void LeftBehindTests(HMONITOR primary) {
    size_t log = 0;
    Fresh();
    g_showDelay = 1.0;                         // Explorer slow: Start comes a second after the request
    g_hangShellMakes = 1;                      // the first click's reach of the shell hangs
    const int doneBefore = g_threadsDone;
    log = LogSize();
    Click(primary, false);
    WaitFor([] { return g_shellMade.load() >= 1; });
    Advance(kStuck + 1);                       // the user waits, then clicks again
    g_pauseAtUs = g_virtUs.load();             // the new worker's first look after its request waits for the test
    Click(primary, false);
    Check(LogHas(log, L"starting over"), "a click after kStuck starts over");
    WaitForSingleObject(g_paused, 20000);      // the new click's request went out: Start is on its way (a second)
    Check(g_shows == 1, "...and its worker asks the shell");
    SetEvent(g_unblock);                       // Explorer answers the first click's worker again
    Check(WaitFor([&] { return g_threadsDone.load() >= doneBefore + 1; }), "...the worker left behind ends");
    SetEvent(g_resume);
    Check(WaitFor([&] { return g_threadsDone.load() >= doneBefore + 2; }), "...and so does the new one");
    Check(g_shows == 1 && g_cancelled == 0, "...and the worker left behind sent nothing once a later click took over (no second request to stop Start on its way)");
    Check(StartUp(), "...so Start comes");
    Check(LogHas(log, L"left behind on a hung Explorer got an answer again") && LogHas(log, L"by the shell's request"),
          "...dock.log says the one left behind sent nothing more; the new one reports as usual");
    Check(g_connects == 1 && g_services == 2 && g_visMade == 1,
          "...and it asks Explorer nothing more: no service, no IAppVisibility, Explorer's one launcher not moved to its monitor while the new click's Start comes");
    Check(NoLeak(), "...no reference left over");

    {   // as it would be: Explorer hangs both clicks' workers (reaching the shell) and answers both at once
        Fresh();
        g_showDelay = 1.0;
        g_hangShellMakes = 2;
        const int d0 = g_threadsDone;
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 2; });
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_shows == 1 && g_cancelled == 0 && g_keys == 0 && g_presses == 0 && StartUp(),
              "both clicks' workers hung on Explorer, answered at once: one request to the shell in all, and Start comes");
        Check(g_connects == 1 && g_services == 2 && g_visMade == 1, "...the one left behind asks Explorer nothing more (the launcher connected once)");
        Check(NoLeak(), "...no reference left over");
    }
    {   // Explorer hangs the first click's worker later on its way to the shell (finding the dock's monitor): once answered,
        // it does not move Explorer's one launcher to its monitor - the later click's Start came there meanwhile
        Fresh();
        g_hangWheres = 1;
        const int d0 = g_threadsDone;
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        Click(primary, false);
        WaitFor([] { return g_shows.load() >= 1; });   // the new click's request went out
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_shows == 1 && g_keys == 0 && StartUp(),
              "a worker hung finding the dock's monitor for the shell, answered after a later click's request: one request in all, Start comes");
        Check(g_connects == 1 && g_visMade == 1, "...and the launcher is connected to a monitor once, by the later click (none by the one left behind)");
        Check(NoLeak(), "...no reference left over");
    }
    {   // both workers hung on their first look (IsLauncherVisible), answered at once: the one left behind does not ask the
        // shell after that look
        Fresh();
        g_hangVisLooks = 2;
        const int d0 = g_threadsDone;
        log = LogSize();
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 2; });
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_shows == 1 && g_presses == 0 && g_keys == 0 && g_cancelled == 0 && StartUp(),
              "both workers hung looking whether Start is up, answered at once: the shell asked once (a second request would stop Start on its way)");
        Check(LogHas(log, L"nothing more sent, nothing learned (a later click started over; sent before: the taskbar's button 0, the shell 0, the Windows key 0)"),
              "...and the one left behind says it sent nothing");
        Check(NoLeak(), "...no reference left over");
    }
    {   // the same, both hung making IAppVisibility
        Fresh();
        g_hangVisMakes = 2;
        const int d0 = g_threadsDone;
        log = LogSize();
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 2; });
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_shows == 1 && g_presses == 0 && g_keys == 0 && g_cancelled == 0 && StartUp(),
              "both workers hung making IAppVisibility and answered at once: the shell asked once");
        Check(LogHas(log, L"left behind on a hung Explorer"), "...and the one left behind says so");
        Check(NoLeak(), "...no reference left over");
    }
    {   // Explorer answers the one left behind only once the later click is over and Start was closed: it stays closed
        Fresh();
        g_hangShellMakes = 1;
        const int d0 = g_threadsDone;
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        Check(ClickOpen(primary) && g_shows == 1 && StartUp(), "a click after kStuck, Explorer answering it: Start comes");
        CloseStart();   // the user closes it
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_shows == 1 && g_keys == 0 && g_presses == 0 && !StartUp(),
              "...the worker left behind, answered after that, does not bring Start back");
        Check(NoLeak(), "...no reference left over");
    }
    {   // hung in the ladder's second step - the press, kConfirm after its request - and left behind: answered, it sends
        // no key at kGiveUp (the later click's Start is up, and the key would close it)
        Fresh();
        g_showBrings = false;
        g_pressBrings = false;
        g_hangPresses = 1;
        const int d0 = g_threadsDone;
        log = LogSize();
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });   // its press, 2 s in, hangs on Explorer
        Advance(kStuck + 1);
        g_showBrings = true;
        Check(ClickOpen(primary) && g_shows == 2 && StartUp(), "a worker hung pressing the taskbar's button; a click after kStuck starts over and its request brings Start");
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_keys == 0 && g_presses == 1 && g_shows == 2,
              "...the one left behind, its press answered at last, sends no key after it");
        Check(LogHas(log, L"nothing more sent, nothing learned (a later click started over; sent before: the taskbar's button 1, the shell 1, the Windows key 0)"),
              "...and says what it had sent before");
        Check(NoLeak(), "...no reference left over");
    }
}

// The window in front, as the worker's looks read it (Visible, every kLook while it waits): a window's program never
// changes, so only a window that came in front since the last look has its program looked into; whether a program runs
// as administrator is asked once per click (can the Windows key reach Start), not at each look nor at the press-time
// checks on the UI thread. Counted: the looks, the launcher's looks (none - IAppVisibility and the window in front
// tell), the processes and tokens opened (and one process more a click, Explorer's, for its age - no call into it).
static void FrontTests(HMONITOR primary) {
    SelfElevated();   // its one look at the test's own token (once per process; a test run as administrator) is not counted below
    // ---- a long wait: the shell took the request, Start does not come, the click slow - kWaitAccepted of looks
    Begin();
    g_explorerAgeS = 60;
    g_showBrings = false;
    int looks = g_looks, opened = g_processesOpened, tokens = g_tokensOpened, launcher = g_launcherLooks;
    Check(ClickOpen(primary) && !StartUp() && Sent(1, 0, 0), "a wait of kWaitAccepted on a request the shell took (slow): nothing comes, nothing else sent");
    looks = g_looks - looks; opened = g_processesOpened - opened; tokens = g_tokensOpened - tokens; launcher = g_launcherLooks - launcher;
    printf("  a wait of %.0f s: %d looks, %d launcher looks, %d processes opened, %d tokens\n", kWaitAccepted, looks, launcher, opened, tokens);
    Check(looks > 200 && opened == 3 && tokens == 1,
          "...the program in front looked into at the click and at the wait's first look, not at each look; whether it runs as administrator asked once; Explorer's process once, for its age");
    Check(launcher == 0, "...and the launcher is never asked whether Start is up (ILauncher::IsVisible, undocumented): IAppVisibility and the window in front tell");
    Check(looks == 269 && opened == 3 && tokens == 1, "[char] ...exactly 269 looks in that wait of kWaitAccepted, 3 processes opened, 1 token");

    // ---- the whole ladder (nothing brings Start: the request, the press, the key): the wait's looks and two more - the
    // worker looks again right after a way went out (the press, the key), as it always did
    Begin();
    g_showBrings = false; g_pressBrings = false; g_keyBrings = false;
    looks = g_looks; opened = g_processesOpened; tokens = g_tokensOpened; launcher = g_launcherLooks;
    Check(ClickOpen(primary) && !StartUp() && Sent(1, 1, 1), "the whole ladder, nothing coming: each way once");
    looks = g_looks - looks; opened = g_processesOpened - opened; tokens = g_tokensOpened - tokens; launcher = g_launcherLooks - launcher;
    printf("  the whole ladder: %d looks, %d launcher looks, %d processes opened, %d tokens\n", looks, launcher, opened, tokens);
    Check(looks == 271 && launcher == 0 && opened == 3 && tokens == 1, "[char] ...exactly 271 looks, no launcher look, 3 processes opened, 1 token");

    // ---- the usual click: Start comes by the request after a few looks
    Begin();
    looks = g_looks; opened = g_processesOpened; tokens = g_tokensOpened; launcher = g_launcherLooks;
    Check(ClickOpen(primary) && StartUp() && g_shows == 1, "the usual click: Start comes by the shell's request");
    looks = g_looks - looks; opened = g_processesOpened - opened; tokens = g_tokensOpened - tokens; launcher = g_launcherLooks - launcher;
    printf("  the usual click: %d looks, %d launcher looks, %d processes opened, %d tokens\n", looks, launcher, opened, tokens);
    Check(opened <= 3 && tokens == 1, "...the program in front looked into at most twice, whether it runs as administrator once, Explorer's process once");
    Check(looks == 6 && launcher == 0 && opened == 3 && tokens == 1, "[char] ...exactly 6 looks, no launcher look, 3 processes opened, 1 token");

    // ---- no IAppVisibility to be had for the click: then, and only then, the launcher is asked at each look
    Begin();
    g_visMakeHr = E_NOINTERFACE;
    looks = g_looks; launcher = g_launcherLooks;
    size_t log = LogSize();
    g_blind = true; g_launcherSees = true;
    Check(ClickOpen(primary) && StartUp() && Sent(1, 0, 0), "no IAppVisibility to be had: the click goes on, Start comes by the request");
    looks = g_looks - looks; launcher = g_launcherLooks - launcher;
    Check(looks == 0 && launcher == 6 && LogHas(log, L"seen by the launcher"), "[char] ...the launcher asked at each of the 6 looks instead (no IAppVisibility look), and dock.log says it saw Start");
    Check(NoLeak(), "...no reference left over");

    // ---- the press-time checks on the UI thread: the program in front, never its token
    Fresh();
    tokens = g_tokensOpened;
    CloseStart();
    Check(!InFront(), "Start not up: the press-time check says so");
    g_blind = true;   // IAppVisibility says nothing: Start's own window in front tells
    g_frontWnd = g_otherWnd;
    wcscpy_s(g_frontExe, kStartExe);
    Check(InFront(), "...Start's own window in front, IAppVisibility saying nothing: it sees Start");
    Check(g_tokensOpened == tokens, "...and asks nothing about administrator");

    // ---- another window comes in front during a wait: it is looked into (Start's, IAppVisibility saying nothing)
    Begin();
    g_blind = true;
    g_showBrings = false;   // the shell takes the request; Start's window comes in front 0.3 s later
    g_pauseAtUs = 300000;
    looks = g_looks; opened = g_processesOpened; tokens = g_tokensOpened;
    Click(primary, false);
    WaitForSingleObject(g_paused, 20000);
    wcscpy_s(g_frontExe, kStartExe);
    g_frontWnd = g_otherWnd;
    double t0 = NowSeconds();
    SetEvent(g_resume);
    Check(WaitIdle() && NowSeconds() - t0 < 0.1 && Sent(1, 0, 0) && g_finds == 0,
          "Start's window comes in front during a wait, IAppVisibility saying nothing: the next look sees Start (no press, no key)");
    looks = g_looks - looks; opened = g_processesOpened - opened; tokens = g_tokensOpened - tokens;
    printf("  a window come in front during a wait: %d looks, %d processes opened, %d tokens\n", looks, opened, tokens);
    Check(looks == 12 && opened == 4 && tokens == 1, "[char] ...exactly 12 looks, 4 processes opened (the click, Explorer's age, the first look, the window come in front), 1 token");

    // ---- a program in front not to be looked into is tried again at the next look: only a name read is kept
    Begin();
    g_blind = true;
    g_showBrings = false;
    wcscpy_s(g_frontExe, kStartExe);
    g_openRefused = true;
    g_pauseAtUs = 300000;
    looks = g_looks; opened = g_processesOpened; tokens = g_tokensOpened;
    Click(primary, false);
    WaitForSingleObject(g_paused, 20000);
    g_openRefused = false;
    t0 = NowSeconds();
    SetEvent(g_resume);
    Check(WaitIdle() && NowSeconds() - t0 < 0.1 && Sent(1, 0, 0),
          "Start's window in front, its process refused at first: once it can be opened, the next look sees Start (a refusal is not kept)");
    looks = g_looks - looks; opened = g_processesOpened - opened; tokens = g_tokensOpened - tokens;
    printf("  a program refused at first: %d looks, %d processes opened, %d tokens\n", looks, opened, tokens);
    Check(looks == 12 && opened == 14 && tokens == 0, "[char] ...exactly 12 looks, 14 processes opened (refused ones tried again at each look; Explorer's refused too), no token (the first refused)");
    Check(NoLeak(), "...no reference left over");
}

// what the click's lines add, as most scenes write it (their fakes: the right had and handed on, no input since the click,
// the same window in front throughout, Start's window not listed, the display not seen going off, the dock on the primary
// monitor)
#define DOCK_MON L"the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)"

static void CharacterTests(HMONITOR primary) {
    InFront();   // its IAppVisibility (one reference, for the life of the process) is there before any scene
    Traced t;
    size_t log = 0;

    // ==== 1. the usual click: the shell's request brings Start - the usual line, every time
    Scene({ "usual click, the shell",
        { L"start: Start came by the shell's request in 120 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary, nullptr,
          [] { return StartUp() && Sent(1, 0, 0) && g_finds == 0; }, "Start comes by the shell's request alone");
    Scene({ "the next usual click",
        { L"start: Start came by the shell's request in 120 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary, nullptr,
          [] { return StartUp() && Sent(2, 0, 0) && g_finds == 0; }, "the same again: its line too", false);

    // ==== 2. ...not on the dock's monitor (the shell's monitor service fails, or gives none): Windows chooses
    Scene({ "usual click, the shell, not on the dock's monitor",
        { L"start: Start came by the shell's request in 120 ms (not on the dock's monitor: Windows chose) [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary, [] { g_whereHr = E_FAIL; },
          [] { return StartUp() && Sent(1, 0, 0) && g_connects == 0; }, "Start comes all the same, the launcher not moved");
    Scene({ "the shell's monitor service gives no monitor",
        { L"start: Start came by the shell's request in 120 ms (not on the dock's monitor: Windows chose) [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary, [] { g_whereNull = true; },
          [] { return StartUp() && Sent(1, 0, 0) && g_connects == 0; }, "Start comes all the same, the launcher not moved");

    // ==== 3. the usual line's bounds: Start seen kUsual after the request, or more clicks meanwhile - the long line
    Scene({ "Start seen 0.57 s after the request: the usual line",
        { L"start: Start came by the shell's request in 570 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x19 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "21 looks, 0 launcher looks, 3 processes, 1 tokens, 23 front reads, 19 sleeps, 27 clock reads, 1 locks, 14 window reads" }, primary, [] { g_showDelay = 0.57; },
          [] { return StartUp() && Sent(1, 0, 0); }, "Start comes by the request");
    Scene({ "Start seen 0.6 s after the request (kUsual): the long line",
        { L"start: Start came after 0.6 s, by the shell's request - taskbar: not pressed; shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x20 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "22 looks, 0 launcher looks, 3 processes, 1 tokens, 24 front reads, 20 sleeps, 28 clock reads, 1 locks, 14 window reads" }, primary, [] { g_showDelay = 0.6; },
          [] { return StartUp() && Sent(1, 0, 0); }, "Start comes by the request");
    Scene({ "ready in 0.4 s (two calls of 0.2 s, neither busy): the usual line",
        { L"start: Start came by the shell's request in 520 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@400000 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary,
          [] { g_shellMakeTookUs = 200000; g_visMakeTook = 0.2; }, [] { return StartUp() && Sent(1, 0, 0) && At(g_showAtUs, 0.4); }, "Start comes by the request, 0.4 s in");
    Scene({ "ready in 0.4 s, Start 1 s after the request: the long line tells how long getting ready took",
        { L"start: Start came after 1.4 s, by the shell's request - taskbar: not pressed; shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting; getting ready to send took 0.4 s",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.4 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@400000 nowx2 (look sleep now)x34 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "36 looks, 0 launcher looks, 3 processes, 1 tokens, 38 front reads, 34 sleeps, 42 clock reads, 1 locks, 14 window reads" }, primary,
          [] { g_shellMakeTookUs = 200000; g_visMakeTook = 0.2; g_showDelay = 1.0; }, [] { return StartUp() && Sent(1, 0, 0); }, "Start comes by the request");
    // Report's two bounds of getting ready (reaching the shell + making IAppVisibility), pinned at each side as
    // the baseline's scenes pinned them: the long line's tail from 0.3 s (neither call busy: 0.15 s each), the usual line only
    // below kUsual (0.6 s: not to be had without a busy call - reaching the shell 0.4 s, so the click is slow, and says so)
    Scene({ "ready in 0.299999 s, Start slow: the long line, no tail",
        { L"start: Start came after 1.3 s, by the shell's request - taskbar: not pressed; shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.3 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@299999 nowx2 (look sleep now)x34 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "36 looks, 0 launcher looks, 3 processes, 1 tokens, 38 front reads, 34 sleeps, 42 clock reads, 1 locks, 14 window reads" }, primary,
          [] { g_shellMakeTookUs = 150000; g_visMakeTook = 0.149999; g_showDelay = 1.0; }, [] { return StartUp() && Sent(1, 0, 0) && At(g_showAtUs, 0.299999); }, "Start comes by the request");
    Scene({ "ready in 0.3 s, Start slow: the long line, with the tail",
        { L"start: Start came after 1.3 s, by the shell's request - taskbar: not pressed; shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting; getting ready to send took 0.3 s",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.3 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@300000 nowx2 (look sleep now)x34 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "36 looks, 0 launcher looks, 3 processes, 1 tokens, 38 front reads, 34 sleeps, 42 clock reads, 1 locks, 14 window reads" }, primary,
          [] { g_shellMakeTookUs = 150000; g_visMakeTook = 0.15; g_showDelay = 1.0; }, [] { return StartUp() && Sent(1, 0, 0) && At(g_showAtUs, 0.3); }, "Start comes by the request");
    Scene({ "ready in 0.599999 s: the usual line",
        { L"start: Start came by the shell's request in 720 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); slow - reaching the shell took 400 ms: Explorer busy]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@599999 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary,
          [] { g_shellMakeTookUs = 400000; g_visMakeTook = 0.199999; }, [] { return StartUp() && Sent(1, 0, 0) && At(g_showAtUs, 0.599999); }, "Start comes by the request");
    Scene({ "ready in 0.6 s (kUsual): the long line, with the tail",
        { L"start: Start came after 0.7 s, by the shell's request - taskbar: not pressed (slow: the shell's request is waited on alone); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting; getting ready to send took 0.6 s; slow - reaching the shell took 400 ms: Explorer busy",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.6 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@600000 nowx2 (look sleep now)x4 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary,
          [] { g_shellMakeTookUs = 400000; g_visMakeTook = 0.2; }, [] { return StartUp() && Sent(1, 0, 0) && At(g_showAtUs, 0.6); }, "Start comes by the request");

    // ==== 4. 2026-11-20 the other way round: the shell takes the request, nothing comes; the button, off, pressed at kConfirm
    Scene({ "the shell brings nothing, the button brings Start",
        { L"start: Start came after 2.1 s, by the taskbar's Start button - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x4 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "74 looks, 0 launcher looks, 3 processes, 1 tokens, 77 front reads, 71 sleeps, 83 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showBrings = false; },
          [] { return StartUp() && Sent(1, 1, 0) && At(g_pressAtUs, kConfirm) && g_finds == 1; }, "one request, one press at kConfirm, no key");
    Scene({ "the button kept from before",
        { L"start: Start came after 2.1 s, by the taskbar's Start button - taskbar: pressed (toggle, state off -> on; the button kept); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x4 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "74 looks, 0 launcher looks, 3 processes, 1 tokens, 77 front reads, 71 sleeps, 83 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showBrings = false; g_found = taskbarstart::Found::Kept; },
          [] { return StartUp() && Sent(1, 1, 0) && At(g_pressAtUs, kConfirm); }, "one request, one press at kConfirm, no key");

    // ==== 5. the button's state not off at its turn: left alone, the key at kGiveUp
    Scene({ "the button's state on: left alone, the key",
        { L"start: Start came after 4.1 s, by the Windows key - taskbar: left alone at 2.0 s: its toggle state was on - Explorer holds Start open, and a press would close it (the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; reading the taskbar button's state took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now hold(primary)@2010000 nowx2 (look sleep now)x66 look key(5B)@4000000 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "140 looks, 0 launcher looks, 3 processes, 1 tokens, 143 front reads, 136 sleeps, 150 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showBrings = false; g_buttonState = 1; },
          [] { return StartUp() && Sent(1, 0, 1) && g_holds == 1 && At(g_holdAtUs, kConfirm) && At(g_keyAtUs, kGiveUp); }, "no press, the key at kGiveUp");
    Scene({ "the button's state not to be read (indeterminate): left alone",
        { L"start: Start came after 4.1 s, by the Windows key - taskbar: left alone at 2.0 s: its toggle state was not to be read (indeterminate) (the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; reading the taskbar button's state took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now hold(primary)@2010000 nowx2 (look sleep now)x66 look key(5B)@4000000 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "140 looks, 0 launcher looks, 3 processes, 1 tokens, 143 front reads, 136 sleeps, 150 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showBrings = false; g_buttonState = 2; },
          [] { return StartUp() && Sent(1, 0, 1) && g_holds == 1 && At(g_keyAtUs, kGiveUp); }, "no press, the key at kGiveUp");
    Scene({ "the button's state not read: left alone",
        { L"start: Start came after 4.1 s, by the Windows key - taskbar: left alone at 2.0 s: its toggle state was not to be read (?) (the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; reading the taskbar button's state took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now hold(primary)@2010000 nowx2 (look sleep now)x66 look key(5B)@4000000 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "140 looks, 0 launcher looks, 3 processes, 1 tokens, 143 front reads, 136 sleeps, 150 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showBrings = false; g_buttonState = -1; },
          [] { return StartUp() && Sent(1, 0, 1) && g_holds == 1 && At(g_keyAtUs, kGiveUp); }, "no press, the key at kGiveUp");
    Scene({ "the button has no toggle state to read: left alone",
        { L"start: Start came after 4.1 s, by the Windows key - taskbar: left alone at 2.0 s: no toggle state to read (the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; reading the taskbar button's state took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now hold(primary)@2010000 nowx2 (look sleep now)x66 look key(5B)@4000000 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "140 looks, 0 launcher looks, 3 processes, 1 tokens, 143 front reads, 136 sleeps, 150 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showBrings = false; g_noToggle = true; },
          [] { return StartUp() && Sent(1, 0, 1) && g_holds == 1 && At(g_keyAtUs, kGiveUp); }, "no press, the key at kGiveUp");

    // ==== 6. no button to press: the key at kGiveUp, not at kConfirm
    Scene({ "no button in the taskbar's tree: the key at kGiveUp",
        { L"start: Start came after 4.1 s, by the Windows key - taskbar: none to press (none); shell: asked 1 time, accepted (00000000); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) nowx2 (look sleep now)x67 look key(5B)@4020000 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "141 looks, 0 launcher looks, 3 processes, 1 tokens, 144 front reads, 137 sleeps, 150 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showBrings = false; g_found = taskbarstart::Found::NoButton; },
          [] { return StartUp() && Sent(1, 0, 1) && g_holds == 0 && At(g_findAtUs, kConfirm) && At(g_keyAtUs, kGiveUp); }, "looked for at kConfirm, the key at kGiveUp");
    Scene({ "no taskbar on the monitor: the key at kGiveUp",
        { L"start: Start came after 4.1 s, by the Windows key - taskbar: none to press (no taskbar); shell: asked 1 time, accepted (00000000); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) nowx2 (look sleep now)x67 look key(5B)@4020000 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "141 looks, 0 launcher looks, 3 processes, 1 tokens, 144 front reads, 137 sleeps, 150 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showBrings = false; g_found = taskbarstart::Found::NoTaskbar; },
          [] { return StartUp() && Sent(1, 0, 1) && At(g_keyAtUs, kGiveUp); }, "the key at kGiveUp");

    // ==== 7. the press: not through (the key at kGiveUp), through and bringing nothing (the key kConfirm after it), timed out
    // (nothing more)
    Scene({ "the press does not go through: the key at kGiveUp",
        { L"start: Start came after 4.1 s, by the Windows key - taskbar: the press did not go through: toggle (80004005); shell: asked 1 time, accepted (00000000); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x66 look key(5B)@4000000 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "140 looks, 0 launcher looks, 3 processes, 1 tokens, 144 front reads, 136 sleeps, 150 clock reads, 1 locks, 20 window reads" }, primary,
          [] { g_showBrings = false; g_pressed.ok = false; g_pressed.hr = E_FAIL; },
          [] { return StartUp() && Sent(1, 1, 1) && At(g_pressAtUs, kConfirm) && At(g_keyAtUs, kGiveUp); }, "the press at kConfirm, the key at kGiveUp");
    Scene({ "the press brings nothing: the key",
        { L"start: Start came after 4.1 s, by the Windows key - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x67 look key(5B)@4030000 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "141 looks, 0 launcher looks, 3 processes, 1 tokens, 145 front reads, 137 sleeps, 151 clock reads, 1 locks, 20 window reads" }, primary, [] { g_showBrings = false; g_pressBrings = false; },
          [] { return StartUp() && Sent(1, 1, 1) && At(g_keyAtUs, kGiveUp); }, "each way once, Start by the key");
    Scene({ "the press timed out: nothing more",
        { L"start: Start did not come in 8.0 s - taskbar: pressed (toggle), unsure whether it arrived (80131505): waited on, nothing else sent; shell: asked 1 time, accepted (00000000); Windows key: not pressed (the taskbar's button took the press; the key would close a Start on its way); in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x200 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "270 looks, 0 launcher looks, 3 processes, 1 tokens, 274 front reads, 267 sleeps, 279 clock reads, 1 locks, 17 window reads" }, primary,
          [] { g_showBrings = false; g_pressBrings = false; g_pressed.ok = false; g_pressed.unsure = true; g_pressed.hr = static_cast<HRESULT>(0x80131505); },
          [] { return !StartUp() && Sent(1, 1, 0); }, "no key after a press that may still arrive");
    Scene({ "nothing brings Start: each way once",
        { L"start: Start did not come in 8.0 s - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: pressed; in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x67 look key(5B)@4030000 nowx2 (look sleep now)x133 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "271 looks, 0 launcher looks, 3 processes, 1 tokens, 276 front reads, 267 sleeps, 281 clock reads, 1 locks, 20 window reads" }, primary, [] { g_showBrings = false; g_pressBrings = false; g_keyBrings = false; },
          [] { return !StartUp() && Sent(1, 1, 1); }, "the request, the press, the key - and given up");
    Scene({ "nothing brings Start, the key not inserted",
        { L"start: Start did not come in 8.0 s - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: not inserted (input blocked); in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x67 look key(5B)@4030000 nowx2 (look sleep now)x133 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "271 looks, 0 launcher looks, 3 processes, 1 tokens, 276 front reads, 267 sleeps, 281 clock reads, 1 locks, 20 window reads" }, primary,
          [] { g_showBrings = false; g_pressBrings = false; g_keyInserted = false; }, [] { return !StartUp() && Sent(1, 1, 1); }, "each way once");

    // ==== 8. a Start slower than kConfirm, the click not slow: the button's state decides (the price of the ladder, a design
    // decision of 2026-11-20 - a slow click waits alone: 9)
    Scene({ "Start 3 s after the request, the button's state following it: left alone, Start comes",
        { L"start: Start came after 3.0 s, by the shell's request - taskbar: left alone at 2.0 s: its toggle state was on - Explorer holds Start open, and a press would close it (the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; reading the taskbar button's state took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now hold(primary)@2010000 nowx2 (look sleep now)x33 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "103 looks, 0 launcher looks, 3 processes, 1 tokens, 105 front reads, 100 sleeps, 112 clock reads, 1 locks, 14 window reads" }, primary, [] { g_showDelay = 3.0; },
          [] { return StartUp() && Sent(1, 0, 0) && g_holds == 1 && g_cancelled == 0; }, "the button left alone at kConfirm (on), no key before kGiveUp: Start comes by the request");
    Scene({ "Start 3 s after the request, the button's state staying off: the press stops it, the key brings it",
        { L"start: Start came after 4.1 s, by the Windows key - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x67 look key(5B)@4030000 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "141 looks, 0 launcher looks, 3 processes, 1 tokens, 145 front reads, 137 sleeps, 151 clock reads, 1 locks, 20 window reads" }, primary,
          [] { g_showDelay = 3.0; g_buttonState = 0; }, [] { return StartUp() && Sent(1, 1, 1) && g_cancelled == 1 && At(g_pressAtUs, kConfirm); },
          "the press at kConfirm stops the Start on its way (once); the key, kConfirm after the press, brings it");
    Scene({ "Start 5 s after the request, the button left alone: the key at kGiveUp stops it",
        { L"start: Start did not come in 8.0 s - taskbar: left alone at 2.0 s: its toggle state was on - Explorer holds Start open, and a press would close it (the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: pressed; in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the Windows key (4.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; reading the taskbar button's state took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now hold(primary)@2010000 nowx2 (look sleep now)x66 look key(5B)@4000000 nowx2 (look sleep now)x134 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "271 looks, 0 launcher looks, 3 processes, 1 tokens, 275 front reads, 267 sleeps, 281 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showDelay = 5.0; },
          [] { return !StartUp() && Sent(1, 0, 1) && g_holds == 1 && g_cancelled == 1 && At(g_keyAtUs, kGiveUp); }, "the key at kGiveUp toggles the Start on its way shut");

    // ==== 9. slow: what was taken is waited on alone
    Scene({ "slow (Explorer started a minute ago): the request waited on alone",
        { L"start: Start did not come in 8.0 s - taskbar: not pressed (slow: the shell's request is waited on alone); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s; slow - Windows up 3600 s, Explorer 60 s: started minutes ago",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x267 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "269 looks, 0 launcher looks, 3 processes, 1 tokens, 272 front reads, 267 sleeps, 275 clock reads, 1 locks, 14 window reads" }, primary, [] { g_explorerAgeS = 60; g_showBrings = false; },
          [] { return !StartUp() && Sent(1, 0, 0) && g_finds == 0; }, "one request, nothing else, the button not looked for");
    Scene({ "slow (Windows started a minute ago), Start 5 s after the request: it comes",
        { L"start: Start came after 5.0 s, by the shell's request - taskbar: not pressed (slow: the shell's request is waited on alone); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting; slow - Windows up 60 s, Explorer 864000 s: started minutes ago",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x167 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "169 looks, 0 launcher looks, 3 processes, 1 tokens, 171 front reads, 167 sleeps, 175 clock reads, 1 locks, 14 window reads" }, primary, [] { g_tickBaseMs = 60000; g_showDelay = 5.0; },
          [] { return StartUp() && Sent(1, 0, 0) && g_finds == 0 && g_cancelled == 0; }, "Start comes by the request alone");
    Scene({ "slow, Start at once: the usual line says so",
        { L"start: Start came by the shell's request in 120 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); slow - Windows up 3600 s, Explorer 60 s: started minutes ago]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary, [] { g_explorerAgeS = 60; },
          [] { return StartUp() && Sent(1, 0, 0); }, "Start comes by the request");
    Scene({ "reaching the shell took 0.25 s: slow",
        { L"start: Start did not come in 8.3 s - taskbar: not pressed (slow: the shell's request is waited on alone); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s; slow - reaching the shell took 250 ms: Explorer busy",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.2 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@250000 nowx2 (look sleep now)x267 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "269 looks, 0 launcher looks, 3 processes, 1 tokens, 272 front reads, 267 sleeps, 275 clock reads, 1 locks, 14 window reads" }, primary, [] { g_shellMakeTookUs = 250000; g_showBrings = false; },
          [] { return !StartUp() && Sent(1, 0, 0) && g_finds == 0 && At(g_showAtUs, 0.25); }, "the request waited on alone");
    Scene({ "making IAppVisibility took 0.25 s: slow",
        { L"start: Start did not come in 8.3 s - taskbar: not pressed (slow: the shell's request is waited on alone); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s; slow - making IAppVisibility took 250 ms: Explorer busy",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.2 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@250000 nowx2 (look sleep now)x267 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "269 looks, 0 launcher looks, 3 processes, 1 tokens, 272 front reads, 267 sleeps, 275 clock reads, 1 locks, 14 window reads" }, primary, [] { g_visMakeTook = 0.25; g_showBrings = false; },
          [] { return !StartUp() && Sent(1, 0, 0) && g_finds == 0; }, "the request waited on alone");
    Scene({ "the request itself took 0.25 s: slow",
        { L"start: Start did not come in 8.3 s - taskbar: not pressed (slow: the shell's request is waited on alone); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s; slow - the shell's request took 250 ms: Explorer busy",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.2 s): {np}; at the end: {np}; Start's window not found; the request took 250 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x267 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "269 looks, 0 launcher looks, 3 processes, 1 tokens, 272 front reads, 267 sleeps, 275 clock reads, 1 locks, 14 window reads" }, primary, [] { g_showTookUs = 250000; g_showBrings = false; },
          [] { return !StartUp() && Sent(1, 0, 0) && g_finds == 0; }, "the request waited on alone");
    Scene({ "finding the button took 0.25 s: slow, not pressed",
        { L"start: Start did not come in 8.0 s - taskbar: not pressed (slow: the shell's request is waited on alone); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s; slow - finding the taskbar's button took 250 ms: Explorer busy",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) nowx2 (look sleep now)x192 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "262 looks, 0 launcher looks, 3 processes, 1 tokens, 265 front reads, 259 sleeps, 270 clock reads, 1 locks, 14 window reads" }, primary, [] { g_findTookUs = 250000; g_showBrings = false; },
          [] { return !StartUp() && Sent(1, 0, 0) && g_finds == 1 && g_holds == 0; }, "looked for at kConfirm, then neither pressed nor the key");
    Scene({ "the press took 0.25 s: slow, no key",
        { L"start: Start did not come in 8.0 s - taskbar: pressed (toggle, state off -> on; the button found at the click), slow: waited on alone; shell: asked 1 time, accepted (00000000); Windows key: not pressed (the taskbar's button took the press; the key would close a Start on its way); in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s; slow - pressing the taskbar's button took 250 ms: Explorer busy",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.3 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the press took 250 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x192 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "262 looks, 0 launcher looks, 3 processes, 1 tokens, 266 front reads, 259 sleeps, 271 clock reads, 1 locks, 17 window reads" }, primary, [] { g_pressTook = 0.25; g_showBrings = false; g_pressBrings = false; },
          [] { return !StartUp() && Sent(1, 1, 0); }, "the press, then no key");
    Scene({ "reading the button's state took 0.25 s (on): slow, no key",
        { L"start: Start did not come in 8.0 s - taskbar: left alone at 2.3 s: its toggle state was on - Explorer holds Start open, and a press would close it (the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s; slow - reading the taskbar button's state took 250 ms: Explorer busy",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; reading the taskbar button's state took 250 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now hold(primary)@2010000 nowx2 (look sleep now)x192 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "262 looks, 0 launcher looks, 3 processes, 1 tokens, 265 front reads, 259 sleeps, 271 clock reads, 1 locks, 14 window reads" }, primary,
          [] { g_pressTook = 0.25; g_showBrings = false; g_buttonState = 1; }, [] { return !StartUp() && Sent(1, 0, 0) && g_holds == 1; }, "the button left alone, then no key");
    Scene({ "slow, the shell refuses: the press at once, waited on alone",
        { L"start: Start did not come in 8.0 s - taskbar: pressed (toggle, state off -> on; the button found at the click), slow: waited on alone; shell: asked 1 time, refused (80004005); Windows key: not pressed (the taskbar's button took the press; the key would close a Start on its way); in front: notepad.exe{adm}; 0 more clicks; Windows up 3608 s; slow - Windows up 3600 s, Explorer 60 s: started minutes ago",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (0.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 look now find(primary) now press(primary)@0 nowx2 (look sleep now)x267 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "270 looks, 0 launcher looks, 3 processes, 1 tokens, 274 front reads, 267 sleeps, 279 clock reads, 1 locks, 17 window reads" }, primary,
          [] { g_explorerAgeS = 60; g_showHr = E_FAIL; g_pressBrings = false; }, [] { return !StartUp() && Sent(1, 1, 0) && At(g_pressAtUs, 0); }, "the press at once, no key");

    // ==== 10. the shell does not take a request: the next way at once
    Scene({ "the shell not to be had: the button at once",
        { L"start: Start came after 0.1 s, by the taskbar's Start button - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: not available (80080005); Windows key: not pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the press (0.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell now mkvis nowx2 look now find(primary) now press(primary)@0 nowx2 (look sleep now)x4 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 13 clock reads, 1 locks, 14 window reads" }, primary, [] { g_shellMakeHr = CO_E_SERVER_EXEC_FAILURE; },
          [] { return StartUp() && Sent(0, 1, 0) && At(g_pressAtUs, 0); }, "no request, the press at once brings Start");
    Scene({ "the shell refuses: the button at once",
        { L"start: Start came after 0.1 s, by the taskbar's Start button - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, refused (80004005); Windows key: not pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (0.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 look now find(primary) now press(primary)@0 nowx2 (look sleep now)x4 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "7 looks, 0 launcher looks, 3 processes, 1 tokens, 10 front reads, 4 sleeps, 16 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showHr = E_FAIL; },
          [] { return StartUp() && Sent(1, 1, 0) && At(g_pressAtUs, 0); }, "one refused request, the press at once brings Start");
    Scene({ "the shell refuses, the press brings nothing: the key kConfirm later",
        { L"start: Start came after 2.1 s, by the Windows key - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, refused (80004005); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (0.0 s): {np}; right after the Windows key (2.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 look now find(primary) now press(primary)@0 nowx2 (look sleep now)x67 look key(5B)@2020000 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "74 looks, 0 launcher looks, 3 processes, 1 tokens, 78 front reads, 70 sleeps, 84 clock reads, 1 locks, 20 window reads" }, primary, [] { g_showHr = E_FAIL; g_pressBrings = false; },
          [] { return StartUp() && Sent(1, 1, 1) && At(g_keyAtUs, kConfirm); }, "the press at once, the key kConfirm after it");
    Scene({ "the shell refuses, no button: the key at once",
        { L"start: Start came after 0.1 s, by the Windows key - taskbar: none to press (none); shell: asked 1 time, refused (80004005); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the Windows key (0.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 look now find(primary) nowx2 look key(5B)@0 nowx2 (look sleep now)x3 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "7 looks, 0 launcher looks, 3 processes, 1 tokens, 10 front reads, 3 sleeps, 16 clock reads, 1 locks, 17 window reads" }, primary, [] { g_showHr = E_FAIL; g_found = taskbarstart::Found::NoButton; },
          [] { return StartUp() && Sent(1, 0, 1) && At(g_keyAtUs, 0); }, "the key at once brings Start");
    Scene({ "no button, the shell refuses, the key",
        { L"start: Start did not come in 4.0 s - taskbar: none to press (none); shell: asked 1 time, refused (80004005); Windows key: pressed; in front: nothing; 0 more clicks; Windows up 3604 s",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: nothing; right after the shell's request (0.0 s): nothing; right after the Windows key (0.0 s): nothing; at the end: nothing; Start's window not found; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 look now find(primary) nowx2 look key(5B)@0 nowx2 (look sleep now)x134 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "138 looks, 0 launcher looks, 1 processes, 0 tokens, 142 front reads, 134 sleeps, 147 clock reads, 1 locks, 5 window reads" }, primary,
          [] { g_showHr = E_FAIL; g_found = taskbarstart::Found::NoButton; g_keyBrings = false; g_frontWnd = nullptr; },
          [] { return !StartUp() && Sent(1, 0, 1) && NowScene() >= kGiveUp && NowScene() < kGiveUp + 0.1; }, "given up kGiveUp after the key");
    Scene({ "no button, the shell refuses, the key not inserted",
        { L"start: Start did not come in 0.0 s - taskbar: none to press (none); shell: asked 1 time, refused (80004005); Windows key: not inserted (input blocked); in front: notepad.exe{adm}; 0 more clicks; Windows up 3600 s",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the Windows key (0.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 look now find(primary) nowx2 look key(5B)@0 nowx2 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "4 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 0 sleeps, 13 clock reads, 1 locks, 17 window reads" }, primary,
          [] { g_showHr = E_FAIL; g_found = taskbarstart::Found::NoButton; g_keyInserted = false; },
          [] { return !StartUp() && Sent(1, 0, 1) && NowScene() < 0.1; }, "the dock stops at once: nothing was taken, nothing is on its way");
    Scene({ "Explorer not up",
        { L"start: Start did not come in 4.0 s - taskbar: Explorer is not up; shell: not available (80070426); Windows key: pressed; in front: notepad.exe{adm}; 0 more clicks; Windows up 3604 s",
          L"start: around that click - the dock's right to change the front window: had it; Explorer allowed to come in front: no (no shell window); input after the click before the first way went out: no; in front before: {np}; right after the Windows key (0.0 s): {np}; at the end: {np}; Start's window not found; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd tick now coinit shellwnd tick nowx2 mkvis nowx2 look now look key(5B)@0 nowx2 (look sleep now)x134 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "137 looks, 0 launcher looks, 2 processes, 1 tokens, 140 front reads, 134 sleeps, 142 clock reads, 1 locks, 14 window reads" }, primary, [] { g_explorerUp = false; g_keyBrings = false; },
          [] { return !StartUp() && Sent(0, 0, 1) && g_finds == 0 && g_shellMade == 0; }, "one key, nothing of Explorer touched");
    Scene({ "Explorer not up, the program in front not to be looked into",
        { L"start: Start did not come in 4.0 s - taskbar: Explorer is not up; shell: not available (80070426); Windows key: pressed; in front: ?; 0 more clicks; Windows up 3604 s",
          L"start: around that click - the dock's right to change the front window: had it; Explorer allowed to come in front: no (no shell window); input after the click before the first way went out: no; in front before: ? hunt-start_menu-front {front}; right after the Windows key (0.0 s): ? hunt-start_menu-front {front}; at the end: ? hunt-start_menu-front {front}; Start's window not found; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd tick now coinit shellwnd tick nowx2 mkvis nowx2 look now look key(5B)@0 nowx2 (look sleep now)x134 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "137 looks, 0 launcher looks, 138 processes, 0 tokens, 140 front reads, 134 sleeps, 142 clock reads, 1 locks, 14 window reads" }, primary,
          [] { g_explorerUp = false; g_keyBrings = false; g_openRefused = true; }, [] { return !StartUp() && Sent(0, 0, 1); }, "one key");

    // ==== 11. a program in front that runs as administrator (the dock does not): no key
    if (SelfElevated()) {
        for (int i = 0; i < 15; ++i) Check(true, "[char] administrator in front: not run (the test itself runs as administrator)");
    } else {
        Scene({ "an administrator's window in front, the shell refuses",
            { L"start: Start did not come in 0.0 s - taskbar: none to press (none); shell: asked 1 time, refused (80004005); Windows key: not pressed: notepad.exe runs as administrator and Windows drops keys sent to it; in front: notepad.exe (administrator); 0 more clicks; Windows up 3600 s",
              L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
            "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 look now find(primary) nowx2 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
            "3 looks, 0 launcher looks, 3 processes, 1 tokens, 6 front reads, 0 sleeps, 11 clock reads, 1 locks, 14 window reads" }, primary,
              [] { g_showHr = E_FAIL; g_found = taskbarstart::Found::NoButton; g_frontElevated = true; },
              [] { return !StartUp() && Sent(1, 0, 0) && NowScene() < 0.1; }, "no key into the void, the dock stops at once");
        Scene({ "an administrator's window in front, nothing brings Start: no key",
            { L"start: Start did not come in 8.0 s - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: not pressed: notepad.exe runs as administrator and Windows drops keys sent to it; in front: notepad.exe (administrator); 0 more clicks; Windows up 3608 s",
              L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
            "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x200 look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
            "270 looks, 0 launcher looks, 3 processes, 1 tokens, 274 front reads, 267 sleeps, 279 clock reads, 1 locks, 17 window reads" }, primary,
              [] { g_showBrings = false; g_pressBrings = false; g_frontElevated = true; }, [] { return !StartUp() && Sent(1, 1, 0); }, "the request and the press, never the key");
        // ...and the shell took the request, Start a second later: the key's text names the administrator
        Scene({ "an administrator's window in front, the shell slow",
            { L"start: Start came after 1.0 s, by the shell's request - taskbar: not pressed; shell: asked 1 time, accepted (00000000); Windows key: not pressed: notepad.exe runs as administrator and Windows drops keys sent to it; 0 more clicks while waiting",
              L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
            "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x34 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
            "36 looks, 0 launcher looks, 3 processes, 1 tokens, 38 front reads, 34 sleeps, 42 clock reads, 1 locks, 14 window reads" }, primary,
              [] { g_found = taskbarstart::Found::NoButton; g_frontElevated = true; g_showDelay = 1.0; }, [] { return StartUp() && Sent(1, 0, 0); }, "Start comes by the request, no key");
    }
    // ...no button at all, the shell reached in 0.4 s (slow: Explorer busy) and bringing Start at once: the usual line says so
    Scene({ "no button, the shell reached in 0.4 s",
        { L"start: Start came by the shell's request in 520 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); slow - reaching the shell took 400 ms: Explorer busy]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@400000 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary,
          [] { g_found = taskbarstart::Found::NoButton; g_shellMakeTookUs = 400000; }, [] { return StartUp() && Sent(1, 0, 0) && At(g_showAtUs, 0.4); }, "Start comes by the request, 0.4 s in");

    // ==== 12. the dock on another monitor: the monitor goes to the shell's monitor service, to the lookup and to the press
    Scene({ "usual click on another monitor, the shell",
        { L"start: Start came by the shell's request in 120 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor -1440,-560 1440x2560 (another monitor: a secondary taskbar)]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(other) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, kOtherMon, nullptr,
          [] { return StartUp() && Sent(1, 0, 0) && g_connects == 1; }, "the shell's request, the launcher moved to that monitor");
    Scene({ "another monitor: the shell brings nothing, that monitor's button brings Start",
        { L"start: Start came after 2.1 s, by the taskbar's Start button - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the press took 10 ms; the dock's monitor -1440,-560 1440x2560 (another monitor: a secondary taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(other) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(other) now press(other)@2010000 nowx2 (look sleep now)x4 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "74 looks, 0 launcher looks, 3 processes, 1 tokens, 77 front reads, 71 sleeps, 83 clock reads, 1 locks, 17 window reads" }, kOtherMon, [] { g_showBrings = false; },
          [] { return StartUp() && Sent(1, 1, 0); }, "the request, then the press of that monitor's taskbar button");

    // ==== 13. more clicks while waiting (one, then two: each taken in at a look of its own, counted with its time)
    for (int more = 1; more <= 2; ++more) {
        log = Begin();
        g_showBrings = false;
        g_pauseAtUs = 300000;
        Click(primary, false);
        for (int i = 0; i < more; ++i) {
            WaitForSingleObject(g_paused, 20000);
            Click(primary, false);   // joins the wait: counted
            if (i + 1 < more) g_pauseAtUs = g_virtUs.load() + 300000;
            SetEvent(g_resume);
        }
        Check(WaitIdle() && StartUp() && Sent(1, 1, 0) && At(g_pressAtUs, kConfirm), "[char] more clicks while the shell's request is waited on: nothing more sent for them");
        t = EndTrace();
        if (more == 1) ExpectClick(log, t, { "one more click while waiting",
            { L"start: Start came after 2.1 s, by the taskbar's Start button - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 1 more click while waiting (at 0.3 s)",
              L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
            "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x9 look sleep tick (now look sleep)x57 now look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x4 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
            "74 looks, 0 launcher looks, 3 processes, 1 tokens, 77 front reads, 71 sleeps, 83 clock reads, 1 locks, 17 window reads" });
        else ExpectClick(log, t, { "two more clicks while waiting",
            { L"start: Start came after 2.1 s, by the taskbar's Start button - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 2 more clicks while waiting (at 0.3, 0.6 s)",
              L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; right after the press (2.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
            "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x9 look sleep tick (now look sleep)x10 tick (now look sleep)x47 now look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x4 look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
            "74 looks, 0 launcher looks, 3 processes, 1 tokens, 77 front reads, 71 sleeps, 83 clock reads, 1 locks, 17 window reads" });
    }
    // ...a usual click with one more click meanwhile: the long line (the usual one only with no more clicks)
    log = Begin();
    g_pauseAtUs = 1;   // the worker's first sleep waits for the test
    Click(primary, false);
    WaitForSingleObject(g_paused, 20000);
    Click(primary, false);
    SetEvent(g_resume);
    Check(WaitIdle() && StartUp() && Sent(1, 0, 0), "[char] one more click while the usual request is waited on: nothing more sent");
    t = EndTrace();
    ExpectClick(log, t, { "usual click, one more click meanwhile",
        { L"start: Start came after 0.1 s, by the shell's request - taskbar: not pressed; shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 1 more click while waiting (at 0.0 s)",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 look sleep tick (now look sleep)x3 now look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" });
    // ...'did not come' with one more click and 0.4 s to reach the shell (slow: Explorer busy - and the shell then refuses,
    // so nothing is waited on alone): the singular, and the line's end
    log = Begin();
    g_shellMakeTookUs = 400000;
    g_showHr = E_FAIL;
    g_found = taskbarstart::Found::NoButton;
    g_keyBrings = false;
    g_pauseAtUs = 1;
    Click(primary, false);
    WaitForSingleObject(g_paused, 20000);
    Click(primary, false);
    SetEvent(g_resume);
    Check(WaitIdle() && !StartUp() && Sent(1, 0, 1), "[char] the shell refuses, the key brings nothing, one more click: neither sent again");
    t = EndTrace();
    ExpectClick(log, t, { "did not come, one more click, the shell reached in 0.4 s",
        { L"start: Start did not come in 4.4 s - taskbar: none to press (none); shell: asked 1 time, refused (80004005); Windows key: pressed; in front: notepad.exe{adm}; 1 more click (at 0.4 s); Windows up 3604 s; getting ready to send took 0.4 s; slow - reaching the shell took 400 ms: Explorer busy",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.4 s): {np}; right after the Windows key (0.4 s): {np}; at the end: {np}; Start's window not found; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@400000 nowx2 look now find(primary) nowx2 look key(5B)@400000 nowx2 look sleep tick (now look sleep)x133 now look lock tick logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "138 looks, 0 launcher looks, 3 processes, 1 tokens, 142 front reads, 134 sleeps, 147 clock reads, 1 locks, 17 window reads" });

    // ==== 14. a worker stuck on a hung Explorer (reaching the shell), left behind by a later click; it says so and sends
    // nothing more. (At its leaving, the later worker - paused in its wait - holds its launcher and IAppVisibility.)
    log = Begin();
    g_showDelay = 1.0;
    g_hangShellMakes = 1;
    {
        const int d0 = g_threadsDone;
        Click(primary, false);
        WaitFor([] { return g_shellMade.load() >= 1 && g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        g_pauseAtUs = g_virtUs.load();
        Click(primary, false);
        WaitForSingleObject(g_paused, 20000);
        SetEvent(g_unblock);
        WaitFor([&] { return g_threadsDone.load() >= d0 + 1; });
        SetEvent(g_resume);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_shows == 1 && StartUp(), "[char] a click left behind on a hung Explorer: one request in all");
    }
    t = EndTrace();
    ExpectClick(log, t, { "a click left behind on a hung Explorer",
        { L"start: the last click's request has waited on Explorer for 16 s: starting over",
          L"start: the click left behind on a hung Explorer got an answer again after 16.0 s: nothing more sent, nothing learned (a later click started over; sent before: the taskbar's button 0, the shell 0, the Windows key 0)",
          L"start: Start came after 1.0 s, by the shell's request - taskbar: not pressed; shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): {np}; at the end: {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the request took 0 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell tick log shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@16000000 nowx2 look sleep nowx2 lock now log unlock couninit[shell 0, launcher 1, monitors 0, where 0, vis 2, current another] (now look sleep)x33 now look lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "36 looks, 0 launcher looks, 5 processes, 2 tokens, 39 front reads, 34 sleeps, 47 clock reads, 2 locks, 14 window reads",
        "couninit[shell 0, launcher 1, monitors 0, where 0, vis 2, current another] couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]" });

    // ==== 15. left behind after it sent: its second look hangs after the request; or, the request bringing nothing, the look
    // after its press at kConfirm - the counts in its line
    for (int press = 0; press < 2; ++press) {
        log = Begin();
        if (press) { g_showBrings = false; g_pressBrings = false; }
        const int d0 = g_threadsDone;
        g_pauseAtUs = press ? 2040000 : 1;   // (the sleep after the press at 2.01 s; or the worker's first sleep)
        Click(primary, false);
        WaitForSingleObject(g_paused, 20000);
        g_hangVisLooks = 1;   // its next look hangs on Explorer
        SetEvent(g_resume);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        if (press) g_showBrings = true;
        Click(primary, false);   // starts over
        WaitIdle();
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && StartUp() && g_presses == (press ? 1 : 0) && g_shows == (press ? 2 : 1) && g_keys == 0,
              press ? "[char] left behind after its press: no key sent by it, the new click's request brings Start" : "[char] left behind after the shell's request: nothing more sent by either");
        t = EndTrace();
        if (press) ExpectClick(log, t, { "left behind after the press",
            { L"start: the last click's request has waited on Explorer for 18 s: starting over",
              L"start: Start came by the shell's request in 120 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]",
              L"start: the click left behind on a hung Explorer got an answer again after 18.2 s: nothing more sent, nothing learned (a later click started over; sent before: the taskbar's button 1, the shell 1, the Windows key 0)" },
            "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 look sleep now look tick log shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@18050000 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 1, monitors 0, where 0, vis 2, current mine] lock now log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current 0]",
            "77 looks, 0 launcher looks, 6 processes, 2 tokens, 81 front reads, 72 sleeps, 93 clock reads, 2 locks, 25 window reads",
            "couninit[shell 0, launcher 1, monitors 0, where 0, vis 2, current mine] couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current 0]" });
        else ExpectClick(log, t, { "left behind after the shell's request",
            { L"start: the last click's request has waited on Explorer for 16 s: starting over",
              L"start: the click left behind on a hung Explorer got an answer again after 16.0 s: nothing more sent, nothing learned (a later click started over; sent before: the taskbar's button 0, the shell 1, the Windows key 0)" },
            "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 look sleep now look tick log shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look lock unlock couninit[shell 0, launcher 1, monitors 0, where 0, vis 2, current mine] lock now log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current 0]",
            "4 looks, 0 launcher looks, 5 processes, 2 tokens, 5 front reads, 1 sleeps, 15 clock reads, 2 locks, 12 window reads",
            "couninit[shell 0, launcher 1, monitors 0, where 0, vis 2, current mine] couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current 0]" });
    }

    // ==== 16. a click while one hangs (Again), then one after kStuck: the new worker does not take in the click the hung one
    // never took (g_again cleared)
    {
        log = Begin();
        g_hangShellMakes = 1;
        const int d0 = g_threadsDone;
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(5);
        Click(primary, false);   // Again: taken by no one (the worker hangs)
        Advance(kStuck);
        Click(primary, false);   // 20 s after the first: starts over
        WaitIdle();
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && StartUp() && g_shows == 1 && g_keys == 0,
              "[char] a click meanwhile on a hung worker, then one after kStuck: one request, Start comes");
        t = EndTrace();
        ExpectClick(log, t, { "a click on a hung worker, then starting over",
            { L"start: the last click's request has waited on Explorer for 20 s: starting over",
              L"start: Start came by the shell's request in 120 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]",
              L"start: the click left behind on a hung Explorer got an answer again after 20.1 s: nothing more sent, nothing learned (a later click started over; sent before: the taskbar's button 0, the shell 0, the Windows key 0)" },
            "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell tickx2 log shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@20000000 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine] nowx2 lock now log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current 0]",
            "6 looks, 0 launcher looks, 5 processes, 2 tokens, 9 front reads, 4 sleeps, 17 clock reads, 2 locks, 14 window reads",
            "couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine] couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current 0]" });
    }
    // ...Start closed meanwhile (the button's key; the hung worker never takes in the close), then a click after kStuck: the
    // new worker asks the shell all the same (g_closed cleared)
    {
        log = Begin();
        g_hangShellMakes = 1;
        const int d0 = g_threadsDone;
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        BringStart(0); Advance(0.01);   // the user's own key brought Start meanwhile
        wcscpy_s(g_frontExe, kSearchExe);
        Click(primary, true);   // ...the button closes it (the Windows key)
        Advance(kStuck + 1);
        Click(primary, false);
        WaitIdle();
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && StartUp() && g_shows == 1 && g_keys == 1,
              "[char] Start closed while a worker hangs, then a click after kStuck: the new worker's request brings Start");
        t = EndTrace();
        ExpectClick(log, t, { "Start closed on a hung worker, then starting over",
            { L"start: closing Start (the start button pressed while it was open; in front: SearchHost.exe) by the Windows key",
              L"start: the last click's request has waited on Explorer for 16 s: starting over",
              L"start: Start came by the shell's request in 120 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: SearchHost.exe hunt-start_menu-front {front} cloak 0 -> SearchHost.exe hunt-start_menu-front {front} cloak 0; Start's window not found; seen by IAppVisibility; came on the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]",
              L"start: the click left behind on a hung Explorer got an answer again after 16.1 s: nothing more sent, nothing learned (a later click started over; sent before: the taskbar's button 0, the shell 0, the Windows key 0)" },
            "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell tick key(5B)@10000 log tick log shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@16010000 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine] nowx2 lock now log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current 0]",
            "6 looks, 0 launcher looks, 6 processes, 2 tokens, 10 front reads, 4 sleeps, 17 clock reads, 2 locks, 14 window reads",
            "couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine] couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current 0]" });
    }

    // ==== 17. Start closed with the button while a wait runs (the request brought nothing; the user's own key brought it):
    // the wait ends at once - and the worker reports as if the request had brought it
    log = Begin();
    g_showBrings = false;
    g_pauseAtUs = 300000;
    Click(primary, false);
    WaitForSingleObject(g_paused, 20000);
    BringStart(0); Advance(0.001);
    wcscpy_s(g_frontExe, kSearchExe);
    Click(primary, true);
    SetEvent(g_resume);
    Check(WaitIdle() && Sent(1, 0, 1) && !StartUp(), "[char] Start closed while a wait runs: nothing sent after the key that closed it");
    t = EndTrace();
    ExpectClick(log, t, { "Start closed while a wait runs",
        { L"start: closing Start (the start button pressed while it was open; in front: SearchHost.exe) by the Windows key",
          L"start: Start came by the shell's request in 301 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; not seen: the start button closed it meanwhile; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x9 look sleep tick key(5B)@301000 log now lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "11 looks, 0 launcher looks, 4 processes, 1 tokens, 15 front reads, 10 sleeps, 18 clock reads, 1 locks, 14 window reads" });

    // ==== 18. Start already up at the worker's first look (the user's own key): nothing sent, no line
    log = Begin();
    BringStart(0);
    Check(ClickOpen(primary) && StartUp() && Sent(0, 0, 0), "[char] Start up at the first look: nothing sent");
    t = EndTrace();
    ExpectClick(log, t, { "Start up at the first look",
        {},
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look lock unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "1 looks, 0 launcher looks, 2 processes, 1 tokens, 1 front reads, 0 sleeps, 5 clock reads, 1 locks, 4 window reads" });

    // ==== 19. closing Start: the Windows key, on the UI thread alone - whatever is in front and on whichever monitor
    // (never the taskbar's button; no closing thread, no lookup, no lock)
    struct Close { const char* name; HMONITOR m; const wchar_t* front; const wchar_t* line; };
    const Close closes[] = {
        { "closing Start by the key", primary, kSearchExe, L"start: closing Start (the start button pressed while it was open; in front: SearchHost.exe) by the Windows key" },
        { "closing Start, its own window in front", primary, kStartExe, L"start: closing Start (the start button pressed while it was open; in front: StartMenuExperienceHost.exe) by the Windows key" },
        { "closing Start from another monitor's dock", kOtherMon, kSearchExe, L"start: closing Start (the start button pressed while it was open; in front: SearchHost.exe) by the Windows key" },
        { "closing Start, another program in front", primary, L"C:\\Program Files\\Test\\notepad.exe", L"start: closing Start (the start button pressed while it was open; in front: notepad.exe) by the Windows key" },
        { "closing Start, nothing in front", primary, nullptr, L"start: closing Start (the start button pressed while it was open; in front: nothing) by the Windows key" },
    };
    for (const Close& c : closes) {
        log = Begin();
        BringStart(0); Advance(0.01);
        if (c.front) wcscpy_s(g_frontExe, c.front); else g_frontWnd = nullptr;
        const int d0 = g_threadsDone;
        Click(c.m, true);
        t = EndTrace();
        const std::string n = std::string("[char] ") + c.name;
        Check(!StartUp() && Sent(0, 0, 1) && g_holds == 0 && g_finds == 0 && g_keptAsked == 0 && g_threadsDone == d0 && g_current.load() == 0,
              (n + ": the Windows key closes Start - the taskbar's button neither asked for nor pressed, no thread").c_str());
        ExpectLines(log, { c.line }, n + ": dock.log word for word");
        // (no lock, nothing asked of the taskbar: before mod.36 "lock unlock tick [kept(...) fg ...] key ...")
        ExpectText(Compress(t.all), c.front ? "tick key(5B)@10000 fg open log" : "tick key(5B)@10000 fg log", n + ": the UI thread's calls and reads, in order");
        Check(t.off.empty(), (n + ": no other thread did anything").c_str());
    }

    // ==== 20. the press-time check: Windows' search (SearchHost.exe) in front with Start not up is not Start
    Begin();
    wcscpy_s(g_frontExe, kSearchExe);
    const bool searchOnly = InFront();
    wcscpy_s(g_frontExe, kStartExe);
    const bool startWnd = InFront();
    EndTrace();
    Check(!searchOnly && startWnd, "[char] InFront: Start's own window in front is Start; Windows' search box in front, Start not up, is not (IAppVisibility saying no)");
    // ...and (mod.34, mod.35) a closed Start's window left in front - cloaked, IAppVisibility saying no - counts as Start in
    // front all the same: the click goes the closing way, and its key, a toggle, opens Start
    log = Begin();
    wcscpy_s(g_frontExe, kStartExe);
    g_frontCloak = 2;
    const bool closedInFront = InFront();
    Click(primary, closedInFront);
    t = EndTrace();
    Advance(0.1);   // (the key's Start: a few frames)
    Check(closedInFront && Sent(0, 0, 1) && StartUp() && g_current.load() == 0 && g_shellMade == 0,
          "[char] a closed Start's window in front (cloaked): taken as Start in front - the closing way's key opens Start, no worker, the shell not asked (as mod.35)");
    ExpectLines(log, { L"start: closing Start (the start button pressed while it was open; in front: StartMenuExperienceHost.exe) by the Windows key" },
                "[char] a closed Start's window in front: its line, word for word");
}

// ---- the lines a click adds for the next miss: the right to come in front as Click handed it on, the window in
// front at the click, right after each way went out and at the end, Start's own window (who, how cloaked), which check
// saw Start and on which monitor, when the more clicks came, the user's input before the first way, the dock's monitor
// and taskbar, the calls' milliseconds, when the display came on.
static void LineTests(HMONITOR primary) {
    Traced t;
    size_t log = 0;

    // ==== 40. 2026-11-20's click as the ladder takes it (then: a trusted press, off -> on, no Start in 8.3 s, seven more
    // clicks swallowed). Chrome in front, the PC up 23528 s, the display on for an hour, Start's window there (cloaked:
    // closed); the shell takes the request and brings nothing; two more clicks; the button, off, pressed at kConfirm,
    // brings Start.
    Begin();
    g_showBrings = false;
    g_found = taskbarstart::Found::Kept;
    g_pressTook = 0.012;
    g_shellMakeTookUs = 200000;   // getting ready in 0.2 s
    wcscpy_s(g_frontExe, kChromeExe);
    wcscpy_s(g_firstClass, L"Chrome_WidgetWin_1");
    g_coreWindows = true;
    g_frontWhenUp = g_searchWnd;
    log = Begin(false);
    const int64_t base = 4000LL * 1000000;   // the scene starts 4000 s into the dock's run (NowSeconds)
    g_virtUs = base; g_sceneBaseUs = base;
    g_tickBaseMs = 23528000 - 4002000;       // Windows up 23526 s at the click
    g_displayOnAt = 246.0;                   // the display came on 3754 s before the click
    int opened = g_processesOpened;
    {
        const double at[] = { 0.9, 1.6 };
        g_pauseAtUs = base + static_cast<int64_t>(at[0] * 1e6);
        Click(primary, false);
        for (int i = 0; i < 2; ++i) {
            WaitForSingleObject(g_paused, 20000);
            Click(primary, false);   // joins the wait: counted, and now its time
            if (i + 1 < 2) g_pauseAtUs = base + static_cast<int64_t>(at[i + 1] * 1e6);
            SetEvent(g_resume);
        }
        WaitIdle();
    }
    t = EndTrace();
    opened = g_processesOpened - opened;
    Check(StartUp() && Sent(1, 1, 0), "[mod.36] the 11-20 click, the ladder: the request, 2 s later the press - Start comes");
    ExpectClick(log, t, { "the 11-20 click, the ladder",
        { L"start: Start came after 2.3 s, by the taskbar's Start button - taskbar: pressed (toggle, state off -> on; the button kept); shell: asked 1 time, accepted (00000000); Windows key: not pressed (the shell took the request; the key would close a Start on its way); 2 more clicks while waiting (at 0.9, 1.6 s)",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: chrome.exe Chrome_WidgetWin_1 {front}; right after the shell's request (0.2 s): chrome.exe Chrome_WidgetWin_1 {front}; right after the press (2.2 s): chrome.exe Chrome_WidgetWin_1 {front}; at the end: SearchHost.exe Windows.UI.Core.CoreWindow {search} cloak 0; Start's window {start} (StartMenuExperienceHost.exe pid 2147483636, up 23400 s) cloak 2 -> 2 -> 0; seen by IAppVisibility; came on the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the request took 0 ms; the press took 12 ms; the display came on 3756 s ago" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis now ptimes now look now show(11,4)@200000 nowx2 (look sleep now)x23 look sleep tick (now look sleep)x23 tick (now look sleep)x20 now look now find(primary) now press(primary)@2210000 nowx2 (look sleep now)x4 look date lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "74 looks, 0 launcher looks, 6 processes, 1 tokens, 77 front reads, 71 sleeps, 83 clock reads, 1 locks, 21 window reads" });
    Check(opened == 6, "[mod.36] ...6 processes opened in all: the window in front at the click and at the wait's first look, Explorer's for its age, Start's lookup 3 (once a run)");

    // ==== 41. the same Start's window at the next click: remembered (IsWindow, its process) - no process opened for it
    {
        Begin(false);
        g_showBrings = true;   // (the usual click now: the shell brings Start)
        const int o0 = g_processesOpened;
        const size_t l0 = LogSize();
        Check(ClickOpen(primary) && StartUp() && Sent(2, 1, 0), "[mod.36] the next click: Start comes by the request");
        t = EndTrace();
        Check(g_processesOpened - o0 == 3, "[mod.36] ...3 processes opened, as a click without Start's window: Start's own is remembered, and the search box's program");
        Check(LogHas(l0, L"Start's window cloak 2 -> 0; seen by IAppVisibility"), "[mod.36] ...and the usual line tells Start's window uncloaked (2 -> 0)");
        CloseStart();
        // gone (Start or Explorer restarted): looked up again
        HWND gone = CreateWindowExW(WS_EX_TOOLWINDOW, L"hunt-start_menu-front", L"hunt-start_menu gone", WS_POPUP, 10, 10, 20, 20, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        DestroyWindow(gone);
        g_start.window = gone;
        const int o1 = g_processesOpened;
        size_t l1 = LogSize();
        Check(ClickOpen(primary) && StartUp(), "[mod.36] Start's remembered window gone: the click goes on");
        Check(g_processesOpened - o1 == 4 && g_start.window == g_startCoreWnd && LogHas(l1, L"Start's window cloak 2 -> 0"),
              "[mod.36] ...Start's window looked up again: 1 process more (Start's; the other CoreWindows' are remembered as not Start's), and the line tells it");
        CloseStart();
    }

    // ==== 42. the right to come in front: each of TakeForegroundRight's four answers, and the shell's process refusing it
    const struct { ForegroundRight r; const wchar_t* text; } rights[] = {
        { ForegroundRight::Had, L"had it" }, { ForegroundRight::Taken, L"taken with an unassigned key" },
        { ForegroundRight::Refused, L"refused even after an unassigned key" }, { ForegroundRight::NotInserted, L"not taken: the key was not inserted" } };
    for (const auto& r : rights) {
        Fresh();
        g_right = r.r;
        log = LogSize();
        Check(ClickOpen(primary) && StartUp() && Sent(1, 0, 0), "[mod.36] a usual click, whatever the right: Start comes by the request (nothing else sent)");
        Check(LogHas(log, (std::wstring(L"[the dock's right to change the front window: ") + r.text + L"; Explorer allowed to come in front: yes;").c_str()),
              ("[mod.36] ...the usual line says the right was " + Utf8(r.text) + " (shell_front.cpp's words, as the app clicks' lines)").c_str());
        CloseStart();
    }
    Fresh();
    g_allowRefused = true;
    log = LogSize();
    Check(ClickOpen(primary) && StartUp(), "[mod.36] the shell's process not allowed to come in front: the click goes on as before");
    Check(LogHas(log, L"; Explorer allowed to come in front: no (error 5);"), "[mod.36] ...the usual line says so, with the error");
    CloseStart();
    Fresh();
    g_allowRefused = true;
    g_right = ForegroundRight::Refused;
    g_showBrings = false;
    log = LogSize();
    Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 0), "[mod.36] the request brings nothing, the right refused: the press 2 s later (the right decides nothing)");
    Check(LogHas(log, L"start: around that click - the dock's right to change the front window: refused even after an unassigned key; "
                      L"Explorer (pid 2147483632) allowed to come in front: no (error 5); "), "[mod.36] ...the line around the click names both, with the shell's process");
    CloseStart();

    // ==== 43. the window in front at each moment, and where Start came: the request puts a cloaked shell window in front
    // (closed Start's ghost: mod.25, 11-19 22:58), the press another window, and the key brings Start with its search box
    // in front - on another monitor than the dock's. Start's window: 2 at the click, after the press and after the key, 0 at
    // the end. All three ways, each named with when it went out.
    log = Begin();
    g_showBrings = false; g_pressBrings = false;
    g_coreWindows = true;
    g_frontAfterShow = g_otherCoreWnd;
    g_frontAfterPress = g_otherWnd;
    g_frontWhenUp = g_searchWnd;
    g_searchMon = kOtherMon;
    Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 1), "[mod.36] three ways, a window in front after each: the request, the press, the key");
    t = EndTrace();
    ExpectClick(log, t, { "three ways, a window in front after each",
        { L"start: Start came after 4.1 s, by the Windows key - taskbar: pressed (toggle, state off -> on; the button found at the click); shell: asked 1 time, accepted (00000000); Windows key: pressed; 0 more clicks while waiting",
          L"start: around that click - the dock's right to change the front window: had it; Explorer (pid 2147483632) allowed to come in front: yes; input after the click before the first way went out: no; in front before: {np}; right after the shell's request (0.0 s): ShellExperienceHost.exe Windows.UI.Core.CoreWindow cloaked {core}; right after the press (2.0 s): notepad.exe hunt-start_menu-front {other}; right after the Windows key (4.0 s): notepad.exe hunt-start_menu-front {other}; at the end: SearchHost.exe Windows.UI.Core.CoreWindow {search} cloak 0; Start's window {start} (StartMenuExperienceHost.exe pid 2147483636, up 23400 s) cloak 2 -> 2 -> 2 -> 0; seen by IAppVisibility; came on monitor -1440,-560 1440x2560, not the dock's 0,0 2560x1440 (the primary monitor: the main taskbar); the request took 0 ms; the press took 10 ms; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar); the display not seen going off in this run" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis now ptimes now look now show(11,4)@0 nowx2 (look sleep now)x67 look now find(primary) now press(primary)@2010000 nowx2 (look sleep now)x67 look key(5B)@4030000 nowx2 (look sleep now)x3 look date lock logx2 unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "141 looks, 0 launcher looks, 8 processes, 1 tokens, 145 front reads, 137 sleeps, 151 clock reads, 1 locks, 26 window reads" });
    CloseStart();

    // ==== 44. Start's window uncloaked (0) after the press, yet no check sees Start: 2 -> 0 and no Start (the dock's check
    // missed it), against 2 -> 2 (Explorer did not bring it)
    log = Begin();
    g_showBrings = false; g_pressBrings = false; g_keyBrings = false;
    g_blind = true;
    g_coreWindows = true;
    g_cloakAfterPress = 0;
    Check(ClickOpen(primary) && !StartUp() && Sent(1, 1, 1), "[mod.36] Start's window uncloaked by the press, no check sees Start: the ladder goes on to the key");
    EndTrace();
    Check(LogHas(log, L"Start did not come in 8.0 s") && LogHas(log, L"(StartMenuExperienceHost.exe pid 2147483636, up 23400 s) cloak 2 -> 0 -> 0 -> 0;"),
          "[mod.36] ...the line around the click says Start's window went 2 -> 0 (uncloaked) at the press while the checks saw nothing");

    // ==== 45. Start's window not listed: 'not found'; another worker in the lookup meanwhile: not waited for
    Fresh();
    log = LogSize();
    ClickOpen(primary); CloseStart();
    Check(LogHas(log, L"; Start's window not found; seen by IAppVisibility"), "[mod.36] Start's window not listed: the tail says 'not found'");
    {
        Fresh();
        g_coreWindows = true;
        g_startBusy = true;
        const int o0 = g_processesOpened;
        log = LogSize();
        Check(ClickOpen(primary) && StartUp() && Sent(1, 0, 0), "[mod.36] Start's window being looked up by another worker: the click goes on (as before)");
        Check(g_processesOpened - o0 == 3 && LogHas(log, L"; Start's window not found;") && g_start.window == nullptr,
              "[mod.36] ...it neither waits nor looks: Start's window 'not found', no process opened for it");
        g_startBusy = false;
        CloseStart();
    }

    // ==== 46. input after the click before the first way went out: yes, with when
    log = Begin();
    g_showBrings = false;
    g_lastInputTick = 3600000 + 150;   // the click is at tick 3600000 (Windows up 3600 s at the scene's start)
    Check(ClickOpen(primary) && StartUp() && Sent(1, 1, 0), "[mod.36] input after the click: the request, then the press (the input decides nothing)");
    EndTrace();
    Check(LogHas(log, L"; input after the click before the first way went out: yes, 0.15 s after the click; "), "[mod.36] ...the line around the click says there was input, and when");

    // ==== 47. the dock on another monitor: the line around a miss names it and its taskbar (a secondary one)
    log = Begin();
    g_explorerAgeS = 60;
    g_showBrings = false;
    Check(ClickOpen(kOtherMon) && !StartUp() && Sent(1, 0, 0), "[mod.36] a slow click on another monitor's dock: the request waited on alone");
    EndTrace();
    Check(LogHas(log, L"; the dock's monitor -1440,-560 1440x2560 (another monitor: a secondary taskbar); the display not seen going off in this run"),
          "[mod.36] ...the line around the click names the dock's monitor and a secondary taskbar");

    // ==== 48. the measures of a busy Explorer hold no new read: reaching the shell ('mkshell' to 'connect' between two clock
    // reads), IAppVisibility's making ('mkvis' between two), the request (the clock read right before it and right after);
    // the input is read just before that, the window in front after
    log = Begin();
    Check(ClickOpen(primary) && StartUp(), "[mod.36] a usual click, traced whole");
    t = EndTrace();
    {
        const auto& a = t.all;
        const auto sh = std::find(a.begin(), a.end(), "mkshell");
        const auto mk = std::find(a.begin(), a.end(), "mkvis");
        const auto rq = std::find_if(a.begin(), a.end(), [](const std::string& x) { return x.rfind("show(", 0) == 0; });
        const bool reachClean = sh != a.end() && sh > a.begin() && sh + 5 < a.end() && sh[-1] == "now" && sh[1] == "qs-launcher" && sh[2] == "qs-monitors" &&
                                sh[3] == "where(primary)" && sh[4] == "connect" && sh[5] == "now";
        const bool visClean = mk != a.end() && mk > a.begin() && mk + 1 < a.end() && mk[-1] == "now" && mk[1] == "now";   // (v0; then t0)
        const bool noneBefore = mk != a.end() && std::find(a.begin(), mk + 2, std::string("win")) == mk + 2;
        const bool requestClean = rq != a.end() && rq > a.begin() + 1 && rq + 1 < a.end() && rq[-1] == "now" && rq[-2] == "win" && rq[1] == "now";
        Check(reachClean && visClean && noneBefore,
              "[mod.36] ...no read of the lines' before the timeline starts (t0): reaching the shell and making IAppVisibility each lie between two clock reads, nothing else between");
        Check(requestClean, "[mod.36] ...the input read, then the clock, the request, the clock: the request's measure holds the call alone; the window in front is read after");
    }

    // ==== 49. the same for every way: whatever went out - the request, the press, the key, first or after another - its time
    // is read right after it ('now' the next token: the window in front and Start's cloak after that), and the input before
    // the first one. (The trace's Calls leave the reads out and the reads move no clock: only the raw trace tells where they sit.)
    {
        auto isSend = [](const std::string& x) { return x.rfind("press(", 0) == 0 || x.rfind("show(", 0) == 0 || x.rfind("key(", 0) == 0; };
        auto sendsClean = [&](const std::vector<std::string>& a, const char* want) {
            std::string sent;
            bool clean = true, first = true;
            for (size_t i = 0; i < a.size(); ++i) {
                if (!isSend(a[i])) continue;
                sent += (sent.empty() ? "" : " ") + a[i].substr(0, a[i].find('('));
                // (before the request: the clock read that starts its measure, the input before that; before a press or a key: the input)
                const bool inputBefore = i > 0 && (a[i - 1] == "win" || (a[i].rfind("show(", 0) == 0 && i > 1 && a[i - 1] == "now" && a[i - 2] == "win"));
                clean = clean && i + 1 < a.size() && a[i + 1] == "now" && (!first || inputBefore);
                first = false;
            }
            return clean && sent == want;
        };
        const struct { const char* name; const char* sent; void (*set)(); } ways[] = {
            { "the request alone", "show", [] {} },
            { "the request, then the press", "show press", [] { g_showBrings = false; } },
            { "the request, the press, the key", "show press key", [] { g_showBrings = false; g_pressBrings = false; } },
            { "the request, the button left alone, the key", "show key", [] { g_showBrings = false; g_buttonState = 1; } },
            { "no shell: the press first", "press", [] { g_shellMakeHr = CO_E_SERVER_EXEC_FAILURE; } },
            { "no shell, the button left alone: the key first (the input read again before it)", "key", [] { g_shellMakeHr = CO_E_SERVER_EXEC_FAILURE; g_buttonState = 1; } },
            { "Explorer not up: the key first", "key", [] { g_explorerUp = false; g_keyBrings = false; } },
        };
        for (const auto& w : ways) {
            Begin();
            w.set();
            ClickOpen(primary);
            t = EndTrace();
            Check(sendsClean(t.all, w.sent), (std::string("[mod.36] ") + w.name + ": each way's time read right after it went out (the reads after), the input before the first").c_str());
            CloseStart();
        }
    }

    // ==== 50. IAppVisibility tells Start up while the program's window is still in front - here on another monitor than
    // the dock's: that window is not Start's, so the line names no monitor Start came on; Start's own window in front (the
    // only check that sees it): it does
    Scene({ "the program in front on another monitor, Start up",
        { L"start: Start came by the shell's request in 120 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> {np}; Start's window not found; seen by IAppVisibility; the window in front at the end is not Start's; the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 3 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary, [] { g_frontMon = kOtherMon; },
          [] { return StartUp() && Sent(1, 0, 0); }, "Start comes by the request");
    Scene({ "only Start's own window in front tells",
        { L"start: Start came by the shell's request in 120 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> StartMenuExperienceHost.exe Windows.UI.Core.CoreWindow {start}; Start's window not found; seen by the window in front (Start's own); came on the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]" },
        "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
        "6 looks, 0 launcher looks, 4 processes, 1 tokens, 9 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary, [] { g_blind = true; g_frontWhenUp = g_startCoreWnd; },
          [] { return StartUp() && Sent(1, 0, 0); }, "Start comes by the request, seen by its window in front");

    // ==== 51. the usual click as on 25H2 and 26H2: Start's search box in front once Start is up, IAppVisibility telling it
    // first (so the wait's looks never read that window). Its program is looked into once a run for the line, then
    // remembered (g_start.named); with Start's window listed the search box is named by Start's lookup already
    {
        Scene({ "the search box in front once Start is up",
            { L"start: Start came by the shell's request in 120 ms [the dock's right to change the front window: had it; Explorer allowed to come in front: yes; the request took 0 ms; in front: {np} -> SearchHost.exe Windows.UI.Core.CoreWindow {search} cloak 0; Start's window not found; seen by IAppVisibility; came on the dock's monitor 0,0 2560x1440 (the primary monitor: the main taskbar)]" },
            "tick shellwnd allowfg(shell) tick now coinit shellwnd tick ptimes date now mkshell qs-launcher qs-monitors where(primary) connect now mkvis nowx2 look now show(11,4)@0 nowx2 (look sleep now)x4 look lock log unlock couninit[shell 0, launcher 0, monitors 0, where 0, vis 1, current mine]",
            "6 looks, 0 launcher looks, 4 processes, 1 tokens, 8 front reads, 4 sleeps, 12 clock reads, 1 locks, 14 window reads" }, primary, [] { g_frontWhenUp = g_searchWnd; },
              [] { return StartUp() && Sent(1, 0, 0); }, "Start comes by the request");
        CloseStart();
        for (int i = 0; i < 2; ++i) {
            Begin(false);
            const int o0 = g_processesOpened, s0 = g_shows;
            const size_t l0 = LogSize();
            Check(ClickOpen(primary) && StartUp() && g_shows - s0 == 1, "[mod.36] the next such click: Start comes by the request");
            Check(g_processesOpened - o0 == 3 && LogHas(l0, L" -> SearchHost.exe Windows.UI.Core.CoreWindow "),
                  "[mod.36] ...3 processes opened, as a usual click: the search box's program remembered, and still named");
            CloseStart();
        }
        Begin(false);   // another worker in Start's window meanwhile (g_startBusy): the program looked into, as without the memory
        g_startBusy = true;
        int o0 = g_processesOpened;
        Check(ClickOpen(primary) && StartUp(), "[mod.36] the memory held by another worker: the click goes on");
        Check(g_processesOpened - o0 == 4, "[mod.36] ...4 processes opened: the search box's looked into again, nothing waited for");
        g_startBusy = false;
        CloseStart();
        log = Begin();
        g_frontWhenUp = g_searchWnd;
        g_coreWindows = true;
        o0 = g_processesOpened;
        Check(ClickOpen(primary) && StartUp(), "[mod.36] Start's window listed, the search box in front: Start comes by the request");
        Check(g_processesOpened - o0 == 6, "[mod.36] ...6 processes opened: 3 as a usual click, Start's lookup 3 (the search box's program named on the way)");
        CloseStart();
        Begin(false);
        o0 = g_processesOpened;
        Check(ClickOpen(primary) && StartUp() && g_processesOpened - o0 == 3, "[mod.36] ...and the next click 3: Start's window and the search box's program remembered");
        CloseStart();
    }
}

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_mainTid = GetCurrentThreadId();
    g_paused = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_resume = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_unblock = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // the test's own folder, where the dock's old verdict file would be
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t* slash = wcsrchr(exe, L'\\')) *slash = 0;
    _snwprintf_s(g_dataDir, _TRUNCATE, L"%ls\\data-start_menu_run", exe);
    CreateDirectoryW(g_dataDir, nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"hunt-start_menu-front";
    RegisterClassW(&wc);
    // hidden windows of the test's own: "the window in front" (and another one), "the shell's window" (never shown)
    g_firstWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"hunt-start_menu front", WS_POPUP, 10, 10, 20, 20, nullptr, nullptr, wc.hInstance, nullptr);
    g_otherWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"hunt-start_menu other", WS_POPUP, 10, 10, 20, 20, nullptr, nullptr, wc.hInstance, nullptr);
    g_shellWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"hunt-start_menu shell", WS_POPUP, 10, 10, 20, 20, nullptr, nullptr, wc.hInstance, nullptr);
    // the shell's CoreWindows (hidden, never shown): Start's, its search box's, another program's
    g_startCoreWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"hunt-start_menu start", WS_POPUP, 10, 10, 20, 20, nullptr, nullptr, wc.hInstance, nullptr);
    g_searchWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"hunt-start_menu search", WS_POPUP, 10, 10, 20, 20, nullptr, nullptr, wc.hInstance, nullptr);
    g_otherCoreWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"hunt-start_menu core", WS_POPUP, 10, 10, 20, 20, nullptr, nullptr, wc.hInstance, nullptr);
    g_frontWnd = g_firstWnd;
    const HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    g_primaryMon = primary;
    InFront();   // its IAppVisibility (one reference, for the life of the process) is there before any test
    LadderTests(primary);
    VerdictFileTests(primary);
    LeftBehindTests(primary);
    FrontTests(primary);
    CharacterTests(primary);
    LineTests(primary);
    WaitIdle();
    Check(g_pressesAnyState == 0, "in all of it the worker never asked for a press whatever the button's state: only of a button that reads off");
    Check(g_fileCalls == 0 && g_dataDirAsked == 0, "in all of it start_menu.cpp made no file or registry call and never asked for the data folder (nothing is kept between clicks)");
    Summary("start_menu_run_test");
    fflush(stdout);
    ExitProcess(static_cast<UINT>(portable_check::Failures()));   // workers left behind may still sit in a fake call
}
