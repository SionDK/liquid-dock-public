#include "taskbar_start.h"
#include "explorer_taskbar.h"
#include <uiautomation.h>
#include <mutex>
#include <thread>

// What taskbar_start.h says. The Start button on Windows 11's taskbar: AutomationId "StartButton" (the XAML
// taskbar since 21H2).

namespace taskbarstart {
namespace {

constexpr DWORD kRememberWaitMs = 800;   // the taskbar's vanishing waits this long for the lookup
constexpr DWORD kUiaTimeoutMs = 500;     // per call into a busy Explorer (connection, transaction)
constexpr double kLookAgain = 30.0;      // a lookup that found nothing is not repeated sooner for that taskbar,
constexpr double kLookAgainMax = 1800.0; // ...and twice as long after each further miss, up to this
constexpr double kLookupBudget = 1.0;    // a lookup tries no further root once this long has gone (a hung Explorer)
constexpr double kCheckAgain = 10.0;     // a kept button is asked whether it is still there at most this often
constexpr double kQuietWait = 5.0;       // after a wait for a lookup ran out, the UI thread does not wait again this long
constexpr HRESULT kUiaTimeout = static_cast<HRESULT>(0x80131505);   // UIA_E_TIMEOUT (not in every SDK's headers)

struct Entry {
    HWND taskbar = nullptr;
    HMONITOR monitor = nullptr;            // its monitor when last seen expanded (auto-hidden it lies mostly off it)
    ComPtr<IUIAutomationElement> button;   // set and released on MTA threads only
    const wchar_t* from = L"";             // where it was found
    bool kept = false;                     // button is set (to be read without touching COM)
    int lookers = 0;                       // lookups for this taskbar running or about to run
    double missedAt = -1;                  // when the last lookup found nothing
    int misses = 0;                        // lookups in a row that found nothing (the wait before the next one grows)
    double checkedAt = -1;                 // when the kept button was found, or last answered
    bool saidNone = false;                 // a miss has been logged
};

struct State {
    std::mutex book;              // entries; never held across a call into Explorer (the UI thread takes it)
    std::mutex calls;             // calls into UI Automation, one at a time; uia
    ComPtr<IUIAutomation> uia;
    std::vector<Entry> entries;
    double waitRanOutAt = -1e9;   // book: when Remember's caller last waited in vain
};
// Never destroyed: a detached worker may still be inside a call when the process ends.
State& S() { static State* const s = new State; return *s; }

// The apartment's objects die with it, and the MTA ends when its last thread leaves it: kept for the process.
void KeepMta() {
    static std::once_flag once;
    std::call_once(once, [] { CO_MTA_USAGE_COOKIE cookie = nullptr; CoIncrementMTAUsage(&cookie); });   // never released
}

// calls held. CUIAutomation8 takes the timeouts and AutoSetFocus (CUIAutomation, Windows 7's, has no IUIAutomation2).
IUIAutomation* Uia() {
    State& s = S();
    if (!s.uia) {
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s.uia))) &&
            FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s.uia)))) {
            s.uia.Reset();
            return nullptr;
        }
        ComPtr<IUIAutomation2> u2;
        if (SUCCEEDED(s.uia.As(&u2))) {
            u2->put_ConnectionTimeout(kUiaTimeoutMs);
            u2->put_TransactionTimeout(kUiaTimeoutMs);
            u2->put_AutoSetFocus(FALSE);   // a press must not focus the hidden taskbar (it would slide in)
        }
    }
    return s.uia.Get();
}

// The element was surely not reached: it is gone (Explorer rebuilt its taskbar, or restarted). Look again.
bool Gone(HRESULT hr) {
    return hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) || hr == RPC_E_DISCONNECTED || hr == RPC_E_SERVER_DIED_DNE ||
           hr == CO_E_OBJNOTCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED_DNE);
}
// The call failed, and may have run all the same (the call timed out, or Explorer went away during it).
bool MayHaveRun(HRESULT hr) {
    return hr == kUiaTimeout || hr == RPC_E_TIMEOUT || hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) || hr == RPC_E_SERVER_DIED ||
           hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED);
}

