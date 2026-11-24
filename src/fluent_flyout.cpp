#include "fluent_flyout.h"
#include "fluent_logic.h"
#include "fluent_settings.h"
#include "tray_host.h"
#include "elevation.h"
#include "explorer_taskbar.h"
#include <shellapi.h>
#include "fluent_internal.h"

#ifndef LD_FLUENT_EXE
#define LD_FLUENT_EXE L"FluentFlyout.exe"
#endif

namespace fluentflyout {
namespace {

std::wstring FileOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// ---------------------------------------------------------------- monitors
// The monitors in FluentFlyout's numbering (fluent_logic.h), read again when the displays change
// (InvalidateMonitors, from the dock's rebuild) and every few seconds in case a change went unnoticed -
// not on every pass of the main loop.
std::vector<HMONITOR> g_monitorHandles;
std::vector<fluentlogic::Monitor> g_monitorDescs;
double g_monitorsAt = -1;
int g_fileMonitor = -1;                     // g_fileSetting as a position among the monitors (UpdateMonitor)

void RefreshMonitors(double now) {
    if (g_monitorsAt >= 0 && now - g_monitorsAt < 5.0) return;
    g_monitorsAt = now;
    struct Found { std::vector<HMONITOR> handles; std::vector<fluentlogic::Monitor> descs; } found;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFO mi = { sizeof(mi) };
        if (!GetMonitorInfoW(h, &mi)) return TRUE;
        auto* f = (Found*)lp;
        f->handles.push_back(h);
        f->descs.push_back({ (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, mi.rcMonitor.left });
        return TRUE;
    }, (LPARAM)&found);
    bool same = found.handles == g_monitorHandles;
    for (size_t i = 0; same && i < found.descs.size(); ++i)
        same = found.descs[i].primary == g_monitorDescs[i].primary && found.descs[i].left == g_monitorDescs[i].left;
    if (same) return;
    g_monitorHandles = std::move(found.handles);
    g_monitorDescs = std::move(found.descs);
    g_choiceDirty = true;
}

int MonitorIndex(HMONITOR m) {
    for (size_t i = 0; i < g_monitorHandles.size(); ++i)
        if (g_monitorHandles[i] == m) return (int)i;
    return -1;
}

}   // namespace

// ---------------------------------------------------------------- FluentFlyout's windows
DWORD g_pid = 0;

namespace {

HANDLE g_proc = nullptr;
HWND g_widget = nullptr;                     // its taskbar widget window ("TaskbarWindow")
HWND g_lostWidget = nullptr;                 // that window closed while FluentFlyout runs: a new one is coming
double g_lostAt = -1;
double g_adoptedAt = -1;                     // when the current widget window was found
double g_nextScan = 0;
HMONITOR g_monitor = nullptr;                // the monitor its widget goes to
bool g_decidedOpen = false;                  // the slot's state as WidgetState last decided it (not while the first list is awaited)
bool g_slotShown = false;                    // what WidgetState last answered (closed while the dock takes no widget)
RECT g_lastBox{};                            // what it showed while its slot was open
enum class ParentKind { None, Host, ExplorerMain, ExplorerSecondary, Other };
HWND g_parent = (HWND)-1;                    // where its widget window is (looked at when that changes)
ParentKind g_parentKind = ParentKind::None;
bool g_parentTells = false;                  // ...one of Explorer's taskbars that tells FluentFlyout's monitor
HMONITOR g_shownMonitor = nullptr;           // the monitor of the Explorer taskbar FluentFlyout last put it in
double g_shownAt = -1;
double g_notInHostSince = -1;                // the host waits for the widget since then
bool g_notInHostLogged = false;

bool IsFluentProcess(DWORD pid) {
    static DWORD lastPid = 0;
    static bool lastResult = false;
    if (pid == lastPid) return lastResult;
    lastPid = pid;
    lastResult = false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    lastResult = !_wcsicmp(FileOf(ImagePath(p)).c_str(), LD_FLUENT_EXE);
    CloseHandle(p);
    return lastResult;
}

// The widget window: a WPF window ("HwndWrapper[...]") titled "TaskbarWindow" of FluentFlyout's
// process. The class is checked first: it is the cheapest test and rules out nearly every window.
bool IsWidgetWindow(HWND h) {
    wchar_t cls[64] = {}, title[32] = {};
    if (!GetClassNameW(h, cls, 64) || wcsncmp(cls, L"HwndWrapper[", 12)) return false;
    if (!GetWindowTextW(h, title, 32) || wcscmp(title, L"TaskbarWindow")) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid && IsFluentProcess(pid);
}

HWND WidgetChild(HWND parent) {
    if (!parent) return nullptr;
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        if (IsWidgetWindow(c)) return c;
    return nullptr;
}

// Where FluentFlyout puts it: in the first "Shell_TrayWnd" (the tray host's window while it runs),
// a secondary taskbar, or still a window of its own while it starts.
HWND FindWidget() {
    if (HWND w = WidgetChild(trayhost::Window())) return w;
    if (HWND w = WidgetChild(ExplorerTaskbar())) return w;
    for (HWND s = nullptr; (s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        if (HWND w = WidgetChild(s)) return w;
    // a window of its own: only the top-level windows titled "TaskbarWindow" are looked at (FindWindowExW
    // compares titles in one call; this runs every 2 s while the widget is not found, and asking every
    // top-level window for its class cost a call into the system each)
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, nullptr, L"TaskbarWindow")) != nullptr;)
        if (IsWidgetWindow(h)) return h;
    return nullptr;
}

