// Pressing the hidden-icons button of the stock Windows 11 taskbar (src/native_tray.cpp, OpenOverflowWin11)
// against a fake taskbar: a window of this test (own thread, off screen) whose child windows tell UI
// Automation they are the taskbar's tray buttons ("SystemTray.NormalButton" with 26H2's automation ids,
// the network and clock buttons).
//   - Windows shows no hidden-icons button (no hidden icons, or its hidden icon menu off): app icons only,
//     or no tray buttons at all - told once the content has stayed the same about 0.2 s, not after all 40 looks
//   - a chevron that can be pressed: pressed once, also when the tray's content only comes after the
//     taskbar is expanded (auto-hide off), or up to 150 ms after the app icons
// And the calendar and quick settings (OpenCalendar, OpenQuickSettings) against a fake taskbar that is
// hidden already, as Explorer's is from logon with Windows' own auto-hide on (26H2,
// 2026-11-12): nothing under the taskbar's own window, the whole tray under a hidden child window of the
// XAML island's class (a UI Automation fragment root of this test).
//   - the network and clock buttons are found from the island and kept; the hidden-icons button is not
//     taken from there
//   - a click presses the taskbar's own button: the kept one, one looked up at the click, one looked up
//     again when the kept one is gone; a second click presses it again (the button toggles by itself)
//   - the shortcut (Win+N, Win+A) only when the button cannot be pressed, once a click, never on top of a
//     press that may still arrive; Win+Alt+D only on a classic taskbar; no press while the taskbar the
//     dock hides is not invisible
// SHAppBarMessage is faked: the real taskbar's auto-hide is never touched. ExplorerTaskbar is faked: the
// real taskbar is never looked into or pressed. No key is sent (SendKeyCombo only records), no real
// window is looked at. The island's and the classic tray's class names are Windows' own, as the code
// under test asks for them - on child windows of the test's fake taskbar only, where no other program
// looks for them.
//   cl /std:c++20 /EHsc /utf-8 tests\tray_overflow_test.cpp user32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib uiautomationcore.lib
#include "../src/common.h"
#include "../src/system.h"
#include "../src/explorer_taskbar.h"   // before the macro below: the real one keeps its name (and is never called)
#include "../src/taskbar_visibility.h"
#include <shellapi.h>
#include <shldisp.h>
#include <uiautomation.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include "portable_check.h"

// ---- the taskbar's auto-hide, as OpenOverflowWin11 asks for it and turns it off for a moment
static UINT_PTR g_appbarState = ABS_AUTOHIDE;
static int g_appbarSets = 0;
static HWND g_expandBar = nullptr;   // a fake taskbar whose content comes once it is expanded
constexpr UINT kExpandMsg = WM_APP + 1;
static UINT_PTR WINAPI FakeAppBar(DWORD msg, PAPPBARDATA d) {
    if (msg == ABM_GETSTATE) return g_appbarState;
    if (msg == ABM_SETSTATE) {
        ++g_appbarSets; g_appbarState = d->lParam;
        if (!(d->lParam & ABS_AUTOHIDE) && g_expandBar) PostMessageW(g_expandBar, kExpandMsg, 0, 0);
    }
    return TRUE;
}
// ---- Explorer's taskbar, as a click on the dock's clock or network cell asks for it: the scenario's fake one
static HWND g_explorerBar = nullptr;
static HWND FakeExplorerTaskbar() { return g_explorerBar; }
#define SHAppBarMessage FakeAppBar
#define ExplorerTaskbar FakeExplorerTaskbar
#include "../src/native_tray.cpp"
#undef ExplorerTaskbar
#undef SHAppBarMessage

// ---- the keys a click sends: recorded, none reaches Windows
static std::vector<std::vector<WORD>> g_keys;
static bool g_keysInserted = true;
bool SendKeyCombo(std::initializer_list<WORD> keys) { g_keys.emplace_back(keys); return g_keysInserted; }
static int Sent(std::initializer_list<WORD> keys) {
    int n = 0;
    for (const auto& k : g_keys) if (k == std::vector<WORD>(keys)) ++n;
    return n;
}
NativeTaskbar g_taskbar;
bool NativeTaskbar::WithInvisibleTaskbars(const std::function<bool()>&) { std::abort(); }
static std::atomic<int> g_noneLogged{ 0 };
static std::vector<std::wstring> g_log;   // dock.log's lines (all written by the test's main thread)
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (wcsstr(buf, L"no hidden-icons button")) ++g_noneLogged;
    g_log.push_back(buf);
    std::printf("  log: %ls\n", buf);
}
static int LogCount(size_t from, const wchar_t* text) {
    int n = 0;
    for (size_t i = from; i < g_log.size(); ++i) if (g_log[i].find(text) != std::wstring::npos) ++n;
    return n;
}
static bool LogHas(size_t from, const wchar_t* text) { return LogCount(from, text) > 0; }
// what follows `text` in the first line since `from` that has it, up to `end` (empty: no such line)
static std::wstring LogAfter(size_t from, const wchar_t* text, wchar_t end) {
    for (size_t i = from; i < g_log.size(); ++i) {
        const size_t at = g_log[i].find(text);
        if (at != std::wstring::npos) { const size_t start = at + wcslen(text); return g_log[i].substr(start, g_log[i].find(end, start) - start); }
    }
    return L"";
}
double NowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