const wchar_t* Name(HWND taskbar) { return taskbar == ExplorerTaskbar() ? L"the main taskbar" : L"a secondary taskbar"; }

double Backoff(int misses) {
    double b = kLookAgain;
    for (int i = 1; i < misses && b < kLookAgainMax; ++i) b *= 2;
    return b < kLookAgainMax ? b : kLookAgainMax;
}
bool MissedRecently(const Entry& e, double now) { return !e.kept && e.missedAt >= 0 && now - e.missedAt < Backoff(e.misses); }
void Unlook(Entry& e) { if (e.lookers > 0) --e.lookers; }   // (an entry made anew after its taskbar went away starts at 0)

Entry& At(HWND taskbar) {   // book held; the reference is good until the next At
    State& s = S();
    for (auto& e : s.entries) if (e.taskbar == taskbar) return e;
    s.entries.emplace_back();
    s.entries.back().taskbar = taskbar;
    return s.entries.back();
}

// Taskbars that are gone (Explorer restarted, a monitor left). MTA thread: the elements are released after the
// book is let go - releasing one is a call into Explorer.
void Prune() {
    std::vector<Entry> dead;
    {
        std::lock_guard<std::mutex> lk(S().book);
        auto& v = S().entries;
        for (auto it = v.begin(); it != v.end();) {
            if (!IsWindow(it->taskbar)) { dead.push_back(std::move(*it)); it = v.erase(it); }
            else ++it;
        }
    }
}

void Drop(HWND taskbar) {   // MTA thread
    ComPtr<IUIAutomationElement> old;
    {
        std::lock_guard<std::mutex> lk(S().book);
        Entry& e = At(taskbar);
        old.Swap(e.button);
        e.kept = false;
        e.from = L"";
    }
}

ComPtr<IUIAutomationCondition> IdIs(IUIAutomation* uia, const wchar_t* id) {
    ComPtr<IUIAutomationCondition> c;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(id);
    if (v.bstrVal) uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &c);
    VariantClear(&v);
    return c;
}

ComPtr<IUIAutomationElement> FindUnder(IUIAutomation* uia, HWND window, IUIAutomationCondition* cond) {
    ComPtr<IUIAutomationElement> root, found;
    if (SUCCEEDED(uia->ElementFromHandle(window, &root)) && root) root->FindFirst(TreeScope_Descendants, cond, &found);
    return found;
}

BOOL CALLBACK AddIsland(HWND w, LPARAM p) {
    wchar_t cls[96] = L"";
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") || !wcscmp(cls, L"Windows.UI.Input.InputSite.WindowClass"))
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(w);
    return TRUE;
}

// From the taskbar's window first; then from its XAML island's windows (hidden ones too), which the tree may
// no longer reach into from the taskbar once it is hidden - while kLookupBudget lasts. calls held.
ComPtr<IUIAutomationElement> Lookup(IUIAutomation* uia, HWND taskbar, const wchar_t** from) {
    const double until = NowSeconds() + kLookupBudget;
    ComPtr<IUIAutomationCondition> cond = IdIs(uia, L"StartButton");
    if (!cond) return nullptr;
    if (auto b = FindUnder(uia, taskbar, cond.Get())) { *from = L"from the taskbar window"; return b; }
    std::vector<HWND> islands;
    EnumChildWindows(taskbar, AddIsland, reinterpret_cast<LPARAM>(&islands));
    for (size_t i = 0; i < islands.size() && i < 8 && NowSeconds() < until; ++i)
        if (auto b = FindUnder(uia, islands[i], cond.Get())) { *from = L"from its XAML island"; return b; }
    return nullptr;
}

