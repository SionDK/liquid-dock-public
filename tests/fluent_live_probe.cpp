// FluentFlyout with the dock on this PC, observed only (tests\run_fluent_live.bat): nothing is moved,
// clicked, sent or written - it reads windows, FluentFlyout's settings file and the dock's log.
//   - the monitors in FluentFlyout's numbering ("1", "2" ...) and their DPI
//   - Explorer's taskbars: the main one, the secondary ones, the dock's host window ("Shell_TrayWnd"
//     marked LiquidDock.TrayHost). No secondary taskbar on a PC with several monitors: FluentFlyout,
//     set to another monitor, falls back to the first Shell_TrayWnd: the dock follows its settings file
//   - FluentFlyout's settings file(s) (the classic one, the Store one): the newest, its widget monitor
//   - where its widget window is, on which monitor, and whether what it shows fits in its parent
//   - for a while (default 25 s): its flyouts - where each opened, where it settled, and whether it
//     was opened again during its closing fade (its open animation hides it and shows it again at once)
//     and where it settled then
//   - the dock's lines about FluentFlyout from dock.log
// Exit code: the number of FAIL verdicts (2 more when FluentFlyout is not running).
// Build: tests\run_fluent_live.bat
//   cl /std:c++20 /EHsc /utf-8 tests\fluent_live_probe.cpp user32.lib gdi32.lib shell32.lib ole32.lib shcore.lib
#include <windows.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <tlhelp32.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../src/fluent_logic.h"
#include "../src/fluent_settings.h"

namespace {

int g_fails = 0;
LARGE_INTEGER g_freq, g_start;

double Now() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart);
}

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string b(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), b.data(), n, nullptr, nullptr);
    return b;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

void Verdict(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

std::wstring ClassOf(HWND h) { wchar_t c[256] = {}; GetClassNameW(h, c, 256); return c; }
std::wstring TitleOf(HWND h) { wchar_t t[256] = {}; GetWindowTextW(h, t, 256); return t; }

// ---------------------------------------------------------------- monitors
struct Mon { HMONITOR h; RECT r, work; bool primary; UINT dpi; };
std::vector<Mon> g_mons;
std::vector<fluentlogic::Monitor> g_descs;

int MonIndex(HMONITOR h) {
    for (size_t i = 0; i < g_mons.size(); ++i) if (g_mons[i].h == h) return (int)i;
    return -1;
}

std::string MonName(HMONITOR h) {
    int i = MonIndex(h);
    if (i < 0) return "(no monitor)";
    char t[160];
    const Mon& m = g_mons[i];
    std::snprintf(t, sizeof(t), "monitor %d \"%d\" (%ld,%ld %ldx%ld, %u%%%s)", i, fluentlogic::SettingNumber(g_descs, i), m.r.left, m.r.top,
                  m.r.right - m.r.left, m.r.bottom - m.r.top, m.dpi * 100 / 96, m.primary ? ", main" : "");
    return t;
}

// ---------------------------------------------------------------- FluentFlyout's settings
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) s = p;
    CoTaskMemFree(p);
    return s;
}

bool ReadFileText(const std::wstring& path, std::wstring& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20);
    std::string bytes;
    if (ok) {
        bytes.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr) && got == bytes.size();
    }
    CloseHandle(f);
    if (!ok) return false;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) bytes.erase(0, 3);
    out = FromUtf8(bytes);
    return true;
}

// ---------------------------------------------------------------- FluentFlyout's flyouts, watched
struct Track {
    HWND h = nullptr;
    int number = 0;
    RECT last{}, settled{}, firstSettled{};
    double lastMoveAt = 0;
    int settles = 0;
    bool reopened = false;     // shown again right after it hid: opened again during its closing fade
    RECT before{};             // ...where it had rested until then
};
std::vector<Track> g_tracks;
int g_trackCount = 0;
struct Hidden { HWND h; double at; RECT rested; };
std::vector<Hidden> g_hidden;  // flyouts that hid a moment ago after resting somewhere
DWORD g_pid = 0;

bool IsFlyout(HWND h) {
    if (!IsWindowVisible(h) || GetAncestor(h, GA_PARENT) != GetDesktopWindow()) return false;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) return false;
    if (TitleOf(h) == L"TaskbarWindow") return false;
    RECT r{};
    if (!GetWindowRect(h, &r) || r.right <= r.left || r.bottom <= r.top) return false;
    UINT dpi = GetDpiForWindow(h);
    float s = dpi ? dpi / 96.0f : 1.0f;
    return r.right - r.left <= 520 * s && r.bottom - r.top <= 360 * s;
}

