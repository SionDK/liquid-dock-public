// How Windows tells whether Start is up (IAppVisibility, src/start_menu.cpp), on real Windows - the user opens
// and closes Start with the Windows key and restarts Explorer when asked; this program only reads.
// The dock asks it two ways: InFront(), on its UI thread (an STA), keeps ONE object made at the first press for
// the life of the process; the click's worker makes a fresh one on an MTA thread each click, and takes a slow
// making (kBusyCall, 0.25 s) as "Explorer is busy". This shows
//   1. how long making the object takes on a fresh MTA thread (what the worker's Explorer-busy guess rests on),
//   2. how long IsLauncherVisible takes on the STA (what the dock's UI thread waits at each press),
//   3. whether the object kept from before an Explorer restart still sees Start afterwards (if not, the dock's
//      start button no longer closes an open Start once Explorer has restarted, on 25H2 where Start's search box
//      is what is in front),
//   4. what it says while only Search (Win+S) is open.
// Nothing is pressed, sent or changed; no window is touched. tests\run_start_menu_visibility_live.bat runs it.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\start_menu_visibility_live.cpp /link user32.lib ole32.lib advapi32.lib
#include "../src/common.h"
#include "../src/explorer_taskbar.h"
#include "../src/app_visibility.h"
#include <objbase.h>
#include <cstdarg>
#include <cstdio>
#include <thread>

namespace {

using IVisibility = IAppVisibilityDecl;   // IAppVisibility, the dock's own declaration (src/app_visibility.h)

double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

// Output to the log (stdout) and, while the log is redirected (logged.bat), to the console as well: the user
// has to read the steps as they come.
HANDLE g_console = INVALID_HANDLE_VALUE;
void Say(bool prompt, const char* fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    fputs(b, stdout); fputs("\n", stdout); fflush(stdout);
    if (prompt && g_console != INVALID_HANDLE_VALUE) {
        wchar_t w[1024];
        MultiByteToWideChar(CP_UTF8, 0, b, -1, w, ARRAYSIZE(w));
        DWORD put = 0;
        WriteConsoleW(g_console, w, static_cast<DWORD>(wcslen(w)), &put, nullptr);
        WriteConsoleW(g_console, L"\r\n", 2, &put, nullptr);
    }
}

std::string FrontExe() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid) return "nothing";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return "?";
    wchar_t path[MAX_PATH * 2];
    DWORD n = ARRAYSIZE(path);
    std::string r = "?";
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        r = ToUtf8(name ? name + 1 : path);
    }
    CloseHandle(p);
    return r;
}
DWORD ExplorerPid() {
    DWORD pid = 0;
    if (HWND h = ExplorerTaskbar()) GetWindowThreadProcessId(h, &pid);
    return pid;
}

struct Look { HRESULT hr = E_FAIL; BOOL visible = FALSE; double ms = 0; };
Look Ask(IVisibility* v) {
    Look l;
    if (!v) return l;
    const double t0 = Now();
    l.hr = v->IsLauncherVisible(&l.visible);
    l.ms = (Now() - t0) * 1000;
    return l;
}

}   // namespace

