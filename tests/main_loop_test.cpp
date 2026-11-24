// The dock's entry point and main loop (src/main.cpp, with src/main_autostart.cpp), compiled under fakes with wWinMain renamed (as
// sensors_loop_test does with the helper's): the command line, the start, BuildDocks, the main window's messages
// (MainProc), a pass of the main loop and hours of them on a fake clock, SetAutostart, ApplyRevealHotkey and the
// exit. Characterization test: it fixes what main.cpp calls, in which order, with which
// arguments, how long it waits and what it writes to dock.log, so a change of main.cpp that changes any of it fails here.
// main.cpp's file statics (g_docks, g_rebuildAt, MainProc, BuildDocks ...) are reached through the include.
// Everything around it is a stand-in: the docks (FakeDock: no window, no GPU), the app model, system, sensors,
// GPU and taskbar objects (renamed by #define), the other modules (link stubs that record), main_instance.cpp
// (stubs: tests\main_instance_test.cpp tests it), the monitors, the pointer, the message queue, the clock, the
// registry, processes, the instance mutex, hotkeys, windows and power notifications. Nothing outside this
// process is read, started, registered or sent to: FindWindowW never sees the running dock, the Run key and
// schtasks are fakes, the logon task's XML goes to a folder of the test (build\tests\main_loop_tmp).
// Built twice by run_regressions.bat: as the release dock (main_loop_test) and with /DLD_DEV (main_loop_dev_test:
// the developer flags of a dev build's command line).
// Linked without user32, advapi32 and ole32 (and /NODEFAULTLIB for them): a call of theirs that no fake below catches
// (main.cpp split or given a new call) is then a link error, not a real call from a test that passes.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\main_loop_test.cpp /link /NODEFAULTLIB:user32.lib /NODEFAULTLIB:advapi32.lib /NODEFAULTLIB:ole32.lib
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/dock.h"
#include "../src/apps.h"
#include "../src/system.h"
#include "../src/capture.h"
#include "../src/sensors.h"
#include "../src/admin_helper.h"
#include "../src/native_tray.h"
#include "../src/elevation.h"
#include <dwmapi.h>
#include <climits>
#include <tlhelp32.h>
#include <psapi.h>
#include "../src/oled.h"
#include "../src/buttons.h"
#include "../src/settings_window.h"
#include "../src/tray_host.h"
#include "../src/explorer_taskbar.h"
#include "../src/taskbar_visibility.h"
#include "../src/idle_wait.h"
#include "../src/version.h"
#include "../src/main_internal.h"
#include <cstdio>
#include <cstdarg>
#include <deque>
#include <map>
#include <fstream>
#include <iterator>
#include "portable_check.h"

#ifdef LD_DEV
#define ML_NAME "main_loop_dev_test"
#else
#define ML_NAME "main_loop_test"
#endif

// ---------------------------------------------------------------- the test's world
namespace ml {

std::vector<std::wstring> trace;   // the calls, in order (while tracing)
bool tracing = true;
std::vector<std::wstring> logs;    // dock.log lines

static void Rec(const wchar_t* fmt, ...) {
    if (!tracing) return;
    wchar_t buf[1200];
    va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a); va_end(a);
    trace.push_back(buf);
}

// what a pass of the loop called, counted whether tracing or not
struct Counts {
    int peek = 0, cursor = 0, monFromPoint = 0, monInfo = 0, waits = 0, dwmFlush = 0, restore = 0, hide = 0, now = 0,
        update = 0, render = 0, deviceLost = 0, reset = 0, buildDocks = 0;
    std::map<DWORD, int> waitMs;   // MsgWaitForMultipleObjectsEx by its timeout
};
Counts n;

long long nowUs = 1000000;   // the clock (NowSeconds), in microseconds
static double Now() { return nowUs / 1e6; }

struct Mon { HMONITOR h; RECT rc; RECT work; bool primary; const wchar_t* name; };
std::vector<Mon> mons;
const Mon kDesk = { (HMONITOR)0xA001, { 0, 0, 2560, 1440 }, { 0, 0, 2560, 1440 }, true, L"desk" };
const Mon kLaptop = { (HMONITOR)0xA002, { 0, 0, 2880, 1800 }, { 0, 0, 2880, 1800 }, true, L"laptop" };   // 150 %: the same px to main.cpp
const Mon kPortrait = { (HMONITOR)0xA003, { -1440, -560, 0, 2000 }, { -1440, -560, 0, 2000 }, false, L"portrait" };
bool monInfoFails = false;

static const wchar_t* MonName(HMONITOR h) {
    for (auto& m : mons) if (m.h == h) return m.name;
    return h ? L"?" : L"null";
}

// the pointer
POINT cursor = { 1280, 700 };
bool cursorReadable = true;
DWORD lastError = 0;

// the message queue of the UI thread
std::deque<MSG> queue;
const HWND kMain = (HWND)0x1234, kOther = (HWND)0x5678;
WNDPROC registeredProc = nullptr;
static void Post(UINT m, WPARAM w = 0, LPARAM l = 0, HWND h = kMain) { MSG msg{}; msg.hwnd = h; msg.message = m; msg.wParam = w; msg.lParam = l; queue.push_back(msg); }
static void Quit() { Post(WM_QUIT, 0, 0, nullptr); }

// the passes: afterWait runs at the end of each pass's wait (MsgWaitForMultipleObjectsEx or DwmFlush), the clock
// already advanced by it
int passes = 0;
std::function<void(int)> afterWait;
int maxPasses = 2000000;

// the docks' answers (by index)
struct DockState { bool busy = false, liveGlass = false, fullscreen = false, panel = false, holdsWidget = false, relayoutOk = true, createOk = true; };
DockState dock[4];
struct UpdateCall { int index; double now; float dt; POINT c; bool unseen, forceShow; size_t attention; bool alert; };   // attention, alert: what the dock could see of g_apps.attentionEvents and g_sensors.alertPending
std::vector<UpdateCall> updates;
long long updateCostUs = 0, renderCostUs = 0, appsCostUs = 0, sysCostUs = 0, sensorsCostUs = 0;

// the other modules' answers
bool ffWants = false, ffBusy = false, thRunning = false, thStarts = true, explorerTaskbar = true, oledDisables = true;
bool gfxInits = true, gfxLost = false, gfxResets = true, preTranslate = false;
bool gfxLostAfterFailedReset = true;   // false: as the real Gfx::DeviceLost after a failed Reset (no d3d device: false)
std::function<bool(int)> restoreWorks = [](int) { return true; };
bool watchdogReady = true, watchdogStarts = true, restartUnelevated = false, elevated = false;
int runWatchdogRc = 7, restoreWhenIdleRc = 0;
bool threadsLeft = true;

// processes, the mutex, windows, the registry
HWND otherDock = nullptr;          // FindWindowW(LiquidDockMain): the dock that runs
DWORD otherWait = WAIT_OBJECT_0;   // it ends within 3 s (WAIT_TIMEOUT: hung)
bool openProcessWorks = true;      // false: OpenProcess fails (an elevated dock, --keep-admin, and a --quit run as the user)
bool mutexMade = true; DWORD mutexWait = WAIT_OBJECT_0;
bool windowMade = true, powerNotify = true, uxtheme = true, hklm = true, runKey = true;
std::wstring build = L"26300";
std::map<UINT, bool> hotkeyTaken;  // RegisterHotKey(vk) fails: another program has it
DWORD schtasksCode = 0; bool schtasksStarts = true;
std::wstring tempDir;              // GetTempPathW: the test's folder
std::wstring xmlFile; std::string xmlBytes; bool xmlSeen = false;
std::vector<std::wstring> processes;   // CreateProcessW's command lines
std::wstring runValue; DWORD runValueType = 0, runValueSize = 0;

static void ResetWorld() {
    trace.clear(); tracing = true; logs.clear(); n = Counts{}; nowUs = 1000000;
    mons = { kDesk }; monInfoFails = false;
    cursor = { 1280, 700 }; cursorReadable = true; lastError = 0;
    queue.clear(); registeredProc = nullptr;
    passes = 0; afterWait = nullptr; maxPasses = 2000000;
    for (auto& d : dock) d = DockState{};
    updates.clear(); updateCostUs = renderCostUs = appsCostUs = sysCostUs = sensorsCostUs = 0;
    ffWants = ffBusy = thRunning = false; thStarts = true; explorerTaskbar = true; oledDisables = true;
    gfxInits = true; gfxLost = false; gfxResets = true; preTranslate = false; gfxLostAfterFailedReset = true;
    restoreWorks = [](int) { return true; };
    watchdogReady = watchdogStarts = true; restartUnelevated = false; elevated = false; runWatchdogRc = 7; restoreWhenIdleRc = 0;
    threadsLeft = true;
    otherDock = nullptr; otherWait = WAIT_OBJECT_0; openProcessWorks = true; mutexMade = true; mutexWait = WAIT_OBJECT_0;
    windowMade = powerNotify = uxtheme = hklm = runKey = true; build = L"26300"; hotkeyTaken.clear();
    schtasksCode = 0; schtasksStarts = true; xmlFile.clear(); xmlBytes.clear(); xmlSeen = false; processes.clear();
    runValue.clear(); runValueType = runValueSize = 0;
}

static std::string U8(const std::wstring& s) {
    int k = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(k, '\0');
    if (k) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), o.data(), k, nullptr, nullptr);
    return o;
}

static bool Same(const std::vector<std::wstring>& got, const std::vector<std::wstring>& want, const char* what) {
    const bool ok = got == want;
    if (!ok) {
        std::printf("  got (%zu):\n", got.size());
        for (auto& s : got) std::printf("    %s\n", U8(s).c_str());
        std::printf("  expected (%zu):\n", want.size());
        for (auto& s : want) std::printf("    %s\n", U8(s).c_str());
    }
    return Check(ok, what);
}

static bool IsWait(const std::wstring& s) { return s.rfind(L"MsgWait(", 0) == 0 || s == L"DwmFlush"; }

// the calls of pass k (from 0): after the wait that ended pass k-1, up to and with the wait that ends it
static std::vector<std::wstring> PassTrace(int k) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < trace.size() && trace[i] != L"Peek" && trace[i].rfind(L"Peek:", 0) != 0) ++i;   // the start before pass 0
    for (int p = 0; p < k && i < trace.size(); ++i) if (IsWait(trace[i])) ++p;
    for (; i < trace.size(); ++i) { out.push_back(trace[i]); if (IsWait(trace[i])) break; }
    return out;
}
// the calls before the first look at the message queue (the start)
static std::vector<std::wstring> StartTrace() {
    std::vector<std::wstring> out;
    for (auto& s : trace) { if (s == L"Peek" || s.rfind(L"Peek:", 0) == 0) break; out.push_back(s); }
    return out;
}
// the calls after the last pass's look at the queue that found WM_QUIT
static std::vector<std::wstring> ExitTrace() {
    std::vector<std::wstring> out;
    size_t i = trace.size();
    while (i > 0 && trace[i - 1] != L"Peek:WM_QUIT") --i;
    if (i == 0) return out;
    for (--i; i < trace.size(); ++i) out.push_back(trace[i]);
    return out;
}
static int CountOf(const std::vector<std::wstring>& v, const std::wstring& s) { int k = 0; for (auto& x : v) if (x == s) ++k; return k; }
static int CountPrefix(const std::vector<std::wstring>& v, const std::wstring& s) { int k = 0; for (auto& x : v) if (x.rfind(s, 0) == 0) ++k; return k; }
static bool Has(const std::vector<std::wstring>& v, const std::wstring& s) { return CountOf(v, s) > 0; }
static int IndexOf(const std::vector<std::wstring>& v, const std::wstring& s) { for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return (int)i; return -1; }

// fake handles, named for the trace; anything else is a real handle (the XML file) and goes to the real CloseHandle
std::map<HANDLE, std::wstring> handles;
int handleCount = 0;
static HANDLE NewHandle(const wchar_t* name) {
    HANDLE h = (HANDLE)(ULONG_PTR)(0x7A000000 + 16 * ++handleCount);
    handles[h] = name;
    return h;
}
static std::wstring HName(HANDLE h) { auto it = handles.find(h); return it == handles.end() ? L"real" : it->second; }
static const wchar_t* WName(HWND h) { return h == kMain ? L"main" : h == kOther ? L"other" : h ? L"?" : L"null"; }

}   // namespace ml
using ml::Rec;
using ml::StartTrace;
using ml::ExitTrace;

// ---------------------------------------------------------------- common.cpp's functions (not linked)
double NowSeconds() { ++ml::n.now; return ml::Now(); }
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1200];
    va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a); va_end(a);
    ml::logs.push_back(buf);
    Rec(L"Log: %ls", buf);
}
void LogAppend() { Rec(L"LogAppend"); }
void LogStartSession() { Rec(L"LogStartSession"); }
std::wstring SystemExe(const wchar_t* name) { return L"\"C:\\Windows\\System32\\" + std::wstring(name) + L"\""; }
void Settings::Load() { Rec(L"settings.Load"); }   // g_settings as the test set it
void Settings::Save() const { Rec(L"settings.Save(autostart %d)", (int)autostart); }
bool g_demoMode = false;
void MonitorCapture::RequestBorderlessAccess() { Rec(L"MonitorCapture::RequestBorderlessAccess"); }

// ---------------------------------------------------------------- main_instance.cpp (stubs: main_instance_test tests it)
HANDLE g_watchdogProcess = nullptr;
bool WatchdogReady() { Rec(L"WatchdogReady"); return ml::watchdogReady; }
bool RestartUnelevated(const std::wstring& args) { Rec(L"RestartUnelevated([%ls])", args.c_str()); return ml::restartUnelevated; }
LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) { Rec(L"CrashHandler"); return EXCEPTION_CONTINUE_SEARCH; }
int RestoreTaskbarWhenIdle(DWORD ms) { Rec(L"RestoreTaskbarWhenIdle(%lu)", ms); return ml::restoreWhenIdleRc; }
int RunWatchdog(DWORD pid, const wchar_t* ready) { Rec(L"RunWatchdog(%lu, %ls)", pid, ready ? ready : L"null"); return ml::runWatchdogRc; }
bool StartWatchdog() {
    Rec(L"StartWatchdog");
    if (ml::watchdogStarts) g_watchdogProcess = ml::NewHandle(L"watchdog");
    ml::watchdogReady = ml::watchdogStarts;   // WatchdogReady asks the process StartWatchdog kept
    return ml::watchdogStarts;
}

// ---------------------------------------------------------------- the other modules (link stubs that record)
namespace fluentflyout {
void Tick(double) { Rec(L"ff.Tick"); }
void InvalidateMonitors() { Rec(L"ff.InvalidateMonitors"); }
bool WantsHost() { Rec(L"ff.WantsHost"); return ml::ffWants; }
HWND WidgetWindow() { Rec(L"ff.WidgetWindow"); return ml::ffWants ? (HWND)0x4444 : nullptr; }
bool Busy() { Rec(L"ff.Busy"); return ml::ffBusy; }
void Shutdown() { Rec(L"ff.Shutdown"); }
}
namespace trayhost {
bool Start(bool announce) { Rec(L"th.Start(%d)", (int)announce); if (ml::thStarts) ml::thRunning = true; return ml::thStarts; }
void Announce() { Rec(L"th.Announce"); }
void Stop() { Rec(L"th.Stop"); ml::thRunning = false; }
bool Running() { Rec(L"th.Running"); return ml::thRunning; }
void SetHosting(bool on, HWND only) { Rec(L"th.SetHosting(%d%ls)", (int)on, only ? L", widget" : L""); }
void Place(bool shown, const RECT& r, bool raise, const RECT&, HWND) { Rec(L"th.Place(%d, %ld %ld %ld %ld, %d)", (int)shown, r.left, r.top, r.right, r.bottom, (int)raise); }
}
namespace oled {
void InvalidateCovers() { Rec(L"oled.InvalidateCovers"); }
void Reapply() { Rec(L"oled.Reapply"); }
void Tick(double) { Rec(L"oled.Tick"); }
bool Disable() { Rec(L"oled.Disable"); return ml::oledDisables; }
bool SetDesktopIconsVisible(bool v) { Rec(L"oled.SetDesktopIconsVisible(%d)", (int)v); return true; }
}
namespace settingswindow {
void Show(HMONITOR m) { Rec(L"sw.Show(%ls)", ml::MonName(m)); }
void PlaceBottomRight() { Rec(L"sw.PlaceBottomRight"); }
void Close() { Rec(L"sw.Close"); }
bool PreTranslate(MSG*) { Rec(L"sw.PreTranslate"); return ml::preTranslate; }
}
namespace nativetray {
bool OpenOverflow(POINT p, Edge e, RECT r, OverflowFailure*) { Rec(L"nt.OpenOverflow(%ld,%ld, edge %d, %ld %ld %ld %ld)", p.x, p.y, (int)e, r.left, r.top, r.right, r.bottom); return true; }
void OpenCalendar() { Rec(L"nt.OpenCalendar"); }
void OpenVolume(POINT p) { Rec(L"nt.OpenVolume(%ld,%ld)", p.x, p.y); }
void OpenQuickSettings() { Rec(L"nt.OpenQuickSettings"); }
}
namespace features { void Tick(double) { Rec(L"features.Tick"); } }
namespace adminhelper { void Tick(double) { Rec(L"admin.Tick"); } }
// the start button told when the display came on (start_menu.cpp: for its dock.log lines only) - counted, kept out
// of the trace, which holds main.cpp's own calls
namespace startmenu {
int displayOnCalls = 0;
double displayOnAt = -1;
void DisplayCameOn(double now) { ++displayOnCalls; displayOnAt = now; }
}

// the objects main.cpp uses (renamed below)
struct FakeApps {
    std::vector<std::shared_ptr<AppItem>> items;
    std::vector<AttentionEvent> attentionEvents;
    void Init() { Rec(L"apps.Init"); }
    void Tick(double) { Rec(L"apps.Tick"); ml::nowUs += ml::appsCostUs; }
    void OnShellHook(WPARAM w, LPARAM l) { Rec(L"apps.OnShellHook(%llu, %lld)", (unsigned long long)w, (long long)l); }
    void ReloadIcons() { Rec(L"apps.ReloadIcons"); }
};
FakeApps fakeApps;
struct FakeSys {
    void PollBattery() { Rec(L"sys.PollBattery"); }
    void Tick(double) { Rec(L"sys.Tick"); ml::nowUs += ml::sysCostUs; }
    void Start() { Rec(L"sys.Start"); }
};
FakeSys fakeSys;
struct FakeSensors {
    bool alertPending = false;
    void Start() { Rec(L"sensors.Start"); }
    void Tick(double) { Rec(L"sensors.Tick"); ml::nowUs += ml::sensorsCostUs; }
    void SetPanelOpen(bool open, double) { Rec(L"sensors.SetPanelOpen(%d)", (int)open); }
};
FakeSensors fakeSensors;
struct FakeGfx {
    bool Init() { Rec(L"gfx.Init"); return ml::gfxInits; }
    bool DeviceLost() { ++ml::n.deviceLost; Rec(L"gfx.DeviceLost"); return ml::gfxLost; }
    bool Reset() { ++ml::n.reset; Rec(L"gfx.Reset"); if (ml::gfxResets || !ml::gfxLostAfterFailedReset) ml::gfxLost = false; return ml::gfxResets; }
};
FakeGfx fakeGfx;
struct FakeTaskbar {
    bool hidden = false;
    bool Hide() { ++ml::n.hide; Rec(L"taskbar.Hide"); hidden = true; return true; }
    bool Restore() { const int k = ml::n.restore++; Rec(L"taskbar.Restore"); hidden = false; return ml::restoreWorks(k); }   // clears hidden even when it fails (native_taskbar.cpp)
    void RecheckSoon() { Rec(L"taskbar.RecheckSoon"); }
    void Enforce(bool onUi = true, bool check = true) { Rec(L"taskbar.Enforce(%d, %d)", (int)onUi, (int)check); }
    void StartEnforcer() { Rec(L"taskbar.StartEnforcer"); }
};
FakeTaskbar fakeTaskbar;

