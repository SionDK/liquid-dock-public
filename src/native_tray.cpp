#include "native_tray.h"
#include "system.h"
#include "explorer_taskbar.h"
#include "taskbar_visibility.h"
#include "tray_buttons.h"
#include <shellapi.h>
#include <shldisp.h>
#include <uiautomation.h>

// The native taskbar stays "shown" (just invisible, see NativeTaskbar), so Explorer's own tray
// windows open normally when their buttons are pressed.
namespace nativetray {

bool ToggleDesktop() {
    // Same Shell action as the native taskbar button. No synthetic key presses,
    // per-window minimization, or locally maintained restore list.
    ComPtr<IShellDispatch4> shell;
    HRESULT hr = CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&shell));
    if (SUCCEEDED(hr)) hr = shell->ToggleDesktop();
    if (FAILED(hr)) Log(L"show desktop failed: %08x", hr);
    return SUCCEEDED(hr);
}

// Stock Windows 11 taskbar (XAML): its tray buttons are only reachable through UI Automation.
// The overflow host can be positioned after Explorer creates it.
//
// Once the taskbar auto-hides (as the dock makes it) its XAML tray content leaves the automation tree
// rooted at the taskbar's window: the XAML island's child window is hidden then, and the tree does not
// reach into a hidden window. The content stays reachable from the island's windows though
// (a 26H2 build, 2026-11-12: 3 descendants and no tray button from the taskbar's window,
// the whole tray from its island). And with Windows' own auto-hide on, the taskbar is hidden from
// logon: there is no expanded moment to look in. So the network and clock buttons are looked for from
// the taskbar's window and then from its island's windows (FindButton): before the taskbar vanishes
// (CacheTrayButtons), and at a click while none is kept. An element found keeps working while the
// taskbar is hidden: so the ones found before it vanished have been pressed, and so the Start button
// found from the island is (taskbar_start.cpp). The hidden-icons button keeps its own way: the
// taskbar expanded for a moment, the button looked for from its window (OpenOverflowWin11).
enum Button { Chevron, Network, Clock, ButtonCount };
static const wchar_t* kButtonClass[ButtonCount] = { L"SystemTray.NormalButton",    // first one: hidden icons
                                                    L"SystemTray.AccentButton",    // network / quick settings
                                                    L"SystemTray.OmniButton" };    // clock / calendar
static ComPtr<IUIAutomation> g_uia;
static ComPtr<IUIAutomationElement> g_button[ButtonCount];
static const wchar_t* g_buttonFrom[ButtonCount] = { L"", L"", L"" };   // where each kept one was found (for the log)
static HWND g_buttonTray = nullptr;
// A lookup starts with no further window once this long has gone (a hung Explorer: the UI thread waits)
constexpr double kLookupBudget = 0.6;

static ComPtr<IUIAutomationCondition> ClassIs(const wchar_t* cls) {
    ComPtr<IUIAutomationCondition> c;
    VARIANT v; v.vt = VT_BSTR; v.bstrVal = SysAllocString(cls);
    g_uia->CreatePropertyCondition(UIA_ClassNamePropertyId, v, &c);
    VariantClear(&v);
    return c;
}

static std::wstring Take(BSTR b) { std::wstring s = b ? b : L""; SysFreeString(b); return s; }

