// The start button's worker (src/start_menu.cpp: Click, Run, CloseWithTaskbar, the kept verdict) against a
// simulated Explorer: the shell's launcher, IAppVisibility, the taskbar's Start button (taskbarstart::), the
// Windows key, the window in front and the clocks are fakes, so every path of the worker runs here - the
// taskbar's button found or not, going through, timing out, slow; the shell reached late or not at all;
// Explorer gone; a worker stuck on a hung Explorer and left behind; Start closed while a wait runs; the
// verdict file data\taskbar-start.txt read, written, expired (at a run's first click and within a run that
// lasts days); a failure learned minutes after Windows or Explorer started; the window in front as a wait's
// looks read it (the processes and tokens opened, counted). Nothing reaches Windows: no key,
// no press, no call into Explorer; the verdict file lives in data-start_menu_run\ next to the test.
// Time is virtual: the worker's looks (Sleep) move a clock of the test's own, so an 8 s wait takes a few
// milliseconds and every run is the same.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\start_menu_run_test.cpp /link user32.lib ole32.lib uuid.lib advapi32.lib
#include "../src/common.h"
#include "../src/system.h"
#include "../src/taskbar_start.h"
#include "../src/start_menu.h"
#include "../src/start_menu_logic.h"
#include <objbase.h>
#include <servprov.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

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
#include "../src/start_menu.cpp"
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

// ---- the virtual clock: NowSeconds, GetTickCount64 and the worker's Sleep
static std::atomic<int64_t> g_virtUs{ 1000000000 };   // microseconds
static std::atomic<int64_t> g_tickBaseMs{ 10LL * 86400 * 1000 };   // Windows up 10 days (a failure is kept)
static std::atomic<long> g_dayShift{ 0 };              // days added to the real date (Today)
static std::atomic<int64_t> g_explorerAgeS{ 10LL * 86400 };   // Explorer (the shell window's process) up 10 days; -1: no telling
static std::atomic<int64_t> g_pauseAtUs{ -1 };         // the next Sleep that reaches this time waits for the test
static HANDLE g_paused = nullptr, g_resume = nullptr;
double NowSeconds() { return g_virtUs.load() / 1e6; }
static void Advance(double s) { g_virtUs += static_cast<int64_t>(s * 1e6); }
ULONGLONG WINAPI FakeTickCount64() { return static_cast<ULONGLONG>(g_tickBaseMs.load() + g_virtUs.load() / 1000); }
void WINAPI FakeSystemTimeAsFileTime(LPFILETIME ft) {
    GetSystemTimeAsFileTime(ft);
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime; u.HighPart = ft->dwHighDateTime;
    u.QuadPart += static_cast<ULONGLONG>(g_dayShift.load()) * 864000000000ULL;
    ft->dwLowDateTime = u.LowPart; ft->dwHighDateTime = u.HighPart;
}
// the shell window's process (here the test's own): started g_explorerAgeS before the (shifted) date
BOOL WINAPI FakeProcessTimes(HANDLE, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel, LPFILETIME user) {
    const int64_t age = g_explorerAgeS.load();
    if (age < 0) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    FILETIME now;
    FakeSystemTimeAsFileTime(&now);
    ULARGE_INTEGER u;
    u.LowPart = now.dwLowDateTime; u.HighPart = now.dwHighDateTime;
    u.QuadPart -= static_cast<ULONGLONG>(age) * 10000000ULL;
    created->dwLowDateTime = u.LowPart; created->dwHighDateTime = u.HighPart;
    *exited = *kernel = *user = FILETIME{};
    return TRUE;
}
void WINAPI FakeSleep(DWORD ms) {
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
    std::lock_guard<std::mutex> lk(g_logMu);
    g_log.push_back(buf);
    if (g_printLog) {
        char utf8[4096] = {};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
        printf("  log: %s\n", utf8);
    }
}
static wchar_t g_dataDir[MAX_PATH] = L"";
std::wstring AppDataDir() { return g_dataDir; }   // data-start_menu_run\ next to the test: the verdict file is the test's
ForegroundRight TakeForegroundRight() { return ForegroundRight::Had; }   // nothing goes out

// ---- simulated Start: visible from a virtual time on (-1: not coming)
static std::atomic<int64_t> g_visibleAtUs{ -1 };
static std::atomic<int> g_cancelled{ 0 };   // a Start on its way stopped by a second request
static bool StartUp() { const int64_t at = g_visibleAtUs.load(); return at >= 0 && g_virtUs.load() >= at; }
static void BringStart(double after) {
    if (g_visibleAtUs.load() < 0) g_visibleAtUs = g_virtUs.load() + static_cast<int64_t>(after * 1e6);
}
static void CloseStart() { g_visibleAtUs = -1; }