// a dock without window or GPU: what main.cpp asks of it, recorded
struct FakeDock {
    FakeDock(HMONITOR mon, int index, bool primary) : monitor(mon), index(index), primary(primary) {
        Rec(L"Dock(%ls, %d, %d)", ml::MonName(mon), index, (int)primary);
    }
    ~FakeDock() { Rec(L"d%d.~Dock", index); }
    bool Create() { Rec(L"d%d.Create", index); return ml::dock[index & 3].createOk; }
    void Update(double now, float dt, POINT c) {
        ++ml::n.update; Rec(L"d%d.Update", index);
        ml::updates.push_back({ index, now, dt, c, screenUnseen, debugForceShow, fakeApps.attentionEvents.size(), fakeSensors.alertPending });
        if (ml::dock[index & 3].holdsWidget) {   // as Dock's widget placement (dock_window.cpp): placed, and where
            widgetPlaced = true;
            widgetLastRect = { 1000 + 100 * index, 1300, 1400 + 100 * index, 1440 };
        }
        ml::nowUs += ml::updateCostUs;
    }
    void Render() { ++ml::n.render; Rec(L"d%d.Render", index); ml::nowUs += ml::renderCostUs; }
    void RecordTick(double) { Rec(L"d%d.RecordTick", index); }
    bool Busy() const { Rec(L"d%d.Busy", index); return ml::dock[index & 3].busy; }
    bool LiveGlass() const { Rec(L"d%d.LiveGlass", index); return ml::dock[index & 3].liveGlass; }
    bool FullscreenHere() const { Rec(L"d%d.FullscreenHere", index); return ml::dock[index & 3].fullscreen; }
    bool PanelWanted() const { Rec(L"d%d.PanelWanted", index); return ml::dock[index & 3].panel; }
    void RequestSnapshot() { Rec(L"d%d.RequestSnapshot", index); }
    bool Relayout() { Rec(L"d%d.Relayout", index); return ml::dock[index & 3].relayoutOk; }
    void HotkeyReveal(double) { Rec(L"d%d.HotkeyReveal", index); }
    void StartRecording(RECT crop, float scale, bool append = false) {
        Rec(L"d%d.StartRecording(%ld %ld %ld %ld, %.3f, %d)", index, crop.left, crop.top, crop.right, crop.bottom, scale, (int)append);
    }
    void StopRecording() { Rec(L"d%d.StopRecording", index); }
    static void ResetNotifyBitmaps() { Rec(L"Dock::ResetNotifyBitmaps"); }
    HMONITOR monitor;
    int index;
    bool primary;
    static inline bool debugForceShow = false, debugForcePanel = false, screenUnseen = false, widgetPlaced = false, carryRecording = false;
    static inline RECT widgetLastRect{}, carryCrop{};
    static inline float carryScale = 0.5f;
};

template <class F> bool FakeTryDetach(F&& work) {   // runs the work at once, on this thread (deterministic)
    Rec(L"TryDetach");
    if (!ml::threadsLeft) return false;
    work();
    return true;
}
static HWND FakeExplorerTaskbar() { Rec(L"ExplorerTaskbar"); return ml::explorerTaskbar ? (HWND)0x3333 : nullptr; }
static bool FakeProcessElevated() { Rec(L"ProcessElevated"); return ml::elevated; }
namespace winrt {
static void FakeInitApartment(apartment_type t) { Rec(L"init_apartment(%ls)", t == apartment_type::single_threaded ? L"STA" : L"MTA"); }
}

// ---------------------------------------------------------------- Windows (named in the #define list below)
static DWORD WINAPI MlGetLastError() { return ml::lastError; }
static BOOL WINAPI MlEnumDisplayMonitors(HDC dc, LPCRECT clip, MONITORENUMPROC proc, LPARAM lp) {
    Rec(L"EnumDisplayMonitors(%ls, %ls)", dc ? L"dc" : L"null", clip ? L"clip" : L"null");
    for (auto& m : ml::mons) { RECT r = m.rc; if (!proc(m.h, nullptr, &r, lp)) break; }
    return TRUE;
}
static HMONITOR WINAPI MlMonitorFromPoint(POINT p, DWORD flags) {
    ++ml::n.monFromPoint;
    Rec(L"MonitorFromPoint(%ld,%ld,%lu)", p.x, p.y, flags);
    for (auto& m : ml::mons) if (p.x >= m.rc.left && p.x < m.rc.right && p.y >= m.rc.top && p.y < m.rc.bottom) return m.h;
    if (flags == MONITOR_DEFAULTTOPRIMARY) { for (auto& m : ml::mons) if (m.primary) return m.h; return nullptr; }
    if (flags == MONITOR_DEFAULTTONEAREST) {
        const ml::Mon* best = nullptr; double bestD = 0;
        for (auto& m : ml::mons) {
            const double dx = p.x < m.rc.left ? m.rc.left - p.x : p.x >= m.rc.right ? p.x - m.rc.right + 1 : 0;
            const double dy = p.y < m.rc.top ? m.rc.top - p.y : p.y >= m.rc.bottom ? p.y - m.rc.bottom + 1 : 0;
            if (!best || dx * dx + dy * dy < bestD) { best = &m; bestD = dx * dx + dy * dy; }
        }
        return best ? best->h : nullptr;
    }
    return nullptr;
}
static BOOL WINAPI MlGetMonitorInfoW(HMONITOR h, LPMONITORINFO mi) {
    ++ml::n.monInfo;
    Rec(L"GetMonitorInfoW(%ls%ls)", ml::MonName(h), mi && mi->cbSize == sizeof(MONITORINFO) ? L"" : L", bad size");
    if (ml::monInfoFails) return FALSE;
    for (auto& m : ml::mons) if (m.h == h) { mi->rcMonitor = m.rc; mi->rcWork = m.work; mi->dwFlags = m.primary ? MONITORINFOF_PRIMARY : 0; return TRUE; }
    return FALSE;
}
static BOOL WINAPI MlGetCursorPos(LPPOINT p) {
    ++ml::n.cursor;
    Rec(L"GetCursorPos");
    if (!ml::cursorReadable) { ml::lastError = ERROR_ACCESS_DENIED; return FALSE; }
    *p = ml::cursor;
    return TRUE;
}
static wchar_t mlExe[MAX_PATH] = L"C:\\Tools & Docks\\LiquidDock\\LiquidDock.exe";
static DWORD WINAPI MlGetModuleFileNameW(HMODULE m, LPWSTR out, DWORD size) {
    Rec(L"GetModuleFileNameW(%ls, %lu)", m ? L"module" : L"null", size);
    wcscpy_s(out, size, mlExe);
    return (DWORD)wcslen(mlExe);
}
static const HKEY kRunKey = (HKEY)(ULONG_PTR)0x9001, kHklmKey = (HKEY)(ULONG_PTR)0x9002;
static const wchar_t* KName(HKEY k) {
    return k == HKEY_CURRENT_USER ? L"HKCU" : k == HKEY_LOCAL_MACHINE ? L"HKLM" : k == kRunKey ? L"run" : k == kHklmKey ? L"version" : L"?";
}
static LSTATUS WINAPI MlRegOpenKeyExW(HKEY root, LPCWSTR sub, DWORD opts, REGSAM sam, PHKEY out) {
    Rec(L"RegOpenKeyExW(%ls, %ls, %lu, 0x%lx)", KName(root), sub ? sub : L"null", opts, sam);
    if (root == HKEY_CURRENT_USER) { if (!ml::runKey) return ERROR_ACCESS_DENIED; *out = kRunKey; return ERROR_SUCCESS; }
    if (root == HKEY_LOCAL_MACHINE) { if (!ml::hklm) return ERROR_FILE_NOT_FOUND; *out = kHklmKey; return ERROR_SUCCESS; }
    return ERROR_FILE_NOT_FOUND;
}
static LSTATUS WINAPI MlRegSetValueExW(HKEY k, LPCWSTR name, DWORD res, DWORD type, const BYTE* data, DWORD size) {
    Rec(L"RegSetValueExW(%ls, %ls, %lu, %lu, [%ls], %lu)", KName(k), name, res, type, (const wchar_t*)data, size);
    ml::runValue = (const wchar_t*)data; ml::runValueType = type; ml::runValueSize = size;
    return ERROR_SUCCESS;
}
static LSTATUS WINAPI MlRegDeleteValueW(HKEY k, LPCWSTR name) { Rec(L"RegDeleteValueW(%ls, %ls)", KName(k), name); return ERROR_SUCCESS; }
static LSTATUS WINAPI MlRegCloseKey(HKEY k) { Rec(L"RegCloseKey(%ls)", KName(k)); return ERROR_SUCCESS; }
static LSTATUS WINAPI MlRegGetValueW(HKEY k, LPCWSTR sub, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD size) {
    Rec(L"RegGetValueW(%ls, %ls, %ls, 0x%lx, %ls, %lu)", KName(k), sub ? sub : L"null", value, flags, type ? L"type" : L"null", size ? *size : 0);
    if (!wcscmp(value, L"DisplayVersion")) { wcscpy_s((wchar_t*)data, *size / 2, L"26H2"); return ERROR_SUCCESS; }
    if (!wcscmp(value, L"CurrentBuild")) { wcscpy_s((wchar_t*)data, *size / 2, ml::build.c_str()); return ERROR_SUCCESS; }
    if (!wcscmp(value, L"UBR")) { *(DWORD*)data = 1000; return ERROR_SUCCESS; }
    return ERROR_FILE_NOT_FOUND;
}
static BOOL WINAPI MlCreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL inherit, DWORD flags,
                                    LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    Rec(L"CreateProcessW(%ls, [%ls], %d, 0x%lx, %ls, %ls, si flags 0x%lx show %u)", app ? app : L"null", cmd, inherit, flags,
        env ? L"env" : L"null", dir ? dir : L"null", si->dwFlags, (unsigned)si->wShowWindow);
    ml::processes.push_back(cmd);
    if (wcsstr(cmd, L"/XML")) {   // the task's XML file as schtasks would read it
        std::ifstream f(ml::xmlFile, std::ios::binary);
        ml::xmlSeen = f.good();
        ml::xmlBytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (!ml::schtasksStarts) return FALSE;
    pi->hProcess = ml::NewHandle(L"schtasks"); pi->hThread = ml::NewHandle(L"schtasks thread");
    return TRUE;
}
static DWORD WINAPI MlWaitForSingleObject(HANDLE h, DWORD ms) {
    const std::wstring n = ml::HName(h);
    Rec(L"WaitForSingleObject(%ls, %lu)", n.c_str(), ms);
    if (n == L"mutex") return ml::mutexWait;
    if (n == L"other dock") return ml::otherWait;
    if (n == L"schtasks") return WAIT_OBJECT_0;
    return WAIT_FAILED;
}
static BOOL WINAPI MlGetExitCodeProcess(HANDLE h, LPDWORD code) { Rec(L"GetExitCodeProcess(%ls)", ml::HName(h).c_str()); *code = ml::schtasksCode; return TRUE; }
static BOOL WINAPI MlCloseHandle(HANDLE h) {
    const std::wstring n = ml::HName(h);
    Rec(L"CloseHandle(%ls)", n.c_str());
    return n == L"real" ? ::CloseHandle(h) : TRUE;
}
static DWORD WINAPI MlGetEnvironmentVariableW(LPCWSTR name, LPWSTR out, DWORD size) {
    Rec(L"GetEnvironmentVariableW(%ls, %lu)", name, size);
    const wchar_t* v = !wcscmp(name, L"USERNAME") ? L"User<&>" : !wcscmp(name, L"USERDOMAIN") ? L"DESK&TOP" : nullptr;
    if (!v) return 0;
    wcscpy_s(out, size, v);
    return (DWORD)wcslen(v);
}
static DWORD WINAPI MlGetTempPathW(DWORD size, LPWSTR out) {
    Rec(L"GetTempPathW(%lu)", size);
    wcscpy_s(out, size, ml::tempDir.c_str());
    ml::xmlFile = ml::tempDir + L"LiquidDockLogon.xml";
    return (DWORD)ml::tempDir.size();
}
static BOOL WINAPI MlDeleteFileW(LPCWSTR path) {
    Rec(L"DeleteFileW(%ls)", path == ml::xmlFile ? L"<temp>\\LiquidDockLogon.xml" : path);
    return ::DeleteFileW(path);   // only ever the test's file (GetTempPathW is the test's folder)
}
static BOOL WINAPI MlUnregisterHotKey(HWND h, int id) { Rec(L"UnregisterHotKey(%ls, %d)", ml::WName(h), id); return TRUE; }
static BOOL WINAPI MlRegisterHotKey(HWND h, int id, UINT mods, UINT vk) {
    Rec(L"RegisterHotKey(%ls, %d, 0x%x, 0x%x)", ml::WName(h), id, mods, vk);
    if (ml::hotkeyTaken[vk]) { ml::lastError = ERROR_HOTKEY_ALREADY_REGISTERED; return FALSE; }
    return TRUE;
}
static LRESULT WINAPI MlDefWindowProcW(HWND h, UINT m, WPARAM, LPARAM) { Rec(L"DefWindowProcW(%ls, 0x%x)", ml::WName(h), m); return 0xDEF; }
static const HMODULE kUxtheme = (HMODULE)(ULONG_PTR)0x7700;
static int WINAPI MlSetPreferredAppMode(int mode) { Rec(L"SetPreferredAppMode(%d)", mode); return 0; }
static void WINAPI MlFlushMenuThemes() { Rec(L"FlushMenuThemes"); }
static HMODULE WINAPI MlLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    Rec(L"LoadLibraryExW(%ls, %ls, 0x%lx)", name, file ? L"file" : L"null", flags);
    return ml::uxtheme ? kUxtheme : nullptr;
}
static FARPROC WINAPI MlGetProcAddress(HMODULE m, LPCSTR name) {
    const ULONG_PTR ord = (ULONG_PTR)name;
    Rec(L"GetProcAddress(%ls, #%llu)", m == kUxtheme ? L"uxtheme" : L"?", (unsigned long long)ord);
    if (ord == 135) return (FARPROC)&MlSetPreferredAppMode;
    if (ord == 136) return (FARPROC)&MlFlushMenuThemes;
    return nullptr;
}
static BOOL WINAPI MlSetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT c) {
    Rec(L"SetProcessDpiAwarenessContext(%ls)", c == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ? L"per-monitor v2" : L"other");
    return TRUE;
}
static HWND WINAPI MlFindWindowW(LPCWSTR cls, LPCWSTR title) { Rec(L"FindWindowW(%ls, %ls)", cls ? cls : L"null", title ? title : L"null"); return ml::otherDock; }
static BOOL WINAPI MlPostMessageW(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m >= WM_APP) Rec(L"PostMessageW(%ls, WM_APP+%d, %llu, %lld)", ml::WName(h), (int)(m - WM_APP), (unsigned long long)w, (long long)l);
    else Rec(L"PostMessageW(%ls, 0x%x, %llu, %lld)", ml::WName(h), m, (unsigned long long)w, (long long)l);
    if (h == ml::kMain) ml::Post(m, w, l);
    return TRUE;
}
static BOOL WINAPI MlAllowSetForegroundWindow(DWORD pid) { Rec(L"AllowSetForegroundWindow(%ls)", pid == ASFW_ANY ? L"ASFW_ANY" : L"pid"); return TRUE; }
static DWORD WINAPI MlGetWindowThreadProcessId(HWND h, LPDWORD pid) { Rec(L"GetWindowThreadProcessId(%ls)", ml::WName(h)); if (pid) *pid = 3131; return 77; }
static HANDLE WINAPI MlOpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    Rec(L"OpenProcess(0x%lx, %d, %lu)", access, inherit, pid);
    return ml::openProcessWorks ? ml::NewHandle(L"other dock") : nullptr;
}
static BOOL WINAPI MlTerminateProcess(HANDLE h, UINT code) { Rec(L"TerminateProcess(%ls, %u)", ml::HName(h).c_str(), code); return TRUE; }
static HANDLE WINAPI MlCreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name) {
    Rec(L"CreateMutexW(%ls, %d, %ls)", sa ? L"sa" : L"null", owner, name);
    return ml::mutexMade ? ml::NewHandle(L"mutex") : nullptr;
}
static BOOL WINAPI MlReleaseMutex(HANDLE h) { Rec(L"ReleaseMutex(%ls)", ml::HName(h).c_str()); return TRUE; }
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI MlSetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    Rec(L"SetUnhandledExceptionFilter(%ls)", f == CrashHandler ? L"CrashHandler" : L"other");
    return nullptr;
}
static int WINAPI MlMessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type) { Rec(L"MessageBoxW(%ls, %ls, %ls, 0x%x)", ml::WName(h), text, caption, type); return IDOK; }
static ATOM WINAPI MlRegisterClassExW(const WNDCLASSEXW* wc) {
    Rec(L"RegisterClassExW(%ls, cb %u, style 0x%x, hInstance %ls, icon %ls, cursor %ls)", wc->lpszClassName, wc->cbSize, wc->style,
        wc->hInstance == (HINSTANCE)0x400000 ? L"exe" : L"?", wc->hIcon ? L"set" : L"null", wc->hCursor ? L"set" : L"null");
    ml::registeredProc = wc->lpfnWndProc;
    return 0xC001;
}
static HWND WINAPI MlCreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int cx, int cy, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    Rec(L"CreateWindowExW(0x%lx, %ls, %ls, 0x%lx, %d %d %d %d, %ls, %ls, %ls, %ls)", ex, cls, title, style, x, y, cx, cy, ml::WName(parent),
        menu ? L"menu" : L"null", inst == (HINSTANCE)0x400000 ? L"exe" : L"?", param ? L"param" : L"null");
    return ml::windowMade ? ml::kMain : nullptr;
}
static UINT WINAPI MlRegisterWindowMessageW(LPCWSTR name) {
    Rec(L"RegisterWindowMessageW(%ls)", name);
    return !wcscmp(name, L"SHELLHOOK") ? 0xC0A1 : !wcscmp(name, L"TaskbarCreated") ? 0xC0A2 : 0;
}
static BOOL WINAPI MlChangeWindowMessageFilterEx(HWND h, UINT m, DWORD action, PCHANGEFILTERSTRUCT cfs) {
    Rec(L"ChangeWindowMessageFilterEx(%ls, 0x%x, %lu, %ls)", ml::WName(h), m, action, cfs ? L"cfs" : L"null");
    return TRUE;
}
static BOOL WINAPI MlRegisterShellHookWindow(HWND h) { Rec(L"RegisterShellHookWindow(%ls)", ml::WName(h)); return TRUE; }
static BOOL WINAPI MlDeregisterShellHookWindow(HWND h) { Rec(L"DeregisterShellHookWindow(%ls)", ml::WName(h)); return TRUE; }
static const GUID kDisplayStatus = { 0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 } };   // GUID_SESSION_DISPLAY_STATUS
static const HPOWERNOTIFY kPowerNotify = (HPOWERNOTIFY)(ULONG_PTR)0x6600;
static HPOWERNOTIFY WINAPI MlRegisterPowerSettingNotification(HANDLE h, LPCGUID g, DWORD flags) {
    Rec(L"RegisterPowerSettingNotification(%ls, %ls, %lu)", ml::WName((HWND)h), IsEqualGUID(*g, kDisplayStatus) ? L"GUID_SESSION_DISPLAY_STATUS" : L"other", flags);
    return ml::powerNotify ? kPowerNotify : nullptr;
}
static BOOL WINAPI MlUnregisterPowerSettingNotification(HPOWERNOTIFY h) { Rec(L"UnregisterPowerSettingNotification(%ls)", h == kPowerNotify ? L"display" : L"?"); return TRUE; }
static const wchar_t* MsgName(UINT m) {
    switch (m) {
    case WM_QUIT: return L"WM_QUIT"; case WM_CLOSE: return L"WM_CLOSE"; case WM_HOTKEY: return L"WM_HOTKEY";
    case WM_DISPLAYCHANGE: return L"WM_DISPLAYCHANGE"; case WM_POWERBROADCAST: return L"WM_POWERBROADCAST";
    default: return nullptr;
    }
}
static BOOL WINAPI MlPeekMessageW(LPMSG msg, HWND h, UINT lo, UINT hi, UINT remove) {
    ++ml::n.peek;
    if (h || lo || hi || remove != PM_REMOVE) Rec(L"Peek(unexpected arguments)");
    if (ml::queue.empty()) { Rec(L"Peek"); return FALSE; }
    *msg = ml::queue.front(); ml::queue.pop_front();
    if (const wchar_t* name = MsgName(msg->message)) Rec(L"Peek:%ls", name);
    else Rec(L"Peek:0x%x", msg->message);
    return TRUE;
}
static BOOL WINAPI MlTranslateMessage(const MSG*) { Rec(L"TranslateMessage"); return FALSE; }
static LRESULT WINAPI MlDispatchMessageW(const MSG* msg) {
    Rec(L"DispatchMessageW");
    if (msg->hwnd == ml::kMain && ml::registeredProc) return ml::registeredProc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
    return 0;
}
static void EndOfPass() {
    const int p = ml::passes++;
    if (ml::afterWait) ml::afterWait(p);
    if (ml::passes >= ml::maxPasses && (ml::queue.empty() || ml::queue.back().message != WM_QUIT)) {
        if (ml::maxPasses == 5000000) std::printf("FAIL: the loop ran 5,000,000 passes without the test ending it\n");
        ml::Quit();
    }
}
// GUID_SESSION_DISPLAY_STATUS's notification (WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE): 0 off, 1 on, 2 dimmed
static LPARAM DisplayNotice(DWORD v) {
    static struct { POWERBROADCAST_SETTING s; BYTE more[8]; } ps[3] = {};
    auto& p = ps[v % 3];
    p.s.PowerSetting = kDisplayStatus; p.s.DataLength = sizeof(DWORD); memcpy(p.s.Data, &v, sizeof v);
    return (LPARAM)&p.s;
}
static DWORD WINAPI MlMsgWaitForMultipleObjectsEx(DWORD count, const HANDLE* hs, DWORD ms, DWORD mask, DWORD flags) {
    ++ml::n.waits; ++ml::n.waitMs[ms];
    if (count || hs || mask != QS_ALLINPUT || flags != MWMO_INPUTAVAILABLE) Rec(L"MsgWait(%lu, unexpected arguments)", ms);
    else Rec(L"MsgWait(%lu)", ms);
    ml::nowUs += (long long)ms * 1000;
    EndOfPass();
    return WAIT_TIMEOUT;
}
static HRESULT WINAPI MlDwmFlush() { ++ml::n.dwmFlush; Rec(L"DwmFlush"); ml::nowUs += 16667; EndOfPass(); return S_OK; }
// --profile's process numbers
static const HANDLE kSelf = (HANDLE)(LONG_PTR)-1;
static HANDLE WINAPI MlGetCurrentProcess() { return kSelf; }
static DWORD WINAPI MlGetCurrentProcessId() { return 2020; }
static BOOL WINAPI MlGetProcessHandleCount(HANDLE h, PDWORD n) { Rec(L"GetProcessHandleCount(%ls)", h == kSelf ? L"self" : L"?"); *n = 345; return TRUE; }
static BOOL WINAPI MlGetProcessMemoryInfo(HANDLE h, PPROCESS_MEMORY_COUNTERS pm, DWORD cb) {
    Rec(L"GetProcessMemoryInfo(%ls, %lu)", h == kSelf ? L"self" : L"?", cb);
    if (cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) ((PROCESS_MEMORY_COUNTERS_EX*)pm)->PrivateUsage = 50 * 1048576 + 104858;
    return TRUE;
}
static int snapAt = 0;
static HANDLE WINAPI MlCreateToolhelp32Snapshot(DWORD flags, DWORD pid) { Rec(L"CreateToolhelp32Snapshot(0x%lx, %lu)", flags, pid); snapAt = 0; return ml::NewHandle(L"snapshot"); }
static BOOL MlThread(LPTHREADENTRY32 te) {   // three threads: two of the dock, one of another process
    static const DWORD owners[] = { 2020, 999, 2020 };
    if (snapAt >= 3 || te->dwSize != sizeof(THREADENTRY32)) return FALSE;
    te->th32OwnerProcessID = owners[snapAt++];
    return TRUE;
}
static BOOL WINAPI MlThread32First(HANDLE, LPTHREADENTRY32 te) { snapAt = 0; return MlThread(te); }
static BOOL WINAPI MlThread32Next(HANDLE, LPTHREADENTRY32 te) { return MlThread(te); }
static BOOL WINAPI MlGetProcessTimes(HANDLE, LPFILETIME a, LPFILETIME b, LPFILETIME k, LPFILETIME u) {
    Rec(L"GetProcessTimes");
    *a = *b = FILETIME{};
    ULONGLONG kt = 12000000, ut = 23000000;   // 1.2 s kernel, 2.3 s user
    k->dwLowDateTime = (DWORD)kt; k->dwHighDateTime = (DWORD)(kt >> 32);
    u->dwLowDateTime = (DWORD)ut; u->dwHighDateTime = (DWORD)(ut >> 32);
    return TRUE;
}