std::string RectText(const RECT& r) {
    char t[96];
    std::snprintf(t, sizeof(t), "%ld,%ld %ldx%ld (bottom %ld)", r.left, r.top, r.right - r.left, r.bottom - r.top, r.bottom);
    return t;
}

// near the bottom of its monitor's work area: FluentFlyout's own place for a bottom position
bool AtOwnPlace(const RECT& r) {
    int i = MonIndex(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST));
    if (i < 0) return false;
    const float s = g_mons[i].dpi / 96.0f;
    return r.bottom > g_mons[i].work.bottom - (long)std::lround(40 * s) && r.bottom <= g_mons[i].work.bottom;
}

void Report(const Track& t) {
    std::printf("  flyout %d closed: opened to %s, %d place(s) where it rested%s\n", t.number, RectText(t.firstSettled).c_str(), t.settles,
                t.reopened ? ", opened again during its closing fade" : "");
    if (!t.reopened || t.settles == 0) return;
    const bool movedByDock = !AtOwnPlace(t.before);
    std::printf("    before it was opened again it rested at %s (%s), then at %s (%s)\n", RectText(t.before).c_str(),
                movedByDock ? "moved next to the dock" : "FluentFlyout's own place", RectText(t.settled).c_str(),
                AtOwnPlace(t.settled) ? "FluentFlyout's own place" : "next to the dock");
    if (!movedByDock) { std::printf("    (the dock had not moved it - hidden dock or top position: nothing to compare)\n"); return; }
    Verdict(!AtOwnPlace(t.settled), "a flyout opened again during its closing fade rests next to the dock again, not over its glass");
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    auto it = g_tracks.begin();
    for (; it != g_tracks.end() && it->h != h; ++it) {}
    const double now = Now();
    std::erase_if(g_hidden, [&](const Hidden& x) { return now - x.at > 0.3; });
    if (ev == EVENT_OBJECT_HIDE || ev == EVENT_OBJECT_DESTROY) {
        if (it == g_tracks.end()) return;
        Report(*it);
        if (ev == EVENT_OBJECT_HIDE && it->settles > 0) g_hidden.push_back({ h, now, it->settled });
        g_tracks.erase(it);
        return;
    }
    RECT r{};
    GetWindowRect(h, &r);
    if (ev == EVENT_OBJECT_SHOW) {
        if (it != g_tracks.end() || !IsFlyout(h)) return;
        Track t;
        t.h = h; t.number = ++g_trackCount; t.last = r; t.lastMoveAt = now;
        // opened again during its closing fade: FluentFlyout's open animation hides the window and shows it
        // again at once (MainWindow.OpenAnimation, WindowHelper.SetVisibility)
        auto was = std::find_if(g_hidden.begin(), g_hidden.end(), [&](const Hidden& x) { return x.h == h; });
        if (was != g_hidden.end()) { t.reopened = true; t.before = was->rested; g_hidden.erase(was); }
        g_tracks.push_back(t);
        std::printf("  %.2f s: flyout %d shown at %s on %s%s\n", now, t.number, RectText(r).c_str(), MonName(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)).c_str(),
                    t.reopened ? " - the same window right after it hid: opened again during its closing fade" : "");
        return;
    }
    if (ev != EVENT_OBJECT_LOCATIONCHANGE || it == g_tracks.end() || EqualRect(&r, &it->last)) return;
    it->last = r;
    it->lastMoveAt = now;
}

