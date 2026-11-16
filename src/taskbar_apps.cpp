#include "taskbar_apps.h"
#include "taskbar_apps_logic.h"
#include "explorer_taskbar.h"
#include <uiautomation.h>
#include <shlobj.h>
#include <algorithm>
#include <mutex>
#include <thread>

// What taskbar_apps.h says. The calls into Explorer follow the ones taskbar_start.cpp makes for the Start button
// (the same per-call timeouts, the same reading of a failed call), kept apart so the Start button's tested code
// stays as it is. The press differs on purpose: an app button is pressed by Invoke or Toggle only (ReadWays),
// where the Start button also falls back to the accessibility default action.

namespace taskbarapps {
namespace {

constexpr DWORD kUiaTimeoutMs = 500;      // per call into a busy Explorer (connection, transaction)
constexpr double kLookupBudget = 1.5;     // a lookup tries no further root once this long has gone (a hung Explorer)
constexpr HRESULT kUiaTimeout = static_cast<HRESULT>(0x80131505);   // UIA_E_TIMEOUT (not in every SDK's headers)

struct State {
    std::mutex calls;               // calls into UI Automation, one at a time; uia
    std::mutex book;                // done
    ComPtr<IUIAutomation> uia;
    std::vector<Result> done;
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

// The element was surely not reached: it is gone (Explorer rebuilt its taskbar, or restarted).
bool Gone(HRESULT hr) {
    return hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) || hr == RPC_E_DISCONNECTED || hr == RPC_E_SERVER_DIED_DNE ||
           hr == CO_E_OBJNOTCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED_DNE);
}
// The call failed, and may have run all the same (the call timed out, or Explorer went away during it).
bool MayHaveRun(HRESULT hr) {
    return hr == kUiaTimeout || hr == RPC_E_TIMEOUT || hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) || hr == RPC_E_SERVER_DIED ||
           hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED);
}

// A press that may have run is Unsure, whether Explorer went away during it or not: no taskbar is kept here to
// drop (taskbar_start.cpp tells the two apart for that).
enum class After : uint8_t { Done, NotDone, Gone, Unsure };
After Classify(HRESULT hr, bool pressCall) {
    if (SUCCEEDED(hr)) return After::Done;
    if (Gone(hr)) return After::Gone;
    if (MayHaveRun(hr)) {
        if (pressCall) return After::Unsure;
        const bool dead = hr == RPC_E_SERVER_DIED || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED);
        return dead ? After::Gone : After::NotDone;   // no press was made yet
    }
    return After::NotDone;
}

// The ways UI Automation offers to press it: Invoke, else Toggle. Not the accessibility default action ("누르기"),
// all an app button offers on a 25H2 build: there it answered S_OK and the taskbar did nothing - no window
// came up, went down or was restored, none got a message - with the taskbar hidden or shown and Explorer given the
// right to the front (measured 2026-10-27; tests\run_taskbar_apps_diag.bat repeats the measurement: it presses by it
// and records what follows).
// Read before a press is claimed: reading is no press, so the dock may still go its own way meanwhile.
struct Ways {
    ComPtr<IUIAutomationInvokePattern> invoke;
    ComPtr<IUIAutomationTogglePattern> toggle;
};
HRESULT ReadWays(IUIAutomationElement* e, Ways& w) {
    HRESULT hr = e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&w.invoke));
    if (SUCCEEDED(hr) && !w.invoke) hr = e->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&w.toggle));
    return hr;
}

// The press, by one of them. Only a failure of the press itself may have reached Explorer.
After PressElement(const Ways& w, HRESULT* hr, const wchar_t** how) {
    if (w.invoke) { *how = L"invoke"; *hr = w.invoke->Invoke(); }
    else { *how = L"toggle"; *hr = w.toggle->Toggle(); }
    return Classify(*hr, true);
}

