// What UI Automation sees of Explorer's taskbars: for the dock's start button, which presses the taskbar's own
// Start button (src/taskbar_start.cpp), and for its app icons, which could press the taskbar's app buttons
// later. Reads only - nothing is pressed, moved or changed. tests\run_taskbar_probe.bat builds and runs it.
// Run it while the dock runs (the taskbar hidden, as the dock keeps it); once more with the dock closed (the
// taskbar shown) tells what changes when the taskbar hides.
// Prints, for each taskbar: its window (where, shown or hidden, the dock's guard on it), its child windows,
// its automation tree (raw view), from where the Start button can be found and how it could be pressed (its
// patterns), what the dock's own lookup finds, and every button with an "Appid:" (the app buttons).
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\taskbar_probe.cpp /link user32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib shell32.lib
#include "../src/taskbar_start.cpp"
#include <shellapi.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

// ---- what taskbar_start.cpp needs from the rest of the dock
static void Out(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    char utf8[12288] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    fputs(utf8, stdout);
    fputs("\n", stdout);
    fflush(stdout);
}
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    Out(L"  (dock's log line: %ls)", buf);
}
double NowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

namespace {

// IAppVisibility (shobjidl_core.h), as src/start_menu.cpp declares it
const CLSID kAppVisibility = { 0x7e5fe3d9, 0x985f, 0x4908, { 0x91, 0xf9, 0xee, 0x19, 0xf9, 0xfd, 0x15, 0x14 } };
const IID kAppVisibilityIid = { 0x2246ea2d, 0xcaea, 0x4444, { 0xa3, 0xc4, 0x6d, 0xe8, 0x27, 0xe4, 0x43, 0x13 } };
struct IVisibility : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAppVisibilityOnMonitor(HMONITOR, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLauncherVisible(BOOL* visible) = 0;
};

constexpr int kMaxDepth = 12;
constexpr int kMaxNodes = 400;
ComPtr<IUIAutomation> g_probe;   // the probe's own, with longer timeouts than the dock's

std::wstring Take(BSTR b) { std::wstring s = b ? b : L""; SysFreeString(b); return s; }
std::wstring Short(std::wstring s, size_t n = 60) {
    for (auto& c : s) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    if (s.size() > n) s = s.substr(0, n) + L"...";
    return s;
}
std::wstring Class(HWND h) { wchar_t c[128] = L""; GetClassNameW(h, c, ARRAYSIZE(c)); return c; }
std::wstring Rect(const RECT& r) {
    wchar_t b[96];
    _snwprintf_s(b, ARRAYSIZE(b), _TRUNCATE, L"(%ld,%ld)-(%ld,%ld) %ldx%ld", r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
    return b;
}
double Ms(double since) { return (NowSeconds() - since) * 1000; }

const wchar_t* TypeName(CONTROLTYPEID t) {
    static const wchar_t* names[] = { L"button", L"calendar", L"checkbox", L"combobox", L"edit", L"hyperlink", L"image", L"listitem",
                                      L"list", L"menu", L"menubar", L"menuitem", L"progressbar", L"radiobutton", L"scrollbar", L"slider",
                                      L"spinner", L"statusbar", L"tab", L"tabitem", L"text", L"toolbar", L"tooltip", L"tree", L"treeitem",
                                      L"custom", L"group", L"thumb", L"datagrid", L"dataitem", L"document", L"splitbutton", L"window",
                                      L"pane", L"header", L"headeritem", L"table", L"titlebar", L"separator", L"semanticzoom", L"appbar" };
    const int i = t - UIA_ButtonControlTypeId;
    return i >= 0 && i < int(ARRAYSIZE(names)) ? names[i] : L"?";
}

bool BoolProp(IUIAutomationElement* e, PROPERTYID id) {
    VARIANT v; VariantInit(&v);
    bool r = SUCCEEDED(e->GetCurrentPropertyValue(id, &v)) && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
    VariantClear(&v);
    return r;
}
int IntProp(IUIAutomationElement* e, PROPERTYID id, int otherwise) {
    VARIANT v; VariantInit(&v);
    int r = SUCCEEDED(e->GetCurrentPropertyValue(id, &v)) && v.vt == VT_I4 ? v.lVal : otherwise;
    VariantClear(&v);
    return r;
}
std::wstring TextProp(IUIAutomationElement* e, PROPERTYID id) {
    VARIANT v; VariantInit(&v);
    std::wstring r = SUCCEEDED(e->GetCurrentPropertyValue(id, &v)) && v.vt == VT_BSTR && v.bstrVal ? v.bstrVal : L"";
    VariantClear(&v);
    return r;
}

// How it could be pressed: the patterns that matter for a button, with their state.
std::wstring Patterns(IUIAutomationElement* e) {
    std::wstring p;
    if (BoolProp(e, UIA_IsInvokePatternAvailablePropertyId)) p += L" Invoke";
    if (BoolProp(e, UIA_IsTogglePatternAvailablePropertyId)) {
        const int s = IntProp(e, UIA_ToggleToggleStatePropertyId, -1);
        p += s == 0 ? L" Toggle(off)" : s == 1 ? L" Toggle(on)" : L" Toggle(?)";
    }
    if (BoolProp(e, UIA_IsExpandCollapsePatternAvailablePropertyId)) {
        const int s = IntProp(e, UIA_ExpandCollapseExpandCollapseStatePropertyId, -1);
        p += s == 0 ? L" ExpandCollapse(collapsed)" : s == 1 ? L" ExpandCollapse(expanded)" : L" ExpandCollapse";
    }
    if (BoolProp(e, UIA_IsSelectionItemPatternAvailablePropertyId)) p += L" SelectionItem";
    if (BoolProp(e, UIA_IsLegacyIAccessiblePatternAvailablePropertyId)) {
        const std::wstring a = TextProp(e, UIA_LegacyIAccessibleDefaultActionPropertyId);
        p += L" LegacyIAccessible(default action \"" + Short(a, 30) + L"\")";
    }
    return p.empty() ? L" none" : p;
}

// One line: what it is, its ids, where, and how it could be pressed.
std::wstring Describe(IUIAutomationElement* e) {
    CONTROLTYPEID type = 0;
    e->get_CurrentControlType(&type);
    BSTR name = nullptr, cls = nullptr, id = nullptr, fw = nullptr;
    e->get_CurrentName(&name); e->get_CurrentClassName(&cls); e->get_CurrentAutomationId(&id); e->get_CurrentFrameworkId(&fw);
    UIA_HWND hwnd = nullptr;
    e->get_CurrentNativeWindowHandle(&hwnd);
    RECT r{};
    e->get_CurrentBoundingRectangle(&r);
    BOOL off = FALSE, enabled = FALSE;
    e->get_CurrentIsOffscreen(&off);
    e->get_CurrentIsEnabled(&enabled);
    const std::wstring sid = Take(id);
    wchar_t window[32] = L"";
    if (hwnd) _snwprintf_s(window, ARRAYSIZE(window), _TRUNCATE, L" hwnd=%p", static_cast<void*>(hwnd));
    wchar_t head[640];
    _snwprintf_s(head, ARRAYSIZE(head), _TRUNCATE, L"[%ls] \"%ls\" class=%ls id=%ls fw=%ls%ls rect=%ls%ls%ls patterns:",
                 TypeName(type), Short(Take(name)).c_str(), Short(Take(cls), 50).c_str(), Short(sid, 70).c_str(), Take(fw).c_str(),
                 window, Rect(r).c_str(), off ? L" offscreen" : L"", enabled ? L"" : L" disabled");
    std::wstring line = head + Patterns(e);
    if (sid == L"StartButton") line += L"   <== the Start button";
    else if (sid.rfind(L"Appid:", 0) == 0) line += L"   <== an app button";
    return line;
}

void Tree(IUIAutomationTreeWalker* walker, IUIAutomationElement* e, int depth, int& nodes) {
    if (nodes >= kMaxNodes) return;
    ++nodes;
    Out(L"    %*ls%ls", depth * 2, L"", Describe(e).c_str());
    if (depth >= kMaxDepth) return;
    ComPtr<IUIAutomationElement> child;
    walker->GetFirstChildElement(e, &child);
    while (child && nodes < kMaxNodes) {
        Tree(walker, child.Get(), depth + 1, nodes);
        ComPtr<IUIAutomationElement> next;
        walker->GetNextSiblingElement(child.Get(), &next);
        child = next;
    }
}

ComPtr<IUIAutomationCondition> Is(PROPERTYID id, const wchar_t* text) {
    ComPtr<IUIAutomationCondition> c;
    VARIANT v; VariantInit(&v); v.vt = VT_BSTR; v.bstrVal = SysAllocString(text);
    g_probe->CreatePropertyCondition(id, v, &c);
    VariantClear(&v);
    return c;
}

// The Start button under this window's element, and how long the look took. patterns: its patterns, if found.
bool LookFrom(HWND w, const wchar_t* what, std::wstring* patterns) {
    ComPtr<IUIAutomationElement> root, found;
    const double t = NowSeconds();
    HRESULT hr = g_probe->ElementFromHandle(w, &root);
    if (SUCCEEDED(hr) && root) hr = root->FindFirst(TreeScope_Descendants, Is(UIA_AutomationIdPropertyId, L"StartButton").Get(), &found);
    if (found) Out(L"    from %ls: found in %.0f ms - %ls", what, Ms(t), Describe(found.Get()).c_str());
    else Out(L"    from %ls: not found (%08lx) in %.0f ms", what, (unsigned long)hr, Ms(t));
    if (found && patterns && patterns->empty()) *patterns = Patterns(found.Get());
    return found != nullptr;
}

BOOL CALLBACK AddChild(HWND w, LPARAM p) { reinterpret_cast<std::vector<HWND>*>(p)->push_back(w); return TRUE; }

struct Facts {
    HWND window;
    bool main;
    bool hidden = false;          // the dock's guard: invisible and no region
    bool fromTaskbar = false, fromIsland = false;
    std::wstring dock;            // what the dock's own lookup said
    int apps = 0;
    std::wstring appPatterns, startPatterns;
};

// Every button under this root (the taskbar window, or one of its XAML island's windows - hidden while the
// taskbar auto-hides, and then the only way into its XAML content), each once: app buttons with their whole
// id and name, for the dock's app icons to press later.
void Buttons(HWND w, const wchar_t* what, std::vector<ComPtr<IUIAutomationElement>>& seen, Facts& f) {
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationCondition> buttons;
    ComPtr<IUIAutomationElementArray> all;
    VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = UIA_ButtonControlTypeId;
    g_probe->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &buttons);
    const double t = NowSeconds();
    int count = 0, fresh = 0;
    if (buttons && SUCCEEDED(g_probe->ElementFromHandle(w, &root)) && root && SUCCEEDED(root->FindAll(TreeScope_Descendants, buttons.Get(), &all)) && all)
        all->get_Length(&count);
    Out(L"    under %ls: %d button%ls (%.0f ms)", what, count, count == 1 ? L"" : L"s", Ms(t));
    for (int i = 0; i < count && i < 80; ++i) {
        ComPtr<IUIAutomationElement> e;
        if (FAILED(all->GetElement(i, &e)) || !e) continue;
        bool known = false;
        for (auto& k : seen) {
            BOOL same = FALSE;
            if (SUCCEEDED(g_probe->CompareElements(k.Get(), e.Get(), &same)) && same) { known = true; break; }
        }
        if (known) continue;
        seen.push_back(e);
        ++fresh;
        BSTR id = nullptr, name = nullptr;
        e->get_CurrentAutomationId(&id);
        e->get_CurrentName(&name);
        const std::wstring sid = Take(id), sname = Take(name);
        Out(L"      %ls", Describe(e.Get()).c_str());
        if (sid.rfind(L"Appid:", 0) == 0) {
            ++f.apps;
            if (f.appPatterns.empty()) f.appPatterns = Patterns(e.Get());
            Out(L"        id: %ls", Short(sid, 240).c_str());
            Out(L"        name: %ls; item status: \"%ls\"; help: \"%ls\"", Short(sname, 160).c_str(),
                Short(TextProp(e.Get(), UIA_ItemStatusPropertyId), 80).c_str(), Short(TextProp(e.Get(), UIA_HelpTextPropertyId), 80).c_str());
        }
        if (sid == L"StartButton" && f.startPatterns.empty()) f.startPatterns = Patterns(e.Get());
    }
    if (count && !fresh) Out(L"      (all of them listed above already)");
}

void PrintTaskbar(int n, Facts& f) {
    HWND h = f.window;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    RECT wr{};
    GetWindowRect(h, &wr);
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    const int region = GetWindowRgn(h, rgn);
    DeleteObject(rgn);
    COLORREF key = 0; BYTE alpha = 255; DWORD flags = 0;
    const bool layered = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) && GetLayeredWindowAttributes(h, &key, &alpha, &flags);
    const bool guard = GetPropW(h, L"LiquidDock.OpacityBackup.v1") != nullptr;
    f.hidden = region == NULLREGION || (layered && (flags & LWA_ALPHA) && alpha == 0);
    HMONITOR mon = f.main ? MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY) : MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    Out(L"");
    Out(L"== taskbar %d: %ls %p (%ls), Explorer pid %lu", n, Class(h).c_str(), static_cast<void*>(h), f.main ? L"the main one" : L"secondary", pid);
    Out(L"  window %ls, %ls; region %ls; %ls%ls", Rect(wr).c_str(), IsWindowVisible(h) ? L"visible" : L"not visible",
        region == NULLREGION ? L"empty (draws nothing, takes no clicks)" : region == ERROR ? L"none (the whole window)" : L"set",
        layered ? (L"layered, alpha " + std::to_wstring(alpha)).c_str() : L"not layered", guard ? L"; the dock's opacity guard is on it" : L"");
    Out(L"  monitor %ls%ls; the dock's start button on that monitor presses %ls", Rect(mi.rcMonitor).c_str(), (mi.dwFlags & MONITORINFOF_PRIMARY) ? L" (primary)" : L"",
        taskbarstart::TaskbarOn(mon) == h ? L"this taskbar's Start button" : L"another taskbar's (or none)");

    Out(L"  child windows:");
    std::vector<HWND> kids;
    EnumChildWindows(h, AddChild, reinterpret_cast<LPARAM>(&kids));
    for (size_t i = 0; i < kids.size() && i < 40; ++i) {
        int depth = 0;
        for (HWND p = GetParent(kids[i]); p && p != h && depth < 8; p = GetParent(p)) ++depth;
        RECT cr{};
        GetWindowRect(kids[i], &cr);
        wchar_t text[64] = L"";
        GetWindowTextW(kids[i], text, ARRAYSIZE(text));
        Out(L"    %*ls%ls %p %ls %ls \"%ls\"", depth * 2, L"", Class(kids[i]).c_str(), static_cast<void*>(kids[i]),
            IsWindowVisible(kids[i]) ? L"visible" : L"hidden", Rect(cr).c_str(), Short(text, 40).c_str());
    }

    // the trees: from the taskbar window, and from its XAML island's window (which the first does not reach
    // into while the taskbar is hidden)
    std::vector<HWND> roots = { h };
    for (HWND k : kids)
        if (Class(k) == L"Windows.UI.Composition.DesktopWindowContentBridge") roots.push_back(k);
    for (HWND r : roots) {
        Out(L"  automation tree (raw view, from %ls %p; depth <= %d, at most %d elements):", r == h ? L"the taskbar window" : L"its XAML island",
            static_cast<void*>(r), kMaxDepth, kMaxNodes);
        ComPtr<IUIAutomationTreeWalker> walker;
        ComPtr<IUIAutomationElement> root;
        const double t = NowSeconds();
        int nodes = 0;
        if (SUCCEEDED(g_probe->get_RawViewWalker(&walker)) && walker && SUCCEEDED(g_probe->ElementFromHandle(r, &root)) && root) Tree(walker.Get(), root.Get(), 0, nodes);
        else Out(L"    (no element for it)");
        Out(L"    (%d element%ls in %.0f ms%ls)", nodes, nodes == 1 ? L"" : L"s", Ms(t), nodes >= kMaxNodes ? L"; more not shown" : L"");
    }

    Out(L"  the Start button (AutomationId \"StartButton\"), looked for as the dock does:");
    f.fromTaskbar = LookFrom(h, L"the taskbar window", &f.startPatterns);
    for (HWND k : kids) {
        const std::wstring c = Class(k);
        if (c != L"Windows.UI.Composition.DesktopWindowContentBridge" && c != L"Windows.UI.Input.InputSite.WindowClass") continue;
        wchar_t what[160];
        _snwprintf_s(what, ARRAYSIZE(what), _TRUNCATE, L"its %ls %p (%ls)", c.c_str(), static_cast<void*>(k), IsWindowVisible(k) ? L"visible" : L"hidden");
        f.fromIsland |= LookFrom(k, what, &f.startPatterns);
    }
    if (HWND classic = FindWindowExW(h, nullptr, L"Start", nullptr)) {
        ComPtr<IUIAutomationElement> e;
        Out(L"    the classic Start window %p (%ls): %ls", static_cast<void*>(classic), IsWindowVisible(classic) ? L"visible" : L"hidden",
            SUCCEEDED(g_probe->ElementFromHandle(classic, &e)) && e ? Describe(e.Get()).c_str() : L"no element");
    }
    {
        const double t = NowSeconds();
        const taskbarstart::Found found = taskbarstart::TaskbarOn(mon) == h ? taskbarstart::Find(mon) : taskbarstart::Found::NoTaskbar;
        f.dock = taskbarstart::FoundText(found);
        Out(L"    the dock's own lookup (src/taskbar_start.cpp): %ls, in %.0f ms (%ls)", f.dock.c_str(), Ms(t), taskbarstart::Describe().c_str());
    }

    Out(L"  buttons (app buttons have an \"Appid:\" id), each listed once:");
    {
        std::vector<ComPtr<IUIAutomationElement>> seen;
        Buttons(h, L"the taskbar window", seen, f);
        for (HWND r : roots)
            if (r != h) Buttons(r, L"its XAML island", seen, f);
    }
}

