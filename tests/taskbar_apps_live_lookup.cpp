// Which taskbar button the dock would press, read from Windows' own taskbars (src/taskbar_apps.cpp, taskbar_apps_logic.h),
// for every monitor's dock: it only reads - nothing is pressed, no key or click goes out, no window is made, moved or
// activated. What tests\run_taskbar_apps_live.bat does not show: every taskbar (the main one and each monitor's), the
// order a click on each monitor's dock looks at them in (Taskbars: that monitor's taskbar first - the hidden secondary
// taskbar's monitor as MonitorFromWindow gives it), the ways each app button offers to press it, and for each running
// app with one window the button a click would press from each monitor (the first one counting its one window:
// CountFits), or why none. Also the taskbar settings that change the buttons (combining, several displays). Worth
// running with two monitors and once with "Combine taskbar buttons: Never" (the buttons' names).
// tests\run_taskbar_apps_live_lookup.bat builds and runs it, with the dock running (its taskbars hidden) as the dock
// sees them. A second or two; the mouse and keyboard may be used meanwhile.
#include "../src/common.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <appmodel.h>
#include <cstdio>
#include <map>
#include <thread>
#include "../src/taskbar_apps.cpp"

double NowSeconds() {
    static const LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* what) { ++checks; if (!ok) ++failures; printf("%s: %s\n", ok ? "PASS" : "FAIL", what); fflush(stdout); }

std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// ---- the running apps' windows, as the dock tells them apart (apps.cpp Refresh, in short)
std::wstring ExeOf(DWORD pid) {
    std::wstring out;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
        CloseHandle(p);
    }
    return taskbarapps::Lowered(out);
}
std::wstring ProcessAumid(DWORD pid) {
    std::wstring out;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH];
        UINT32 n = ARRAYSIZE(buf);
        if (GetApplicationUserModelId(p, &n, buf) == ERROR_SUCCESS) out = buf;
        CloseHandle(p);
    }
    return out;
}
std::wstring WindowAumid(HWND h) {
    std::wstring out;
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) out = pv.pwszVal;
        PropVariantClear(&pv);
    }
    return out;
}
// The windows the taskbar shows (the dock's own filter, apps.cpp IsTaskbarWindow).
bool OnTaskbar(HWND h) {
    if (!IsWindowVisible(h)) return false;
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_APPWINDOW) && (GetWindow(h, GW_OWNER) || (ex & WS_EX_TOOLWINDOW) || (ex & WS_EX_NOACTIVATE))) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return false;
    return GetWindowTextLengthW(h) > 0 || !wcscmp(cls, L"CabinetWClass");
}
struct App { std::wstring exe, aumid, cls; std::vector<HWND> windows; };
std::vector<App> RunningApps() {
    std::vector<HWND> all;
    EnumWindows([](HWND h, LPARAM p) -> BOOL { if (OnTaskbar(h)) reinterpret_cast<std::vector<HWND>*>(p)->push_back(h); return TRUE; },
                reinterpret_cast<LPARAM>(&all));
    std::map<std::wstring, App> byKey;   // the dock groups an app's windows by its id, else by its program's path
    std::vector<std::wstring> order;
    for (HWND h : all) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        std::wstring exe = ExeOf(pid), aumid = WindowAumid(h);
        if (aumid.empty()) aumid = ProcessAumid(pid);
        if (exe.size() > 26 && exe.compare(exe.size() - 26, 26, L"\\applicationframehost.exe") == 0) {   // a store app: its own process is the child's
            struct Ctx { DWORD host, found; } c{ pid, 0 };
            EnumChildWindows(h, [](HWND ch, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(lp);
                DWORD p = 0;
                GetWindowThreadProcessId(ch, &p);
                if (p != c->host) { c->found = p; return FALSE; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&c));
            if (c.found) { exe = ExeOf(c.found); if (aumid.empty()) aumid = ProcessAumid(c.found); }
        }
        const std::wstring key = aumid.empty() ? exe : taskbarapps::Lowered(aumid);
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, ARRAYSIZE(cls));
            it = byKey.emplace(key, App{ exe, aumid, cls, {} }).first;
            order.push_back(key);
        }
        it->second.windows.push_back(h);
    }
    std::vector<App> out;
    for (const auto& k : order) out.push_back(byKey[k]);
    return out;
}