// The hidden-icons chevron among the NormalButtons (tray_buttons.h decides: on Windows 11 26H2 the
// app icons shown on the taskbar are NormalButtons too).
// NormalButtons on FindChevron's last look: 0 also while the tray's content is not there; -1 the look
// failed (Explorer not answering)
static int g_trayButtonsSeen = -1;
static ComPtr<IUIAutomationElement> FindChevron(HWND tray) {
    static bool saidNone = false;
    g_trayButtonsSeen = -1;
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationCondition> cls = ClassIs(traybuttons::kNormal);
    ComPtr<IUIAutomationElementArray> all;
    if (!cls || FAILED(g_uia->ElementFromHandle(tray, &root)) || !root ||
        FAILED(root->FindAll(TreeScope_Descendants, cls.Get(), &all)) || !all) return nullptr;
    int n = 0;
    all->get_Length(&n);
    std::vector<traybuttons::Facts> facts;
    std::vector<ComPtr<IUIAutomationElement>> elements;
    for (int i = 0; i < n && i < 64; ++i) {
        ComPtr<IUIAutomationElement> e;
        if (FAILED(all->GetElement(i, &e)) || !e) continue;
        BSTR id = nullptr;
        e->get_CurrentAutomationId(&id);
        facts.push_back({ traybuttons::kNormal, Take(id) });
        elements.push_back(e);
    }
    g_trayButtonsSeen = (int)facts.size();
    int k = traybuttons::ChevronIndex(facts);
    if (k < 0) {
        if (!saidNone && !facts.empty()) {   // (none at all: the taskbar's content is not there yet)
            saidNone = true;
            Log(L"tray: no hidden-icons button among %d tray buttons (no hidden icons, or Windows' hidden icon menu is off)", (int)facts.size());
        }
        return nullptr;
    }
    BSTR name = nullptr;
    elements[k]->get_CurrentName(&name);
    Log(L"tray: hidden-icons button = %d of %d (\"%ls\", %ls)", k + 1, (int)facts.size(), Take(name).c_str(),
        facts[k].automationId.empty() ? L"no automation id" : facts[k].automationId.c_str());
    return elements[k];
}

static BOOL CALLBACK AddIsland(HWND w, LPARAM p) {   // (taskbar_start.cpp looks for the Start button from the same windows)
    wchar_t cls[96] = L"";
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") || !wcscmp(cls, L"Windows.UI.Input.InputSite.WindowClass"))
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(w);
    return TRUE;
}

// The windows the network and clock buttons are looked for from: the taskbar's, then its XAML island's
// (hidden ones too - they are hidden while the taskbar is; at most 8).
static std::vector<HWND> Roots(HWND tray) {
    std::vector<HWND> roots{ tray };
    EnumChildWindows(tray, AddIsland, reinterpret_cast<LPARAM>(&roots));
    if (roots.size() > 9) roots.resize(9);
    return roots;
}

// The network or clock button: from the taskbar's window (always asked), then from its island's windows
// while NowSeconds() < until. The class must match exactly: the volume button is a
// "SystemTray.OmniButtonRight". The chevron only ever from the taskbar's window (FindChevron).
static ComPtr<IUIAutomationElement> FindButton(HWND tray, Button b, double until) {
    if (b == Chevron) {
        ComPtr<IUIAutomationElement> chevron = FindChevron(tray);
        if (chevron) g_buttonFrom[b] = L"from the taskbar window";
        return chevron;
    }
    ComPtr<IUIAutomationCondition> cls = ClassIs(kButtonClass[b]);
    if (!cls) return nullptr;
    const std::vector<HWND> roots = Roots(tray);
    for (size_t i = 0; i < roots.size() && (!i || NowSeconds() < until); ++i) {
        ComPtr<IUIAutomationElement> root, btn;
        if (SUCCEEDED(g_uia->ElementFromHandle(roots[i], &root)) && root &&
            SUCCEEDED(root->FindFirst(TreeScope_Descendants, cls.Get(), &btn)) && btn) {
            g_buttonFrom[b] = i ? L"from its XAML island" : L"from the taskbar window";
            return btn;
        }
    }
    return nullptr;
}

static bool ClassicTray(HWND tray) {   // classic (Windows 10 style) taskbar, no XAML buttons: it has the classic hidden-icons button
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    return notify && GetDlgItem(notify, 1502);
}

// The buttons from `first` to `last` that are not cached yet, looked up now: the first always, the
// others while NowSeconds() < until.
static void CacheButtons(HWND tray, Button first, Button last, double until = NowSeconds() + kLookupBudget) {
    if (ClassicTray(tray)) return;
    if (g_buttonTray != tray) {
        for (auto& b : g_button) b.Reset();
        for (auto& from : g_buttonFrom) from = L"";
        g_buttonTray = tray;
    }
    if (!g_uia) {
        // CUIAutomation8 takes the call timeouts; CUIAutomation (Windows 7's, asked for here until mod.27) has no
        // IUIAutomation2, so the 0.5 s below was never set and a call into a hung Explorer waited as long as it hung
        // (measured on a 26H2 build, 2026-11-12). AutoSetFocus is left as it was: these presses worked with it on.
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uia))) &&
            FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uia)))) { g_uia.Reset(); return; }
        // a busy explorer must not freeze the dock: give up quickly
        ComPtr<IUIAutomation2> u2;
        if (SUCCEEDED(g_uia.As(&u2))) { u2->put_ConnectionTimeout(500); u2->put_TransactionTimeout(500); }
    }
    for (int i = first; i <= last && (i == first || NowSeconds() < until); ++i)
        if (!g_button[i] && (g_button[i] = FindButton(tray, (Button)i, until))) Log(L"tray button %d cached (found %ls)", i, g_buttonFrom[i]);
}

