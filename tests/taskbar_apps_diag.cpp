// What a press on a taskbar app button really does, measured on Windows' own taskbar (src/taskbar_apps.cpp finds the
// buttons). It presses the button of a window of this test's own only - three times, from three starting states it
// makes itself on that window (behind the console, in front, minimized) - and records what the live test
// (tests\taskbar_apps_live_test.cpp) does not: whether Explorer got the right to the front, every way each app button
// offers to press it, the shell's flashes and activations (a shell-hook window of its own), the foreground changes
// (SetWinEventHook) and the messages that reach its window. A press Explorer acted on shows up in those; a press
// Windows refused shows a flash; a press that did nothing shows neither. Only a button counting its one window is
// pressed (never one it shares with another copy of the test); a press that may still arrive (a call that timed out)
// stops the steps after it. Like the dock, it takes the right to change the front window with a key no program acts
// on (0xE8; system.cpp, TakeForegroundRight) and hands it on to Explorer before each press. Its window keeps handling
// messages while a press is made, as the dock's windows do: a press Explorer acts on may send to it at once.
//
// Why it is kept: mod.22 stopped pressing app buttons by the accessibility default action, all they offer on a 25H2
// build, because an earlier copy of this measurement (pressing through taskbarapps::Begin, as mod.21 did)
// showed it answers S_OK and does nothing - with the dock running (taskbar hidden) and closed (taskbar shown), and
// with Explorer given the right to the front (2026-10-27). Another Windows (26H2) can be measured the same way.
//
// Arguments: [default | dock]. default (or none): presses by the accessibility default action (LegacyIAccessible
// DoDefaultAction), the press mod.21 made. dock: presses as the dock does now (taskbarapps::PressNow: Invoke, else
// Toggle, else nothing - NoWay). Whatever is in front at the start is taken as the console it was started from.
// tests\run_taskbar_apps_diag.bat [dock] builds and runs it. Exit code: 0 measured (whatever the press did), 2 not
// measured (no step had a press made).
#include "../src/common.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <appmodel.h>
#include <cstdio>
#include <cstdarg>
#include <thread>
#include "../src/taskbar_apps.cpp"

double NowSeconds() {
    static LARGE_INTEGER f{}, t0{};
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - t0.QuadPart) / double(f.QuadPart);
}

namespace {
constexpr double kWatch = 3.0;   // s after the press was answered: how long a step looks for its effect (the dock judges
                                 // after taskbarapps::kPressCheck)

HWND g_h = nullptr;          // the test's window
HWND g_original = nullptr;   // in front at the start
double g_t0 = 0;             // the current step's press
int g_msgs = 0, g_flashes = 0, g_activations = 0;

// a line with the time since the current step's press
void Out(const char* fmt, ...) {
    printf("[%7.3f] ", NowSeconds() - g_t0);
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
    fflush(stdout);
}
std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
std::wstring ExeName(DWORD pid) {
    std::wstring out;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
        CloseHandle(p);
    }
    const size_t slash = out.find_last_of(L'\\');
    return slash == std::wstring::npos ? out : out.substr(slash + 1);
}
std::string Describe(HWND w) {
    if (!w) return "(none)";
    if (w == g_h) return "THIS WINDOW";
    wchar_t cls[96] = {};
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    char buf[512];
    snprintf(buf, sizeof(buf), "%p %s pid %lu %s", (void*)w, U8(cls).c_str(), pid, U8(ExeName(pid)).c_str());
    return buf;
}

void Pump(int ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(5);
    } while (GetTickCount64() < end);
}

