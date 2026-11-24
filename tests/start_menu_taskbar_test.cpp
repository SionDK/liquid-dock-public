// The taskbar's Start button as the dock finds, checks and presses it (src/taskbar_start.cpp), against a
// "taskbar" of the test's own: a window with a child that UI Automation sees as a button with AutomationId
// "StartButton" and a Toggle pattern (a provider of the test's own), as Explorer's XAML Start button is. The
// real lookup, the check of a kept button, Explorer rebuilding its taskbar (the button replaced, or gone), the
// wait before looking again, the press and what its failures mean, the taskbar's vanishing (Remember) run here;
// Explorer's own taskbar is never looked into or pressed (only Classic() reads its window, as the dock does).
// The windows are the test's own, invisible (alpha 0, never activated), in a corner of the primary monitor.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\start_menu_taskbar_test.cpp /link user32.lib ole32.lib oleaut32.lib uuid.lib uiautomationcore.lib
#include "../src/common.h"
#include "../src/taskbar_start.h"
#include <uiautomation.h>
#include <atomic>
#include <mutex>
#include <thread>

// ---- Explorer's taskbars as taskbarstart::Press finds them (TaskbarOn, ExplorerTaskbar, Classic): as they are -
// what every test but PressTests sees - unless g_ownBars: then the test's own window (g_ownBar) is "Explorer's main
// taskbar" on a made-up primary monitor (kOwnPrimary, never looked into), and there is no other taskbar nor a classic
// tray: Press can reach no taskbar but the test's own.
static std::atomic<bool> g_ownBars{ false };
static HWND g_ownBar = nullptr;
static const HMONITOR kOwnPrimary = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x7e570002));
HWND WINAPI OwnShellWindow() { return g_ownBars ? nullptr : GetShellWindow(); }
HWND WINAPI OwnFindWindowW(LPCWSTR cls, LPCWSTR name) {
    if (!g_ownBars) return FindWindowW(cls, name);
    return cls && !wcscmp(cls, L"Shell_TrayWnd") ? g_ownBar : nullptr;
}
HWND WINAPI OwnFindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR name) {
    return g_ownBars ? nullptr : FindWindowExW(parent, after, cls, name);   // no other taskbar; the test's has no TrayNotifyWnd
}
HMONITOR WINAPI OwnMonitorFromPoint(POINT p, DWORD flags) { return g_ownBars ? kOwnPrimary : MonitorFromPoint(p, flags); }
#define GetShellWindow OwnShellWindow
#define FindWindowW OwnFindWindowW
#define FindWindowExW OwnFindWindowExW
#define MonitorFromPoint OwnMonitorFromPoint
#include "../src/taskbar_start.cpp"
#undef GetShellWindow
#undef FindWindowW
#undef FindWindowExW
#undef MonitorFromPoint
#include <cstdarg>
#include <cstdio>
#include <type_traits>