// ---- what explorer_taskbar.h's callers would need (nothing here logs)
std::string ToUtf8(const std::wstring& w) {
    char b[1024] = {};
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // as the dock's UI thread
    g_console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    int problems = 0;

    // ---- 1. making the object on a fresh MTA thread, as each click's worker does (>= 250 ms: "Explorer busy")
    double first = 0, worst = 0;
    for (int i = 0; i < 8; ++i) {
        double ms = 0;
        std::thread([&] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ComPtr<IVisibility> v;
            const double t0 = Now();
            CoCreateInstance(kAppVisibility, nullptr, CLSCTX_ALL, kAppVisibilityIid, reinterpret_cast<void**>(v.GetAddressOf()));
            ms = (Now() - t0) * 1000;
            v.Reset();
            CoUninitialize();
        }).join();
        if (!i) first = ms;
        if (ms > worst) worst = ms;
    }
    Say(false, "MAKE on a fresh MTA thread: first %.1f ms, worst of 8 %.1f ms (the worker takes 250 ms or more as \"Explorer busy\")", first, worst);

    // ---- the object kept on this STA, as InFront() keeps it
    ComPtr<IVisibility> kept;
    const HRESULT made = CoCreateInstance(kAppVisibility, nullptr, CLSCTX_ALL, kAppVisibilityIid, reinterpret_cast<void**>(kept.GetAddressOf()));
    Say(false, "KEPT made on the STA: hr %08lx", (unsigned long)made);
    if (FAILED(made)) { Say(true, "VERDICT: IAppVisibility cannot be made here: InFront() never sees Start"); return 1; }

    // ---- poll every 100 ms; steps for the user
    enum Step { OpenFirst, CloseFirst, Restart, Back, OpenAfter, CloseAfter, Search, End } step = OpenFirst;
    auto prompt = [](Step s) {
        switch (s) {
        case OpenFirst: Say(true, ">>> 1/4: press the Windows key to open Start (60 s)"); break;
        case CloseFirst: Say(true, ">>> ...now close Start (Esc)"); break;
        case Restart: Say(true, ">>> 2/4: restart Explorer: Ctrl+Shift+Esc, Processes, \"Windows Explorer\", Restart (90 s). Wait until the taskbar or the dock is back."); break;
        case Back: break;
        case OpenAfter: Say(true, ">>> 3/4: press the Windows key to open Start again (60 s)"); break;
        case CloseAfter: Say(true, ">>> ...now close Start (Esc)"); break;
        case Search: Say(true, ">>> 4/4 (optional): press Win+S to open Search only, then Esc (30 s)"); break;
        case End: break;
        }
    };
    prompt(step);
    const DWORD explorerAtStart = ExplorerPid();
    Say(false, "explorer pid %lu", explorerAtStart);
    double stepAt = Now(), maxKeptMs = 0, backAt = 0;
    bool keptSawBefore = false, keptSawAfter = false, freshSawAfter = false, keptMissedAfter = false, searchSeen = false, searchSaysStart = false;
    std::string lastLine;
    while (step != End) {
        Sleep(100);
        const Look k = Ask(kept.Get());
        ComPtr<IVisibility> fresh;
        CoCreateInstance(kAppVisibility, nullptr, CLSCTX_ALL, kAppVisibilityIid, reinterpret_cast<void**>(fresh.GetAddressOf()));
        const Look f = Ask(fresh.Get());
        fresh.Reset();
        if (k.ms > maxKeptMs) maxKeptMs = k.ms;
        const std::string front = FrontExe();
        const DWORD pid = ExplorerPid();
        char line[512];
        snprintf(line, sizeof(line), "kept: hr %08lx visible %d | fresh: hr %08lx visible %d | front %s | explorer %lu",
                 (unsigned long)k.hr, k.visible, (unsigned long)f.hr, f.visible, front.c_str(), pid);
        if (line != lastLine) { Say(false, "%7.2f s  %s  (kept call %.1f ms)", Now() - stepAt, line, k.ms); lastLine = line; }
        if (k.ms >= 100) Say(false, "SLOW: the kept object's IsLauncherVisible took %.0f ms on the STA (the dock's UI thread waits that long at a press)", k.ms);
        const bool startUp = f.visible || !_stricmp(front.c_str(), "StartMenuExperienceHost.exe");
        const double inStep = Now() - stepAt;
        const Step was = step;
        switch (step) {
        case OpenFirst: if (startUp) { keptSawBefore = SUCCEEDED(k.hr) && k.visible; step = CloseFirst; } else if (inStep > 60) step = Restart; break;
        case CloseFirst: if (!startUp) step = Restart; else if (inStep > 30) step = Restart; break;
        case Restart:
            if (pid && pid != explorerAtStart) { step = Back; backAt = Now(); }
            else if (inStep > 90) { Say(true, "Explorer was not restarted: step 3 skipped"); step = Search; }
            break;
        case Back: if (Now() - backAt > 5) step = OpenAfter; break;   // let the new Explorer settle
        case OpenAfter:
            if (startUp) {
                freshSawAfter = true;
                keptSawAfter = SUCCEEDED(k.hr) && k.visible;
                keptMissedAfter = !keptSawAfter;
                step = CloseAfter;
            } else if (inStep > 60) step = Search;
            break;
        case CloseAfter: if (!startUp || inStep > 30) step = Search; break;
        case Search:
            if (!_stricmp(front.c_str(), "SearchHost.exe") && !startUp) { searchSeen = true; }
            if (!_stricmp(front.c_str(), "SearchHost.exe") && SUCCEEDED(f.hr) && f.visible) { searchSeen = true; searchSaysStart = true; }
            if ((searchSeen && _stricmp(front.c_str(), "SearchHost.exe")) || inStep > 30) step = End;
            break;
        case End: break;
        }
        if (step != was) { stepAt = Now(); prompt(step); }
    }

    Say(false, "max IsLauncherVisible on the STA (kept object): %.1f ms", maxKeptMs);
    Say(false, "before the restart the kept object %s Start", keptSawBefore ? "saw" : "did NOT see");
    if (freshSawAfter) {
        Say(false, "after the restart a fresh object saw Start; the kept one %s", keptSawAfter ? "saw it too" : "did NOT");
        if (keptMissedAfter) {
            ++problems;
            Say(true, "VERDICT: PROBLEM - the IAppVisibility object kept from before Explorer restarted no longer sees Start: the dock's InFront() "
                      "(one object for the life of the process) then misses an open Start, and its start button no longer closes it");
        }
    } else {
        Say(false, "after the restart: Start was not opened (not checked)");
    }
    if (searchSeen)
        Say(false, "Search (Win+S) alone: IsLauncherVisible %s", searchSaysStart ? "says Start is up (a press would close Search instead of opening Start)" : "says no (as it should)");
    if (worst >= 250) {
        ++problems;
        Say(true, "VERDICT: PROBLEM - making IAppVisibility took %.0f ms on a fresh thread with Explorer idle: the worker would take Explorer for busy", worst);
    }
    if (maxKeptMs >= 250) Say(true, "NOTE: IsLauncherVisible took %.0f ms on the STA once: the dock's UI thread waited that long at a press", maxKeptMs);
    Say(true, "VERDICT: %s (%d problem%s)", problems ? "PROBLEMS FOUND" : "ok", problems, problems == 1 ? "" : "s");
    return problems;
}