ParentKind Classify(HWND parent) {
    if (!parent || parent == GetDesktopWindow()) return ParentKind::None;
    if (parent == trayhost::Window()) return ParentKind::Host;
    wchar_t cls[64] = {};
    GetClassNameW(parent, cls, 64);
    if (!wcscmp(cls, L"Shell_SecondaryTrayWnd")) return ParentKind::ExplorerSecondary;
    if (!wcscmp(cls, L"Shell_TrayWnd") && !GetPropW(parent, kTrayHostProp)) return ParentKind::ExplorerMain;
    return ParentKind::Other;
}

// Where FluentFlyout's widget window is, for the log.
std::wstring ParentName(HWND parent) {
    if (!parent || parent == GetDesktopWindow()) return L"a window of its own (not moved into a taskbar)";
    if (parent == trayhost::Window()) return L"the dock's window";
    wchar_t cls[64] = {};
    GetClassNameW(parent, cls, 64);
    if (!wcscmp(cls, L"Shell_TrayWnd")) return L"Explorer's taskbar (hidden by the dock)";
    if (!wcscmp(cls, L"Shell_SecondaryTrayWnd")) return L"a secondary Explorer taskbar (hidden by the dock)";
    return std::wstring(L"a window of class ") + cls;
}

std::wstring MonitorText(HMONITOR m) {
    MONITORINFO mi = { sizeof(mi) };
    if (!m || !GetMonitorInfoW(m, &mi)) return L"(no monitor)";
    wchar_t t[96];
    int number = fluentlogic::SettingNumber(g_monitorDescs, MonitorIndex(m));
    swprintf_s(t, L"%s monitor at %ld,%ld (FluentFlyout's \"%d\")", (mi.dwFlags & MONITORINFOF_PRIMARY) ? L"main" : L"other",
               mi.rcMonitor.left, mi.rcMonitor.top, number);
    return t;
}