// Look this taskbar's Start button up now and keep it, or note the miss. check: a kept one is first asked
// whether it is still there (one call: Explorer may have rebuilt its taskbar's content in the same window),
// and looked up again if it is gone. MTA thread.
Found Look(HWND taskbar, bool check) {
    Prune();
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.book);
        ++At(taskbar).lookers;
    }
    ComPtr<IUIAutomationElement> kept, button;
    const wchar_t* from = L"";
    bool stillThere = false, wasGone = false, missedMeanwhile = false;
    {
        std::lock_guard<std::mutex> call(s.calls);
        {
            std::lock_guard<std::mutex> lk(s.book);
            const Entry& e = At(taskbar);
            kept = e.button;   // kept before, or found meanwhile by another thread
            missedMeanwhile = !check && MissedRecently(e, NowSeconds());   // another thread looked in vain while this one waited
        }
        if (kept && check) {
            BSTR id = nullptr;
            const HRESULT hr = kept->get_CurrentAutomationId(&id);   // answers only while the element is there
            const bool same = SUCCEEDED(hr) && id && !wcscmp(id, L"StartButton");
            SysFreeString(id);
            wasGone = Gone(hr) || hr == RPC_E_SERVER_DIED || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) || (SUCCEEDED(hr) && !same);
            stillThere = !wasGone;   // a timeout says nothing: kept
        } else if (kept) {
            stillThere = true;
        }
        if (!stillThere && !missedMeanwhile)
            if (IUIAutomation* uia = Uia()) button = Lookup(uia, taskbar, &from);
    }
    ComPtr<IUIAutomationElement> old;   // a button gone stale, released after the book is let go
    bool sayNone = false;
    double again = 0;
    {
        std::lock_guard<std::mutex> lk(s.book);
        Entry& e = At(taskbar);
        Unlook(e);
        if (stillThere) {
            e.checkedAt = NowSeconds();
            return Found::Kept;
        }
        if (missedMeanwhile) return Found::NoButton;
        old.Swap(e.button);
        if (button) {
            e.button = button;
            e.from = from;
            e.kept = true;
            e.missedAt = -1;
            e.misses = 0;
            e.checkedAt = NowSeconds();
        } else {
            e.kept = false;
            e.from = L"";
            e.missedAt = NowSeconds();
            again = Backoff(++e.misses);
            sayNone = !e.saidNone;
            e.saidNone = true;
        }
    }
    if (wasGone) Log(L"taskbar: the kept Start button of %ls was gone (Explorer rebuilt its taskbar): looked up again", Name(taskbar));
    if (button) Log(L"taskbar: kept the Start button of %ls (found %ls)", Name(taskbar), from);
    else if (sayNone)
        Log(L"taskbar: no Start button in the automation tree of %ls (not built yet, or hidden): looked for again after %.0f s at the "
            L"earliest (longer after each miss); until found the start button asks the shell", Name(taskbar), again);
    return button ? Found::Now : Found::NoButton;
}

// A click on it, as UI Automation offers one: Invoke (what a click raises), else Toggle, else the
// accessibility default action. Only a failure of the press itself may have reached Explorer.
enum class After : uint8_t { Done, NotDone, Gone, Unsure, UnsureGone };
After Classify(HRESULT hr, bool pressCall) {
    if (SUCCEEDED(hr)) return After::Done;
    if (Gone(hr)) return After::Gone;
    if (MayHaveRun(hr)) {
        const bool dead = hr == RPC_E_SERVER_DIED || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED);
        if (!pressCall) return dead ? After::Gone : After::NotDone;   // no press was made yet
        return dead ? After::UnsureGone : After::Unsure;
    }
    return After::NotDone;
}

After PressElement(IUIAutomationElement* e, HRESULT* hr, const wchar_t** how) {
    ComPtr<IUIAutomationInvokePattern> invoke;
    *hr = e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke));
    if (FAILED(*hr)) { *how = L"its patterns could not be read"; return Classify(*hr, false); }
    if (invoke) { *how = L"invoke"; *hr = invoke->Invoke(); return Classify(*hr, true); }
    ComPtr<IUIAutomationTogglePattern> toggle;
    *hr = e->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle));
    if (FAILED(*hr)) { *how = L"its patterns could not be read"; return Classify(*hr, false); }
    if (toggle) { *how = L"toggle"; *hr = toggle->Toggle(); return Classify(*hr, true); }
    ComPtr<IUIAutomationLegacyIAccessiblePattern> acc;
    *hr = e->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&acc));
    if (FAILED(*hr)) { *how = L"its patterns could not be read"; return Classify(*hr, false); }
    if (acc) { *how = L"default action"; *hr = acc->DoDefaultAction(); return Classify(*hr, true); }
    *how = L"no way to press it";
    *hr = E_NOINTERFACE;
    return After::NotDone;
}

