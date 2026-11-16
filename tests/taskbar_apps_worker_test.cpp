// The taskbar app buttons' worker (src/taskbar_apps.cpp) against a fake taskbar: fake UI Automation objects and
// fake Explorer windows (the Win32 calls it makes are redirected here), so no real taskbar, Explorer window or other
// program is touched and no key or click goes out (tests\run_taskbar_apps_live.bat: the real taskbar). Checks how a
// click's job is looked up and pressed: which taskbar is looked at first, which button is the app's (by id, by a
// known folder's path, by the windows it counts - on several taskbars), Invoke before Toggle and nothing without
// either (NoWay), a button that went away looked up once more, a press that timed out (Unsure), the dock giving up
// meanwhile (Stage: no press after it), a hung Explorer (the lookup's budget), the results through Begin/Take, one
// lookup at a time, and that nothing it gets from UI Automation is leaked.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\taskbar_apps_worker_test.cpp /link user32.lib ole32.lib oleaut32.lib shell32.lib uuid.lib
#include "../src/common.h"
#include <uiautomation.h>
#include <shlobj.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdio>
#include "../src/explorer_taskbar.h"
#include "../src/taskbar_apps_logic.h"
#include "portable_check.h"

// ---- the fake Explorer and UI Automation
namespace fake {
const HMONITOR kPrimary = (HMONITOR)(UINT_PTR)0x7A10, kSecond = (HMONITOR)(UINT_PTR)0x7A20;
const HWND kMain = (HWND)(UINT_PTR)0x7B100, kSecondBar = (HWND)(UINT_PTR)0x7B200, kNotify = (HWND)(UINT_PTR)0x7B1F0;
const DWORD kExplorer = 0x7C01;

struct Button {
    std::wstring id, name;                   // AutomationId, Name
    bool invoke = true, toggle = false;      // the patterns it offers
    HRESULT readHr = S_OK;                   // GetCurrentPatternAs
    std::vector<HRESULT> pressHr;            // the presses' answers in turn (S_OK once run out)
    std::function<void()> onRead, onPress;   // run inside those calls (the dock giving up meanwhile, say)
    int reads = 0, invokes = 0, toggles = 0;
};
struct Root {                    // a window UI Automation is asked about: a taskbar or one of its XAML island's
    HWND hwnd = nullptr, bar = nullptr;
    std::wstring cls;
    HRESULT elementHr = S_OK, findHr = S_OK;
    double takes = 0;            // s the find call takes (the fake clock moves on that much)
    std::vector<Button> buttons;
    int finds = 0;
};
struct Bar { HWND hwnd; DWORD pid; HMONITOR monitor; };

std::recursive_mutex mu;         // everything below (the worker's thread and the test's)
HWND mainBar = kMain;
std::vector<Bar> secondaries;
std::vector<Root> roots;
bool classic = false;
HRESULT create8 = S_OK, create7 = S_OK;
int creates = 0;
BOOL autoSetFocus = TRUE;
DWORD connectionTimeout = 0, transactionTimeout = 0;
std::wstring condClass;
PROPERTYID condProperty = 0;
std::vector<PROPERTYID> cached;
TreeScope scope = TreeScope_None;
std::vector<HWND> posted;
std::atomic<int> live{ 0 }, bstrs{ 0 };   // fake objects alive; BSTRs handed out and not freed
std::atomic<long long> skewUs{ 0 };       // the fake clock's lead over the real one
std::atomic<int> inside{ 0 }, maxInside{ 0 };
HANDLE gate = nullptr;           // set: a find call waits on it (a busy Explorer)

void Advance(double s) { skewUs += (long long)(s * 1e6); }
Root& R(HWND h) { for (auto& r : roots) if (r.hwnd == h) return r; static Root none; return none; }
Button& B(HWND root, size_t i) { return R(root).buttons[i]; }

// The usual taskbar: its XAML island (the bridge, with the buttons, and the bridge's input window), then itself.
void Reset() {
    std::lock_guard<std::recursive_mutex> lk(mu);
    mainBar = kMain;
    secondaries.clear();
    roots.clear();
    roots.push_back({ (HWND)(UINT_PTR)0x7B101, kMain, L"Windows.UI.Composition.DesktopWindowContentBridge" });
    roots.push_back({ (HWND)(UINT_PTR)0x7B102, kMain, L"Windows.UI.Input.InputSite.WindowClass" });
    roots.push_back({ kMain, kMain, L"Shell_TrayWnd" });
    classic = false;
    create8 = create7 = S_OK;
    posted.clear();
    gate = nullptr;
}
const HWND kIsland = (HWND)(UINT_PTR)0x7B101, kIsland2 = (HWND)(UINT_PTR)0x7B201;
void AddSecondary() {
    std::lock_guard<std::recursive_mutex> lk(mu);
    secondaries.push_back({ kSecondBar, kExplorer, kSecond });
    roots.push_back({ kIsland2, kSecondBar, L"Windows.UI.Composition.DesktopWindowContentBridge" });
    roots.push_back({ kSecondBar, kSecondBar, L"Shell_SecondaryTrayWnd" });
}
Button Btn(std::wstring id, std::wstring name, bool invoke = true, bool toggle = false) {
    Button b;
    b.id = std::move(id); b.name = std::move(name); b.invoke = invoke; b.toggle = toggle;
    return b;
}

#define NI override { return E_NOTIMPL; }
#define FAKE_UNKNOWN(I)                                                                                                     \
    LONG refs = 1;                                                                                                          \
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) override {                                                         \
        if (riid == __uuidof(IUnknown) || riid == __uuidof(I)) { *pp = static_cast<I*>(this); AddRef(); return S_OK; }    \
        *pp = nullptr;                                                                                                      \
        return E_NOINTERFACE;                                                                                               \
    }                                                                                                                       \
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }                                         \
    STDMETHODIMP_(ULONG) Release() override { const LONG n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
struct Counted { Counted() { ++live; } ~Counted() { --live; } };

// What UI Automation declares and the worker never calls (generated from the SDK's UIAutomationClient.h)
#define STUBS_IUIAutomation \
    STDMETHODIMP CompareElements(IUIAutomationElement*, IUIAutomationElement*, BOOL*) NI STDMETHODIMP CompareRuntimeIds(SAFEARRAY*, SAFEARRAY*, BOOL*) NI \
    STDMETHODIMP GetRootElement(IUIAutomationElement**) NI STDMETHODIMP ElementFromPoint(POINT, IUIAutomationElement**) NI \
    STDMETHODIMP GetFocusedElement(IUIAutomationElement**) NI STDMETHODIMP GetRootElementBuildCache(IUIAutomationCacheRequest*, IUIAutomationElement**) NI \
    STDMETHODIMP ElementFromHandleBuildCache(UIA_HWND, IUIAutomationCacheRequest*, IUIAutomationElement**) NI \
    STDMETHODIMP ElementFromPointBuildCache(POINT, IUIAutomationCacheRequest*, IUIAutomationElement**) NI \
    STDMETHODIMP GetFocusedElementBuildCache(IUIAutomationCacheRequest*, IUIAutomationElement**) NI \
    STDMETHODIMP CreateTreeWalker(IUIAutomationCondition*, IUIAutomationTreeWalker**) NI STDMETHODIMP get_ControlViewWalker(IUIAutomationTreeWalker**) NI \
    STDMETHODIMP get_ContentViewWalker(IUIAutomationTreeWalker**) NI STDMETHODIMP get_RawViewWalker(IUIAutomationTreeWalker**) NI \
    STDMETHODIMP get_RawViewCondition(IUIAutomationCondition**) NI STDMETHODIMP get_ControlViewCondition(IUIAutomationCondition**) NI \
    STDMETHODIMP get_ContentViewCondition(IUIAutomationCondition**) NI STDMETHODIMP CreateTrueCondition(IUIAutomationCondition**) NI \
    STDMETHODIMP CreateFalseCondition(IUIAutomationCondition**) NI \
    STDMETHODIMP CreatePropertyConditionEx(PROPERTYID, VARIANT, enum PropertyConditionFlags, IUIAutomationCondition**) NI \
    STDMETHODIMP CreateAndCondition(IUIAutomationCondition*, IUIAutomationCondition*, IUIAutomationCondition**) NI \
    STDMETHODIMP CreateAndConditionFromArray(SAFEARRAY*, IUIAutomationCondition**) NI \
    STDMETHODIMP CreateAndConditionFromNativeArray(IUIAutomationCondition**, int, IUIAutomationCondition**) NI \
    STDMETHODIMP CreateOrCondition(IUIAutomationCondition*, IUIAutomationCondition*, IUIAutomationCondition**) NI \
    STDMETHODIMP CreateOrConditionFromArray(SAFEARRAY*, IUIAutomationCondition**) NI \
    STDMETHODIMP CreateOrConditionFromNativeArray(IUIAutomationCondition**, int, IUIAutomationCondition**) NI \
    STDMETHODIMP CreateNotCondition(IUIAutomationCondition*, IUIAutomationCondition**) NI \
    STDMETHODIMP AddAutomationEventHandler(EVENTID, IUIAutomationElement*, enum TreeScope, IUIAutomationCacheRequest*, IUIAutomationEventHandler*) NI \
    STDMETHODIMP RemoveAutomationEventHandler(EVENTID, IUIAutomationElement*, IUIAutomationEventHandler*) NI \
    STDMETHODIMP AddPropertyChangedEventHandlerNativeArray(IUIAutomationElement*, enum TreeScope, IUIAutomationCacheRequest*, IUIAutomationPropertyChangedEventHandler*, PROPERTYID*, int) NI \
    STDMETHODIMP AddPropertyChangedEventHandler(IUIAutomationElement*, enum TreeScope, IUIAutomationCacheRequest*, IUIAutomationPropertyChangedEventHandler*, SAFEARRAY*) NI \
    STDMETHODIMP RemovePropertyChangedEventHandler(IUIAutomationElement*, IUIAutomationPropertyChangedEventHandler*) NI \
    STDMETHODIMP AddStructureChangedEventHandler(IUIAutomationElement*, enum TreeScope, IUIAutomationCacheRequest*, IUIAutomationStructureChangedEventHandler*) NI \
    STDMETHODIMP RemoveStructureChangedEventHandler(IUIAutomationElement*, IUIAutomationStructureChangedEventHandler*) NI \
    STDMETHODIMP AddFocusChangedEventHandler(IUIAutomationCacheRequest*, IUIAutomationFocusChangedEventHandler*) NI \
    STDMETHODIMP RemoveFocusChangedEventHandler(IUIAutomationFocusChangedEventHandler*) NI STDMETHODIMP RemoveAllEventHandlers(void) NI \
    STDMETHODIMP IntNativeArrayToSafeArray(int*, int, SAFEARRAY**) NI STDMETHODIMP IntSafeArrayToNativeArray(SAFEARRAY*, int**, int*) NI \
    STDMETHODIMP RectToVariant(RECT, VARIANT*) NI STDMETHODIMP VariantToRect(VARIANT, RECT*) NI \
    STDMETHODIMP SafeArrayToRectNativeArray(SAFEARRAY*, RECT**, int*) NI \
    STDMETHODIMP CreateProxyFactoryEntry(IUIAutomationProxyFactory*, IUIAutomationProxyFactoryEntry**) NI \
    STDMETHODIMP get_ProxyFactoryMapping(IUIAutomationProxyFactoryMapping**) NI STDMETHODIMP GetPropertyProgrammaticName(PROPERTYID, BSTR*) NI \
    STDMETHODIMP GetPatternProgrammaticName(PATTERNID, BSTR*) NI \
    STDMETHODIMP PollForPotentialSupportedPatterns(IUIAutomationElement*, SAFEARRAY**, SAFEARRAY**) NI \
    STDMETHODIMP PollForPotentialSupportedProperties(IUIAutomationElement*, SAFEARRAY**, SAFEARRAY**) NI STDMETHODIMP CheckNotSupported(VARIANT, BOOL*) NI \
    STDMETHODIMP get_ReservedNotSupportedValue(IUnknown**) NI STDMETHODIMP get_ReservedMixedAttributeValue(IUnknown**) NI \
    STDMETHODIMP ElementFromIAccessible(IAccessible*, int, IUIAutomationElement**) NI \
    STDMETHODIMP ElementFromIAccessibleBuildCache(IAccessible*, int, IUIAutomationCacheRequest*, IUIAutomationElement**) NI
#define STUBS_IUIAutomation2 \
    STDMETHODIMP get_AutoSetFocus(BOOL*) NI STDMETHODIMP get_ConnectionTimeout(DWORD*) NI STDMETHODIMP get_TransactionTimeout(DWORD*) NI
#define STUBS_IUIAutomationElement \
    STDMETHODIMP SetFocus(void) NI STDMETHODIMP GetRuntimeId(SAFEARRAY**) NI \
    STDMETHODIMP FindFirst(enum TreeScope, IUIAutomationCondition*, IUIAutomationElement**) NI \
    STDMETHODIMP FindAll(enum TreeScope, IUIAutomationCondition*, IUIAutomationElementArray**) NI \
    STDMETHODIMP FindFirstBuildCache(enum TreeScope, IUIAutomationCondition*, IUIAutomationCacheRequest*, IUIAutomationElement**) NI \
    STDMETHODIMP BuildUpdatedCache(IUIAutomationCacheRequest*, IUIAutomationElement**) NI STDMETHODIMP GetCurrentPropertyValue(PROPERTYID, VARIANT*) NI \
    STDMETHODIMP GetCurrentPropertyValueEx(PROPERTYID, BOOL, VARIANT*) NI STDMETHODIMP GetCachedPropertyValue(PROPERTYID, VARIANT*) NI \
    STDMETHODIMP GetCachedPropertyValueEx(PROPERTYID, BOOL, VARIANT*) NI STDMETHODIMP GetCachedPatternAs(PATTERNID, REFIID, void**) NI \
    STDMETHODIMP GetCurrentPattern(PATTERNID, IUnknown**) NI STDMETHODIMP GetCachedPattern(PATTERNID, IUnknown**) NI \
    STDMETHODIMP GetCachedParent(IUIAutomationElement**) NI STDMETHODIMP GetCachedChildren(IUIAutomationElementArray**) NI \
    STDMETHODIMP get_CurrentProcessId(int*) NI STDMETHODIMP get_CurrentControlType(CONTROLTYPEID*) NI \
    STDMETHODIMP get_CurrentLocalizedControlType(BSTR*) NI STDMETHODIMP get_CurrentName(BSTR*) NI STDMETHODIMP get_CurrentAcceleratorKey(BSTR*) NI \
    STDMETHODIMP get_CurrentAccessKey(BSTR*) NI STDMETHODIMP get_CurrentHasKeyboardFocus(BOOL*) NI STDMETHODIMP get_CurrentIsKeyboardFocusable(BOOL*) NI \
    STDMETHODIMP get_CurrentIsEnabled(BOOL*) NI STDMETHODIMP get_CurrentAutomationId(BSTR*) NI STDMETHODIMP get_CurrentClassName(BSTR*) NI \
    STDMETHODIMP get_CurrentHelpText(BSTR*) NI STDMETHODIMP get_CurrentCulture(int*) NI STDMETHODIMP get_CurrentIsControlElement(BOOL*) NI \
    STDMETHODIMP get_CurrentIsContentElement(BOOL*) NI STDMETHODIMP get_CurrentIsPassword(BOOL*) NI \
    STDMETHODIMP get_CurrentNativeWindowHandle(UIA_HWND*) NI STDMETHODIMP get_CurrentItemType(BSTR*) NI STDMETHODIMP get_CurrentIsOffscreen(BOOL*) NI \
    STDMETHODIMP get_CurrentOrientation(enum OrientationType*) NI STDMETHODIMP get_CurrentFrameworkId(BSTR*) NI \
    STDMETHODIMP get_CurrentIsRequiredForForm(BOOL*) NI STDMETHODIMP get_CurrentItemStatus(BSTR*) NI STDMETHODIMP get_CurrentBoundingRectangle(RECT*) NI \
    STDMETHODIMP get_CurrentLabeledBy(IUIAutomationElement**) NI STDMETHODIMP get_CurrentAriaRole(BSTR*) NI \
    STDMETHODIMP get_CurrentAriaProperties(BSTR*) NI STDMETHODIMP get_CurrentIsDataValidForForm(BOOL*) NI \
    STDMETHODIMP get_CurrentControllerFor(IUIAutomationElementArray**) NI STDMETHODIMP get_CurrentDescribedBy(IUIAutomationElementArray**) NI \
    STDMETHODIMP get_CurrentFlowsTo(IUIAutomationElementArray**) NI STDMETHODIMP get_CurrentProviderDescription(BSTR*) NI \
    STDMETHODIMP get_CachedProcessId(int*) NI STDMETHODIMP get_CachedControlType(CONTROLTYPEID*) NI STDMETHODIMP get_CachedLocalizedControlType(BSTR*) NI \
    STDMETHODIMP get_CachedAcceleratorKey(BSTR*) NI STDMETHODIMP get_CachedAccessKey(BSTR*) NI STDMETHODIMP get_CachedHasKeyboardFocus(BOOL*) NI \
    STDMETHODIMP get_CachedIsKeyboardFocusable(BOOL*) NI STDMETHODIMP get_CachedIsEnabled(BOOL*) NI STDMETHODIMP get_CachedClassName(BSTR*) NI \
    STDMETHODIMP get_CachedHelpText(BSTR*) NI STDMETHODIMP get_CachedCulture(int*) NI STDMETHODIMP get_CachedIsControlElement(BOOL*) NI \
    STDMETHODIMP get_CachedIsContentElement(BOOL*) NI STDMETHODIMP get_CachedIsPassword(BOOL*) NI STDMETHODIMP get_CachedNativeWindowHandle(UIA_HWND*) NI \
    STDMETHODIMP get_CachedItemType(BSTR*) NI STDMETHODIMP get_CachedIsOffscreen(BOOL*) NI STDMETHODIMP get_CachedOrientation(enum OrientationType*) NI \
    STDMETHODIMP get_CachedFrameworkId(BSTR*) NI STDMETHODIMP get_CachedIsRequiredForForm(BOOL*) NI STDMETHODIMP get_CachedItemStatus(BSTR*) NI \
    STDMETHODIMP get_CachedBoundingRectangle(RECT*) NI STDMETHODIMP get_CachedLabeledBy(IUIAutomationElement**) NI \
    STDMETHODIMP get_CachedAriaRole(BSTR*) NI STDMETHODIMP get_CachedAriaProperties(BSTR*) NI STDMETHODIMP get_CachedIsDataValidForForm(BOOL*) NI \
    STDMETHODIMP get_CachedControllerFor(IUIAutomationElementArray**) NI STDMETHODIMP get_CachedDescribedBy(IUIAutomationElementArray**) NI \
    STDMETHODIMP get_CachedFlowsTo(IUIAutomationElementArray**) NI STDMETHODIMP get_CachedProviderDescription(BSTR*) NI \
    STDMETHODIMP GetClickablePoint(POINT*, BOOL*) NI

BSTR Bstr(const std::wstring& s) { ++bstrs; return SysAllocStringLen(s.data(), (UINT)s.size()); }

struct InvokeWay : IUIAutomationInvokePattern, Counted {
    FAKE_UNKNOWN(IUIAutomationInvokePattern)
    HWND root; size_t i;
    InvokeWay(HWND r, size_t n) : root(r), i(n) {}
    STDMETHODIMP Invoke() override {
        std::function<void()> on;
        HRESULT hr = S_OK;
        {
            std::lock_guard<std::recursive_mutex> lk(mu);
            Button& b = B(root, i);
            if (b.invokes < (int)b.pressHr.size()) hr = b.pressHr[b.invokes];
            ++b.invokes;
            on = b.onPress;
        }
        if (on) on();
        return hr;
    }
};
struct ToggleWay : IUIAutomationTogglePattern, Counted {
    FAKE_UNKNOWN(IUIAutomationTogglePattern)
    HWND root; size_t i;
    ToggleWay(HWND r, size_t n) : root(r), i(n) {}
    STDMETHODIMP Toggle() override {
        std::lock_guard<std::recursive_mutex> lk(mu);
        Button& b = B(root, i);
        const HRESULT hr = b.toggles < (int)b.pressHr.size() ? b.pressHr[b.toggles] : S_OK;
        ++b.toggles;
        return hr;
    }
    STDMETHODIMP get_CurrentToggleState(ToggleState*) NI
    STDMETHODIMP get_CachedToggleState(ToggleState*) NI
};
struct Element;
struct Array : IUIAutomationElementArray, Counted {
    FAKE_UNKNOWN(IUIAutomationElementArray)
    std::vector<ComPtr<IUIAutomationElement>> items;
    STDMETHODIMP get_Length(int* n) override { *n = (int)items.size(); return S_OK; }
    STDMETHODIMP GetElement(int i, IUIAutomationElement** e) override {
        if (i < 0 || i >= (int)items.size()) return E_INVALIDARG;
        *e = items[i].Get();
        (*e)->AddRef();
        return S_OK;
    }
};
struct Condition : IUIAutomationCondition, Counted { FAKE_UNKNOWN(IUIAutomationCondition) };
struct Cache : IUIAutomationCacheRequest, Counted {
    FAKE_UNKNOWN(IUIAutomationCacheRequest)
    STDMETHODIMP AddProperty(PROPERTYID p) override { std::lock_guard<std::recursive_mutex> lk(mu); cached.push_back(p); return S_OK; }
    STDMETHODIMP AddPattern(PATTERNID) NI STDMETHODIMP Clone(IUIAutomationCacheRequest**) NI STDMETHODIMP get_TreeScope(enum TreeScope*) NI
    STDMETHODIMP put_TreeScope(enum TreeScope) NI STDMETHODIMP get_TreeFilter(IUIAutomationCondition**) NI STDMETHODIMP put_TreeFilter(IUIAutomationCondition*) NI
    STDMETHODIMP get_AutomationElementMode(enum AutomationElementMode*) NI STDMETHODIMP put_AutomationElementMode(enum AutomationElementMode) NI
};
// a root (button == SIZE_MAX) or one of its buttons
struct Element : IUIAutomationElement, Counted {
    FAKE_UNKNOWN(IUIAutomationElement)
    HWND root; size_t button;
    Element(HWND r, size_t b) : root(r), button(b) {}
    STDMETHODIMP FindAllBuildCache(enum TreeScope s, IUIAutomationCondition* cond, IUIAutomationCacheRequest* cache, IUIAutomationElementArray** found) override {
        *found = nullptr;
        if (button != SIZE_MAX || !cond || !cache) return E_INVALIDARG;
        const int now = ++inside;
        for (int m = maxInside; now > m && !maxInside.compare_exchange_weak(m, now);) {}
        HANDLE wait = nullptr;
        { std::lock_guard<std::recursive_mutex> lk(mu); wait = gate; }
        if (wait) WaitForSingleObject(wait, 5000);
        HRESULT hr = S_OK;
        auto* arr = new Array;
        {
            std::lock_guard<std::recursive_mutex> lk(mu);
            scope = s;
            Root& r = R(root);
            ++r.finds;
            Advance(r.takes);
            hr = r.findHr;
            if (SUCCEEDED(hr))
                for (size_t i = 0; i < r.buttons.size(); ++i) arr->items.push_back(ComPtr<IUIAutomationElement>(new Element(root, i)));
            if (SUCCEEDED(hr)) for (auto& e : arr->items) e->Release();   // the ComPtr took a reference of its own
        }
        --inside;
        if (FAILED(hr)) { arr->Release(); return hr; }
        *found = arr;
        return S_OK;
    }
    STDMETHODIMP get_CachedAutomationId(BSTR* out) override { std::lock_guard<std::recursive_mutex> lk(mu); *out = Bstr(B(root, button).id); return S_OK; }
    STDMETHODIMP get_CachedName(BSTR* out) override { std::lock_guard<std::recursive_mutex> lk(mu); *out = Bstr(B(root, button).name); return S_OK; }
    STDMETHODIMP GetCurrentPatternAs(PATTERNID pattern, REFIID riid, void** pp) override {
        *pp = nullptr;
        std::function<void()> on;
        HRESULT hr = S_OK;
        bool invoke = false, toggle = false;
        {
            std::lock_guard<std::recursive_mutex> lk(mu);
            Button& b = B(root, button);
            ++b.reads;
            on = b.onRead;
            hr = b.readHr;
            invoke = b.invoke;
            toggle = b.toggle;
        }
        if (on) on();
        if (FAILED(hr)) return hr;
        IUnknown* p = nullptr;
        if (pattern == UIA_InvokePatternId && invoke) p = static_cast<IUIAutomationInvokePattern*>(new InvokeWay(root, button));
        else if (pattern == UIA_TogglePatternId && toggle) p = static_cast<IUIAutomationTogglePattern*>(new ToggleWay(root, button));
        if (!p) return S_OK;   // as UI Automation: a pattern not offered is no error
        hr = p->QueryInterface(riid, pp);
        p->Release();
        return hr;
    }
    STUBS_IUIAutomationElement
};
struct Uia : IUIAutomation2, Counted {
    LONG refs = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IUIAutomation) || riid == __uuidof(IUIAutomation2)) { *pp = static_cast<IUIAutomation2*>(this); AddRef(); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override { const LONG n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
    STDMETHODIMP ElementFromHandle(UIA_HWND h, IUIAutomationElement** e) override {
        *e = nullptr;
        std::lock_guard<std::recursive_mutex> lk(mu);
        Root& r = R((HWND)h);
        if (!r.hwnd) return UIA_E_ELEMENTNOTAVAILABLE;
        if (FAILED(r.elementHr)) return r.elementHr;
        *e = new Element((HWND)h, SIZE_MAX);
        return S_OK;
    }
    STDMETHODIMP CreatePropertyCondition(PROPERTYID p, VARIANT v, IUIAutomationCondition** c) override {
        std::lock_guard<std::recursive_mutex> lk(mu);
        condProperty = p;
        condClass = v.vt == VT_BSTR && v.bstrVal ? std::wstring(v.bstrVal, SysStringLen(v.bstrVal)) : L"";
        *c = new Condition;
        return S_OK;
    }
    STDMETHODIMP CreateCacheRequest(IUIAutomationCacheRequest** c) override { *c = new Cache; return S_OK; }
    STDMETHODIMP put_AutoSetFocus(BOOL b) override { autoSetFocus = b; return S_OK; }
    STDMETHODIMP put_ConnectionTimeout(DWORD ms) override { connectionTimeout = ms; return S_OK; }
    STDMETHODIMP put_TransactionTimeout(DWORD ms) override { transactionTimeout = ms; return S_OK; }
    STUBS_IUIAutomation
    STUBS_IUIAutomation2
};

// ---- the Win32 calls taskbar_apps.cpp makes, answered from the fake Explorer above
HWND FakeExplorerTaskbar() { std::lock_guard<std::recursive_mutex> lk(mu); return mainBar; }
HWND WINAPI FakeFindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    if (parent && parent == mainBar && cls && !wcscmp(cls, L"TrayNotifyWnd")) return classic ? kNotify : nullptr;
    if (!parent && cls && !wcscmp(cls, L"Shell_SecondaryTrayWnd")) {
        bool past = !after;
        for (const auto& s : secondaries) {
            if (past) return s.hwnd;
            if (s.hwnd == after) past = true;
        }
    }
    return nullptr;
}
HWND WINAPI FakeGetDlgItem(HWND h, int id) { return h == kNotify && id == 1502 ? (HWND)(UINT_PTR)0x7B1F1 : nullptr; }
DWORD WINAPI FakeGetWindowThreadProcessId(HWND h, LPDWORD pid) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    DWORD p = h == mainBar ? kExplorer : 0;
    for (const auto& s : secondaries) if (s.hwnd == h) p = s.pid;
    if (pid) *pid = p;
    return p ? 1 : 0;
}
HMONITOR WINAPI FakeMonitorFromWindow(HWND h, DWORD) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    for (const auto& s : secondaries) if (s.hwnd == h) return s.monitor;
    return kPrimary;
}
HMONITOR WINAPI FakeMonitorFromPoint(POINT, DWORD) { return kPrimary; }
BOOL WINAPI FakeEnumChildWindows(HWND parent, WNDENUMPROC proc, LPARAM lp) {
    std::vector<HWND> kids;
    {
        std::lock_guard<std::recursive_mutex> lk(mu);
        for (const auto& r : roots) if (r.bar == parent && r.hwnd != parent) kids.push_back(r.hwnd);
    }
    for (HWND k : kids) if (!proc(k, lp)) break;
    return TRUE;
}
int WINAPI FakeGetClassNameW(HWND h, LPWSTR buf, int n) {
    std::lock_guard<std::recursive_mutex> lk(mu);
    const std::wstring cls = R(h).cls;
    if (n <= 0) return 0;
    wcsncpy_s(buf, n, cls.c_str(), _TRUNCATE);
    return (int)wcslen(buf);
}
HRESULT WINAPI FakeCoCreateInstance(REFCLSID clsid, LPUNKNOWN, DWORD, REFIID riid, LPVOID* pp) {
    *pp = nullptr;
    std::lock_guard<std::recursive_mutex> lk(mu);
    ++creates;
    const HRESULT hr = clsid == __uuidof(CUIAutomation8) ? create8 : clsid == __uuidof(CUIAutomation) ? create7 : REGDB_E_CLASSNOTREG;
    if (FAILED(hr)) return hr;
    auto* u = new Uia;
    const HRESULT q = u->QueryInterface(riid, pp);
    u->Release();
    return q;
}
BOOL WINAPI FakePostMessageW(HWND h, UINT, WPARAM, LPARAM) { std::lock_guard<std::recursive_mutex> lk(mu); posted.push_back(h); return TRUE; }
void WINAPI FakeSysFreeString(BSTR b) { if (b) --bstrs; SysFreeString(b); }
}   // namespace fake

