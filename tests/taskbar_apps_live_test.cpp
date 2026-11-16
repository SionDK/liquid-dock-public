// The app buttons on Windows' own taskbar, for real (src/taskbar_apps.cpp, taskbar_apps_logic.h):
//   1. reads only: every app button (id, name), and which of the running apps' windows the dock would find a
//      button for - the same way it looks when an app is clicked;
//   2. presses the button of a window of this test's own, three times, as a click on the dock would: it comes
//      to the front, goes down, comes back. Each step makes its own starting state on that window (behind the
//      console, in front, minimized), so a step that fails does not fail the ones after it (a press that may still
//      arrive stops the steps). A Windows whose app buttons offer no press that works (a 25H2 build: only
//      the accessibility default action, measured to do nothing) is told apart: nothing is pressed, the window is
//      left as it was, the dock goes its own way.
// No other program's window is pressed, moved or activated. Like the dock, the test takes the right to change
// the front window with a key no program acts on (0xE8; system.cpp, TakeForegroundRight) and hands it on to
// Explorer before a press (and says whether it could). tests\run_taskbar_apps_live.bat builds and runs it: keep
// its window in front. Arguments: "front" (whatever is in front now: the console it was started from) or the
// process id of the window the caller saw in front.
#include "../src/common.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <appmodel.h>
#include <cstdio>
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
int checks = 0, failures = 0;
void Check(bool ok, const char* what) { ++checks; if (!ok) ++failures; printf("%s: %s\n", ok ? "PASS" : "FAIL", what); fflush(stdout); }

std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

void Pump(int ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    do {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    } while (GetTickCount64() < end);
}

// as system.cpp's TakeForegroundRight, then Explorer gets the right (as AppModel::StartPress does, Explorer last)
void LetExplorerToFront() {
    const DWORD me = GetCurrentProcessId();
    const BOOL had = AllowSetForegroundWindow(me);
    BOOL taken = had;
    if (!had) {
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = 0xE8;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        taken = AllowSetForegroundWindow(me);
    }
    DWORD shell = 0;
    if (HWND s = GetShellWindow()) GetWindowThreadProcessId(s, &shell);
    const BOOL given = shell && AllowSetForegroundWindow(shell);
    printf("RIGHT to the front: this test %s; Explorer (%lu) %s\n", had ? "had it" : taken ? "took it with an unassigned key" : "has it NOT",
           (unsigned long)shell, given ? "given it" : "NOT given it");
}

std::wstring ExeOf(DWORD pid) {
    std::wstring out;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
        CloseHandle(p);
    }
    return Lower(out);
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
// The windows the taskbar shows (the dock's own filter, apps.cpp IsTaskbarWindow, in short).
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

// On an MTA thread of its own, as in the dock (the calling thread keeps pumping its window's messages).
std::vector<taskbarapps::Button> ListButtons(double* took) {
    std::vector<taskbarapps::Button> out;
    std::thread([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        out = taskbarapps::List(took);
        CoUninitialize();
    }).join();
    return out;
}

// The ways the button naming exe offers to press it (reads only), on an MTA thread as in the dock.
void PrintWays(const std::wstring& exe, HMONITOR monitor) {
    std::thread([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            std::lock_guard<std::mutex> call(taskbarapps::S().calls);
            if (IUIAutomation* uia = taskbarapps::Uia())
                taskbarapps::EachButton(uia, taskbarapps::Taskbars(monitor), [] { return false; },
                    [&](const ComPtr<IUIAutomationElement>& e, std::wstring_view id, const std::wstring&, HWND) {
                        if (!taskbarapps::Names(id, L"", exe, taskbarapps::KnownFolderPath)) return false;
                        auto has = [&](PATTERNID p) { ComPtr<IUnknown> u; return SUCCEEDED(e->GetCurrentPattern(p, &u)) && u ? 1 : 0; };
                        printf("WAYS to press its button: Invoke %d, Toggle %d, ExpandCollapse %d, SelectionItem %d, LegacyIAccessible %d\n",
                               has(UIA_InvokePatternId), has(UIA_TogglePatternId), has(UIA_ExpandCollapsePatternId), has(UIA_SelectionItemPatternId),
                               has(UIA_LegacyIAccessiblePatternId));
                        return true;
                    });
        }
        CoUninitialize();
    }).join();
}

taskbarapps::Result Press(const taskbarapps::Job& job) {
    taskbarapps::Result r;
    if (!taskbarapps::Begin(job)) { r.how = L"no thread"; return r; }
    const ULONGLONG end = GetTickCount64() + 6000;
    bool got = false;
    while (!(got = taskbarapps::Take(job.id, r)) && GetTickCount64() < end) Pump(15);
    if (!got) {   // as AppModel::PollPresses: no press under way is given up on; one under way may still arrive
        r.how = L"no answer in 6 s";
        if (!taskbarapps::Abandon(*job.stage)) r.outcome = taskbarapps::Outcome::Unsure;
    }
    return r;
}