void CacheTrayButtons(HWND tray) {
    CacheButtons(tray, Chevron, Clock);
    // (the hidden-icons button is not expected here: it is found once the taskbar is expanded for its press)
    static bool saidNone = false;
    if (!saidNone && !(g_buttonTray == tray && (g_button[Network] || g_button[Clock])) && !ClassicTray(tray)) {
        saidNone = true;
        Log(L"tray: no clock or network button in the taskbar's automation tree yet (its window and %d island windows): looked for at the click",
            (int)Roots(tray).size() - 1);
    }
}

// Whether the tray's content is in the automation tree now: its clock or network button, looked up
// anew in one search (one found earlier keeps working while the taskbar is hidden, so it tells nothing).
static bool TrayContentThere(HWND tray) {
    if (!g_uia) return false;
    ComPtr<IUIAutomationCondition> clock = ClassIs(kButtonClass[Clock]), network = ClassIs(kButtonClass[Network]), either;
    ComPtr<IUIAutomationElement> root, btn;
    return clock && network && SUCCEEDED(g_uia->CreateOrCondition(clock.Get(), network.Get(), &either)) && either &&
           SUCCEEDED(g_uia->ElementFromHandle(tray, &root)) && root &&
           SUCCEEDED(root->FindFirst(TreeScope_Descendants, either.Get(), &btn)) && btn != nullptr;
}