// As system.cpp's TakeForegroundRight, then Explorer gets the right (AppModel::StartPress), every result printed.
void GiveExplorerTheRight() {
    const DWORD me = GetCurrentProcessId();
    SetLastError(0);
    const BOOL had = AllowSetForegroundWindow(me);
    const DWORD e1 = GetLastError();
    UINT inserted = 0;
    BOOL after = -1;
    DWORD e2 = 0;
    if (!had) {
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = 0xE8;   // "unassigned": no program acts on it
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        inserted = SendInput(2, in, sizeof(INPUT));
        SetLastError(0);
        after = AllowSetForegroundWindow(me);
        e2 = GetLastError();
    }
    DWORD shell = 0;
    if (HWND s = GetShellWindow()) GetWindowThreadProcessId(s, &shell);
    SetLastError(0);
    const BOOL toShell = shell ? AllowSetForegroundWindow(shell) : FALSE;
    const DWORD e3 = GetLastError();
    Out("RIGHT  ASFW(me)=%d err %lu | key 0xE8 %s (inserted %u) ASFW(me) again=%d err %lu | ASFW(explorer %lu)=%d err %lu",
        had, e1, had ? "not needed" : "sent", inserted, after, e2, shell, toShell, e3);
}

void PrintWays(IUIAutomation* uia, IUIAutomationElement* e) {
    auto has = [&](PATTERNID id) {
        ComPtr<IUnknown> p;
        return SUCCEEDED(e->GetCurrentPattern(id, &p)) && p ? 1 : 0;
    };
    BSTR cls = nullptr, fw = nullptr;
    e->get_CurrentClassName(&cls);
    e->get_CurrentFrameworkId(&fw);
    CONTROLTYPEID ct = 0;
    e->get_CurrentControlType(&ct);
    BOOL off = FALSE, enabled = FALSE, focusable = FALSE;
    e->get_CurrentIsOffscreen(&off);
    e->get_CurrentIsEnabled(&enabled);
    e->get_CurrentIsKeyboardFocusable(&focusable);
    RECT r{};
    e->get_CurrentBoundingRectangle(&r);
    printf("         class %s fw %s controlType %d offscreen %d enabled %d focusable %d rect (%ld,%ld)-(%ld,%ld)\n",
           U8(taskbarapps::Text(cls)).c_str(), U8(taskbarapps::Text(fw)).c_str(), (int)ct, off, enabled, focusable, r.left, r.top, r.right, r.bottom);
    SysFreeString(cls);
    SysFreeString(fw);
    printf("         Invoke %d Toggle %d ExpandCollapse %d SelectionItem %d LegacyIAccessible %d\n", has(UIA_InvokePatternId),
           has(UIA_TogglePatternId), has(UIA_ExpandCollapsePatternId), has(UIA_SelectionItemPatternId), has(UIA_LegacyIAccessiblePatternId));
    ComPtr<IUIAutomationLegacyIAccessiblePattern> acc;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&acc))) && acc) {
        DWORD role = 0, state = 0;
        BSTR action = nullptr;
        acc->get_CurrentRole(&role);
        acc->get_CurrentState(&state);
        acc->get_CurrentDefaultAction(&action);
        printf("         legacy role 0x%lx state 0x%lx default action \"%s\"\n", role, state, U8(taskbarapps::Text(action)).c_str());
        SysFreeString(action);
    }
    SAFEARRAY* ids = nullptr;
    SAFEARRAY* names = nullptr;
    if (SUCCEEDED(uia->PollForPotentialSupportedPatterns(e, &ids, &names)) && names) {
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(names, 1, &lo);
        SafeArrayGetUBound(names, 1, &hi);
        std::string all;
        for (LONG i = lo; i <= hi; ++i) {
            BSTR n = nullptr;
            if (SUCCEEDED(SafeArrayGetElement(names, &i, &n)) && n) { all += (all.empty() ? "" : ", ") + U8(taskbarapps::Text(n)); SysFreeString(n); }
        }
        printf("         potential patterns: %s\n", all.c_str());
    }
    if (ids) SafeArrayDestroy(ids);
    if (names) SafeArrayDestroy(names);
    fflush(stdout);
}

// f() on an MTA thread of its own, as the dock calls into UI Automation: the apartment kept alive for the process
// (taskbarapps::KeepMta - the shared IUIAutomation outlives each thread). pump: this thread, its window's, keeps
// handling messages meanwhile (a press Explorer acts on may send to the window at once); else it only waits.
template <class F> void OnMta(F f, bool pump) {
    std::atomic<bool> finished{ false };
    std::thread t([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        taskbarapps::KeepMta();
        f();
        CoUninitialize();
        finished = true;
    });
    while (pump && !finished) Pump(5);
    t.join();
}