void PrintSystem() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = { sizeof(v) };
    if (auto f = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) f(&v);
    wchar_t display[32] = L"?";
    DWORD size = sizeof(display), ubr = 0, usize = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion", RRF_RT_REG_SZ, nullptr, display, &size);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &usize);
    bool elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{}; DWORD got = 0;
        elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got) && e.TokenIsElevated;
        CloseHandle(token);
    }
    Out(L"  Windows %ls, build %lu.%lu; the probe runs %ls", display, v.dwBuildNumber, ubr, elevated ? L"as administrator" : L"as the user (as the dock does)");
    Out(L"  Liquid Dock: %ls", FindWindowW(L"LiquidDockMain", nullptr) ? L"running" : L"not running (the taskbar should be shown as usual)");
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = ExplorerTaskbar();
    if (abd.hWnd) Out(L"  taskbar auto-hide: %ls (the dock turns it on while it runs)", (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) ? L"on" : L"off");
    Out(L"  taskbar kind: %ls", taskbarstart::Classic() ? L"classic (Windows 10 style: no XAML Start button; the dock asks the shell)" : L"Windows 11's XAML taskbar");
    ComPtr<IVisibility> vis;
    BOOL open = FALSE;
    if (SUCCEEDED(CoCreateInstance(kAppVisibility, nullptr, CLSCTX_ALL, kAppVisibilityIid, reinterpret_cast<void**>(vis.GetAddressOf()))) && vis &&
        SUCCEEDED(vis->IsLauncherVisible(&open)))
        Out(L"  Start: %ls", open ? L"open" : L"closed");
}

}   // namespace