// ---------------------------------------------------------------- the source, under the fakes
#define Dock FakeDock
#define g_apps fakeApps
#define g_sys fakeSys
#define g_sensors fakeSensors
#define g_gfx fakeGfx
#define g_taskbar fakeTaskbar
#define TryDetach FakeTryDetach
#define ExplorerTaskbar FakeExplorerTaskbar
#define ProcessElevated FakeProcessElevated
#define init_apartment FakeInitApartment
#define wWinMain MainEntry
#define GetLastError MlGetLastError
#define EnumDisplayMonitors MlEnumDisplayMonitors
#define MonitorFromPoint MlMonitorFromPoint
#define GetMonitorInfoW MlGetMonitorInfoW
#define GetCursorPos MlGetCursorPos
#define GetModuleFileNameW MlGetModuleFileNameW
#define RegOpenKeyExW MlRegOpenKeyExW
#define RegSetValueExW MlRegSetValueExW
#define RegDeleteValueW MlRegDeleteValueW
#define RegCloseKey MlRegCloseKey
#define RegGetValueW MlRegGetValueW
#define CreateProcessW MlCreateProcessW
#define WaitForSingleObject MlWaitForSingleObject
#define GetExitCodeProcess MlGetExitCodeProcess
#define CloseHandle MlCloseHandle
#define GetEnvironmentVariableW MlGetEnvironmentVariableW
#define GetTempPathW MlGetTempPathW
#define DeleteFileW MlDeleteFileW
#define UnregisterHotKey MlUnregisterHotKey
#define RegisterHotKey MlRegisterHotKey
#define DefWindowProcW MlDefWindowProcW
#define LoadLibraryExW MlLoadLibraryExW
#define GetProcAddress MlGetProcAddress
#define SetProcessDpiAwarenessContext MlSetProcessDpiAwarenessContext
#define FindWindowW MlFindWindowW
#define PostMessageW MlPostMessageW
#define AllowSetForegroundWindow MlAllowSetForegroundWindow
#define GetWindowThreadProcessId MlGetWindowThreadProcessId
#define OpenProcess MlOpenProcess
#define TerminateProcess MlTerminateProcess
#define CreateMutexW MlCreateMutexW
#define ReleaseMutex MlReleaseMutex
#define SetUnhandledExceptionFilter MlSetUnhandledExceptionFilter
#define MessageBoxW MlMessageBoxW
#define RegisterClassExW MlRegisterClassExW
#define CreateWindowExW MlCreateWindowExW
#define RegisterWindowMessageW MlRegisterWindowMessageW
#define ChangeWindowMessageFilterEx MlChangeWindowMessageFilterEx
#define RegisterShellHookWindow MlRegisterShellHookWindow
#define DeregisterShellHookWindow MlDeregisterShellHookWindow
#define RegisterPowerSettingNotification MlRegisterPowerSettingNotification
#define UnregisterPowerSettingNotification MlUnregisterPowerSettingNotification
#define PeekMessageW MlPeekMessageW
#define TranslateMessage MlTranslateMessage
#define DispatchMessageW MlDispatchMessageW
#define MsgWaitForMultipleObjectsEx MlMsgWaitForMultipleObjectsEx
#define DwmFlush MlDwmFlush
#define GetCurrentProcess MlGetCurrentProcess
#define GetCurrentProcessId MlGetCurrentProcessId
#define GetProcessHandleCount MlGetProcessHandleCount
#undef GetProcessMemoryInfo
#define GetProcessMemoryInfo MlGetProcessMemoryInfo
#define CreateToolhelp32Snapshot MlCreateToolhelp32Snapshot
#define Thread32First MlThread32First
#define Thread32Next MlThread32Next
#define GetProcessTimes MlGetProcessTimes
#include "../src/main.cpp"
#include "../src/main_autostart.cpp"   // SetAutostart (main_autostart.cpp): under the same fakes
#undef Dock
#undef GetLastError
#undef CloseHandle
#undef MonitorFromPoint
#undef GetMonitorInfoW
#undef GetCursorPos
#undef PeekMessageW
#undef DeleteFileW
#undef GetModuleFileNameW

// ---------------------------------------------------------------- driving it
static const HINSTANCE kExeInstance = (HINSTANCE)0x400000;

// main.cpp's file statics as at a fresh start of the exe. g_look holds the display state
// and the loop's look at the pointer (the loop's PointerLook::Start resets the look), and
// g_fluentWantedHost what FluentFlyout wanted at the pass before: all reset here, so no run starts from the one before
// (every run still ends with FluentFlyout's WantsHost false)
static void ResetMain() {
    ml::tracing = false;
    g_docks.clear();
    ml::tracing = true;
    g_running = true; g_enforceCount = 0; g_rebuildAt = 0; g_deviceResetRequested = false; g_nextDeviceRetry = 0; g_docksReady = false;
    g_restoreRetry = taskbarvisibility::RestoreRetry{}; g_look = idlewait::PointerLook{}; g_fluentWantedHost = false;
    g_msgShellHook = g_msgTaskbarCreated = 0; g_mainWnd = nullptr;
    g_settings = Settings{}; g_demoMode = false; g_watchdogProcess = nullptr;
    FakeDock::debugForceShow = FakeDock::debugForcePanel = FakeDock::screenUnseen = FakeDock::widgetPlaced = FakeDock::carryRecording = false;
    FakeDock::widgetLastRect = {}; FakeDock::carryCrop = {}; FakeDock::carryScale = 0.5f;
    fakeApps = FakeApps{}; fakeSensors = FakeSensors{}; fakeTaskbar.hidden = false;
    ml::handles.clear(); ml::handleCount = 0;
}
static void Fresh() { ml::ResetWorld(); ResetMain(); }

// The exe started with this command line; the loop stops after `passes` passes (or when afterWait quits it).
static int Run(const wchar_t* cmd, int passes = 1, std::function<void(int)> afterWait = nullptr) {
    ml::maxPasses = passes > 0 ? passes : 5000000;   // 0: until afterWait ends it
    ml::afterWait = std::move(afterWait);
    std::wstring c = cmd ? cmd : L"";
    return MainEntry(kExeInstance, nullptr, cmd ? c.data() : nullptr, SW_SHOWDEFAULT);
}