// Where its widget window is. In one of Explorer's taskbars it tells which monitor FluentFlyout is set
// to (fluentlogic::WantedMonitor) - unless its fallback may have put it there (fluentlogic::ShowsMonitor).
// And if FluentFlyout has not moved it into the dock's window after a few seconds: say where it is and
// the likely reason.
void WatchWidgetParent(double now, bool hosting) {
    HWND parent = GetAncestor(g_widget, GA_PARENT);
    const bool moved = parent != g_parent;
    if (moved) {
        g_parent = parent;
        g_parentKind = Classify(parent);
        Log(L"fluentflyout: widget window is in %s", ParentName(parent).c_str());
    }
    // looked at when it moves, or when the monitors or its setting changed (UpdateMonitor clears that next)
    if (moved || g_choiceDirty) {
        const bool told = g_parentTells;
        g_parentTells = false;
        if (g_parentKind == ParentKind::ExplorerMain || g_parentKind == ParentKind::ExplorerSecondary) {
            int secondaries = 0;
            for (HWND s = nullptr; (s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr && secondaries < 16;) ++secondaries;
            g_parentTells = fluentlogic::ShowsMonitor(g_parentKind == ParentKind::ExplorerSecondary, secondaries, (int)g_monitorHandles.size());
            if (!g_parentTells && (moved || told))
                Log(L"fluentflyout: Explorer has no taskbar on every display (%d secondary, %d monitors): FluentFlyout puts its widget into the main one when its monitor has none - its settings decide",
                    secondaries, (int)g_monitorHandles.size());
        }
    }
    if ((g_parentKind == ParentKind::ExplorerMain || g_parentKind == ParentKind::ExplorerSecondary) && g_parentTells) {
        HMONITOR m = MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST);
        if (m != g_shownMonitor)
            Log(L"fluentflyout: FluentFlyout took that taskbar: its widget belongs on the %s", MonitorText(m).c_str());
        g_shownMonitor = m;
        g_shownAt = now;
    }
    bool inHost = g_parentKind == ParentKind::Host;
    if (!hosting || inHost) { g_notInHostSince = -1; g_notInHostLogged = false; return; }
    if (g_notInHostSince < 0) g_notInHostSince = now;
    if (g_notInHostLogged || now - g_notInHostSince < 6) return;
    g_notInHostLogged = true;
    Log(L"fluentflyout: FluentFlyout has not moved its widget into the dock's window (it is in %s)%s", ParentName(parent).c_str(),
        ProcessElevated() ? L" - the dock runs as administrator, and Windows does not let an ordinary program put a window into an elevated one: start Liquid Dock normally"
                          : L" - is FluentFlyout's taskbar widget on, with Premium?");
}

// The dock whose slot holds the widget (fluentlogic::WantedMonitor).
void UpdateMonitor() {
    if (g_choiceDirty) {
        g_choiceDirty = false;
        g_fileMonitor = g_fileSetting >= 0 ? fluentlogic::MonitorForSetting(g_monitorDescs, g_fileSetting) : -1;
    }
    fluentlogic::Choice c;
    c.fileMonitor = g_fileMonitor;
    c.fileAt = g_fileAt;
    c.shownMonitor = MonitorIndex(g_shownMonitor);
    c.shownAt = g_shownAt;
    fluentlogic::Reason why = fluentlogic::Reason::Default;
    int i = fluentlogic::WantedMonitor(c, fluentlogic::PrimaryOf(g_monitorDescs), &why);
    HMONITOR m = i >= 0 && i < (int)g_monitorHandles.size() ? g_monitorHandles[i] : nullptr;
    if (m == g_monitor) return;
    g_monitor = m;
    wchar_t setting[48] = L"";
    if (g_fileSetting >= 0) swprintf_s(setting, L", its setting \"%d\"", g_fileSetting + 1);
    Log(L"fluentflyout: its widget goes to the dock on the %s - %s%s", MonitorText(m).c_str(),
        why == fluentlogic::Reason::Shown ? L"where FluentFlyout put it" : why == fluentlogic::Reason::File ? L"from its settings" : L"not known yet",
        setting);
}

void ForgetProcess() {
    g_pid = 0;
    if (g_proc) { CloseHandle(g_proc); g_proc = nullptr; }
    Unhook();
    ForgetFlyouts();
}