#define ExplorerTaskbar fake::FakeExplorerTaskbar
#define FindWindowExW fake::FakeFindWindowExW
#define GetDlgItem fake::FakeGetDlgItem
#define GetWindowThreadProcessId fake::FakeGetWindowThreadProcessId
#define MonitorFromWindow fake::FakeMonitorFromWindow
#define MonitorFromPoint fake::FakeMonitorFromPoint
#define EnumChildWindows fake::FakeEnumChildWindows
#define GetClassNameW fake::FakeGetClassNameW
#define CoCreateInstance fake::FakeCoCreateInstance
#define PostMessageW fake::FakePostMessageW
#define SysFreeString fake::FakeSysFreeString
#include "../src/taskbar_apps.cpp"
#undef ExplorerTaskbar
#undef FindWindowExW
#undef GetDlgItem
#undef GetWindowThreadProcessId
#undef MonitorFromWindow
#undef MonitorFromPoint
#undef EnumChildWindows
#undef GetClassNameW
#undef CoCreateInstance
#undef PostMessageW
#undef SysFreeString

// the dock's clock (common.cpp), ahead by what the fake Explorer's calls "took"
double NowSeconds() {
    static const LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart) + fake::skewUs.load() / 1e6;
}

namespace {
using namespace taskbarapps;
using fake::Btn;

Job J(std::wstring aumid, std::wstring exe = L"", bool running = true, HMONITOR monitor = fake::kPrimary) {
    static uint32_t ids = 100;
    Job j;
    j.id = ++ids;
    j.aumid = std::move(aumid);
    j.exe = std::move(exe);
    j.running = running;
    j.monitor = monitor;
    return j;
}
// As the dock's worker does it, on a thread of the multithreaded apartment.
Result Press(const Job& job) {
    Result r;
    std::thread([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        r = PressNow(job);
        CoUninitialize();
    }).join();
    return r;
}
void Island(fake::Button b, HWND root = fake::kIsland) { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::R(root).buttons.push_back(std::move(b)); }
fake::Button& At(size_t i, HWND root = fake::kIsland) { return fake::B(root, i); }
int Finds(HWND root) { std::lock_guard<std::recursive_mutex> lk(fake::mu); return fake::R(root).finds; }
int Presses(size_t i, HWND root = fake::kIsland) { std::lock_guard<std::recursive_mutex> lk(fake::mu); return fake::B(root, i).invokes + fake::B(root, i).toggles; }
bool Wait(uint32_t job, Result& r, int ms) {
    for (int t = 0; t < ms; t += 5) { if (Take(job, r)) return true; Sleep(5); }
    return false;
}
const std::wstring kChrome = L"Appid: Chrome";
}   // namespace