// Windows 10's taskbar (or ExplorerPatcher's classic one): no XAML buttons. As taskbar_start.cpp tells them
// apart: the classic tray's hidden-icons button (1502).
bool Classic() {
    HWND main = ExplorerTaskbar();
    HWND notify = main ? FindWindowExW(main, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    return notify && GetDlgItem(notify, 1502);
}

// Explorer's taskbars, the clicked monitor's first (its main one is on the primary monitor; a secondary one by
// its window's - near enough here: the others are looked at too, for a window shown only on its own monitor's).
std::vector<HWND> Taskbars(HMONITOR monitor) {
    std::vector<HWND> all;
    HWND main = ExplorerTaskbar();
    if (!main) return all;
    DWORD explorer = 0;
    GetWindowThreadProcessId(main, &explorer);
    all.push_back(main);
    HWND s = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr);
    for (int guard = 0; s && guard < 16; ++guard, s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(s, &pid);
        if (pid == explorer) all.push_back(s);
    }
    const HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    std::stable_partition(all.begin(), all.end(), [&](HWND t) {
        return t == main ? monitor == primary : MonitorFromWindow(t, MONITOR_DEFAULTTONEAREST) == monitor;
    });
    return all;
}

BOOL CALLBACK AddIsland(HWND w, LPARAM p) {
    wchar_t cls[96] = L"";
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") || !wcscmp(cls, L"Windows.UI.Input.InputSite.WindowClass"))
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(w);
    return TRUE;
}

// Where to look: the taskbar's XAML island's windows (they reach the buttons while the taskbar is hidden; from
// the taskbar's own window the tree stops at its Win32 children then), then the taskbar's window.
std::vector<HWND> Roots(HWND taskbar) {
    std::vector<HWND> roots;
    EnumChildWindows(taskbar, AddIsland, reinterpret_cast<LPARAM>(&roots));
    if (roots.size() > 8) roots.resize(8);
    roots.push_back(taskbar);
    return roots;
}

// The app buttons under one root, each with its id and name fetched in the same call: S_OK when none are there
// (found empty, or null), else the failed call's error (Explorer busy, the root gone meanwhile). calls held.
HRESULT ButtonsUnder(IUIAutomation* uia, HWND root, ComPtr<IUIAutomationElementArray>& found) {
    ComPtr<IUIAutomationElement> top;
    HRESULT hr = uia->ElementFromHandle(root, &top);
    if (FAILED(hr) || !top) return FAILED(hr) ? hr : E_FAIL;
    ComPtr<IUIAutomationCondition> cond;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocStringLen(kButtonClass.data(), static_cast<UINT>(kButtonClass.size()));
    hr = v.bstrVal ? uia->CreatePropertyCondition(UIA_ClassNamePropertyId, v, &cond) : E_OUTOFMEMORY;
    VariantClear(&v);
    if (FAILED(hr) || !cond) return FAILED(hr) ? hr : E_FAIL;
    ComPtr<IUIAutomationCacheRequest> cache;
    hr = uia->CreateCacheRequest(&cache);
    if (FAILED(hr) || !cache) return FAILED(hr) ? hr : E_FAIL;
    cache->AddProperty(UIA_AutomationIdPropertyId);
    cache->AddProperty(UIA_NamePropertyId);
    return top->FindAllBuildCache(TreeScope_Descendants, cond.Get(), cache.Get(), &found);
}

std::wstring Text(BSTR b) { return b ? std::wstring(b, SysStringLen(b)) : std::wstring(); }

// As Windows lowers text (the dock's paths are lowered so): SameText then needs no case of its own.
std::wstring Lowered(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty()) CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

struct Walk {
    int seen = 0;            // app buttons seen
    bool cut = false;        // stopped early (stop())
    HRESULT error = S_OK;    // the first root that could not be looked into
};

// Each app button: fn(element, id without "Appid: ", name, taskbar) until it returns true. The first root of a
// taskbar that has buttons has them all. stop(): no further root is looked at. calls held.
template <class Stop, class Fn>
Walk EachButton(IUIAutomation* uia, const std::vector<HWND>& bars, Stop stop, Fn fn) {
    Walk w;
    for (HWND bar : bars) {
        for (HWND root : Roots(bar)) {
            if (stop()) { w.cut = true; return w; }
            ComPtr<IUIAutomationElementArray> arr;
            const HRESULT hr = ButtonsUnder(uia, root, arr);
            if (FAILED(hr) && SUCCEEDED(w.error)) w.error = hr;
            int n = 0;
            if (FAILED(hr) || !arr || FAILED(arr->get_Length(&n)) || n <= 0) continue;
            w.seen += n;
            for (int i = 0; i < n; ++i) {
                ComPtr<IUIAutomationElement> e;
                if (FAILED(arr->GetElement(i, &e)) || !e) continue;
                BSTR id = nullptr, name = nullptr;
                e->get_CachedAutomationId(&id);
                e->get_CachedName(&name);
                const std::wstring idText = Text(id), nameText = Text(name);
                SysFreeString(id);
                SysFreeString(name);
                const std::wstring_view appId = AppIdOf(idText);
                if (!appId.empty() && fn(e, appId, nameText, bar)) return w;
            }
            break;   // this taskbar's buttons, all of them
        }
    }
    return w;
}