// ---- the Windows key
static std::atomic<int> g_keys{ 0 };
static bool g_keyBrings = true;
bool FakeKeyCombo(std::initializer_list<WORD>) {
    ++g_keys;
    if (StartUp()) CloseStart(); else if (g_keyBrings) BringStart(0.08);
    return true;
}

// ---- the window in front: a hidden window of the test's own, "named" like the program the test wants
// (g_otherWnd: another one of the test's own, as another program's window coming in front)
static HWND g_shellWnd = nullptr, g_firstWnd = nullptr, g_otherWnd = nullptr;
static std::atomic<HWND> g_frontWnd{ nullptr };
static std::atomic<bool> g_explorerUp{ true };
static wchar_t g_frontExe[MAX_PATH] = L"C:\\Program Files\\Test\\notepad.exe";
HWND WINAPI FakeShellWindow() { return g_explorerUp ? g_shellWnd : nullptr; }
HWND WINAPI FakeForegroundWindow() { return g_frontWnd; }
// the processes and tokens start_menu.cpp opens (the real ones, of the test's own process), counted;
// g_openRefused: the program in front may not be looked into (as some system processes)
static std::atomic<int> g_processesOpened{ 0 }, g_tokensOpened{ 0 };
static std::atomic<bool> g_openRefused{ false };
HANDLE WINAPI FakeOpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    ++g_processesOpened;
    if (g_openRefused) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    return OpenProcess(access, inherit, pid);
}
BOOL WINAPI FakeOpenProcessToken(HANDLE process, DWORD access, PHANDLE token) {
    ++g_tokensOpened;
    return OpenProcessToken(process, access, token);
}
BOOL WINAPI FakeQueryFullProcessImageNameW(HANDLE, DWORD, LPWSTR name, PDWORD size) {
    if (wcslen(g_frontExe) + 1 > *size) return FALSE;
    wcscpy_s(name, *size, g_frontExe);
    *size = static_cast<DWORD>(wcslen(g_frontExe));
    return TRUE;
}
static std::atomic<int> g_rightsGiven{ 0 };
BOOL WINAPI FakeAllowSetForegroundWindow(DWORD) { ++g_rightsGiven; return TRUE; }

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
static std::atomic<bool> g_blind{ false };
static std::atomic<int> g_looks{ 0 };
struct FakeVis : Counted<IVisibility> {
    const IID& Iid() const override { return kAppVisibilityIid; }
    HRESULT STDMETHODCALLTYPE GetAppVisibilityOnMonitor(HMONITOR, int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsLauncherVisible(BOOL* v) override { ++g_looks; MaybeHang(g_hangVisLooks); *v = !g_blind && StartUp(); return S_OK; }
} g_vis;

struct FakeWhere : Counted<IUnknown> {
    const IID& Iid() const override { return IID_IUnknown; }
} g_where;

struct FakeMonitors : Counted<IMonitorService> {
    const IID& Iid() const override { return kMonitorServiceIid; }
    HRESULT STDMETHODCALLTYPE GetCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetConnectedCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAt(UINT, IUnknown**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFromHandle(HMONITOR, IUnknown** out) override {
        MaybeHang(g_hangWheres);
        g_where.AddRef(); *out = &g_where; return S_OK;
    }
} g_monitors;

static std::atomic<int> g_shows{ 0 }, g_connects{ 0 }, g_services{ 0 };   // requests; launchers moved to a monitor; services asked for
static HRESULT g_showHr = S_OK;
static bool g_showBrings = true;
static double g_showDelay = 0.12;
struct FakeLauncher : Counted<ILauncher> {
    const IID& Iid() const override { return kLauncherIid; }
    HRESULT STDMETHODCALLTYPE ShowStartView(int, int) override {
        ++g_shows;
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
    HRESULT STDMETHODCALLTYPE IsVisible(BOOL* v) override { *v = !g_blind && StartUp(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStartButtonPressed(int, int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetForeground() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ConnectToMonitor(IUnknown*) override { ++g_connects; return S_OK; }
} g_launcher;

struct FakeShell : Counted<IServiceProvider> {
    const IID& Iid() const override { return IID_IServiceProvider; }
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID iid, void** out) override {
        ++g_services;
        *out = nullptr;
        if (service == kLauncherService && iid == kLauncherIid) { g_launcher.AddRef(); *out = static_cast<ILauncher*>(&g_launcher); return S_OK; }
        if (service == kMonitorService && iid == kMonitorServiceIid) { g_monitors.AddRef(); *out = static_cast<IMonitorService*>(&g_monitors); return S_OK; }
        return E_NOINTERFACE;
    }
} g_shell;

static std::atomic<int> g_shellMade{ 0 }, g_shellMadeWithoutExplorer{ 0 }, g_visMade{ 0 };
static HRESULT g_shellMakeHr = S_OK;
static double g_visMakeTook = 0;
HRESULT WINAPI FakeCoCreateInstance(REFCLSID clsid, LPUNKNOWN, DWORD, REFIID, LPVOID* out) {
    *out = nullptr;
    if (clsid == kAppVisibility) {
        ++g_visMade;
        MaybeHang(g_hangVisMakes);
        Advance(g_visMakeTook);
        g_vis.AddRef(); *out = static_cast<IVisibility*>(&g_vis); return S_OK;
    }
    if (clsid == kImmersiveShell) {
        ++g_shellMade;
        if (!g_explorerUp) ++g_shellMadeWithoutExplorer;
        MaybeHang(g_hangShellMakes);
        if (FAILED(g_shellMakeHr)) return g_shellMakeHr;
        g_shell.AddRef(); *out = static_cast<IServiceProvider*>(&g_shell); return S_OK;
    }
    return REGDB_E_CLASSNOTREG;
}
static std::atomic<int> g_threadsDone{ 0 };   // a worker (or the closing thread) left its apartment: it is through
void WINAPI FakeCoUninitialize() { CoUninitialize(); ++g_threadsDone; }

// ---- the taskbar's own Start button (taskbar_start.h)
static std::atomic<int> g_finds{ 0 }, g_presses{ 0 };
static taskbarstart::Found g_found = taskbarstart::Found::NoButton;
static bool g_kept = false;
static taskbarstart::Pressed g_pressed;   // what a press answers
static bool g_pressBrings = true;
static double g_pressDelay = 0.1, g_pressTook = 0.01;
namespace taskbarstart {
HWND TaskbarOn(HMONITOR) { return nullptr; }
void Remember(HWND) {}
const wchar_t* FoundText(Found f) { return f == Found::Kept ? L"kept" : f == Found::Now ? L"found at the click" : L"none"; }
Found Find(HMONITOR) { ++g_finds; return g_found; }
bool Kept(HMONITOR) { return g_kept; }
bool Classic() { return false; }
std::wstring Describe() { return L""; }
Pressed Press(HMONITOR) {
    ++g_presses;
    MaybeHang(g_hangPresses);
    Advance(g_pressTook);
    Pressed p = g_pressed;
    p.took = g_pressTook;
    if (p.ok || p.unsure) {   // like a click on the taskbar's button: it toggles Start
        if (StartUp()) CloseStart();
        else if (g_pressBrings) BringStart(g_pressDelay);
    }
    return p;
}
}   // namespace taskbarstart

// ---- checks
static int checks = 0, failures = 0;
static void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
}
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
static std::string Ascii(const std::wstring& w) { std::string s; for (wchar_t c : w) s += static_cast<char>(c); return s; }

// As the dock starts: nothing learned, nothing kept, Explorer up, a plain program in front, every fake answering
// as on a test PC where both work (the taskbar's button found and working, the shell taking requests).
static void Fresh() {
    WaitIdle();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pref = Preference{};
        g_reportedFirst = false;
        g_verdictLoaded = false;
    }
    DeleteFileW(VerdictPath().c_str());
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
    g_blind = false;
    CloseStart();
    g_cancelled = 0;
    g_keys = 0; g_keyBrings = true;
    g_shows = 0; g_showHr = S_OK; g_showBrings = true; g_showDelay = 0.12; g_connects = 0; g_services = 0;
    g_shellMade = 0; g_shellMadeWithoutExplorer = 0; g_shellMakeHr = S_OK; g_visMakeTook = 0; g_visMade = 0;
    g_finds = 0; g_presses = 0; g_found = taskbarstart::Found::Now; g_kept = true;
    g_pressed = taskbarstart::Pressed{}; g_pressed.ok = true; g_pressed.hr = S_OK; g_pressed.how = L"toggle";
    g_pressBrings = true; g_pressDelay = 0.1; g_pressTook = 0.01;
}
static bool ClickOpen(HMONITOR m) { Click(m, false); return WaitIdle(); }
static TaskbarState TaskbarNow() { std::lock_guard<std::mutex> lk(g_mu); return g_pref.taskbar; }

static void Tests(HMONITOR primary) {
    // ---- the usual click: the taskbar's button found and pressed, Start comes by it
    Fresh();
    size_t log = LogSize();
    Check(ClickOpen(primary), "a click's worker ends by itself");
    Check(g_presses == 1 && g_shows == 0 && g_keys == 0 && StartUp(), "the taskbar's button brings Start: pressed once, nothing else sent");
    Check(g_shellMade == 0, "...and the shell is not even reached (only when needed behind the button)");
    Check(TaskbarNow() == TaskbarState::Works, "...and from now on the button is trusted");
    const std::string kept = ReadVerdict();
    const std::string expect = "works " + Ascii(WindowsBuild()) + " " + std::to_string(Today());
    Check(kept == expect, ("...and the verdict is kept in data\\taskbar-start.txt as \"" + expect + "\" (found \"" + kept + "\")").c_str());
    Check(LogHas(log, L"Start came by the taskbar's Start button"), "dock.log names the way Start came, once");
    Check(g_rightsGiven >= 1, "an opening click hands the right to come in front to the shell");
    Check(NoLeak(), "no reference to the shell's objects is left over");

    // ---- Start open (its search box in front, as on 25H2): the trusted button closes it, on a thread of its own
    wcscpy_s(g_frontExe, L"C:\\Windows\\SystemApps\\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\\SearchHost.exe");
    Check(InFront(), "Start up: the press-time check sees it (IAppVisibility)");
    const int presses = g_presses, done = g_threadsDone;
    Click(primary, true);
    Check(WaitFor([&] { return g_threadsDone > done; }) && g_presses == presses + 1 && g_keys == 0 && !StartUp(),
          "a click with Start open: the taskbar's button closes it, no Windows key");
    // the press does not go through: the key closes Start, unless Start went by itself
    BringStart(0); Advance(0.01);
    g_pressed.ok = false; g_pressed.hr = E_FAIL; g_pressed.how = L"no way to press it";
    const int d2 = g_threadsDone;
    log = LogSize();
    Click(primary, true);
    Check(WaitFor([&] { return g_threadsDone > d2; }) && g_keys == 1 && !StartUp() && LogHas(log, L"the Windows key closed it"),
          "...a press that did not go through: the Windows key closes Start instead");
    Check(NoLeak(), "...no reference left over");

    // ---- Explorer not up (restarting): nothing of it is touched, the key goes out once
    Fresh();
    g_explorerUp = false;
    g_keyBrings = false;   // so the click logs what it saw
    log = LogSize();
    Check(ClickOpen(primary), "Explorer not up: the worker ends");
    Check(g_finds == 0 && g_presses == 0 && g_shellMade == 0 && g_keys == 1, "...without looking for the taskbar's button or reaching the shell; one key");
    Check(LogHas(log, L"taskbar: Explorer is not up") && LogHas(log, L"shell: not available (80070426)"), "...and dock.log says why");

    // ---- no button: the shell is asked once, at once
    Fresh();
    g_found = taskbarstart::Found::NoButton;
    log = LogSize();
    Check(ClickOpen(primary) && g_shows == 1 && g_keys == 0 && g_presses == 0 && StartUp(), "no taskbar button: one request to the shell brings Start, no key");
    Check(LogHas(log, L"Start came by the shell's request"), "...and dock.log names the shell's request");
    Check(NoLeak(), "...no reference left over");

    // ---- the press times out: it may still arrive, so nothing else is sent; nothing learned
    Fresh();
    g_pressed.ok = false; g_pressed.unsure = true; g_pressed.hr = static_cast<HRESULT>(0x80131505); g_pressed.how = L"toggle";
    g_pressBrings = false;
    const double t0 = NowSeconds();
    log = LogSize();
    Check(ClickOpen(primary), "a press that timed out: the worker ends");
    Check(g_shows == 0 && g_keys == 0 && g_shellMade == 0, "...nothing else sent (a late press plus a request would toggle Start twice)");
    Check(NowSeconds() - t0 >= kWaitAccepted - 0.1, "...after waiting kWaitAccepted on it");
    Check(TaskbarNow() == TaskbarState::Unknown && ReadVerdict() == "(none)", "...and nothing learned or kept");
    Check(LogHas(log, L"unsure whether it arrived"), "...and dock.log says so");

    // ---- a slow press (Explorer busy): waited on like a trusted one, no fallback at kTaskbarProof
    Fresh();
    g_pressTook = 0.3; g_pressDelay = 3.0;
    Check(ClickOpen(primary) && StartUp() && g_shows == 0 && g_keys == 0, "a slow press: a Start that takes 3 s comes by it alone (Explorer busy)");
    Fresh();
    g_visMakeTook = 0.3; g_pressDelay = 3.0;
    Check(ClickOpen(primary) && StartUp() && g_shows == 0 && g_keys == 0, "IAppVisibility slow to make: the same (Explorer busy)");

    // ---- the shell, reached only when needed behind the button, cannot be had: the key at once
    Fresh();
    g_pressed.ok = false; g_pressed.hr = E_FAIL; g_pressed.how = L"no way to press it";
    g_shellMakeHr = CO_E_SERVER_EXEC_FAILURE;
    Check(ClickOpen(primary) && g_shellMade == 1 && g_shows == 0 && g_keys == 1 && StartUp(), "the press did not go through and the shell cannot be had: the Windows key");
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Check(g_pref.misses == 0 && g_pref.shellFirst, "...and the shell is not blamed");
    }