static bool InvokeButton(Button b) {
    ComPtr<IUIAutomationInvokePattern> inv;
    if (g_button[b] && SUCCEEDED(g_button[b]->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv && SUCCEEDED(inv->Invoke())) return true;
    g_button[b].Reset();   // stale (explorer restarted?)
    return false;
}

// Presses the stock taskbar's chevron. NoButton: Windows shows none (nothing hidden, or its hidden
// icon menu turned off).
static OverflowFailure OpenOverflowWin11(HWND tray) {
    bool cached = g_buttonTray == tray && g_button[Chevron], ok = cached && InvokeButton(Chevron), found = cached;
    if (!ok) {
        // Materialize Explorer's automation content only under the opacity guard
        // acquired by OpenOverflow. An empty region alone is NOT sufficient:
        // Explorer rebuilds it while expanding its taskbar.
        APPBARDATA abd = { sizeof(abd) }; abd.hWnd = tray;
        UINT_PTR st = SHAppBarMessage(ABM_GETSTATE, &abd);
        bool autoHide = (st & ABS_AUTOHIDE) != 0;
        if (autoHide) { abd.lParam = st & ~ABS_AUTOHIDE; SHAppBarMessage(ABM_SETSTATE, &abd); }
        // Up to 40 looks (about 1.5 s) while Explorer builds the content. Windows shows no chevron
        // (NoButton) once the content is there - tray buttons, or else the clock / network button - and
        // has stayed the same for kSettleMs, over three looks or more (not still being built: the chevron
        // may come a moment after the other buttons); else every press would take all 40 looks, on the
        // UI thread, the taskbar not auto-hidden meanwhile. Timed, not counted: a look's length and
        // Sleep's granularity vary.
        constexpr ULONGLONG kSettleMs = 200;
        int seen = -1, same = 0;
        ULONGLONG since = 0;   // when the content was first seen as it is now
        bool settled = false;
        for (int i = 0; i < 40 && !g_button[Chevron]; ++i) {
            g_trayButtonsSeen = -1;
            // the chevron every look; the network and clock buttons not cached yet only once the content is
            // there (while it is not, each would be looked for in vain: one more question to Explorer a look)
            CacheButtons(tray, Chevron, Chevron);
            const int n = g_trayButtonsSeen;
            const bool there = g_button[Chevron] || n > 0 || (n == 0 && TrayContentThere(tray));   // (no extra look when Explorer does not answer)
            if (there && (!g_button[Network] || !g_button[Clock])) CacheButtons(tray, Network, Clock);
            if (g_button[Chevron]) break;
            const ULONGLONG now = GetTickCount64();
            same = !there ? 0 : n == seen ? same + 1 : 1;
            if (same == 1) since = now;
            seen = n;
            settled = same >= 3 && now - since >= kSettleMs;
            if (settled) break;
            Sleep(25);
        }
        static bool saidEmpty = false;   // (with tray buttons FindChevron says so)
        if (!g_button[Chevron] && settled && seen == 0 && !saidEmpty) {
            saidEmpty = true;
            Log(L"tray: no hidden-icons button, no tray buttons at all (no hidden icons, or Windows' hidden icon menu is off)");
        }
        found = found || g_button[Chevron] != nullptr;
        ok = InvokeButton(Chevron);
        if (autoHide) { abd.lParam = st; SHAppBarMessage(ABM_SETSTATE, &abd); }
    }
    Log(L"win11 overflow: cached=%d invoked=%d", (int)cached, (int)ok);
    if (ok) return OverflowFailure::None;
    return found ? OverflowFailure::NotOpened : OverflowFailure::NoButton;
}

// The flyout's window, once shown (its first opening in a session makes it, which can take a while).
static HWND WaitShown(const wchar_t* cls, DWORD ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    for (;;) {
        HWND w = FindWindowW(cls, nullptr);
        if (w && IsWindowVisible(w)) return w;
        if (GetTickCount64() >= until) return nullptr;
        Sleep(15);
    }
}

bool OpenOverflow(POINT anchor, Edge edge, RECT bar, OverflowFailure* why) {
    OverflowFailure failure = OverflowFailure::NotOpened;
    bool ok = g_taskbar.WithInvisibleTaskbars([&]() -> bool {
    HWND tray = ExplorerTaskbar();
    if (!tray) return false;
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    HWND chevron = notify ? GetDlgItem(notify, 1502) : nullptr;   // classic (Windows 10 style) taskbar
    const wchar_t* popupClass = chevron ? L"NotifyIconOverflowWindow" : L"TopLevelWindowForOverflowXamlIsland";
    HWND previous = FindWindowW(popupClass, nullptr);
    bool wasOpen = previous && IsWindowVisible(previous);
    if (!chevron) {
        OverflowFailure f = OpenOverflowWin11(tray);
        if (f != OverflowFailure::None) { failure = f; return false; }
    } else {
        DWORD_PTR r = 0;
        if (!SendMessageTimeoutW(chevron, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 500, &r)) return false;
    }

    // A second click closes the existing flyout. Do not reopen or reposition it.
    if (wasOpen) { failure = OverflowFailure::None; return true; }
    HWND ov = WaitShown(popupClass, 1000);
    if (!ov && !chevron) {
        // the stock taskbar sometimes takes a second press; not if the flyout has just come after all
        if (!(ov = WaitShown(popupClass, 0)) && InvokeButton(Chevron)) ov = WaitShown(popupClass, 1000);
        Log(L"win11 overflow: %ls", ov ? L"shown after a second press" : L"no hidden-icons window came");
    }
    if (!ov) return false;
    RECT wr{}; if (!GetWindowRect(ov, &wr)) return false;
    int w = wr.right - wr.left, h = wr.bottom - wr.top, gap = 12;
    int x = anchor.x - w / 2, y = anchor.y - h / 2;
    switch (edge) {
    case Edge::Bottom: y = bar.top - h - gap; break;
    case Edge::Top:    y = bar.bottom + gap; break;
    case Edge::Left:   x = bar.right + gap; break;
    case Edge::Right:  x = bar.left - w - gap; break;
    }
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    x = std::clamp<int>(x, mi.rcMonitor.left + 4, std::max(mi.rcMonitor.left + 4, mi.rcMonitor.right - w - 4));
    y = std::clamp<int>(y, mi.rcMonitor.top + 4, std::max(mi.rcMonitor.top + 4, mi.rcMonitor.bottom - h - 4));
    if (!SetWindowPos(ov, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE))   // shown: placing it is a nicety
        Log(L"overflow: shown where Windows put it (not moved next to the dock: %lu)", GetLastError());
    failure = OverflowFailure::None;
    return true;
    });
    if (why) *why = ok ? OverflowFailure::None : failure;
    return ok;
}

// ---- Windows' calendar (its notification centre) and quick settings, by the stock taskbar's own clock /
// network button: Explorer then does what a click on its taskbar does, and a second click closes the
// flyout the same way (the button toggles by itself). The shortcut only when the button cannot be
// pressed. Both ways toggle, so a click never gets both: a press that may still arrive would open the
// flyout and the keys shut it.

constexpr HRESULT kUiaTimeout = static_cast<HRESULT>(0x80131505);   // UIA_E_TIMEOUT (not in every SDK's headers)
// What a failed call means, told as taskbar_start.cpp tells it for the Start button.
// The element was surely not reached: it is gone (Explorer rebuilt its taskbar, or restarted). Look again.
static bool Gone(HRESULT hr) {
    return hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) || hr == RPC_E_DISCONNECTED || hr == RPC_E_SERVER_DIED_DNE ||
           hr == CO_E_OBJNOTCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED_DNE);
}
// The call failed, and may have run all the same (the call timed out, or Explorer went away during it).
static bool MayHaveRun(HRESULT hr) {
    return hr == kUiaTimeout || hr == RPC_E_TIMEOUT || hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) || hr == RPC_E_SERVER_DIED ||
           hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED);
}
// How a press went. pressCall: the press itself (Invoke) was called - only its failure may have reached
// Explorer. Gone: surely not pressed, and the element is no more (worth one more lookup when it was a kept
// one). Unsure: the press may still arrive - nothing may follow it; UnsureGone: and Explorer went away.
// Not as for the Start button: a failure before the press call that is no timeout counts as gone too - an
// island's window gone under its elements answers a plain E_FAIL once their patterns are asked for
// (tests\tray_overflow_test.cpp), and no press has been made that a second one could undo.
enum class After : uint8_t { Done, NotDone, Gone, Unsure, UnsureGone };
static After Classify(HRESULT hr, bool pressCall) {
    if (SUCCEEDED(hr)) return After::Done;
    if (Gone(hr)) return After::Gone;
    if (MayHaveRun(hr)) {
        const bool dead = hr == RPC_E_SERVER_DIED || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED);
        if (!pressCall) return dead ? After::Gone : After::NotDone;   // no press was made yet
        return dead ? After::UnsureGone : After::Unsure;
    }
    return pressCall ? After::NotDone : After::Gone;
}

