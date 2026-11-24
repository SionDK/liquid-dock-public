#include "common.h"
#include "gfx.h"
#include "dock.h"
#include "apps.h"
#include "system.h"
#include "capture.h"
#include "sensors.h"
#include "admin_helper.h"
#include "native_tray.h"
#include "elevation.h"
#include <dwmapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "oled.h"
#include "buttons.h"
#include "settings_window.h"
#include "tray_host.h"
#include "explorer_taskbar.h"
#include "taskbar_visibility.h"
#include "idle_wait.h"
#include "start_menu.h"
#include "version.h"
#include "main_args.h"
#include "main_internal.h"

Settings g_settings;
HWND g_mainWnd = nullptr;
// The main window's class: other runs of the exe find the running dock by it, and so do Setup
// (installer\setup_main.cpp) and the diagnostic tests. A macro: the same literal at each use.
#define LD_MAIN_CLASS L"LiquidDockMain"

static std::vector<std::unique_ptr<Dock>> g_docks;
static UINT g_msgShellHook = 0, g_msgTaskbarCreated = 0;
static bool g_running = true;
static int g_enforceCount = 0;   // taskbar re-hides done on the UI thread (--profile)
static double g_rebuildAt = 0;
static bool g_deviceResetRequested = false;
static double g_nextDeviceRetry = 0;
static bool g_docksReady = false;
static taskbarvisibility::RestoreRetry g_restoreRetry;   // the taskbar back while no dock replaces it
// The session's display (GUID_SESSION_DISPLAY_STATUS, power setting notifications): off, nobody sees the docks
// and the main loop looks less often (idle_wait.h: unseen) until an "on" notification or a pointer that moves
static constexpr GUID kSessionDisplayStatus = { 0x2b84c20e, 0xad23, 0x4ddf, { 0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5 } };
static idlewait::PointerLook g_look;   // the main loop's look at the pointer from pass to pass, and displayOff (MainProc)

// While the dock runs, its restores go through here or the main loop's g_restoreRetry.TryWhenDue (the exits and the
// failed starts call g_taskbar.Restore() directly): the main loop tries again (every second) only after one failed
static void RestoreTaskbar() { g_restoreRetry.Tried(g_taskbar.Restore()); }

// The tray host runs while the docks replace the taskbar and either shows the tray icons on the dock
// (experimental) or holds FluentFlyout's widget for it. It waits for Explorer's taskbar (early logon):
// Explorer announces it with TaskbarCreated. Only for the icons are programs asked to re-add theirs.
static void ApplyTrayHost() {
    const bool icons = g_settings.trayIcons, widget = fluentflyout::WantsHost();
    if ((icons || widget) && g_running && g_docksReady && ExplorerTaskbar()) {
        if (!trayhost::Running()) { if (!trayhost::Start(icons)) Log(L"tray host: could not start"); }
        else if (icons) trayhost::Announce();
    } else trayhost::Stop();
}

// Every pass of the main loop: FluentFlyout started, stopped, or its widget was switched - the tray host follows
static bool g_fluentWantedHost = false;   // FluentFlyout wanted the tray host at the pass before
static void FollowFluentFlyout() {
    bool wants = fluentflyout::WantsHost();
    trayhost::SetHosting(wants, fluentflyout::WidgetWindow());
    if (wants != g_fluentWantedHost) { g_fluentWantedHost = wants; ApplyTrayHost(); }
}

static void BuildDocks() {
    g_docksReady = false;
    g_docks.clear();
    std::vector<HMONITOR> mons;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        ((std::vector<HMONITOR>*)lp)->push_back(m); return TRUE;
    }, (LPARAM)&mons);
    HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    int i = 0;
    for (HMONITOR m : mons) {
        auto d = std::make_unique<Dock>(m, i++, m == primary);
        if (d->Create()) g_docks.push_back(std::move(d));
        else Log(L"dock create failed for monitor %d", i - 1);
    }
    Log(L"docks: %d", (int)g_docks.size());
    if (Dock::carryRecording)   // a recording in progress continues on the new primary dock
        for (auto& d : g_docks) if (d->primary) d->StartRecording(Dock::carryCrop, Dock::carryScale, true);
    // A replacement must exist on every monitor before hiding all native taskbars.
    g_docksReady = !mons.empty() && g_docks.size() == mons.size();
    if (!g_docksReady) g_docks.clear();
    if (g_docksReady && WatchdogReady()) { if (!g_taskbar.hidden) g_taskbar.Hide(); }
    else RestoreTaskbar();
    oled::InvalidateCovers();   // monitors changed: the OLED covers follow on the next pass
    fluentflyout::InvalidateMonitors();   // ...and the dock that holds FluentFlyout's widget
    ApplyTrayHost();
}