static std::vector<std::wstring> V(std::initializer_list<const wchar_t*> l) { return std::vector<std::wstring>(l.begin(), l.end()); }
static std::vector<std::wstring> Cat(std::vector<std::wstring> a, const std::vector<std::wstring>& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

// what every pass calls before the pointer (docks ready, the watchdog there, no message, nothing to rebuild)
static std::vector<std::wstring> PassHead() {
    return V({ L"Peek", L"gfx.DeviceLost", L"WatchdogReady", L"apps.Tick", L"features.Tick", L"oled.Tick", L"ff.Tick", L"admin.Tick",
               L"ff.WantsHost", L"ff.WidgetWindow", L"th.SetHosting(0)", L"sys.Tick", L"sensors.Tick", L"GetCursorPos", L"ff.Busy" });
}
// a dock's part of the pass
static std::vector<std::wstring> DockPart(int i) {
    wchar_t b[5][40];
    swprintf_s(b[0], L"d%d.Update", i); swprintf_s(b[1], L"d%d.Render", i); swprintf_s(b[2], L"d%d.RecordTick", i);
    swprintf_s(b[3], L"d%d.Busy", i); swprintf_s(b[4], L"d%d.LiveGlass", i);
    return { b[0], b[1], b[2], b[3], b[4] };
}

// the whole start of a plain run on the one desktop monitor (default settings), up to the first pass
static std::vector<std::wstring> PlainStart(const wchar_t* args) {
    std::wstring restart = std::wstring(L"RestartUnelevated([") + args + L"])";
    std::wstring windows = L"Log: Windows 11 26H2 (build 26300.1000), Liquid Dock " LD_VERSION_WSTR;
    return { L"LogAppend", L"SetProcessDpiAwarenessContext(per-monitor v2)", L"init_apartment(STA)", restart,
             L"CreateMutexW(null, 0, LiquidDock.SingleInstance)", L"WaitForSingleObject(mutex, 3000)", L"LogStartSession",
             L"SetUnhandledExceptionFilter(CrashHandler)", L"LoadLibraryExW(uxtheme.dll, null, 0x800)", L"GetProcAddress(uxtheme, #135)",
             L"GetProcAddress(uxtheme, #136)", L"SetPreferredAppMode(1)", L"FlushMenuThemes", L"settings.Load",
             L"RegOpenKeyExW(HKLM, SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion, 0, 0x20019)",
             L"RegGetValueW(version, null, DisplayVersion, 0x2, null, 64)", L"RegGetValueW(version, null, CurrentBuild, 0x2, null, 64)",
             L"RegGetValueW(version, null, UBR, 0x10, null, 4)", L"RegCloseKey(version)", windows, L"gfx.Init",
             L"MonitorCapture::RequestBorderlessAccess",
             L"RegisterClassExW(LiquidDockMain, cb 80, style 0x0, hInstance exe, icon null, cursor null)",
             L"CreateWindowExW(0x80, LiquidDockMain, Liquid Dock, 0x80000000, 0 0 0 0, null, null, exe, null)",
             L"RegisterWindowMessageW(SHELLHOOK)", L"RegisterWindowMessageW(TaskbarCreated)", L"ChangeWindowMessageFilterEx(main, 0xc0a2, 1, null)",
             L"RegisterShellHookWindow(main)", L"RegisterPowerSettingNotification(main, GUID_SESSION_DISPLAY_STATUS, 0)",
             L"RegisterHotKey(main, 1, 0x4007, 0x7b)", L"UnregisterHotKey(main, 2)", L"apps.Init", L"sys.Tick", L"sys.Start", L"sensors.Start",
             L"taskbar.StartEnforcer", L"StartWatchdog", L"EnumDisplayMonitors(null, null)", L"MonitorFromPoint(0,0,1)", L"Dock(desk, 0, 1)",
             L"d0.Create", L"Log: docks: 1", L"WatchdogReady", L"taskbar.Hide", L"oled.InvalidateCovers", L"ff.InvalidateMonitors",
             L"ff.WantsHost", L"th.Stop", L"oled.Reapply" };
}

// ---------------------------------------------------------------- the start and the exit
static void StartTests() {
    Fresh();
    int rc = Run(L"");
    Check(rc == 0, "start: a plain start (no arguments) runs and exits with 0");
    ml::Same(StartTrace(), PlainStart(L""), "start: LogAppend first; DPI awareness and the STA before anything else; the mutex (3 s); the session log; the crash filter; themed menus; settings; the Windows line; GPU; the main window and its registrations; the modules; the watchdog; the docks; OLED");
    Check(ml::registeredProc == MainProc, "start: the main window's class has MainProc as its window procedure");
    ml::Same(ml::PassTrace(0), Cat(Cat(PassHead(), DockPart(0)), V({ L"th.Place(0, 0 0 0 0, 0)", L"d0.PanelWanted", L"sensors.SetPanelOpen(0)",
                                                                   L"MonitorFromPoint(1280,700,2)", L"GetMonitorInfoW(desk)", L"d0.FullscreenHere", L"MsgWait(45)" })),
             "the process's first pass: as any (the loop's lastWants starts false: FluentFlyout not wanting the host is no change, no ApplyTrayHost)");
    Check(ml::logs.size() == 2 && ml::logs[0] == L"Windows 11 26H2 (build 26300.1000), Liquid Dock " LD_VERSION_WSTR && ml::logs[1] == L"docks: 1",
          "start: dock.log gets the Windows line and 'docks: 1', nothing else");
    ml::Same(ExitTrace(), V({ L"Peek:WM_QUIT", L"ff.Shutdown", L"th.Stop", L"d0.~Dock", L"Dock::ResetNotifyBitmaps", L"taskbar.Restore",
                             L"DeregisterShellHookWindow(main)", L"UnregisterPowerSettingNotification(display)", L"ReleaseMutex(mutex)",
                             L"CloseHandle(mutex)", L"CloseHandle(watchdog)" }),
             "exit: WM_QUIT ends the loop at once; then FluentFlyout, the tray host, the docks, the notification bitmaps, the taskbar (restored directly), the shell hook, the power notification, the mutex, the watchdog's handle");
    Check(!g_docksReady && g_docks.empty(), "exit: docks gone, not ready");

    Fresh(); ml::build = L"19045";
    Run(L"");
    Check(!ml::logs.empty() && ml::logs[0] == L"Windows 10 26H2 (build 19045.1000), Liquid Dock " LD_VERSION_WSTR, "start: a build under 22000 is Windows 10 in the log line");
    Fresh(); ml::hklm = false;
    Run(L"");
    Check(!ml::logs.empty() && ml::logs[0] == L"docks: 1", "start: the version key cannot be opened - no Windows line");
    Fresh(); ml::uxtheme = false;
    Run(L"");
    Check(!ml::Has(StartTrace(), L"SetPreferredAppMode(1)") && !ml::Has(StartTrace(), L"GetProcAddress(uxtheme, #135)") &&
          ml::Has(StartTrace(), L"settings.Load"), "start: no uxtheme.dll - no themed menus, the start goes on");

    Fresh(); ml::watchdogStarts = false;
    Run(L"");
    {
        auto s = StartTrace();
        const int log = ml::IndexOf(s, L"Log: watchdog not ready; keeping native taskbar"), enumAt = ml::IndexOf(s, L"EnumDisplayMonitors(null, null)");
        Check(log > 0 && log == ml::IndexOf(s, L"StartWatchdog") + 1 && log < enumAt, "start: the watchdog does not come up - 'watchdog not ready; keeping native taskbar' before the docks");
        Check(ml::Has(s, L"WatchdogReady") && !ml::Has(s, L"taskbar.Hide") && ml::CountOf(s, L"taskbar.Restore") == 1,
              "...the docks are made, the taskbar is not hidden but restored (RestoreTaskbar)");
    }
    ml::Same(ExitTrace(), V({ L"Peek:WM_QUIT", L"ff.Shutdown", L"th.Stop", L"d0.~Dock", L"Dock::ResetNotifyBitmaps", L"taskbar.Restore",
                             L"DeregisterShellHookWindow(main)", L"UnregisterPowerSettingNotification(display)", L"ReleaseMutex(mutex)", L"CloseHandle(mutex)" }),
             "exit without a watchdog: the same, no watchdog handle to close");

    Fresh(); ml::mutexMade = false;
    rc = Run(L"");
    Check(rc == 1 && ml::trace.back() == L"CreateMutexW(null, 0, LiquidDock.SingleInstance)", "start: the instance mutex cannot be made - exit code 1, nothing after it");
    Fresh(); ml::mutexWait = WAIT_TIMEOUT;   // another dock runs
    rc = Run(L"");
    Check(rc == 0 && ml::trace.back() == L"CloseHandle(mutex)" && !ml::Has(ml::trace, L"LogStartSession"),
          "start: another dock holds the mutex for 3 s - exit code 0, the mutex closed, dock.log not started afresh");
    Fresh(); ml::mutexWait = WAIT_ABANDONED;   // the dock before crashed holding it
    rc = Run(L"");
    Check(rc == 0 && ml::Has(ml::trace, L"LogStartSession") && ml::Has(ml::trace, L"d0.Update"), "start: an abandoned mutex - this run is the dock");
    Fresh(); ml::gfxInits = false;
    rc = Run(L"");
    ml::Same(std::vector<std::wstring>(ml::trace.end() - 3, ml::trace.end()), V({ L"gfx.Init", L"taskbar.Restore", L"MessageBoxW(null, GPU 초기화에 실패했습니다., Liquid Dock, 0x10)" }),
             "start: the GPU cannot be set up - the taskbar restored, a message box");
    Check(rc == 1, "...exit code 1 (the mutex left to the process's end)");
    Fresh(); ml::windowMade = false;
    rc = Run(L"");
    Check(rc == 1 && ml::trace.back() == L"taskbar.Restore" && ml::trace[ml::trace.size() - 2].rfind(L"CreateWindowExW(", 0) == 0,
          "start: no main window - the taskbar restored, exit code 1");
    Fresh(); ml::restartUnelevated = true;
    rc = Run(L"--welcome");
    Check(rc == 0 && ml::trace.back() == L"RestartUnelevated([--welcome])" && !ml::Has(ml::trace, L"CreateMutexW(null, 0, LiquidDock.SingleInstance)"),
          "start as administrator handed to the user - exit code 0 before the mutex; RestartUnelevated gets the whole command line");
    Fresh(); ml::powerNotify = false;
    Run(L"");
    Check(!ml::Has(ExitTrace(), L"UnregisterPowerSettingNotification(display)"), "exit: no power notification registered - none unregistered");

    // the settings at the start
    Fresh(); g_settings.autostart = true; g_settings.revealHotkey = 1;   // (AutostartTests looks at the logon task; here only where it comes)
    Run(L"");
    {
        auto s = StartTrace();
        const int reapply = ml::IndexOf(s, L"oled.Reapply"), reg = ml::IndexOf(s, L"RegOpenKeyExW(HKCU, Software\\Microsoft\\Windows\\CurrentVersion\\Run, 0, 0x2)");
        Check(reg == reapply + 2 && ml::IndexOf(s, L"RegisterHotKey(main, 2, 0x4003, 0x44)") == ml::IndexOf(s, L"UnregisterHotKey(main, 2)") + 1,
              "start: autostart on - SetAutostart(true) after the docks and OLED; the reveal shortcut registered right after the emergency exit's");
    }
}

// ---------------------------------------------------------------- the command line
static void CommandLineTests() {
    struct W { const wchar_t* cmd; DWORD pid; const wchar_t* ready; const char* what; };
    const W watchdogs[] = {
        { L"--watchdog 4242 --ready Local\\LiquidDock.WatchdogReady.{00112233-4455-6677-8899-AABBCCDDEEFF}", 4242,
          L"Local\\LiquidDock.WatchdogReady.{00112233-4455-6677-8899-AABBCCDDEEFF}", "--watchdog <pid> --ready <event> (as StartWatchdog writes it): RunWatchdog(pid, the event)" },
        { L"--watchdog 4242 --ready Local\\A name with spaces --profile", 4242, L"Local\\A name with spaces --profile",
          "--ready's value runs to the end of the line, spaces and later flags included" },
        { L"--watchdog 77", 77, nullptr, "--watchdog <pid> without --ready: no event (null)" },
        { L"--watchdog 77 --ready", 77, nullptr, "--ready at the very end (no space after it): no event" },
        { L"--ready X --watchdog 9", 9, nullptr, "--ready before --watchdog at the line's start (no space before it): no event" },
        { L"--quit --watchdog 12", 12, nullptr, "--watchdog anywhere in the line wins over every other flag" },
        { L"--watchdog abc", 0, nullptr, "a pid that is no number: 0 (_wtoi)" },
    };
    for (const W& c : watchdogs) {
        Fresh(); ml::runWatchdogRc = 1237;
        const int rc = Run(c.cmd);
        wchar_t want[300];
        swprintf_s(want, L"RunWatchdog(%lu, %ls)", c.pid, c.ready ? c.ready : L"null");
        const std::string what = std::string("command line ") + c.what + " - LogAppend, then RunWatchdog alone (no DPI, no COM), its code returned";
        ml::Same(ml::trace, V({ L"LogAppend", want }), what.c_str());
        Check(rc == 1237, "...the watchdog's exit code is the exe's");
    }
    Fresh();
    Run(L"--watchdog");
    Check(ml::Has(ml::trace, L"d0.Update") && !ml::Has(ml::trace, L"RunWatchdog(0, null)"), "command line --watchdog without a space after it: not the watchdog, the dock starts");

    // --settings
    Fresh(); ml::otherDock = ml::kOther;
    int rc = Run(L"--settings");
    ml::Same(ml::trace, V({ L"LogAppend", L"SetProcessDpiAwarenessContext(per-monitor v2)", L"init_apartment(STA)", L"FindWindowW(LiquidDockMain, null)",
                           L"AllowSetForegroundWindow(ASFW_ANY)", L"PostMessageW(other, WM_APP+4, 0, 0)" }),
             "--settings while the dock runs: after DPI and COM, the dock found by its class, allowed to the front, WM_APP_SETTINGS posted");
    Check(rc == 0, "...exit code 0");
    Fresh();
    rc = Run(L"--settings");
    Check(rc == 0 && ml::CountOf(ml::trace, L"FindWindowW(LiquidDockMain, null)") == 1 && ml::Has(ml::trace, L"d0.Update") && !ml::Has(ml::trace, L"sw.Show(desk)"),
          "--settings with no dock running: this run becomes the dock (the settings window does not open by itself)");
    Fresh(); ml::otherDock = ml::kOther;
    rc = Run(L"--quit --settings");
    Check(rc == 0 && ml::Has(ml::trace, L"PostMessageW(other, WM_APP+4, 0, 0)") && !ml::Has(ml::trace, L"PostMessageW(other, 0x10, 0, 0)") &&
          !ml::Has(ml::trace, L"RestoreTaskbarWhenIdle(5000)"), "--quit --settings with a dock: --settings is looked at first (the dock is not closed)");
    Fresh(); ml::otherDock = ml::kOther;
    Run(L"--settingsX");
    Check(ml::Has(ml::trace, L"PostMessageW(other, WM_APP+4, 0, 0)"), "flags are found anywhere in the line, as parts of longer words too (--settingsX)");

    // --quit, --restore
    Fresh(); ml::otherDock = ml::kOther; ml::restoreWhenIdleRc = 0;
    rc = Run(L"--quit");
    ml::Same(ml::trace, V({ L"LogAppend", L"SetProcessDpiAwarenessContext(per-monitor v2)", L"init_apartment(STA)", L"FindWindowW(LiquidDockMain, null)",
                           L"GetWindowThreadProcessId(other)", L"OpenProcess(0x100001, 0, 3131)",
                           L"PostMessageW(other, 0x10, 0, 0)", L"WaitForSingleObject(other dock, 3000)", L"CloseHandle(other dock)",
                           L"RestoreTaskbarWhenIdle(5000)" }),
             "--quit with a dock running: its process opened (SYNCHRONIZE | PROCESS_TERMINATE), WM_CLOSE posted, 3 s for it to end, the taskbar restored when idle (5 s); OLED left alone");
    Check(rc == 0, "...exit code: RestoreTaskbarWhenIdle's (0)");
    Fresh(); ml::otherDock = ml::kOther; ml::otherWait = WAIT_TIMEOUT;
    Run(L"--quit");
    {
        const int t = ml::IndexOf(ml::trace, L"TerminateProcess(other dock, 1)");
        Check(t > 0 && ml::trace[t + 1] == L"WaitForSingleObject(other dock, 2000)" && ml::trace[t + 2] == L"CloseHandle(other dock)",
              "--quit with a hung dock (no end in 3 s): it is ended (code 1), 2 s more, then the restore");
    }
    Fresh(); ml::otherDock = ml::kOther; ml::openProcessWorks = false;   // the dock runs elevated (--keep-admin), --quit as the user
    rc = Run(L"--quit");
    ml::Same(ml::trace, V({ L"LogAppend", L"SetProcessDpiAwarenessContext(per-monitor v2)", L"init_apartment(STA)", L"FindWindowW(LiquidDockMain, null)",
                           L"GetWindowThreadProcessId(other)", L"OpenProcess(0x100001, 0, 3131)", L"PostMessageW(other, 0x10, 0, 0)",
                           L"RestoreTaskbarWhenIdle(5000)" }),
             "--quit with a dock whose process cannot be opened: WM_CLOSE posted all the same, then straight to the restore (no wait, no TerminateProcess, no CloseHandle)");
    Check(rc == 0, "...exit code: RestoreTaskbarWhenIdle's (0)");
    Fresh(); ml::restoreWhenIdleRc = 1237;
    rc = Run(L"--quit");
    ml::Same(ml::trace, V({ L"LogAppend", L"SetProcessDpiAwarenessContext(per-monitor v2)", L"init_apartment(STA)", L"FindWindowW(LiquidDockMain, null)",
                           L"RestoreTaskbarWhenIdle(5000)" }), "--quit with no dock running: just the restore");
    Check(rc == 1237, "...its code (ERROR_RETRY) is the exit code");
    Fresh();
    rc = Run(L"--restore");
    Check(rc == 0 && ml::trace.back() == L"oled.Disable" && ml::trace[ml::trace.size() - 2] == L"RestoreTaskbarWhenIdle(5000)",
          "--restore (uninstall, the restore shortcut): the taskbar first, then OLED undone - exit code 0");
    Fresh(); ml::oledDisables = false;
    rc = Run(L"--restore");
    Check(rc == ERROR_RETRY, "--restore: the OLED record not undone (Explorer away) - ERROR_RETRY (the uninstaller keeps it)");
    Fresh(); ml::oledDisables = false; ml::restoreWhenIdleRc = 5;
    rc = Run(L"--restore");
    Check(rc == 5, "--restore: the taskbar's code wins when it is not 0");
    Fresh(); ml::restoreWhenIdleRc = 5;
    Run(L"--restored");
    Check(ml::trace.back() == L"oled.Disable", "--restored (a longer word) counts as --restore");

    // --keep-admin, --from-admin
    Fresh();
    Run(L"--keep-admin --welcome");
    Check(!ml::Has(ml::trace, L"RestartUnelevated([--keep-admin --welcome])") && ml::Has(ml::trace, L"LogStartSession"), "--keep-admin: no hand-over to the user, the dock runs elevated");
    Fresh(); ml::elevated = true;
    Run(L"--from-admin");
    {
        auto s = StartTrace();
        const int line = ml::IndexOf(s, L"Log: started as administrator, running on as the user - still elevated (Explorer is)");
        Check(!ml::Has(s, L"RestartUnelevated([--from-admin])") && line > 0 && s[line - 1] == L"ProcessElevated" &&
              s[line - 2] == L"Log: Windows 11 26H2 (build 26300.1000), Liquid Dock " LD_VERSION_WSTR,
              "--from-admin (the copy started by the hand-over): not handed over again; after the Windows line 'started as administrator, running on as the user - still elevated (Explorer is)'");
    }
    Fresh(); ml::elevated = false;
    Run(L"x --from-admin");
    Check(ml::Has(ml::logs, L"started as administrator, running on as the user"), "...not elevated any more: the line without the suffix");
    Fresh();
    Run(L"--demo");
    Check(g_demoMode, "--demo: g_demoMode");
    Fresh();
    Run(L"--bogus --whatever");
    ml::Same(StartTrace(), PlainStart(L"--bogus --whatever"), "unknown arguments: a plain start (RestartUnelevated gets them as they are)");
    Fresh();
    Run(nullptr);
    ml::Same(StartTrace(), PlainStart(L""), "no command line at all (null): as an empty one");

    // --welcome: the dock shown for 3.5 s
    Fresh();
    Run(L"--welcome", 0, [](int p) { if (ml::Now() > 6) ml::Quit(); (void)p; });
    {
        double lastShown = -1, firstHidden = -1; const double t0 = ml::updates.empty() ? 0 : ml::updates[0].now;
        for (auto& u : ml::updates) { if (u.forceShow) lastShown = u.now; else if (firstHidden < 0) firstHidden = u.now; }
        std::printf("  --welcome: shown until %.3f s, hidden from %.3f s after the first pass\n", lastShown - t0, firstHidden - t0);
        Check(!ml::updates.empty() && ml::updates[0].forceShow && lastShown - t0 < 3.5 && firstHidden - t0 >= 3.5 && firstHidden - t0 < 3.6,
              "--welcome: the dock forced up from the first pass until the first pass 3.5 s after the start");
    }

#ifdef LD_DEV
    struct D { const wchar_t* flag; int msg; };
    const D dev[] = { { L"--debug-show", 10 }, { L"--debug-hide", 11 }, { L"--snap", 12 }, { L"--debug-panel", 13 }, { L"--debug-alert", 14 },
                      { L"--autostart-on", 15 }, { L"--autostart-off", 16 }, { L"--rec-start", 17 }, { L"--rec-stop", 18 }, { L"--debug-bounce", 19 },
                      { L"--debug-overflow", 21 }, { L"--debug-calendar", 22 }, { L"--debug-volume", 23 }, { L"--debug-network", 24 },
                      { L"--debug-device-reset", 25 }, { L"--debug-settings", 26 }, { L"--debug-icons-hide", 27 }, { L"--debug-icons-show", 28 },
                      { L"--rec-tight", 29 }, { L"--rec-full", 31 } };
    bool all = true;
    for (const D& d : dev) {
        Fresh(); ml::otherDock = ml::kOther;
        const int r = Run(d.flag);
        wchar_t post[80]; swprintf_s(post, L"PostMessageW(other, WM_APP+%d, 0, 0)", d.msg);
        const auto want = V({ L"LogAppend", L"SetProcessDpiAwarenessContext(per-monitor v2)", L"init_apartment(STA)", L"FindWindowW(LiquidDockMain, null)", post });
        if (ml::trace != want || r != 0) { all = false; std::printf("  %s: not as expected\n", ml::U8(d.flag).c_str()); for (auto& s : ml::trace) std::printf("    %s\n", ml::U8(s).c_str()); }
    }
    Check(all, "dev build: each of the 20 developer flags posts its WM_APP+n to the running dock (after DPI and COM) and exits with 0");
    Fresh();
    rc = Run(L"--debug-show");
    ml::Same(ml::trace, V({ L"LogAppend", L"SetProcessDpiAwarenessContext(per-monitor v2)", L"init_apartment(STA)", L"FindWindowW(LiquidDockMain, null)" }),
             "dev build: a developer flag with no dock running - nothing posted, no dock started");
    Check(rc == 0, "...exit code 0");
    Fresh(); ml::otherDock = ml::kOther;
    Run(L"--rec-full --debug-hide --rec-stop");
    Check(ml::trace.back() == L"PostMessageW(other, WM_APP+11, 0, 0)" && ml::CountPrefix(ml::trace, L"PostMessageW(") == 1,
          "dev build: several flags - the first in the table's order (--debug-hide before --rec-stop and --rec-full), not the line's; one message");
    Fresh(); ml::otherDock = ml::kOther;
    Run(L"--debug-show --settings --quit");
    Check(ml::trace.back() == L"PostMessageW(other, WM_APP+10, 0, 0)", "dev build: a developer flag is looked at before --settings and --quit");
    Fresh(); ml::otherDock = ml::kOther;
    Run(L"--watchdog 5 --debug-show");
    Check(ml::trace.back() == L"RunWatchdog(5, null)", "dev build: --watchdog still before the developer flags");
#else
    Fresh(); ml::otherDock = ml::kOther;
    rc = Run(L"--debug-show");
    Check(!ml::Has(ml::trace, L"PostMessageW(other, WM_APP+10, 0, 0)") && ml::CountOf(ml::trace, L"FindWindowW(LiquidDockMain, null)") == 0 &&
          ml::Has(ml::trace, L"LogStartSession"), "release build: developer flags are not read (--debug-show starts the dock as any unknown argument)");
#endif
}

// ---------------------------------------------------------------- BuildDocks
static void BuildDocksTests() {
    struct Config { std::vector<ml::Mon> mons; std::vector<std::wstring> made; const char* what; };
    const Config configs[] = {
        { { ml::kDesk }, { L"Dock(desk, 0, 1)", L"d0.Create" }, "one 2560x1440 monitor at 100 %: one dock, index 0, primary" },
        { { ml::kLaptop }, { L"Dock(laptop, 0, 1)", L"d0.Create" }, "a laptop's 2880x1800 at 150 %: one dock, index 0, primary (main.cpp does no DPI)" },
        { { ml::kDesk, ml::kPortrait }, { L"Dock(desk, 0, 1)", L"d0.Create", L"Dock(portrait, 1, 0)", L"d1.Create" },
          "a two-monitor desktop (2560x1440 + portrait 1440x2560 at -1440,-560): two docks, index in the order Windows lists the monitors, the one at 0,0 primary" },
        { { ml::kPortrait, ml::kDesk }, { L"Dock(portrait, 0, 0)", L"d0.Create", L"Dock(desk, 1, 1)", L"d1.Create" },
          "...listed the other way round: the portrait dock is index 0, not primary" },
    };
    for (const Config& c : configs) {
        Fresh(); ml::mons = c.mons; g_mainWnd = ml::kMain;
        BuildDocks();
        std::vector<std::wstring> made;
        for (auto& s : ml::trace) if (s.rfind(L"Dock(", 0) == 0 || s.find(L".Create") != std::wstring::npos) made.push_back(s);
        const std::string what = std::string("BuildDocks: ") + c.what;
        Check(made == c.made && g_docksReady && g_docks.size() == c.mons.size(), what.c_str());
    }
    Fresh(); ml::mons = { ml::kDesk, ml::kPortrait }; g_mainWnd = ml::kMain;
    BuildDocks();
    ml::Same(ml::trace, V({ L"EnumDisplayMonitors(null, null)", L"MonitorFromPoint(0,0,1)", L"Dock(desk, 0, 1)", L"d0.Create", L"Dock(portrait, 1, 0)",
                           L"d1.Create", L"Log: docks: 2", L"WatchdogReady", L"taskbar.Hide", L"oled.InvalidateCovers", L"ff.InvalidateMonitors",
                           L"ff.WantsHost", L"th.Stop" }),
             "BuildDocks: monitors listed, the primary asked at 0,0, each dock made and created, 'docks: 2', the taskbar hidden (watchdog ready), OLED covers and FluentFlyout told, the tray host settled");
    ml::trace.clear();
    BuildDocks();
    Check(ml::trace.size() > 3 && ml::trace[0] == L"d0.~Dock" && ml::trace[1] == L"d1.~Dock" && ml::trace[2] == L"EnumDisplayMonitors(null, null)" &&
          !ml::Has(ml::trace, L"taskbar.Hide"), "BuildDocks again: the old docks go first; a hidden taskbar is not hidden again");

    Fresh(); ml::mons = { ml::kDesk, ml::kPortrait }; ml::dock[1].createOk = false; g_mainWnd = ml::kMain;
    BuildDocks();
    {
        const int failed = ml::IndexOf(ml::trace, L"Log: dock create failed for monitor 1");
        Check(failed > 0 && ml::trace[failed + 1] == L"d1.~Dock" && ml::trace[failed + 2] == L"Log: docks: 1" && ml::trace[failed + 3] == L"d0.~Dock",
              "BuildDocks: a dock that cannot be created - 'dock create failed for monitor 1', 'docks: 1', then every dock let go (no monitor may lack one)");
        Check(!g_docksReady && g_docks.empty() && ml::Has(ml::trace, L"taskbar.Restore") && !ml::Has(ml::trace, L"WatchdogReady") && !ml::Has(ml::trace, L"taskbar.Hide"),
              "...not ready: the taskbar restored, the watchdog not asked");
    }
    Fresh(); ml::mons = { ml::kDesk, ml::kPortrait }; ml::dock[0].createOk = false; g_mainWnd = ml::kMain;
    BuildDocks();
    ml::Same(ml::trace, V({ L"EnumDisplayMonitors(null, null)", L"MonitorFromPoint(0,0,1)", L"Dock(desk, 0, 1)", L"d0.Create",
                           L"Log: dock create failed for monitor 0", L"d0.~Dock", L"Dock(portrait, 1, 0)", L"d1.Create", L"Log: docks: 1", L"d1.~Dock",
                           L"taskbar.Restore", L"oled.InvalidateCovers", L"ff.InvalidateMonitors", L"ff.WantsHost", L"th.Stop" }),
             "BuildDocks: the first of two docks cannot be created - 'dock create failed for monitor 0', the next monitor's dock still made, 'docks: 1', then let go; the taskbar restored");
    Check(!g_docksReady && g_docks.empty(), "...not ready, no dock kept");
    Fresh(); ml::mons = {}; g_mainWnd = ml::kMain;
    BuildDocks();
    Check(!g_docksReady && ml::Has(ml::trace, L"Log: docks: 0") && ml::Has(ml::trace, L"taskbar.Restore"), "BuildDocks: no monitor - 'docks: 0', not ready, the taskbar restored");
    Fresh(); ml::watchdogReady = false; g_mainWnd = ml::kMain;
    BuildDocks();
    Check(g_docksReady && ml::Has(ml::trace, L"WatchdogReady") && ml::Has(ml::trace, L"taskbar.Restore") && !ml::Has(ml::trace, L"taskbar.Hide") &&
          g_restoreRetry.restored, "BuildDocks: docks made but the watchdog gone - ready, but the taskbar restored (and the try recorded)");
    Fresh(); ml::mons = { ml::kDesk, ml::kPortrait }; FakeDock::carryRecording = true; FakeDock::carryCrop = { 10, 20, 30, 40 }; FakeDock::carryScale = 0.875f; g_mainWnd = ml::kMain;
    BuildDocks();
    Check(ml::CountPrefix(ml::trace, L"d0.StartRecording(10 20 30 40, 0.875, 1)") == 1 && ml::CountPrefix(ml::trace, L"d1.StartRecording") == 0 &&
          ml::IndexOf(ml::trace, L"d0.StartRecording(10 20 30 40, 0.875, 1)") == ml::IndexOf(ml::trace, L"Log: docks: 2") + 1,
          "BuildDocks: a recording in progress goes on on the new primary dock (appending), right after 'docks:'");

    // the tray host (ApplyTrayHost, at the end of BuildDocks)
    struct T { bool icons, widget, running, explorer, starts; std::vector<std::wstring> tail; const char* what; };
    const T trays[] = {
        { false, false, false, true, true, { L"ff.WantsHost", L"th.Stop" }, "tray icons off and no FluentFlyout widget: the tray host stopped" },
        { true, false, false, true, true, { L"ff.WantsHost", L"ExplorerTaskbar", L"th.Running", L"th.Start(1)" }, "tray icons on, Explorer's taskbar there: started, announcing" },
        { false, true, false, true, true, { L"ff.WantsHost", L"ExplorerTaskbar", L"th.Running", L"th.Start(0)" }, "FluentFlyout's widget only: started without announcing" },
        { true, false, true, true, true, { L"ff.WantsHost", L"ExplorerTaskbar", L"th.Running", L"th.Announce" }, "tray icons on and the host running: programs asked again" },
        { false, true, true, true, true, { L"ff.WantsHost", L"ExplorerTaskbar", L"th.Running" }, "the widget only and the host running: nothing" },
        { true, false, false, false, true, { L"ff.WantsHost", L"ExplorerTaskbar", L"th.Stop" }, "no Explorer taskbar yet (early logon): stopped, waits for TaskbarCreated" },
        { true, false, false, true, false, { L"ff.WantsHost", L"ExplorerTaskbar", L"th.Running", L"th.Start(1)", L"Log: tray host: could not start" }, "the host cannot start: 'tray host: could not start'" },
    };
    for (const T& t : trays) {
        Fresh(); g_settings.trayIcons = t.icons; ml::ffWants = t.widget; ml::thRunning = t.running; ml::explorerTaskbar = t.explorer; ml::thStarts = t.starts; g_mainWnd = ml::kMain;
        BuildDocks();
        std::vector<std::wstring> tail(ml::trace.end() - std::min(ml::trace.size(), t.tail.size()), ml::trace.end());
        const std::string what = std::string("ApplyTrayHost: ") + t.what;
        ml::Same(tail, t.tail, what.c_str());
        ml::ffWants = false;
    }
    Fresh(); g_settings.trayIcons = true; g_mainWnd = ml::kMain; g_running = false;
    BuildDocks();
    Check(ml::trace.back() == L"th.Stop" && !ml::Has(ml::trace, L"ExplorerTaskbar"), "ApplyTrayHost: the dock exiting (g_running false) - stopped, Explorer not asked");
}

// ---------------------------------------------------------------- MainProc
static std::vector<std::wstring> Proc(UINT m, WPARAM w, LPARAM l, LRESULT* r) {
    ml::trace.clear(); ml::logs.clear();
    *r = MainProc(ml::kMain, m, w, l);
    return ml::trace;
}
static void SetUpProc(const std::vector<ml::Mon>& mons = { ml::kDesk }) {
    Fresh(); ml::mons = mons; g_mainWnd = ml::kMain; g_msgShellHook = 0xC0A1; g_msgTaskbarCreated = 0xC0A2;
    BuildDocks();
    ml::trace.clear(); ml::logs.clear();
}

static void MainProcTests() {
    LRESULT r = 0;
    SetUpProc();
    ml::Same(Proc(0xC0A1, 6, 0x777, &r), V({ L"apps.OnShellHook(6, 1911)" }), "MainProc: SHELLHOOK goes to the app model");
    Check(r == 0, "...returns 0");
    ml::Same(Proc(0xC0A2, 0, 0, &r), V({ L"WatchdogReady", L"taskbar.Hide", L"oled.Reapply", L"ff.WantsHost", L"th.Stop" }),
             "MainProc: TaskbarCreated (a new Explorer) while the docks run - the taskbar hidden again, OLED reapplied, the tray host settled");
    Check(r == 0, "...returns 0");
    ml::watchdogReady = false;
    ml::Same(Proc(0xC0A2, 0, 0, &r), V({ L"WatchdogReady", L"taskbar.Restore", L"oled.Reapply", L"ff.WantsHost", L"th.Stop" }),
             "MainProc: TaskbarCreated without the watchdog - the taskbar restored instead");
    ml::watchdogReady = true; g_docksReady = false;
    ml::Same(Proc(0xC0A2, 0, 0, &r), V({ L"taskbar.Restore", L"oled.Reapply", L"ff.WantsHost", L"th.Stop" }), "...docks not ready: restored, the watchdog not asked");
    g_docksReady = true; g_running = false;
    ml::Same(Proc(0xC0A2, 0, 0, &r), V({ L"taskbar.Restore", L"oled.Reapply", L"ff.WantsHost", L"th.Stop" }), "...the dock exiting: restored");
    g_running = true;
    g_msgShellHook = 0; g_msgTaskbarCreated = 0;
    ml::Same(Proc(0, 0, 0, &r), V({ L"DefWindowProcW(main, 0x0)" }), "MainProc: before the messages are registered, message 0 is neither SHELLHOOK nor TaskbarCreated");
    Check(r == 0xDEF, "...DefWindowProc's answer returned");
    g_msgShellHook = 0xC0A1; g_msgTaskbarCreated = 0xC0A2;

    ml::nowUs = 5000000;
    ml::Same(Proc(WM_DISPLAYCHANGE, 32, 0, &r), V({}), "MainProc: WM_DISPLAYCHANGE - nothing at once");
    Check(r == 0 && std::fabs(g_rebuildAt - 5.8) < 1e-9, "...the docks rebuilt 0.8 s later (g_rebuildAt)");
    g_rebuildAt = 0;
    Proc(WM_DPICHANGED, 0, 0, &r);
    Check(r == 0 && std::fabs(g_rebuildAt - 5.8) < 1e-9, "MainProc: WM_DPICHANGED - the same 0.8 s");
    g_rebuildAt = 0;
    Proc(WM_APP_REBUILD, 0, 0, &r);
    Check(r == 0 && g_rebuildAt == 5.0, "MainProc: WM_APP_REBUILD - rebuilt on the next pass (now)");
    g_rebuildAt = 0;

    const wchar_t tray[] = L"TraySettings", trayLower[] = L"traysettings", other[] = L"ImmersiveColorSet";
    ml::Same(Proc(WM_SETTINGCHANGE, 0, (LPARAM)tray, &r), V({ L"taskbar.RecheckSoon" }), "MainProc: WM_SETTINGCHANGE \"TraySettings\" - the enforcer asks soon");
    Check(r == 0, "...returns 0");
    ml::Same(Proc(WM_SETTINGCHANGE, 0, (LPARAM)trayLower, &r), V({ L"taskbar.RecheckSoon" }), "...in any case");
    ml::Same(Proc(WM_SETTINGCHANGE, 0, (LPARAM)other, &r), V({}), "...another area: nothing");
    ml::Same(Proc(WM_SETTINGCHANGE, 0, 0, &r), V({}), "...no area: nothing");
    Check(r == 0, "...returns 0 (never DefWindowProc)");

    ml::Same(Proc(WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE, 0, &r), V({ L"sys.PollBattery" }), "MainProc: power status change - the battery read at once");
    Check(r == TRUE, "...returns TRUE");
    {
        struct { POWERBROADCAST_SETTING s; BYTE more[8]; } p{};
        p.s.PowerSetting = kDisplayStatus; p.s.DataLength = sizeof(DWORD);
        auto set = [&](DWORD v) { memcpy(p.s.Data, &v, sizeof v); };
        set(0);
        ml::Same(Proc(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&p, &r), V({ L"Log: display: off - nobody sees the docks: the loop looks every 100 ms" }),
                 "MainProc: the display off (GUID_SESSION_DISPLAY_STATUS 0) - logged, taken as off");
        Check(r == TRUE && g_look.displayOff, "...returns TRUE, g_displayOff");
        ml::Same(Proc(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&p, &r), V({}), "...off again: no second line");
        set(2);
        ml::Same(Proc(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&p, &r), V({ L"Log: display: on" }), "...dimmed (2) is on (still seen): 'display: on'");
        Check(!g_look.displayOff, "...not off");
        set(0); Proc(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&p, &r);
        const int toldBefore = startmenu::displayOnCalls;
        set(1);
        ml::Same(Proc(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&p, &r), V({ L"Log: display: on" }), "...on (1): 'display: on'");
        Check(startmenu::displayOnCalls == toldBefore + 1 && startmenu::displayOnAt == ml::Now(),
              "...and the start button is told when (startmenu::DisplayCameOn, for its dock.log lines), once per change to on");
        set(0); p.s.DataLength = 2;
        Proc(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&p, &r);
        Check(!g_look.displayOff && r == TRUE, "...data shorter than a DWORD: ignored");
        p.s.DataLength = 4; p.s.PowerSetting = GUID{ 1, 2, 3, { 4 } };
        Proc(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, (LPARAM)&p, &r);
        Check(!g_look.displayOff, "...another power setting: ignored");
        Proc(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, 0, &r);
        Check(!g_look.displayOff && r == TRUE, "...no data: ignored, TRUE");
        ml::Same(Proc(WM_POWERBROADCAST, PBT_APMSUSPEND, 0, &r), V({}), "...suspend: nothing, TRUE");
        Check(r == TRUE, "...returns TRUE");
    }

    ml::cursor = { 100, 200 };
    ml::Same(Proc(WM_APP_SETTINGS, 0, 0, &r), V({ L"GetCursorPos", L"MonitorFromPoint(100,200,1)", L"sw.Show(desk)" }),
             "MainProc: WM_APP_SETTINGS - the settings window on the pointer's monitor (primary if none)");
    Check(r == 0, "...returns 0");
    ml::Same(Proc(WM_APP_RELAYOUT, 0, 0, &r), V({ L"d0.Relayout" }), "MainProc: WM_APP_RELAYOUT - each dock laid out in place");
    Check(r == 0 && g_rebuildAt == 0, "...no rebuild when it fits");
    SetUpProc({ ml::kDesk, ml::kPortrait }); ml::dock[0].relayoutOk = false;
    ml::Same(Proc(WM_APP_RELAYOUT, 0, 0, &r), V({ L"d0.Relayout" }), "...a dock that must be made again: a rebuild now, the other docks not asked");
    Check(g_rebuildAt == ml::Now(), "...g_rebuildAt = now");
    SetUpProc();
    ml::Same(Proc(WM_APP_ENFORCE, 0, 0, &r), V({ L"taskbar.Enforce(1, 1)" }), "MainProc: WM_APP_ENFORCE (from the enforcer thread) - Enforce on the UI thread");
    Check(r == 0 && g_enforceCount == 1, "...counted for --profile (re-hides)");
    ml::Same(Proc(WM_APP_HOTKEY, 0, 0, &r), V({ L"UnregisterHotKey(main, 2)" }), "MainProc: WM_APP_HOTKEY - the reveal shortcut applied again (off: unregistered)");
    ml::Same(Proc(WM_APP_TRAYHOST, 0, 0, &r), V({ L"ff.WantsHost", L"th.Stop" }), "MainProc: WM_APP_TRAYHOST - the tray host settled");
    Check(r == 0, "...returns 0");
    {
        const std::wstring tempBefore = ml::tempDir;
        ml::tempDir = tempBefore + L"no such folder\\";
        g_settings.autostart = false;
        auto t = Proc(WM_APP_AUTOSTART, 0, 0, &r);
        Check(r == 0 && !t.empty() && t[0] == L"GetModuleFileNameW(null, 260)" && ml::Has(t, L"RegDeleteValueW(run, LiquidDock)"),
              "MainProc: WM_APP_AUTOSTART - SetAutostart with the setting as it is (off: the Run value deleted)");
        ml::tempDir = tempBefore;
        g_settings.autostart = true;   // the settings window's autostart switch turned on (settings_window_pages.cpp posts WM_APP_AUTOSTART)
        ml::processes.clear();
        t = Proc(WM_APP_AUTOSTART, 0, 0, &r);
        const std::wstring create = L"\"C:\\Windows\\System32\\schtasks.exe\" /Create /F /TN LiquidDock /XML \"" + ml::tempDir + L"LiquidDockLogon.xml\"";
        Check(r == 0 && !t.empty() && t[0] == L"GetModuleFileNameW(null, 260)" &&
              ml::Has(t, L"RegSetValueExW(run, LiquidDock, 0, 1, [\"C:\\Tools & Docks\\LiquidDock\\LiquidDock.exe\"], 90)") && !ml::Has(t, L"RegDeleteValueW(run, LiquidDock)") &&
              ml::processes.size() == 1 && ml::processes[0] == create && t.back() == L"Log: logon task rc=0",
              "...on: the Run value set (the quoted exe) and the logon task made (schtasks /Create), 'logon task rc=0'");
        g_settings.autostart = false;
    }

    // WM_HOTKEY
    SetUpProc({ ml::kDesk, ml::kPortrait });
    ml::cursor = { -700, 1000 };
    ml::Same(Proc(WM_HOTKEY, 2, 0, &r), V({ L"GetCursorPos", L"MonitorFromPoint(-700,1000,1)", L"d1.HotkeyReveal" }),
             "MainProc: the reveal shortcut (hotkey 2) - the dock of the pointer's monitor revealed");
    Check(r == 0 && g_running, "...returns 0, the dock runs on");
    ml::Same(Proc(WM_HOTKEY, 1, 0, &r), V({}), "MainProc: the emergency exit (hotkey 1, Ctrl+Alt+Shift+F12)");
    Check(r == 0 && !g_running, "...ends the loop (g_running false)");
    g_running = true;
    Proc(WM_HOTKEY, 7, 0, &r);
    Check(!g_running, "...any hotkey but 2 ends it");
    g_running = true;

    // the session's end, close
    ml::Same(Proc(WM_QUERYENDSESSION, 0, 0, &r), V({}), "MainProc: WM_QUERYENDSESSION - TRUE, nothing else");
    Check(r == TRUE && g_running, "...returns TRUE");
    ml::Same(Proc(WM_ENDSESSION, 0, 0, &r), V({}), "MainProc: WM_ENDSESSION not ending - nothing");
    Check(r == 0 && g_running, "...returns 0");
    ml::Same(Proc(WM_ENDSESSION, 1, 0, &r), V({ L"taskbar.Restore" }), "MainProc: WM_ENDSESSION (Windows logs off) - the taskbar restored at once");
    Check(r == 0 && !g_running, "...the loop ends");
    g_running = true;
    ml::Same(Proc(WM_CLOSE, 0, 0, &r), V({}), "MainProc: WM_CLOSE (--quit) - the loop ends, no DefWindowProc (the window is not destroyed here)");
    Check(r == 0 && !g_running, "...returns 0, g_running false");
    g_running = true;
    ml::Same(Proc(WM_DESTROY, 0, 0, &r), V({ L"DefWindowProcW(main, 0x2)" }), "MainProc: WM_DESTROY - DefWindowProc (main.cpp has no case for it)");
    ml::Same(Proc(WM_PAINT, 0, 0, &r), V({ L"DefWindowProcW(main, 0xf)" }), "MainProc: any other message - DefWindowProc");
    Check(r == 0xDEF, "...its answer returned");

    // the developer messages (every build handles them; a dev build's command line sends them)
    SetUpProc({ ml::kDesk, ml::kPortrait });
    Proc(WM_DEV_SHOW, 0, 0, &r);
    Check(r == 0 && FakeDock::debugForceShow && !FakeDock::debugForcePanel && ml::trace.empty(), "WM_APP+10 (--debug-show): docks forced up");
    Proc(WM_DEV_PANEL, 0, 0, &r);
    Check(FakeDock::debugForceShow && FakeDock::debugForcePanel, "WM_APP+13 (--debug-panel): up with the sensor panel");
    Proc(WM_DEV_HIDE, 0, 0, &r);
    Check(!FakeDock::debugForceShow && !FakeDock::debugForcePanel, "WM_APP+11 (--debug-hide): both off");
    Proc(WM_DEV_ALERT, 0, 0, &r);
    Check(fakeSensors.alertPending && r == 0, "WM_APP+14 (--debug-alert): a sensor alert pending");
    ml::Same(Proc(WM_DEV_SNAP, 0, 0, &r), V({ L"d0.RequestSnapshot", L"d1.RequestSnapshot" }), "WM_APP+12 (--snap): every dock takes a snapshot");
    ml::Same(Proc(WM_DEV_REC_STOP, 0, 0, &r), V({ L"d0.StopRecording", L"d1.StopRecording" }), "WM_APP+18 (--rec-stop): every dock stops recording");
    struct Rc { Edge edge; UINT msg; const wchar_t* rec; const char* what; };
    const Rc recs[] = {
        { Edge::Bottom, WM_DEV_REC_START, L"d0.StartRecording(0 720 2560 1440, 0.500, 0)", "WM_APP+17 (--rec-start), bottom dock: the lower half of the primary monitor at 0.5" },
        { Edge::Left, WM_DEV_REC_START, L"d0.StartRecording(0 0 1280 1440, 0.500, 0)", "...left dock: the left half" },
        { Edge::Right, WM_DEV_REC_START, L"d0.StartRecording(1280 0 2560 1440, 0.500, 0)", "...right dock: the right half" },
        { Edge::Top, WM_DEV_REC_START, L"d0.StartRecording(0 0 2560 720, 0.500, 0)", "...top dock: the upper half" },
        { Edge::Bottom, WM_DEV_REC_TIGHT, L"d0.StartRecording(180 680 2380 1440, 0.875, 0)", "WM_APP+29 (--rec-tight), bottom: 2200 x 760 px at the middle of the edge, 0.875" },
        { Edge::Top, WM_DEV_REC_TIGHT, L"d0.StartRecording(180 0 2380 760, 0.875, 0)", "...top" },
        { Edge::Left, WM_DEV_REC_TIGHT, L"d0.StartRecording(0 -380 760 1820, 0.875, 0)", "...left (760 x 2200, past the monitor's ends on a 1440 px side)" },
        { Edge::Right, WM_DEV_REC_TIGHT, L"d0.StartRecording(1800 -380 2560 1820, 0.875, 0)", "...right" },
        { Edge::Bottom, WM_DEV_REC_FULL, L"d0.StartRecording(0 0 2560 1440, 0.500, 0)", "WM_APP+31 (--rec-full): the whole primary monitor at 0.5" },
    };
    for (const Rc& c : recs) {
        g_settings.edge = c.edge;
        const auto t = Proc(c.msg, 0, 0, &r);
        const std::string what = std::string(c.what) + " (only the primary dock; its monitor asked)";
        ml::Same(t, V({ L"GetMonitorInfoW(desk)", c.rec }), what.c_str());
    }
    g_settings.edge = Edge::Bottom;
    {   // --debug-bounce: KakaoTalk first, else the first running app
        auto app = [](int id, const wchar_t* exe, bool running) {
            auto a = std::make_shared<AppItem>(); a->id = id; a->exe = exe; if (running) a->windows.push_back((HWND)0x99); return a;
        };
        fakeApps.items = { app(1, L"c:\\a\\notepad.exe", true), app(2, L"c:\\kakao\\kakaotalk.exe", false), app(3, L"c:\\b\\code.exe", true) };
        ml::nowUs = 9000000;
        Proc(WM_DEV_BOUNCE, 0, 0, &r);
        Check(r == 0 && fakeApps.items[1]->attention && fakeApps.items[1]->lastAttentionBounce == 9.0 && fakeApps.attentionEvents.size() == 1 &&
              fakeApps.attentionEvents[0].id == 2 && !fakeApps.items[0]->attention, "WM_APP+19 (--debug-bounce): KakaoTalk bounces (running or not), stamped now");
        fakeApps.items.erase(fakeApps.items.begin() + 1); fakeApps.attentionEvents.clear();
        Proc(WM_DEV_BOUNCE, 0, 0, &r);
        Check(fakeApps.items[0]->attention && fakeApps.attentionEvents.size() == 1 && fakeApps.attentionEvents[0].id == 1, "...no KakaoTalk: the first running app");
        fakeApps.items.clear(); fakeApps.attentionEvents.clear();
        Proc(WM_DEV_BOUNCE, 0, 0, &r);
        Check(fakeApps.attentionEvents.empty() && r == 0, "...no app: nothing");
    }
    ml::cursor = { 300, 400 };
    ml::Same(Proc(WM_DEV_OVERFLOW, 0, 0, &r), V({ L"GetCursorPos", L"nt.OpenOverflow(300,400, edge 0, 300 400 301 401)", L"Log: debug overflow: 1" }),
             "WM_APP+21 (--debug-overflow): the hidden-icons window at the pointer, logged");
    ml::Same(Proc(WM_DEV_CALENDAR, 0, 0, &r), V({ L"nt.OpenCalendar" }), "WM_APP+22 (--debug-calendar)");
    ml::Same(Proc(WM_DEV_VOLUME, 0, 0, &r), V({ L"GetCursorPos", L"nt.OpenVolume(300,400)" }), "WM_APP+23 (--debug-volume): at the pointer");
    ml::Same(Proc(WM_DEV_NETWORK, 0, 0, &r), V({ L"nt.OpenQuickSettings" }), "WM_APP+24 (--debug-network)");
    Proc(WM_DEV_DEVICE_RESET, 0, 0, &r);
    Check(g_deviceResetRequested && r == 0 && ml::trace.empty(), "WM_APP+25 (--debug-device-reset): a GPU reset requested for the next pass");
    g_deviceResetRequested = false;
    ml::Same(Proc(WM_DEV_SETTINGS, 0, 0, &r), V({ L"sw.Show(desk)", L"sw.PlaceBottomRight" }), "WM_APP+26 (--debug-settings): the settings window on the primary dock's monitor, lower right");
    ml::Same(Proc(WM_DEV_ICONS_HIDE, 0, 0, &r), V({ L"oled.SetDesktopIconsVisible(0)" }), "WM_APP+27 (--debug-icons-hide)");
    ml::Same(Proc(WM_DEV_ICONS_SHOW, 0, 0, &r), V({ L"oled.SetDesktopIconsVisible(1)" }), "WM_APP+28 (--debug-icons-show)");
    {
        const std::wstring tempBefore = ml::tempDir;
        ml::tempDir = tempBefore + L"no such folder\\";
        auto t = Proc(WM_DEV_AUTOSTART_ON, 0, 0, &r);
        Check(g_settings.autostart && !t.empty() && t[0] == L"settings.Save(autostart 1)" && t[1] == L"GetModuleFileNameW(null, 260)" &&
              t.back() == L"Log: autostart 1", "WM_APP+15 (--autostart-on): the setting on and saved, SetAutostart(true), 'autostart 1'");
        t = Proc(WM_DEV_AUTOSTART_OFF, 0, 0, &r);
        Check(!g_settings.autostart && t[0] == L"settings.Save(autostart 0)" && ml::Has(t, L"RegDeleteValueW(run, LiquidDock)") && t.back() == L"Log: autostart 0",
              "WM_APP+16 (--autostart-off): off, saved, SetAutostart(false), 'autostart 0'");
        ml::tempDir = tempBefore;
    }
    {   // the numbers are the contract with a dev build (tools\demo_record.ps1)
        const UINT nums[] = { WM_DEV_SHOW, WM_DEV_HIDE, WM_DEV_SNAP, WM_DEV_PANEL, WM_DEV_ALERT, WM_DEV_AUTOSTART_ON, WM_DEV_AUTOSTART_OFF, WM_DEV_REC_START,
                              WM_DEV_REC_STOP, WM_DEV_BOUNCE, WM_DEV_OVERFLOW, WM_DEV_CALENDAR, WM_DEV_VOLUME, WM_DEV_NETWORK, WM_DEV_DEVICE_RESET,
                              WM_DEV_SETTINGS, WM_DEV_ICONS_HIDE, WM_DEV_ICONS_SHOW, WM_DEV_REC_TIGHT, WM_DEV_REC_FULL };
        const int want[] = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 24, 25, 26, 27, 28, 29, 31 };
        bool same = true;
        for (int i = 0; i < 20; ++i) same &= nums[i] == WM_APP + want[i];
        Check(same && WM_APP_REBUILD == WM_APP + 1 && WM_APP_AUTOSTART == WM_APP + 2 && WM_APP_RELAYOUT == WM_APP + 3 && WM_APP_SETTINGS == WM_APP + 4 &&
              WM_APP_HOTKEY == WM_APP + 5 && WM_APP_TRAYHOST == WM_APP + 6 && WM_APP_ENFORCE == WM_APP + 30 && kRevealHotkeyId == 2,
              "message numbers: WM_APP+1..6 and +30, the developer ones 10-19, 21-29, 31; the reveal hotkey is id 2");
        bool unused = true;
        for (UINT m : { WM_APP + 0, WM_APP + 7, WM_APP + 20, WM_APP + 32 }) {
            ml::trace.clear();
            unused &= MainProc(ml::kMain, m, 0, 0) == 0xDEF;
        }
        Check(unused, "...WM_APP+0, 7, 20 and 32 are nobody's: DefWindowProc");
    }
}