    // ---- Start closed with the button while a wait runs: the wait ends, nothing more is sent
    Fresh();
    g_pressBrings = false;   // the press brings nothing; Start is opened meanwhile by the user's own key
    g_pauseAtUs = g_virtUs.load() + 300000;
    Click(primary, false);
    WaitForSingleObject(g_paused, 20000);
    BringStart(0); Advance(0.001);
    wcscpy_s(g_frontExe, L"C:\\Windows\\SystemApps\\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\\SearchHost.exe");
    g_kept = false;   // closed by the key
    Click(primary, true);
    SetEvent(g_resume);
    Check(WaitIdle() && g_shows == 0 && g_keys == 1 && !StartUp(), "Start closed while a wait runs: the wait ends at once, nothing sent after the key that closed it");

    // ---- the taskbar's button does nothing here: two clear misses and it is left alone, kept for kVerdictDays
    Fresh();
    g_pressBrings = false;   // the press goes through and brings nothing; the shell's request, 2 s later, brings Start at once
    ClickOpen(primary);
    Check(StartUp() && g_presses == 1 && g_shows == 1 && g_keys == 0, "the press brought nothing: the shell's request kTaskbarProof later brings Start");
    CloseStart();
    log = LogSize();
    ClickOpen(primary);
    CloseStart();
    Check(TaskbarNow() == TaskbarState::Failed && LogHas(log, L"did not bring Start 2 clicks in a row"), "...twice: the button is left alone");
    const std::string failed = "failed " + Ascii(WindowsBuild()) + " " + std::to_string(Today());
    Check(ReadVerdict() == failed, ("...and that is kept as \"" + failed + "\"").c_str());
    const int finds = g_finds;
    ClickOpen(primary);
    Check(g_finds == finds && g_shows == 3 && StartUp(), "...the next click asks the shell without looking for the button");
    CloseStart();
    {   // the next run reads it back, and tries the button again kVerdictDays later
        std::lock_guard<std::mutex> lk(g_mu);
        g_pref = Preference{}; g_verdictLoaded = false;
        LoadVerdict();
        Check(g_pref.taskbar == TaskbarState::Failed, "the next run reads the failure back");
        g_pref = Preference{}; g_verdictLoaded = false;
        g_dayShift = kVerdictDays;
        LoadVerdict();
        Check(g_pref.taskbar == TaskbarState::Unknown, "...a run kVerdictDays later tries the button again");
        g_dayShift = 0;
    }
    {   // the same within one run that lasts kVerdictDays (the dock runs for days: a laptop sleeps, it does not restart)
        Fresh();
        g_pressBrings = false;
        ClickOpen(primary); CloseStart();
        ClickOpen(primary); CloseStart();   // left alone, kept for kVerdictDays (as above)
        const int f0 = g_finds, p0 = g_presses;
        g_dayShift = kVerdictDays;          // the dock still runs kVerdictDays later
        g_pressBrings = true;
        log = LogSize();
        ClickOpen(primary);
        Check(TaskbarNow() == TaskbarState::Works && g_finds == f0 + 1 && g_presses == p0 + 1,
              "a failure learned kVerdictDays ago in this same run: the button is tried again, as the log promised (\"tried again in 3 days\")");
        Check(LogHas(log, L"left alone 3 days ago") && ReadVerdict() == "works " + Ascii(WindowsBuild()) + " " + std::to_string(Today()),
              "...dock.log says so, and what it does now is kept");
        CloseStart();
        g_dayShift = 0;
    }
    {   // ...not a day sooner; tried again, it starts from no misses: one more is not a failure yet, two are, kept anew
        Fresh();
        g_pressBrings = false;
        ClickOpen(primary); CloseStart();
        ClickOpen(primary); CloseStart();
        const int f0 = g_finds;
        g_dayShift = kVerdictDays - 1;
        ClickOpen(primary); CloseStart();
        Check(g_finds == f0 && TaskbarNow() == TaskbarState::Failed, "a failure learned kVerdictDays - 1 ago in this run: still left alone");
        g_dayShift = kVerdictDays;
        ClickOpen(primary); CloseStart();
        Check(g_finds == f0 + 1 && TaskbarNow() == TaskbarState::Unknown, "...tried again kVerdictDays later: one miss leaves it checked, not left alone at once");
        ClickOpen(primary); CloseStart();
        const std::string again = "failed " + Ascii(WindowsBuild()) + " " + std::to_string(Today());
        Check(TaskbarNow() == TaskbarState::Failed && ReadVerdict() == again, ("...two: left alone again, kept as \"" + again + "\" (from that day on)").c_str());
        g_dayShift = 0;
    }
    {   // a failure read back from an earlier run (a day old) runs out within this run as well
        Fresh();
        WriteVerdict("failed " + Ascii(WindowsBuild()) + " " + std::to_string(Today() - 1));
        const int f0 = g_finds;
        ClickOpen(primary); CloseStart();
        Check(g_finds == f0 && TaskbarNow() == TaskbarState::Failed, "a failure kept a day ago by an earlier run: left alone");
        g_dayShift = kVerdictDays - 1;   // kVerdictDays since it was kept
        ClickOpen(primary); CloseStart();
        Check(g_finds == f0 + 1 && TaskbarNow() == TaskbarState::Works, "...the same run, kVerdictDays after it was kept: tried again, and it works");
        g_dayShift = 0;
    }
    {   // the clock went back since the failure was kept: tried again (as a run reading the file would), and dock.log
        // says why rather than "left alone -2 days ago"
        Fresh();
        g_pressBrings = false;
        ClickOpen(primary); CloseStart();
        ClickOpen(primary); CloseStart();
        const int f0 = g_finds;
        g_dayShift = -2;
        log = LogSize();
        ClickOpen(primary); CloseStart();
        Check(g_finds == f0 + 1 && LogHas(log, L"left alone on a later day than today (the clock went back)") && !LogHas(log, L"left alone -"),
              "a failure kept on a day still to come (the clock went back): tried again, and dock.log says the clock went back");
        g_dayShift = 0;
    }