// ---- what taskbar_start.cpp needs from the rest of the dock
static std::mutex g_logMu;
static std::vector<std::wstring> g_log;
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_logMu);
    g_log.push_back(buf);
}
// g_virtual: PressTests' clock, moved only by the provider below (a slow read or press is a step of it, taken at once)
static std::atomic<bool> g_virtual{ false };
static std::atomic<int64_t> g_virtUs{ 0 };
double NowSeconds() {
    if (g_virtual) return g_virtUs.load() / 1e6;
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

// ---- checks
#include "portable_check.h"
static const bool kFlushEachCheck = (portable_check::FlushEach() = true);
static size_t LogSize() { std::lock_guard<std::mutex> lk(g_logMu); return g_log.size(); }
static bool LogHas(size_t from, const wchar_t* text) {
    std::lock_guard<std::mutex> lk(g_logMu);
    for (size_t i = from; i < g_log.size(); ++i) if (g_log[i].find(text) != std::wstring::npos) return true;
    return false;
}

// ---- the button's provider: AutomationId "StartButton", a Toggle pattern that counts its presses
// and, when asked (patterns), an Invoke pattern and a LegacyIAccessible one that count theirs: a button
// pressed "only when off" must be pressed by its Toggle alone, never by what a Toggle-less button offers
constexpr int kToggle = 1, kInvoke = 2, kLegacy = 4;
struct StartProvider : IRawElementProviderSimple, IToggleProvider, IInvokeProvider, ILegacyIAccessibleProvider {
    HWND hwnd = nullptr;
    std::atomic<long> refs{ 1 };
    std::atomic<int> toggles{ 0 };
    std::atomic<HRESULT> toggleHr{ S_OK };
    std::atomic<DWORD> toggleSleep{ 0 };
    std::atomic<int> patterns{ kToggle };   // the patterns it offers (Explorer's XAML Start button: Toggle)
    std::atomic<int> invokes{ 0 }, defaultActions{ 0 };
    std::atomic<bool> idGone{ false };      // its AutomationId no longer "StartButton": a lookup finds nothing
    std::atomic<bool> goneOnRead{ false };  // the next read of its state answers "not available" (once)...
    std::atomic<bool> goneTakesId{ false }; // ...and with it the id goes (the button not to be found again)
    // IUnknown (the IRawElementProviderSimple base is the identity)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(IToggleProvider)) *out = static_cast<IToggleProvider*>(this);
        else if (iid == __uuidof(IInvokeProvider)) *out = static_cast<IInvokeProvider*>(this);
        else if (iid == __uuidof(ILegacyIAccessibleProvider)) *out = static_cast<ILegacyIAccessibleProvider*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs); }
    ULONG STDMETHODCALLTYPE Release() override { const long r = --refs; if (!r) delete this; return static_cast<ULONG>(r); }
    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* o) override { *o = ProviderOptions_ServerSideProvider; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** out) override {
        *out = nullptr;
        const int has = patterns.load();
        if (id == UIA_TogglePatternId && (has & kToggle)) { *out = static_cast<IToggleProvider*>(this); AddRef(); }
        if (id == UIA_InvokePatternId && (has & kInvoke)) { *out = static_cast<IInvokeProvider*>(this); AddRef(); }
        if (id == UIA_LegacyIAccessiblePatternId && (has & kLegacy)) { *out = static_cast<ILegacyIAccessibleProvider*>(this); AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override {
        VariantInit(v);
        if (id == UIA_AutomationIdPropertyId) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(idGone ? L"NotTheStartButton" : L"StartButton"); }
        else if (id == UIA_ControlTypePropertyId) { v->vt = VT_I4; v->lVal = UIA_ButtonControlTypeId; }
        else if (id == UIA_NamePropertyId) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(L"Start"); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override { return UiaHostProviderFromHwnd(hwnd, out); }
    // IToggleProvider
    std::atomic<int> state{ ToggleState_Off };   // flipped by a press that goes through, as a XAML ToggleButton's
    std::atomic<int> stateReads{ 0 };
    std::atomic<HRESULT> stateHr{ S_OK };
    // (PressTests' virtual clock) the press takes toggleTakesUs; the next read of the state takes readTakesUs (once);
    // the first read after a press that went through takes afterReadTakesUs (once)
    std::atomic<int64_t> toggleTakesUs{ 0 }, readTakesUs{ 0 }, afterReadTakesUs{ 0 };
    HRESULT STDMETHODCALLTYPE Toggle() override {
        ++toggles;
        g_virtUs += toggleTakesUs.load();
        if (SUCCEEDED(toggleHr)) if (const int64_t us = afterReadTakesUs.exchange(0)) readTakesUs = us;
        if (toggleSleep) Sleep(toggleSleep);
        if (SUCCEEDED(toggleHr)) state = state == ToggleState_On ? ToggleState_Off : ToggleState_On;
        return toggleHr;
    }
    std::atomic<DWORD> stateSleepOnce{ 0 };   // the next read waits this long (Explorer busy), once
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* s) override {
        ++stateReads;
        if (const DWORD ms = stateSleepOnce.exchange(0)) Sleep(ms);
        g_virtUs += readTakesUs.exchange(0);
        *s = static_cast<ToggleState>(state.load());
        if (goneOnRead.exchange(false)) { if (goneTakesId) idGone = true; return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE); }
        return stateHr;
    }
    // IInvokeProvider: counted, as a click would be
    HRESULT STDMETHODCALLTYPE Invoke() override { ++invokes; return S_OK; }
    // ILegacyIAccessibleProvider: the default action counted; the rest answers as a plain button
    HRESULT STDMETHODCALLTYPE Select(long) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DoDefaultAction() override { ++defaultActions; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIAccessible(IAccessible** a) override { *a = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ChildId(int* id) override { *id = CHILDID_SELF; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Name(BSTR* s) override { *s = SysAllocString(L"Start"); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* s) override { *s = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Description(BSTR* s) override { *s = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Role(DWORD* r) override { *r = ROLE_SYSTEM_PUSHBUTTON; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_State(DWORD* st) override { *st = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Help(BSTR* s) override { *s = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_KeyboardShortcut(BSTR* s) override { *s = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** a) override { *a = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_DefaultAction(BSTR* s) override { *s = SysAllocString(L"Press"); return S_OK; }
};

// ---- the test's taskbar: a top-level window and its button, on a thread of their own that pumps messages
// (UI Automation reaches the provider through the button window's thread)
constexpr UINT kMake = WM_APP + 1, kRemove = WM_APP + 2, kDestroyBar = WM_APP + 3, kMakeBar = WM_APP + 4;
static HWND g_bar = nullptr, g_button = nullptr;
static StartProvider* g_provider = nullptr;
static std::atomic<int> g_asked{ 0 };   // UI Automation asked the button window for its provider
static DWORD g_uiThread = 0;
static HANDLE g_done = nullptr;

static LRESULT CALLBACK ButtonProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_GETOBJECT && static_cast<long>(l) == UiaRootObjectId && g_provider && h == g_provider->hwnd) {
        ++g_asked;
        return UiaReturnRawElementProvider(h, w, l, g_provider);
    }
    return DefWindowProcW(h, m, w, l);
}
static void MakeButton() {   // a new button (Explorer rebuilt its taskbar's content in the same window)
    g_button = CreateWindowExW(0, L"hunt-start_menu-button", L"Start", WS_CHILD | WS_VISIBLE, 0, 0, 20, 20, g_bar, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto* p = new StartProvider;
    p->hwnd = g_button;
    g_provider = p;
}
static void RemoveButton() {
    if (g_provider) { UiaDisconnectProvider(g_provider); g_provider->Release(); g_provider = nullptr; }
    if (g_button) { DestroyWindow(g_button); g_button = nullptr; }
}
static void UiThread() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"hunt-start_menu-taskbar";
    RegisterClassW(&wc);
    wc.lpfnWndProc = ButtonProc;
    wc.lpszClassName = L"hunt-start_menu-button";
    RegisterClassW(&wc);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    // invisible to the user (alpha 0, clicks go through), never activated, wholly on the primary monitor
    g_bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, L"hunt-start_menu-taskbar", L"hunt-start_menu taskbar", WS_POPUP,
                            mi.rcMonitor.left, mi.rcMonitor.top, 40, 20, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(g_bar, 0, 0, LWA_ALPHA);
    ShowWindow(g_bar, SW_SHOWNOACTIVATE);
    MakeButton();
    SetEvent(g_done);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!msg.hwnd && msg.message == kMake) { MakeButton(); SetEvent(g_done); continue; }
        if (!msg.hwnd && msg.message == kRemove) { RemoveButton(); SetEvent(g_done); continue; }
        if (!msg.hwnd && msg.message == kDestroyBar) { RemoveButton(); DestroyWindow(g_bar); SetEvent(g_done); continue; }
        if (!msg.hwnd && msg.message == kMakeBar) {   // a taskbar of the test's own again, as the first: invisible, never activated
            g_bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, L"hunt-start_menu-taskbar", L"hunt-start_menu taskbar", WS_POPUP,
                                    mi.rcMonitor.left, mi.rcMonitor.top, 40, 20, nullptr, nullptr, wc.hInstance, nullptr);
            SetLayeredWindowAttributes(g_bar, 0, 0, LWA_ALPHA);
            ShowWindow(g_bar, SW_SHOWNOACTIVATE);
            MakeButton();
            SetEvent(g_done);
            continue;
        }
        DispatchMessageW(&msg);
    }
    CoUninitialize();
}
static void OnUi(UINT what) { PostThreadMessageW(g_uiThread, what, 0, 0); WaitForSingleObject(g_done, 5000); }

static taskbarstart::Entry Copy(HWND bar) {
    std::lock_guard<std::mutex> lk(taskbarstart::S().book);
    taskbarstart::Entry e = taskbarstart::At(bar);
    return e;
}

static void PureTests() {
    using namespace taskbarstart;
    // growing the book moves its entries: a copy would release elements (calls into Explorer) under the book
    static_assert(std::is_nothrow_move_constructible_v<Entry>, "the book's entries move without a copy");
    Check(Backoff(0) == kLookAgain && Backoff(1) == kLookAgain && Backoff(2) == 2 * kLookAgain && Backoff(3) == 4 * kLookAgain,
          "a lookup that found nothing: looked for again after 30 s, twice as long after each further miss");
    Check(Backoff(7) == kLookAgainMax && Backoff(1000) == kLookAgainMax, "...up to kLookAgainMax, however many misses");
    Entry e;
    Check(!MissedRecently(e, 100), "never looked up: not missed");
    e.missedAt = 100; e.misses = 1;
    Check(MissedRecently(e, 129) && !MissedRecently(e, 131), "missed once: not again within 30 s");
    e.kept = true;
    Check(!MissedRecently(e, 101), "...unless one is kept since");
    // what a failed call means
    struct { HRESULT hr; bool press; After want; const char* what; } table[] = {
        { S_OK, true, After::Done, "S_OK: pressed" },
        { E_FAIL, true, After::NotDone, "E_FAIL: surely not pressed" },
        { static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), true, After::Gone, "element not available: gone, looked up again" },
        { static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), false, After::Gone, "...before the press too" },
        { RPC_E_DISCONNECTED, true, After::Gone, "disconnected (Explorer restarted): gone" },
        { HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE), true, After::Gone, "server unavailable: gone" },
        { kUiaTimeout, true, After::Unsure, "a press that timed out: unsure (may still arrive)" },
        { kUiaTimeout, false, After::NotDone, "...reading its patterns timed out: no press was made" },
        { RPC_E_SERVER_DIED, true, After::UnsureGone, "Explorer died during the press: unsure, and gone" },
        { RPC_E_SERVER_DIED, false, After::Gone, "...before the press: gone" },
        { HRESULT_FROM_WIN32(RPC_S_CALL_FAILED), true, After::UnsureGone, "the call failed mid-way: unsure, and gone" },
        { HRESULT_FROM_WIN32(ERROR_TIMEOUT), true, After::Unsure, "a Win32 timeout: unsure" },
        { E_ACCESSDENIED, true, After::NotDone, "access denied: not pressed" },
    };
    bool all = true;
    for (const auto& t : table) {
        const bool ok = Classify(t.hr, t.press) == t.want;
        if (!ok) printf("  wrong: %s\n", t.what);
        all = all && ok;
    }
    Check(all, "every failure of a press is put down as done, not done, gone, unsure (13 cases)");
}