// The optional shortcut that shows the dock (settings: revealHotkey).
static constexpr int kRevealHotkeyId = 2;
static void ApplyRevealHotkey() {
    UnregisterHotKey(g_mainWnd, kRevealHotkeyId);
    struct Combo { UINT mods, vk; } combos[] = { {}, { MOD_CONTROL | MOD_ALT, 'D' }, { MOD_CONTROL | MOD_SHIFT, VK_SPACE }, { MOD_WIN | MOD_SHIFT, 'D' } };
    int i = std::clamp(g_settings.revealHotkey, 0, 3);
    if (i == 0) return;
    if (!RegisterHotKey(g_mainWnd, kRevealHotkeyId, combos[i].mods | MOD_NOREPEAT, combos[i].vk))
        Log(L"reveal hotkey %d unavailable (%lu)", i, GetLastError());   // another program owns that combination
}

// The developer commands ("LiquidDock.exe --debug-show" and the rest: the table kDevFlags below, LD_DEV builds)
// post these to the running dock; MainProc handles them in every build. The numbers are the contract
// between a developer build and the dock that runs (tools\demo_record.ps1).
static constexpr UINT WM_DEV_SHOW = WM_APP + 10;          // --debug-show
static constexpr UINT WM_DEV_HIDE = WM_APP + 11;          // --debug-hide
static constexpr UINT WM_DEV_SNAP = WM_APP + 12;          // --snap
static constexpr UINT WM_DEV_PANEL = WM_APP + 13;         // --debug-panel
static constexpr UINT WM_DEV_ALERT = WM_APP + 14;         // --debug-alert
static constexpr UINT WM_DEV_AUTOSTART_ON = WM_APP + 15;  // --autostart-on
static constexpr UINT WM_DEV_AUTOSTART_OFF = WM_APP + 16; // --autostart-off
static constexpr UINT WM_DEV_REC_START = WM_APP + 17;     // --rec-start
static constexpr UINT WM_DEV_REC_STOP = WM_APP + 18;      // --rec-stop
static constexpr UINT WM_DEV_BOUNCE = WM_APP + 19;        // --debug-bounce
static constexpr UINT WM_DEV_OVERFLOW = WM_APP + 21;      // --debug-overflow
static constexpr UINT WM_DEV_CALENDAR = WM_APP + 22;      // --debug-calendar
static constexpr UINT WM_DEV_VOLUME = WM_APP + 23;        // --debug-volume
static constexpr UINT WM_DEV_NETWORK = WM_APP + 24;       // --debug-network
static constexpr UINT WM_DEV_DEVICE_RESET = WM_APP + 25;  // --debug-device-reset
static constexpr UINT WM_DEV_SETTINGS = WM_APP + 26;      // --debug-settings
static constexpr UINT WM_DEV_ICONS_HIDE = WM_APP + 27;    // --debug-icons-hide
static constexpr UINT WM_DEV_ICONS_SHOW = WM_APP + 28;    // --debug-icons-show
static constexpr UINT WM_DEV_REC_TIGHT = WM_APP + 29;     // --rec-tight
static constexpr UINT WM_DEV_REC_FULL = WM_APP + 31;      // --rec-full
#ifdef LD_DEV
// developer controls for a running instance (build.bat dev): the flag and the message posted for it; the first entry
// whose flag is on the command line counts (mainargs::FirstFlag: this table's order)
namespace { struct DevFlag { const wchar_t* flag; UINT msg; }; }
static constexpr DevFlag kDevFlags[] = { { L"--debug-show", WM_DEV_SHOW }, { L"--debug-hide", WM_DEV_HIDE }, { L"--snap", WM_DEV_SNAP },
                                         { L"--debug-panel", WM_DEV_PANEL }, { L"--debug-alert", WM_DEV_ALERT },
                                         { L"--autostart-on", WM_DEV_AUTOSTART_ON }, { L"--autostart-off", WM_DEV_AUTOSTART_OFF },
                                         { L"--rec-start", WM_DEV_REC_START }, { L"--rec-stop", WM_DEV_REC_STOP }, { L"--debug-bounce", WM_DEV_BOUNCE },
                                         { L"--debug-overflow", WM_DEV_OVERFLOW }, { L"--debug-calendar", WM_DEV_CALENDAR },
                                         { L"--debug-volume", WM_DEV_VOLUME }, { L"--debug-network", WM_DEV_NETWORK },
                                         { L"--debug-device-reset", WM_DEV_DEVICE_RESET }, { L"--debug-settings", WM_DEV_SETTINGS },
                                         { L"--debug-icons-hide", WM_DEV_ICONS_HIDE }, { L"--debug-icons-show", WM_DEV_ICONS_SHOW },
                                         { L"--rec-tight", WM_DEV_REC_TIGHT }, { L"--rec-full", WM_DEV_REC_FULL } };