// Its window lies wholly on one monitor (expanded): that monitor. Auto-hidden it lies mostly off its own -
// perhaps over the one below - and tells nothing.
HMONITOR ShownOn(HWND h) {
    RECT r{};
    if (!GetWindowRect(h, &r) || r.right <= r.left || r.bottom <= r.top) return nullptr;
    HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    RECT in{};
    if (!GetMonitorInfoW(m, &mi) || !IntersectRect(&in, &r, &mi.rcMonitor) || !EqualRect(&in, &r)) return nullptr;
    return m;
}

}   // namespace

bool Classic() {
    // native_tray.cpp tells them apart the same way: the classic tray's hidden-icons button (1502)
    HWND main = ExplorerTaskbar();
    HWND notify = main ? FindWindowExW(main, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    return notify && GetDlgItem(notify, 1502);
}

HWND TaskbarOn(HMONITOR monitor) {
    if (!monitor) return nullptr;
    HWND main = ExplorerTaskbar();
    if (!main) return nullptr;
    // Explorer's main taskbar is on the primary monitor ("make this my main display" moves it). Not asked
    // of its window: auto-hidden, it lies mostly off its monitor, maybe over the one below.
    if (monitor == MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY)) return main;
    DWORD explorer = 0;
    GetWindowThreadProcessId(main, &explorer);
    std::vector<HWND> secondary;
    HWND s = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr);
    for (int guard = 0; s && guard < 16; ++guard, s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(s, &pid);
        if (pid == explorer) secondary.push_back(s);
    }
    // the one last seen expanded on this monitor; else by its window (right unless monitors are stacked)
    std::vector<HMONITOR> seen(secondary.size(), nullptr);
    {
        std::lock_guard<std::mutex> lk(S().book);
        for (size_t i = 0; i < secondary.size(); ++i)
            for (const auto& e : S().entries)
                if (e.taskbar == secondary[i]) seen[i] = e.monitor;
    }
    for (size_t i = 0; i < secondary.size(); ++i)
        if (seen[i] == monitor) return secondary[i];
    for (size_t i = 0; i < secondary.size(); ++i) {
        MONITORINFO mi = { sizeof(mi) };
        const bool stale = !seen[i] || !GetMonitorInfoW(seen[i], &mi);
        if (stale && MonitorFromWindow(secondary[i], MONITOR_DEFAULTTONEAREST) == monitor) return secondary[i];
    }
    return nullptr;
}

void Remember(HWND taskbar) {
    if (!taskbar || !IsWindow(taskbar) || Classic()) return;
    const HMONITOR shown = ShownOn(taskbar);
    bool wait = true;
    {
        std::lock_guard<std::mutex> lk(S().book);
        Entry& e = At(taskbar);
        if (shown) e.monitor = shown;
        const double now = NowSeconds();
        if (e.lookers > 0 || (e.kept && now - e.checkedAt < kCheckAgain) || MissedRecently(e, now)) return;
        ++e.lookers;   // no second thread for it meanwhile; undone when the thread is through
        wait = now - S().waitRanOutAt >= kQuietWait;   // a wait ran out moments ago: Explorer is busy, the UI goes on
    }
    // the lookup runs on a thread of the MTA (the caller may be the UI thread, an STA); the caller waits a
    // little for it, as the taskbar is about to vanish - and goes on without it after kRememberWaitMs
    std::shared_ptr<void> done(CreateEventW(nullptr, TRUE, FALSE, nullptr), [](void* h) { if (h) CloseHandle(h); });
    bool started = false;
    if (done.get()) {
        try {
            std::thread([taskbar, done] {
                const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (SUCCEEDED(co)) {
                    KeepMta();
                    Look(taskbar, true);
                    CoUninitialize();
                }
                {
                    std::lock_guard<std::mutex> lk(S().book);
                    Unlook(At(taskbar));
                }
                SetEvent(done.get());
            }).detach();
            started = true;
        } catch (...) {
        }
    }
    if (!started) {   // no thread to be had: a click looks for it
        std::lock_guard<std::mutex> lk(S().book);
        Unlook(At(taskbar));
        return;
    }
    if (wait && WaitForSingleObject(done.get(), kRememberWaitMs) == WAIT_TIMEOUT) {
        std::lock_guard<std::mutex> lk(S().book);
        S().waitRanOutAt = NowSeconds();
    }
}