// fn(uia, element, id, name) for each app button on the taskbars, the given monitor's first, until it returns true:
// calls held, focus never moved (taskbarapps::Uia).
template <class Fn> void ForEachButton(HMONITOR monitor, Fn fn, bool pump = false) {
    OnMta([&] {
        std::lock_guard<std::mutex> call(taskbarapps::S().calls);
        if (IUIAutomation* uia = taskbarapps::Uia())
            taskbarapps::EachButton(uia, taskbarapps::Taskbars(monitor), [] { return false; },
                [&](const ComPtr<IUIAutomationElement>& e, std::wstring_view id, const std::wstring& name, HWND) {
                    return fn(uia, e.Get(), id, name);
                });
    }, pump);
}

// This test's own button: named by its program's path, counting its one window (not one it shares with another copy).
bool Mine(std::wstring_view id, const std::wstring& name, const std::wstring& exe) {
    return taskbarapps::Names(Lower(std::wstring(id)), L"", exe, taskbarapps::KnownFolderPath) &&
           taskbarapps::CountFits(true, taskbarapps::WindowCount(name));
}

struct Pressed {
    std::string what;    // for the PRESS line
    bool made = false;   // a press call was made (else nothing was pressed: nothing to watch)
    bool late = false;   // it may still arrive (the call timed out): no step after it
};

// The press by the accessibility default action, on this test's own button only.
Pressed PressByDefaultAction(const std::wstring& exe, HMONITOR monitor) {
    Pressed p{ "its button (counting its one window) was not found: nothing pressed" };
    ForEachButton(monitor, [&](IUIAutomation*, IUIAutomationElement* e, std::wstring_view id, const std::wstring& name) {
        if (!Mine(id, name, exe)) return false;
        ComPtr<IUIAutomationLegacyIAccessiblePattern> acc;
        HRESULT hr = e->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&acc));
        if (SUCCEEDED(hr) && !acc) hr = E_NOINTERFACE;
        char buf[160];
        if (FAILED(hr)) {
            snprintf(buf, sizeof(buf), "no default action to press by (hr %08lx): nothing pressed", (unsigned long)hr);
            p.what = buf;
            return true;
        }
        const double c0 = NowSeconds();
        hr = acc->DoDefaultAction();
        snprintf(buf, sizeof(buf), "default action: hr %08lx in %.0f ms", (unsigned long)hr, (NowSeconds() - c0) * 1000);
        p.what = buf;
        p.made = true;
        p.late = taskbarapps::MayHaveRun(hr);
        return true;
    }, true);
    return p;
}

// The press as the dock makes it now (Invoke, else Toggle, else nothing: NoWay).
Pressed PressAsTheDock(const taskbarapps::Job& job) {
    taskbarapps::Result r;
    OnMta([&] { r = taskbarapps::PressNow(job); }, true);
    char hr[24];
    snprintf(hr, sizeof(hr), ", hr %08lx)", (unsigned long)r.hr);
    Pressed p{ "the dock's way: " + U8(taskbarapps::OutcomeText(r.outcome)) + " (" + U8(r.how) + hr };
    p.made = r.outcome == taskbarapps::Outcome::Pressed || r.outcome == taskbarapps::Outcome::Unsure;
    p.late = r.outcome == taskbarapps::Outcome::Unsure;
    return p;
}