#endif

// The developer recordings (--rec-start, --rec-tight, --rec-full): the primary monitor's dock records the part
// crop(w, hgt) of its monitor (w x hgt px) at outScale
static void RecordOnPrimary(RECT (*crop)(int w, int hgt), float outScale) {
    for (auto& d : g_docks) if (d->primary) {
        MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(d->monitor, &mi);
        int w = mi.rcMonitor.right - mi.rcMonitor.left, hgt = mi.rcMonitor.bottom - mi.rcMonitor.top;
        d->StartRecording(crop(w, hgt), outScale);
    }
}

// A developer command (WM_DEV_*, every build: a dev build's command line posts it, tools\demo_record.ps1) - false: m is
// none of them. MainProc returns 0 for each.
static bool HandleDevMessage(UINT m) {
    switch (m) {
    case WM_DEV_SHOW: Dock::debugForceShow = true; return true;
    case WM_DEV_HIDE: Dock::debugForceShow = false; Dock::debugForcePanel = false; return true;
    case WM_DEV_PANEL: Dock::debugForceShow = true; Dock::debugForcePanel = true; return true;
    case WM_DEV_ALERT: g_sensors.alertPending = true; return true;
    case WM_DEV_REC_START:   // record the half of the primary monitor next to the dock
        RecordOnPrimary([](int w, int hgt) {
            RECT crop = { 0, 0, w, hgt };
            switch (g_settings.edge) {
            case Edge::Left:   crop = { 0, 0, w / 2, hgt }; break;
            case Edge::Right:  crop = { w / 2, 0, w, hgt }; break;
            case Edge::Bottom: crop = { 0, hgt / 2, w, hgt }; break;
            case Edge::Top:    crop = { 0, 0, w, hgt / 2 }; break;
            }
            return crop;
        }, 0.5f);
        return true;
    case WM_DEV_REC_TIGHT:   // record a close-up: 2200 x 760 px around the middle of the edge, at 0.875
        RecordOnPrimary([](int w, int hgt) {
            RECT crop = { w / 2 - 1100, hgt - 760, w / 2 + 1100, hgt };
            switch (g_settings.edge) {
            case Edge::Top:   crop = { w / 2 - 1100, 0, w / 2 + 1100, 760 }; break;
            case Edge::Left:  crop = { 0, hgt / 2 - 1100, 760, hgt / 2 + 1100 }; break;
            case Edge::Right: crop = { w - 760, hgt / 2 - 1100, w, hgt / 2 + 1100 }; break;
            default: break;
            }
            return crop;
        }, 0.875f);
        return true;
    case WM_DEV_REC_FULL:   // record the whole primary monitor at half size
        RecordOnPrimary([](int w, int hgt) { return RECT{ 0, 0, w, hgt }; }, 0.5f);
        return true;
    case WM_DEV_REC_STOP: for (auto& d : g_docks) d->StopRecording(); return true;
    case WM_DEV_BOUNCE: {   // simulate a KakaoTalk notification
        std::shared_ptr<AppItem> target;
        for (auto& a : g_apps.items) if (a->exe.find(L"kakaotalk") != std::wstring::npos) { target = a; break; }
        if (!target) for (auto& a : g_apps.items) if (a->Running()) { target = a; break; }
        if (target) { target->attention = true; target->lastAttentionBounce = NowSeconds(); g_apps.attentionEvents.push_back({ target->id }); }
        return true;
    }
    case WM_DEV_OVERFLOW: {   // developer: open the native hidden-icons window as the dock's chevron does
        POINT p{}; GetCursorPos(&p);
        RECT r = { p.x, p.y, p.x + 1, p.y + 1 };
        Log(L"debug overflow: %d", (int)nativetray::OpenOverflow(p, g_settings.edge, r));
        return true;
    }
    case WM_DEV_CALENDAR: nativetray::OpenCalendar(); return true;
    case WM_DEV_VOLUME: { POINT p{}; GetCursorPos(&p); nativetray::OpenVolume(p); return true; }
    case WM_DEV_NETWORK: nativetray::OpenQuickSettings(); return true;
    case WM_DEV_DEVICE_RESET: g_deviceResetRequested = true; return true;   // developer: simulate a GPU reset
    case WM_DEV_ICONS_HIDE: case WM_DEV_ICONS_SHOW: oled::SetDesktopIconsVisible(m == WM_DEV_ICONS_SHOW); return true;   // developer (recordings)
    case WM_DEV_SETTINGS:   // developer (recordings): settings window in the lower right corner of the primary monitor
        for (auto& d : g_docks) if (d->primary) { settingswindow::Show(d->monitor); settingswindow::PlaceBottomRight(); }
        return true;
    case WM_DEV_AUTOSTART_ON: case WM_DEV_AUTOSTART_OFF:
        g_settings.autostart = (m == WM_DEV_AUTOSTART_ON);
        g_settings.Save();
        SetAutostart(g_settings.autostart);
        Log(L"autostart %d", (int)g_settings.autostart);
        return true;
    case WM_DEV_SNAP: for (auto& d : g_docks) d->RequestSnapshot(); return true;
    }
    return false;
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_msgShellHook && g_msgShellHook) { g_apps.OnShellHook(w, l); return 0; }
    if (m == g_msgTaskbarCreated && g_msgTaskbarCreated) {
        // A restored taskbar must stay available while the dock is unavailable or exiting.
        if (g_running && g_docksReady && WatchdogReady()) g_taskbar.Hide();
        else RestoreTaskbar();
        oled::Reapply();   // a new Explorer shows the desktop icons again
        ApplyTrayHost();   // (also sent by the tray host itself when it starts: then nothing to do)
        return 0;
    }
    switch (m) {
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        g_rebuildAt = NowSeconds() + 0.8;
        return 0;
    case WM_SETTINGCHANGE:
        // Explorer's taskbar settings changed (auto-hide may be off again): the enforcer thread asks at
        // once instead of at its next 5 s look, and puts it back
        if (l && !lstrcmpiW(reinterpret_cast<LPCWSTR>(l), L"TraySettings")) g_taskbar.RecheckSoon();
        return 0;
    case WM_POWERBROADCAST:   // plugged in or out, battery saver: the battery cell follows at once
        if (w == PBT_APMPOWERSTATUSCHANGE) g_sys.PollBattery();
        else if (w == PBT_POWERSETTINGCHANGE) {   // the display went off or came on (registered in CreateMainWindow)
            const auto* s = reinterpret_cast<const POWERBROADCAST_SETTING*>(l);
            if (s && s->PowerSetting == kSessionDisplayStatus && s->DataLength >= sizeof(DWORD)) {
                const bool off = *reinterpret_cast<const DWORD*>(s->Data) == 0;   // 1 on, 2 dimmed (still seen)
                if (off != g_look.displayOff) {
                    g_look.displayOff = off;
                    Log(L"display: %ls", off ? L"off - nobody sees the docks: the loop looks every 100 ms" : L"on");
                    if (!off) startmenu::DisplayCameOn(NowSeconds());   // for the start button's dock.log lines
                }
            }
        }
        return TRUE;
    case WM_APP_REBUILD:
        g_rebuildAt = NowSeconds();
        return 0;
    case WM_APP_SETTINGS: {   // "LiquidDock.exe --settings" from a shortcut while the dock runs
        POINT p{}; GetCursorPos(&p);
        settingswindow::Show(MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY));
        return 0;
    }
    case WM_APP_RELAYOUT:   // shape sliders: in place, unless a window has to change size
        for (auto& d : g_docks) if (!d->Relayout()) { g_rebuildAt = NowSeconds(); break; }
        return 0;
    case WM_APP_ENFORCE:   // from the enforcer thread: a new taskbar needs its buttons cached here first
        ++g_enforceCount;
        g_taskbar.Enforce(true);
        return 0;
    case WM_APP_AUTOSTART:
        SetAutostart(g_settings.autostart);
        return 0;
    case WM_HOTKEY:
        if (w == kRevealHotkeyId) {   // show the dock under the pointer
            POINT p{}; GetCursorPos(&p);
            HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
            for (auto& d : g_docks) if (d->monitor == m) d->HotkeyReveal(NowSeconds());
            return 0;
        }
        g_running = false;   // the emergency exit, Ctrl+Alt+Shift+F12
        return 0;
    case WM_APP_HOTKEY:
        ApplyRevealHotkey();
        return 0;
    case WM_APP_TRAYHOST:
        ApplyTrayHost();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (w) { g_running = false; g_taskbar.Restore(); }
        return 0;
    case WM_CLOSE:
        g_running = false;
        return 0;
    default:   // the developer commands (WM_DEV_*), else DefWindowProc as for any other message
        if (HandleDevMessage(m)) return 0;
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// The Windows release in dock.log: the stock taskbar's insides (tray buttons, flyouts) change between them.
static void LogWindowsVersion() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    wchar_t display[32] = {}, build[32] = {};
    DWORD ubr = 0, cb = sizeof(display);
    RegGetValueW(k, nullptr, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, display, &cb);
    cb = sizeof(build);
    RegGetValueW(k, nullptr, L"CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &cb);
    cb = sizeof(ubr);
    RegGetValueW(k, nullptr, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &cb);
    RegCloseKey(k);
    Log(L"Windows %ls %ls (build %ls.%lu), Liquid Dock %ls", _wtoi(build) >= 22000 ? L"11" : L"10", display, build, ubr, LD_VERSION_WSTR);
}