// MTA thread.
Result Run(const Job& job) {
    Result r;
    r.job = job.id;
    std::atomic<int>& stage = *job.stage;
    auto abandoned = [&] { return stage.load() == kAbandoned; };
    if (Classic()) { r.outcome = Outcome::Classic; r.how = L"Windows' classic taskbar"; return r; }
    const std::vector<HWND> bars = Taskbars(job.monitor);
    if (bars.empty()) { r.outcome = Outcome::NoTaskbar; r.how = L"no taskbar"; return r; }
    std::lock_guard<std::mutex> call(S().calls);   // held by an earlier press into a hung Explorer: the dock may have gone its own way meanwhile
    if (abandoned()) { r.outcome = Outcome::Abandoned; return r; }
    IUIAutomation* uia = Uia();
    if (!uia) { r.outcome = Outcome::Failed; r.how = L"no UI Automation"; return r; }
    const std::wstring aumid = Lowered(job.aumid), otherAumid = Lowered(job.otherAumid), exe = Lowered(job.exe);
    const KnownFolder folder = [](std::wstring_view id) { return Lowered(KnownFolderPath(id)); };
    for (int attempt = 0; attempt < 2; ++attempt) {
        const double t0 = NowSeconds();
        ComPtr<IUIAutomationElement> mine;
        bool other = false;   // a button of the app's, but not the one meant (CountFits)
        const Walk w = EachButton(uia, bars, [&] { return abandoned() || NowSeconds() > t0 + kLookupBudget; },
                                  [&](const ComPtr<IUIAutomationElement>& e, std::wstring_view id, const std::wstring& name, HWND bar) {
            const std::wstring low = Lowered(id);
            if (!Names(low, aumid, exe, folder) && (otherAumid.empty() || !Names(low, otherAumid, exe, folder))) return false;
            const int count = WindowCount(name);
            if (!CountFits(job.running, count)) {   // a pinned copy of a running app's button, say: looked further (another taskbar's)
                if (!other) { other = true; r.count = count; r.button = std::wstring(id); r.name = name; }
                return false;
            }
            mine = e;
            r.count = count;
            r.button = std::wstring(id);
            r.name = name;
            r.taskbar = bar;
            return true;
        });
        r.buttons = w.seen;
        r.lookup += NowSeconds() - t0;
        if (!mine) {
            if (abandoned()) r.outcome = Outcome::Abandoned;
            else if (w.cut) { r.outcome = Outcome::Failed; r.slow = true; r.how = L"Explorer too slow to answer"; }   // not "no button": not looked at all
            else if (other) { r.outcome = Outcome::CountDiffers; r.how = L"its button counts other windows"; }
            else if (!w.seen && FAILED(w.error)) {   // no button seen as no root could be looked into: not "no button" either
                r.outcome = Outcome::Failed;
                r.hr = w.error;
                r.slow = MayHaveRun(w.error);   // timed out: Explorer busy
                r.how = L"the taskbar could not be looked into";
            } else { r.outcome = Outcome::NoButton; r.how = L"no button names the app"; }
            return r;
        }
        Ways ways;
        r.hr = ReadWays(mine.Get(), ways);
        if (FAILED(r.hr)) {   // nothing pressed, nothing claimed
            r.how = L"its patterns could not be read";
            if (Classify(r.hr, false) == After::Gone) continue;   // Explorer rebuilt its taskbar's content meanwhile: looked up once more
            r.outcome = abandoned() ? Outcome::Abandoned : Outcome::Failed;
            r.slow = MayHaveRun(r.hr);   // a pattern read timed out: Explorer busy
            return r;
        }
        if (!ways.invoke && !ways.toggle) {   // nothing to press it by (taskbar_apps_logic.h NoWay)
            r.hr = E_NOINTERFACE;
            r.how = L"no Invoke or Toggle pattern (its accessibility default action is not tried: on a 25H2 build it answered "
                    L"S_OK and did nothing)";
            r.outcome = abandoned() ? Outcome::Abandoned : Outcome::NoWay;
            return r;
        }
        if (!ClaimPress(stage)) { r.outcome = Outcome::Abandoned; return r; }   // the dock went its own way meanwhile: no press now
        const double c0 = NowSeconds();
        const After a = PressElement(ways, &r.hr, &r.how);
        r.took = NowSeconds() - c0;
        switch (a) {
        case After::Done: r.outcome = Outcome::Pressed; return r;
        case After::Unsure: r.outcome = Outcome::Unsure; return r;
        case After::NotDone: r.outcome = Outcome::Failed; return r;   // refused
        case After::Gone: LookAgain(stage); break;   // Explorer rebuilt its taskbar's content meanwhile: looked up once more
        }
    }
    r.outcome = abandoned() ? Outcome::Abandoned : Outcome::Failed;
    r.how = L"the button went away twice";
    return r;
}

}   // namespace