    // ---- a failure learned in Windows' first minutes is not kept
    Fresh();
    g_tickBaseMs = 60 * 1000 - g_virtUs.load() / 1000;   // Windows up one minute
    g_pressBrings = false;
    ClickOpen(primary); CloseStart();
    log = LogSize();
    ClickOpen(primary); CloseStart();
    Check(TaskbarNow() == TaskbarState::Failed && ReadVerdict() == "(none)" && LogHas(log, L"not kept"),
          "a failure learned minutes after Windows started: left alone for this run, not kept");
    // ---- nor in Explorer's: Windows up for days - fast startup (Windows' default) turns a shutdown into hibernation,
    // and its uptime runs on across it - while Explorer started a minute ago, at logon
    Fresh();
    g_explorerAgeS = 60;
    g_pressBrings = false;
    ClickOpen(primary); CloseStart();
    log = LogSize();
    ClickOpen(primary); CloseStart();
    Check(TaskbarNow() == TaskbarState::Failed && ReadVerdict() == "(none)" && LogHas(log, L"Explorer 60 s") && LogHas(log, L"not kept"),
          "Windows up for days, Explorer started a minute ago (logon after fast startup): left alone for this run, not kept");
    {
        const int f0 = g_finds;
        g_dayShift = kVerdictDays;
        ClickOpen(primary); CloseStart();
        Check(g_finds == f0 && TaskbarNow() == TaskbarState::Failed, "...for this run only: not tried again kVerdictDays later either (it was never kept)");
        g_dayShift = 0;
    }
    // ---- Explorer's age not to be had: Windows' uptime alone decides, as before
    Fresh();
    g_explorerAgeS = -1;
    g_pressBrings = false;
    ClickOpen(primary); CloseStart();
    ClickOpen(primary); CloseStart();
    Check(TaskbarNow() == TaskbarState::Failed && ReadVerdict() == failed, "Explorer's age not to be had, Windows up for days: the failure is kept");
    Fresh();   // ...Windows up one minute: not kept, and dock.log says Explorer's age is unknown (not "Explorer -1 s")
    g_tickBaseMs = 60 * 1000 - g_virtUs.load() / 1000;
    g_explorerAgeS = -1;
    g_pressBrings = false;
    ClickOpen(primary); CloseStart();
    log = LogSize();
    ClickOpen(primary); CloseStart();
    Check(TaskbarNow() == TaskbarState::Failed && ReadVerdict() == "(none)" && LogHas(log, L"Explorer's age unknown: started minutes ago, not kept") &&
          !LogHas(log, L"Explorer -1"),
          "...Explorer's age not to be had, Windows up one minute: not kept, and dock.log says the age is unknown");