// Popup menus follow the Windows app theme (dark/light) like Explorer's. Undocumented but stable
// uxtheme exports (ordinal 135 SetPreferredAppMode, 136 FlushMenuThemes) since Windows 10 1903.
static void EnableThemedMenus() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto setMode = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(1);   // AllowDark: follow the system setting
    if (flush) flush();
}

// ---------------------------------------------------------------- wWinMain's stages, in its order

// A run that is not the dock: a developer flag (LD_DEV), --settings while a dock runs, --restore and --quit, or a start
// as administrator handed to the user. true: this run ends here with exitCode; false: it goes on to be the dock.
static bool HandleOtherRun(const std::wstring& args, const mainargs::LaunchArgs& launch, int& exitCode) {
#ifdef LD_DEV
    if (const DevFlag* d = mainargs::FirstFlag(args, kDevFlags)) {
        if (HWND other = FindWindowW(LD_MAIN_CLASS, nullptr)) PostMessageW(other, d->msg, 0, 0);
        exitCode = 0;
        return true;
    }
#endif
    if (launch.settings) {
        if (HWND other = FindWindowW(LD_MAIN_CLASS, nullptr)) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(other, WM_APP_SETTINGS, 0, 0); exitCode = 0; return true; }
        // not running: start normally (the settings window does not open by itself then)
    }
    if (launch.restore || launch.quit) {
        if (HWND other = FindWindowW(LD_MAIN_CLASS, nullptr)) {
            // ask nicely; a hung dock is ended and the taskbar restored from here
            DWORD pid = 0; GetWindowThreadProcessId(other, &pid);
            HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
            PostMessageW(other, WM_CLOSE, 0, 0);
            if (p && WaitForSingleObject(p, 3000) == WAIT_TIMEOUT) {
                TerminateProcess(p, 1);
                WaitForSingleObject(p, 2000);
            }
            if (p) CloseHandle(p);
        }
        // uninstall and the restore shortcut put the wallpaper and desktop icons back too
        const int rc = RestoreTaskbarWhenIdle(5000);
        // after the taskbar: an Explorer that was restarting is back by then, its wallpaper API most likely too
        const bool oledOff = !launch.restore || oled::Disable();
        exitCode = rc != 0 ? rc : oledOff ? 0 : ERROR_RETRY;   // the OLED record not undone (Explorer away): the uninstaller keeps it
        return true;
    }

    if (!launch.keepAdmin && !launch.fromAdmin && RestartUnelevated(args)) {
        exitCode = 0;   // the dock runs on as the user
        return true;
    }
    return false;
}