const wchar_t* OutcomeText(Outcome o) {
    switch (o) {
    case Outcome::Pressed: return L"pressed";
    case Outcome::Unsure: return L"unsure whether the press arrived";
    case Outcome::NoTaskbar: return L"no taskbar";
    case Outcome::Classic: return L"Windows' classic taskbar";
    case Outcome::NoWay: return L"its button offers no press that works";
    case Outcome::NoButton: return L"no button of its on the taskbar";
    case Outcome::CountDiffers: return L"its button counts other windows than the dock does";
    case Outcome::Failed: return L"the press did not go through";
    case Outcome::Abandoned: return L"the dock did the click itself meanwhile";
    }
    return L"?";
}

std::wstring KnownFolderPath(std::wstring_view id) {
    GUID g{};
    const std::wstring text(id);
    if (FAILED(IIDFromString(text.c_str(), &g))) return L"";
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(g, KF_FLAG_DONT_VERIFY, nullptr, &path)) && path) out = path;
    CoTaskMemFree(path);
    return out;
}

Result PressNow(const Job& job) {
    KeepMta();
    return Run(job);
}

bool Begin(const Job& job) {
    try {
        std::thread([job] {
            Result r;
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(co)) {
                KeepMta();
                r = Run(job);
                CoUninitialize();
            } else {
                r.job = job.id;
                r.hr = co;
                r.how = L"no apartment for UI Automation";
            }
            if (r.outcome == Outcome::Abandoned) return;   // no one waits for it
            {
                std::lock_guard<std::mutex> lk(S().book);
                auto& done = S().done;
                if (done.size() >= 16) done.erase(done.begin());   // results no one came for (a click the dock forgot)
                done.push_back(std::move(r));
            }
            if (job.wake) PostMessageW(job.wake, WM_NULL, 0, 0);
        }).detach();
        return true;
    } catch (...) {
        return false;
    }
}

bool Take(uint32_t job, Result& out) {
    std::lock_guard<std::mutex> lk(S().book);
    auto& done = S().done;
    for (auto it = done.begin(); it != done.end(); ++it)
        if (it->job == job) {
            out = std::move(*it);
            done.erase(it);
            return true;
        }
    return false;
}

std::vector<Button> List(double* took) {
    KeepMta();
    std::vector<Button> out;
    const double t0 = NowSeconds();
    {
        std::lock_guard<std::mutex> call(S().calls);
        if (IUIAutomation* uia = Uia()) {
            const std::vector<HWND> bars = Taskbars(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
            EachButton(uia, bars, [&] { return NowSeconds() > t0 + kLookupBudget * 2; },
                       [&](const ComPtr<IUIAutomationElement>&, std::wstring_view id, const std::wstring& name, HWND bar) {
                out.push_back({ std::wstring(id), name, bar });
                return false;
            });
        }
    }
    if (took) *took = NowSeconds() - t0;
    return out;
}

}   // namespace taskbarapps