void Settle() {
    const double now = Now();
    for (auto& t : g_tracks) {
        if (now - t.lastMoveAt < 0.35 || (t.settles > 0 && EqualRect(&t.last, &t.settled))) continue;
        t.settled = t.last;
        if (++t.settles == 1) t.firstSettled = t.last;
        std::printf("  %.2f s: flyout %d rests at %s%s\n", now, t.number, RectText(t.last).c_str(), AtOwnPlace(t.last) ? " - FluentFlyout's own place" : " - moved (next to the dock)");
    }
}

}   // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // physical pixels, as the dock
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_start);
    const int watchSeconds = argc > 1 ? (std::max)(0, _wtoi(argv[1])) : 25;   // (std::max): windows.h's macro

    // ---- monitors
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM) -> BOOL {
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(h, &mi);
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &dx, &dy);
        g_mons.push_back({ h, mi.rcMonitor, mi.rcWork, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, dx });
        g_descs.push_back({ (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, mi.rcMonitor.left });
        return TRUE;
    }, 0);
    std::printf("monitors (system order, FluentFlyout's number in quotes):\n");
    for (auto& m : g_mons) std::printf("  %s\n", MonName(m.h).c_str());

    // ---- taskbars
    int secondaries = 0;
    HWND host = nullptr, explorerMain = nullptr;
    std::printf("taskbars:\n");
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)) != nullptr;) {
        const bool isHost = GetPropW(h, L"LiquidDock.TrayHost") != nullptr;
        if (isHost && !host) host = h;
        if (!isHost && !explorerMain) explorerMain = h;
        RECT r{}; GetWindowRect(h, &r);
        std::printf("  Shell_TrayWnd %p%s at %s on %s\n", (void*)h, isHost ? " (the dock's host window)" : " (Explorer's main taskbar)",
                    RectText(r).c_str(), MonName(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)).c_str());
    }
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr; ++secondaries)
        std::printf("  Shell_SecondaryTrayWnd %p on %s\n", (void*)h, MonName(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)).c_str());
    HWND first = FindWindowW(L"Shell_TrayWnd", nullptr);
    std::printf("  the first Shell_TrayWnd (where FluentFlyout looks first): %s\n", first == host && host ? "the dock's host window" : first ? "Explorer's" : "none");
    if (g_mons.size() > 1 && secondaries == 0)
        std::printf("  NOTE: several monitors but no secondary taskbar (\"show my taskbar on all displays\" off): FluentFlyout, set to\n"
                    "  another monitor, falls back to the first Shell_TrayWnd - the dock puts its widget where its settings file says\n");

    // ---- FluentFlyout's process
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"FluentFlyout.exe")) { g_pid = pe.th32ProcessID; break; }
    CloseHandle(snap);
    if (!g_pid) {
        std::printf("FluentFlyout is not running: start it (with its taskbar widget on) and run this again\n");
        return 2;
    }
    std::printf("FluentFlyout: process %lu\n", g_pid);

    // ---- its settings file(s): the newest counts (as the dock reads them)
    std::vector<std::wstring> files;
    const std::wstring roaming = KnownFolder(FOLDERID_RoamingAppData), local = KnownFolder(FOLDERID_LocalAppData);
    if (!roaming.empty()) files.push_back(roaming + L"\\FluentFlyout\\settings.xml");
    if (!local.empty()) {
        WIN32_FIND_DATAW fd;
        const std::wstring packages = local + L"\\Packages\\";
        HANDLE f = FindFirstFileW((packages + L"unchihugo.FluentFlyout_*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) files.push_back(packages + fd.cFileName + L"\\LocalCache\\Roaming\\FluentFlyout\\settings.xml");
            while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    fluentsettings::Settings ff;
    std::wstring best;
    FILETIME bestTime{};
    std::printf("its settings files:\n");
    for (auto& p : files) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) { std::printf("  %s: none\n", Utf8(p).c_str()); continue; }
        SYSTEMTIME st; FILETIME lt; FileTimeToLocalFileTime(&a.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &st);
        std::printf("  %s: saved %04d-%02d-%02d %02d:%02d:%02d\n", Utf8(p).c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        if (best.empty() || CompareFileTime(&a.ftLastWriteTime, &bestTime) > 0) { best = p; bestTime = a.ftLastWriteTime; }
    }
    std::wstring xml;
    if (!best.empty() && ReadFileText(best, xml)) ff = fluentsettings::Parse(xml);
    int wantMonitor = -1;
    if (ff.loaded) {
        wantMonitor = fluentlogic::MonitorForSetting(g_descs, ff.widgetMonitor);
        std::printf("  the newest: widget %d, hide completely %d, its monitor setting \"%d\" -> %s, padding %d, animation %d, app filter %d (%s, %d allowed, %d blocked)\n",
                    (int)ff.widgetEnabled, (int)ff.hideCompletely, ff.widgetMonitor + 1, wantMonitor >= 0 ? MonName(g_mons[wantMonitor].h).c_str() : "?",
                    ff.manualPadding, ff.animationSpeed, (int)ff.filtering, ff.filterMode == 0 ? "block" : "allow", (int)ff.allowed.size(), (int)ff.blocked.size());
    } else std::printf("  no readable settings file: FluentFlyout's defaults\n");

    // ---- its widget window
    HWND widget = nullptr;
    auto consider = [&](HWND h) {
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == g_pid && TitleOf(h) == L"TaskbarWindow" && !ClassOf(h).compare(0, 12, L"HwndWrapper[")) widget = h;
    };
    for (HWND h = nullptr; !widget && (h = FindWindowExW(nullptr, h, nullptr, L"TaskbarWindow")) != nullptr;) consider(h);
    for (HWND t : { host, explorerMain })
        for (HWND c = t ? GetWindow(t, GW_CHILD) : nullptr; !widget && c; c = GetWindow(c, GW_HWNDNEXT)) consider(c);
    for (HWND s = nullptr; !widget && (s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        for (HWND c = GetWindow(s, GW_CHILD); !widget && c; c = GetWindow(c, GW_HWNDNEXT)) consider(c);
    if (!widget) std::printf("its widget window: not found (widget off, no Premium, or not made yet)\n");
    else {
        HWND parent = GetAncestor(widget, GA_PARENT);
        RECT wr{}, pr{};
        GetWindowRect(widget, &wr); GetWindowRect(parent, &pr);
        HRGN rgn = CreateRectRgn(0, 0, 0, 0);
        RECT box{};
        int type = GetWindowRgn(widget, rgn);
        if (type == ERROR) box = { 0, 0, wr.right - wr.left, wr.bottom - wr.top }; else GetRgnBox(rgn, &box);
        DeleteObject(rgn);
        const std::wstring pc = parent == GetDesktopWindow() ? L"(none: a window of its own)" : ClassOf(parent);
        const HMONITOR pm = MonitorFromWindow(parent == GetDesktopWindow() ? widget : parent, MONITOR_DEFAULTTONEAREST);
        std::printf("its widget window %p: in %s%s on %s, DPI of the parent %u\n", (void*)widget, Utf8(pc).c_str(), parent == host ? " (the dock's host window)" : "",
                    MonName(pm).c_str(), GetDpiForWindow(parent == GetDesktopWindow() ? widget : parent));
        std::printf("  window %s in a parent of %ldx%ld, what it shows %ld,%ld-%ld,%ld (in its window)%s\n", RectText(wr).c_str(), pr.right - pr.left, pr.bottom - pr.top,
                    box.left, box.top, box.right, box.bottom, type == NULLREGION ? " - empty (hidden by FluentFlyout)" : "");
        if (parent == host) {
            const long ox = wr.left - pr.left;
            Verdict(type == NULLREGION || (box.left + ox >= 0 && box.right + ox <= pr.right - pr.left), "what the widget shows fits in the dock's host window at this DPI");
            if (wantMonitor >= 0)
                Verdict(MonIndex(pm) == wantMonitor, "the widget is on the dock of the monitor FluentFlyout's settings name (a stale, unsaved setting would explain a FAIL)");
        } else if (parent != GetDesktopWindow() && host)
            std::printf("  (not in the dock's host window right now: FluentFlyout moves it there every 1.5 s when the host window is on its monitor)\n");
    }

    // ---- its flyouts, for a while
    if (watchSeconds > 0) {
        std::printf("watching FluentFlyout's flyouts for %d s. Meanwhile: bring the dock up (pointer at its edge), press\n"
                    "play/pause or a volume key once, wait until the flyout starts to fade (about 3 s), press it again right\n"
                    "then; two or three times.\n", watchSeconds);
        HWINEVENTHOOK h1 = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, OnEvent, g_pid, 0, WINEVENT_OUTOFCONTEXT);
        HWINEVENTHOOK h2 = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnEvent, g_pid, 0, WINEVENT_OUTOFCONTEXT);
        const double until = Now() + watchSeconds;
        while (Now() < until) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            Settle();
        }
        for (auto& t : g_tracks) Report(t);
        if (h1) UnhookWinEvent(h1);
        if (h2) UnhookWinEvent(h2);
        if (g_trackCount == 0) std::printf("  no flyout opened\n");
    }

    // ---- the dock's lines about FluentFlyout
    std::wstring log;
    const std::wstring logPath = local + L"\\Programs\\LiquidDock\\data\\dock.log";
    if (!local.empty() && ReadFileText(logPath, log)) {
        std::vector<std::wstring> lines;
        for (size_t p = 0; p < log.size();) {
            size_t e = log.find(L'\n', p);
            std::wstring line = log.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
            if (line.find(L"fluentflyout:") != std::wstring::npos || line.find(L"tray host: widget") != std::wstring::npos) lines.push_back(line);
            if (e == std::wstring::npos) break;
            p = e + 1;
        }
        std::printf("dock.log, its lines about FluentFlyout (the last %d of %d):\n", (int)std::min<size_t>(lines.size(), 40), (int)lines.size());
        for (size_t i = lines.size() > 40 ? lines.size() - 40 : 0; i < lines.size(); ++i) std::printf("  %s\n", Utf8(lines[i]).c_str());
    } else std::printf("dock.log: not found at %s\n", Utf8(logPath).c_str());

    std::printf("fluent_live_probe: %d FAIL verdict%s\n", g_fails, g_fails == 1 ? "" : "s");
    CoUninitialize();
    return g_fails;
}