// ---- the monitors, as a click on each one's dock passes it on (Job::monitor)
struct Monitor { HMONITOR h; RECT r; bool primary; };
std::vector<Monitor> Monitors() {
    std::vector<Monitor> out;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(m, &mi);
        reinterpret_cast<std::vector<Monitor>*>(p)->push_back({ m, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    return out;
}
int IndexOf(const std::vector<Monitor>& ms, HMONITOR m) {
    for (size_t i = 0; i < ms.size(); ++i) if (ms[i].h == m) return (int)i;
    return -1;
}
const char* Kind(HWND bar) { return bar == ExplorerTaskbar() ? "main" : "secondary"; }

DWORD Setting(const wchar_t* name) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return 0xFFFFFFFF;
    return v;
}

// What a click on the app from that monitor's dock would press: the same lookup as taskbar_apps.cpp Run, without the press.
struct Found { std::wstring id, name; HWND bar = nullptr; int count = -1; bool other = false; int otherCount = -1; std::wstring otherName; };
Found Lookup(IUIAutomation* uia, HMONITOR monitor, const App& app, bool running) {
    using namespace taskbarapps;
    Found f;
    const std::wstring aumid = Lowered(app.aumid), exe = Lowered(app.exe);
    const KnownFolder folder = [](std::wstring_view id) { return Lowered(KnownFolderPath(id)); };
    EachButton(uia, Taskbars(monitor), [] { return false; },
               [&](const ComPtr<IUIAutomationElement>&, std::wstring_view id, const std::wstring& name, HWND bar) {
        if (!Names(Lowered(id), aumid, exe, folder)) return false;
        const int count = WindowCount(name);
        if (!CountFits(running, count)) {
            if (!f.other) { f.other = true; f.otherCount = count; f.otherName = name; }
            return false;
        }
        f.id = std::wstring(id); f.name = name; f.bar = bar; f.count = count;
        return true;
    });
    return f;
}
}   // namespace