void TaskbarState() {
    HWND bar = ExplorerTaskbar();
    APPBARDATA abd{ sizeof(abd) };
    const UINT_PTR abm = SHAppBarMessage(ABM_GETSTATE, &abd);
    RECT r{};
    if (bar) GetWindowRect(bar, &r);
    BYTE alpha = 255;
    DWORD flags = 0;
    COLORREF key = 0;
    const BOOL layered = bar && (GetWindowLongPtrW(bar, GWL_EXSTYLE) & WS_EX_LAYERED) && GetLayeredWindowAttributes(bar, &key, &alpha, &flags);
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    const int region = bar ? GetWindowRgn(bar, rgn) : ERROR;
    DeleteObject(rgn);
    printf("TASKBAR explorer %s visible %d rect (%ld,%ld)-(%ld,%ld) ABM state 0x%llx layered alpha %s region %s | dock running %d\n",
           Describe(bar).c_str(), bar && IsWindowVisible(bar) ? 1 : 0, r.left, r.top, r.right, r.bottom, (unsigned long long)abm,
           layered ? std::to_string(alpha).c_str() : "-", region == ERROR ? "none" : region == NULLREGION ? "NULL (empty)" : "set",
           FindWindowW(L"LiquidDockMain", nullptr) ? 1 : 0);
    for (HWND root : taskbarapps::Roots(bar)) {
        if (root == bar) continue;
        wchar_t cls[96] = {};
        GetClassNameW(root, cls, ARRAYSIZE(cls));
        printf("        island %p %s visible %d\n", (void*)root, U8(cls).c_str(), IsWindowVisible(root) ? 1 : 0);
    }
    fflush(stdout);
}

const char* Hshell(WPARAM code) {
    switch (code & 0xFFFF) {
    case HSHELL_WINDOWCREATED: return "WINDOWCREATED";
    case HSHELL_WINDOWDESTROYED: return "WINDOWDESTROYED";
    case HSHELL_WINDOWACTIVATED: return "WINDOWACTIVATED";
    case HSHELL_RUDEAPPACTIVATED: return "RUDEAPPACTIVATED";
    case HSHELL_FLASH: return "FLASH";
    case HSHELL_REDRAW: return "REDRAW";
    case HSHELL_WINDOWREPLACED: return "WINDOWREPLACED";
    case HSHELL_MONITORCHANGED: return "MONITORCHANGED";
    }
    return nullptr;
}

UINT g_shellMsg = 0;
LRESULT CALLBACK HookProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_shellMsg && m == g_shellMsg) {
        const char* what = Hshell(w);
        HWND target = reinterpret_cast<HWND>(l);
        const bool activation = (w & 0x7FFF) == HSHELL_WINDOWACTIVATED;
        if (what && (target == g_h || activation)) {
            if (target == g_h && (w & 0xFFFF) == HSHELL_FLASH) ++g_flashes;
            Out("SHELL  %s %s", what, Describe(target).c_str());
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void CALLBACK WinEvent(HWINEVENTHOOK, DWORD ev, HWND w, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW) return;
    if (ev == EVENT_SYSTEM_FOREGROUND) { if (w == g_h) ++g_activations; Out("EVENT  foreground -> %s", Describe(w).c_str()); }
    else if (w == g_h && ev == EVENT_SYSTEM_MINIMIZESTART) Out("EVENT  THIS WINDOW minimize start");
    else if (w == g_h && ev == EVENT_SYSTEM_MINIMIZEEND) Out("EVENT  THIS WINDOW minimize end");
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(40, 120, 200));
        FillRect(dc, &r, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE: ++g_msgs; Out("MSG    WM_ACTIVATE %s (other %s)", LOWORD(w) == WA_INACTIVE ? "inactive" : LOWORD(w) == WA_CLICKACTIVE ? "clickactive" : "active", Describe((HWND)l).c_str()); break;
    case WM_NCACTIVATE: ++g_msgs; Out("MSG    WM_NCACTIVATE %d", (int)w); break;
    case WM_ACTIVATEAPP: ++g_msgs; Out("MSG    WM_ACTIVATEAPP %d (thread %lu)", (int)w, (unsigned long)l); break;
    case WM_SYSCOMMAND: ++g_msgs; Out("MSG    WM_SYSCOMMAND 0x%04x", (unsigned)(w & 0xFFF0)); break;
    case WM_SHOWWINDOW: ++g_msgs; Out("MSG    WM_SHOWWINDOW %d status %d", (int)w, (int)l); break;
    case WM_QUERYOPEN: ++g_msgs; Out("MSG    WM_QUERYOPEN"); break;
    case WM_SIZE: if (w == SIZE_MINIMIZED || w == SIZE_RESTORED) { ++g_msgs; Out("MSG    WM_SIZE %s", w == SIZE_MINIMIZED ? "minimized" : "restored"); } break;
    }
    return DefWindowProcW(h, m, w, l);
}
}