static void ProviderTests() {
    using namespace taskbarstart;
    const HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    Check(ShownOn(g_bar) == primary, "a taskbar wholly on a monitor is seen on it");

    // ---- found, kept, checked
    size_t log = LogSize();
    Check(Look(g_bar, false) == Found::Now, "the Start button is found in the taskbar's automation tree");
    Entry e = Copy(g_bar);
    Check(e.kept && e.button && !wcscmp(e.from, L"from the taskbar window") && e.lookers == 0 && e.misses == 0,
          "...kept, from the taskbar's window, nobody looking any more");
    Check(LogHas(log, L"kept the Start button of a secondary taskbar"), "...and dock.log says so");
    const int asked = g_asked;
    Check(Look(g_bar, false) == Found::Kept && g_asked == asked, "a click with one kept: no lookup, no call");
    Check(Look(g_bar, true) == Found::Kept, "a kept one asked whether it is still there: it is");

    // ---- pressed
    After a = After::NotDone;
    HRESULT hr = E_FAIL;
    const wchar_t* how = L"";
    a = PressElement(e.button.Get(), &hr, &how);
    Check(a == After::Done && hr == S_OK && !wcscmp(how, L"toggle") && g_provider->toggles == 1, "a press: its Toggle, once");
    g_provider->toggleHr = E_FAIL;
    a = PressElement(e.button.Get(), &hr, &how);
    Check(a == After::NotDone && g_provider->toggles == 2, "a press that fails plainly: not done (the other ways go out)");
    g_provider->toggleHr = static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    a = PressElement(e.button.Get(), &hr, &how);
    Check(a == After::Gone, "a press answered \"not available\": gone (looked up once more)");
    g_provider->toggleHr = S_OK;
    {   // a slow press (Explorer busy): what the press answers once UI Automation's own timeout (kUiaTimeoutMs) runs out
        g_provider->toggleSleep = 1500;
        const double t0 = NowSeconds();
        a = PressElement(e.button.Get(), &hr, &how);
        const double took = NowSeconds() - t0;
        printf("  a press that takes 1.5 s: %s after %.2f s (hr %08lx)\n", a == After::Done ? "done" : a == After::Unsure ? "unsure" : "other", took, (unsigned long)hr);
        Check(a == After::Done || a == After::Unsure, "...a slow press is never taken for one surely not made (it would be followed by another way)");
        g_provider->toggleSleep = 0;
        Sleep(1600);
    }
    {   // the button's toggle state around a press, for dock.log (2026-11-17: a trusted press went through, no Start)
        ToggleSeen seen;
        g_provider->state = ToggleState_On;   // on, as if out of step with a closed Start
        a = PressElement(e.button.Get(), &hr, &how, &seen);
        Check(a == After::Done && seen.before == 1 && seen.after == 0 && g_provider->state == ToggleState_Off,
              "a toggle press reads the button's state before it and after it: on -> off");
        seen = ToggleSeen{};
        a = PressElement(e.button.Get(), &hr, &how, &seen);
        Check(a == After::Done && seen.before == 0 && seen.after == 1, "...and off -> on");
        seen = ToggleSeen{};
        const int reads = g_provider->stateReads;
        g_provider->toggleHr = E_FAIL;
        a = PressElement(e.button.Get(), &hr, &how, &seen);
        Check(a == After::NotDone && seen.before == 1 && seen.after == -1 && g_provider->stateReads == reads + 1,
              "...a press that did not go through: read before it only");
        g_provider->toggleHr = S_OK;
        seen = ToggleSeen{};
        g_provider->stateHr = E_FAIL;
        const int toggles = g_provider->toggles;
        a = PressElement(e.button.Get(), &hr, &how, &seen);
        printf("  the state's read failing: press %s, before %d, after %d, %d toggle%s\n", a == After::Done ? "done" : "not done", seen.before, seen.after,
               g_provider->toggles - toggles, g_provider->toggles - toggles == 1 ? "" : "s");
        // UI Automation answers a provider's failed read with the property's default (Indeterminate), not an error
        const bool unread = (seen.before == -1 || seen.before == ToggleState_Indeterminate) && (seen.after == -1 || seen.after == ToggleState_Indeterminate);
        Check(a == After::Done && unread && g_provider->toggles == toggles + 1,
              "...the state not to be read: -1 or indeterminate, and the press goes out all the same (nothing is decided by the state)");
        g_provider->stateHr = S_OK;
        a = PressElement(e.button.Get(), &hr, &how);
        Check(a == After::Done && g_provider->toggles == toggles + 2, "...a press asked without a ToggleSeen (no reads wanted): pressed as before");
        {   // Explorer busy at the read before the press: that wait is the press's (Pressed::took leaves out only the read after it)
            seen = ToggleSeen{};
            g_provider->stateSleepOnce = 300;
            const double t0 = NowSeconds();
            a = PressElement(e.button.Get(), &hr, &how, &seen);
            const double took = NowSeconds() - t0 - seen.afterTook;
            printf("  a read before the press that waits 0.3 s: the press took %.2f s, the read after it %.3f s\n", took, seen.afterTook);
            Check(a == After::Done && took >= 0.28 && seen.afterTook < 0.2,
                  "...a slow read before the press counts in the press's time (Explorer busy, kBusyCall), as the press would have waited");
        }
    }
    {   // a press asked only of a button that is off - the start button's worker, behind the shell's request: on,
        // Explorer holds Start open and a press would close it; a state not to be read is not pressed either
        ToggleSeen seen;
        g_provider->state = ToggleState_On;
        int toggles = g_provider->toggles;
        const int reads = g_provider->stateReads;
        a = PressElement(e.button.Get(), &hr, &how, &seen, true);
        Check(a == After::NotDone && seen.held && seen.before == 1 && seen.after == -1 && !wcscmp(how, L"toggle") && g_provider->toggles == toggles &&
              g_provider->stateReads == reads + 1 && g_provider->state == ToggleState_On,
              "[mod.36] a press asked only of an off button, its state on: read once, nothing pressed (held), the state as it was");
        seen = ToggleSeen{};
        g_provider->state = ToggleState_Off;
        a = PressElement(e.button.Get(), &hr, &how, &seen, true);
        Check(a == After::Done && !seen.held && seen.before == 0 && seen.after == 1 && g_provider->toggles == toggles + 1, "[mod.36] ...its state off: pressed once, off -> on");
        seen = ToggleSeen{};
        g_provider->state = ToggleState_Off;
        g_provider->stateHr = E_FAIL;   // (UI Automation answers a failed read with Indeterminate)
        toggles = g_provider->toggles;
        a = PressElement(e.button.Get(), &hr, &how, &seen, true);
        g_provider->stateHr = S_OK;
        Check(a == After::NotDone && seen.held && seen.before != 0 && g_provider->toggles == toggles, "[mod.36] ...its state not to be read: not pressed either (held)");
        a = PressElement(e.button.Get(), &hr, &how, nullptr, true);
        Check(a == After::Done && g_provider->toggles == toggles + 1, "[mod.36] ...asked without a ToggleSeen there is no state to hold by: pressed as before");
    }
    {   // only when off, whatever else the button offers: pressed by its Toggle alone; a button with no Toggle
        // pattern - Invoke or the accessibility default action only - is not pressed at all (held: no state to read)
        StartProvider& b = *g_provider;
        ToggleSeen seen;
        b.patterns = kToggle | kInvoke;
        b.state = ToggleState_Off;
        int toggles = b.toggles, invokes = b.invokes;
        a = PressElement(e.button.Get(), &hr, &how, &seen, true);
        Check(a == After::Done && !seen.held && !wcscmp(how, L"toggle") && seen.before == 0 && seen.after == 1 && b.toggles == toggles + 1 && b.invokes == invokes,
              "[mod.36] only when off, a button with Invoke and Toggle, off: pressed by its Toggle (state read), never invoked");
        seen = ToggleSeen{};
        toggles = b.toggles;
        a = PressElement(e.button.Get(), &hr, &how, &seen, true);   // (on now)
        Check(a == After::NotDone && seen.held && seen.before == 1 && b.toggles == toggles && b.invokes == invokes && b.state == ToggleState_On,
              "[mod.36] ...the same, on: nothing pressed - neither toggled nor invoked (held)");
        b.state = ToggleState_Off;
        a = PressElement(e.button.Get(), &hr, &how);
        Check(a == After::Done && !wcscmp(how, L"invoke") && b.invokes == invokes + 1 && b.toggles == toggles,
              "[mod.36] ...asked of any button (not only when off): invoked, as a click (as before)");
        b.patterns = kInvoke;   // Invoke only: no state to read
        seen = ToggleSeen{};
        invokes = b.invokes;
        a = PressElement(e.button.Get(), &hr, &how, &seen, true);
        Check(a == After::NotDone && seen.held && hr == S_OK && !wcscmp(how, L"no toggle state to read") && b.invokes == invokes && b.toggles == toggles,
              "[mod.36] only when off, a button with Invoke and no Toggle: not invoked - held, \"no toggle state to read\"");
        b.patterns = kLegacy;   // the accessibility default action only
        seen = ToggleSeen{};
        int actions = b.defaultActions;
        a = PressElement(e.button.Get(), &hr, &how, &seen, true);
        Check(a == After::NotDone && seen.held && seen.before == -1 && hr == S_OK && !wcscmp(how, L"no toggle state to read") && b.defaultActions == actions &&
              b.invokes == invokes && b.toggles == toggles,
              "[mod.36] only when off, a button with only the default action: its default action not done - held, \"no toggle state to read\"");
        a = PressElement(e.button.Get(), &hr, &how);
        Check(a == After::Done && !wcscmp(how, L"default action") && b.defaultActions == actions + 1,
              "[mod.36] ...asked of any button: its default action, once (as before)");
        b.patterns = kToggle;
        b.state = ToggleState_Off;
    }

    // ---- Explorer rebuilt its taskbar's content in the same window: the kept button is gone, another one there
    OnUi(kRemove);
    OnUi(kMake);
    log = LogSize();
    Check(Look(g_bar, true) == Found::Now, "the kept button gone, another one there: found again");
    Check(LogHas(log, L"was gone") && LogHas(log, L"kept the Start button"), "...and dock.log says the kept one was gone");
    e = Copy(g_bar);
    a = PressElement(e.button.Get(), &hr, &how);
    Check(a == After::Done && g_provider->toggles == 1, "...and the new one is what is pressed");

    // ---- no button any more: a miss, not looked for again for a while
    OnUi(kRemove);
    log = LogSize();
    Check(Look(g_bar, true) == Found::NoButton, "the kept button gone and none there: not found");
    e = Copy(g_bar);
    Check(!e.kept && !e.button && e.misses == 1 && e.missedAt > 0 && MissedRecently(e, NowSeconds()), "...a miss: not looked for again within 30 s");
    Check(LogHas(log, L"no Start button in the automation tree"), "...and dock.log says so, once");
    Check(LogHas(log, L"until found the start button has no press: the shell's request, then the Windows key"),
          "...and what a click does meanwhile (the ladder without its press)");
    const int asked2 = g_asked;
    Check(Look(g_bar, false) == Found::NoButton && g_asked == asked2, "a click within those 30 s: no lookup");
    log = LogSize();
    {   // the next miss (forced: as 30 s later) waits twice as long, and is not logged again
        { std::lock_guard<std::mutex> lk(S().book); At(g_bar).missedAt = NowSeconds() - kLookAgain - 1; }
        Check(Look(g_bar, false) == Found::NoButton, "30 s later: looked up again, still none");
        e = Copy(g_bar);
        Check(e.misses == 2 && MissedRecently(e, NowSeconds() + kLookAgain + 1), "...the wait doubles");
        Check(!LogHas(log, L"no Start button"), "...and the log line is not repeated");
    }

    // ---- the taskbar vanishing (Remember): looked up on a thread of the MTA, the caller waits a little
    OnUi(kMake);
    { std::lock_guard<std::mutex> lk(S().book); At(g_bar).missedAt = -1; At(g_bar).misses = 0; }
    if (!Classic()) {
        const double t0 = NowSeconds();
        Remember(g_bar);
        const double waited = NowSeconds() - t0;
        // the lookup may outlast the caller's wait (a busy machine): let its thread finish first
        for (int i = 0; i < 250 && Copy(g_bar).lookers > 0; ++i) Sleep(20);
        e = Copy(g_bar);
        Check(e.kept && e.lookers == 0 && e.monitor == primary, "the taskbar about to vanish: its Start button looked up and kept, its monitor noted");
        Check(waited < kRememberWaitMs / 1000.0 + 0.3, "...the caller waited at most kRememberWaitMs");
        const int asked3 = g_asked;
        Remember(g_bar);
        Sleep(200);
        Check(g_asked == asked3, "...and again within kCheckAgain: nothing asked of Explorer");
    } else {
        printf("  not checked: Explorer's taskbar is the classic one here (Remember looks into none)\n");
    }

    // ---- the taskbar window gone (Explorer restarted, a monitor left): its entry is dropped at the next lookup
    const HWND bar = g_bar;
    OnUi(kDestroyBar);
    HWND other = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"hunt-start_menu other", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
    Look(other, false);
    bool still = false;
    {
        std::lock_guard<std::mutex> lk(S().book);
        for (const auto& x : S().entries) if (x.taskbar == bar) still = true;
    }
    Check(!still, "a taskbar window that is gone: its entry (and its kept button) dropped");
    DestroyWindow(other);
    Remember(nullptr);
    Remember(bar);   // gone: nothing to remember
    {
        std::lock_guard<std::mutex> lk(S().book);
        for (const auto& x : S().entries) if (x.taskbar == bar || !x.taskbar) still = true;
    }
    Check(!still, "Remember of no window, or of one that is gone: nothing kept for it");
    Check(!TaskbarOn(nullptr) && !Kept(nullptr), "no monitor: no taskbar, nothing kept");
}