const char* Kind(HWND bar) { return bar == ExplorerTaskbar() ? "main" : "secondary"; }

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
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
    return DefWindowProcW(h, m, w, l);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    HWND original = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(original, &pid);
    const bool front = !wcscmp(argv[1], L"front");
    if (!original || (!front && pid != wcstoul(argv[1], nullptr, 10))) { puts("Foreground changed; nothing done."); return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ---- 1. what the taskbar shows, and what the dock would find there (reads only)
    double took = 0;
    std::vector<taskbarapps::Button> buttons = ListButtons(&took);
    printf("BUTTONS %zu app buttons in %.0f ms (classic taskbar: %d)\n", buttons.size(), took * 1000, taskbarapps::Classic() ? 1 : 0);
    for (const auto& b : buttons)
        printf("BUTTON %s taskbar: id \"%s\" name \"%s\" windows %d\n", Kind(b.taskbar), U8(b.id).c_str(), U8(b.name).c_str(), taskbarapps::WindowCount(b.name));
    Check(!buttons.empty(), "the taskbar's app buttons are found (the dock hides the taskbar: from its XAML island)");
    std::vector<HWND> windows;
    EnumWindows([](HWND h, LPARAM p) -> BOOL { if (OnTaskbar(h)) reinterpret_cast<std::vector<HWND>*>(p)->push_back(h); return TRUE; },
                reinterpret_cast<LPARAM>(&windows));
    int mapped = 0;
    for (HWND h : windows) {
        DWORD wp = 0;
        GetWindowThreadProcessId(h, &wp);
        std::wstring exe = ExeOf(wp), aumid = WindowAumid(h);
        if (aumid.empty()) aumid = ProcessAumid(wp);
        if (exe.size() > 26 && exe.compare(exe.size() - 26, 26, L"\\applicationframehost.exe") == 0) {   // a store app: its own process is the child's
            struct Ctx { DWORD host, found; } c{ wp, 0 };
            EnumChildWindows(h, [](HWND ch, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(lp);
                DWORD p = 0;
                GetWindowThreadProcessId(ch, &p);
                if (p != c->host) { c->found = p; return FALSE; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&c));
            if (c.found) { exe = ExeOf(c.found); if (aumid.empty()) aumid = ProcessAumid(c.found); }
        }
        const taskbarapps::Button* match = nullptr;
        for (const auto& b : buttons)
            if (taskbarapps::Names(b.id, aumid, exe, taskbarapps::KnownFolderPath)) { match = &b; break; }
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, ARRAYSIZE(cls));
        const size_t slash = exe.find_last_of(L'\\');
        printf("WINDOW %s (%s) aumid \"%s\" -> %s\n", U8(slash == std::wstring::npos ? exe : exe.substr(slash + 1)).c_str(), U8(cls).c_str(),
               U8(aumid).c_str(), match ? ("button \"" + U8(match->id) + "\"").c_str() : "no button (the dock's own way)");
        mapped += match != nullptr;
    }
    printf("WINDOWS %zu on the taskbar, %d with a button the dock finds\n", windows.size(), mapped);

    // ---- 2. this test's own window: its button pressed three times
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = L"LiquidDockTaskbarButtonTest";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Liquid Dock - taskbar button test", WS_OVERLAPPEDWINDOW, 200, 200, 480, 300, nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) { puts("Window creation failed"); return 2; }
    ShowWindow(h, SW_SHOWNOACTIVATE);
    Pump(300);
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    taskbarapps::Job job;
    job.exe = Lower(self);
    job.monitor = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    // its button: Explorer adds it a moment after the window shows
    std::wstring seen, seenName;
    for (int i = 0; i < 30 && seen.empty(); ++i) {
        for (const auto& b : ListButtons(nullptr))
            if (taskbarapps::Names(b.id, L"", job.exe, taskbarapps::KnownFolderPath)) { seen = b.id; seenName = b.name; break; }
        if (seen.empty()) Pump(100);
    }
    printf("OWN window's button: \"%s\" name \"%s\" windows %d\n", U8(seen).c_str(), U8(seenName).c_str(), taskbarapps::WindowCount(seenName));
    Check(!seen.empty(), "this test's window gets a button the dock finds (by its program's path)");
    Check(seen.empty() || taskbarapps::CountFits(true, taskbarapps::WindowCount(seenName)),
          "...its name counts its one window (else the dock would take it for another app's button and not press it)");
    if (!seen.empty()) PrintWays(job.exe, job.monitor);
    // start: the window's state the step makes first, on its own window - 0 behind the console, 1 in front, 2 minimized
    struct Step { const char* what; int start; };
    const Step steps[] = { { "a window behind the console comes to the front", 0 },
                           { "the window in front goes down", 1 },
                           { "the minimized window comes back to the front", 2 } };
    uint32_t id = 0;
    bool noWay = false, late = false;   // late: a press that may still arrive (no step, nor the check below, may meet it)
    for (const auto& s : steps) {
        if (seen.empty() || noWay || late) break;
        if (s.start == 0) {
            if (IsIconic(h)) ShowWindow(h, SW_SHOWNOACTIVATE);
            if (GetForegroundWindow() == h && IsWindow(original)) SetForegroundWindow(original);
        } else if (s.start == 1) {
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            if (!SetForegroundWindow(h)) { LetExplorerToFront(); SetForegroundWindow(h); }
        } else {
            ShowWindow(h, SW_MINIMIZE);
        }
        Pump(400);
        const bool startFront = GetForegroundWindow() == h, startDown = IsIconic(h) != FALSE;
        // minimized and no longer the active window, as the dock sends it to the taskbar (Why::Minimized)
        if (s.start == 0 ? startFront || startDown : s.start == 1 ? !startFront || startDown : !startDown || startFront) {
            printf("SKIPPED %s: its starting state could not be made (front: %s, minimized %d)\n", s.what,
                   startFront ? "this window" : "another", startDown ? 1 : 0);
            Check(false, "the step's starting state is made");
            continue;
        }
        LetExplorerToFront();
        job.id = ++id;
        job.wake = nullptr;
        job.stage = std::make_shared<std::atomic<int>>(taskbarapps::kLooking);   // one per press, as the dock makes one per click
        const double t0 = NowSeconds();
        const taskbarapps::Result r = Press(job);
        printf("PRESS %s: %s (%s, hr %08lx; button \"%s\" named \"%s\", found in %.0f ms among %d, counting %d windows, pressed in %.0f ms)\n",
               s.what, U8(taskbarapps::OutcomeText(r.outcome)).c_str(), U8(r.how).c_str(), (unsigned long)r.hr, U8(r.button).c_str(),
               U8(r.name).c_str(), r.lookup * 1000, r.buttons, r.count, r.took * 1000);
        if (r.outcome == taskbarapps::Outcome::NoWay) {   // the dock goes its own way for the run: nothing more to press here
            Pump(1500);
            const bool still = (GetForegroundWindow() == h) == startFront && (IsIconic(h) != FALSE) == startDown;
            puts("NO WAY: this Windows' app buttons offer no press that works - the dock goes its own way; the other steps are not run");
            Check(still, "a button with no press that works is not pressed: the window is left as it was");
            noWay = true;
            continue;
        }
        Check(r.outcome == taskbarapps::Outcome::Pressed, "the press goes through");
        bool ok = false;
        double at = 0;
        for (int i = 0; i < 100 && !ok; ++i) {   // up to taskbarapps::kPressCheck and a bit
            Pump(15);
            const bool inFront = GetForegroundWindow() == h, down = IsIconic(h) != FALSE;
            ok = s.start == 1 ? down : !down && inFront;
            at = NowSeconds() - t0;
        }
        printf("AFTER %s: %s in %.0f ms (front: %s, minimized %d)\n", s.what, ok ? "done" : "NOT done", at * 1000,
               GetForegroundWindow() == h ? "this window" : "another", IsIconic(h) ? 1 : 0);
        Check(ok, s.what);
        if (r.outcome == taskbarapps::Outcome::Unsure) {
            puts("UNSURE: this press may still reach Explorer - the other steps and the check below are not run");
            late = true;
        }
        Pump(400);
    }
    if (!seen.empty() && !late) {   // a press the dock gave up waiting for (Explorer slow) never comes later
        LetExplorerToFront();
        const bool wasFront = GetForegroundWindow() == h, wasDown = IsIconic(h) != FALSE;
        job.id = ++id;
        job.stage = std::make_shared<std::atomic<int>>(taskbarapps::kAbandoned);
        taskbarapps::Result r;
        const bool begun = taskbarapps::Begin(job);
        bool answered = false;
        for (int i = 0; i < 40 && !answered; ++i) { Pump(15); answered = taskbarapps::Take(job.id, r); }
        Pump(600);
        const bool still = (GetForegroundWindow() == h) == wasFront && (IsIconic(h) != FALSE) == wasDown;
        printf("ABANDONED job: begun %d, a result %d, window unchanged %d\n", begun ? 1 : 0, answered ? 1 : 0, still ? 1 : 0);
        Check(begun && !answered && still, "a job the dock gave up on presses nothing and gives no result");
    }
    DestroyWindow(h);
    Pump(200);
    if (IsWindow(original)) SetForegroundWindow(original);
    printf("RESULT taskbar_apps_live checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