int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    printf("SETTINGS combine %ld (0 always, 1 when full, 2 never; -1 not set), several displays: taskbar on all %ld, buttons on %ld "
           "(0 all taskbars, 1 main and where the window is, 2 where the window is), combine on the others %ld\n",
           (long)Setting(L"TaskbarGlomLevel"), (long)Setting(L"MMTaskbarEnabled"), (long)Setting(L"MMTaskbarMode"), (long)Setting(L"MMTaskbarGlomLevel"));
    const std::vector<Monitor> monitors = Monitors();
    for (size_t i = 0; i < monitors.size(); ++i)
        printf("MONITOR %zu%s: %ld,%ld - %ld,%ld\n", i, monitors[i].primary ? " (primary)" : "", monitors[i].r.left, monitors[i].r.top,
               monitors[i].r.right, monitors[i].r.bottom);
    const std::vector<App> apps = RunningApps();
    printf("APPS %zu running with windows on the taskbar\n", apps.size());
    printf("CLASSIC taskbar: %d\n", taskbarapps::Classic() ? 1 : 0);

    std::thread([&] {   // on a thread of the multithreaded apartment, as in the dock
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            std::lock_guard<std::mutex> call(taskbarapps::S().calls);
            IUIAutomation* uia = taskbarapps::Uia();
            Check(uia != nullptr, "UI Automation is there");
            if (uia) {
                // every taskbar and its buttons
                const HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
                int total = 0;
                for (HWND bar : taskbarapps::Taskbars(primary)) {
                    RECT r{};
                    GetWindowRect(bar, &r);
                    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
                    const int region = GetWindowRgn(bar, rgn);
                    DeleteObject(rgn);
                    printf("TASKBAR %s %p: monitor %d (MonitorFromWindow), window %ld,%ld - %ld,%ld%s\n", Kind(bar), (void*)bar,
                           IndexOf(monitors, MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST)), r.left, r.top, r.right, r.bottom,
                           region == NULLREGION ? " (made invisible by the dock)" : "");
                    int n = 0;
                    taskbarapps::EachButton(uia, { bar }, [] { return false; },
                                            [&](const ComPtr<IUIAutomationElement>& e, std::wstring_view id, const std::wstring& name, HWND) {
                        auto has = [&](PATTERNID p) { ComPtr<IUnknown> u; return SUCCEEDED(e->GetCurrentPattern(p, &u)) && u ? 1 : 0; };
                        printf("  BUTTON id \"%s\" name \"%s\" windows %d ways: Invoke %d Toggle %d LegacyIAccessible %d\n", U8(std::wstring(id)).c_str(),
                               U8(name).c_str(), taskbarapps::WindowCount(name), has(UIA_InvokePatternId), has(UIA_TogglePatternId),
                               has(UIA_LegacyIAccessiblePatternId));
                        ++n;
                        return false;
                    });
                    printf("  %d app buttons\n", n);
                    total += n;
                }
                Check(total > 0, "the taskbars' app buttons are found (the dock hides the taskbars: from their XAML islands)");
                // the order a click on each monitor's dock looks at the taskbars in
                for (size_t i = 0; i < monitors.size(); ++i) {
                    printf("ORDER from monitor %zu's dock:", i);
                    for (HWND bar : taskbarapps::Taskbars(monitors[i].h))
                        printf(" %s (monitor %d)", Kind(bar), IndexOf(monitors, MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST)));
                    printf("\n");
                }
                // each running app: the button a click would press, from each monitor's dock
                int one = 0, found = 0, counted = 0, withButton = 0;
                for (const App& app : apps) {
                    const size_t slash = app.exe.find_last_of(L'\\');
                    const std::wstring shortExe = slash == std::wstring::npos ? app.exe : app.exe.substr(slash + 1);
                    const HMONITOR its = MonitorFromWindow(app.windows.front(), MONITOR_DEFAULTTONEAREST);
                    printf("APP %s (%s) aumid \"%s\": %zu window(s), the first on monitor %d\n", U8(shortExe).c_str(), U8(app.cls).c_str(),
                           U8(app.aumid).c_str(), app.windows.size(), IndexOf(monitors, its));
                    bool anyCount = false, anyButton = false;
                    for (size_t i = 0; i < monitors.size(); ++i) {
                        const Found f = Lookup(uia, monitors[i].h, app, true);
                        anyButton |= !f.id.empty() || f.other;
                        anyCount |= f.count >= 1 || f.otherCount >= 1;
                        if (app.windows.size() != 1) continue;   // several windows: the dock's own rules and preview, no press
                        ++one;
                        if (!f.id.empty()) {
                            ++found;
                            printf("  from monitor %zu's dock: would press the %s taskbar's button \"%s\" (\"%s\", counting %d)\n", i, Kind(f.bar),
                                   U8(f.id).c_str(), U8(f.name).c_str(), f.count);
                        } else if (f.other) {
                            printf("  from monitor %zu's dock: its button counts %d (\"%s\"), the dock 1 - the dock's own way\n", i, f.otherCount, U8(f.otherName).c_str());
                        } else {
                            printf("  from monitor %zu's dock: no button names it - the dock's own way\n", i);
                        }
                    }
                    withButton += anyButton;
                    counted += anyButton && anyCount;
                    if (anyButton && !anyCount) printf("  NO COUNT: its buttons' names give no window count (this language or setting): the dock never presses them\n");
                }
                printf("SUMMARY %d of %zu apps have a button the dock finds; %d of those name their windows' count; one-window clicks: %d of %d find the button meant\n",
                       withButton, apps.size(), counted, found, one);
                Check(counted == withButton, "every running app's button the dock finds tells how many windows it has (else the dock never presses it)");
            }
        }
        CoUninitialize();
    }).join();
    printf("RESULT taskbar_apps_live_lookup checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