// pressed: its Invoke was accepted. unsure: the press was made and may still arrive (nothing may follow
// it). Neither: surely not pressed - found tells whether there was a button at all. kept: the one pressed
// (or tried) had been kept before the click.
struct TrayPress { bool pressed = false, unsure = false, found = false, kept = false; HRESULT hr = S_OK; };

// One press of the network or clock button: the kept one, else looked up now (until: as FindButton). A
// kept one that is gone (Explorer rebuilt its taskbar) was surely not pressed: looked up once more and
// pressed once. Nothing is ever pressed a second time.
static TrayPress PressButton(HWND tray, Button b, double until) {
    TrayPress p;
    for (int attempt = 0; attempt < 2; ++attempt) {
        p.kept = g_buttonTray == tray && g_button[b];
        if (!p.kept) CacheButtons(tray, b, b, until);
        // (its own reference: the calls below wait on Explorer, and the UI thread may dispatch messages meanwhile)
        ComPtr<IUIAutomationElement> button = g_buttonTray == tray ? g_button[b] : nullptr;
        p.found = button != nullptr;
        if (!button) return p;
        ComPtr<IUIAutomationInvokePattern> invoke;
        p.hr = button->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke));
        if (SUCCEEDED(p.hr) && !invoke) p.hr = E_NOINTERFACE;
        if (invoke) p.hr = invoke->Invoke();
        const After a = Classify(p.hr, invoke != nullptr);
        if (a == After::Done) { p.pressed = true; return p; }
        if (a == After::Unsure || a == After::UnsureGone) {
            if (a == After::UnsureGone) g_button[b].Reset();   // Explorer's next taskbar has another button
            p.unsure = true;
            return p;
        }
        g_button[b].Reset();   // surely not pressed
        if (!p.kept || a != After::Gone) break;
        Log(L"tray: the kept %ls button was gone (%08x; Explorer rebuilt its taskbar): looked up again", b == Clock ? L"clock" : L"network", (unsigned)p.hr);
    }
    return p;
}