// The one dock: the instance mutex, waited for up to 3 s (a dock that is still exiting). false: this run ends with
// exitCode (1: no mutex, 0: another dock runs). The dock keeps it to its exit (EndDock).
static bool TakeInstance(HANDLE& mutex, int& exitCode) {
    mutex = CreateMutexW(nullptr, FALSE, kInstanceMutex);
    if (!mutex) { exitCode = 1; return false; }
    DWORD ownership = WaitForSingleObject(mutex, 3000);
    if (ownership != WAIT_OBJECT_0 && ownership != WAIT_ABANDONED) { CloseHandle(mutex); exitCode = 0; return false; }
    return true;
}

// The dock's process before its window: the crash handler, menus, settings, the Windows line, the GPU.
// false: no GPU (the taskbar restored, a message box): the run ends with 1.
static bool InitDockProcess(const mainargs::LaunchArgs& launch) {
    SetUnhandledExceptionFilter(CrashHandler);
    EnableThemedMenus();
    g_settings.Load();
    LogWindowsVersion();
    if (launch.fromAdmin)
        Log(L"started as administrator, running on as the user%s", ProcessElevated() ? L" - still elevated (Explorer is)" : L"");
    g_demoMode = launch.demo;
    if (!g_gfx.Init()) {
        g_taskbar.Restore();
        MessageBoxW(nullptr, L"GPU 초기화에 실패했습니다.", L"Liquid Dock", MB_ICONERROR);
        return false;
    }
    MonitorCapture::RequestBorderlessAccess();
    return true;
}