void Forget(const wchar_t* why) {
    if (g_widget || g_lostWidget) Log(L"fluentflyout: %s", why);
    g_parent = (HWND)-1;
    g_parentKind = ParentKind::None;
    g_parentTells = false;
    g_notInHostSince = -1;
    g_notInHostLogged = false;
    g_monitor = nullptr;
    g_widget = g_lostWidget = nullptr;
    g_lostAt = -1;
    g_shownMonitor = nullptr;
    g_shownAt = -1;
    g_decidedOpen = g_slotShown = false;
    g_lastBox = {};
    g_namesWanted = false;   // no apps named for a filter nothing uses
    WantMedia(false);        // the media thread sleeps
    ForgetProcess();
}

void Adopt(HWND w, double now) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (g_lostWidget && pid == g_pid) {   // the same FluentFlyout made its widget window again
        Log(L"fluentflyout: its new widget window is here after %.1f s", now - g_lostAt);
        g_widget = w;
        g_lostWidget = nullptr;
        g_lostAt = -1;
        g_adoptedAt = now;
        return;
    }
    if (g_pid) ForgetProcess();   // another FluentFlyout: what the last one showed no longer counts
    g_shownMonitor = nullptr;
    g_shownAt = -1;
    g_lostWidget = nullptr;
    g_lostAt = -1;
    g_widget = w;
    g_adoptedAt = now;
    g_pid = pid;
    g_proc = OpenProcess(SYNCHRONIZE, FALSE, g_pid);
    Log(L"fluentflyout: running (process %lu)", g_pid);
    Hook();
    LoadSettings(now, true);
    g_namesWanted = g_ff.loaded && g_ff.filtering;   // LoadSettings returns early for a file it has read
}

}   // namespace

// ---------------------------------------------------------------- module

void InvalidateMonitors() { g_monitorsAt = -1; }

void Tick(double now) {
    if (!g_settings.fluentFlyout) {
        if (g_widget || g_lostWidget) Forget(L"integration switched off");
        return;
    }
    if (g_widget || g_lostWidget) {
        if (g_proc && WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) {
            Forget(L"no longer running");
            g_nextScan = now + 2.0;
        } else if (g_widget && !IsWindow(g_widget)) {
            // FluentFlyout closes its widget window and makes a new one when the displays or the work area
            // change: the slot keeps its place meanwhile (fluentlogic::kRehomeSeconds)
            Log(L"fluentflyout: its widget window closed - keeping its place for the new one");
            g_lostWidget = g_widget;
            g_lostAt = now;
            g_widget = nullptr;
            g_parent = (HWND)-1;
            g_parentKind = ParentKind::None;
            g_nextScan = now;
        }
    }
    if (!g_widget && now >= g_nextScan) {
        const bool waiting = g_lostWidget != nullptr;
        g_nextScan = now + (waiting ? 0.1 : 2.0);
        HWND w = FindWidget();
        if (w && w != g_lostWidget) Adopt(w, now);
        else if (waiting && !fluentlogic::StillWaiting(g_lostAt, now)) {
            Forget(L"its widget window did not come back");
            g_nextScan = now + 0.5;
        }
    }
    if (!g_widget && !g_lostWidget) return;
    RefreshMonitors(now);
    LoadSettings(now, false);
    WantMedia(WantsHost() && g_ff.loaded && g_ff.hideCompletely);   // only then does the slot follow the sessions (WidgetState)
    if (g_mediaWanted) UpdateHasMedia(now);
    if (g_widget) WatchWidgetParent(now, WantsHost() && trayhost::Window() != nullptr);
    UpdateMonitor();
    PruneFlyouts();
}

bool Present() { return g_settings.fluentFlyout && (g_widget != nullptr || g_lostWidget != nullptr); }

// While a new widget window is awaited the closed one is named: nothing in the host window matches it,
// so nothing shows there until the new one is found.
HWND WidgetWindow() { return Present() ? (g_widget ? g_widget : g_lostWidget) : nullptr; }

bool WantsHost() {
    return Present() && !IsVertical(g_settings.edge) && (!g_ff.loaded || g_ff.widgetEnabled);
}