const wchar_t* FoundText(Found f) {
    switch (f) {
    case Found::Kept: return L"kept from when the taskbar was expanded";
    case Found::Now: return L"found at the click";
    case Found::NoTaskbar: return L"no taskbar on this monitor";
    case Found::NoButton: return L"not in the taskbar's automation tree";
    case Found::Classic: return L"Windows' classic taskbar, which has none";
    }
    return L"?";
}

Found Find(HMONITOR monitor) {
    HWND taskbar = TaskbarOn(monitor);
    if (!taskbar) return Found::NoTaskbar;
    if (Classic()) return Found::Classic;
    KeepMta();
    {
        std::lock_guard<std::mutex> lk(S().book);
        const Entry& e = At(taskbar);
        if (e.kept) return Found::Kept;
        if (MissedRecently(e, NowSeconds())) return Found::NoButton;
    }
    return Look(taskbar, false);
}

bool Kept(HMONITOR monitor) {
    HWND taskbar = TaskbarOn(monitor);
    if (!taskbar) return false;
    std::lock_guard<std::mutex> lk(S().book);
    for (const auto& e : S().entries) if (e.taskbar == taskbar) return e.kept;
    return false;
}

Pressed Press(HMONITOR monitor) {
    Pressed p;
    HWND taskbar = TaskbarOn(monitor);
    if (!taskbar) { p.hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); p.how = L"no taskbar on this monitor"; return p; }
    KeepMta();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const Found f = Find(monitor);
        if (f != Found::Kept && f != Found::Now) { p.hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND); p.how = FoundText(f); return p; }
        ComPtr<IUIAutomationElement> button;
        {
            std::lock_guard<std::mutex> lk(S().book);
            button = At(taskbar).button;
        }
        if (!button) { p.how = L"the button went away meanwhile"; continue; }   // dropped by another thread: look again
        After a = After::NotDone;
        {
            std::lock_guard<std::mutex> call(S().calls);
            const double c0 = NowSeconds();   // the press itself, not the wait for the lock or a lookup
            a = PressElement(button.Get(), &p.hr, &p.how);
            p.took = NowSeconds() - c0;
        }
        p.ok = a == After::Done;
        p.unsure = a == After::Unsure || a == After::UnsureGone;
        if (a == After::Gone || a == After::UnsureGone) Drop(taskbar);
        // Only a button that is gone was surely not pressed: looked up once more. Anything else - a timeout
        // above all, which may still reach Explorer - is never pressed a second time.
        if (a != After::Gone) return p;
    }
    return p;
}

std::wstring Describe() {
    std::lock_guard<std::mutex> lk(S().book);
    std::wstring s;
    for (const auto& e : S().entries) {
        if (!s.empty()) s += L"; ";
        wchar_t b[200];
        if (e.kept) _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"%ls %p: kept (found %ls)", Name(e.taskbar), static_cast<void*>(e.taskbar), e.from);
        else if (e.missedAt >= 0) _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"%ls %p: not found %.0f s ago", Name(e.taskbar), static_cast<void*>(e.taskbar), NowSeconds() - e.missedAt);
        else _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"%ls %p: not looked up yet", Name(e.taskbar), static_cast<void*>(e.taskbar));
        s += b;
    }
    return s.empty() ? L"none kept" : s;
}

}   // namespace taskbarstart