// ---------------------------------------------------------------- one pass of the main loop
static void PassTests() {
    // resting, pointer away from the docks' edge, moving
    Fresh();
    Run(L"", 3, [](int p) { ml::cursor = { 1280 + p + 1, 700 }; });
    const auto restAway = Cat(Cat(PassHead(), DockPart(0)), V({ L"th.Place(0, 0 0 0 0, 0)", L"d0.PanelWanted", L"sensors.SetPanelOpen(0)",
                                                              L"MonitorFromPoint(1281,700,2)", L"GetMonitorInfoW(desk)", L"d0.FullscreenHere", L"MsgWait(45)" }));
    ml::Same(ml::PassTrace(1), restAway, "a pass, resting, the pointer moving away from the edge: messages, GPU, watchdog, modules, the pointer once, each dock (Update, Render, RecordTick, Busy, LiveGlass), the tray host's widget place, the panel, then the pointer's monitor and a 45 ms wait");
    Check(ml::n.cursor == 3 && ml::n.monFromPoint == 3 + 1 && ml::n.monInfo == 3 && ml::n.waits == 3 && ml::n.dwmFlush == 0 && ml::n.peek == 4,
          "...per pass: GetCursorPos 1, MonitorFromPoint 1, GetMonitorInfoW 1, MsgWait 1, PeekMessage 1 (plus BuildDocks' MonitorFromPoint and the last pass's look)");
    Check(ml::updates.size() == 3 && ml::updates[0].dt == 0.0f && std::fabs(ml::updates[1].dt - 0.045f) < 1e-6f && ml::updates[1].c.x == 1281 && !ml::updates[1].unseen,
          "...the first pass's dt 0 (the clock read just before the loop), then the 45 ms waited; the docks get the pointer as read");
    {
        int nows = 0; ml::n.now = 0;
        Fresh(); Run(L"", 5); nows = ml::n.now;
        Fresh(); Run(L"", 6);
        std::printf("  NowSeconds per pass without --profile: %d\n", ml::n.now - nows);
        Check(ml::n.now - nows == 2, "...NowSeconds twice a pass without --profile (now, and the lap mark that is read even then)");
    }

    // near the dock's edge, a fullscreen app there, other edges, still pointer
    struct C { POINT p; Edge edge; bool fullscreen, live; std::vector<ml::Mon> mons; DWORD wait; const char* what; };
    const C cases[] = {
        { { 1280, 1430 }, Edge::Bottom, false, false, { ml::kDesk }, 15, "bottom dock, the pointer in the bottom 120 px: 15 ms" },
        { { 1280, 1320 }, Edge::Bottom, false, false, { ml::kDesk }, 45, "...120 px up: 45 ms" },
        { { 1280, 1430 }, Edge::Bottom, true, false, { ml::kDesk }, 45, "...a fullscreen app on that monitor: 45 ms" },
        { { 1280, 5 }, Edge::Bottom, false, false, { ml::kDesk }, 45, "...at the top edge (browser tabs): 45 ms" },
        { { 2555, 700 }, Edge::Bottom, false, false, { ml::kDesk }, 45, "...at the right edge (a scroll bar): 45 ms" },
        { { 5, 700 }, Edge::Left, false, false, { ml::kDesk }, 15, "left dock, at the left edge: 15 ms" },
        { { 1280, 5 }, Edge::Top, false, false, { ml::kDesk }, 15, "top dock, at the top edge: 15 ms" },
        { { 2555, 700 }, Edge::Right, false, false, { ml::kDesk }, 15, "right dock, at the right edge: 15 ms" },
        { { 1280, 700 }, Edge::Bottom, false, true, { ml::kDesk }, 33, "an always-shown dock's live glass, the pointer away: 33 ms" },
        { { 1280, 1430 }, Edge::Bottom, false, true, { ml::kDesk }, 15, "...near the edge: 15 ms" },
        { { 1440, 1790 }, Edge::Bottom, false, false, { ml::kLaptop }, 15, "a laptop (2880x1800), bottom 120 px: 15 ms" },
        { { 1440, 1680 }, Edge::Bottom, false, false, { ml::kLaptop }, 45, "...at 1680 (120 px up): 45 ms" },
        { { -700, 1990 }, Edge::Bottom, false, false, { ml::kDesk, ml::kPortrait }, 15, "a two-monitor desktop, the portrait monitor's bottom (2000): 15 ms" },
        { { -5, 700 }, Edge::Bottom, false, false, { ml::kDesk, ml::kPortrait }, 45, "...the seam between the monitors: 45 ms" },
    };
    for (const C& c : cases) {
        Fresh(); ml::mons = c.mons; g_settings.edge = c.edge; ml::cursor = c.p;
        for (int i = 0; i < 4; ++i) { ml::dock[i].fullscreen = c.fullscreen; ml::dock[i].liveGlass = c.live; }
        Run(L"", 2);
        const auto t = ml::PassTrace(1);
        wchar_t w[40]; swprintf_s(w, L"MsgWait(%lu)", c.wait);
        const std::string what = std::string("a pass: ") + c.what;
        Check(!t.empty() && t.back() == w, what.c_str());
        if (t.empty() || t.back() != w) std::printf("  got %s\n", t.empty() ? "nothing" : ml::U8(t.back()).c_str());
    }
    {   // a two-monitor desktop: FullscreenHere asked only of the dock on the pointer's monitor
        Fresh(); ml::mons = { ml::kDesk, ml::kPortrait }; ml::cursor = { -700, 1990 };
        Run(L"", 2);
        const auto t = ml::PassTrace(1);
        ml::Same(std::vector<std::wstring>(t.end() - 4, t.end()), V({ L"MonitorFromPoint(-700,1990,2)", L"GetMonitorInfoW(portrait)", L"d1.FullscreenHere", L"MsgWait(15)" }),
                 "two docks: only the pointer's monitor's dock is asked whether a fullscreen app fills it");
        Check(ml::IndexOf(t, L"d1.Update") == ml::IndexOf(t, L"d0.LiveGlass") + 1 && ml::IndexOf(t, L"d1.PanelWanted") == ml::IndexOf(t, L"d0.PanelWanted") + 1,
              "...docks in order, each Update-Render-RecordTick-Busy-LiveGlass; then each one's PanelWanted");
    }

    // the pointer still: 45 ms waits until 2 s, then 100 ms
    Fresh();
    Run(L"", 60);
    {
        int first100 = -1;
        for (int k = 0; k < 60; ++k) { auto t = ml::PassTrace(k); if (!t.empty() && t.back() == L"MsgWait(100)") { first100 = k; break; } }
        std::printf("  a still pointer: the first 100 ms wait at pass %d\n", first100);
        Check(first100 == 45 && ml::n.waitMs[45] == 45, "a still pointer: 45 waits of 45 ms (2.025 s), then 100 ms (kStill 2 s from the first pass's move)");
        Check(ml::updates.size() == 60 && ml::updates[46].dt == 0.05f && std::fabs(ml::updates[45].dt - 0.045f) < 1e-6f,
              "...the docks' dt: the time since the last pass, at most 0.05 s (a 100 ms wait gives 0.05)");
    }

    // busy
    Fresh(); ml::dock[0].busy = true;
    Run(L"", 3);
    ml::Same(ml::PassTrace(1), Cat(Cat(PassHead(), DockPart(0)), V({ L"th.Place(0, 0 0 0 0, 0)", L"d0.PanelWanted", L"sensors.SetPanelOpen(0)", L"DwmFlush" })),
             "a busy pass (a dock animates): DwmFlush instead of the wait - no MonitorFromPoint, no GetMonitorInfoW");
    Check(ml::n.monFromPoint == 1 && ml::n.monInfo == 0 && ml::n.waits == 0 && ml::n.dwmFlush == 3, "...three busy passes: no monitor asked at all (BuildDocks' one only)");
    Fresh(); ml::ffBusy = true;
    Run(L"", 2);
    Check(ml::PassTrace(1).back() == L"DwmFlush" && ml::n.monInfo == 0, "a FluentFlyout flyout moving: busy too");

    // the widget held by a dock, the panel
    Fresh(); ml::dock[0].holdsWidget = true; ml::dock[0].panel = true;
    Run(L"", 2);
    {
        auto t = ml::PassTrace(1);
        Check(ml::CountPrefix(t, L"th.Place") == 0 && ml::Has(t, L"sensors.SetPanelOpen(1)"), "a dock holds FluentFlyout's widget: the tray host not placed by the loop; its panel wanted: sensors every second");
    }
    {   // FluentFlyout starts wanting the host (g_fluentWantedHost starts false): the tray host
        // follows on the first pass that sees it, and again when it stops wanting it; SetHosting every pass
        Fresh(); ml::ffWants = true;
        Run(L"", 3);
        const auto p0 = ml::PassTrace(0), p1 = ml::PassTrace(1);
        const int hosting = ml::IndexOf(p0, L"th.SetHosting(1, widget)");
        Check(hosting > 0 && p0[hosting - 1] == L"ff.WidgetWindow" && p0[hosting - 2] == L"ff.WantsHost" && p0[hosting + 1] == L"ff.WantsHost" &&
              p0[hosting + 2] == L"ExplorerTaskbar" && p0[hosting + 3] == L"th.Running" && p0[hosting + 4] == L"sys.Tick",
              "FluentFlyout wants the host: on the first pass WantsHost, WidgetWindow, SetHosting(on, its widget), then ApplyTrayHost once (already running from BuildDocks)");
        Check(ml::Has(p1, L"th.SetHosting(1, widget)") && ml::CountOf(p1, L"ff.WantsHost") == 1 && !ml::Has(p1, L"ExplorerTaskbar"),
              "...the next passes: SetHosting again, no ApplyTrayHost");
        Fresh(); ml::thRunning = true;   // the pass before wanted it (as the run above left it; Fresh resets it now): FluentFlyout gone
        g_fluentWantedHost = true;
        Run(L"", 2);
        const auto q0 = ml::PassTrace(0);
        const int off = ml::IndexOf(q0, L"th.SetHosting(0)");
        Check(off > 0 && q0[off + 1] == L"ff.WantsHost" && q0[off + 2] == L"th.Stop" && !ml::Has(ml::PassTrace(1), L"th.Stop"),
              "...FluentFlyout no longer wants it: SetHosting(off), then ApplyTrayHost stops the host, once");
    }
    Fresh(); FakeDock::debugForcePanel = true;
    Run(L"", 2);
    Check(ml::Has(ml::PassTrace(1), L"sensors.SetPanelOpen(1)"), "--debug-panel: the panel counts as wanted");
    Fresh(); fakeApps.attentionEvents.push_back({ 5 }); fakeSensors.alertPending = true;
    Run(L"", 1);
    Check(fakeApps.attentionEvents.empty() && !fakeSensors.alertPending, "after the docks: the attention events and the sensor alert consumed");
    // a two-monitor desktop's two docks: what one pass keeps for the next, and what is summed over the docks
    Fresh(); ml::mons = { ml::kDesk, ml::kPortrait };
    Run(L"", 3, [](int p) { if (p == 0) { fakeApps.attentionEvents.push_back({ 5 }); fakeSensors.alertPending = true; } });   // an app flashes, a sensor alert, between passes 0 and 1
    {
        const auto& u = ml::updates;
        Check(u.size() == 6 && u[0].attention == 0 && !u[0].alert && u[1].attention == 0 && !u[1].alert &&
              u[2].index == 0 && u[2].attention == 1 && u[2].alert && u[3].index == 1 && u[3].attention == 1 && u[3].alert,
              "two docks: an attention event and a sensor alert that came before a pass are seen by both docks' Update in that pass");
        Check(u.size() == 6 && u[4].attention == 0 && !u[4].alert && u[5].attention == 0 && !u[5].alert && fakeApps.attentionEvents.empty() && !fakeSensors.alertPending,
              "...and consumed after the docks: the next pass's docks see none");
    }
    Fresh(); ml::mons = { ml::kDesk, ml::kPortrait }; ml::dock[0].holdsWidget = true;
    Run(L"", 4, [](int p) { if (p == 1) ml::dock[0].holdsWidget = false; });   // the first dock stops holding FluentFlyout's widget after pass 1
    {
        const auto t0 = ml::PassTrace(0), t1 = ml::PassTrace(1), t2 = ml::PassTrace(2), t3 = ml::PassTrace(3);
        Check(ml::CountPrefix(t0, L"th.Place") == 0 && ml::CountPrefix(t1, L"th.Place") == 0,
              "two docks, the first holds FluentFlyout's widget: the loop does not place the tray host (the second dock, not holding it, does not undo that)");
        Check(ml::CountPrefix(t2, L"th.Place") == 1 && ml::Has(t2, L"th.Place(0, 1000 1300 1400 1440, 0)") && ml::IndexOf(t2, L"th.Place(0, 1000 1300 1400 1440, 0)") == ml::IndexOf(t2, L"d1.LiveGlass") + 1 &&
              ml::Has(t3, L"th.Place(0, 1000 1300 1400 1440, 0)"),
              "...no dock holds it any more: from the next pass on, each pass places it hidden at the last place a dock gave (Dock::widgetLastRect), after the docks");
    }
    {
        struct S { int which; int kind; const char* what; };   // kind 0 busy, 1 live glass, 2 the panel
        const S ss[] = {
            { 0, 0, "two docks, only the first busy: DwmFlush" }, { 1, 0, "two docks, only the second busy: DwmFlush" },
            { 0, 1, "two docks, only the first with live glass: 33 ms" }, { 1, 1, "two docks, only the second with live glass: 33 ms" },
            { 0, 2, "two docks, only the first shows its panel: sensors every second" }, { 1, 2, "two docks, only the second shows its panel: sensors every second" },
        };
        for (const S& s : ss) {
            Fresh(); ml::mons = { ml::kDesk, ml::kPortrait }; ml::cursor = { 1280, 700 };
            if (s.kind == 0) ml::dock[s.which].busy = true;
            if (s.kind == 1) ml::dock[s.which].liveGlass = true;
            if (s.kind == 2) ml::dock[s.which].panel = true;
            Run(L"", 2);
            const auto t = ml::PassTrace(1);
            const bool ok = !t.empty() && (s.kind == 0 ? t.back() == L"DwmFlush" : s.kind == 1 ? t.back() == L"MsgWait(33)" : ml::Has(t, L"sensors.SetPanelOpen(1)"));
            const std::string what = std::string("a pass: ") + s.what + " (any dock counts)";
            Check(ok, what.c_str());
            if (!ok) std::printf("  got %s\n", t.empty() ? "nothing" : ml::U8(t.back()).c_str());
        }
    }
    Fresh(); ml::mons = { ml::kDesk, ml::kPortrait }; ml::ffBusy = true;
    Run(L"", 2);
    ml::Same(ml::PassTrace(1), Cat(Cat(Cat(PassHead(), DockPart(0)), DockPart(1)), V({ L"th.Place(0, 0 0 0 0, 0)", L"d0.PanelWanted", L"d1.PanelWanted",
                                                                                    L"sensors.SetPanelOpen(0)", L"DwmFlush" })),
             "two docks, a FluentFlyout flyout moving (busy before the docks): each dock still asked Busy and LiveGlass, in order");

    // the pointer unreadable (lock screen, UAC)
    Fresh();
    Run(L"", 6, [](int p) { if (p == 1) ml::cursorReadable = false; if (p == 3) ml::cursorReadable = true; });
    {
        auto t2 = ml::PassTrace(2);
        const int at = ml::IndexOf(t2, L"GetCursorPos");
        Check(at > 0 && t2[at + 1] == L"Log: pointer: unreadable (error 5: the lock screen, a UAC prompt or another desktop has the input) - the docks see none" &&
              t2[at + 2] == L"ff.Busy", "pointer unreadable: the line with GetCursorPos's error right after it (nothing in between)");
        Check(t2.back() == L"MsgWait(100)" && ml::Has(t2, L"MonitorFromPoint(-32000,-32000,2)"), "...a 100 ms wait; the monitor asked for the point off every monitor");
        Check(ml::updates[2].c.x == -32000 && ml::updates[2].c.y == -32000 && ml::updates[2].unseen && ml::updates[3].unseen,
              "...the docks get -32000,-32000 (off every monitor) and screenUnseen");
        Check(ml::PassTrace(3).back() == L"MsgWait(100)" && ml::CountOf(ml::logs, L"pointer: unreadable (error 5: the lock screen, a UAC prompt or another desktop has the input) - the docks see none") == 1,
              "...one line for the whole time");
        auto t4 = ml::PassTrace(4);
        Check(t4[ml::IndexOf(t4, L"GetCursorPos") + 1] == L"Log: pointer: readable again" && !ml::updates[4].unseen && ml::updates[4].c.x == 1280,
              "readable again: 'pointer: readable again', the docks see it, not unseen");
        std::printf("  unlocked at the same point after %.3f s still: wait %s\n", ml::updates[4].now - ml::updates[0].now, ml::U8(t4.back()).c_str());
    }
    {   // unlocked where it was: no move (lastCursor kept through the locked passes)
        Fresh();
        Run(L"", 70, [](int p) { if (p == 46) ml::cursorReadable = false; if (p == 50) ml::cursorReadable = true; });
        auto t = ml::PassTrace(51);
        Check(t.back() == L"MsgWait(100)", "unlocked with the pointer where it was before the lock: still resting (100 ms) - the locked passes did not move it");
        Fresh();
        Run(L"", 70, [](int p) { if (p == 46) ml::cursorReadable = false; if (p == 50) { ml::cursorReadable = true; ml::cursor = { 1300, 700 }; } });
        Check(ml::PassTrace(51).back() == L"MsgWait(45)", "...unlocked somewhere else: a move (45 ms)");
    }
    {   // the display off
        Fresh();
        const int toldBefore = startmenu::displayOnCalls;
        Run(L"", 8, [](int p) {
            if (p == 1) ml::Post(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, DisplayNotice(0));
            if (p == 4) ml::cursor = { 1500, 700 };
        });
        Check(ml::PassTrace(2).back() == L"MsgWait(100)" && ml::updates[2].unseen && ml::PassTrace(3).back() == L"MsgWait(100)",
              "the display off (notification): unseen, 100 ms waits, the pointer read as usual");
        auto t5 = ml::PassTrace(5);
        const int at = ml::IndexOf(t5, L"GetCursorPos");
        Check(at > 0 && t5[at + 1] == L"Log: display: taken as on - the pointer moved, no notification said so" && !ml::updates[5].unseen && t5.back() == L"MsgWait(45)",
              "...the pointer moves: 'display: taken as on - the pointer moved, no notification said so', seen again, 45 ms");
        Check(startmenu::displayOnCalls == toldBefore + 1 && startmenu::displayOnAt == ml::updates[5].now,
              "...and the start button is told the pass's time (startmenu::DisplayCameOn: no clock read more)");
    }
    {   // GetMonitorInfoW failing: rcMonitor stays zero and NearDockEdge runs on it
        Fresh(); ml::monInfoFails = true; ml::cursor = { 1280, 700 };
        Run(L"", 2);
        Check(ml::PassTrace(1).back() == L"MsgWait(15)", "GetMonitorInfoW fails: the zero rectangle makes a bottom dock's edge 'near' for any y > -120 - 15 ms (as it is)");
    }

    // messages in a pass: dispatched first, PreTranslate before Translate
    Fresh();
    Run(L"", 3, [](int p) { if (p == 0) { ml::Post(WM_APP_ENFORCE); ml::Post(WM_APP_REBUILD); } });
    {
        auto t = ml::PassTrace(1);
        ml::Same(std::vector<std::wstring>(t.begin(), t.begin() + 10), V({ L"Peek:0x801e", L"sw.PreTranslate", L"TranslateMessage", L"DispatchMessageW", L"taskbar.Enforce(1, 1)",
                                                                         L"Peek:0x8001", L"sw.PreTranslate", L"TranslateMessage", L"DispatchMessageW", L"Peek" }),
                 "a pass with messages: each one PreTranslate (settings window), Translate, Dispatch, until the queue is empty");
        Check(t[10] == L"d0.~Dock" && t[11] == L"EnumDisplayMonitors(null, null)" && ml::IndexOf(t, L"Log: docks: 1") < ml::IndexOf(t, L"gfx.DeviceLost"),
              "...WM_APP_REBUILD: the docks rebuilt in the same pass, right after the messages and before the GPU check");
    }
    Fresh(); ml::preTranslate = true;
    Run(L"", 2, [](int p) { if (p == 0) ml::Post(WM_APP_ENFORCE); });
    Check(!ml::Has(ml::PassTrace(1), L"taskbar.Enforce(1, 1)") && !ml::Has(ml::PassTrace(1), L"TranslateMessage"), "a message the settings window takes (Esc): not translated or dispatched");
    Fresh();
    Run(L"", 5, [](int p) { if (p == 1) { ml::Post(WM_CLOSE); ml::Post(WM_APP_ENFORCE); } });
    {
        auto t = ml::PassTrace(2);
        Check(ml::Has(t, L"taskbar.Enforce(1, 1)") && !ml::Has(t, L"apps.Tick") && ml::passes == 2 && ml::Has(ml::trace, L"ff.Shutdown"),
              "WM_CLOSE: the queue still emptied (the next message dispatched), then the loop ends before the pass's work");
    }
}