    // ---- a worker stuck on a hung Explorer is left behind by the next click; once Explorer answers again it must
    // send nothing more: the new click's request is on its way (a second request stops it - seen on a test PC, mod.17)
    Fresh();
    g_found = taskbarstart::Found::NoButton;   // the shell first (no button to press)
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
        g_found = taskbarstart::Found::NoButton;
        g_showDelay = 1.0;
        g_hangShellMakes = 2;
        const int d0 = g_threadsDone;
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 2; });
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_shows == 1 && g_cancelled == 0 && g_keys == 0 && StartUp(),
              "both clicks' workers hung on Explorer, answered at once: one request to the shell in all, and Start comes");
        Check(g_connects == 1 && g_services == 2 && g_visMade == 1, "...the one left behind asks Explorer nothing more (the launcher connected once)");
        Check(NoLeak(), "...no reference left over");
    }
    {   // Explorer hangs the first click's worker later on its way to the shell (finding the dock's monitor): once answered,
        // it does not move Explorer's one launcher to its monitor - the later click's Start came there meanwhile
        Fresh();
        g_found = taskbarstart::Found::NoButton;
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
    {   // the trusted taskbar button, both workers hung on their first look (IsLauncherVisible), answered at once:
        // the one left behind does not press after that look
        Fresh();
        { std::lock_guard<std::mutex> lk(g_mu); g_pref.taskbar = TaskbarState::Works; }
        g_hangVisLooks = 2;
        const int d0 = g_threadsDone;
        log = LogSize();
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 2; });
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_presses == 1 && g_shows == 0 && g_keys == 0 && StartUp(),
              "the trusted taskbar button, both workers hung looking whether Start is up, answered at once: pressed once (a second press would close Start)");
        Check(TaskbarNow() == TaskbarState::Works && LogHas(log, L"nothing learned"), "...and the one left behind learns nothing");
        Check(NoLeak(), "...no reference left over");
    }
    {   // the same with the trusted taskbar button (where it works), both hung making IAppVisibility: a second press would toggle
        Fresh();
        { std::lock_guard<std::mutex> lk(g_mu); g_pref.taskbar = TaskbarState::Works; }
        g_hangVisMakes = 2;
        const int d0 = g_threadsDone;
        log = LogSize();
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 2; });
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_presses == 1 && g_shows == 0 && g_keys == 0 && StartUp(),
              "the trusted taskbar button, both workers hung on Explorer and answered at once: pressed once (a second press would close Start)");
        Check(TaskbarNow() == TaskbarState::Works && LogHas(log, L"nothing learned"), "...and the one left behind learns nothing");
        Check(NoLeak(), "...no reference left over");
    }
    {   // Explorer answers the one left behind only once the later click is over and Start was closed: it stays closed
        Fresh();
        g_found = taskbarstart::Found::NoButton;
        g_hangShellMakes = 1;
        const int d0 = g_threadsDone;
        Click(primary, false);
        WaitFor([] { return g_hanging.load() >= 1; });
        Advance(kStuck + 1);
        Check(ClickOpen(primary) && g_shows == 1 && StartUp(), "a click after kStuck, Explorer answering it: Start comes");
        CloseStart();   // the user closes it
        SetEvent(g_unblock);
        Check(WaitFor([&] { return g_threadsDone.load() >= d0 + 2; }) && g_shows == 1 && g_keys == 0 && !StartUp(),
              "...the worker left behind, answered after that, does not bring Start back");
        Check(NoLeak(), "...no reference left over");
    }
}