struct TrayFlyout {
    const wchar_t* name; Button button; const wchar_t* buttonName;
    bool (*keys)(); const wchar_t* keysName;   // Windows 11's shortcut for it
    bool (*classicKeys)();                     // the one a classic taskbar answers
};
// Win+N: "notification center and calendar" (Microsoft's list of Windows 11 shortcuts). Win+Alt+D, sent
// here until mod.27, brought no calendar on a 26H2 build (2026-11-12).
static const TrayFlyout kCalendar{ L"calendar", Clock, L"clock", [] { return SendKeyCombo({ VK_LWIN, 'N' }); }, L"Win+N",
                                   [] { return SendKeyCombo({ VK_LWIN, VK_MENU, 'D' }); } };
static const TrayFlyout kQuickSettings{ L"quick settings", Network, L"network", [] { return SendKeyCombo({ VK_LWIN, 'A' }); }, L"Win+A",
                                        [] { return SendKeyCombo({ VK_LWIN, 'A' }); } };
static bool g_saidPressed[ButtonCount];   // the usual outcome is logged once per run for each flyout

// UI thread, no waiting for the flyout. The time it takes is Explorer's: a few ms as a rule (a kept
// button: two calls; none kept: two more for each window looked into). Against a hung Explorer the lookup
// starts with no further window after kLookupBudget, counted once for the whole click, so a click waits
// about that budget plus one call's timeout - at the very worst (Explorer hanging in the middle of a
// click, the kept button gone) a lookup's and a press's calls on top. A call's timeout is the 0.5 s
// CacheButtons asks for (CUIAutomation8 takes it); only where there is just CUIAutomation does a call into
// a hung Explorer wait as long as Explorer hangs.
static void OpenFlyout(const TrayFlyout& f) {
    HWND tray = ExplorerTaskbar();   // (never FindWindow: the first Shell_TrayWnd may be the dock's own tray host)
    if (!tray || ClassicTray(tray)) { f.classicKeys(); return; }   // no XAML buttons to press
    static bool busy = false;   // a click dispatched while Explorer is being asked: dropped (two presses would open and close it)
    if (busy) return;
    busy = true;
    wchar_t reason[96] = L"";   // why the button was not pressed: the keys go instead
    bool keys = true;
    // The press focuses the auto-hidden taskbar, which then expands: it must be invisible by then. Asked,
    // not made so here (Protect): that could race the enforcer's and save alpha 0 as the taskbar's own.
    if (g_taskbar.hidden && !taskbarvisibility::IsProtected(tray)) {
        wcscpy_s(reason, L"not pressed: the taskbar is not invisible yet");
    } else {
        const TrayPress p = PressButton(tray, f.button, NowSeconds() + kLookupBudget);
        if (p.pressed) {
            keys = false;
            if (!g_saidPressed[f.button]) {
                g_saidPressed[f.button] = true;
                Log(L"tray: %ls - pressed the taskbar's %ls button (%ls, found %ls)", f.name, f.buttonName,
                    p.kept ? L"kept" : L"found at the click", g_buttonFrom[f.button]);
            }
        } else if (p.unsure) {
            keys = false;
            Log(L"tray: %ls - the taskbar's %ls button pressed, no answer in time (%08x): nothing more sent", f.name, f.buttonName, (unsigned)p.hr);
        } else if (!p.found) {
            wcscpy_s(reason, L"not in the taskbar's automation tree");
        } else {
            swprintf_s(reason, L"its press did not go through (%08x)", (unsigned)p.hr);
        }
    }
    if (keys) {
        const bool inserted = f.keys();
        Log(L"tray: %ls - the taskbar's %ls button %ls: %ls sent%ls", f.name, f.buttonName, reason, f.keysName, inserted ? L"" : L" (not inserted)");
    }
    busy = false;
}

void OpenCalendar() { OpenFlyout(kCalendar); }

void OpenVolume(POINT c) {
    // SndVol's flyout mode takes the screen position as MAKELPARAM(x, y) of its centre
    wchar_t arg[32]; swprintf_s(arg, L"-f %ld", (long)MAKELPARAM((WORD)c.x, (WORD)c.y));
    ShellExecuteW(nullptr, L"open", L"SndVol.exe", arg, nullptr, SW_SHOWNORMAL);
}

void OpenQuickSettings() { OpenFlyout(kQuickSettings); }

}