// ---------------------------------------------------------------- the loop over time
static void TimeTests() {
    // the display changes: rebuilt 0.8 s after the last change
    Fresh(); ml::mons = { ml::kDesk };
    double changedAt = 0, rebuiltAt = 0;
    Run(L"", 0, [&](int p) {
        if (p == 3) { ml::mons = { ml::kDesk, ml::kPortrait }; ml::Post(WM_DISPLAYCHANGE); changedAt = ml::Now(); }   // taken at pass 4, at this time
        if (ml::Now() > 4) ml::Quit();
    });
    for (auto& u : ml::updates) if (u.index == 1) { rebuiltAt = u.now; break; }   // the new portrait dock's first pass
    std::printf("  display change: rebuilt %.3f s after the pass that took the message\n", rebuiltAt - changedAt);
    Check(rebuiltAt > 0 && rebuiltAt - changedAt >= 0.8 && rebuiltAt - changedAt < 0.85 && ml::CountOf(ml::logs, L"docks: 2") == 1,
          "WM_DISPLAYCHANGE: the docks made again for the new monitors on the first pass 0.8 s later, once");

    // the GPU lost: recreated; failing, tried again every 2 s with the taskbar back
    Fresh();
    Run(L"", 0, [](int p) { if (p == 2) ml::gfxLost = true; if (ml::Now() > 3) ml::Quit(); });
    {
        const int lost = ml::IndexOf(ml::trace, L"Log: GPU device lost - recreating");
        ml::Same(std::vector<std::wstring>(ml::trace.begin() + lost - 1, ml::trace.begin() + lost + 12),
                 V({ L"gfx.DeviceLost", L"Log: GPU device lost - recreating", L"sw.Close", L"d0.~Dock", L"Dock::ResetNotifyBitmaps", L"gfx.Reset", L"apps.ReloadIcons",
                     L"EnumDisplayMonitors(null, null)", L"MonitorFromPoint(0,0,1)", L"Dock(desk, 0, 1)", L"d0.Create", L"Log: docks: 1", L"WatchdogReady" }),
                 "GPU device lost: logged, the settings window closed, docks and notification bitmaps let go, the device reset, icons reloaded, docks built again");
        Check(ml::n.reset == 1, "...once");
    }
    Fresh();
    Run(L"", 0, [](int p) { if (p == 2) { ml::gfxLost = true; ml::gfxResets = false; } if (ml::Now() > 12) ml::Quit(); });
    {
        std::printf("  GPU reset failing for ~10 s: %d resets, %d taskbar restores, %d DeviceLost asks\n", ml::n.reset, ml::n.restore, ml::n.deviceLost);
        Check(ml::n.reset == 6 && ml::CountOf(ml::logs, L"device re-create failed, retrying") == 6, "GPU reset failing: tried again every 2 s (6 tries in ~10 s), 'device re-create failed, retrying' each time");
        Check(ml::CountOf(ml::trace, L"taskbar.Restore") >= 1 && !g_docksReady, "...the taskbar restored meanwhile, no docks");
        const int firstFail = ml::IndexOf(ml::trace, L"Log: device re-create failed, retrying");
        Check(firstFail > 0 && ml::trace[firstFail + 1] == L"taskbar.Restore" && ml::trace[firstFail + 2] != L"taskbar.Restore" && ml::n.restore == 1 + 1,
              "...right after the first failure: RestoreTaskbar (the taskbar was hidden); it worked, so the loop's retry adds none (one more Restore: the exit's)");
    }
    Fresh();
    Run(L"", 3, [](int p) { if (p == 0) ml::Post(WM_DEV_DEVICE_RESET); });
    {
        auto t = ml::PassTrace(1);
        Check(ml::Has(t, L"Log: GPU device lost - recreating") && !ml::Has(t, L"gfx.DeviceLost"), "--debug-device-reset: the reset path on the next pass without asking the device (short-circuit)");
    }
    Fresh();
    Run(L"", 10, [](int p) { if (p == 0) ml::Post(WM_DEV_DEVICE_RESET); });
    Check(ml::n.reset == 1 && ml::CountOf(ml::logs, L"GPU device lost - recreating") == 1 && ml::CountOf(ml::logs, L"docks: 1") == 2,
          "...over 9 more passes: one reset only (the request cleared), the docks built once more");
    Fresh();   // the reset fails once, then works
    Run(L"", 0, [](int p) { if (p == 2) { ml::gfxLost = true; ml::gfxResets = false; } if (ml::n.reset >= 1) ml::gfxResets = true; if (ml::Now() > 8) ml::Quit(); });
    {
        std::printf("  GPU reset failing once: %d resets, %d hides, last dock pass at %.3f s\n", ml::n.reset, ml::n.hide, ml::updates.empty() ? 0.0 : ml::updates.back().now);
        Check(ml::n.reset == 2 && ml::CountOf(ml::logs, L"GPU device lost - recreating") == 2 && ml::CountOf(ml::logs, L"device re-create failed, retrying") == 1 &&
              ml::CountOf(ml::logs, L"docks: 1") == 2, "GPU reset failing once: tried again 2 s later and working - 2 resets in all, none after (the request cleared), 'docks: 1' once more");
        Check(ml::n.hide == 2 && !ml::updates.empty() && ml::updates.back().now > 7.9, "...the docks back (updated to the end) and the taskbar hidden again");
    }
    Fresh(); ml::gfxLostAfterFailedReset = false;   // as the real Gfx: after a failed Reset there is no device, DeviceLost says false
    Run(L"", 0, [](int p) { if (p == 2) { ml::gfxLost = true; ml::gfxResets = false; } if (ml::Now() > 12) ml::Quit(); });
    {
        std::printf("  GPU reset failing, DeviceLost false after it: %d resets\n", ml::n.reset);
        Check(ml::n.reset == 6 && ml::CountOf(ml::logs, L"device re-create failed, retrying") == 6,
              "GPU reset failing with no device left to ask: still tried every 2 s (the failure requests the next try itself)");
    }

    // the watchdog dies while the dock runs, after a restore that worked and a hide again
    Fresh();
    Run(L"", 8, [](int p) {
        if (p == 1) { ml::watchdogReady = false; ml::Post(WM_APP_REBUILD); }   // pass 2: BuildDocks restores the taskbar (it works)
        if (p == 3) { ml::watchdogReady = true; ml::Post(WM_APP_REBUILD); }    // pass 4: hidden again
        if (p == 5) ml::watchdogReady = false;                                   // pass 6 on: the watchdog gone
    });
    Check(ml::CountOf(ml::PassTrace(2), L"taskbar.Restore") == 1 && ml::CountOf(ml::PassTrace(3), L"taskbar.Restore") == 0 && ml::CountOf(ml::PassTrace(4), L"taskbar.Hide") == 1,
          "the watchdog away, then back: BuildDocks restores the taskbar (worked: the loop does not try again), then hides it again");
    Check(ml::CountOf(ml::PassTrace(6), L"taskbar.Restore") == 1 && ml::CountOf(ml::PassTrace(7), L"taskbar.Restore") == 0 && ml::n.restore == 3,
          "...the watchdog dies with the taskbar hidden: the next pass restores it (the earlier restore that worked does not stop it), once (plus the exit's)");

    // no dock replaces the taskbar (the watchdog gone): Restore tried every second until it works (taskbar_recovery_test's copy loop)
    struct R { double backAt; bool live; int restores; const char* what; };
    const R rs[] = {
        { 0, true, 1, "an hour with the watchdog gone, the restore working: 1 Restore (BuildDocks' own; the loop tries none) - the copy loop counts 1 try too" },
        { 4.5, true, 7, "...Explorer back after 4.5 s: BuildDocks' try plus the loop's at 0, 1, 2, 3, 4, 5 s - 7 (the copy loop counts 6: it has no BuildDocks try)" },
    };
    for (const R& r : rs) {
        Fresh(); ml::watchdogStarts = false; ml::watchdogReady = false; ml::dock[0].liveGlass = r.live; ml::tracing = false;
        const double t0 = ml::Now(), back = r.backAt;
        ml::restoreWorks = [t0, back](int) { return ml::Now() - t0 >= back; };
        Run(L"", 0, [t0](int) { if (ml::Now() - t0 >= 3600) ml::Quit(); });
        std::printf("  watchdog gone, Explorer back at %.1f s: %d Restore in an hour (%d passes)\n", r.backAt, ml::n.restore - 1, ml::passes);
        Check(ml::n.restore - 1 == r.restores, r.what);   // -1: the exit's own Restore
    }

    // an hour nobody sees the screen (idle_wait_test's copy loop: 109,090 looks become 36,000)
    struct H { bool locked, displayOff, live; int passes; const char* what; };
    const H hs[] = {
        { true, false, true, 36000, "an hour locked, an always-shown dock's live glass at rest: 36,000 passes, every one a 100 ms wait (idle_wait_test's sum: 36,000)" },
        { false, true, true, 36001, "...the display off (its notification after the first pass) instead: 36,001 (the first pass's 33 ms, then 36,000 of 100 ms)" },
        { false, false, true, 109091, "...seen, at rest: 109,091 passes of 33 ms (idle_wait_test computes 3,600,000 / 33 = 109,090: the real loop has one more, its first at 0 ms)" },
        { false, false, false, 36025, "an auto-hiding dock at rest, seen, the pointer still: 36,025 (45 passes of 45 ms for the first 2.025 s, then 100 ms)" },
    };
    for (const H& h : hs) {
        Fresh(); ml::dock[0].liveGlass = h.live; ml::tracing = false;
        if (h.locked) ml::cursorReadable = false;
        const long long t0 = ml::nowUs;
        const bool off = h.displayOff;
        Run(L"", 0, [t0, off](int p) {
            if (off && p == 0) ml::Post(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, DisplayNotice(0));
            if (ml::nowUs - t0 >= 3600LL * 1000000) ml::Quit();
        });
        std::printf("  an hour (locked %d, display off %d, live glass %d): %d passes, waits 15:%d 33:%d 45:%d 100:%d, GetCursorPos %d, MonitorFromPoint %d, GetMonitorInfoW %d, DwmFlush %d\n",
                    h.locked, h.displayOff, h.live, ml::passes, ml::n.waitMs[15], ml::n.waitMs[33], ml::n.waitMs[45], ml::n.waitMs[100], ml::n.cursor, ml::n.monFromPoint - 1, ml::n.monInfo, ml::n.dwmFlush);
        Check(ml::passes == h.passes && ml::n.cursor == ml::passes && ml::n.monInfo == ml::passes && ml::n.monFromPoint - 1 == ml::passes && ml::n.dwmFlush == 0, h.what);
    }
    {   // Windows sends the display's state at once when the dock registers for it: off at the start
        Fresh(); ml::dock[0].liveGlass = true;
        ml::Post(WM_POWERBROADCAST, PBT_POWERSETTINGCHANGE, DisplayNotice(0));
        Run(L"", 3);
        ml::Same(ml::logs, V({ L"Windows 11 26H2 (build 26300.1000), Liquid Dock " LD_VERSION_WSTR, L"docks: 1",
                              L"display: off - nobody sees the docks: the loop looks every 100 ms", L"display: taken as on - the pointer moved, no notification said so" }),
                 "the display off when the dock starts: the first pass's first pointer read counts as a move - taken as on at once (as it is)");
        Check(ml::PassTrace(1).back() == L"MsgWait(33)", "...and the loop waits as for a display that is on");
    }
    {   // the counts per pass in those hours: one GetCursorPos, one MonitorFromPoint, one GetMonitorInfoW, one wait
        Fresh(); ml::cursorReadable = false; ml::dock[0].liveGlass = true; ml::tracing = false;
        const long long t0 = ml::nowUs;
        Run(L"", 0, [t0](int) { if (ml::nowUs - t0 >= 3600LL * 1000000) ml::Quit(); });
        Check(ml::n.waitMs.size() == 1 && ml::n.waitMs[100] == 36000 && ml::n.peek == 36001 && ml::n.update == 36000 && ml::n.render == 36000,
              "locked for an hour: only 100 ms waits; PeekMessage, Update and Render once a pass");
        Check(ml::CountOf(ml::logs, L"pointer: unreadable (error 5: the lock screen, a UAC prompt or another desktop has the input) - the docks see none") == 1,
              "...one 'pointer: unreadable' line");
    }
    {   // busy for a second: about 60 DwmFlush, nothing else waits
        Fresh(); ml::dock[0].busy = true; ml::tracing = false;
        const long long t0 = ml::nowUs;
        Run(L"", 0, [t0](int) { if (ml::nowUs - t0 >= 1000000) ml::Quit(); });
        Check(ml::n.dwmFlush == 60 && ml::n.waits == 0 && ml::n.monInfo == 0 && ml::n.cursor == 60, "busy for a second at 60 Hz: 60 passes paced by DwmFlush, the pointer read each, no monitor asked");
    }
}