// The window in front, as the worker's looks read it (Visible, every kLook while it waits): a window's program never
// changes, so only a window that came in front since the last look has its program looked into; whether a program runs
// as administrator is asked once per click (can the Windows key reach Start), not at each look nor at the press-time
// checks on the UI thread. Counted: the processes and tokens opened.
static const wchar_t kStartExe[] = L"C:\\Windows\\SystemApps\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\StartMenuExperienceHost.exe";
static void FrontTests(HMONITOR primary) {
    SelfElevated();   // its one look at the test's own token (once per process; a test run as administrator) is not counted below
    // ---- a long wait: the press timed out (it may still arrive) and Start does not come - kWaitAccepted of looks
    Fresh();
    g_pressed.ok = false; g_pressed.unsure = true; g_pressed.hr = static_cast<HRESULT>(0x80131505); g_pressed.how = L"toggle";
    g_pressBrings = false;
    int looks = g_looks, opened = g_processesOpened, tokens = g_tokensOpened;
    Check(ClickOpen(primary) && !StartUp() && g_shows == 0 && g_keys == 0, "a wait of kWaitAccepted on a press that timed out: nothing comes, nothing else sent");
    looks = g_looks - looks; opened = g_processesOpened - opened; tokens = g_tokensOpened - tokens;
    printf("  a wait of %.0f s: %d looks, %d processes opened, %d tokens\n", kWaitAccepted, looks, opened, tokens);
    Check(looks > 200 && opened == 2 && tokens == 1,
          "...the program in front looked into at the click and at the wait's first look, not at each look; whether it runs as administrator asked once");

    // ---- the usual click: Start comes by the press after a few looks
    Fresh();
    looks = g_looks; opened = g_processesOpened; tokens = g_tokensOpened;
    Check(ClickOpen(primary) && StartUp() && g_presses == 1, "the usual click: Start comes by the press");
    looks = g_looks - looks; opened = g_processesOpened - opened; tokens = g_tokensOpened - tokens;
    printf("  the usual click: %d looks, %d processes opened, %d tokens\n", looks, opened, tokens);
    Check(opened <= 2 && tokens == 1, "...the program in front looked into at most twice, whether it runs as administrator once");

    // ---- the press-time checks on the UI thread: the program in front, never its token
    tokens = g_tokensOpened;
    CloseStart();
    Check(!InFront(), "Start not up: the press-time check says so");
    g_blind = true;   // IAppVisibility says nothing: Start's own window in front tells
    g_frontWnd = g_otherWnd;
    wcscpy_s(g_frontExe, kStartExe);
    Check(InFront(), "...Start's own window in front, IAppVisibility saying nothing: it sees Start");
    Check(g_tokensOpened == tokens, "...and asks nothing about administrator");

    // ---- another window comes in front during a wait: it is looked into (Start's, IAppVisibility saying nothing)
    Fresh();
    g_blind = true;
    g_pressBrings = false;   // the press goes through; Start's window comes in front 0.3 s later
    g_pauseAtUs = g_virtUs.load() + 300000;
    Click(primary, false);
    WaitForSingleObject(g_paused, 20000);
    wcscpy_s(g_frontExe, kStartExe);
    g_frontWnd = g_otherWnd;
    double t0 = NowSeconds();
    SetEvent(g_resume);
    Check(WaitIdle() && NowSeconds() - t0 < 0.1 && g_shows == 0 && g_keys == 0 && TaskbarNow() == TaskbarState::Works,
          "Start's window comes in front during a wait, IAppVisibility saying nothing: the next look sees Start (no request, no key)");

    // ---- a program in front not to be looked into is tried again at the next look: only a name read is kept
    Fresh();
    g_blind = true;
    g_pressBrings = false;
    wcscpy_s(g_frontExe, kStartExe);
    g_openRefused = true;
    g_pauseAtUs = g_virtUs.load() + 300000;
    Click(primary, false);
    WaitForSingleObject(g_paused, 20000);
    g_openRefused = false;
    t0 = NowSeconds();
    SetEvent(g_resume);
    Check(WaitIdle() && NowSeconds() - t0 < 0.1 && g_shows == 0 && g_keys == 0 && TaskbarNow() == TaskbarState::Works,
          "Start's window in front, its process refused at first: once it can be opened, the next look sees Start (a refusal is not kept)");
    Check(NoLeak(), "...no reference left over");
}