int wmain(int argc, wchar_t** argv) {
    const bool dockWay = argc > 1 && !_wcsicmp(argv[1], L"dock");
    g_original = GetForegroundWindow();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_t0 = NowSeconds();
    printf("DIAG pressing by %s\n", dockWay ? "the dock's way (Invoke, else Toggle, else nothing)" : "the accessibility default action");
    UINT lockTimeout = 0;
    SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &lockTimeout, 0);
    LASTINPUTINFO li{ sizeof(li) };
    GetLastInputInfo(&li);
    printf("START  front: %s | lock timeout %u ms | since last input %lu ms\n", Describe(g_original).c_str(), lockTimeout,
           (unsigned long)(GetTickCount() - li.dwTime));
    if (!g_original) { puts("nothing in front: nothing measured"); return 2; }
    TaskbarState();
    puts("ALL APP BUTTONS (reads only):");
    ForEachButton(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY),
                  [](IUIAutomation* uia, IUIAutomationElement* e, std::wstring_view id, const std::wstring& name) {
                      printf("  BUTTON \"%s\" name \"%s\"\n", U8(std::wstring(id)).c_str(), U8(name).c_str());
                      PrintWays(uia, e);
                      return false;
                  });

    WNDCLASSW hc{};
    hc.hInstance = GetModuleHandleW(nullptr);
    hc.lpfnWndProc = HookProc;
    hc.lpszClassName = L"LiquidDockTaskbarDiagHook";
    RegisterClassW(&hc);
    HWND hook = CreateWindowExW(WS_EX_TOOLWINDOW, hc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hc.hInstance, nullptr);
    g_shellMsg = RegisterWindowMessageW(L"SHELLHOOK");
    const BOOL hooked = hook && RegisterShellHookWindow(hook);
    HWINEVENTHOOK events = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, WinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    printf("WATCH  shell hook %d, win events %d\n", hooked, events ? 1 : 0);

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = L"LiquidDockTaskbarButtonTest";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_h = CreateWindowExW(0, wc.lpszClassName, L"Liquid Dock - taskbar button diag", WS_OVERLAPPEDWINDOW, 200, 200, 480, 300, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_h) { puts("Window creation failed: nothing measured"); return 2; }
    ShowWindow(g_h, SW_SHOWNOACTIVATE);
    Pump(300);
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    taskbarapps::Job job;
    job.exe = Lower(self);
    job.monitor = MonitorFromWindow(g_h, MONITOR_DEFAULTTONEAREST);
    // its button: Explorer adds it a moment after the window shows
    std::wstring seen;
    for (int i = 0; i < 30 && seen.empty(); ++i) {
        ForEachButton(job.monitor, [&](IUIAutomation* uia, IUIAutomationElement* e, std::wstring_view id, const std::wstring& name) {
            if (!Mine(id, name, job.exe)) return false;
            seen = std::wstring(id);
            printf("OWN BUTTON \"%s\" name \"%s\"\n", U8(seen).c_str(), U8(name).c_str());
            PrintWays(uia, e);
            return true;
        }, true);   // its window is up: Explorer may ask it something (its icon, its title) while the buttons are read
        if (seen.empty()) Pump(100);
    }
    if (seen.empty()) { puts("no button of its own counting its one window (another copy running?): nothing measured"); DestroyWindow(g_h); return 2; }

    // start: the window's state the step makes first, on its own window - 0 behind the console, 1 in front, 2 minimized
    struct Step { const char* name; const char* expect; int start; };
    const Step steps[] = { { "A behind", "comes to the front", 0 },
                           { "B in front", "goes down (minimized)", 1 },
                           { "C minimized", "comes back (restored, in front)", 2 } };
    uint32_t id = 0;
    int measured = 0, done = 0, reached = 0, unpressed = 0;
    bool late = false;   // a press that may still arrive: no step after it
    for (const auto& s : steps) {
        if (late) break;
        g_t0 = NowSeconds();
        printf("\n==== STEP %s: a click on its taskbar button %s\n", s.name, s.expect);
        if (s.start == 0) {
            if (IsIconic(g_h)) ShowWindow(g_h, SW_SHOWNOACTIVATE);
            if (GetForegroundWindow() == g_h && IsWindow(g_original)) SetForegroundWindow(g_original);
        } else if (s.start == 1) {
            if (IsIconic(g_h)) ShowWindow(g_h, SW_RESTORE);
            if (!SetForegroundWindow(g_h)) { GiveExplorerTheRight(); SetForegroundWindow(g_h); }
        } else {
            ShowWindow(g_h, SW_MINIMIZE);
        }
        Pump(500);
        const bool front = GetForegroundWindow() == g_h, iconic = IsIconic(g_h) != FALSE;
        const bool ready = s.start == 0 ? !front && !iconic : s.start == 1 ? front && !iconic : iconic && !front;
        Out("BEFORE front: %s | minimized %d -> starting state %s", Describe(GetForegroundWindow()).c_str(), iconic ? 1 : 0, ready ? "ok" : "NOT MET");
        if (!ready) { puts("SKIPPED (its starting state could not be made)"); continue; }
        g_msgs = g_flashes = g_activations = 0;
        g_t0 = NowSeconds();
        GiveExplorerTheRight();
        job.id = ++id;
        job.wake = nullptr;
        job.stage = std::make_shared<std::atomic<int>>(taskbarapps::kLooking);
        const Pressed press = dockWay ? PressAsTheDock(job) : PressByDefaultAction(job.exe, job.monitor);
        const double answered = NowSeconds();
        Out("PRESS  %s", press.what.c_str());
        if (!press.made) {
            Out("NOT PRESSED: nothing to watch - this step is not counted");
            ++unpressed;
            Pump(300);
            continue;
        }
        bool ok = false;
        while (!ok && NowSeconds() - answered < kWatch) {
            Pump(5);
            const bool f = GetForegroundWindow() == g_h, ic = IsIconic(g_h) != FALSE;
            ok = s.start == 1 ? ic : !ic && f;
        }
        const double reaction = NowSeconds() - answered;
        Pump(300);   // late messages
        Out("AFTER  %s %.0f ms after the press was answered (looking it up and pressing took %.0f ms) | front: %s | minimized %d | "
            "messages to it %d, flashes %d, became foreground %d", ok ? "DONE" : "NOT done", reaction * 1000, (answered - g_t0) * 1000,
            Describe(GetForegroundWindow()).c_str(), IsIconic(g_h) ? 1 : 0, g_msgs, g_flashes, g_activations);
        if (!ok && s.start == 2 && !IsIconic(g_h)) Out("NOTE   restored but not in front (activation refused?)");
        ++measured;
        done += ok;
        reached += g_msgs > 0 || g_flashes > 0 || g_activations > 0;
        if (press.late) {
            Out("UNSURE: this press may still arrive - the other steps are not run");
            late = true;
        }
        Pump(300);
    }
    if (events) UnhookWinEvent(events);
    if (hooked) DeregisterShellHookWindow(hook);
    DestroyWindow(g_h);
    if (hook) DestroyWindow(hook);
    Pump(200);
    if (IsWindow(g_original)) SetForegroundWindow(g_original);
    printf("\nVERDICT pressing by %s: did what a click does in %d of %d pressed steps; in %d of them its window got a message, a flash or "
           "the front; %d step(s) had no press made\n", dockWay ? "the dock's way" : "the default action", done, measured, reached, unpressed);
    return measured ? 0 : 2;
}