int main() {
    // ---- the press, the usual way: its one window's button, by its AppUserModelID
    fake::Reset();
    Island(Btn(L"Appid: Microsoft.WindowsTerminal_8wekyb3d8bbwe!App", L"터미널 - 2개의 실행 중인 창"));
    Island(Btn(kChrome, L"Chrome - 1개의 실행 중인 창 고정됨"));
    Island(Btn(L"Appid: {7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"카카오톡 고정됨"));
    Result r = Press(J(L"chrome"));
    Check(r.outcome == Outcome::Pressed && !wcscmp(r.how, L"invoke") && Presses(1) == 1 && Presses(0) == 0 && Presses(2) == 0,
          "an app with one window: its button (id compared without case) pressed once, by Invoke; no other button");
    Check(r.button == L"Chrome" && r.count == 1 && r.buttons == 3 && r.taskbar == fake::kMain && r.lookup >= 0,
          "...the result names it (id without \"Appid: \"), the windows it counts, the buttons seen, its taskbar");
    Check(fake::condProperty == UIA_ClassNamePropertyId && fake::condClass == L"Taskbar.TaskListButtonAutomationPeer" && fake::scope == TreeScope_Descendants,
          "the buttons are looked for by their class, among all descendants");
    Check(std::count(fake::cached.begin(), fake::cached.end(), UIA_AutomationIdPropertyId) >= 1 &&
          std::count(fake::cached.begin(), fake::cached.end(), UIA_NamePropertyId) >= 1,
          "...each with its id and name in the same call (FindAllBuildCache)");
    Check(fake::autoSetFocus == FALSE && fake::connectionTimeout == 500 && fake::transactionTimeout == 500,
          "UI Automation never moves the focus to the hidden taskbar, and gives up on a busy Explorer after 500 ms a call");
    Check(Finds(fake::kIsland) == 1 && Finds(fake::kMain) == 0, "the XAML island with the buttons is looked into first, and alone");

    // ---- by the program's path: a known folder written as its id (resolved by the system)
    r = Press(J(L"", L"C:\\Program Files (x86)\\Kakao\\KakaoTalk\\KakaoTalk.exe", false));
    Check(r.outcome == Outcome::Pressed && Presses(2) == 1, "a pinned app not running, by its path in a known folder (Program Files (x86)'s id): its pinned button starts it");
    r = Press(J(L"", L"C:\\Program Files\\Kakao\\KakaoTalk\\KakaoTalk.exe", false));
    Check(r.outcome == Outcome::NoButton && Presses(2) == 1, "the same program in another folder: no button names it, nothing pressed");
    r = Press(J(L"Some.Other.Id", L"", true));
    Check(r.outcome == Outcome::NoButton && r.buttons == 3 && !wcscmp(OutcomeText(r.outcome), L"no button of its on the taskbar"), "an app with no button: NoButton, the buttons seen counted");
    {   // the system's own resolver (SHGetKnownFolderPath): ids it does not know, and ones not written as a GUID
        wchar_t x86[MAX_PATH] = L"";
        ExpandEnvironmentStringsW(L"%ProgramFiles(x86)%", x86, MAX_PATH);
        Check(SameText(Lowered(KnownFolderPath(L"{7c5a40ef-a0fb-4bfc-874a-c0f2e0b9fa8e}")), Lowered(x86)),
              "a known folder's id, lowered as the dock lowers the button's id: the folder (Program Files (x86))");
        Check(KnownFolderPath(L"{00000000-0000-0000-0000-000000000000}").empty() && KnownFolderPath(L"{7C5A40EF-A0FB}").empty() &&
              KnownFolderPath(L"{not a guid}").empty() && KnownFolderPath(L"{}").empty() && KnownFolderPath(L"").empty(),
              "an id the system does not know, a cut-short or malformed one, none: no folder");
        Island(Btn(L"Appid: {00000000-0000-0000-0000-000000000000}\\Kakao\\KakaoTalk\\KakaoTalk.exe", L"카카오톡 - 1개의 실행 중인 창"));
        Island(Btn(L"Appid: {Kakao}\\KakaoTalk.exe", L"카카오톡 - 1개의 실행 중인 창"));
        r = Press(J(L"", L"C:\\Kakao\\KakaoTalk\\KakaoTalk.exe"));
        Check(r.outcome == Outcome::NoButton && Presses(3) == 0 && Presses(4) == 0 && r.buttons == 5,
              "buttons named by a folder id the system does not know, or by no GUID at all: no match, nothing pressed");
        std::lock_guard<std::recursive_mutex> lk(fake::mu);
        fake::R(fake::kIsland).buttons.resize(3);
    }
    {   // the window's own id, else the app's
        Job j = J(L"chrome.profile7");
        j.otherAumid = L"Chrome";
        r = Press(j);
        Check(r.outcome == Outcome::Pressed && Presses(1) == 2, "a window with an id of its own: the app's id names its button too (otherAumid)");
    }

    // ---- by the windows it counts
    r = Press(J(L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"));
    Check(r.outcome == Outcome::CountDiffers && r.count == 2 && Presses(0) == 0,
          "its button counts two windows, the dock one: not pressed (Windows' thumbnails would come up at the hidden taskbar)");
    r = Press(J(L"Chrome", L"", false));
    Check(r.outcome == Outcome::CountDiffers && Presses(1) == 2, "a launch, but its button counts a window: not pressed (it would bring that one up)");
    r = Press(J(L"", L"C:\\Program Files (x86)\\Kakao\\KakaoTalk\\KakaoTalk.exe", true));
    Check(r.outcome == Outcome::CountDiffers && r.count == -1 && Presses(2) == 1, "a running app's window, but only its pinned button: not pressed (it would start the app again)");

    // ---- several taskbars
    fake::Reset();
    fake::AddSecondary();
    Island(Btn(kChrome, L"Chrome 고정됨"));                                    // the main taskbar: its pinned copy
    Island(Btn(kChrome, L"Chrome - 1개의 실행 중인 창 고정됨"), fake::kIsland2);   // its window's monitor
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Pressed && Presses(0) == 0 && Presses(0, fake::kIsland2) == 1 && r.taskbar == fake::kSecondBar,
          "its window shown only on the other monitor's taskbar: the main one's pinned copy passed over, that one pressed");
    Check(r.buttons == 2, "...the buttons of both taskbars seen");
    r = Press(J(L"Chrome", L"", false));
    Check(r.outcome == Outcome::Pressed && Presses(0) == 1, "a launch: the pinned copy");
    fake::Reset();
    fake::AddSecondary();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    Island(Btn(kChrome, L"Chrome - 1 running window"), fake::kIsland2);
    r = Press(J(L"Chrome", L"", true, fake::kSecond));
    Check(r.outcome == Outcome::Pressed && Presses(0) == 0 && Presses(0, fake::kIsland2) == 1 && Finds(fake::kIsland) == 0,
          "a button on every taskbar, the dock clicked on the second monitor: that monitor's taskbar first (the main one not looked into)");
    r = Press(J(L"Chrome", L"", true, fake::kPrimary));
    Check(r.outcome == Outcome::Pressed && Presses(0) == 1 && Presses(0, fake::kIsland2) == 1, "...on the primary monitor: the main taskbar's");
    {   // a secondary taskbar of another process (a program of its own that names its windows so): never looked into
        fake::Reset();
        { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::secondaries.push_back({ fake::kSecondBar, 0x7C99, fake::kSecond }); }
        { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::roots.push_back({ fake::kIsland2, fake::kSecondBar, L"Windows.UI.Composition.DesktopWindowContentBridge" }); }
        Island(Btn(kChrome, L"Chrome - 1 running window"), fake::kIsland2);
        r = Press(J(L"Chrome", L"", true, fake::kSecond));
        Check(r.outcome == Outcome::NoButton && Finds(fake::kIsland2) == 0, "a 'secondary taskbar' window of another process than Explorer's: not looked into");
    }

    // ---- the ways to press it
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window", false, true));
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Pressed && !wcscmp(r.how, L"toggle") && At(0).toggles == 1, "no Invoke: Toggle");
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window", true, true));
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Pressed && At(0).invokes == 1 && At(0).toggles == 0 && At(0).reads == 1, "both: Invoke alone (Toggle not even read)");
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window", false, false));
    {
        Job j = J(L"Chrome");
        r = Press(j);
        Check(r.outcome == Outcome::NoWay && r.hr == E_NOINTERFACE && Presses(0) == 0 && At(0).reads == 2,
              "neither (a 25H2 build: only the accessibility default action): NoWay, nothing pressed");
        Check(j.stage->load() == kLooking, "...and no press claimed: the dock may still go its own way meanwhile");
        Check(r.lookup >= 0 && r.buttons == 1 && r.button == L"Chrome", "...the lookup's time and the button for the log");
    }
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).readHr = static_cast<HRESULT>(UIA_E_TIMEOUT); }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Failed && r.slow && Presses(0) == 0, "its patterns could not be read in time: not pressed, Explorer taken for slow");
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).readHr = E_ACCESSDENIED; At(0).reads = 0; }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Failed && !r.slow && At(0).reads == 1, "...refused: not pressed, not slow");

    // ---- a press that went away or may still arrive
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).pressHr = { static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) }; }
    {
        Job j = J(L"Chrome");
        r = Press(j);
        Check(r.outcome == Outcome::Pressed && At(0).invokes == 2 && Finds(fake::kIsland) == 2,
              "its button gone at the press (Explorer rebuilt it): looked up once more and pressed");
        Check(j.stage->load() == kPressing, "...the press claimed again for the second try");
    }
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).pressHr = { RPC_E_DISCONNECTED, RPC_E_DISCONNECTED, S_OK }; }
    {
        Job j = J(L"Chrome");
        r = Press(j);
        Check(r.outcome == Outcome::Failed && At(0).invokes == 2 && !wcscmp(r.how, L"the button went away twice"), "gone twice: Failed, no third try");
        Check(j.stage->load() == kLooking, "...and the press given back (the dock may go its own way: nothing arrived)");
    }
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).pressHr = { static_cast<HRESULT>(UIA_E_TIMEOUT) }; }
    {
        Job j = J(L"Chrome");
        r = Press(j);
        Check(r.outcome == Outcome::Unsure && At(0).invokes == 1 && j.stage->load() == kPressing,
              "the press timed out: Unsure (it may still arrive), no second press, and the dock cannot go its own way any more");
    }
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).pressHr = { RPC_E_SERVER_DIED }; At(0).invokes = 0; }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Unsure && At(0).invokes == 1, "Explorer died during the press: unsure too, nothing pressed again");
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).pressHr = { HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) }; At(0).invokes = 0; }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Unsure && At(0).invokes == 1, "the press call failed mid-way: unsure too, nothing pressed again");
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).pressHr = {}; At(0).invokes = 0; At(0).reads = 0; At(0).readHr = RPC_E_SERVER_DIED; }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Failed && At(0).reads == 2 && Presses(0) == 0 && !wcscmp(r.how, L"the button went away twice"),
          "Explorer died while its patterns were read: no press was made - looked up once more, then given up (nothing pressed)");
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).readHr = S_OK; }
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).pressHr = { E_ACCESSDENIED }; At(0).invokes = 0; }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Failed && !r.slow && At(0).invokes == 1, "the press refused: Failed (the dock does the click itself)");

    // ---- the dock giving up on the press (Stage): no press after it
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    {
        Job j = J(L"Chrome");
        Abandon(*j.stage);
        r = Press(j);
        Check(r.outcome == Outcome::Abandoned && Finds(fake::kIsland) == 0 && Presses(0) == 0, "given up before the lookup: nothing looked up or pressed");
    }
    {
        Job j = J(L"Chrome");
        auto stage = j.stage;
        { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).onRead = [stage] { Abandon(*stage); }; }
        r = Press(j);
        Check(r.outcome == Outcome::Abandoned && Presses(0) == 0, "given up while its patterns are read: no press");
        { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).onRead = nullptr; }
    }
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    {
        Job j = J(L"Chrome");
        auto stage = j.stage;
        { std::lock_guard<std::recursive_mutex> lk(fake::mu); At(0).pressHr = { static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) };
          At(0).onRead = [stage, n = std::make_shared<int>(0)] { if (++*n == 2) Abandon(*stage); }; }
        r = Press(j);
        Check(r.outcome == Outcome::Abandoned && At(0).invokes == 1, "the dock gave up after a press that went away: the second try presses nothing");
    }

    {   // the worker about to press and the dock done waiting, at the same moment on two threads: exactly one of them wins
        int both = 0, neither = 0;
        for (int i = 0; i < 2000; ++i) {
            std::atomic<int> stage{ kLooking }, go{ 0 };
            bool claimed = false, abandoned = false;
            std::thread worker([&] { while (!go) {} claimed = ClaimPress(stage); });
            std::thread dock([&] { while (!go) {} abandoned = Abandon(stage); });
            go = 1;
            worker.join();
            dock.join();
            both += claimed && abandoned;
            neither += !claimed && !abandoned;
        }
        Check(both == 0 && neither == 0, "a press claimed and the dock giving up at once (2000 times on two threads): always exactly one of them");
    }

    // ---- a busy Explorer: the lookup's budget
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    { std::lock_guard<std::recursive_mutex> lk(fake::mu);
      fake::R(fake::kIsland).findHr = static_cast<HRESULT>(UIA_E_TIMEOUT); fake::R(fake::kIsland).takes = 0.8;
      fake::R((HWND)(UINT_PTR)0x7B102).findHr = static_cast<HRESULT>(UIA_E_TIMEOUT); fake::R((HWND)(UINT_PTR)0x7B102).takes = 0.8; }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Failed && r.slow && Finds(fake::kMain) == 0 && !wcscmp(r.how, L"Explorer too slow to answer"),
          "every root timing out: no further root after kLookupBudget, Failed and slow (not 'no button')");
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::R(fake::kIsland).findHr = static_cast<HRESULT>(UIA_E_TIMEOUT); }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Failed && r.slow && r.hr == static_cast<HRESULT>(UIA_E_TIMEOUT) && Finds(fake::kMain) == 1,
          "the island timed out, the taskbar's own window has no buttons: the taskbar could not be looked into (slow), not 'no button'");
    fake::Reset();
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::R(fake::kIsland).findHr = E_FAIL; fake::R(fake::kMain).buttons.push_back(Btn(kChrome, L"Chrome - 1 running window")); }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Pressed && Presses(0, fake::kMain) == 1, "the island could not be looked into, the taskbar's window has the buttons: pressed there");
    fake::Reset();
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::R(fake::kIsland).findHr = E_FAIL; }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Failed && !r.slow && r.hr == E_FAIL, "...no root could be looked into, not for slowness: Failed, not slow");

    // ---- no taskbar, the classic one, no UI Automation
    fake::Reset();
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::mainBar = nullptr; }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::NoTaskbar, "Explorer's taskbar not there (restarting): NoTaskbar");
    fake::Reset();
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::classic = true; }
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Classic && Finds(fake::kIsland) == 0, "Windows 10's classic taskbar: Classic, nothing looked up");
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    {
        std::lock_guard<std::mutex> call(S().calls);
        S().uia.Reset();
    }
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::create8 = REGDB_E_CLASSNOTREG; fake::creates = 0; }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Pressed && fake::creates == 2, "no CUIAutomation8 (Windows 7's UI Automation only): the older one");
    {
        std::lock_guard<std::mutex> call(S().calls);
        S().uia.Reset();
    }
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::create8 = fake::create7 = REGDB_E_CLASSNOTREG; }
    r = Press(J(L"Chrome"));
    Check(r.outcome == Outcome::Failed && !wcscmp(r.how, L"no UI Automation") && Presses(0) == 1, "no UI Automation at all: Failed, nothing pressed");
    { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::create8 = fake::create7 = S_OK; }

    // ---- through Begin and Take, as the dock does
    fake::Reset();
    Island(Btn(kChrome, L"Chrome - 1 running window"));
    {
        Job j = J(L"Chrome");
        j.wake = (HWND)(UINT_PTR)0x7D001;
        Check(Begin(j), "a job begun on a thread of its own");
        Result got;
        Check(Wait(j.id, got, 3000) && got.outcome == Outcome::Pressed && got.job == j.id, "...its result comes through Take");
        Check(!Take(j.id, got), "...once");
        Check(std::count(fake::posted.begin(), fake::posted.end(), j.wake) == 1, "...and the dock's window is woken for it (once)");
        Check(!Take(424242, got), "no result for a job never begun");
    }
    {
        Job j = J(L"Chrome");
        Abandon(*j.stage);
        Check(Begin(j), "a job the dock gave up on begun");
        Result got;
        Check(!Wait(j.id, got, 300) && Presses(0) == 1, "...gives no result and presses nothing");
    }
    {   // a click while the worker is still busy with an earlier one: one lookup at a time; given up on meanwhile, it does nothing
        HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::gate = gate; }
        fake::maxInside = 0;
        Job first = J(L"Chrome"), second = J(L"Chrome");
        Begin(first);
        Sleep(50);
        Begin(second);
        Sleep(50);
        Check(Abandon(*second.stage), "a second job waiting for the first one's call into a busy Explorer: the dock can still give it up");
        SetEvent(gate);
        Result got;
        Check(Wait(first.id, got, 3000) && got.outcome == Outcome::Pressed, "...the first one's press goes through once Explorer answers");
        Sleep(200);
        Check(!Take(second.id, got) && Presses(0) == 2 && Finds(fake::kIsland) == 2, "...the second one, given up on, looks up nothing more and presses nothing");
        Check(fake::maxInside == 1, "calls into UI Automation one at a time");
        { std::lock_guard<std::recursive_mutex> lk(fake::mu); fake::gate = nullptr; }
        CloseHandle(gate);
    }
    {   // results no one comes for (the dock gave up on them as they came in) are kept for a while only
        std::vector<uint32_t> ids;
        for (int i = 0; i < 20; ++i) {
            Job j = J(L"Chrome");
            ids.push_back(j.id);
            Begin(j);
            Result got;
            for (int t = 0; t < 3000; t += 5) {   // in, but not taken
                { std::lock_guard<std::mutex> lk(S().book); if (std::any_of(S().done.begin(), S().done.end(), [&](const Result& d) { return d.job == j.id; })) break; }
                Sleep(5);
            }
        }
        size_t kept = 0;
        { std::lock_guard<std::mutex> lk(S().book); kept = S().done.size(); }
        Result got;
        Check(kept == 16 && !Take(ids[0], got) && Take(ids[19], got), "results no one came for: the 16 latest kept, older ones dropped");
        std::lock_guard<std::mutex> lk(S().book);
        S().done.clear();
    }

    // ---- List (the live test's): every app button, the main taskbar first
    fake::Reset();
    fake::AddSecondary();
    Island(Btn(kChrome, L"Chrome 고정됨"));
    Island(Btn(L"StartButton", L"시작"));
    Island(Btn(kChrome, L"Chrome - 1 running window"), fake::kIsland2);
    {
        std::vector<Button> all;
        std::thread([&] { CoInitializeEx(nullptr, COINIT_MULTITHREADED); all = List(); CoUninitialize(); }).join();
        Check(all.size() == 2 && all[0].taskbar == fake::kMain && all[1].taskbar == fake::kSecondBar && all[0].id == L"Chrome",
              "List: the app buttons only (not Start), the main taskbar's first");
    }

    // ---- nothing leaked: every element, pattern, array, condition and string it got is released
    {
        std::lock_guard<std::mutex> call(S().calls);   // the detached workers above are past their calls
        S().uia.Reset();
    }
    for (int t = 0; t < 3000 && (fake::live != 0 || fake::bstrs != 0); t += 10) Sleep(10);   // ...and their last releases
    Check(fake::live == 0, "every UI Automation object it was handed is released (only the one UI Automation is kept, until now)");
    Check(fake::bstrs == 0, "every id and name it was handed is freed");
    return Summary("taskbar_apps_worker_test");
}