// The verdict file as the next run's first click reads it.
static void VerdictFileTests() {
    const std::string b = Ascii(WindowsBuild()), d = std::to_string(Today());
    auto load = [](const std::string& text, bool write = true) {
        if (write) WriteVerdict(text);
        std::lock_guard<std::mutex> lk(g_mu);
        g_pref = Preference{}; g_verdictLoaded = false;
        LoadVerdict();
        return g_pref.taskbar;
    };
    Fresh();
    Check(load("works " + b + " " + d) == TaskbarState::Works, "kept \"works\" read back");
    Check(load("works " + b + " " + d + "\r\n") == TaskbarState::Works, "...with a line end (an edited file)");
    Check(load("failed " + b + " " + d) == TaskbarState::Failed, "kept \"failed\" read back");
    Check(load("failed " + b + " " + std::to_string(Today() - kVerdictDays)) == TaskbarState::Unknown, "...an old failure is tried again");
    Check(load("failed " + b + " " + std::to_string(Today() + 5)) == TaskbarState::Unknown, "...one from the future (the clock went back) too");
    Check(load("works 99999.1 " + d) == TaskbarState::Unknown, "...another Windows build is checked afresh");
    Check(load("") == TaskbarState::Unknown && load(std::string(300, 'x')) == TaskbarState::Unknown, "...an empty or overlong file counts as nothing kept");
    Check(load("\xEF\xBB\xBFworks " + b + " " + d) == TaskbarState::Unknown && load(std::string("works ") + b + std::string(1, '\0') + d) == TaskbarState::Unknown,
          "...a byte-order mark or a NUL counts as nothing kept (checked afresh, never trusted wrongly)");
    Check(load("works " + b.substr(0, b.size() - 1)) == TaskbarState::Unknown, "...a file cut short counts as nothing kept");
    DeleteFileW(VerdictPath().c_str());
    Check(load("", false) == TaskbarState::Unknown, "...no file: nothing kept");
    {   // written, overwritten shorter, cleared
        std::lock_guard<std::mutex> lk(g_mu);
        SaveVerdict(TaskbarState::Failed);
        SaveVerdict(TaskbarState::Works);
    }
    Check(ReadVerdict() == "works " + b + " " + d, "a verdict overwrites the last one whole (no tail of a longer one left)");
    {
        std::lock_guard<std::mutex> lk(g_mu);
        SaveVerdict(TaskbarState::Unknown);
    }
    Check(ReadVerdict() == "(none)", "...and nothing proven either way removes it");
    // a folder in the file's place: neither read nor written, nothing breaks
    CreateDirectoryW(VerdictPath().c_str(), nullptr);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        SaveVerdict(TaskbarState::Works);
    }
    Check(load("", false) == TaskbarState::Unknown, "a folder where the file should be: nothing read, nothing breaks");
    RemoveDirectoryW(VerdictPath().c_str());
}

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_paused = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_resume = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_unblock = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // the test's own folder for the verdict file
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
    g_frontWnd = g_firstWnd;
    const HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    VerdictFileTests();
    Tests(primary);
    FrontTests(primary);
    WaitIdle();
    DeleteFileW(VerdictPath().c_str());
    printf("start_menu_run_test: %s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", checks, failures, failures == 1 ? "" : "s");
    fflush(stdout);
    ExitProcess(static_cast<UINT>(failures));   // workers left behind may still sit in a fake call
}