// ---- a tray button as UI Automation sees it: a class, an automation id, Invoke for the chevron
constexpr wchar_t kAccent[] = L"SystemTray.AccentButton", kOmni[] = L"SystemTray.OmniButton", kOmniRight[] = L"SystemTray.OmniButtonRight";
struct ButtonSpec { const wchar_t* cls; const wchar_t* id; UINT delay = 0; };   // delay: ms after expanding (0: there from the start)
static const ButtonSpec kAppIcon{ traybuttons::kNormal, traybuttons::kAppIconId };
static const ButtonSpec kChevron{ traybuttons::kNormal, traybuttons::kChevronId };
static const ButtonSpec kNetwork{ kAccent, L"SystemTrayIcon" }, kClock{ kOmni, L"SystemTrayIcon" };   // their ids as on 26H2
static const ButtonSpec kVolume{ kOmniRight, L"SystemTrayIcon" };   // 26H2: a class of its own, which starts like the clock's
static ButtonSpec Later(ButtonSpec b, UINT ms) { b.delay = ms; return b; }

static std::atomic<int> g_invoked{ 0 };
class FakeButton : public IRawElementProviderSimple, public IInvokeProvider {
    LONG ref = 1; HWND hwnd; std::wstring cls, id;
public:
    FakeButton(HWND h, const ButtonSpec& b) : hwnd(h), cls(b.cls), id(b.id) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) *pp = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IInvokeProvider)) *pp = static_cast<IInvokeProvider*>(this);
        else { *pp = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return (ULONG)r; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* o) override { *o = ProviderOptions_ServerSideProvider; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID p, IUnknown** out) override {
        *out = nullptr;
        if (p == UIA_InvokePatternId && cls == traybuttons::kNormal && id == traybuttons::kChevronId) { *out = static_cast<IInvokeProvider*>(this); AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID p, VARIANT* v) override {
        v->vt = VT_EMPTY;
        if (p == UIA_AutomationIdPropertyId) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(id.c_str()); }
        else if (p == UIA_ClassNamePropertyId) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(cls.c_str()); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** p) override { return UiaHostProviderFromHwnd(hwnd, p); }
    HRESULT STDMETHODCALLTYPE Invoke() override { ++g_invoked; return S_OK; }
};