// ---------------------------------------------------------------- --profile
static void ProfileTests() {
    Fresh();
    ml::appsCostUs = 100; ml::updateCostUs = 1000; ml::renderCostUs = 2000; ml::sysCostUs = 50; ml::sensorsCostUs = 25;
    const long long t0 = ml::nowUs;
    Run(L"--profile", 0, [t0](int p) { if (p == 5) ml::Post(WM_APP_ENFORCE); if (p == 6) ml::Post(WM_APP_ENFORCE); if (ml::nowUs - t0 >= 10500000) ml::Quit(); });
    std::vector<std::wstring> lines;
    for (auto& l : ml::logs) if (l.rfind(L"profile", 0) == 0) lines.push_back(l);
    for (auto& l : lines) std::printf("  %s\n", ml::U8(l).c_str());
    ml::Same(lines, V({ L"profile 10s frames 121 busy 0 | ms/s: msg 0.00 apps 1.20 features 0.00 sys 0.60 sensors 0.30 update 12.01 render 24.03 wait 962.2",
                       L"profile state: threads 2 handles 345 private 50.1 MB cpu-total 3.5 s re-hides 2" }),
             "--profile: after 10 s one line of ms per second by part (each part's time where its lap is) and one of the process's state");
    {
        auto t = ml::PassTrace(3);
        Check(ml::CountOf(t, L"GetProcessHandleCount(self)") == 0, "--profile: the state is gathered only with the 10 s line");
    }
    Fresh();
    Run(L"", 0, [](int) { if (ml::Now() > 13) ml::Quit(); });
    bool none = true;
    for (auto& l : ml::logs) if (l.rfind(L"profile", 0) == 0) none = false;
    Check(none && !ml::Has(ml::trace, L"GetProcessHandleCount(self)"), "without --profile: no profile lines, nothing gathered");
}