// The main window (MainProc) and what it is told of: the shell hook, a new Explorer's TaskbarCreated, the display's
// state, the two hotkeys. displayNotify: for EndDock. false: no window (the taskbar restored): the run ends with 1.
static bool CreateMainWindow(HINSTANCE hInst, HPOWERNOTIFY& displayNotify) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.lpszClassName = LD_MAIN_CLASS;
    RegisterClassExW(&wc);
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Liquid Dock", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_mainWnd) { g_taskbar.Restore(); return false; }
    g_msgShellHook = RegisterWindowMessageW(L"SHELLHOOK");
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_mainWnd, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    RegisterShellHookWindow(g_mainWnd);
    // the display going off and on (WM_POWERBROADCAST); Windows sends the state it is in at once
    displayNotify = RegisterPowerSettingNotification(g_mainWnd, &kSessionDisplayStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
    // emergency exit: Ctrl+Alt+Shift+F12
    RegisterHotKey(g_mainWnd, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
    ApplyRevealHotkey();
    return true;
}

// The modules' threads, the watchdog, then the docks (the taskbar hidden once they replace it), OLED, the logon task
static void StartDock() {
    g_apps.Init();
    g_sys.Tick(NowSeconds());
    g_sys.Start();
    g_sensors.Start();
    g_taskbar.StartEnforcer();
    if (!StartWatchdog()) Log(L"watchdog not ready; keeping native taskbar");
    BuildDocks();
    oled::Reapply();
    if (g_settings.autostart) SetAutostart(true);
}

// --profile: time spent per part of the loop, logged every 10 s (off otherwise: no cost). mark is set every pass
// (before the modules' ticks) whether on or not, as it always was.
namespace {
enum ProfilePart { PMsg, PApps, PFeatures, PSys, PSensors, PUpdate, PRender, PWait, PCount };
struct LoopProfile {
    bool on = false;
    double prof[PCount] = {}, profStart = 0, mark = 0; int profFrames = 0, profBusy = 0;
    void Lap(int part) { if (!on) return; double t = NowSeconds(); prof[part] += t - mark; mark = t; }
    void Report(double now, bool busy);
};

// After a pass (--profile only): counts it, and every 10 s the two lines and a fresh count
void LoopProfile::Report(double now, bool busy) {
    ++profFrames; profBusy += busy;
    if (now - profStart >= 10) {
        double total = now - profStart;
        Log(L"profile %.0fs frames %d busy %d | ms/s: msg %.2f apps %.2f features %.2f sys %.2f sensors %.2f update %.2f render %.2f wait %.1f",
            total, profFrames, profBusy, prof[PMsg] * 1000 / total, prof[PApps] * 1000 / total, prof[PFeatures] * 1000 / total, prof[PSys] * 1000 / total,
            prof[PSensors] * 1000 / total, prof[PUpdate] * 1000 / total, prof[PRender] * 1000 / total, prof[PWait] * 1000 / total);
        {   // what grows over time: threads, handles, memory, taskbar re-hides
            DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
            PROCESS_MEMORY_COUNTERS_EX pm = { sizeof(pm) }; GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
            int threads = 0; HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te = { sizeof(te) };
            if (snap != INVALID_HANDLE_VALUE) { for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) if (te.th32OwnerProcessID == GetCurrentProcessId()) ++threads; CloseHandle(snap); }
            FILETIME a, b, k, u; GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
            double cpu = ((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e7;
            Log(L"profile state: threads %d handles %lu private %.1f MB cpu-total %.1f s re-hides %d", threads, handles, pm.PrivateUsage / 1048576.0, cpu, g_enforceCount);
        }
        for (auto& v : prof) v = 0;
        profStart = now; profFrames = profBusy = 0;
    }
    mark = NowSeconds();
}
}  // namespace

// GPU reset / driver update: the device is gone; rebuild everything on a new one (tried again every 2 s)
static void RecoverLostDevice(double now) {
    if ((g_deviceResetRequested || g_gfx.DeviceLost()) && now >= g_nextDeviceRetry) {
        g_deviceResetRequested = false;
        Log(L"GPU device lost - recreating");
        settingswindow::Close();   // its swap chain belongs to the old device
        g_docksReady = false;
        g_docks.clear();
        Dock::ResetNotifyBitmaps();
        if (g_gfx.Reset()) { g_apps.ReloadIcons(); BuildDocks(); }
        else {
            g_nextDeviceRetry = now + 2.0; g_deviceResetRequested = true;
            Log(L"device re-create failed, retrying");
            if (g_taskbar.hidden) RestoreTaskbar();   // usable taskbar until the dock is back
        }
    }
}

// GetCursorPos fails while another desktop has the input (the lock screen, a UAC prompt, Ctrl+Alt+Del):
// nobody can use a dock then, so the docks see no pointer (off every monitor: they go away as when it
// leaves) and the loop looks at it as at a resting pointer away from the edge
// (idle_wait.h LookAtPointer: what moved, what nobody sees; here only the Windows calls and the lines).
// c: the point the docks see this pass.
static idlewait::Look ReadPointer(double now, POINT& c) {
    constexpr POINT kNoPointer = { idlewait::kNoPointerX, idlewait::kNoPointerY };   // off every monitor
    const bool cursorRead = GetCursorPos(&c) != FALSE;
    const idlewait::Look look = idlewait::LookAtPointer(g_look, cursorRead, c.x, c.y, now);
    if (look.readChanged) {
        if (cursorRead) Log(L"pointer: readable again");
        else Log(L"pointer: unreadable (error %lu: the lock screen, a UAC prompt or another desktop has the input) - the docks see none", GetLastError());
    }
    if (!cursorRead) c = kNoPointer;
    if (look.displayTakenOn) {
        Log(L"display: taken as on - the pointer moved, no notification said so");
        startmenu::DisplayCameOn(now);
    }
    Dock::screenUnseen = look.unseen;   // locked (or a UAC prompt), or the display off: the sensor cell's new text waits (Dock::Update)
    return look;
}

// Each dock updated, drawn and recorded with the pass's pointer; then the tray host's widget, the sensor thread's
// pace and the pass's notifications. busy: an animation (or FluentFlyout) wants the next frame; liveGlass: a dock's
// glass shows what moves behind it.
namespace { struct DocksPass { bool busy, liveGlass; }; }
static DocksPass UpdateDocks(double now, float dt, POINT c, LoopProfile& profile) {
    bool busy = fluentflyout::Busy(), liveGlass = false;
    Dock::widgetPlaced = false;
    for (auto& d : g_docks) {
        d->Update(now, dt, c); profile.Lap(PUpdate);
        d->Render(); profile.Lap(PRender);
        d->RecordTick(now);
        busy |= d->Busy();
        liveGlass |= d->LiveGlass();
    }
    if (!Dock::widgetPlaced) trayhost::Place(false, Dock::widgetLastRect);   // no dock holds FluentFlyout's widget
    {   // the sensor thread samples every second, the panel's drives and memory too, while any dock shows its
        // panel (and 5 s after); every 2 s otherwise (sensors.h)
        bool panelWanted = Dock::debugForcePanel;
        for (auto& d : g_docks) panelWanted |= d->PanelWanted();
        g_sensors.SetPanelOpen(panelWanted, now);
    }
    g_apps.attentionEvents.clear();
    g_sensors.alertPending = false;
    return { busy, liveGlass };
}

// idle: look at the pointer quickly only near the edge where the docks come up (idle_wait.h) - not
// at the other three (a browser's tabs and a title bar at the top bring no dock up)
static void WaitIdle(POINT c, const idlewait::Look& look, bool liveGlass, double now) {
    MONITORINFO mi = { sizeof(mi) };
    const HMONITOR cursorMonitor = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(cursorMonitor, &mi);
    const RECT& r = mi.rcMonitor;
    bool fullscreenHere = false;   // a game fills the pointer's monitor: no dock comes up there
    for (auto& d : g_docks) if (d->monitor == cursorMonitor && d->FullscreenHere()) fullscreenHere = true;
    DWORD wait = idlewait::IdleWaitMs(g_look, look, (int)g_settings.edge, r.left, r.top, r.right, r.bottom, c.x, c.y, fullscreenHere, liveGlass, now);
    MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

// The main loop, until WM_QUIT, WM_CLOSE (--quit), WM_ENDSESSION or the emergency exit: a pass a frame while
// anything is busy (DwmFlush), else a wait for input or the time idle_wait.h gives
static void RunMainLoop(const mainargs::LaunchArgs& launch) {
    // right after installing: show the dock for a few seconds so people see where it is
    double welcomeUntil = launch.welcome ? NowSeconds() + 3.5 : 0;
    if (welcomeUntil) Dock::debugForceShow = true;

    double last = NowSeconds();
    g_look.Start(last);
    LoopProfile profile;
    profile.on = launch.profile;
    profile.profStart = profile.mark = NowSeconds();
    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            if (settingswindow::PreTranslate(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;
        profile.Lap(PMsg);
        double now = NowSeconds();
        float dt = (float)std::clamp(now - last, 0.0, 0.05);
        last = now;

        if (g_rebuildAt > 0 && now >= g_rebuildAt) { g_rebuildAt = 0; BuildDocks(); }
        if (welcomeUntil && now >= welcomeUntil) { welcomeUntil = 0; Dock::debugForceShow = false; }
        RecoverLostDevice(now);

        // no dock replaces the taskbar: back it comes, tried again every second until that works (taskbar_visibility.h)
        if (!g_docksReady || !WatchdogReady()) g_restoreRetry.TryWhenDue(g_taskbar.hidden, now, [] { return g_taskbar.Restore(); });

        profile.mark = NowSeconds();
        g_apps.Tick(now); profile.Lap(PApps);
        features::Tick(now); oled::Tick(now);
        fluentflyout::Tick(now);
        adminhelper::Tick(now);
        FollowFluentFlyout();
        profile.Lap(PFeatures);
        g_sys.Tick(now); profile.Lap(PSys);
        g_sensors.Tick(now); profile.Lap(PSensors);

        POINT c{};
        const idlewait::Look look = ReadPointer(now, c);
        const DocksPass pass = UpdateDocks(now, dt, c, profile);

        if (profile.on) profile.Report(now, pass.busy);
        if (pass.busy) DwmFlush();
        else WaitIdle(c, look, pass.liveGlass, now);
        profile.Lap(PWait);
    }
}

// The dock's exit, in this order: FluentFlyout and the tray host let go, the docks gone, the taskbar back, then the
// window's registrations, the instance mutex and the watchdog's handle
static void EndDock(HANDLE mutex, HPOWERNOTIFY displayNotify) {
    g_docksReady = false;
    fluentflyout::Shutdown();
    trayhost::Stop();   // programs send their icons to Explorer again; FluentFlyout's widget goes back to its taskbar
    g_docks.clear();
    Dock::ResetNotifyBitmaps();
    g_taskbar.Restore();
    DeregisterShellHookWindow(g_mainWnd);
    if (displayNotify) UnregisterPowerSettingNotification(displayNotify);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    if (g_watchdogProcess) CloseHandle(g_watchdogProcess);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmd, int) {
    LogAppend();   // dock.log: only the dock instance starts it afresh (LogStartSession below); the restore shortcut, --quit and an elevated copy that hands over add to it
    mainargs::WatchdogArgs watchdog;
    if (mainargs::FindWatchdog(cmd, watchdog)) return RunWatchdog((DWORD)_wtoi(watchdog.pid), watchdog.ready);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    std::wstring args = cmd ? cmd : L"";   // as it is: an elevated start hands it over whole (RestartUnelevated)
    const mainargs::LaunchArgs launch = mainargs::ParseLaunchArgs(args);
    int exitCode = 0;
    if (HandleOtherRun(args, launch, exitCode)) return exitCode;

    HANDLE mutex = nullptr;
    if (!TakeInstance(mutex, exitCode)) return exitCode;
    LogStartSession();   // this run is the dock: its session starts dock.log afresh, with this run's lines from before (why it stays elevated) at the top

    if (!InitDockProcess(launch)) return 1;
    HPOWERNOTIFY displayNotify = nullptr;
    if (!CreateMainWindow(hInst, displayNotify)) return 1;
    StartDock();
    RunMainLoop(launch);
    EndDock(mutex, displayNotify);
    return 0;
}