// messages the fake taskbar's thread answered for another thread (UI Automation asking "Explorer")
static std::atomic<int> g_asked{ 0 };
static LRESULT CALLBACK ButtonProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (InSendMessage()) ++g_asked;
    if (m == WM_GETOBJECT && (long)l == UiaRootObjectId)
        if (auto* b = (FakeButton*)GetWindowLongPtrW(h, GWLP_USERDATA)) return UiaReturnRawElementProvider(h, w, l, b);
    if (m == WM_NCDESTROY) {
        if (auto* b = (FakeButton*)GetWindowLongPtrW(h, GWLP_USERDATA)) { UiaReturnRawElementProvider(h, 0, 0, nullptr); b->Release(); }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- the taskbar's XAML island as UI Automation sees it: one window, the tray's buttons elements under it
// (no windows of their own). Hidden while the taskbar is: the tree rooted at the taskbar's window does not
// reach into it then, a lookup from the island's own window does. Every button can be pressed, as on 26H2.
constexpr wchar_t kIslandClass[] = L"Windows.UI.Composition.DesktopWindowContentBridge";
static std::atomic<int> g_invokedClock{ 0 }, g_invokedNetwork{ 0 }, g_invokedOther{ 0 };   // presses of the island's buttons
static std::atomic<HRESULT> g_islandPressHr{ S_OK };   // what a press of its clock or network button answers
static std::atomic<bool> g_islandNoPress{ false };     // ...they offer none (no Invoke pattern)
class FakeIsland;
class IslandButton : public IRawElementProviderSimple, public IRawElementProviderFragment, public IInvokeProvider {
    FakeIsland* island; int index; std::wstring cls, id;
public:
    IslandButton(FakeIsland* is, int i, const ButtonSpec& b) : island(is), index(i), cls(b.cls), id(b.id) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) *pp = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment)) *pp = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IInvokeProvider)) *pp = static_cast<IInvokeProvider*>(this);
        else { *pp = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override;   // one count for the island and its buttons: they refer to each other
    ULONG STDMETHODCALLTYPE Release() override;
    // (server side only, as FakeButton: the fake taskbar's thread has no COM apartment)
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* o) override { *o = ProviderOptions_ServerSideProvider; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID p, IUnknown** out) override {
        *out = nullptr;
        // (a failure answered here does not reach the client: UI Automation hands it "no such pattern" all the same)
        if (p == UIA_InvokePatternId && !(g_islandNoPress && (cls == kOmni || cls == kAccent))) { *out = static_cast<IInvokeProvider*>(this); AddRef(); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID p, VARIANT* v) override {
        v->vt = VT_EMPTY;
        if (p == UIA_AutomationIdPropertyId) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(id.c_str()); }
        else if (p == UIA_ClassNamePropertyId) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(cls.c_str()); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** p) override { *p = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection d, IRawElementProviderFragment** out) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
        int rid[] = { UiaAppendRuntimeId, index + 1 };
        if (!(*out = SafeArrayCreateVector(VT_I4, 0, 2))) return E_OUTOFMEMORY;
        for (LONG i = 0; i < 2; ++i) SafeArrayPutElement(*out, &i, &rid[i]);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override { *r = {}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override { *out = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override;
    HRESULT STDMETHODCALLTYPE Invoke() override;
};
class FakeIsland : public IRawElementProviderSimple, public IRawElementProviderFragment, public IRawElementProviderFragmentRoot {
    std::atomic<long> refs{ 1 };
public:
    HWND hwnd;
    std::atomic<bool> gone{ false };   // its window was destroyed (Explorer rebuilt its taskbar's content)
    std::vector<std::unique_ptr<IslandButton>> buttons;   // as made, never changed (UI Automation asks from threads of its own)
    FakeIsland(HWND h, const std::vector<ButtonSpec>& specs) : hwnd(h) {
        for (size_t i = 0; i < specs.size(); ++i) buttons.push_back(std::make_unique<IslandButton>(this, (int)i, specs[i]));
    }
    virtual ~FakeIsland() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) *pp = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment)) *pp = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot)) *pp = static_cast<IRawElementProviderFragmentRoot*>(this);
        else { *pp = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const long r = --refs; if (!r) delete this; return (ULONG)r; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* o) override { *o = ProviderOptions_ServerSideProvider; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** out) override { *out = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID, VARIANT* v) override { v->vt = VT_EMPTY; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** p) override { return UiaHostProviderFromHwnd(hwnd, p); }
    IRawElementProviderFragment* Child(int i) {   // with a reference; none there: null
        if (gone || i < 0 || i >= (int)buttons.size()) return nullptr;
        buttons[i]->AddRef();
        return buttons[i].get();
    }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection d, IRawElementProviderFragment** out) override {
        *out = d == NavigateDirection_FirstChild ? Child(0) : d == NavigateDirection_LastChild ? Child((int)buttons.size() - 1) : nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override { *out = nullptr; return S_OK; }   // (its window's)
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* r) override { *r = {}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override { *out = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override { *out = this; AddRef(); return S_OK; }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double, double, IRawElementProviderFragment** out) override { *out = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override { *out = nullptr; return S_OK; }
};
ULONG STDMETHODCALLTYPE IslandButton::AddRef() { return island->AddRef(); }
ULONG STDMETHODCALLTYPE IslandButton::Release() { return island->Release(); }
HRESULT STDMETHODCALLTYPE IslandButton::Navigate(NavigateDirection d, IRawElementProviderFragment** out) {
    *out = nullptr;
    if (island->gone) return UIA_E_ELEMENTNOTAVAILABLE;
    if (d == NavigateDirection_Parent) { *out = island; island->AddRef(); }
    else if (d == NavigateDirection_NextSibling) *out = island->Child(index + 1);
    else if (d == NavigateDirection_PreviousSibling) *out = island->Child(index - 1);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE IslandButton::get_FragmentRoot(IRawElementProviderFragmentRoot** out) { *out = island; island->AddRef(); return S_OK; }
HRESULT STDMETHODCALLTYPE IslandButton::Invoke() {
    if (island->gone) return UIA_E_ELEMENTNOTAVAILABLE;   // as a button of content Explorer has taken down
    if (cls == kOmni) { ++g_invokedClock; return g_islandPressHr; }
    if (cls == kAccent) { ++g_invokedNetwork; return g_islandPressHr; }
    ++g_invokedOther;
    return S_OK;
}
static LRESULT CALLBACK IslandProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (InSendMessage()) ++g_asked;
    auto* is = (FakeIsland*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_GETOBJECT && (long)l == UiaRootObjectId && is) return UiaReturnRawElementProvider(h, w, l, static_cast<IRawElementProviderSimple*>(is));
    if (m == WM_NCDESTROY && is) {
        is->gone = true;
        UiaReturnRawElementProvider(h, 0, 0, nullptr);
        is->Release();
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(h, m, w, l);
}

// a fake taskbar on its own thread (UI Automation reaches its buttons through their window messages);
// buttons with a delay come that long after it is expanded, as Explorer builds its content then
constexpr UINT kIslandMsg = WM_APP + 2;   // its island destroyed (lParam: its elements disconnected first) and, wParam, made anew
struct FakeTaskbar {
    HWND hwnd = nullptr; DWORD tid = 0; std::thread t;
    std::vector<ButtonSpec> buttons;
    // a hidden taskbar's tray: under its XAML island (a hidden child window), nothing under the taskbar's own window
    bool hasIsland = false; std::vector<ButtonSpec> island; HWND islandWnd = nullptr;
    bool classic = false;   // a classic (Windows 10 style) tray: TrayNotifyWnd with its hidden-icons button, 1502
    void MakeIsland() {
        islandWnd = CreateWindowExW(0, kIslandClass, L"DesktopWindowXamlSource", WS_CHILD, 0, 0, 400, 40, hwnd, nullptr,   // (no WS_VISIBLE)
                                    GetModuleHandleW(nullptr), nullptr);
        if (islandWnd) SetWindowLongPtrW(islandWnd, GWLP_USERDATA, (LONG_PTR)new FakeIsland(islandWnd, island));
    }
    // Explorer rebuilding its taskbar's content: the island's window and elements gone, new ones in their place.
    // disconnect: the old elements are taken down in order (UiaDisconnectProvider), as XAML does with its own -
    // they then answer "not available"; without, their window just goes away under them
    HANDLE remade = nullptr;
    void RemakeIsland(bool disconnect) {   // posted, not sent: UiaDisconnectProvider refuses to work inside a sent message
        remade = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        PostMessageW(hwnd, kIslandMsg, 1, disconnect);
        WaitForSingleObject(remade, 5000);
        CloseHandle(remade);
    }
    void RunHidden(std::vector<ButtonSpec> tray) { hasIsland = true; island = std::move(tray); Run({}); }
    void Add(size_t i) {
        HWND b = CreateWindowExW(0, L"TrayOverflowTestButton", L"", WS_CHILD | WS_VISIBLE, (int)i * 40, 0, 40, 40, hwnd, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(b, GWLP_USERDATA, (LONG_PTR)new FakeButton(b, buttons[i]));
    }
    static LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (InSendMessage()) ++g_asked;
        auto* self = (FakeTaskbar*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (self && m == kIslandMsg) {
            if (self->islandWnd) {
                if (l)   // its elements taken down first, as XAML does with its own
                    if (auto* is = (FakeIsland*)GetWindowLongPtrW(self->islandWnd, GWLP_USERDATA)) {
                        for (auto& b : is->buttons) UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(b.get()));
                        UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(is));
                    }
                DestroyWindow(self->islandWnd);
                self->islandWnd = nullptr;
            }
            if (w) self->MakeIsland();
            SetEvent(self->remade);
            return 0;
        }
        if (self && m == kExpandMsg) {
            for (size_t i = 0; i < self->buttons.size(); ++i)
                if (self->buttons[i].delay) SetTimer(h, 100 + i, self->buttons[i].delay, nullptr);
            return 0;
        }
        if (self && m == WM_TIMER && w >= 100 && w < 100 + self->buttons.size()) {
            KillTimer(h, w);
            self->Add(w - 100);
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
    void Run(std::vector<ButtonSpec> specs) {
        buttons = std::move(specs);
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        t = std::thread([this, ready] {
            tid = GetCurrentThreadId();
            HINSTANCE inst = GetModuleHandleW(nullptr);
            WNDCLASSEXW tc = { sizeof(tc) }; tc.lpfnWndProc = BarProc; tc.hInstance = inst; tc.lpszClassName = L"TrayOverflowTestTaskbar";
            WNDCLASSEXW bc = { sizeof(bc) }; bc.lpfnWndProc = ButtonProc; bc.hInstance = inst; bc.lpszClassName = L"TrayOverflowTestButton";
            WNDCLASSEXW ic = { sizeof(ic) }; ic.lpfnWndProc = IslandProc; ic.hInstance = inst; ic.lpszClassName = kIslandClass;
            WNDCLASSEXW nc = { sizeof(nc) }; nc.lpfnWndProc = DefWindowProcW; nc.hInstance = inst; nc.lpszClassName = L"TrayNotifyWnd";
            RegisterClassExW(&tc); RegisterClassExW(&bc);   // (the second time: registered already)
            RegisterClassExW(&ic); RegisterClassExW(&nc);   // (classes of this program, whatever their names)
            hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, tc.lpszClassName, L"tray overflow test: fake taskbar", WS_POPUP,
                                   -12000, -12000, 400, 40, nullptr, nullptr, inst, nullptr);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
            for (size_t i = 0; i < buttons.size(); ++i) if (!buttons[i].delay) Add(i);
            if (hasIsland) MakeIsland();
            if (classic) {
                HWND notify = CreateWindowExW(0, nc.lpszClassName, L"", WS_CHILD, 0, 0, 40, 40, hwnd, nullptr, inst, nullptr);
                CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 20, 20, notify, (HMENU)(INT_PTR)1502, inst, nullptr);
            }
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);   // off screen; shown, so UI Automation lists its children
            SetEvent(ready);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
            DestroyWindow(hwnd);
        });
        WaitForSingleObject(ready, 5000);
        CloseHandle(ready);
    }
    void Quit() { PostThreadMessageW(tid, WM_QUIT, 0, 0); if (t.joinable()) t.join(); }
};