Widget WidgetState() {
    Widget w;
    if (!WantsHost()) { g_slotShown = false; return w; }
    w.monitor = g_monitor;
    // Its widget window is being made again, or the new one has not reached the host window yet:
    // the slot keeps its place and size for that moment.
    auto keep = [&] { w.open = g_slotShown = g_decidedOpen; w.box = g_lastBox; w.fitted = true; return w; };
    if (!g_widget) return keep();
    auto hosted = trayhost::HostedState();
    if (hosted.window != g_widget && g_decidedOpen && NowSeconds() - g_adoptedAt < 1.6) return keep();
    w.box = hosted.box;
    w.fitted = hosted.fitted;
    const bool there = hosted.window == g_widget && !IsRectEmpty(&hosted.box);
    if (there && g_ff.loaded && g_ff.hideCompletely && FirstListPending(NowSeconds())) {
        w.open = g_slotShown;   // as it is until the sessions are known: nothing opened or closed, nothing written
    } else {
        bool content = !g_ff.loaded || !g_ff.hideCompletely || g_media.value;
        w.open = there && content;
        if (w.open != g_decidedOpen) {
            g_decidedOpen = w.open;
            Log(L"fluentflyout: widget slot %s (%s)", w.open ? L"open" : L"closed",
                w.open ? L"showing" : hosted.window != g_widget ? L"widget hidden or not in the dock's window"
                       : !content ? L"nothing playing, hide completely" : L"empty");
        }
    }
    if (w.open) g_lastBox = hosted.box;
    g_slotShown = w.open;
    return w;
}

int HostWidth(float scale) {
    // Room for FluentFlyout's widget at its widest (about 310 px with playback buttons) next to the
    // visualizer (84 px) and its own 20 px margin, whichever end of "the taskbar" it is set to, plus
    // its manual padding. Not much wider, so the window stays on the dock's monitor when FluentFlyout
    // asks which monitor its taskbar is on.
    return (int)std::lround(480 * scale) + std::abs(g_ff.manualPadding);
}

HMONITOR WidgetMonitor() { return Present() ? g_monitor : nullptr; }

int WidgetMonitorNumber() {
    if (!Present() || !g_monitor) return 0;
    return fluentlogic::SettingNumber(g_monitorDescs, MonitorIndex(g_monitor));
}

// FluentFlyout opens its settings window when its named event "FluentFlyout_OpenSettings" is set - a
// second copy of it does just that and leaves. The event first; starting it again otherwise.
constexpr wchar_t kSettingsEvent[] = L"FluentFlyout_OpenSettings";
#ifndef LD_FLUENT_SETTINGS_EVENT
#define LD_FLUENT_SETTINGS_EVENT kSettingsEvent   // tests name an event of their own: a running FluentFlyout waits on this one
#endif

bool OpenSettings() {
    if (!g_pid) return false;
    AllowSetForegroundWindow(g_pid);   // its settings window may come to the front
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, LD_FLUENT_SETTINGS_EVENT)) {
        BOOL ok = SetEvent(e);
        CloseHandle(e);
        if (ok) { Log(L"fluentflyout: asked FluentFlyout to open its settings"); return true; }
    }
    std::wstring target;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_pid)) {
        // the Store version is started by its app id; its exe in WindowsApps cannot be run directly
        using GetAumid = LONG(WINAPI*)(HANDLE, UINT32*, PWSTR);
        auto getAumid = (GetAumid)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetApplicationUserModelId");
        wchar_t aumid[256] = {};
        UINT32 len = ARRAYSIZE(aumid);
        if (getAumid && getAumid(p, &len, aumid) == ERROR_SUCCESS && aumid[0]) target = std::wstring(L"shell:AppsFolder\\") + aumid;
        else target = ImagePath(p);
        CloseHandle(p);
    }
    if (target.empty()) return false;
    bool ok = (INT_PTR)ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
    Log(L"fluentflyout: %s %s to open its settings", ok ? L"started" : L"could not start", target.c_str());
    return ok;
}

void Shutdown() {
    Forget(L"shutting down");
    StopMedia();
}

}   // namespace fluentflyout