int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // a thread of the multithreaded apartment that owns no windows, as Microsoft advises UI Automation clients
    // (and as the dock's start button worker does)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Out(L"taskbar_probe: what UI Automation sees of Explorer's taskbars (reads only: nothing is pressed or changed)");
    PrintSystem();
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_probe))) &&
        FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_probe)))) {
        Out(L"taskbar_probe: no UI Automation here");
        return 2;
    }
    ComPtr<IUIAutomation2> u2;
    if (SUCCEEDED(g_probe.As(&u2))) { u2->put_ConnectionTimeout(2000); u2->put_TransactionTimeout(2000); }

    std::vector<Facts> bars;
    if (HWND primary = ExplorerTaskbar()) bars.push_back({ primary, true });
    HWND s = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr);
    for (int guard = 0; s && guard < 16; ++guard, s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr)) bars.push_back({ s, false });
    if (bars.empty()) Out(L"  no Explorer taskbar (Explorer not running, or another shell)");
    for (size_t i = 0; i < bars.size(); ++i) PrintTaskbar(int(i) + 1, bars[i]);

    Out(L"");
    Out(L"summary:");
    for (size_t i = 0; i < bars.size(); ++i) {
        const Facts& f = bars[i];
        Out(L"  taskbar %d (%ls, %ls): Start button from the taskbar window: %ls; from its XAML island: %ls; patterns:%ls; the dock's lookup: %ls; %d app button%ls (patterns:%ls)",
            int(i) + 1, f.main ? L"main" : L"secondary", f.hidden ? L"hidden by the dock" : L"shown", f.fromTaskbar ? L"yes" : L"no",
            f.fromIsland ? L"yes" : L"no", f.startPatterns.empty() ? L" -" : f.startPatterns.c_str(), f.dock.c_str(), f.apps, f.apps == 1 ? L"" : L"s",
            f.appPatterns.empty() ? L" -" : f.appPatterns.c_str());
    }
    Out(L"taskbar_probe: done");
    CoUninitialize();
    return 0;
}