static void Forget() {   // as for a taskbar seen for the first time
    for (auto& b : nativetray::g_button) b.Reset();
    nativetray::g_buttonTray = nullptr;
}
// ...and as in a new run of the dock: nothing kept, said, sent or pressed so far
static void NewRun(HWND explorerBar) {
    Forget();
    for (auto& said : nativetray::g_saidPressed) said = false;
    g_keys.clear(); g_keysInserted = true;
    g_invokedClock = 0; g_invokedNetwork = 0; g_invokedOther = 0; g_invoked = 0;
    g_islandPressHr = S_OK; g_islandNoPress = false;
    g_appbarState = ABS_AUTOHIDE; g_appbarSets = 0;
    g_explorerBar = explorerBar;
}

// One press on a taskbar seen for the first time (its content made once it is expanded, when delayed)
// asked: the messages its taskbar answered meanwhile
struct Press { nativetray::OverflowFailure why; double ms; int asked; };
static Press PressOnce(FakeTaskbar& bar) {
    Forget();
    g_appbarState = ABS_AUTOHIDE; g_appbarSets = 0; g_invoked = 0;
    g_expandBar = bar.hwnd;
    const int asked = g_asked.load();
    double t0 = NowSeconds();
    auto why = nativetray::OpenOverflowWin11(bar.hwnd);
    Press p{ why, (NowSeconds() - t0) * 1000, g_asked.load() - asked };
    g_expandBar = nullptr;
    std::printf("  (the taskbar answered %d messages)\n", p.asked);
    return p;
}
static bool Cached(nativetray::Button b) { return nativetray::g_button[b] != nullptr; }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // as the dock's UI thread
    {   // 26H2, Windows shows no hidden-icons button: three app icons on the taskbar, no chevron
        FakeTaskbar bar;
        bar.Run({ kAppIcon, kAppIcon, kAppIcon });
        auto p = PressOnce(bar);
        std::printf("  no chevron: %.0f ms\n", p.ms);
        Check(p.why == nativetray::OverflowFailure::NoButton && g_noneLogged.load() > 0, "app icons only: no hidden-icons button (NoButton), and the log says so");
        Check(g_appbarSets == 2 && g_appbarState == ABS_AUTOHIDE, "the taskbar's auto-hide is put back");
        Check(p.ms < 800, "the tray buttons are there and none is the chevron: no 40 looks of 25 ms on the UI thread (the taskbar meanwhile not auto-hidden)");
        bar.Quit();
    }
    {   // a chevron after the app icons: found and pressed once
        FakeTaskbar bar;
        bar.Run({ kAppIcon, kChevron });
        auto p = PressOnce(bar);
        std::printf("  chevron: %.0f ms\n", p.ms);
        Check(p.why == nativetray::OverflowFailure::None && g_invoked.load() == 1, "the chevron is found among the app icons and pressed once");
        Check(g_appbarState == ABS_AUTOHIDE, "auto-hide put back after looking");
        g_invoked = 0;
        auto why = nativetray::OpenOverflowWin11(bar.hwnd);
        Check(why == nativetray::OverflowFailure::None && g_invoked.load() == 1 && g_appbarSets == 2, "the second time: the remembered button, auto-hide left alone");
        bar.Quit();
    }
    {   // the tray's content comes 100 ms after the taskbar is expanded: no giving up while it is not there
        FakeTaskbar bar;
        bar.Run({ Later(kChevron, 100), Later(kAppIcon, 100), Later(kNetwork, 100), Later(kClock, 100) });
        auto p = PressOnce(bar);
        std::printf("  content after 100 ms, chevron: %.0f ms\n", p.ms);
        Check(p.why == nativetray::OverflowFailure::None && g_invoked.load() == 1 && g_appbarState == ABS_AUTOHIDE,
              "content made only once expanded: the chevron is waited for, found and pressed once");
        Check(Cached(nativetray::Network) && Cached(nativetray::Clock),
              "...and the network and clock buttons that came with it are kept (quick settings and the calendar use them)");
        bar.Quit();
    }
    {   // the chevron comes a moment after the app icons, network and clock (the tray still being built)
        FakeTaskbar bar;
        bar.Run({ Later(kAppIcon, 100), Later(kAppIcon, 100), Later(kNetwork, 100), Later(kClock, 100), Later(kChevron, 130) });
        auto p = PressOnce(bar);
        std::printf("  chevron 30 ms after the rest: %.0f ms\n", p.ms);
        Check(p.why == nativetray::OverflowFailure::None && g_invoked.load() == 1,
              "the chevron 30 ms after the other buttons: still found (no NoButton from one or two looks)");
        bar.Quit();
    }
    // later still (the margin kSettleMs gives): 80 ms, and 150 ms, the case a verifier made (app icons at
    // 100 ms, chevron at 250 ms) - three looks alone (about 0.1 s) told NoButton there; each run three times
    for (UINT late : { 80u, 150u }) {
        int found = 0;
        double slowest = 0;
        for (int run = 0; run < 3; ++run) {
            FakeTaskbar bar;
            bar.Run({ Later(kAppIcon, 100), Later(kAppIcon, 100), Later(kNetwork, 100), Later(kClock, 100), Later(kChevron, 100 + late) });
            auto p = PressOnce(bar);
            if (p.why == nativetray::OverflowFailure::None && g_invoked.load() == 1) ++found;
            if (p.ms > slowest) slowest = p.ms;
            bar.Quit();
        }
        std::printf("  chevron %u ms after the rest: found %d of 3, at most %.0f ms\n", late, found, slowest);
        char what[160];
        std::snprintf(what, sizeof what, "the chevron %u ms after the other buttons: found and pressed once, each of 3 times", late);
        Check(found == 3, what);
    }
    {   // no tray buttons at all once expanded, only network and clock: no chevron either, told soon
        FakeTaskbar bar;
        bar.Run({ Later(kNetwork, 100), Later(kClock, 100) });
        int logged = g_noneLogged.load();
        auto p = PressOnce(bar);
        std::printf("  no tray buttons, content after 100 ms: %.0f ms\n", p.ms);
        Check(p.why == nativetray::OverflowFailure::NoButton && g_noneLogged.load() > logged && g_appbarState == ABS_AUTOHIDE,
              "no tray buttons at all: NoButton, the log says so, auto-hide put back");
        Check(Cached(nativetray::Network) && Cached(nativetray::Clock), "...the network and clock buttons are kept");
        Check(p.ms < 800, "...told once the network and clock buttons have stayed alone about 0.2 s, not after all 40 looks");
        bar.Quit();
    }
    {   // no content at all (the taskbar never expands): all 40 looks, then NoButton - the bound, unchanged
        FakeTaskbar bar;
        bar.Run({});
        auto p = PressOnce(bar);
        std::printf("  nothing there: %.0f ms\n", p.ms);
        Check(p.why == nativetray::OverflowFailure::NoButton && p.ms > 800 && p.ms < 4000 && g_appbarState == ABS_AUTOHIDE,
              "no content ever: NoButton after the full (bounded) wait, auto-hide put back");
        bar.Quit();
    }
    Check(g_keys.empty(), "none of the hidden-icons presses above sent a key");

    // ---- the calendar and quick settings: the taskbar hidden already (its tray under a hidden XAML island)
    using nativetray::Chevron; using nativetray::Network; using nativetray::Clock;
    const std::vector<ButtonSpec> kTray{ kAppIcon, kChevron, kNetwork, kVolume, kClock };
    {   // the bug itself: the taskbar is hidden from logon, its buttons are not under its own window
        FakeTaskbar bar;
        bar.RunHidden(kTray);
        NewRun(bar.hwnd);
        const size_t log = g_log.size();
        Check(bar.islandWnd && GetParent(bar.islandWnd) == bar.hwnd && !IsWindowVisible(bar.islandWnd),
              "the fake taskbar's XAML island: a hidden child window of it");
        nativetray::CacheTrayButtons(bar.hwnd);
        {   // The UI Automation object native_tray made (CacheButtons, since the first scenario above) takes the 0.5 s a
            // call may last: CUIAutomation8. CUIAutomation - Windows 7's, asked for until mod.27 - has no IUIAutomation2,
            // so the timeouts were never set and a call into a hung Explorer held the dock's UI thread for as long as it hung.
            ComPtr<IUIAutomation2> u2;
            DWORD transaction = 0, connection = 0;
            const HRESULT offered = nativetray::g_uia ? nativetray::g_uia.As(&u2) : E_POINTER;
            const HRESULT read = u2 ? u2->get_TransactionTimeout(&transaction) : offered;
            const HRESULT readConnection = u2 ? u2->get_ConnectionTimeout(&connection) : offered;
            std::printf("  (UI Automation's call timeouts: IUIAutomation2 %s (%08x), a call %lu ms (%08x), connecting %lu ms (%08x))\n",
                        u2 ? "offered" : "not offered by this object", (unsigned)offered, transaction, (unsigned)read, connection, (unsigned)readConnection);
            Check(SUCCEEDED(offered) && u2 && SUCCEEDED(read) && transaction == 500 && SUCCEEDED(readConnection) && connection == 500,
                  "the UI Automation object native_tray made takes the call timeouts (IUIAutomation2): 500 ms a call, 500 ms to connect");
        }
        Check(Cached(Network) && Cached(Clock), "a taskbar hidden already: its network and clock buttons are found (from its XAML island) and kept");
        Check(LogHas(log, L"tray button 1 cached (found from its XAML island)") && LogHas(log, L"tray button 2 cached (found from its XAML island)"),
              "...and the log says where they were found");
        Check(!Cached(Chevron) && g_appbarSets == 0 && !LogHas(log, L"hidden-icons button"),
              "...the hidden-icons button is not taken from the island (it keeps its own way), auto-hide is not touched");
        Check(!nativetray::FindButton(bar.hwnd, Clock, 0) && !nativetray::FindButton(bar.hwnd, Network, 0),
              "...from the taskbar's window alone neither is found: the fake has the tree of a hidden taskbar");
        Check(!LogHas(log, L"no clock or network button"), "...nothing said about missing buttons");
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 1 && g_invokedOther.load() == 0 && g_invokedNetwork.load() == 0 && g_keys.empty(),
              "a click on the clock: the kept clock button is pressed once (not the volume button next to it), no key");
        Check(LogHas(log, L"tray: calendar - pressed the taskbar's clock button (kept, found from its XAML island)"), "...and the log says so");
        bar.Quit();
    }
    {   // nothing kept (the lookup before the taskbar vanished found none, or never ran): looked up at the click
        FakeTaskbar bar;
        bar.RunHidden(kTray);
        NewRun(bar.hwnd);
        const size_t log = g_log.size();
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 1 && g_keys.empty(), "no clock button kept: the click looks it up and presses it once, no key");
        Check(LogHas(log, L"tray: calendar - pressed the taskbar's clock button (found at the click, found from its XAML island)") && Cached(Clock),
              "...the log says it was found at the click, and it is kept from then on");
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 2 && g_keys.empty(), "a second click: pressed again (the taskbar's button closes its flyout itself), no key");
        Check(LogCount(log, L"pressed the taskbar's clock button") == 1, "...the usual outcome is logged once per run");
        Check(g_invokedOther.load() == 0 && g_invokedNetwork.load() == 0 && g_appbarSets == 0, "...no other button pressed, auto-hide not touched");
        bar.Quit();
    }
    {   // quick settings: the network button
        FakeTaskbar bar;
        bar.RunHidden(kTray);
        NewRun(bar.hwnd);
        const size_t log = g_log.size();
        nativetray::OpenQuickSettings();
        Check(g_invokedNetwork.load() == 1 && g_invokedClock.load() == 0 && g_invokedOther.load() == 0 && g_keys.empty(),
              "quick settings: the taskbar's network button is looked up and pressed once, no key");
        Check(LogHas(log, L"tray: quick settings - pressed the taskbar's network button (found at the click, found from its XAML island)"),
              "...and the log says so");
        bar.Quit();
    }
    {   // no clock and no network button anywhere (the tray not built yet, or a Windows build without them)
        // (Order: the "no clock or network button" line is written once per run of the dock - CacheTrayButtons keeps
        // that in a static of its own, which NewRun cannot clear. Its count of 1 below holds only while this is the
        // first scenario of the test in which CacheTrayButtons finds neither button: one put before it would use the
        // line up, and the count here would be 0.)
        FakeTaskbar bar;
        bar.RunHidden({ kAppIcon, kChevron, kVolume });
        NewRun(bar.hwnd);
        size_t log = g_log.size();
        nativetray::CacheTrayButtons(bar.hwnd);
        nativetray::CacheTrayButtons(bar.hwnd);
        Check(!Cached(Network) && !Cached(Clock) && !Cached(Chevron) &&
              LogCount(log, L"tray: no clock or network button in the taskbar's automation tree yet (its window and 1 island windows): looked for at the click") == 1,
              "no clock or network button in the tree: none kept, the log says so once");
        nativetray::OpenCalendar();
        Check(g_keys.size() == 1 && Sent({ VK_LWIN, 'N' }) == 1, "no clock button: Win+N, exactly once");
        Check(Sent({ VK_LWIN, VK_MENU, 'D' }) == 0 && g_invokedOther.load() == 0,
              "...never Win+Alt+D, and no other button is pressed for it (the volume button's class only starts like the clock's)");
        Check(LogHas(log, L"tray: calendar - the taskbar's clock button not in the taskbar's automation tree: Win+N sent") && !LogHas(log, L"(not inserted)"),
              "...and the log tells why the keys went");
        g_keys.clear(); g_keysInserted = false; log = g_log.size();
        nativetray::OpenQuickSettings();
        Check(g_keys.size() == 1 && Sent({ VK_LWIN, 'A' }) == 1, "no network button: Win+A, exactly once");
        Check(LogHas(log, L"tray: quick settings - the taskbar's network button not in the taskbar's automation tree: Win+A sent (not inserted)"),
              "...keys Windows did not take are told as such");
        bar.Quit();
    }
    {   // a classic taskbar (Windows 10 style): no XAML buttons are looked for, its own shortcuts at once
        FakeTaskbar bar;
        bar.classic = true;
        bar.RunHidden(kTray);
        NewRun(bar.hwnd);
        const size_t log = g_log.size();
        nativetray::OpenCalendar();
        Check(g_keys.size() == 1 && Sent({ VK_LWIN, VK_MENU, 'D' }) == 1, "a classic taskbar: Win+Alt+D for the calendar");
        nativetray::OpenQuickSettings();
        Check(g_keys.size() == 2 && Sent({ VK_LWIN, 'A' }) == 1, "...Win+A for quick settings");
        Check(g_invokedClock.load() == 0 && g_invokedNetwork.load() == 0 && g_invokedOther.load() == 0 && !Cached(Clock) && !Cached(Network) && g_log.size() == log,
              "...nothing looked up, pressed or logged");
        g_explorerBar = nullptr; g_keys.clear();
        nativetray::OpenCalendar();
        Check(g_keys.size() == 1 && Sent({ VK_LWIN, VK_MENU, 'D' }) == 1 && g_invokedClock.load() == 0, "no taskbar at all (Explorer restarting): the same, as before");
        bar.Quit();
    }
    {   // the opacity guard: the press focuses the auto-hidden taskbar, which expands - never while it could be seen
        FakeTaskbar bar;
        bar.RunHidden(kTray);
        NewRun(bar.hwnd);
        const size_t log = g_log.size();
        g_taskbar.hidden = true;
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 0 && g_keys.size() == 1 && Sent({ VK_LWIN, 'N' }) == 1,
              "the dock hides the taskbar, and it is not invisible yet: the button is not pressed, Win+N once");
        Check(LogHas(log, L"tray: calendar - the taskbar's clock button not pressed: the taskbar is not invisible yet: Win+N sent"), "...and the log tells why");
        Check(taskbarvisibility::Protect(bar.hwnd), "(the fake taskbar made invisible, as the dock makes Explorer's)");
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 1 && g_keys.size() == 1, "...once it is invisible: pressed, no further key");
        Check(taskbarvisibility::Restore(bar.hwnd) && !taskbarvisibility::IsProtected(bar.hwnd), "(and put back)");
        g_taskbar.hidden = false;
        bar.Quit();
    }
    {   // a kept button that is gone: Explorer rebuilt its taskbar's content in the same window
        FakeTaskbar bar;
        bar.RunHidden(kTray);
        NewRun(bar.hwnd);
        nativetray::CacheTrayButtons(bar.hwnd);
        HWND old = bar.islandWnd;
        bar.RemakeIsland(false);
        Check(Cached(Clock) && bar.islandWnd && bar.islandWnd != old && !IsWindow(old), "(a clock button kept, then the island destroyed and made anew)");
        size_t log = g_log.size();
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 1 && g_keys.empty(), "the kept clock button gone: looked up once more and pressed once, no key");
        // (The island's window gone under its elements: asked for their patterns they answer a plain E_FAIL, 80004005,
        // on 26H2. That is UI Automation's own answer, not one of this test's fake: told here, not checked -
        // another Windows build may answer otherwise, and whatever it is must count as gone, which the check above
        // has shown.)
        std::printf("  (an element whose island window is gone answered %ls here)\n", LogAfter(log, L"button was gone (", L';').c_str());
        Check(LogHas(log, L"tray: the kept clock button was gone (") && LogHas(log, L"pressed the taskbar's clock button (found at the click"),
              "...and the log says so");
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 2 && g_keys.empty() && LogCount(log, L"was gone") == 1, "...the new one is kept: the next click presses it straight away");
        // the same, its elements taken down in order first: they answer "not available"
        old = bar.islandWnd; log = g_log.size();
        bar.RemakeIsland(true);
        nativetray::OpenCalendar();
        Check(!IsWindow(old) && g_invokedClock.load() == 3 && g_keys.empty() && LogHas(log, L"tray: the kept clock button was gone (80040201;"),
              "...gone as UI Automation tells it (element not available): looked up once more and pressed once, no key");
        // a kept button that is there and offers no press: one more lookup (it could be an old one), then the keys
        g_islandNoPress = true; log = g_log.size();
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 3 && g_keys.size() == 1 && Sent({ VK_LWIN, 'N' }) == 1 && LogCount(log, L"was gone") == 1 && !Cached(Clock),
              "a kept button that offers no press: looked up once more (once only), nothing pressed, Win+N once");
        g_keys.clear(); log = g_log.size();
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 3 && g_keys.size() == 1 && !LogHas(log, L"was gone") &&
              LogHas(log, L"tray: calendar - the taskbar's clock button its press did not go through (80004002): Win+N sent"),
              "...none kept any more: one lookup, Win+N once, the log tells the failure");
        g_islandNoPress = false;
        bar.Quit();
    }
    {   // what a failed call means (the calls a fake cannot make fail on demand: timeouts, Explorer gone)
        using nativetray::After; using nativetray::Classify;
        const HRESULT timeout = nativetray::kUiaTimeout, notThere = static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
        struct { HRESULT hr; bool press; After want; const char* what; } table[] = {
            { S_OK, true, After::Done, "S_OK: pressed" },
            { E_FAIL, true, After::NotDone, "the press failed plainly: not pressed, not gone (never pressed again)" },
            { E_ACCESSDENIED, true, After::NotDone, "access denied: not pressed" },
            { notThere, true, After::Gone, "element not available: gone, looked up again" },
            { notThere, false, After::Gone, "...before the press too" },
            { RPC_E_DISCONNECTED, true, After::Gone, "disconnected (Explorer restarted): gone" },
            { HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE), true, After::Gone, "server unavailable: gone" },
            { E_FAIL, false, After::Gone, "its patterns could not be read (its island's window gone): gone - no press was made" },
            { E_NOINTERFACE, false, After::Gone, "it offers no press: as gone (one more lookup)" },
            { timeout, true, After::Unsure, "a press that timed out: unsure (may still arrive)" },
            { timeout, false, After::NotDone, "...reading its patterns timed out: no press was made, and no second wait on a busy Explorer" },
            { HRESULT_FROM_WIN32(ERROR_TIMEOUT), true, After::Unsure, "a Win32 timeout: unsure" },
            { RPC_E_TIMEOUT, false, After::NotDone, "an RPC timeout before the press: not pressed" },
            { RPC_E_SERVER_DIED, true, After::UnsureGone, "Explorer died during the press: unsure, and gone" },
            { RPC_E_SERVER_DIED, false, After::Gone, "...before the press: gone" },
            { HRESULT_FROM_WIN32(RPC_S_CALL_FAILED), true, After::UnsureGone, "the call failed mid-way: unsure, and gone" },
        };
        bool all = true;
        for (const auto& t : table) {
            const bool ok = Classify(t.hr, t.press) == t.want;
            if (!ok) std::printf("  wrong: %s\n", t.what);
            all = all && ok;
        }
        Check(all, "every failure of a press is put down as done, not done, gone, unsure (16 cases)");
    }
    {   // a press that fails
        FakeTaskbar bar;
        bar.RunHidden(kTray);
        NewRun(bar.hwnd);
        size_t log = g_log.size();
        g_islandPressHr = E_FAIL;
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 1 && g_keys.size() == 1 && Sent({ VK_LWIN, 'N' }) == 1 && !Cached(Clock),
              "a press that fails plainly: not pressed again, the button dropped, Win+N once");
        Check(LogHas(log, L"tray: calendar - the taskbar's clock button its press did not go through (80004005): Win+N sent"), "...and the log tells the failure");
        g_islandPressHr = S_OK; g_keys.clear();
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 2 && g_keys.empty() && Cached(Clock), "...the next click looks it up again and presses it");
        g_islandPressHr = E_FAIL; log = g_log.size();
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 3 && g_keys.size() == 1 && Sent({ VK_LWIN, 'N' }) == 1 && !LogHas(log, L"was gone") && !Cached(Clock),
              "the press of a kept button fails plainly: it is not taken for gone (no second press), Win+N once");
        g_islandPressHr = S_OK; g_keys.clear();
        nativetray::OpenCalendar();
        // a press that timed out may still arrive: the keys on top would shut the flyout it opens
        g_islandPressHr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); log = g_log.size();
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 5 && g_keys.empty() && Cached(Clock), "a press that timed out: no key on top, not pressed again, the button kept");
        Check(LogHas(log, L"tray: calendar - the taskbar's clock button pressed, no answer in time (800705b4): nothing more sent"), "...and the log says so");
        g_islandPressHr = RPC_E_SERVER_DIED; log = g_log.size();
        nativetray::OpenCalendar();
        Check(g_invokedClock.load() == 6 && g_keys.empty() && !Cached(Clock) && LogHas(log, L"no answer in time (80010007)"),
              "Explorer gone during the press: no key on top either, and the button is dropped");
        g_islandPressHr = S_OK;
        bar.Quit();
    }
    {   // the taskbar still expanded when the dock starts (no auto-hide of Windows' own): found under its window, as before
        FakeTaskbar bar;
        bar.Run({ kAppIcon, kChevron, kNetwork, kClock });
        NewRun(bar.hwnd);
        const size_t log = g_log.size();
        nativetray::CacheTrayButtons(bar.hwnd);
        Check(Cached(Chevron) && Cached(Network) && Cached(Clock) && LogHas(log, L"tray button 0 cached (found from the taskbar window)") &&
              LogHas(log, L"tray button 2 cached (found from the taskbar window)") && g_appbarSets == 0,
              "an expanded taskbar: all three buttons are kept from its own window");
        bar.Quit();
    }
    g_explorerBar = nullptr;
    nativetray::g_uia.Reset();
    Forget();
    CoUninitialize();
    return Summary("tray_overflow_test");
}