// ---- (characterization) taskbarstart::Press as the start button's worker calls it, on the test's own taskbar: what it
// answers (Pressed), and above all Pressed::took - the press call with the state's read before it (a stall that read takes
// over from the press still counts: Explorer busy, kBusyCall), not the read after it, nor the wait for S().calls (mod.34).
// Time is virtual here: a slow read or press is a step of the clock taken by the provider, so the seconds are exact.
static void PressTests() {
    using namespace taskbarstart;
    const HMONITOR realPrimary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    OnUi(kMakeBar);   // ProviderTests destroyed its taskbar: one again
    g_ownBar = g_bar;
    g_ownBars = true;
    // first of all: no taskbar but the test's own is to be had (else nothing is pressed)
    const bool own = g_bar && ExplorerTaskbar() == g_bar && TaskbarOn(kOwnPrimary) == g_bar && !TaskbarOn(realPrimary) && !Classic();
    Check(own, "[char] Press: the only taskbar to be had is the test's own (none of Explorer's is looked into or pressed)");
    constexpr int kChecks = 8;
    if (!own) {
        for (int i = 0; i < kChecks; ++i) Check(false, "[char] Press: not run (a taskbar not the test's could be reached)");
        g_ownBars = false;
        return;
    }
    g_virtual = true;
    g_virtUs = 0;
    Check(Find(kOwnPrimary) == Found::Now, "[char] Press: the test's taskbar's button found (and kept)");

    // the read before the press takes 0.3 s: counted in took
    g_provider->state = ToggleState_Off;
    g_provider->readTakesUs = 300000;
    g_virtUs = 0;
    Pressed p = Press(kOwnPrimary);
    printf("  a read before the press of 0.3 s: took %.6f, before %d, after %d, how %ls, hr %08lx\n", p.took, p.before, p.after, p.how, (unsigned long)p.hr);
    Check(p.ok && !p.unsure && p.hr == S_OK && !wcscmp(p.how, L"toggle") && p.before == 0 && p.after == 1 && p.took == 0.3 && g_virtUs == 300000,
          "[char] Press: the read before the press takes 0.3 s - took exactly 0.3 s; ok, toggle, state off -> on, S_OK");

    // the read after the press takes 0.3 s: left out of took
    g_provider->afterReadTakesUs = 300000;
    g_virtUs = 0;
    p = Press(kOwnPrimary);
    printf("  a read after the press of 0.3 s: took %.6f, before %d, after %d\n", p.took, p.before, p.after);
    Check(p.ok && p.before == 1 && p.after == 0 && p.took == 0 && g_virtUs == 300000,
          "[char] Press: the read after the press takes 0.3 s - took exactly 0 (left out); state on -> off");

    // the press itself takes 0.25 s: counted
    g_provider->toggleTakesUs = 250000;
    g_virtUs = 0;
    p = Press(kOwnPrimary);
    g_provider->toggleTakesUs = 0;
    printf("  a press of 0.25 s: took %.6f\n", p.took);
    Check(p.ok && p.took == 0.25 && g_virtUs == 250000, "[char] Press: the press itself takes 0.25 s - took exactly 0.25 s");

    // another thread holds S().calls (a lookup on a busy Explorer) while 0.3 s go by: the wait is not the press's
    {
        HANDLE held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_virtUs = 0;
        std::thread other([held] {
            std::lock_guard<std::mutex> lk(S().calls);
            SetEvent(held);
            Sleep(300);   // (real time: Press has long reached the lock by then)
            g_virtUs += 300000;
        });
        WaitForSingleObject(held, 5000);
        p = Press(kOwnPrimary);
        other.join();
        CloseHandle(held);
        printf("  S().calls held 0.3 s by another thread: took %.6f\n", p.took);
        Check(p.ok && p.took == 0 && g_virtUs == 300000, "[char] Press: S().calls held by another thread for 0.3 s - took exactly 0 (the wait for the lock left out)");
    }

    // a press that fails plainly: not ok, not unsure, its HRESULT; the read before it still counted, none after it
    g_provider->toggleHr = E_FAIL;
    g_provider->readTakesUs = 300000;
    const int toggles = g_provider->toggles;
    g_virtUs = 0;
    p = Press(kOwnPrimary);
    g_provider->toggleHr = S_OK;
    printf("  a press that fails: took %.6f, before %d, after %d, hr %08lx, %d toggle(s)\n", p.took, p.before, p.after, (unsigned long)p.hr, g_provider->toggles - toggles);
    Check(!p.ok && !p.unsure && p.hr == E_FAIL && !wcscmp(p.how, L"toggle") && p.before == 0 && p.after == -1 && p.took == 0.3 && g_provider->toggles == toggles + 1,
          "[char] Press: a press that fails plainly - not ok, not unsure, E_FAIL, read before only (off), took 0.3 s, pressed once");

    // a press answered "not available" (the button gone): dropped, looked up again and pressed once more - twice in all
    g_provider->toggleHr = static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    const int toggles2 = g_provider->toggles;
    p = Press(kOwnPrimary);
    g_provider->toggleHr = S_OK;
    Check(!p.ok && !p.unsure && p.hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && g_provider->toggles == toggles2 + 2 && !Copy(g_bar).kept,
          "[char] Press: a button answering \"not available\" - looked up again and pressed a second time, then given up, nothing kept");

    // no taskbar on the monitor: nothing pressed, ERROR_NOT_FOUND
    const int toggles3 = g_provider->toggles;
    p = Press(realPrimary);
    Check(!p.ok && !p.unsure && p.hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && !wcscmp(p.how, L"no taskbar on this monitor") && p.took == 0 &&
          g_provider->toggles == toggles3, "[char] Press: no taskbar on the monitor - nothing pressed, ERROR_NOT_FOUND, \"no taskbar on this monitor\"");

    // Press asked only of an off button, as the start button's worker asks it: on - nothing pressed, held, the read's
    // time still in took (a slow read is Explorer busy all the same); off - pressed
    g_provider->state = ToggleState_On;
    g_provider->readTakesUs = 300000;
    const int toggles4 = g_provider->toggles;
    g_virtUs = 0;
    p = Press(kOwnPrimary, true);
    printf("  only when off, the button on: held %d, took %.6f, before %d, after %d, how %ls, %d toggle(s)\n", p.held, p.took, p.before, p.after, p.how, g_provider->toggles - toggles4);
    Check(!p.ok && !p.unsure && p.held && p.before == 1 && p.after == -1 && p.took == 0.3 && !wcscmp(p.how, L"toggle") && g_provider->toggles == toggles4,
          "[mod.36] Press, only when off, the button on: nothing pressed - held, before on, the read's 0.3 s in took");
    g_provider->state = ToggleState_Off;
    g_virtUs = 0;
    p = Press(kOwnPrimary, true);
    Check(p.ok && !p.held && p.before == 0 && p.after == 1 && g_provider->toggles == toggles4 + 1, "[mod.36] ...the button off: pressed once, off -> on, not held");
    p = Press(kOwnPrimary);
    Check(p.ok && !p.held && p.before == 1 && p.after == 0 && g_provider->toggles == toggles4 + 2, "[mod.36] ...Press without it presses whatever the state (on -> off), as before");

    // only when off, the state's read answers "not available" (Explorer rebuilt its taskbar meanwhile): looked up
    // once more. Found again, off: pressed once - the first look's hold is not carried over
    g_provider->state = ToggleState_Off;
    g_provider->goneOnRead = true;
    const int toggles5 = g_provider->toggles, reads5 = g_provider->stateReads;
    p = Press(kOwnPrimary, true);
    printf("  only when off, the read answering \"gone\", found again: ok %d, held %d, before %d, how %ls, hr %08lx, %d read(s), %d toggle(s)\n", p.ok, p.held,
           p.before, p.how, (unsigned long)p.hr, g_provider->stateReads - reads5, g_provider->toggles - toggles5);
    Check(p.ok && !p.held && p.before == 0 && p.after == 1 && !wcscmp(p.how, L"toggle") && g_provider->toggles == toggles5 + 1 && g_provider->stateReads == reads5 + 3 &&
          Copy(g_bar).kept, "[mod.36] Press, only when off, the read answering \"gone\": looked up again, read again (off) and pressed once, not held");
    // ...not found again: nothing pressed at all (held), and how says why - not the lookup's answer alone, which would read
    // as if no button had been there (start: "left alone at N s: <how>")
    g_provider->goneOnRead = true;
    g_provider->goneTakesId = true;
    const int toggles6 = g_provider->toggles, reads6 = g_provider->stateReads;
    p = Press(kOwnPrimary, true);
    printf("  only when off, the read answering \"gone\", not found again: ok %d, held %d, before %d, how %ls, hr %08lx, %d read(s), %d toggle(s)\n", p.ok, p.held,
           p.before, p.how, (unsigned long)p.hr, g_provider->stateReads - reads6, g_provider->toggles - toggles6);
    Check(!p.ok && !p.unsure && p.held && p.before == -1 && p.hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) &&
          !wcscmp(p.how, L"it went away as its state was read, and was not found again") && g_provider->toggles == toggles6 &&
          g_provider->stateReads == reads6 + 1 && !Copy(g_bar).kept,
          "[mod.36] ...not found again: nothing pressed (held), ERROR_NOT_FOUND, how \"it went away as its state was read, and was not found again\"");
    g_provider->goneTakesId = false;
    g_provider->idGone = false;
    { std::lock_guard<std::mutex> lk(S().book); At(g_bar).missedAt = -1; At(g_bar).misses = 0; }
    // ...a press asked of any button, answered "gone", the button not found again: not held (it was pressed), the lookup's
    // answer as before. (The kept button is pressed - Find answers Kept without a look - and its id is gone already, so the
    // second look finds nothing.)
    Check(Find(kOwnPrimary) == Found::Now, "[mod.36] (the test's button found again and kept)");
    g_provider->toggleHr = static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    g_provider->idGone = true;
    const int toggles7 = g_provider->toggles;
    p = Press(kOwnPrimary);
    g_provider->toggleHr = S_OK;
    g_provider->idGone = false;
    Check(!p.ok && !p.unsure && !p.held && p.hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && !wcscmp(p.how, L"not in the taskbar's automation tree") &&
          g_provider->toggles == toggles7 + 1, "[mod.36] ...a press asked of any button answered \"gone\", not found again: pressed once, not held, the lookup's answer (as before)");
    { std::lock_guard<std::mutex> lk(S().book); At(g_bar).missedAt = -1; At(g_bar).misses = 0; }
    g_virtual = false;
    g_ownBars = false;
}

int wmain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // as the start button's worker: an MTA thread
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread ui(UiThread);
    g_uiThread = GetThreadId(ui.native_handle());
    WaitForSingleObject(g_done, 5000);
    PureTests();
    ProviderTests();
    PressTests();
    PostThreadMessageW(g_uiThread, WM_QUIT, 0, 0);
    ui.join();
    Summary("start_menu_taskbar_test");
    fflush(stdout);
    ExitProcess(static_cast<UINT>(portable_check::Failures()));   // the kept UI Automation objects die with the process, as in the dock
}
