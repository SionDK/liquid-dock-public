// The taskbar's Start button as the dock finds, checks and presses it (src/taskbar_start.cpp), against a
// "taskbar" of the test's own: a window with a child that UI Automation sees as a button with AutomationId
// "StartButton" and a Toggle pattern (a provider of the test's own), as Explorer's XAML Start button is. The
// real lookup, the check of a kept button, Explorer rebuilding its taskbar (the button replaced, or gone), the
// wait before looking again, the press and what its failures mean, the taskbar's vanishing (Remember) run here;
// Explorer's own taskbar is never looked into or pressed (only Classic() reads its window, as the dock does).
// The windows are the test's own, invisible (alpha 0, never activated), in a corner of the primary monitor.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\start_menu_taskbar_test.cpp /link user32.lib ole32.lib oleaut32.lib uuid.lib uiautomationcore.lib
#include "../src/taskbar_start.cpp"
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
double NowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

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

// ---- the button's provider: AutomationId "StartButton", a Toggle pattern that counts its presses
struct StartProvider : IRawElementProviderSimple, IToggleProvider {
    HWND hwnd = nullptr;
    std::atomic<long> refs{ 1 };
    std::atomic<int> toggles{ 0 };
    std::atomic<HRESULT> toggleHr{ S_OK };
    std::atomic<DWORD> toggleSleep{ 0 };
    // IUnknown (the IRawElementProviderSimple base is the identity)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(IToggleProvider)) *out = static_cast<IToggleProvider*>(this);
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
        if (id == UIA_TogglePatternId) { *out = static_cast<IToggleProvider*>(this); AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override {
        VariantInit(v);
        if (id == UIA_AutomationIdPropertyId) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(L"StartButton"); }
        else if (id == UIA_ControlTypePropertyId) { v->vt = VT_I4; v->lVal = UIA_ButtonControlTypeId; }
        else if (id == UIA_NamePropertyId) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(L"Start"); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override { return UiaHostProviderFromHwnd(hwnd, out); }
    // IToggleProvider
    HRESULT STDMETHODCALLTYPE Toggle() override { ++toggles; if (toggleSleep) Sleep(toggleSleep); return toggleHr; }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* s) override { *s = ToggleState_Off; return S_OK; }
};

// ---- the test's taskbar: a top-level window and its button, on a thread of their own that pumps messages
// (UI Automation reaches the provider through the button window's thread)
constexpr UINT kMake = WM_APP + 1, kRemove = WM_APP + 2, kDestroyBar = WM_APP + 3;
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

int wmain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // as the start button's worker: an MTA thread
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread ui(UiThread);
    g_uiThread = GetThreadId(ui.native_handle());
    WaitForSingleObject(g_done, 5000);
    PureTests();
    ProviderTests();
    PostThreadMessageW(g_uiThread, WM_QUIT, 0, 0);
    ui.join();
    printf("start_menu_taskbar_test: %s (%d checks, %d failure%s)\n", failures ? "FAILED" : "ALL PASSED", checks, failures, failures == 1 ? "" : "s");
    fflush(stdout);
    ExitProcess(static_cast<UINT>(failures));   // the kept UI Automation objects die with the process, as in the dock
}