// ---------------------------------------------------------------- SetAutostart, ApplyRevealHotkey
static std::string Utf16Bytes(const std::wstring& s) {
    std::string b = "\xFF\xFE";
    for (wchar_t c : s) { b.push_back((char)(c & 0xff)); b.push_back((char)(c >> 8)); }
    return b;
}
static void AutostartTests() {
    const std::wstring xmlWant =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Description>Starts Liquid Dock at logon</Description></RegistrationInfo>\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>DESK&amp;TOP\\User&lt;&amp;&gt;</UserId></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>DESK&amp;TOP\\User&lt;&amp;&gt;</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority><Enabled>true</Enabled></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>\"C:\\Tools &amp; Docks\\LiquidDock\\LiquidDock.exe\"</Command></Exec></Actions>\r\n"
        L"</Task>\r\n";
    Fresh(); g_mainWnd = ml::kMain;
    SetAutostart(true);
    const std::wstring create = L"\"C:\\Windows\\System32\\schtasks.exe\" /Create /F /TN LiquidDock /XML \"" + ml::tempDir + L"LiquidDockLogon.xml\"";
    ml::Same(ml::trace, V({ L"GetModuleFileNameW(null, 260)", L"RegOpenKeyExW(HKCU, Software\\Microsoft\\Windows\\CurrentVersion\\Run, 0, 0x2)",
                           L"RegSetValueExW(run, LiquidDock, 0, 1, [\"C:\\Tools & Docks\\LiquidDock\\LiquidDock.exe\"], 90)", L"RegCloseKey(run)", L"TryDetach",
                           L"GetEnvironmentVariableW(USERNAME, 256)", L"GetEnvironmentVariableW(USERDOMAIN, 256)", L"GetTempPathW(260)",
                           L"CloseHandle(real)", (L"CreateProcessW(null, [" + create + L"], 0, 0x8000000, null, null, si flags 0x1 show 0)").c_str(),
                           L"WaitForSingleObject(schtasks, 15000)", L"GetExitCodeProcess(schtasks)", L"CloseHandle(schtasks)", L"CloseHandle(schtasks thread)",
                           L"DeleteFileW(<temp>\\LiquidDockLogon.xml)", L"Log: logon task rc=0" }),
             "SetAutostart(true): the Run value (the quoted exe, REG_SZ, with its terminator), then on a worker: the user, the XML in %TEMP%, schtasks /Create hidden (15 s), the file deleted, 'logon task rc=0'");
    Check(ml::runValue == L"\"C:\\Tools & Docks\\LiquidDock\\LiquidDock.exe\"" && ml::runValueType == REG_SZ && ml::runValueSize == (ml::runValue.size() + 1) * 2,
          "...the Run value's bytes: the quoted path and its terminator");
    Check(ml::xmlSeen && ml::xmlBytes == Utf16Bytes(xmlWant), "...the task's XML byte for byte: UTF-16 LE with BOM, user and path escaped (&amp; &lt; &gt;), logon trigger, least privilege, no time limit, priority 4");
    if (ml::xmlBytes != Utf16Bytes(xmlWant)) std::printf("  xml: %zu bytes, expected %zu\n", ml::xmlBytes.size(), Utf16Bytes(xmlWant).size());
    Check(GetFileAttributesW((ml::tempDir + L"LiquidDockLogon.xml").c_str()) == INVALID_FILE_ATTRIBUTES, "...the XML file deleted after schtasks");

    Fresh(); g_mainWnd = ml::kMain;
    SetAutostart(false);
    ml::Same(ml::trace, V({ L"GetModuleFileNameW(null, 260)", L"RegOpenKeyExW(HKCU, Software\\Microsoft\\Windows\\CurrentVersion\\Run, 0, 0x2)",
                           L"RegDeleteValueW(run, LiquidDock)", L"RegCloseKey(run)", L"TryDetach",
                           L"CreateProcessW(null, [\"C:\\Windows\\System32\\schtasks.exe\" /Delete /F /TN LiquidDock], 0, 0x8000000, null, null, si flags 0x1 show 0)",
                           L"WaitForSingleObject(schtasks, 15000)", L"GetExitCodeProcess(schtasks)", L"CloseHandle(schtasks)", L"CloseHandle(schtasks thread)" }),
             "SetAutostart(false): the Run value deleted, schtasks /Delete, no dock.log line");

    Fresh(); g_mainWnd = ml::kMain; ml::schtasksCode = 1;
    SetAutostart(true);
    Check(ml::logs.size() == 1 && ml::logs[0] == L"logon task rc=1", "SetAutostart: schtasks fails - its exit code in 'logon task rc=1'");
    Fresh(); g_mainWnd = ml::kMain; ml::schtasksStarts = false;
    SetAutostart(true);
    Check(ml::logs.size() == 1 && ml::logs[0] == L"logon task rc=-1" && !ml::Has(ml::trace, L"WaitForSingleObject(schtasks, 15000)") &&
          ml::Has(ml::trace, L"DeleteFileW(<temp>\\LiquidDockLogon.xml)"), "...schtasks cannot be started: 'logon task rc=-1', the file deleted");
    Fresh(); g_mainWnd = ml::kMain; ml::runKey = false;
    SetAutostart(true);
    Check(!ml::Has(ml::trace, L"RegCloseKey(run)") && ml::Has(ml::logs, L"logon task rc=0"), "SetAutostart: the Run key cannot be opened - the logon task still made");
    {
        Fresh(); g_mainWnd = ml::kMain;
        const std::wstring before = ml::tempDir; ml::tempDir = before + L"no such folder\\";
        SetAutostart(true);
        Check(ml::processes.empty() && ml::logs.empty() && ml::trace.back() == L"GetTempPathW(260)", "SetAutostart: the XML cannot be written - nothing run, no line");
        ml::tempDir = before;
    }
    Fresh(); g_mainWnd = ml::kMain; ml::threadsLeft = false;
    SetAutostart(true);
    Check(ml::logs.size() == 1 && ml::logs[0] == L"logon task: no thread to make it on: the Run key alone" && ml::processes.empty(),
          "SetAutostart: no thread to be had - 'logon task: no thread to make it on: the Run key alone'");
    Fresh(); g_mainWnd = ml::kMain; ml::threadsLeft = false;
    SetAutostart(false);
    Check(ml::logs.size() == 1 && ml::logs[0] == L"logon task: no thread to delete it on: the Run key alone", "...or to delete it on");

    // the reveal shortcut
    struct K { int setting; const wchar_t* reg; const char* what; };
    const K keys[] = {
        { 0, nullptr, "revealHotkey 0: unregistered only" },
        { 1, L"RegisterHotKey(main, 2, 0x4003, 0x44)", "1: Ctrl+Alt+D (MOD_NOREPEAT)" },
        { 2, L"RegisterHotKey(main, 2, 0x4006, 0x20)", "2: Ctrl+Shift+Space" },
        { 3, L"RegisterHotKey(main, 2, 0x400c, 0x44)", "3: Win+Shift+D" },
        { 7, L"RegisterHotKey(main, 2, 0x400c, 0x44)", "7: clamped to 3" },
        { -2, nullptr, "-2: clamped to 0, off" },
    };
    for (const K& k : keys) {
        Fresh(); g_mainWnd = ml::kMain; g_settings.revealHotkey = k.setting;
        ApplyRevealHotkey();
        const std::string what = std::string("ApplyRevealHotkey: ") + k.what;
        ml::Same(ml::trace, k.reg ? V({ L"UnregisterHotKey(main, 2)", k.reg }) : V({ L"UnregisterHotKey(main, 2)" }), what.c_str());
    }
    Fresh(); g_mainWnd = ml::kMain; g_settings.revealHotkey = 2; ml::hotkeyTaken[VK_SPACE] = true;
    ApplyRevealHotkey();
    Check(ml::logs.size() == 1 && ml::logs[0] == L"reveal hotkey 2 unavailable (1409)", "ApplyRevealHotkey: another program owns the combination - 'reveal hotkey 2 unavailable (1409)'");
}

int main() {
    {   // the logon task's XML goes next to this exe (build\tests\main_loop_tmp\), never to %TEMP%
        wchar_t exe[MAX_PATH]; ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe; dir = dir.substr(0, dir.find_last_of(L'\\') + 1) + L"main_loop_tmp\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        ml::tempDir = dir;
    }
    StartTests();
    CommandLineTests();
    BuildDocksTests();
    MainProcTests();
    PassTests();
    TimeTests();
    ProfileTests();
    AutostartTests();
    return Summary(ML_NAME);
}
