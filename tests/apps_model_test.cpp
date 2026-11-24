// The app model (apps*.cpp: windows, grouping, pins, clicks, restores, launches, closing) with a fake window
// system: a table of fake windows and processes stands in for every call apps*.cpp makes about other programs.
// Nothing real is touched - no other program's window is looked at, raised, minimized or closed, nothing is
// started (ShellExecuteEx is a fake), no registry value is read or written (the taskbar's pin list is a fake
// blob), the elevated helper and the taskbar's buttons are stubs. The fake windows' handles are message-only
// windows of this test's own, so a handle never names a real window. Pins and settings go to the data folder
// next to this exe (common.cpp AppDataDir), the fake "Roaming" folder next to it too.
// Build: see this test's line in tests\run_regressions.bat; run from the repository folder.
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/admin_helper.h"
#include "../src/admin_client.h"
#include "../src/app_click.h"
#include "../src/system.h"
#include "../src/taskbar_apps.h"
#include "../src/explorer_taskbar.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <set>
#include <cstdio>
#include <cstdarg>
// APPS_MODEL_FIXED: a copy of apps.h, apps_internal.h and apps*.cpp with a fix being tried, next to a file that defines
// it and includes this one (built with /I src for the rest)
#ifdef APPS_MODEL_FIXED
#define APPS_MODEL_H "apps.h"
#define APPS_MODEL_CPP "apps.cpp"
#define APPS_MODEL_PINS_CPP "apps_pins.cpp"
#define APPS_MODEL_CLICK_CPP "apps_click.cpp"
#define APPS_MODEL_PRESS_CPP "apps_press.cpp"
#else
#define APPS_MODEL_H "../src/apps.h"
#define APPS_MODEL_CPP "../src/apps.cpp"
#define APPS_MODEL_PINS_CPP "../src/apps_pins.cpp"
#define APPS_MODEL_CLICK_CPP "../src/apps_click.cpp"
#define APPS_MODEL_PRESS_CPP "../src/apps_press.cpp"
#endif
#define private public
#include APPS_MODEL_H
#undef private
#include "portable_check.h"

Settings g_settings;
HWND g_mainWnd = nullptr;
Gfx g_gfx;   // never used: apps*.cpp's bitmaps go to iconrec (below) instead, made on no GPU

// ---------------------------------------------------------------- the fake window system
struct FakeWin {
    DWORD pid = 0, tid = 0;
    std::wstring title = L"window", cls = L"FakeAppWindow", aumid;   // aumid: the window's own (its property store)
    bool alive = true, visible = true, cloaked = false, iconic = false, zoomed = false;
    LONG_PTR exStyle = 0, style = WS_OVERLAPPEDWINDOW;
    HWND owner = nullptr;
    RECT rect{ 100, 100, 900, 700 };
    std::vector<HWND> children;
    // its program
    bool busy = false;               // answers no sent message until released
    bool restoresMaximized = false;  // minimized from maximized: its restore brings it back maximized
    bool leavesDown = false;         // handles SC_RESTORE and leaves the window minimized
    int restoreSends = 0, restoresHandled = 0, systemRestores = 0;
};
struct FakeProc { std::wstring exe, aumid; bool elevated = false; };   // elevated: runs as administrator (FakeElevated)
static std::mutex mu;                   // the table: the dock's restore workers read and change it too
static std::condition_variable woke;    // a busy program released
static std::map<HWND, FakeWin> wins;
static std::vector<HWND> zorder;        // EnumWindows' order, topmost first
static std::map<DWORD, FakeProc> procs;
static HWND foreground = nullptr;
static bool takesFront = false;         // SetForegroundWindow works (else Windows refuses the dock, as it may)
static std::atomic<int> inFlight{ 0 };  // restore commands being sent (worker threads)
struct Posted { HWND h; UINT msg; WPARAM wp; };
static std::vector<Posted> posted;
static std::vector<HWND> frontCalls;    // SetForegroundWindow
static std::vector<HWND> frontAtCalls;  // ...the window in front at each of those calls
static std::vector<std::pair<std::wstring, std::wstring>> started;   // ShellExecuteEx: file, verb
static std::vector<BYTE> taskband;      // the taskbar's pin list (Favorites); empty: no value
static bool haveTaskband = false;
static std::wstring roaming;            // FOLDERID_RoamingAppData

static FakeWin* Find(HWND h) { auto it = wins.find(h); return it == wins.end() ? nullptr : &it->second; }
static HANDLE ProcHandle(DWORD pid) { return (HANDLE)(uintptr_t)(0xFA0000000000ull | pid); }
static bool IsProcHandle(HANDLE h) { return ((uintptr_t)h >> 40) == 0xFA; }

static BOOL WINAPI FakeEnumWindows(WNDENUMPROC proc, LPARAM lp) {
    std::vector<HWND> order;
    { std::lock_guard<std::mutex> lk(mu); for (HWND h : zorder) if (Find(h) && Find(h)->alive) order.push_back(h); }
    for (HWND h : order) if (!proc(h, lp)) break;
    return TRUE;
}
static BOOL WINAPI FakeEnumChildWindows(HWND parent, WNDENUMPROC proc, LPARAM lp) {
    std::vector<HWND> kids;
    { std::lock_guard<std::mutex> lk(mu); if (auto* w = Find(parent)) kids = w->children; }   // never a real window's children
    for (HWND h : kids) if (!proc(h, lp)) break;
    return TRUE;
}
static DWORD WINAPI FakeThreadProcessId(HWND h, LPDWORD pid) {
    { std::lock_guard<std::mutex> lk(mu); if (auto* w = Find(h)) { if (pid) *pid = w->alive ? w->pid : 0; return w->alive ? w->tid : 0; } }
    return ::GetWindowThreadProcessId(h, pid);
}
#define FAKE_BOOL(name, expr) static BOOL WINAPI name(HWND h) { std::lock_guard<std::mutex> lk(mu); auto* w = Find(h); return w ? (expr) : FALSE; }
FAKE_BOOL(FakeIsWindow, w->alive)
FAKE_BOOL(FakeIsWindowVisible, w->alive && w->visible)
FAKE_BOOL(FakeIsIconic, w->alive && w->iconic)
FAKE_BOOL(FakeIsZoomed, w->alive && w->zoomed)
FAKE_BOOL(FakeIsWindowEnabled, TRUE)
FAKE_BOOL(FakeIsHungAppWindow, FALSE)
static LONG_PTR WINAPI FakeWindowLong(HWND h, int index) {
    std::lock_guard<std::mutex> lk(mu);
    auto* w = Find(h);
    if (!w) return 0;
    return index == GWL_EXSTYLE ? w->exStyle : index == GWL_STYLE ? w->style : 0;
}
static HWND WINAPI FakeGetWindow(HWND h, UINT cmd) { std::lock_guard<std::mutex> lk(mu); auto* w = Find(h); return w && cmd == GW_OWNER ? w->owner : nullptr; }
static HWND WINAPI FakeAncestor(HWND h, UINT flags) {
    std::lock_guard<std::mutex> lk(mu);
    if (flags != GA_ROOTOWNER) return h;
    for (int guard = 0; guard < 8; ++guard) { auto* w = Find(h); if (!w || !w->owner) break; h = w->owner; }
    return h;
}
static int WINAPI FakeClassName(HWND h, LPWSTR out, int n) {
    std::lock_guard<std::mutex> lk(mu);
    auto* w = Find(h);
    wcsncpy_s(out, n, w ? w->cls.c_str() : L"", _TRUNCATE);
    return (int)wcslen(out);
}
static int WINAPI FakeTextLength(HWND h) { std::lock_guard<std::mutex> lk(mu); auto* w = Find(h); return w ? (int)w->title.size() : 0; }
static int WINAPI FakeText(HWND h, LPWSTR out, int n) {
    std::lock_guard<std::mutex> lk(mu);
    auto* w = Find(h);
    wcsncpy_s(out, n, w ? w->title.c_str() : L"", _TRUNCATE);
    return (int)wcslen(out);
}
static BOOL WINAPI FakeWindowRect(HWND h, LPRECT r) { std::lock_guard<std::mutex> lk(mu); auto* w = Find(h); if (!w) return FALSE; *r = w->rect; return TRUE; }
static BOOL WINAPI FakePlacement(HWND h, WINDOWPLACEMENT* p) { std::lock_guard<std::mutex> lk(mu); auto* w = Find(h); if (!w) return FALSE; p->rcNormalPosition = w->rect; return TRUE; }
static HRESULT WINAPI FakeDwmAttribute(HWND h, DWORD attr, PVOID value, DWORD size) {
    std::lock_guard<std::mutex> lk(mu);
    auto* w = Find(h);
    if (!w || attr != DWMWA_CLOAKED || size < sizeof(DWORD)) return E_INVALIDARG;
    *(DWORD*)value = w->cloaked ? 1 : 0;
    return S_OK;
}
static ULONG_PTR WINAPI FakeClassLong(HWND, int) { return 0; }
static HWND WINAPI FakeForeground() { std::lock_guard<std::mutex> lk(mu); return foreground; }
static BOOL WINAPI FakeSetForeground(HWND h) {
    std::lock_guard<std::mutex> lk(mu);
    frontCalls.push_back(h);
    frontAtCalls.push_back(foreground);
    if (!takesFront) return FALSE;
    foreground = h;
    return TRUE;
}
static BOOL WINAPI FakeAttach(DWORD, DWORD, BOOL) { return TRUE; }
static BOOL WINAPI FakeSetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }
static int allowFrontCalls = 0;         // AllowSetForegroundWindow, counted (Push hands its program the right at the click only)
static BOOL WINAPI FakeAllowFront(DWORD) { ++allowFrontCalls; return TRUE; }
static BOOL WINAPI FakePost(HWND h, UINT msg, WPARAM wp, LPARAM) {
    std::lock_guard<std::mutex> lk(mu);
    posted.push_back({ h, msg, wp });
    if (auto* w = Find(h)) if (msg == WM_SYSCOMMAND && wp == SC_MINIMIZE) w->iconic = true;
    return TRUE;
}
// The program's restore: back to how it was before it went down; on a window that is up and maximized, a
// restore makes it normal size (why the dock never sends two: app_click.h).
static void Restore(FakeWin& w) {
    if (w.iconic) { w.iconic = false; w.zoomed = w.restoresMaximized; }
    else if (w.zoomed) w.zoomed = false;
}
static std::vector<WPARAM> getIconAsked;   // WM_GETICON's wParam, as asked of a fake window
static HICON windowIcon = nullptr;          // ...and the icon a fake window answers it with (none unless a check gives one)
static LRESULT WINAPI FakeSendTimeout(HWND h, UINT msg, WPARAM wp, LPARAM, UINT, UINT timeout, PDWORD_PTR result) {
    if (result) *result = 0;
    std::unique_lock<std::mutex> lk(mu);
    auto* w = Find(h);
    if (!w || !w->alive) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    if (msg == WM_GETICON) { getIconAsked.push_back(wp); if (result) *result = (DWORD_PTR)windowIcon; return 1; }
    if (msg != WM_SYSCOMMAND || wp != SC_RESTORE) return 1;   // WM_GETICON: no icon; WM_NULL: answers
    ++w->restoreSends;
    ++inFlight;
    const bool answered = woke.wait_for(lk, std::chrono::milliseconds(timeout), [&] { return !Find(h)->busy; });
    --inFlight;
    if (!answered) { SetLastError(ERROR_TIMEOUT); return 0; }   // not delivered (Windows takes it back)
    ++w->restoresHandled;
    if (!w->leavesDown) Restore(*w);
    return 1;
}
static BOOL WINAPI FakeShowAsync(HWND h, int cmd) {
    std::lock_guard<std::mutex> lk(mu);
    auto* w = Find(h);
    if (!w) return FALSE;
    if (cmd == SW_RESTORE) { ++w->systemRestores; Restore(*w); }
    return TRUE;
}
static std::map<DWORD, int> opened;     // OpenProcess of a fake process
static HANDLE WINAPI FakeOpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    { std::lock_guard<std::mutex> lk(mu); if (procs.count(pid)) { ++opened[pid]; return ProcHandle(pid); } }
    return ::OpenProcess(access, inherit, pid);
}
static BOOL WINAPI FakeImageName(HANDLE p, DWORD flags, LPWSTR out, PDWORD n) {
    if (!IsProcHandle(p)) return ::QueryFullProcessImageNameW(p, flags, out, n);
    std::lock_guard<std::mutex> lk(mu);
    const std::wstring& exe = procs[(DWORD)((uintptr_t)p & 0xffffffff)].exe;
    if (exe.empty() || exe.size() + 1 > *n) return FALSE;
    wcscpy_s(out, *n, exe.c_str());
    *n = (DWORD)exe.size();
    return TRUE;
}
static LONG WINAPI FakeProcessAumid(HANDLE p, UINT32* n, PWSTR out) {
    if (!IsProcHandle(p)) return ::GetApplicationUserModelId(p, n, out);
    std::lock_guard<std::mutex> lk(mu);
    const std::wstring& id = procs[(DWORD)((uintptr_t)p & 0xffffffff)].aumid;
    if (id.empty()) return APPMODEL_ERROR_NO_APPLICATION;
    wcscpy_s(out, *n, id.c_str());
    return ERROR_SUCCESS;
}
static BOOL WINAPI FakeCloseHandle(HANDLE h) { return IsProcHandle(h) ? TRUE : ::CloseHandle(h); }
// A window's own AppUserModelID, as its property store gives it.
struct FakeStore : IPropertyStore {
    LONG refs = 1;
    std::wstring aumid;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IPropertyStore) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* n) override { *n = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD, PROPERTYKEY* k) override { *k = PKEY_AppUserModel_ID; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override {
        PropVariantInit(pv);
        return IsEqualPropertyKey(key, PKEY_AppUserModel_ID) ? InitPropVariantFromString(aumid.c_str(), pv) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit() override { return E_NOTIMPL; }
};
static HRESULT WINAPI FakeWindowStore(HWND h, REFIID riid, void** ppv) {
    *ppv = nullptr;
    std::wstring id;
    { std::lock_guard<std::mutex> lk(mu); auto* w = Find(h); if (!w || w->aumid.empty()) return E_FAIL; id = w->aumid; }
    auto* s = new FakeStore;
    s->aumid = id;
    HRESULT hr = s->QueryInterface(riid, ppv);
    s->Release();
    return hr;
}
static BOOL WINAPI FakeShellExecute(SHELLEXECUTEINFOW* sei) {
    started.push_back({ sei->lpFile ? sei->lpFile : L"", sei->lpVerb ? sei->lpVerb : L"" });
    return TRUE;
}
// No icon from a file, unless a check hands one (IconPixelTests: a copy of extractGives, as Windows gives a new one)
static HICON extractGives = nullptr;
struct ExtractAsk { std::wstring file; int index, cx, cy; UINT count; };
static std::vector<ExtractAsk> extractAsked;
static std::set<std::wstring> extractRefuses;   // files that give no icon (asked all the same), while extractGives is set
static UINT WINAPI FakeExtractIcons(LPCWSTR file, int index, int cx, int cy, HICON* out, UINT* id, UINT n, UINT) {
    if (!extractGives) return 0;
    extractAsked.push_back({ file ? file : L"", index, cx, cy, n });
    if (extractRefuses.count(file ? file : L"")) return 0;
    *out = CopyIcon(extractGives);
    if (id) *id = 1;
    return *out ? 1 : 0;
}
// The D2D bitmaps apps.cpp makes of icons (BitmapFromHBITMAP, IconFromHICON): kept here instead of made on a GPU -
// size, row pitch, pixel format and the pixels as handed over (read row by row at the pitch given)
namespace iconrec {
// A bitmap handed back (giveBack): an object of this test's own with the interface only - nothing on a GPU, every call
// a no-op. It lets a check see which bitmap made a function returns.
struct FakeBitmap : ID2D1Bitmap1 {
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(ID2D1Resource) || riid == __uuidof(ID2D1Image) || riid == __uuidof(ID2D1Bitmap) ||
            riid == __uuidof(ID2D1Bitmap1)) { *ppv = static_cast<ID2D1Bitmap1*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
    void STDMETHODCALLTYPE GetFactory(ID2D1Factory** f) const override { *f = nullptr; }
    D2D1_SIZE_F STDMETHODCALLTYPE GetSize() const override { return { 0, 0 }; }
    D2D1_SIZE_U STDMETHODCALLTYPE GetPixelSize() const override { return { 0, 0 }; }
    D2D1_PIXEL_FORMAT STDMETHODCALLTYPE GetPixelFormat() const override { return {}; }
    void STDMETHODCALLTYPE GetDpi(FLOAT* x, FLOAT* y) const override { *x = *y = 96; }
    HRESULT STDMETHODCALLTYPE CopyFromBitmap(const D2D1_POINT_2U*, ID2D1Bitmap*, const D2D1_RECT_U*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyFromRenderTarget(const D2D1_POINT_2U*, ID2D1RenderTarget*, const D2D1_RECT_U*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CopyFromMemory(const D2D1_RECT_U*, const void*, UINT32) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE GetColorContext(ID2D1ColorContext** c) const override { *c = nullptr; }
    D2D1_BITMAP_OPTIONS STDMETHODCALLTYPE GetOptions() const override { return D2D1_BITMAP_OPTIONS_NONE; }
    HRESULT STDMETHODCALLTYPE GetSurface(IDXGISurface** s) const override { *s = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Map(D2D1_MAP_OPTIONS, D2D1_MAPPED_RECT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Unmap() override { return E_NOTIMPL; }
};
struct Made { UINT32 w = 0, h = 0, pitch = 0; DXGI_FORMAT format{}; D2D1_ALPHA_MODE alpha{}; D2D1_BITMAP_OPTIONS options{}; float dpiX = 0, dpiY = 0; std::vector<uint32_t> px;
              ID2D1Bitmap1* given = nullptr; };   // given: the bitmap handed back (giveBack), else none
static std::vector<Made> made;
static bool giveBack = false;   // hand a FakeBitmap back (else none: the caller's ComPtr stays empty)
struct Dc {
    template <class Out> HRESULT CreateBitmap(D2D1_SIZE_U size, const void* data, UINT32 pitch, const D2D1_BITMAP_PROPERTIES1& bp, Out out) {
        Made m;
        m.w = size.width; m.h = size.height; m.pitch = pitch;
        m.format = bp.pixelFormat.format; m.alpha = bp.pixelFormat.alphaMode; m.options = bp.bitmapOptions; m.dpiX = bp.dpiX; m.dpiY = bp.dpiY;
        for (UINT32 y = 0; y < size.height; ++y)
            for (UINT32 x = 0; x < size.width; ++x) m.px.push_back(((const uint32_t*)((const BYTE*)data + (size_t)y * pitch))[x]);
        if (giveBack) {
            ID2D1Bitmap1** pp = out;
            *pp = m.given = new FakeBitmap;   // the caller's reference
        }
        made.push_back(std::move(m));
        return S_OK;
    }
};
static Dc dc;
struct { Dc* dc = &iconrec::dc; } gfx;
}
// Shell items (the Start menu's entries, a file's item) of this test's own: none unless a check lists some. One has a
// display name, and an image (IShellItemImageFactory) when it is given a bitmap maker; GetImage's asks are kept.
struct ImageAsk { SIZE size; int flags; };
static std::vector<ImageAsk> imageAsked;
struct FakeItem : IShellItem, IShellItemImageFactory {
    LONG refs = 1;
    std::wstring name;
    std::function<HBITMAP()> image;   // empty: no image factory
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IShellItem) { *ppv = static_cast<IShellItem*>(this); AddRef(); return S_OK; }
        if (riid == IID_IShellItemImageFactory && image) { *ppv = static_cast<IShellItemImageFactory*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*, REFGUID, REFIID, void** ppv) override { *ppv = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(IShellItem** p) override { *p = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDisplayName(SIGDN, LPWSTR* out) override {
        *out = (LPWSTR)CoTaskMemAlloc((name.size() + 1) * sizeof(wchar_t));
        wcscpy_s(*out, name.size() + 1, name.c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(SFGAOF, SFGAOF* a) override { *a = 0; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Compare(IShellItem*, SICHINTF, int* o) override { *o = 1; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetImage(SIZE size, SIIGBF flags, HBITMAP* out) override {
        imageAsked.push_back({ size, (int)flags });
        *out = image();
        return *out ? S_OK : E_FAIL;
    }
};
struct ItemSpec { std::wstring name; std::function<HBITMAP()> image; };
static std::map<std::wstring, ItemSpec> appsFolder;    // the Start menu's entries, by AppUserModelID
static std::map<std::wstring, ItemSpec> parsedItems;   // items by parsing name (a path)
static std::vector<std::wstring> knownFolderAsked, parsedAsked;
static HRESULT GiveItem(const std::map<std::wstring, ItemSpec>& from, PCWSTR key, REFIID riid, void** ppv) {
    *ppv = nullptr;
    auto it = key ? from.find(key) : from.end();
    if (it == from.end()) return E_FAIL;
    auto* i = new FakeItem;
    i->name = it->second.name; i->image = it->second.image;
    HRESULT hr = i->QueryInterface(riid, ppv);
    i->Release();
    return hr;
}
static HRESULT WINAPI FakeItemInKnownFolder(REFKNOWNFOLDERID, DWORD, PCWSTR name, REFIID riid, void** ppv) {
    if (name) knownFolderAsked.push_back(name);
    return GiveItem(appsFolder, name, riid, ppv);
}
static HRESULT WINAPI FakeItemFromName(PCWSTR name, IBindCtx*, REFIID riid, void** ppv) {
    if (name) parsedAsked.push_back(name);
    return GiveItem(parsedItems, name, riid, ppv);
}
static LSTATUS WINAPI FakeRegGetValue(HKEY, LPCWSTR key, LPCWSTR value, DWORD, LPDWORD type, PVOID data, LPDWORD size) {
    if (!haveTaskband || !key || !value || !wcsstr(key, L"Taskband") || wcscmp(value, L"Favorites")) return ERROR_FILE_NOT_FOUND;
    if (type) *type = REG_BINARY;
    if (!data) { *size = (DWORD)taskband.size(); return ERROR_SUCCESS; }
    if (*size < taskband.size()) { *size = (DWORD)taskband.size(); return ERROR_MORE_DATA; }
    memcpy(data, taskband.data(), taskband.size());
    *size = (DWORD)taskband.size();
    return ERROR_SUCCESS;
}
static HRESULT WINAPI FakeKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PWSTR* out) {
    if (id != FOLDERID_RoamingAppData) return ::SHGetKnownFolderPath(id, flags, token, out);
    *out = (PWSTR)CoTaskMemAlloc((roaming.size() + 1) * sizeof(wchar_t));
    wcscpy_s(*out, roaming.size() + 1, roaming.c_str());
    return S_OK;
}

// ---------------------------------------------------------------- stubs for the rest of the dock
static int rightTaken = 0;
static ForegroundRight rightGives = ForegroundRight::Had;   // what taking the right answers (the dock's log names it)
ForegroundRight TakeForegroundRight() { ++rightTaken; return rightGives; }
const wchar_t* ForegroundRightText(ForegroundRight r) {
    return r == ForegroundRight::Had ? L"had it" : r == ForegroundRight::Taken ? L"taken" : r == ForegroundRight::Refused ? L"refused" : L"not inserted";
}
namespace adminhelper {
static std::function<bool(HWND)> onActivate, onMinimize, onClose;   // the helper: none (the dock's own calls)
bool Activate(HWND h, const std::wstring&) { return onActivate ? onActivate(h) : false; }
bool Minimize(HWND h, const std::wstring&) { return onMinimize ? onMinimize(h) : false; }
bool Close(HWND h, const std::wstring&) { return onClose ? onClose(h) : false; }
static std::function<bool(HWND)> onActivateOver;   // the helper over an administrator's window in front: none
bool ActivateOver(HWND h, const std::wstring&) { return onActivateOver ? onActivateOver(h) : false; }
// Elevated (admin_client.h) reads the real token of the window's process. It is included above, before the fakes,
// so it asks Windows about a fake window - a message-only window of this test, its process this test's own: never
// elevated. apps_click.cpp's calls come here instead: a fake window is elevated when its fake process is (FakeProc).
static bool FakeElevated(HWND h) {
    {
        std::lock_guard<std::mutex> lk(mu);
        if (auto* w = Find(h)) { auto p = procs.find(w->pid); return w->alive && p != procs.end() && p->second.elevated; }
    }
    return Elevated(h);   // not a fake window (none is asked about): Windows' answer
}
}
static std::function<bool(HWND)> isShellPopup;   // which fake windows are Start or Search: none
bool IsShellPopupWindow(HWND h) { return isShellPopup ? isShellPopup(h) : false; }
static int launcherShown = -1;   // LauncherVisible: Start shown (1), not (0), or the shell not asked (-1)
static int launcherAsked = 0;
static std::function<int()> onLauncher;   // instead of launcherShown, when set
int LauncherVisible() { ++launcherAsked; return onLauncher ? onLauncher() : launcherShown; }
static std::vector<std::vector<WORD>> keysSent;   // SendKeyCombo's keys, in order (nothing reaches Windows)
static std::function<void(const std::vector<WORD>&)> onKeys;
bool SendKeyCombo(std::initializer_list<WORD> keys) {
    keysSent.emplace_back(keys);
    if (onKeys) onKeys(keysSent.back());
    return true;
}
// dock.log's lines from apps*.cpp, kept here too (and still written to the test's dock.log)
static std::mutex logMu;   // a restore worker may log too
static std::vector<std::wstring> logLines;
static void ModelLog(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    { std::lock_guard<std::mutex> lk(logMu); logLines.push_back(buf); }
    Log(L"%ls", buf);
}
static int LogCount(const wchar_t* part) {
    std::lock_guard<std::mutex> lk(logMu);
    int n = 0;
    for (auto& l : logLines) n += l.find(part) != std::wstring::npos;
    return n;
}
namespace fakepress {
static bool beginWorks = false;
static std::vector<taskbarapps::Job> begun;
static std::map<uint32_t, taskbarapps::Result> ready;
}
namespace taskbarapps {
bool Begin(const Job& job) { if (!fakepress::beginWorks) return false; fakepress::begun.push_back(job); return true; }
bool Take(uint32_t id, Result& out) {
    auto it = fakepress::ready.find(id);
    if (it == fakepress::ready.end()) return false;
    out = it->second;
    fakepress::ready.erase(it);
    return true;
}
const wchar_t* OutcomeText(Outcome) { return L"outcome"; }
}

#define EnumWindows FakeEnumWindows
#define EnumChildWindows FakeEnumChildWindows
#define GetWindowThreadProcessId FakeThreadProcessId
#define IsWindow FakeIsWindow
#define IsWindowVisible FakeIsWindowVisible
#define IsIconic FakeIsIconic
#define IsZoomed FakeIsZoomed
#define IsWindowEnabled FakeIsWindowEnabled
#define IsHungAppWindow FakeIsHungAppWindow
#define GetWindowLongPtrW FakeWindowLong
#define GetWindow FakeGetWindow
#define GetAncestor FakeAncestor
#define GetClassNameW FakeClassName
#define GetWindowTextLengthW FakeTextLength
#define GetWindowTextW FakeText
#define GetWindowRect FakeWindowRect
#define GetWindowPlacement FakePlacement
#define DwmGetWindowAttribute FakeDwmAttribute
#define GetClassLongPtrW FakeClassLong
#define GetForegroundWindow FakeForeground
#define SetForegroundWindow FakeSetForeground
#define AttachThreadInput FakeAttach
#define SetWindowPos FakeSetWindowPos
#define AllowSetForegroundWindow FakeAllowFront
#define PostMessageW FakePost
#define SendMessageTimeoutW FakeSendTimeout
#define ShowWindowAsync FakeShowAsync
#define OpenProcess FakeOpenProcess
#define QueryFullProcessImageNameW FakeImageName
#define GetApplicationUserModelId FakeProcessAumid
#define CloseHandle FakeCloseHandle
#define SHGetPropertyStoreForWindow FakeWindowStore
#define ShellExecuteExW FakeShellExecute
#define PrivateExtractIconsW FakeExtractIcons
#define SHCreateItemInKnownFolder FakeItemInKnownFolder
#define SHCreateItemFromParsingName FakeItemFromName
#define RegGetValueW FakeRegGetValue
#define SHGetKnownFolderPath FakeKnownFolderPath
#define Elevated FakeElevated
#define Log(...) ModelLog(__VA_ARGS__)
#define g_gfx iconrec::gfx
#include APPS_MODEL_CPP
#include APPS_MODEL_PINS_CPP
#include APPS_MODEL_CLICK_CPP
#include APPS_MODEL_PRESS_CPP
#undef g_gfx
#undef Log
#undef Elevated
#undef EnumWindows
#undef EnumChildWindows
#undef GetWindowThreadProcessId
#undef IsWindow
#undef IsWindowVisible
#undef IsIconic
#undef IsZoomed
#undef IsWindowEnabled
#undef IsHungAppWindow
#undef GetWindowLongPtrW
#undef GetWindow
#undef GetAncestor
#undef GetClassNameW
#undef GetWindowTextLengthW
#undef GetWindowTextW
#undef GetWindowRect
#undef GetWindowPlacement
#undef DwmGetWindowAttribute
#undef GetClassLongPtrW
#undef GetForegroundWindow
#undef SetForegroundWindow
#undef AttachThreadInput
#undef SetWindowPos
#undef AllowSetForegroundWindow
#undef PostMessageW
#undef SendMessageTimeoutW
#undef ShowWindowAsync
#undef OpenProcess
#undef QueryFullProcessImageNameW
#undef GetApplicationUserModelId
#undef CloseHandle
#undef SHGetPropertyStoreForWindow
#undef ShellExecuteExW
#undef PrivateExtractIconsW
#undef SHCreateItemInKnownFolder
#undef SHCreateItemFromParsingName
#undef RegGetValueW
#undef SHGetKnownFolderPath

// ---------------------------------------------------------------- fixtures
static const wchar_t kClass[] = L"LiquidDock.hunt-apps_model.FakeWindow";
static HWND NewHandle() {   // a message-only window of this test: an id no real window has
    return CreateWindowExW(0, kClass, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
}
static void Reset() {
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& [h, w] : wins) w.busy = false;   // no restore worker waits on a released program
    }
    woke.notify_all();
    for (int i = 0; i < 400 && inFlight; ++i) Sleep(5);
    Sleep(200);   // the workers' last looks (StartRestore: up to 6 x 25 ms)
    std::lock_guard<std::mutex> lk(mu);
    wins.clear(); zorder.clear(); procs.clear(); opened.clear();
    foreground = nullptr; takesFront = false;
    posted.clear(); frontCalls.clear(); frontAtCalls.clear(); started.clear();
    haveTaskband = false; taskband.clear();
    adminhelper::onActivate = adminhelper::onMinimize = adminhelper::onClose = adminhelper::onActivateOver = nullptr;
    isShellPopup = nullptr; onKeys = nullptr; keysSent.clear();
    launcherShown = -1; launcherAsked = 0; onLauncher = nullptr;
    { std::lock_guard<std::mutex> lg(logMu); logLines.clear(); }
    fakepress::beginWorks = false; fakepress::begun.clear(); fakepress::ready.clear();
    extractGives = nullptr; extractAsked.clear(); iconrec::made.clear();
    extractRefuses.clear(); iconrec::giveBack = false; imageAsked.clear(); appsFolder.clear(); parsedItems.clear(); knownFolderAsked.clear(); parsedAsked.clear();
    getIconAsked.clear(); windowIcon = nullptr; allowFrontCalls = 0; rightGives = ForegroundRight::Had;
    g_settings = Settings{};
    g_settings.taskbarAppButtons = false;   // the dock's own way, unless a check says otherwise
    g_settings.pinsVersion = 2;
}
static void Proc(DWORD pid, const std::wstring& exe, const std::wstring& aumid = L"") { std::lock_guard<std::mutex> lk(mu); procs[pid] = { exe, aumid }; }
// A new top-level window at the bottom of the z-order (front: at the top).
static HWND Win(DWORD pid, const std::wstring& title, bool front = false) {
    HWND h = NewHandle();
    std::lock_guard<std::mutex> lk(mu);
    FakeWin w;
    w.pid = pid; w.tid = pid * 10; w.title = title;
    wins[h] = w;
    if (front) zorder.insert(zorder.begin(), h); else zorder.push_back(h);
    return h;
}
template <class F> static void Edit(HWND h, F f) { { std::lock_guard<std::mutex> lk(mu); f(wins[h]); } woke.notify_all(); }
static FakeWin State(HWND h) { std::lock_guard<std::mutex> lk(mu); return wins[h]; }
static void SetFront(HWND h) { std::lock_guard<std::mutex> lk(mu); foreground = h; }
static int PostedTo(HWND h, UINT msg, WPARAM wp) {
    std::lock_guard<std::mutex> lk(mu);
    int n = 0;
    for (auto& p : posted) n += p.h == h && p.msg == msg && p.wp == wp;
    return n;
}
static int FrontCallsFor(HWND h) { std::lock_guard<std::mutex> lk(mu); return (int)std::count(frontCalls.begin(), frontCalls.end(), h); }
static bool WaitFor(const std::function<bool()>& ok, int ms) {
    for (int i = 0; i < ms / 5; ++i) { if (ok()) return true; Sleep(5); }
    return ok();
}
static std::shared_ptr<AppItem> ItemOf(AppModel& m, HWND h) { return m.FindByWindow(h); }
static std::shared_ptr<AppItem> Pinned(AppModel& m, const std::wstring& name, const std::wstring& exe, const std::wstring& aumid = L"", const std::wstring& launch = L"") {
    auto a = m.NewItem();
    a->pinned = true; a->name = name; a->exe = exe; a->aumid = aumid; a->launch = launch;
    m.items.insert(m.items.begin() + m.PinnedCount(), a);
    return a;
}
static std::wstring Names(const AppModel& m) {
    std::wstring s;
    for (auto& a : m.items) s += (s.empty() ? L"" : L",") + a->name;
    return s;
}
static std::wstring DataFile(const wchar_t* name) { return AppDataDir() + L"\\" + name; }
static std::wstring ExeDir() { wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); std::wstring s = p; return s.substr(0, s.find_last_of(L'\\')); }
// The taskbar's pin list as Windows keeps it: PIDLs one after another, a string UTF-16 wherever an item puts
// it - at an odd byte offset as often as at an even one (on a 25H2 PC, 2026-10-28: "Google Chrome.lnk"
// at byte 223, a store app's "Family!App" at byte 2035, "File Explorer.lnk" at 564).
static std::vector<BYTE> Blob(std::initializer_list<std::pair<std::wstring, bool>> strings) {
    std::vector<BYTE> b = { 0x00, 0x50, 0x01, 0x00, 0x00 };
    for (auto& [s, odd] : strings) {
        for (BYTE f : { 0x3a, 0x1f, 0x80, 0xc8, 0x27, 0x34 }) b.push_back(f);   // filler: no name characters in either alignment
        if ((b.size() % 2 == 1) != odd) b.push_back(0x14);
        for (wchar_t c : s) { b.push_back(BYTE(c & 0xff)); b.push_back(BYTE(c >> 8)); }
        b.push_back(0); b.push_back(0);
    }
    return b;
}
static void UseBlob(const std::vector<BYTE>& b) { std::lock_guard<std::mutex> lk(mu); taskband = b; haveTaskband = true; }
static std::wstring PinFolder() { return roaming + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar"; }
static void EmptyPinFolder() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((PinFolder() + L"\\*.lnk").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do DeleteFileW((PinFolder() + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void TouchPin(const wchar_t* name) { std::ofstream((PinFolder() + L"\\" + name).c_str()) << ""; }
// A real shortcut (in this test's folder) to a program, with arguments (and an AppUserModelID of its own, as a
// browser's profile shortcut has): what a user drops on the dock.
static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target, const wchar_t* args, const wchar_t* aumid = nullptr) {
    ComPtr<IShellLinkW> sl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetArguments(args);
    if (aumid) {
        ComPtr<IPropertyStore> ps;
        PROPVARIANT pv;
        if (FAILED(sl.As(&ps)) || FAILED(InitPropVariantFromString(aumid, &pv))) return false;
        const bool set = SUCCEEDED(ps->SetValue(PKEY_AppUserModel_ID, pv)) && SUCCEEDED(ps->Commit());
        PropVariantClear(&pv);
        if (!set) return false;
    }
    ComPtr<IPersistFile> pf;
    return SUCCEEDED(sl.As(&pf)) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
}
static std::wstring System32(const wchar_t* exe) { wchar_t d[MAX_PATH]; GetSystemDirectoryW(d, MAX_PATH); return ToLower(std::wstring(d) + L"\\" + exe); }

// ---------------------------------------------------------------- which windows count, and how they group
static void RefreshTests() {
    Reset();
    Proc(10, L"c:\\fake\\editor.exe");
    Proc(11, L"c:\\fake\\editor.exe");   // a second process of the same program
    Proc(12, L"c:\\fake\\browser.exe");
    Proc(13, L"c:\\program files\\windowsapps\\contoso.notes_1.0.0.0_x64__8wekyb3d8bbwe\\notes.exe", L"Contoso.Notes_8wekyb3d8bbwe!App");
    Proc(14, L"c:\\windows\\system32\\applicationframehost.exe");
    HWND e1 = Win(10, L"Doc 1"), e2 = Win(11, L"Doc 2");
    HWND tool = Win(10, L"Tool"); Edit(tool, [](FakeWin& w) { w.exStyle = WS_EX_TOOLWINDOW; });
    HWND owned = Win(10, L"Dialog"); Edit(owned, [&](FakeWin& w) { w.owner = e1; });
    HWND ownedApp = Win(10, L"Own button"); Edit(ownedApp, [&](FakeWin& w) { w.owner = e1; w.exStyle = WS_EX_APPWINDOW; });
    HWND cloaked = Win(10, L"Other desktop"); Edit(cloaked, [](FakeWin& w) { w.cloaked = true; });
    HWND untitled = Win(10, L"");
    HWND hidden = Win(10, L"Hidden"); Edit(hidden, [](FakeWin& w) { w.visible = false; });
    HWND noActivate = Win(10, L"Overlay"); Edit(noActivate, [](FakeWin& w) { w.exStyle = WS_EX_NOACTIVATE; });
    HWND b1 = Win(12, L"Browser"), b2 = Win(12, L"Browser profile 2");
    Edit(b2, [](FakeWin& w) { w.aumid = L"Browser.Profile2"; });
    HWND store = Win(13, L"Notes");
    HWND frame = Win(14, L"Notes 2"), core = NewHandle();
    { std::lock_guard<std::mutex> lk(mu); FakeWin c; c.pid = 13; c.tid = 130; c.cls = L"Windows.UI.Core.CoreWindow"; wins[core] = c; wins[frame].children = { core }; }
    HWND suspended = Win(14, L"Suspended");   // a frame whose app is not in it yet, and no id of its own
    Proc(GetCurrentProcessId(), L"c:\\fake\\liquiddock.exe");
    HWND own = Win(GetCurrentProcessId(), L"Liquid Dock");

    AppModel m;
    m.Refresh();
    auto editor = ItemOf(m, e1), browser = ItemOf(m, b1), notes = ItemOf(m, store);
    Check(m.items.size() == 3 && editor && browser && notes, "refresh: three apps from the windows that get taskbar buttons");
    Check(editor && editor->windows == std::vector<HWND>{ e1, e2, ownedApp }, "refresh: an exe's windows from two processes group in z-order; owned with WS_EX_APPWINDOW counts");
    for (HWND h : { tool, owned, cloaked, untitled, hidden, noActivate, suspended, own })
        Check(!ItemOf(m, h), "refresh: tool, owned, cloaked, untitled, hidden, no-activate, unidentified frames and the dock's own windows get no icon");
    Check(browser && ItemOf(m, b2) == browser, "refresh: a window with an AppUserModelID of its own stays with its program's icon");
    Check(notes && ItemOf(m, frame) == notes && notes->aumid == L"Contoso.Notes_8wekyb3d8bbwe!App", "refresh: a store app's frame (ApplicationFrameHost) goes to the app in it");
    Check(m.winCache.count(frame) && m.winCache[frame].exe.find(L"notes.exe") != std::wstring::npos, "refresh: the frame is cached as the app inside it");
    Check(!m.winCache.count(suspended), "refresh: an unidentified frame is not cached (looked at again next time)");
    // the next refresh: what was learnt of each window kept (moved over, not looked into again), the windows the same
    {
        const std::map<DWORD, int> before = [] { std::lock_guard<std::mutex> lk(mu); return opened; }();
        m.Refresh();
        const std::map<DWORD, int> after = [] { std::lock_guard<std::mutex> lk(mu); return opened; }();
        Check(m.items.size() == 3 && editor && ItemOf(m, e1) == editor && editor->windows == std::vector<HWND>{ e1, e2, ownedApp } && ItemOf(m, frame) == notes &&
              ItemOf(m, b2) == browser, "refresh again: the same apps, each with the same windows");
        Check(m.winCache.size() == 7 && m.winCache[e1].exe == L"c:\\fake\\editor.exe" && m.winCache[b2].aumid == L"Browser.Profile2" &&
              m.winCache[store].family == L"contoso.notes_8wekyb3d8bbwe" && m.winCache[frame].exe.find(L"notes.exe") != std::wstring::npos,
              "refresh again: each listed window's id, package and program kept in the cache");
        bool reopened = false;
        for (DWORD pid : { 10, 11, 12, 13 }) reopened |= (after.count(pid) ? after.at(pid) : 0) != (before.count(pid) ? before.at(pid) : 0);
        Check(!reopened && after.count(14) && before.count(14) && after.at(14) > before.at(14),
              "refresh again: no process of a window seen before opened again (only the unidentified frame's host)");
        Edit(e2, [](FakeWin& w) { w.visible = false; });   // hidden: off the list, and out of the cache
        m.Refresh();
        Check(!m.winCache.count(e2) && m.winCache.size() == 6 && editor && editor->windows == std::vector<HWND>{ e1, ownedApp }, "refresh: a window off the list leaves the cache");
        Edit(e2, [](FakeWin& w) { w.visible = true; });
        const int editorOpens = [] { std::lock_guard<std::mutex> lk(mu); return opened[11]; }();
        m.Refresh();
        Check(m.winCache.count(e2) && m.winCache[e2].exe == L"c:\\fake\\editor.exe" && editor && editor->windows == std::vector<HWND>{ e1, e2, ownedApp } &&
              [] { std::lock_guard<std::mutex> lk(mu); return opened[11]; }() > editorOpens, "refresh: ...and is looked into anew when it is back");
    }

    // windows go away: the running app leaves, a pinned one stays
    const int version = m.structureVersion;
    auto pin = Pinned(m, L"Browser", L"c:\\fake\\browser.exe");
    m.items.erase(std::find(m.items.begin(), m.items.end(), browser));
    m.Refresh();
    Check(ItemOf(m, b1) == pin && pin->windows.size() == 2, "refresh: a pinned app takes its program's windows (no second icon)");
    Edit(e1, [](FakeWin& w) { w.alive = false; });
    Edit(e2, [](FakeWin& w) { w.alive = false; });
    Edit(ownedApp, [](FakeWin& w) { w.alive = false; });
    Edit(b1, [](FakeWin& w) { w.alive = false; });
    Edit(b2, [](FakeWin& w) { w.alive = false; });
    m.Refresh();
    Check(!ItemOf(m, e1) && std::find(m.items.begin(), m.items.end(), editor) == m.items.end(), "refresh: a running app whose windows closed leaves the dock");
    Check(std::find(m.items.begin(), m.items.end(), pin) != m.items.end() && !pin->Running(), "refresh: a pinned app stays without windows");
    Check(m.structureVersion != version, "refresh: the dock is told the icons changed");
    Check(m.items.front() == pin, "refresh: pinned first, running after");

    // a store app pinned by one of its ids takes a window labelled with another id of the same package
    Reset();
    Proc(20, L"c:\\program files\\windowsapps\\contoso.mail_2.0.0.0_x64__8wekyb3d8bbwe\\mail.exe", L"Contoso.Mail_8wekyb3d8bbwe!Calendar");
    HWND cal = Win(20, L"Calendar");
    AppModel m2;
    auto mail = Pinned(m2, L"Mail", L"", L"Contoso.Mail_8wekyb3d8bbwe!Mail", L"shell:AppsFolder\\Contoso.Mail_8wekyb3d8bbwe!Mail");
    m2.Refresh();
    Check(ItemOf(m2, cal) == mail && m2.items.size() == 1, "refresh: the package family groups a store app's windows with its pin");

    // the shell's own windows get no icon; a folder window does, untitled too (as the taskbar)
    Reset();
    Proc(21, L"c:\\fake\\explorer.exe");
    std::vector<HWND> shell;
    for (const wchar_t* cls : { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" }) {
        shell.push_back(Win(21, L"Shell"));
        Edit(shell.back(), [cls](FakeWin& w) { w.cls = cls; });
    }
    HWND folder = Win(21, L""), progmanLike = Win(21, L"Program Manager 2");
    Edit(folder, [](FakeWin& w) { w.cls = L"CabinetWClass"; });
    Edit(progmanLike, [](FakeWin& w) { w.cls = L"Progman2"; });
    AppModel m3;
    m3.Refresh();
    bool anyShell = false;
    for (HWND h : shell) anyShell |= ItemOf(m3, h) != nullptr;
    Check(!anyShell && ItemOf(m3, folder) && ItemOf(m3, progmanLike) == ItemOf(m3, folder) && ItemOf(m3, folder)->windows.size() == 2,
          "refresh: desktop, its worker and the taskbars get no icon; an untitled folder window and another class do");
}

// ---------------------------------------------------------------- attention (HSHELL_FLASH)
static void AttentionTests() {
    Reset();
    Proc(30, L"c:\\fake\\chat.exe");
    Proc(31, L"c:\\fake\\other.exe");
    HWND chat = Win(30, L"Chat"), other = Win(31, L"Other", true);
    SetFront(other);
    AppModel m;
    m.Refresh();
    auto a = ItemOf(m, chat);
    m.OnShellHook(HSHELL_FLASH, (LPARAM)chat);
    Check(a && a->attention && m.attentionEvents.size() == 1, "attention: a flashing window behind bounces its icon once");
    m.OnShellHook(HSHELL_FLASH, (LPARAM)chat);
    Check(m.attentionEvents.size() == 1, "attention: it keeps flashing - no second bounce within 30 s");
    m.OnShellHook(HSHELL_WINDOWACTIVATED, (LPARAM)chat);
    Check(a && !a->attention && m.dirty, "attention: activating the window ends it");
    m.attentionEvents.clear();
    SetFront(chat);
    m.OnShellHook(HSHELL_FLASH, (LPARAM)chat);
    Check(m.attentionEvents.empty(), "attention: the window in front flashing is no request");
    SetFront(other);
    HWND later = Win(30, L"Chat 2");   // not seen yet: the flash looks it up
    m.OnShellHook(HSHELL_FLASH, (LPARAM)later);
    Check(m.attentionEvents.size() == 1 && ItemOf(m, later) == a, "attention: a window not listed yet is looked up at its flash");
    // the dock brought it up and Windows flashed it instead: not the program asking
    m.attentionEvents.clear();
    a->attention = false;
    m.BringUp(chat, L"test", 1);
    m.OnShellHook(HSHELL_FLASH, (LPARAM)chat);
    Check(m.attentionEvents.empty() && !a->attention, "attention: a window the dock is bringing up flashes without a bounce (quietFlash)");
}

// ---------------------------------------------------------------- clicks: minimize, restore, launch
static void ClickTests() {
    Reset();
    Proc(40, L"c:\\fake\\one.exe");
    Proc(41, L"c:\\fake\\other.exe");
    HWND one = Win(40, L"One", true), other = Win(41, L"Other");
    SetFront(one);
    AppModel m;
    m.Refresh();
    m.Click(*ItemOf(m, one), 0);
    Check(PostedTo(one, WM_SYSCOMMAND, SC_MINIMIZE) == 1, "click: the active app's only window is minimized (one command)");
    // minimized, its program idle: one restore, the window comes, the watch ends
    SetFront(other);
    takesFront = true;
    m.Click(*ItemOf(m, one), 0);
    Check(WaitFor([&] { return !State(one).iconic; }, 2000) && State(one).restoreSends == 1, "click: a minimized window gets one restore command");
    m.Tick(NowSeconds() + 0.3);
    Check(!m.watch.window && m.lastUp.window == one, "click: the watch ends when the window came up");
    m.Click(*ItemOf(m, one), 0);
    Check(PostedTo(one, WM_SYSCOMMAND, SC_MINIMIZE) == 1, "click: a click right after it came never minimizes it (kJustUp)");
    m.Tick(NowSeconds() + 0.3);   // that click's watch: it is up already
    m.lastUp.at = NowSeconds() - 5;
    m.Click(*ItemOf(m, one), 0);
    Check(PostedTo(one, WM_SYSCOMMAND, SC_MINIMIZE) == 2, "click: later, the active window is minimized again");

    // its program handles the restore command and leaves the window down: the system's restore, once
    Reset();
    Proc(42, L"c:\\fake\\lazy.exe");
    Proc(43, L"c:\\fake\\other.exe");
    HWND lazy = Win(42, L"Lazy"), o2 = Win(43, L"Other", true);
    Edit(lazy, [](FakeWin& w) { w.iconic = true; w.leavesDown = true; });
    SetFront(o2);
    AppModel ml;
    ml.Refresh();
    ml.Click(*ItemOf(ml, lazy), 0);
    Check(WaitFor([&] { return !State(lazy).iconic; }, 2000) && State(lazy).restoresHandled == 1 && State(lazy).systemRestores == 1,
          "click: a restore its program left undone gets the system's restore once");

    // a busy program: clicks while it has not answered add no second restore
    Reset();
    Proc(44, L"c:\\fake\\busy.exe");
    Proc(45, L"c:\\fake\\other.exe");
    HWND busy = Win(44, L"Busy"), o3 = Win(45, L"Other", true);
    Edit(busy, [](FakeWin& w) { w.iconic = true; w.busy = true; w.restoresMaximized = true; });
    SetFront(o3);
    AppModel mb;
    mb.Refresh();
    mb.Click(*ItemOf(mb, busy), 0);
    WaitFor([&] { return State(busy).restoreSends == 1; }, 2000);
    mb.Click(*ItemOf(mb, busy), 0);
    mb.Click(*ItemOf(mb, busy), 0);
    Sleep(150);
    Check(State(busy).restoreSends == 1 && mb.watch.clicks == 3, "click: clicks while a busy program has not answered send no second restore");
    Edit(busy, [](FakeWin& w) { w.busy = false; });
    WaitFor([&] { return !State(busy).iconic; }, 2000);
    Check(State(busy).zoomed, "click: the busy program's maximized window comes back maximized");

    // pinned, not running: started once, through its shortcut - or its program when the shortcut is gone
    Reset();
    AppModel mp;
    auto gone = Pinned(mp, L"Gone", L"c:\\fake\\app.exe", L"", L"c:\\fake\\no such shortcut.lnk");
    mp.Click(*gone, 0);
    Check(started.size() == 1 && started[0].first == L"c:\\fake\\app.exe" && started[0].second.empty() && gone->launchStarted > 0,
          "launch: a pinned app's missing shortcut falls back to its program");
    auto store = Pinned(mp, L"Store", L"", L"Contoso.App_8wekyb3d8bbwe!App", L"shell:AppsFolder\\Contoso.App_8wekyb3d8bbwe!App");
    mp.Launch(*store, 0, true);
    Check(started.size() == 2 && started[1].second.empty(), "launch: run as administrator is not asked for a store app");
    mp.Launch(*gone, 0, true);
    Check(started.size() == 3 && started[2].second == L"runas", "launch: run as administrator for a program");
    auto none = Pinned(mp, L"Nothing", L"");
    mp.Launch(*none, 0);
    Check(started.size() == 3, "launch: nothing to start - nothing started");
}

// The design (app_click.h): never a second restore command while the program has not answered
// the first - a second restore takes a window coming back maximized out of maximized. The watch is replaced
// when the user goes on meanwhile; the command stays on its way, and the dock remembers it apart from the
// watch (AppModel::restores).
static void RestoreAfterGoingOnTests() {
    for (int variant = 0; variant < 2; ++variant) {
        Reset();
        Proc(50, L"c:\\fake\\busy.exe");
        Proc(51, L"c:\\fake\\first.exe");
        Proc(52, L"c:\\fake\\second.exe");
        HWND busy = Win(50, L"Busy"), first = Win(51, L"First", true), second = Win(52, L"Second");
        Edit(busy, [](FakeWin& w) { w.iconic = true; w.busy = true; w.restoresMaximized = true; });
        SetFront(first);
        AppModel m;
        m.Refresh();
        m.Click(*ItemOf(m, busy), 0);
        WaitFor([&] { return State(busy).restoreSends == 1; }, 2000);
        if (variant == 0) {   // the user goes on (Alt+Tab): the watch stops
            SetFront(second);
            m.Tick(NowSeconds() + 0.3);
            Check(!m.watch.window, "busy restore: the watch stops when the user goes on to another program");
        } else {              // the user clicks another app's icon: its watch replaces the first
            m.Click(*ItemOf(m, second), 0);
            Check(m.watch.window == second, "busy restore: a click on another app watches that app's window");
        }
        m.Click(*ItemOf(m, busy), 0);   // and comes back to the busy one within kBusyWait
        Sleep(200);
        Check(State(busy).restoreSends == 1, variant == 0 ? "busy restore: back to the app after Alt+Tab - no second restore while its program has not answered"
                                                          : "busy restore: back to the app after another icon - no second restore while its program has not answered");
        Edit(busy, [](FakeWin& w) { w.busy = false; });
        WaitFor([&] { return inFlight == 0; }, 2000);
        Sleep(100);
        Check(State(busy).zoomed && !State(busy).iconic, "busy restore: its maximized window comes back maximized (not made normal size by a second restore)");
        Check(m.RestoreUnderWay(busy, NowSeconds() + appclick::kAfterRestore + 0.1) == nullptr && m.restores.empty(),
              "busy restore: a command answered a while ago is forgotten (the list does not grow)");
    }
    // a restore command of the dock's own still unanswered (its program busy, the watch over: the user went on), and
    // the next click would go to the app's taskbar button: no press on top of the command - the dock waits for it
    Reset();
    g_settings.taskbarAppButtons = true;
    fakepress::beginWorks = true;
    Proc(53, L"c:\\fake\\busy.exe");
    Proc(54, L"c:\\fake\\first.exe");
    Proc(55, L"c:\\fake\\second.exe");
    HWND busy = Win(53, L"Busy"), first = Win(54, L"First", true), second = Win(55, L"Second");
    Edit(busy, [](FakeWin& w) { w.iconic = true; w.busy = true; w.restoresMaximized = true; });
    SetFront(first);
    AppModel u;
    u.Refresh();
    u.slowUntil = NowSeconds() + 60;   // Explorer slow a moment ago: this click goes the dock's own way
    u.Click(*ItemOf(u, busy), 0);
    WaitFor([&] { return State(busy).restoreSends == 1; }, 2000);
    Check(fakepress::begun.empty() && State(busy).restoreSends == 1, "busy restore, taskbar: the first click is the dock's (Explorer slow)");
    SetFront(second);
    u.Tick(NowSeconds() + 0.3);   // the user went on: the watch stops
    u.slowUntil = 0;              // Explorer answers again: a click would press the app's taskbar button
    u.Click(*ItemOf(u, busy), 0);
    Sleep(200);
    Check(fakepress::begun.empty() && u.presses.empty() && State(busy).restoreSends == 1 && u.watch.window == busy && u.watch.restore,
          "busy restore, taskbar: back to it while the dock's command is unanswered - no taskbar press on top of it, nor a second command");
    Edit(busy, [](FakeWin& w) { w.busy = false; });
    WaitFor([&] { return inFlight == 0; }, 2000);
    Sleep(100);
    Check(State(busy).zoomed && !State(busy).iconic, "busy restore, taskbar: ...and it comes back maximized");

    // The dock's restore brought it up (the watch saw it come) and the user minimized it again at once: a click
    // within kAfterRestore of that answer is a new wish, a restore of its own - for a program that restores it
    // itself, and for one that leaves it to the system's restore.
    for (int lazy = 0; lazy < 2; ++lazy) {
        Reset();
        Proc(56, L"c:\\fake\\app.exe");
        Proc(57, L"c:\\fake\\other.exe");
        HWND app = Win(56, L"App"), other = Win(57, L"Other", true);
        Edit(app, [&](FakeWin& w) { w.iconic = true; w.restoresMaximized = true; w.leavesDown = lazy != 0; });
        SetFront(other);
        takesFront = true;
        AppModel r;
        r.Refresh();
        r.Click(*ItemOf(r, app), 0);
        // up, and its command marked handled (the worker's last step)
        WaitFor([&] { return !State(app).iconic && r.watch.restore && r.watch.restore->state == 1; }, 2000);
        r.Tick(NowSeconds() + 0.3);
        Check(!r.watch.window && r.lastUp.window == app && r.restores.empty(), lazy ? "restore again: came up by the system's restore - its handled command is done with"
                                                                                  : "restore again: came up - its handled command is done with");
        Edit(app, [](FakeWin& w) { w.iconic = true; });   // minimized again at once
        SetFront(other);
        r.Click(*ItemOf(r, app), 0);
        Check(WaitFor([&] { return State(app).restoreSends == 2 && !State(app).iconic; }, 2000) && State(app).zoomed,
              lazy ? "restore again: minimized again right after the system's restore brought it, a click restores it again (maximized)"
                   : "restore again: minimized again right after its restore, a click restores it again (maximized)");
    }
    // ...but a command its program has not answered stays remembered when the window comes up meanwhile: a click on it
    // minimized again sends no second (that command still arrives)
    Reset();
    Proc(58, L"c:\\fake\\busy.exe");
    Proc(59, L"c:\\fake\\other.exe");
    HWND slow = Win(58, L"Busy"), o4 = Win(59, L"Other", true);
    Edit(slow, [](FakeWin& w) { w.iconic = true; w.busy = true; w.restoresMaximized = true; });
    SetFront(o4);
    takesFront = true;
    AppModel s;
    s.Refresh();
    s.Click(*ItemOf(s, slow), 0);
    WaitFor([&] { return State(slow).restoreSends == 1; }, 2000);
    Edit(slow, [](FakeWin& w) { w.iconic = false; w.zoomed = true; });   // up by other means, its program still busy
    SetFront(slow);
    s.Tick(NowSeconds() + 0.3);
    Check(!s.watch.window && s.restores.size() == 1, "restore again: came up while its program has not answered - that command stays remembered");
    Edit(slow, [](FakeWin& w) { w.iconic = true; });
    SetFront(o4);
    s.Click(*ItemOf(s, slow), 0);
    Sleep(200);
    Check(State(slow).restoreSends == 1 && s.watch.window == slow && s.watch.restore && s.watch.restore->state == 0,
          "restore again: ...and a click on it minimized again waits for that command - no second");
    Edit(slow, [](FakeWin& w) { w.busy = false; });
    WaitFor([&] { return inFlight == 0; }, 2000);
    Sleep(100);
    Check(State(slow).zoomed && !State(slow).iconic, "restore again: ...which brings it back maximized");
}

// ---------------------------------------------------------------- pins.txt
static void PinsFileTests() {
    Reset();
    DeleteFileW(DataFile(L"pins.txt").c_str());
    AppModel m;
    const std::wstring longDir(260, L'd');
    const std::wstring odd = L"c:\\program files\\ünïcødé 한글 😀, comma; semi\\" + longDir + L"\\app.exe";
    Pinned(m, L"Ünïcødé 한글 😀, comma; \"quoted\"", odd, L"", L"C:\\Users\\x\\Desktop\\My App, v2.lnk");
    Pinned(m, L"Store", L"", L"Contoso.App_8wekyb3d8bbwe!App", L"shell:AppsFolder\\Contoso.App_8wekyb3d8bbwe!App");
    auto running = m.NewItem();
    running->name = L"Running only"; running->exe = L"c:\\fake\\r.exe";
    m.items.push_back(running);
    m.SavePins();
    AppModel back;
    back.LoadPins();
    Check(back.items.size() == 2, "pins: only pinned apps are saved");
    Check(back.items.size() == 2 && back.items[0]->name == m.items[0]->name && back.items[0]->exe == odd && back.items[0]->launch == m.items[0]->launch,
          "pins: unicode, commas, quotes and a 300-character path come back as saved");
    Check(back.items.size() == 2 && back.items[1]->aumid == L"Contoso.App_8wekyb3d8bbwe!App" && back.items[1]->exe.empty() && back.items[1]->pinned,
          "pins: a store app's id and empty path come back, in order");
    { std::ofstream f(DataFile(L"pins.txt").c_str(), std::ios::app); f << "broken line\n\n" << "a\tb\n"; }
    AppModel skip;
    skip.LoadPins();
    Check(skip.items.size() == 2, "pins: lines with fewer than four fields are skipped");
    // a name with a tab or a line break (a window title, a file description) in it
    AppModel t;
    Pinned(t, L"Tab\tin name", L"c:\\fake\\tab.exe", L"", L"c:\\fake\\tab.exe");
    Pinned(t, L"Line\r\nbreak", L"c:\\fake\\line.exe", L"", L"c:\\fake\\line.exe");
    t.SavePins();
    AppModel tb;
    tb.LoadPins();
    Check(tb.items.size() == 2 && tb.items[0]->exe == L"c:\\fake\\tab.exe" && tb.items[0]->aumid.empty() && tb.items[1]->exe == L"c:\\fake\\line.exe",
          "pins: a tab or line break in a name does not shift or split the saved pin");
    Check(Names(tb) == L"Tab in name,Line  break", "pins: ...each saved as a space");
    // written whole or not at all: a save that cannot be written leaves the last pins.txt as it was (an empty one would
    // be taken for the user's choice), and no temporary file is left behind
    const std::wstring tmp = DataFile(L"pins.txt") + L".tmp";
    Check(GetFileAttributesW(tmp.c_str()) == INVALID_FILE_ATTRIBUTES, "pins: saved - no temporary file left");
    CreateDirectoryW(tmp.c_str(), nullptr);   // the temporary file cannot be made
    AppModel lost;
    Pinned(lost, L"Lost", L"c:\\fake\\lost.exe", L"", L"c:\\fake\\lost.exe");
    lost.SavePins();
    RemoveDirectoryW(tmp.c_str());
    AppModel kept;
    kept.LoadPins();
    Check(Names(kept) == L"Tab in name,Line  break", "pins: a save that cannot be written leaves the last pins.txt whole");
    // a name earlier versions made of the lowercased path (no file description): spelled again; a shortcut's own stays
    { std::ofstream f(DataFile(L"pins.txt").c_str(), std::ios::trunc); f << "new\t\tc:\\fake\\new.exe\tC:\\Fake\\New.exe\n" << "tool\t\tc:\\fake\\tool.exe\tC:\\Fake\\tool.lnk\n"; }
    AppModel low;
    low.LoadPins();
    Check(Names(low) == L"New,tool", "pins: a name saved lowercased from the program's path is spelled again; a shortcut's own name stays");
    // an empty file is the user's choice (everything unpinned): no import from the taskbar
    { std::ofstream f(DataFile(L"pins.txt").c_str(), std::ios::trunc); }
    UseBlob(Blob({ { L"Contoso.Notes_8wekyb3d8bbwe!App", false } }));
    AppModel e;
    e.LoadPins();
    Check(e.items.empty(), "pins: an empty pins file stays empty (no import)");
}

// ---------------------------------------------------------------- the taskbar's pins, imported once
static void ImportTests() {
    CreateDirectoryW((roaming + L"\\Microsoft").c_str(), nullptr);
    CreateDirectoryW((roaming + L"\\Microsoft\\Internet Explorer").c_str(), nullptr);
    CreateDirectoryW((roaming + L"\\Microsoft\\Internet Explorer\\Quick Launch").c_str(), nullptr);
    CreateDirectoryW((roaming + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned").c_str(), nullptr);
    CreateDirectoryW(PinFolder().c_str(), nullptr);
    for (int odd = 0; odd < 2; ++odd) {
        Reset();
        EmptyPinFolder();
        TouchPin(L"Alpha.lnk");
        TouchPin(L"Zulu.lnk");
        // pin order: Zulu, Alpha, the store app (FindFirstFile lists Alpha first)
        UseBlob(Blob({ { L"Zulu.lnk", odd != 0 }, { L"Alpha.lnk", false }, { L"Contoso.Notes_8wekyb3d8bbwe!App", odd != 0 } }));
        DeleteFileW(DataFile(L"pins.txt").c_str());
        g_settings.pinsVersion = 0;
        AppModel m;
        m.LoadPins();
        std::wstring order;
        for (auto& a : m.items) order += (order.empty() ? L"" : L",") + (a->aumid.empty() ? a->name : a->aumid);
        if (!odd) {
            Check(order == L"Zulu,Alpha,Contoso.Notes_8wekyb3d8bbwe!App", "import: the taskbar's pins in its order, store apps too (strings at even offsets)");
        } else {
            Check(order.find(L"Contoso.Notes_8wekyb3d8bbwe!App") != std::wstring::npos, "import: a store app pinned at an odd byte offset of the pin list is imported");
            Check(order.rfind(L"Zulu,Alpha", 0) == 0, "import: a shortcut named at an odd byte offset keeps its place in the pin order");
            Check(order == L"Zulu,Alpha,Contoso.Notes_8wekyb3d8bbwe!App", "import: ...the whole order the taskbar's, odd and even offsets mixed");
        }
        Check(g_settings.pinsVersion == 2 && GetFileAttributesW(DataFile(L"pins.txt").c_str()) != INVALID_FILE_ATTRIBUTES, "import: saved once, and marked done");
    }
    // pins from before store apps were known: the missing store pins are added once, none twice
    for (int odd = 0; odd < 2; ++odd) {
        Reset();
        UseBlob(Blob({ { L"Contoso.Notes_8wekyb3d8bbwe!App", odd != 0 }, { L"Contoso.Mail_8wekyb3d8bbwe!Mail", false } }));
        g_settings.pinsVersion = 0;
        AppModel m;
        Pinned(m, L"Editor", L"c:\\fake\\editor.exe", L"", L"c:\\fake\\editor.exe");
        Pinned(m, L"Mail", L"", L"Contoso.Mail_8wekyb3d8bbwe!Calendar", L"shell:AppsFolder\\Contoso.Mail_8wekyb3d8bbwe!Calendar");
        m.MigratePins();
        int notes = 0, mail = 0;
        for (auto& a : m.items) { notes += a->aumid == L"Contoso.Notes_8wekyb3d8bbwe!App"; mail += PackageFamily(a->aumid) == L"contoso.mail_8wekyb3d8bbwe"; }
        Check(mail == 1, "migrate: a store app pinned already (another id of its package) is not added again");
        if (!odd) Check(notes == 1 && m.items.size() == 3 && m.items.back()->pinned, "migrate: a missing store pin is added once, after the pins");
        else Check(notes == 1, "migrate: a missing store pin at an odd byte offset of the pin list is added");
        Check(g_settings.pinsVersion == 2, "migrate: done once");
    }
    // the parser on odd input: no read outside the blob, no false pins
    static const wchar_t kTwice[] = L"\0Ab_1234567890123!App\0Ab_1234567890123!App\0Cd_abcdefghijklm!X";
    Check(PackagedPins(L"").empty() && PackagedPins(L"!").empty() && PackagedPins(L"!!!").empty() && PackagedPins(L"x!").empty(), "parser: a bang alone, at either end, is no pin");
    Check(PackagedPins(L"Ab_1234567890123!").empty() && PackagedPins(L"_1234567890123!App").empty() && PackagedPins(L"Ab_123456789012!App").empty(),
          "parser: no app id, no package name, or a publisher id not 13 long is no pin");
    auto one = PackagedPins(L"Ab_1234567890123!App");
    Check(one.size() == 1 && one[0].first == 0 && one[0].second == L"Ab_1234567890123!App", "parser: a pin at the very start and end of the blob");
    auto twice = PackagedPins(std::wstring(kTwice, ARRAYSIZE(kTwice) - 1));
    Check(twice.size() == 2 && twice[0].first == 1 && twice[1].second == L"Cd_abcdefghijklm!X", "parser: a pin named twice is listed once, at its first place");
    Check(PackageFamily(L"Contoso.Notes_8wekyb3d8bbwe!App") == L"contoso.notes_8wekyb3d8bbwe" && PackageFamily(L"Chrome").empty(), "parser: package family of an id");
}

// ---------------------------------------------------------------- pinning and moving
static void PinTests() {
    Reset();
    AppModel m;
    auto a = Pinned(m, L"A", L"c:\\fake\\a.exe", L"", L"c:\\fake\\a.exe");
    auto b = Pinned(m, L"B", L"c:\\fake\\b.exe", L"", L"c:\\fake\\b.exe");
    auto c = Pinned(m, L"C", L"c:\\fake\\c.exe", L"", L"c:\\fake\\c.exe");
    auto r = m.NewItem(); r->name = L"R"; r->exe = L"c:\\fake\\r.exe"; r->windows = { NewHandle() }; m.items.push_back(r);
    m.MoveTo(*c, -5, true);
    Check(Names(m) == L"C,A,B,R", "move: an index below 0 is the first place");
    m.MoveTo(*c, 99, true);
    Check(Names(m) == L"A,B,C,R", "move: an index past the pins is the last pinned place");
    m.MoveTo(*a, 1, true);
    Check(Names(m) == L"B,A,C,R", "move: the index counts the others (the dock's drop index)");
    {
        AppModel saved;
        saved.LoadPins();
        Check(Names(saved) == L"B,A,C", "move: a pinned app moved is saved at its new place");
    }
    auto pinsText = [] { std::ifstream f(DataFile(L"pins.txt").c_str(), std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
    { std::ofstream(DataFile(L"pins.txt").c_str(), std::ios::binary | std::ios::trunc) << "not written since"; }
    m.MoveTo(*r, 0, false);
    Check(Names(m) == L"B,A,C,R" && !r->pinned, "move: a running app stays among the running ones");
    Check(pinsText() == "not written since", "move: ...and pins.txt is not written (the pins did not change)");
    m.MoveTo(*b, 2, true);
    Check(Names(m) == L"A,C,B,R" && pinsText() != "not written since", "move: a pinned app moved writes pins.txt again");
    m.MoveTo(*b, 0, true);
    m.SetPinned(*r, true);
    Check(Names(m) == L"B,A,C,R" && r->pinned && r->launch == L"c:\\fake\\r.exe", "pin: a running app pinned at the end, started by its program");
    m.SetPinned(*a, false);
    Check(Names(m) == L"B,C,R", "unpin: an app not running leaves the dock");
    m.SetPinned(*r, false);
    Check(Names(m) == L"B,C,R" && !r->pinned, "unpin: a running app moves to the running ones");
    AppModel back;
    back.LoadPins();
    Check(Names(back) == L"B,C", "pin changes are saved");

    // a file dropped on the dock: the dock's index counts the icons left of the drop point (dock_input.cpp WM_DROPFILES)
    Reset();
    AppModel d;
    Pinned(d, L"A", L"c:\\fake\\a.exe", L"", L"c:\\fake\\a.exe");
    Pinned(d, L"B", L"c:\\fake\\b.exe", L"", L"c:\\fake\\b.exe");
    Pinned(d, L"C", L"c:\\fake\\c.exe", L"", L"c:\\fake\\c.exe");
    d.PinFile(L"C:\\Fake\\New.exe", 1);
    Check(d.items.size() == 4 && d.items[1]->exe == L"c:\\fake\\new.exe" && d.items[1]->pinned, "drop: a new program is pinned at the drop point");
    Check(d.items.size() == 4 && d.items[1]->name == L"New", "drop: a program without a file description is named as its file is spelled (not lowercased)");
    d.PinFile(L"C:\\Fake\\New.exe", 4);
    Check(d.items.size() == 4, "drop: a program pinned already is not pinned twice");
    Check(Names(d) == L"A,B,C,new" || Names(d) == L"A,B,C,New", "drop: ...it moves to the drop point at the end");
    // dropped between B and C (two icons left of the point: A and B), A's own program: A goes there
    Reset();
    AppModel d2;
    Pinned(d2, L"A", L"c:\\fake\\a.exe", L"", L"c:\\fake\\a.exe");
    Pinned(d2, L"B", L"c:\\fake\\b.exe", L"", L"c:\\fake\\b.exe");
    Pinned(d2, L"C", L"c:\\fake\\c.exe", L"", L"c:\\fake\\c.exe");
    d2.PinFile(L"C:\\Fake\\A.exe", 2);
    Check(Names(d2) == L"B,A,C", "drop: an app dropped right of itself lands at the drop point (between B and C), not one further");
    // a shortcut dropped for an app that runs, unpinned: the shortcut (its arguments) is what the pin starts
    Reset();
    const std::wstring cmd = System32(L"cmd.exe");
    const std::wstring lnk = ExeDir() + L"\\hunt-apps_model Work profile.lnk";
    Proc(60, cmd);
    HWND w = Win(60, L"Command Prompt");
    AppModel d3;
    d3.Refresh();
    auto run = ItemOf(d3, w);
    Check(MakeShortcut(lnk, cmd, L"/k title work") && run && !run->pinned, "drop: fixture shortcut and running app");
    d3.PinFile(lnk, 0);
    Check(run && run->pinned && d3.items.size() == 1, "drop: the running app becomes the pin (no second icon)");
    Check(run && run->launch == lnk, "drop: the pin starts the dropped shortcut (with its arguments), not the bare program");
    AppModel d3b;
    d3b.LoadPins();
    Check(d3b.items.size() == 1 && d3b.items[0]->launch == lnk, "drop: ...and so after the next start (pins.txt)");
    const std::wstring other = ExeDir() + L"\\hunt-apps_model Other.lnk";
    Check(MakeShortcut(other, cmd, L"/k title other"), "drop: fixture, another shortcut to the program");
    d3.PinFile(other, 0);
    Check(run && run->launch == lnk && d3.items.size() == 1, "drop: a pinned app keeps what it starts when another shortcut of its program is dropped (moved only)");
    DeleteFileW(lnk.c_str());
    DeleteFileW(other.c_str());
    // a shortcut to another id of a running program (a browser profile's): a pin of its own, as Refresh groups windows
    // (its windows go to that pin); a shortcut with the running app's own id: that app becomes the pin
    Reset();
    const std::wstring profile = ExeDir() + L"\\hunt-apps_model Profile 2.lnk", same = ExeDir() + L"\\hunt-apps_model Default.lnk";
    Proc(61, cmd);
    HWND def = Win(61, L"Default profile");
    Edit(def, [](FakeWin& f) { f.aumid = L"Hunt.Fake.Default"; });
    AppModel d4;
    d4.Refresh();
    auto running = ItemOf(d4, def);
    Check(MakeShortcut(profile, cmd, L"/k title profile 2", L"Hunt.Fake.Profile2") && MakeShortcut(same, cmd, L"/k title default", L"Hunt.Fake.Default") && running,
          "drop: fixture shortcuts with ids of their own");
    d4.PinFile(profile, 0);
    Check(d4.items.size() == 2 && d4.items[0]->pinned && d4.items[0]->aumid == L"Hunt.Fake.Profile2" && d4.items[0]->launch == profile && running && !running->pinned,
          "drop: a shortcut to another id of a running program is a pin of its own (the running app stays as it was)");
    d4.PinFile(same, 1);
    Check(d4.items.size() == 2 && running && running->pinned && running->launch == same, "drop: a shortcut with the running app's own id pins that app, started by the shortcut");
    DeleteFileW(profile.c_str());
    DeleteFileW(same.c_str());
    // several files in one drop: each goes right after the one before (dock_input.cpp passes on where PinFile put it)
    Reset();
    AppModel d5;
    Pinned(d5, L"A", L"c:\\fake\\a.exe", L"", L"c:\\fake\\a.exe");
    Pinned(d5, L"B", L"c:\\fake\\b.exe", L"", L"c:\\fake\\b.exe");
    Pinned(d5, L"C", L"c:\\fake\\c.exe", L"", L"c:\\fake\\c.exe");
    int idx = 2;   // between B and C
    for (const wchar_t* path : { L"C:\\Fake\\A.exe", L"C:\\Fake\\X.exe" }) idx = d5.PinFile(path, idx) + 1;
    Check(Names(d5) == L"B,A,X,C", "drop: A (pinned already) and a new X dropped together between B and C: B,A,X,C");
    Check(d5.PinFile(L"C:\\Fake\\C.exe", 0) == 0 && Names(d5) == L"C,B,A,X", "drop: PinFile says where the app went");
    Check(d5.PinFile(L"C:\\Fake\\C.exe", 1) == 0 && Names(d5) == L"C,B,A,X", "drop: an app dropped on its own icon's right half stays where it is");
}

// ---------------------------------------------------------------- closing
static void CloseTests() {
    Reset();
    Proc(70, L"c:\\fake\\two.exe");
    HWND w1 = Win(70, L"First"), w2 = Win(70, L"Second");
    AppModel m;
    m.Refresh();
    m.CloseAll(*ItemOf(m, w1));
    Check(PostedTo(w1, WM_CLOSE, 0) == 1 && PostedTo(w2, WM_CLOSE, 0) == 1, "close all: each window is asked to close once");
    m.AskToClose(w2, L"Two");
    Check(PostedTo(w2, WM_SYSCOMMAND, SC_CLOSE) == 1, "close one: as its own close button (SC_CLOSE)");
    adminhelper::onClose = [](HWND) { return true; };
    m.AskToClose(w1, L"Two");
    Check(PostedTo(w1, WM_SYSCOMMAND, SC_CLOSE) == 0, "close one: a window the helper closes gets nothing of the dock's");
    // the helper's first call may ask the user (a message box, admin_helper.cpp Offer): the dock's messages go on
    // meanwhile - a flash of a window not listed refreshes the list, and the windows' order changed
    Reset();
    Proc(71, L"c:\\fake\\two.exe");
    HWND x1 = Win(71, L"First"), x2 = Win(71, L"Second");
    HWND stranger = NewHandle();
    AppModel mc;
    mc.Refresh();
    bool asked = false;
    adminhelper::onClose = [&](HWND) {
        if (asked) return false;
        asked = true;
        { std::lock_guard<std::mutex> lk(mu); std::swap(zorder[0], zorder[1]); }
        mc.OnShellHook(HSHELL_FLASH, (LPARAM)stranger);
        return false;
    };
    auto app = ItemOf(mc, x1);
    mc.CloseAll(*app);
    Check(PostedTo(x1, WM_CLOSE, 0) == 1 && PostedTo(x2, WM_CLOSE, 0) == 1, "close all: a list refreshed during the helper's question still closes each window once");
}

// ---------------------------------------------------------------- presses for a running app (MaybeOnItsWay)
static void MaybeTests() {
    Reset();
    Proc(80, L"c:\\fake\\m.exe");
    HWND h = Win(80, L"M"), k = Win(80, L"K");
    Edit(h, [](FakeWin& w) { w.iconic = true; });
    AppModel m;
    const double now = NowSeconds();
    m.maybes.push_back({ k, now + 8, true, false });   // looked at before h's: MaybeOnItsWay stops at the window asked for
    m.maybes.push_back({ h, now + 8, false, true });
    Check(m.MaybeOnItsWay(h, now) && m.maybes.size() == 2, "maybe: a restore press not shown yet keeps the dock off the window");
    Edit(k, [](FakeWin& w) { w.iconic = true; });
    Check(m.MaybeOnItsWay(h, now) && m.maybes.size() == 1, "maybe: a minimize press that showed is over");
    Edit(h, [](FakeWin& w) { w.iconic = false; });
    SetFront(h);
    Check(!m.MaybeOnItsWay(h, now) && m.maybes.empty(), "maybe: the window up in front - over");
    m.maybes.push_back({ h, now + 8, false, false });
    Check(!m.MaybeOnItsWay(h, now + 9) && m.maybes.empty(), "maybe: over after kBusyWait");
    m.maybes.push_back({ h, now + 8, false, false });
    m.maybes.push_back({ k, now + 8, false, false });
    m.ForgetMaybe(h);
    Check(m.maybes.size() == 1 && m.maybes[0].window == k, "maybe: forgetting one window keeps the others");
    // a restore press that restored it, but Windows kept another window in front: it arrived all the same
    m.maybes.clear();
    Edit(h, [](FakeWin& w) { w.iconic = false; });
    Edit(k, [](FakeWin& w) { w.iconic = false; });
    SetFront(k);
    m.maybes.push_back({ h, now + 8, false, true });
    Check(!m.MaybeOnItsWay(h, now) && m.maybes.empty(), "maybe: a restore press that restored it behind another window - over (it arrived)");
    m.maybes.push_back({ h, now + 8, false, false });
    Check(m.MaybeOnItsWay(h, now), "maybe: ...but a bring-up of a window that was up, still behind: not shown yet");
}

// ---------------------------------------------------------------- a program with several icons (CheckWatch)
// A browser's profiles are icons of their own (their windows' ids) in one program. The profile in front at the
// click staying in front is no sign that the other came: it is put in front again (and the log says so if it
// never comes). Another window of that program coming in front after the click ends the watch - no fight.
static void SameProgramTests() {
    Reset();
    Proc(100, L"c:\\fake\\browser.exe");
    HWND p1 = Win(100, L"Profile 1", true), p2 = Win(100, L"Profile 2"), p1b = Win(100, L"Profile 1, another window");
    Edit(p1, [](FakeWin& w) { w.aumid = L"Browser.Profile1"; });
    Edit(p1b, [](FakeWin& w) { w.aumid = L"Browser.Profile1"; });
    Edit(p2, [](FakeWin& w) { w.aumid = L"Browser.Profile2"; });
    SetFront(p1);   // takesFront false: Windows refuses the dock the front
    AppModel m;
    m.Refresh();
    Check(ItemOf(m, p2) && ItemOf(m, p1) && ItemOf(m, p2) != ItemOf(m, p1), "same program: its profiles are icons of their own");
    if (!ItemOf(m, p2)) return;
    m.Click(*ItemOf(m, p2), 0);
    const int tries = FrontCallsFor(p2);
    m.Tick(NowSeconds() + 0.3);
    Check(m.watch.window == p2 && m.watch.refocused == 1 && FrontCallsFor(p2) > tries,
          "same program: the other profile still in front is no sign it came - put in front again");
    m.Tick(NowSeconds() + 0.6);
    m.Tick(NowSeconds() + 0.9);
    Check(!m.watch.window && m.watch.refocused == 0, "same program: ...and given up on after the tries (a line in the log)");
    m.Click(*ItemOf(m, p2), 0);
    SetFront(p1b);   // the user goes on to another window of that program
    m.Tick(NowSeconds() + 0.3);
    Check(!m.watch.window, "same program: another of its windows coming in front after the click ends the watch (no fight over the front)");
    // a window of its own in front: an owned dialog of the window brought up counts as it - after the click, and when
    // it was in front at the click already (the window itself under its dialog is up)
    for (int already = 0; already < 2; ++already) {
        Reset();
        Proc(101, L"c:\\fake\\editor.exe");
        Proc(102, L"c:\\fake\\other.exe");
        HWND ed = Win(101, L"Editor"), o = Win(102, L"Other", true), dlg = Win(101, L"Save?");
        Edit(dlg, [&](FakeWin& w) { w.owner = ed; });
        SetFront(already ? dlg : o);
        AppModel n;
        n.Refresh();
        n.Click(*ItemOf(n, ed), 0);
        SetFront(dlg);
        n.Tick(NowSeconds() + 0.3);
        Check(!n.watch.window && n.lastUp.window == ed, already ? "same program: its dialog in front since before the click - it is up (no fight with its dialog)"
                                                                : "same program: its dialog in front after the click - it came");
    }
}

// ---------------------------------------------------------------- clicks through the taskbar's buttons (stubbed)
static void PressTests() {
    Reset();
    g_settings.taskbarAppButtons = true;
    fakepress::beginWorks = true;
    Proc(90, L"c:\\fake\\single.exe");
    Proc(91, L"c:\\fake\\other.exe");
    HWND s = Win(90, L"Single"), o = Win(91, L"Other", true);
    SetFront(o);
    AppModel m;
    m.Refresh();
    auto a = ItemOf(m, s);
    m.Click(*a, 0);
    Check(fakepress::begun.size() == 1 && m.presses.size() == 1 && FrontCallsFor(s) == 0, "press: one window behind - its taskbar button is looked up, the dock does nothing itself");
    Check(fakepress::begun.size() == 1 && fakepress::begun[0].exe == L"c:\\fake\\single.exe" && fakepress::begun[0].running, "press: looked up by its program, as running");
    m.Click(*a, 0);
    Check(fakepress::begun.size() == 1 && m.presses.size() == 1 && m.presses[0].clicks == 2, "press: a click during the lookup only counts");
    taskbarapps::Result r;
    r.job = m.presses[0].job; r.outcome = taskbarapps::Outcome::Pressed; r.took = 0.02;
    fakepress::ready[r.job] = r;
    m.PollPresses(NowSeconds());
    Check(m.watch.window == s && m.watch.byTaskbar && m.presses.empty(), "press: pressed - the dock watches the window");
    SetFront(s);
    m.Tick(NowSeconds() + 0.3);
    Check(!m.watch.window && m.pressUse.use == taskbarapps::Use::Works && FrontCallsFor(s) == 0, "press: the window came - the taskbar's way works, nothing of the dock's");

    // no press that works on these buttons (25H2): the dock does the click at once, and goes its own way from then on
    Reset();
    g_settings.taskbarAppButtons = true;
    fakepress::beginWorks = true;
    Proc(92, L"c:\\fake\\single.exe");
    Proc(93, L"c:\\fake\\other.exe");
    HWND s2 = Win(92, L"Single"), o2 = Win(93, L"Other", true);
    SetFront(o2);
    AppModel n;
    n.Refresh();
    n.Click(*ItemOf(n, s2), 0);
    r = {};
    r.job = n.presses[0].job; r.outcome = taskbarapps::Outcome::NoWay;
    fakepress::ready[r.job] = r;
    n.PollPresses(NowSeconds());
    Check(n.pressUse.use == taskbarapps::Use::Off && FrontCallsFor(s2) >= 1 && n.watch.window == s2 && !n.watch.byTaskbar, "press: no way to press - the dock brings it up itself at once");
    n.watch = {};
    n.Click(*ItemOf(n, s2), 0);
    Check(fakepress::begun.size() == 1, "press: ...and no button is looked up again this run");

    // a lookup Explorer is too slow for: abandoned after kLookupWait, the dock does the click
    Reset();
    g_settings.taskbarAppButtons = true;
    fakepress::beginWorks = true;
    Proc(94, L"c:\\fake\\single.exe");
    Proc(95, L"c:\\fake\\other.exe");
    HWND s3 = Win(94, L"Single"), o3 = Win(95, L"Other", true);
    SetFront(o3);
    AppModel p;
    p.Refresh();
    p.Click(*ItemOf(p, s3), 0);
    auto stage = p.presses[0].stage;
    const double clicked = p.presses[0].clicked;
    p.PollPresses(clicked + 0.5);
    Check(p.presses.size() == 1 && FrontCallsFor(s3) == 0, "press: within kLookupWait the dock waits for the lookup");
    p.PollPresses(clicked + taskbarapps::kLookupWait + 0.1);
    Check(p.presses.empty() && *stage == taskbarapps::kAbandoned && FrontCallsFor(s3) >= 1 && p.slowUntil > clicked, "press: abandoned - the dock does it, no presses for a while");

    // a press that may still arrive: nothing of the dock's on top (a preview pick does nothing)
    Reset();
    g_settings.taskbarAppButtons = true;
    fakepress::beginWorks = true;
    Proc(96, L"c:\\fake\\single.exe");
    Proc(97, L"c:\\fake\\other.exe");
    HWND s4 = Win(96, L"Single"), o4 = Win(97, L"Other", true);
    SetFront(o4);
    AppModel q;
    q.Refresh();
    q.Click(*ItemOf(q, s4), 0);
    r = {};
    r.job = q.presses[0].job; r.outcome = taskbarapps::Outcome::Unsure;
    fakepress::ready[r.job] = r;
    q.PollPresses(NowSeconds());
    q.ActivateWindow(s4);
    Check(q.maybes.size() == 1 && FrontCallsFor(s4) == 0, "press: unsure whether it arrived - a preview pick adds nothing on top");

    // a press that restored the window but left it behind another (Windows gave Explorer no front): the press arrived -
    // the dock puts it in front itself after kPressCheck, without a restore command of its own on top
    Reset();
    g_settings.taskbarAppButtons = true;
    fakepress::beginWorks = true;
    Proc(98, L"c:\\fake\\single.exe");
    Proc(99, L"c:\\fake\\other.exe");
    HWND s5 = Win(98, L"Single"), o5 = Win(99, L"Other", true);
    Edit(s5, [](FakeWin& w) { w.iconic = true; w.restoresMaximized = true; });
    SetFront(o5);
    AppModel b;
    b.Refresh();
    b.Click(*ItemOf(b, s5), 0);
    Check(b.presses.size() == 1, "press behind: a minimized window's button is looked up");
    if (b.presses.empty()) return;
    r = {};
    r.job = b.presses[0].job; r.outcome = taskbarapps::Outcome::Pressed; r.took = 0.02;
    fakepress::ready[r.job] = r;
    b.PollPresses(NowSeconds());
    Edit(s5, [](FakeWin& w) { w.iconic = false; w.zoomed = true; });   // Explorer restored it; the other window stays in front
    b.Tick(NowSeconds() + 0.3);
    Check(b.watch.byTaskbar && FrontCallsFor(s5) == 0, "press behind: a moment after the restore it is looked at again (the activation may follow)");
    b.Tick(NowSeconds() + taskbarapps::kPressCheck + 0.1);
    Check(b.watch.window == s5 && !b.watch.byTaskbar && FrontCallsFor(s5) >= 1 && State(s5).restoreSends == 0 && State(s5).zoomed,
          "press behind: the dock puts it in front itself - no restore command on top (it stays maximized)");
    Check(b.pressUse.use == taskbarapps::Use::Try && b.pressUse.duds == 1, "press behind: the press arrived - the taskbar's way is not turned off for it");

    // no button found for an app: not looked up again for a while, kept under its id - which is never given again, so
    // nothing is kept once the app left the dock (its next run is a new app, looked up anew)
    Reset();
    g_settings.taskbarAppButtons = true;
    fakepress::beginWorks = true;
    Proc(86, L"c:\\fake\\single.exe");
    Proc(87, L"c:\\fake\\other.exe");
    HWND s6 = Win(86, L"Single"), o6 = Win(87, L"Other", true);
    SetFront(o6);
    AppModel g;
    g.Refresh();
    auto noButton = [&](HWND h) {   // a click on its app, and Explorer's taskbar has no button of it
        auto app = ItemOf(g, h);
        if (!app) return false;
        g.watch = {};
        g.Click(*app, 0);
        if (g.presses.empty()) return false;
        r = {};
        r.job = g.presses[0].job; r.outcome = taskbarapps::Outcome::NoButton;
        fakepress::ready[r.job] = r;
        g.PollPresses(NowSeconds());
        return true;
    };
    const int firstId = ItemOf(g, s6) ? ItemOf(g, s6)->id : -1;
    Check(noButton(s6) && g.noButtonUntil.count(firstId) && g.saidPress.count(firstId) && FrontCallsFor(s6) >= 1,
          "no button: the dock brings it up itself, and keeps that under the app's id");
    g.watch = {};
    g.Click(*ItemOf(g, s6), 0);
    Check(fakepress::begun.size() == 1 && g.presses.empty(), "no button: ...the app's next click looks up nothing (kNoButtonAgain)");
    Edit(s6, [](FakeWin& w) { w.alive = false; });
    g.Refresh();
    Check(!ItemOf(g, s6) && g.noButtonUntil.empty() && g.saidPress.empty(), "no button: the app left the dock - nothing kept under its id");
    int runs = 0;
    for (; runs < 20; ++runs) {   // started and closed again and again: a new app each run
        HWND again = Win(86, L"Single");
        g.Refresh();
        if (!noButton(again)) break;
        Edit(again, [](FakeWin& w) { w.alive = false; });
        g.Refresh();
    }
    Check(runs == 20 && fakepress::begun.size() == 21 && g.noButtonUntil.empty() && g.saidPress.empty(),
          "no button: 20 runs of the app - each looked up anew, and nothing kept after them (no growth)");
    auto unpinned = Pinned(g, L"Pinned", L"c:\\fake\\p.exe", L"", L"c:\\fake\\p.exe");
    g.saidPress[unpinned->id] = 1;
    g.noButtonUntil[unpinned->id] = NowSeconds() + taskbarapps::kNoButtonAgain;
    g.SetPinned(*unpinned, false);
    Check(std::find(g.items.begin(), g.items.end(), unpinned) == g.items.end() && g.noButtonUntil.empty() && g.saidPress.empty(),
          "no button: a pinned app not running, unpinned (off the dock) - nothing kept under its id");

    // a pinned app started by its button: no window in kLaunchCheck twice in a row - the dock starts pinned apps itself
    Reset();
    AppModel l;
    auto pin = Pinned(l, L"Pinned", L"c:\\fake\\p.exe", L"", L"c:\\fake\\p.exe");
    const double t0 = NowSeconds();
    l.launchChecks.push_back({ pin->id, t0, 0, false });
    l.CheckLaunches(t0 + 5);
    Check(l.launchChecks.size() == 1, "launch check: waits up to kLaunchCheck");
    l.CheckLaunches(t0 + taskbarapps::kLaunchCheck + 0.5);
    Check(l.launchChecks.empty() && !l.launchUse.off && l.launchUse.misses == 1, "launch check: one miss");
    l.launchChecks.push_back({ pin->id, t0, 0, true });
    l.CheckLaunches(t0 + taskbarapps::kLaunchCheck + 0.5);
    Check(l.launchUse.misses == 1, "launch check: a press that may not have arrived is no miss");
    pin->windows = { NewHandle() };
    l.launchChecks.push_back({ pin->id, t0, 0, false });
    l.CheckLaunches(t0 + 1);
    Check(l.launchChecks.empty() && l.launchUse.misses == 0, "launch check: a window came - misses start over");
    pin->windows.clear();
    for (int i = 0; i < 2; ++i) { l.launchChecks.push_back({ pin->id, t0, 0, false }); l.CheckLaunches(t0 + taskbarapps::kLaunchCheck + 0.5); }
    Check(l.launchUse.off, "launch check: two misses in a row - pinned apps are started by the dock");
}

// ---------------------------------------------------------------- Start or Search in front at the click (BringUp, CheckWatch)
// Windows keeps a shell pop-up in front of any window the dock asks for, and a click on the dock (which never takes
// activation) leaves it open (seen on a test PC, 2026-10-29: SearchHost in front, seven clicks that did nothing). The
// dock closes it with Esc - only while it is in front: a key for the window behind it would be a key in that
// program - and brings the window up once it has gone, the window the shell gave the front back to taken as the one
// in front at the click.
static HWND StartMenu(DWORD pid) {   // Start: a tool window of the shell's (no icon), the fake's only shell pop-up
    Proc(pid, L"c:\\windows\\systemapps\\microsoft.windows.startmenuexperiencehost_cw5n1h2txyewy\\startmenuexperiencehost.exe");
    HWND s = Win(pid, L"Start", true);
    Edit(s, [](FakeWin& w) { w.exStyle = WS_EX_TOOLWINDOW; });
    isShellPopup = [s](HWND h) { return h == s; };
    return s;
}
static const std::vector<WORD> kEsc = { VK_ESCAPE };
static bool OnlyEsc(size_t n) { return keysSent.size() == n && std::all_of(keysSent.begin(), keysSent.end(), [](const auto& k) { return k == kEsc; }); }
static bool NoFrontCallOver(HWND popup) { std::lock_guard<std::mutex> lk(mu); return std::find(frontAtCalls.begin(), frontAtCalls.end(), popup) == frontAtCalls.end(); }

static void PopupTests() {
    // it closes at the Esc, Windows giving the front back to the window behind it (OBS): the window comes - or, Windows
    // refusing the dock, OBS staying is no sign the user went on
    for (int refused = 0; refused < 2; ++refused) {
        Reset();
        Proc(110, L"c:\\fake\\app.exe");
        Proc(111, L"c:\\fake\\obs64.exe");
        HWND app = Win(110, L"App"), obs = Win(111, L"OBS", true);
        HWND start = StartMenu(112);
        SetFront(start);
        takesFront = !refused;
        onKeys = [obs](const std::vector<WORD>& k) { if (k == kEsc) SetFront(obs); };   // Start closes at its Esc
        AppModel m;
        m.Refresh();
        m.Click(*ItemOf(m, app), 0);
        const double t0 = NowSeconds();
        Check(OnlyEsc(1) && FrontCallsFor(app) == 0 && m.watch.window == app && m.watch.popup == start && m.watch.popupClosed == 1,
              refused ? "pop-up, refused: the click sends one Esc and nothing that brings the window up"
                      : "pop-up: Start in front at the click - one Esc, and nothing that brings the window up while it is in front");
        m.Tick(t0 + 0.15);
        Check(OnlyEsc(1) && FrontCallsFor(app) >= 1 && NoFrontCallOver(start) && !m.watch.popup && m.watch.before == obs,
              refused ? "pop-up, refused: ...gone - the window put in front, OBS taken as the window in front at the click"
                      : "pop-up: ...gone at the next look - the window put in front (no SetForegroundWindow while Start was in front, no second Esc)");
        m.Tick(t0 + 0.5);
        if (!refused) {
            Check(!m.watch.window && m.lastUp.window == app, "pop-up: ...it came - the watch ends done (OBS coming back in front is not the user going on)");
            Check(LogCount(L"came in front") == 0 && LogCount(L"could not bring up") == 0, "pop-up: ...and no line in the log");
        } else {
            Check(m.watch.window == app && m.watch.refocused == 1, "pop-up, refused: OBS still in front is not the user going on - put in front again (no stop)");
            m.Tick(t0 + 0.8);
            m.Tick(t0 + 1.1);
            Check(!m.watch.window && OnlyEsc(1) && LogCount(L"up, but another window stays in front") == 1 && LogCount(L"Start or Search was in front at the click: closed with Esc") == 1,
                  "pop-up, refused: ...given up on after the tries - the log line says Start was closed with Esc first");
        }
    }

    // a pop-up that does not close: Esc again after kPopupGone, then a line in the log; a second click meanwhile only counts
    Reset();
    Proc(113, L"c:\\fake\\app.exe");
    Proc(114, L"c:\\fake\\other.exe");
    HWND app = Win(113, L"App"), other = Win(114, L"Other", true);
    HWND start = StartMenu(115);
    SetFront(start);
    takesFront = true;   // Windows would take the dock's call: none may come while Start is in front
    AppModel m;
    m.Refresh();
    m.Click(*ItemOf(m, app), 0);
    m.Click(*ItemOf(m, app), 0);
    const double t0 = NowSeconds();
    Check(OnlyEsc(1) && m.watch.window == app && m.watch.popup == start && m.watch.clicks == 2 && m.watch.popupClosed == 1,
          "pop-up stays: a second click while it closes counts - no second Esc at once, the same wait");
    m.Tick(t0 + 0.15);
    Check(OnlyEsc(1) && m.watch.popup == start, "pop-up stays: within kPopupGone of the Esc the dock only looks");
    m.Tick(t0 + 0.6);
    Check(OnlyEsc(2) && m.watch.popupClosed == 2, "pop-up stays: ...still in front kPopupGone after the Esc - Esc again");
    m.Tick(t0 + 0.7);
    Check(OnlyEsc(2) && m.watch.window == app, "pop-up stays: ...and waits again after the second");
    m.Tick(t0 + 1.2);
    Check(!m.watch.window && OnlyEsc(2) && FrontCallsFor(app) == 0 && LogCount(L"stayed in front after Esc 2 times") == 1,
          "pop-up stays: ...given up on - Esc twice, nothing that brings the window up, a 'stayed in front' line in the log");
    m.Tick(t0 + 2.0);
    Check(OnlyEsc(2) && !m.lastUp.window, "pop-up stays: no key after the give-up");

    // no Esc when the pop-up is not in front by the time the key would be sent
    Reset();
    Proc(116, L"c:\\fake\\app.exe");
    Proc(117, L"c:\\fake\\other.exe");
    app = Win(116, L"App"); other = Win(117, L"Other", true);
    start = StartMenu(118);
    isShellPopup = [start, other](HWND h) { if (h != start) return false; SetFront(other); return true; };   // closing by itself just then
    SetFront(start);
    takesFront = true;
    AppModel a;
    a.Refresh();
    a.Click(*ItemOf(a, app), 0);
    double t1 = NowSeconds();
    Check(keysSent.empty() && a.watch.popup == start && a.watch.popupClosed == 1 && FrontCallsFor(app) == 0,
          "pop-up gone between the look and the key: no Esc (it would go to the window behind it)");
    a.Tick(t1 + 0.15);
    Check(keysSent.empty() && !a.watch.popup && FrontCallsFor(app) >= 1, "pop-up gone between the look and the key: ...the window put in front at the next look, still no key");
    a.Tick(t1 + 0.5);
    Check(!a.watch.window && a.lastUp.window == app, "pop-up gone between the look and the key: ...it came");
    // ...nor a second Esc when it went just before that one was due
    Reset();
    Proc(116, L"c:\\fake\\app.exe");
    Proc(117, L"c:\\fake\\other.exe");
    app = Win(116, L"App"); other = Win(117, L"Other", true);
    start = StartMenu(118);
    SetFront(start);
    takesFront = true;
    AppModel b;
    b.Refresh();
    b.Click(*ItemOf(b, app), 0);
    t1 = NowSeconds();
    b.Tick(t1 + 0.15);
    SetFront(other);   // Start closed late, after its Esc
    b.Tick(t1 + 0.6);
    Check(OnlyEsc(1) && !b.watch.popup && FrontCallsFor(app) >= 1 && NoFrontCallOver(start),
          "pop-up gone just before the second Esc was due: no second Esc - the window put in front");

    // a minimized window: its restore command once the pop-up has gone, never while it is in front, never twice
    Reset();
    Proc(119, L"c:\\fake\\app.exe");
    Proc(120, L"c:\\fake\\obs64.exe");
    app = Win(119, L"App");
    HWND obs = Win(120, L"OBS", true);
    Edit(app, [](FakeWin& w) { w.iconic = true; w.restoresMaximized = true; });
    start = StartMenu(121);
    SetFront(start);
    takesFront = true;
    AppModel r;
    r.Refresh();
    r.Click(*ItemOf(r, app), 0);
    t1 = NowSeconds();
    Sleep(100);
    Check(OnlyEsc(1) && State(app).restoreSends == 0 && State(app).systemRestores == 0 && State(app).iconic && !r.watch.restore,
          "pop-up, minimized: no restore while Start is in front");
    r.Tick(t1 + 0.15);
    Check(OnlyEsc(1) && State(app).restoreSends == 0 && State(app).iconic && r.watch.popup == start, "pop-up, minimized: ...nor while it closes");
    SetFront(obs);   // Start went
    r.Tick(t1 + 0.3);
    Check(WaitFor([&] { return !State(app).iconic; }, 2000) && State(app).restoreSends == 1, "pop-up, minimized: ...gone - one restore command");
    r.Tick(t1 + 0.6);
    WaitFor([&] { return inFlight == 0; }, 2000);
    Sleep(100);
    Check(State(app).restoreSends == 1 && State(app).systemRestores == 0 && State(app).zoomed && !r.watch.window && r.lastUp.window == app,
          "pop-up, minimized: ...exactly one restore - it comes back maximized, the watch done");

    // the shell hands the front on through none for a moment, then through another of its windows (Start's after
    // Search's): neither is the window the pop-up goes back to - the dock waits, closes that one too, then goes on
    Reset();
    Proc(140, L"c:\\fake\\app.exe");
    Proc(141, L"c:\\fake\\obs64.exe");
    HWND app2 = Win(140, L"App"), obs2 = Win(141, L"OBS", true);
    HWND search = StartMenu(142);
    HWND start2 = Win(142, L"Start", true);
    isShellPopup = [search, start2](HWND h) { return h == search || h == start2; };
    SetFront(search);
    takesFront = true;
    int escs = 0;
    onKeys = [&escs, obs2](const std::vector<WORD>& k) {
        if (k != kEsc) return;
        SetFront(++escs == 1 ? nullptr : obs2);   // the first Esc: nothing in front for a moment; the second closes Start
    };
    AppModel q;
    q.Refresh();
    q.Click(*ItemOf(q, app2), 0);
    const double tq = NowSeconds();
    Check(OnlyEsc(1) && FrontCallsFor(app2) == 0, "pop-up hand-on: one Esc at the click");
    q.Tick(tq + 0.15);
    Check(q.watch.popup == search && FrontCallsFor(app2) == 0 && q.watch.before != nullptr,
          "pop-up hand-on: nothing in front a moment after the Esc - waited on, not taken as the window behind it");
    SetFront(start2);   // then Start's own window
    q.Tick(tq + 0.3);
    Check(q.watch.popup == start2 && FrontCallsFor(app2) == 0, "pop-up hand-on: ...another window of the shell - the one being closed now");
    q.Tick(tq + 0.7);
    Check(OnlyEsc(2) && FrontCallsFor(app2) == 0, "pop-up hand-on: ...Esc to it once kPopupGone has passed");
    q.Tick(tq + 0.85);
    Check(!q.watch.popup && q.watch.before == obs2 && FrontCallsFor(app2) >= 1 && NoFrontCallOver(search) && NoFrontCallOver(start2),
          "pop-up hand-on: ...gone - OBS taken as the window in front at the click, the window put in front");

    // a closed one keeping the front: its window cloaked, nothing on the screen (seen on a test PC, 2026-10-30: SearchHost
    // after the monitors came back on - six clicks on Chrome dropped, a 'stayed in front after Esc 2 times' line each,
    // no Esc ever sent). Nothing to close: no key, the window brought up as usual - a minimized one restored.
    for (int minimized = 0; minimized < 2; ++minimized) {
        Reset();
        Proc(150, L"c:\\fake\\app.exe");
        HWND g = Win(150, L"App");
        if (minimized) Edit(g, [](FakeWin& w) { w.iconic = true; });
        HWND ghost = StartMenu(151);
        Edit(ghost, [](FakeWin& w) { w.cloaked = true; });
        SetFront(ghost);
        takesFront = true;
        AppModel c;
        c.Refresh();
        c.Click(*ItemOf(c, g), 0);
        const double tc = NowSeconds();
        Check(keysSent.empty() && !c.watch.popup && c.watch.ghostFront && c.watch.window == g && c.watch.before == ghost &&
                  (minimized ? WaitFor([&] { return !State(g).iconic; }, 2000) && State(g).restoreSends == 1 : FrontCallsFor(g) >= 1),
              minimized ? "closed pop-up keeping the front, minimized: no Esc - its restore command at the click"
                        : "closed pop-up keeping the front: no Esc - the window put in front at the click");
        Check(LogCount(L"is in front but not shown") == 1 && LogCount(L"cloaked") >= 1,
              "closed pop-up keeping the front: ...a line in the log, the window in front said to be cloaked");
        c.Tick(tc + 0.3);
        c.Tick(tc + 0.6);
        Check(!c.watch.window && c.lastUp.window == g && LogCount(L"stayed in front") == 0 &&
                  LogCount(L"over the closed Start or Search that kept the front") == 1,
              "closed pop-up keeping the front: ...it came - and the log says so, however soon");
    }

    // ...Windows refusing the dock, the user opens Search meanwhile - the shell's same window, shown again: the user went
    // on, the watch stops (no more pushes over it, no give-up line)
    Reset();
    Proc(154, L"c:\\fake\\app.exe");
    HWND r2 = Win(154, L"App");
    HWND ghost2 = StartMenu(155);
    Edit(ghost2, [](FakeWin& w) { w.cloaked = true; });
    SetFront(ghost2);
    takesFront = false;
    AppModel u;
    u.Refresh();
    u.Click(*ItemOf(u, r2), 0);
    const double tu = NowSeconds();
    const int pushes = FrontCallsFor(r2);
    Edit(ghost2, [](FakeWin& w) { w.cloaked = false; });
    u.Tick(tu + 0.3);
    Check(!u.watch.window && FrontCallsFor(r2) == pushes && keysSent.empty() && LogCount(L"could not bring up") == 0,
          "closed pop-up keeping the front, refused: the user opens it meanwhile - the watch stops, nothing pushed over it");

    // Esc sent, the pop-up cloaked on its way out while it still has the front: nothing more to close - the window
    // brought up at the next look (no second Esc, no give-up)
    Reset();
    Proc(152, L"c:\\fake\\app.exe");
    HWND o = Win(152, L"App");
    HWND closing = StartMenu(153);
    SetFront(closing);
    takesFront = true;
    onKeys = [closing](const std::vector<WORD>& k) { if (k == kEsc) Edit(closing, [](FakeWin& w) { w.cloaked = true; }); };
    AppModel e;
    e.Refresh();
    e.Click(*ItemOf(e, o), 0);
    const double te = NowSeconds();
    Check(OnlyEsc(1) && e.watch.popup == closing && e.watch.popupEsc == 1 && FrontCallsFor(o) == 0, "pop-up closing: one Esc at the click");
    e.Tick(te + 0.15);
    Check(OnlyEsc(1) && !e.watch.popup && FrontCallsFor(o) >= 1,
          "pop-up closing: ...cloaked at the next look, still in front - nothing to close: the window put in front, no second Esc");
    e.Tick(te + 0.5);
    Check(!e.watch.window && e.lastUp.window == o && LogCount(L"stayed in front") == 0, "pop-up closing: ...it came, no give-up line");
    Check(launcherAsked == 0, "a shown pop-up in front: Start's visibility never asked (only for a cloaked shell window)");

    // a cloaked shell window in front: the closed one keeping the front (mod.25) whatever IAppVisibility says (an open
    // Start's window in front is not cloaked: measured on a test PC, tests\run_start_ghost_live.bat) - no Esc, the
    // window brought up as usual, the log saying what was seen
    for (int answer = -1; answer <= 1; ++answer) {
        Reset();
        Proc(167, L"c:\\fake\\app.exe");
        HWND gk = Win(167, L"App");
        HWND closed = StartMenu(168);
        Edit(closed, [](FakeWin& w) { w.cloaked = true; });
        SetFront(closed);
        launcherShown = answer;
        takesFront = true;
        AppModel gc;
        gc.Refresh();
        gc.Click(*ItemOf(gc, gk), 0);
        Check(keysSent.empty() && !gc.watch.popup && gc.watch.ghostFront && FrontCallsFor(gk) >= 1 &&
                  LogCount(answer == 1 ? L"cloak 1, Start shown: yes)" : answer == 0 ? L"cloak 1, Start shown: no)" : L"cloak 1, Start shown: ?)") == 1,
              answer == 1 ? "cloaked shell window, Start said shown: still the closed one keeping the front - no Esc, put in front, the log says what IAppVisibility said"
              : answer == 0 ? "cloaked shell window, Start not shown: the closed one keeping the front - no Esc, put in front, the log says Start was not shown"
                            : "cloaked shell window, the shell not answering: as before - no Esc, put in front, the log says it is not known");
    }

    // ...Windows refusing the dock over it (seen on a test PC, 2026-11-17: eight clicks right after Start, the right held, attaching
    // refused): the give-up line has the cloak, whether Start showed at the click and at the end, and what
    // SetForegroundWindow answered
    Reset();
    Proc(176, L"c:\\fake\\app.exe");
    HWND gr = Win(176, L"App");
    HWND keeps = StartMenu(177);
    Edit(keeps, [](FakeWin& w) { w.cloaked = true; });
    SetFront(keeps);
    launcherShown = 0;
    takesFront = false;
    AppModel gu;
    gu.Refresh();
    gu.Click(*ItemOf(gu, gr), 0);
    const double tg = NowSeconds();
    for (double d : { 0.3, 0.6, 0.9 }) gu.Tick(tg + d);
    Check(!gu.watch.window && keysSent.empty() && LogCount(L"up, but another window stays in front") == 1 &&
              LogCount(L"a closed Start or Search kept the front at the click") == 1 && LogCount(L"cloak 1 at the click, Start shown: no then, no now") == 1 &&
              LogCount(L"SetForegroundWindow said 0, then 0 (error 0)") == 1,
          "closed pop-up keeping the front, refused: the give-up line has the cloak, Start then and now, and SetForegroundWindow's answers");

    // a shown pop-up gone between the look and the key, Windows refusing the dock: the give-up line does not say an Esc
    // closed it
    Reset();
    Proc(178, L"c:\\fake\\app.exe");
    Proc(179, L"c:\\fake\\other.exe");
    HWND gn = Win(178, L"App"), other3 = Win(179, L"Other", true);
    HWND quick = StartMenu(180);
    isShellPopup = [quick, other3](HWND h) { if (h != quick) return false; SetFront(other3); return true; };   // closing by itself just then
    SetFront(quick);
    takesFront = false;
    AppModel gq;
    gq.Refresh();
    gq.Click(*ItemOf(gq, gn), 0);
    const double tq2 = NowSeconds();
    for (double d : { 0.15, 0.45, 0.75, 1.05 }) gq.Tick(tq2 + d);
    Check(!gq.watch.window && keysSent.empty() && LogCount(L"Start or Search was in front at the click: it went before an Esc was sent") == 1 &&
              LogCount(L"closed with Esc") == 0,
          "pop-up gone before the key, refused: the give-up line says no Esc was sent");
}

// ---------------------------------------------------------------- a window that runs as administrator in front (Push)
// Windows does not let the dock (it runs as the user) take the front from such a window: its key for the right is
// dropped there, attaching to it is refused, and SetForegroundWindow only flashes the window (seen on a test PC,
// 2026-10-29: OBS run as administrator). The installed helper (it runs as administrator too) is asked instead - for
// a window that is up: a minimized one waits until it is (the helper restores what it brings up). Its answer is no
// proof: one older than mod.24 takes the request and cannot take the front - the dock's own calls then too.
static int overCalls = 0;
static HWND overFor = nullptr;
static void HelperOver(bool there, bool brings = true) {   // brings: the helper puts the window in front
    overCalls = 0; overFor = nullptr;
    adminhelper::onActivateOver = [there, brings](HWND h) {
        ++overCalls; overFor = h;
        if (there && brings) SetFront(h);
        return there;
    };
}
static void Elevate(DWORD pid) { std::lock_guard<std::mutex> lk(mu); procs[pid].elevated = true; }

static void AdminFrontTests() {
    // the helper there: the click is its
    Reset();
    Proc(130, L"c:\\fake\\app.exe");
    Proc(131, L"c:\\fake\\obs64.exe");
    Elevate(131);
    HWND app = Win(130, L"App"), obs = Win(131, L"OBS", true);
    SetFront(obs);   // takesFront false: Windows refuses the dock the front
    HelperOver(true);
    AppModel m;
    m.Refresh();
    Check(adminhelper::FakeElevated(obs) && !adminhelper::FakeElevated(app) && !adminhelper::Elevated(obs),
          "admin in front: fixture - OBS's fake process runs as administrator (Windows' own answer for a fake window: no)");
    m.Click(*ItemOf(m, app), 0);
    double t0 = NowSeconds();
    Check(overCalls == 1 && overFor == app && FrontCallsFor(app) == 0 && m.watch.overAdmin == 1,
          "admin in front: the click hands the bring-up to the helper once - none of the dock's own calls (they would only flash it)");
    m.Tick(t0 + 0.3);
    Check(!m.watch.window && m.lastUp.window == app && overCalls == 1 && FrontCallsFor(app) == 0, "admin in front: ...it came - done, the helper asked once");

    // the helper took it and the window did not come (a helper older than mod.24): asked again at each try, the dock's
    // own calls after each answer, and the log says so
    Reset();
    Proc(130, L"c:\\fake\\app.exe");
    Proc(131, L"c:\\fake\\obs64.exe");
    Elevate(131);
    app = Win(130, L"App"); obs = Win(131, L"OBS", true);
    SetFront(obs);
    HelperOver(true, false);
    AppModel g;
    g.Refresh();
    g.Click(*ItemOf(g, app), 0);
    t0 = NowSeconds();
    g.Tick(t0 + 0.3);
    g.Tick(t0 + 0.6);
    g.Tick(t0 + 0.9);
    Check(!g.watch.window && overCalls == 1 + appclick::kRefocusTries && FrontCallsFor(app) >= 1 + appclick::kRefocusTries &&
          LogCount(L"handed to the admin helper 3 times (an administrator's window in front)") == 1 && LogCount(L"in front: obs64.exe FakeAppWindow (administrator)") == 1,
          "admin in front, not come: the helper asked at the click and at each try, the dock's own calls after it - the give-up line says so");

    // no helper (not installed, or off): the dock's own calls, and the give-up line says what would help
    Reset();
    Proc(130, L"c:\\fake\\app.exe");
    Proc(131, L"c:\\fake\\obs64.exe");
    Elevate(131);
    app = Win(130, L"App"); obs = Win(131, L"OBS", true);
    SetFront(obs);
    HelperOver(false);
    AppModel n;
    n.Refresh();
    n.Click(*ItemOf(n, app), 0);
    t0 = NowSeconds();
    Check(overCalls == 1 && FrontCallsFor(app) >= 1 && n.watch.overAdmin == 0, "admin in front, no helper: asked, not there - the dock's own calls");
    n.Tick(t0 + 0.3);
    n.Tick(t0 + 0.6);
    n.Tick(t0 + 0.9);
    Check(!n.watch.window && LogCount(L"Windows does not let the dock take the front from a window that runs as administrator - the admin helper can") == 1 &&
          LogCount(L"handed to the admin helper") == 0, "admin in front, no helper: ...the give-up line says the admin helper can");

    // a minimized window: no helper at the click (its restore command first), the helper once it is up
    Reset();
    Proc(132, L"c:\\fake\\app.exe");
    Proc(133, L"c:\\fake\\obs64.exe");
    Elevate(133);
    app = Win(132, L"App"); obs = Win(133, L"OBS", true);
    Edit(app, [](FakeWin& w) { w.iconic = true; w.busy = true; w.restoresMaximized = true; });   // busy: still minimized after the click
    SetFront(obs);
    HelperOver(true);
    AppModel d;
    d.Refresh();
    d.Click(*ItemOf(d, app), 0);
    Check(WaitFor([&] { return State(app).restoreSends == 1; }, 2000) && overCalls == 0 && d.watch.overAdmin == 0,
          "admin in front, minimized: no helper call at the click - its restore command, the dock's own calls");
    Edit(app, [](FakeWin& w) { w.busy = false; });
    Check(WaitFor([&] { return !State(app).iconic && d.watch.restore && d.watch.restore->state == 1; }, 2000), "admin in front, minimized: fixture - its program restored it");
    d.Tick(NowSeconds() + 0.3);
    Check(overCalls == 1 && overFor == app && d.watch.overAdmin == 1, "admin in front, minimized: ...up, at the next look - the helper asked once");
    WaitFor([&] { return inFlight == 0; }, 2000);
    Sleep(100);
    Check(State(app).restoreSends == 1 && State(app).systemRestores == 0 && State(app).zoomed, "admin in front, minimized: ...one restore only (it stays maximized)");

    // Start in front with OBS run as administrator behind it: Esc, then the helper (both of 2026-10-29's causes)
    Reset();
    Proc(134, L"c:\\fake\\app.exe");
    Proc(135, L"c:\\fake\\obs64.exe");
    Elevate(135);
    app = Win(134, L"App"); obs = Win(135, L"OBS", true);
    HWND start = StartMenu(136);
    SetFront(start);
    onKeys = [obs](const std::vector<WORD>& k) { if (k == kEsc) SetFront(obs); };
    HelperOver(true);
    AppModel s;
    s.Refresh();
    s.Click(*ItemOf(s, app), 0);
    t0 = NowSeconds();
    Check(OnlyEsc(1) && overCalls == 0 && FrontCallsFor(app) == 0, "start over admin: the click closes Start first - no helper yet");
    s.Tick(t0 + 0.15);
    Check(OnlyEsc(1) && overCalls == 1 && overFor == app && FrontCallsFor(app) == 0, "start over admin: ...gone, OBS in front - the helper brings it up");
}

// ---------------------------------------------------------------- quiet flashes (QuietFlashes, FlashQuiet)
// A window the dock is bringing up that Windows flashes instead: no attention bounce for it, nor for another window
// of its program (a browser's window and its apps' are one process, clicked by turns), nor for one tried a moment
// before another icon was clicked - for kQuietFlash after each try.
static void QuietFlashTests() {
    Reset();
    Proc(140, L"c:\\fake\\browser.exe");
    Proc(141, L"c:\\fake\\chat.exe");
    Proc(142, L"c:\\fake\\other.exe");
    HWND b1 = Win(140, L"Browser"), b2 = Win(140, L"Browser, another window"), chat = Win(141, L"Chat"), other = Win(142, L"Other", true);
    SetFront(other);   // takesFront false: Windows refuses the dock and flashes the window instead
    AppModel m;
    m.Refresh();
    auto browser = ItemOf(m, b1), chatApp = ItemOf(m, chat);
    Check(browser && chatApp && ItemOf(m, b2) == browser, "quiet flashes: fixture - a browser with two windows, a chat app");
    if (!browser || !chatApp) return;
    m.BringUp(b1, L"test", 1);
    const double now = NowSeconds();
    m.OnShellHook(HSHELL_FLASH, (LPARAM)b2);
    Check(m.attentionEvents.empty() && !browser->attention, "quiet flashes: another window of the program being brought up flashes - no bounce");
    Check(m.FlashQuiet(b1, now + appclick::kQuietFlash - 0.1) && m.FlashQuiet(b2, now + appclick::kQuietFlash - 0.1),
          "quiet flashes: ...the window and its program's others quiet until kQuietFlash after the try");
    Check(!m.FlashQuiet(b1, now + appclick::kQuietFlash + 0.1) && !m.FlashQuiet(b2, now + appclick::kQuietFlash + 0.1), "quiet flashes: ...not after");
    Check(!m.FlashQuiet(chat, now), "quiet flashes: another program's window is not quiet");
    m.OnShellHook(HSHELL_FLASH, (LPARAM)chat);
    Check(m.attentionEvents.size() == 1 && m.attentionEvents.back().id == chatApp->id && chatApp->attention, "quiet flashes: ...its flash bounces its icon");
    for (auto& q : m.quietFlashes) q.until -= appclick::kQuietFlash;   // kQuietFlash later
    m.OnShellHook(HSHELL_FLASH, (LPARAM)b2);
    Check(m.attentionEvents.size() == 2 && m.attentionEvents.back().id == browser->id && browser->attention,
          "quiet flashes: kQuietFlash after the try, the program's other window flashing is its own request - it bounces");

    // two icons clicked by turns: the first one's window is still the dock's doing after the second click
    AppModel n;
    n.Refresh();
    browser = ItemOf(n, b1); chatApp = ItemOf(n, chat);
    for (int i = 0; i < 10; ++i) { n.BringUp(b1, L"test", 1); n.BringUp(chat, L"test", 1); }
    n.OnShellHook(HSHELL_FLASH, (LPARAM)b1);
    n.OnShellHook(HSHELL_FLASH, (LPARAM)b2);
    n.OnShellHook(HSHELL_FLASH, (LPARAM)chat);
    Check(n.attentionEvents.empty() && browser && !browser->attention && chatApp && !chatApp->attention,
          "quiet flashes: two icons by turns - both programs' flashes are the dock's doing (the first window not forgotten at the second click)");
    Check(n.quietFlashes.size() == 2, "quiet flashes: ...ten turns each, one entry per window (the list does not grow)");
    for (auto& q : n.quietFlashes) q.until = NowSeconds() - 1;
    n.BringUp(b1, L"test", 1);
    Check(n.quietFlashes.size() == 1 && n.quietFlashes[0].window == b1, "quiet flashes: entries whose time is over are dropped at the next try");
    n.OnShellHook(HSHELL_FLASH, (LPARAM)chat);
    Check(n.attentionEvents.size() == 1 && n.attentionEvents.back().id == chatApp->id, "quiet flashes: ...and the other icon's window flashing then bounces");
}

// ---------------------------------------------------------------- small helpers
static void HelperTests() {
    Check(FileStem(L"c:\\a\\b.c\\name.ext") == L"name" && FileStem(L"noext") == L"noext" && FileStem(L"c:/x/y.tar.gz") == L"y.tar", "names: the file stem");
    Check(EndsWithI(L"X.LNK", L".lnk") && !EndsWithI(L"lnk", L".lnk") && EndsWithI(L".lnk", L".lnk"), "names: the extension, without case");
    Check(!FileDescription(System32(L"cmd.exe")).empty(), "names: a program's file description");
    Check(FileDescription(L"c:\\hunt-apps_model\\no such program.exe").empty(), "names: none for a missing file");
    // a program without a description (or a file or folder pinned): named as the disk spells it, its path lowercased
    const std::wstring file = ExeDir() + L"\\hunt-apps_model SpelledTool.Exe", folder = ExeDir() + L"\\hunt-apps_model Spelled.Folder";
    { std::ofstream(file.c_str()) << ""; }
    CreateDirectoryW(folder.c_str(), nullptr);
    Check(SpelledName(ToLower(file), L"") == L"hunt-apps_model SpelledTool" && SpelledName(ToLower(folder), L"") == L"hunt-apps_model Spelled.Folder",
          "names: as the disk spells the file (a folder keeps its dots)");
    Check(SpelledName(L"c:\\hunt-apps_model\\gone\\mytool.exe", L"C:\\hunt-apps_model\\Gone\\MyTool.exe") == L"MyTool" &&
          SpelledName(L"c:\\hunt-apps_model\\gone\\mytool.exe", L"shell:AppsFolder\\x") == L"mytool",
          "names: a file not there - as its pin spells the path, else the path");
    DeleteFileW(file.c_str());
    RemoveDirectoryW(folder.c_str());
}

// ================================================================ characterization: what the code does now
// These checks pin today's behaviour, right or wrong, so a refactoring that changes it fails here: the presses'
// outcomes (PollPresses) with their dock.log lines, pins.txt's bytes as earlier versions and this one write them,
// the icon pixels apps.cpp hands to Direct2D, and the two "same app" rules (Refresh, PinFile) that differ on purpose.
static size_t LogMark() { std::lock_guard<std::mutex> lk(logMu); return logLines.size(); }
static std::vector<std::wstring> LogsSince(size_t n) {
    std::lock_guard<std::mutex> lk(logMu);
    return std::vector<std::wstring>(logLines.begin() + std::min(n, logLines.size()), logLines.end());
}
static int CountIn(const std::vector<std::wstring>& lines, const wchar_t* part) {
    int n = 0;
    for (auto& l : lines) n += l.find(part) != std::wstring::npos;
    return n;
}
// lines[i] is the line wanted; on a miss the line there is printed too (UTF-8)
static bool CheckLine(const std::vector<std::wstring>& lines, size_t i, const std::wstring& want, const char* what) {
    const bool ok = i < lines.size() && lines[i] == want;
    if (!ok) std::printf("  got:  %s\n  want: %s\n", i < lines.size() ? ToUtf8(lines[i]).c_str() : "(no line)", ToUtf8(want).c_str());
    return Check(ok, what);
}

// ---------------------------------------------------------------- the presses' outcomes (apps_press.cpp PollPresses)
static void PressOn() { Reset(); g_settings.taskbarAppButtons = true; fakepress::beginWorks = true; }
// The program c:\fake\single.exe (named "single": no file description, no such file) with one window behind
// another program's window in front: a click on it goes to its taskbar button.
static HWND OneWindowBehind(AppModel& m, DWORD pid, HWND* other = nullptr) {
    Proc(pid, L"c:\\fake\\single.exe");
    Proc(pid + 1, L"c:\\fake\\other.exe");
    HWND s = Win(pid, L"Single"), o = Win(pid + 1, L"Other", true);
    SetFront(o);
    m.Refresh();
    if (other) *other = o;
    return s;
}
static bool PressFor(AppModel& m, HWND h, int dockIndex = 0) {
    auto a = ItemOf(m, h);
    if (!a) return false;
    const size_t before = m.presses.size();
    m.Click(*a, dockIndex);
    return m.presses.size() == before + 1;
}
static taskbarapps::Result Res(taskbarapps::Outcome o, const wchar_t* how = L"", HRESULT hr = S_OK, double lookup = 0, double took = 0, int buttons = 0) {
    taskbarapps::Result r;
    r.outcome = o; r.how = how; r.hr = hr; r.lookup = lookup; r.took = took; r.buttons = buttons;
    return r;
}
// the last press's result comes in, `after` seconds after its click: the time PollPresses ran at
static double Answer(AppModel& m, taskbarapps::Result r, double after) {
    if (m.presses.empty()) return -1;
    r.job = m.presses.back().job;
    fakepress::ready[r.job] = r;
    const double t = m.presses.back().clicked + after;
    m.PollPresses(t);
    return t;
}
// the dock's own way at once (ClickWith, its decision anew): the dock brings the window up itself
static bool DockItself(AppModel& m, HWND h, int frontCallsBefore) {
    return m.presses.empty() && m.watch.window == h && !m.watch.byTaskbar && FrontCallsFor(h) > frontCallsBefore;
}

static void PressOutcomeTests() {
    using taskbarapps::Outcome;
    using taskbarapps::Use;
    const HRESULT kTimeout = HRESULT_FROM_WIN32(ERROR_TIMEOUT);   // 800705b4
    // ---- pressed: every field of the watch from the press; the line for the run's first press, then only for a slow one
    {
        PressOn();
        AppModel m;
        HWND o = nullptr;
        HWND s = OneWindowBehind(m, 300, &o);
        HWND bar = NewHandle();
        Check(PressFor(m, s, 2), "pressed (0-3): fixture - a click looked up on the taskbar");
        if (m.presses.empty()) return;
        m.presses[0].clicks = 3;
        m.presses[0].frontWasItsProgram = true;
        const double clicked = m.presses[0].clicked;
        auto r = Res(Outcome::Pressed, L"invoke", S_OK, 0.012, 0.02, 7);
        r.taskbar = bar;
        size_t mark = LogMark();
        const double t = Answer(m, r, 0.05);
        auto lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: single - pressed its button on the taskbar (invoke; found in 12 ms among 7 app buttons, pressed in 20 ms)",
                  "pressed (0-3): the run's first press - its line");
        Check(lines.size() == 1, "pressed (0-3): ...the only line");
        const auto& w = m.watch;
        Check(w.window == s && w.before == o && w.name == L"single" && w.why && !wcscmp(w.why, L"bringing up its window") && w.windows == 1 &&
              w.clicked == clicked && w.clicks == 3, "pressed (0-3): the watch takes the window, the one in front at the click, name, why, windows, click time, clicks");
        Check(w.byTaskbar && !w.wantDown && !w.downAtClick && !w.explorerBusy && w.frontWasItsProgram && w.taskbar == bar && !w.forced && !w.answers,
              "pressed (0-3): ...by the taskbar, up, Explorer not busy, its program in front at the click, the taskbar pressed on");
        Check(w.pressedAt == t && w.next == t + appclick::kLook, "pressed (0-3): ...timed from the answer, looked at after kLook");
        Check(!w.restore && w.right == 0 && w.rightAtClick == 0 && w.refocused == 0 && !w.saidBusy && !w.popup && !w.hadPopup && w.popupClosed == 0 && w.popupEsc == 0 &&
              !w.ghostFront && w.launcherAtClick == -1 && w.frontFirst == -1 && w.frontSecond == -1 && w.frontError == 0 && w.attachError == 0 && w.overAdmin == 0,
              "pressed (0-3): ...nothing of the dock's own watch in it (no restore, right, refocus, pop-up, closed Start or front calls)");
        Check(m.presses.empty() && m.maybes.empty() && m.launchChecks.empty() && m.slowUntil == 0 && m.pressUse.use == Use::Try && m.noButtonUntil.empty(),
              "pressed (0-3): no maybe, no launch check, no pause, nothing learnt yet");
        auto a = ItemOf(m, s);
        Check(a && m.saidPress.count(a->id) && m.saidPress[a->id] == 1 && m.saidFirstPress && FrontCallsFor(s) == 0,
              "pressed (0-3): the outcome kept for the app (Pressed + 1), nothing of the dock's");
        // a quick one: no line; a slow one (lookup + press 0.3 s or more): its line; a press of kExplorerBusy (0.25 s): Explorer busy
        m.watch = {};
        Check(PressFor(m, s), "pressed (0-3): fixture - a second click");
        mark = LogMark();
        Answer(m, Res(Outcome::Pressed, L"toggle", S_OK, 0.01, 0.01, 7), 0.05);
        Check(LogsSince(mark).empty() && m.watch.window == s && m.watch.byTaskbar && !m.watch.explorerBusy, "pressed (0-3): a quick press after the first - watched, no line");
        m.watch = {};
        Check(PressFor(m, s), "pressed (0-3): fixture - a third click");
        mark = LogMark();
        Answer(m, Res(Outcome::Pressed, L"toggle", S_OK, 0.1, 0.25, 7), 0.5);
        lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: single - pressed its button on the taskbar (toggle; found in 100 ms among 7 app buttons, pressed in 250 ms)",
                  "pressed (0-3): a slow press - its line again");
        Check(m.watch.window == s && m.watch.explorerBusy, "pressed (0-3): ...a press call of kExplorerBusy - Explorer taken for busy");
        m.watch = {};
        Check(PressFor(m, s), "pressed (0-3): fixture - a fourth click");
        mark = LogMark();
        Answer(m, Res(Outcome::Pressed, L"invoke", S_OK, 0.1, 0.24, 7), 0.5);
        lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: single - pressed its button on the taskbar (invoke; found in 100 ms among 7 app buttons, pressed in 240 ms)",
                  "pressed (0-3): slow by its lookup - its line");
        Check(m.watch.window == s && !m.watch.explorerBusy, "pressed (0-3): ...a press call under kExplorerBusy - Explorer not busy");
        // the edge: lookup and press 0.3 s on the dot - its line (0.3 or more)
        m.watch = {};
        Check(PressFor(m, s), "pressed (0-3): fixture - a fifth click");
        mark = LogMark();
        Answer(m, Res(Outcome::Pressed, L"invoke", S_OK, 0.3, 0, 7), 0.5);
        lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: single - pressed its button on the taskbar (invoke; found in 300 ms among 7 app buttons, pressed in 0 ms)",
                  "pressed (0-3): lookup and press 0.3 s on the dot - its line");
    }
    // ---- unsure: a maybe, watched as Explorer busy from the click, the pause; a line every time; no answer past kPressAnswer
    {
        PressOn();
        AppModel m;
        HWND s = OneWindowBehind(m, 310);
        Check(PressFor(m, s), "unsure (0-3): fixture");
        if (m.presses.empty()) return;
        const double clicked = m.presses[0].clicked;
        size_t mark = LogMark();
        const double t = Answer(m, Res(Outcome::Unsure, L"toggle", kTimeout), 0.4);
        auto lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: single - unsure whether its taskbar button's press arrived (toggle, 800705b4): nothing more sent for that click, and nothing on top of it for up to 8 s",
                  "unsure (0-3): its line");
        Check(m.maybes.size() == 1 && m.maybes[0].window == s && m.maybes[0].until == clicked + appclick::kBusyWait && !m.maybes[0].wantDown && !m.maybes[0].downAtClick,
              "unsure (0-3): a maybe for the window, until kBusyWait from the click");
        Check(m.watch.window == s && m.watch.byTaskbar && m.watch.explorerBusy && m.watch.pressedAt == clicked && m.watch.next == t + appclick::kLook,
              "unsure (0-3): watched as one Explorer is busy with, timed from the click");
        Check(m.slowUntil == t + taskbarapps::kSlowPause && m.pressUse.use == Use::Try && FrontCallsFor(s) == 0, "unsure (0-3): no presses for kSlowPause, nothing of the dock's");
        m.watch = {}; m.maybes.clear(); m.slowUntil = 0;
        Check(PressFor(m, s), "unsure (0-3): fixture - a second click");
        mark = LogMark();
        Answer(m, Res(Outcome::Unsure, L"invoke", E_FAIL), 0.4);
        lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: single - unsure whether its taskbar button's press arrived (invoke, 80004005): nothing more sent for that click, and nothing on top of it for up to 8 s",
                  "unsure (0-3): the same outcome again - its line again (every time, not once a run)");
        // the worker took the press (Pressing) and no result comes: not abandoned; past kPressAnswer taken for unsure
        m.watch = {}; m.maybes.clear(); m.slowUntil = 0;
        Check(PressFor(m, s), "no answer (0-3): fixture - a third click");
        if (m.presses.empty()) return;
        auto stage = m.presses[0].stage;
        const double c = m.presses[0].clicked;
        *stage = taskbarapps::kPressing;
        mark = LogMark();
        m.PollPresses(c + 1.5);
        m.PollPresses(c + 4.9);
        Check(m.presses.size() == 1 && *stage == taskbarapps::kPressing && LogsSince(mark).empty() && !m.watch.window && m.slowUntil == 0,
              "no answer (0-3): a press under way past kLookupWait - not abandoned, waited for until kPressAnswer");
        m.PollPresses(c + 5.2);
        lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: single - unsure whether its taskbar button's press arrived (no answer from Explorer, 800705b4): nothing more sent for that click, and nothing on top of it for up to 8 s",
                  "no answer (0-3): past kPressAnswer - taken for unsure, its line");
        Check(m.presses.empty() && *stage == taskbarapps::kPressing && m.maybes.size() == 1 && m.maybes[0].until == c + appclick::kBusyWait && m.watch.window == s &&
              m.watch.explorerBusy && m.watch.pressedAt == c && m.slowUntil == (c + 5.2) + taskbarapps::kSlowPause,
              "no answer (0-3): ...a maybe, the watch from the click, the pause; the stage left as it was");
    }
    // ---- the click's wish in the watch and the maybe: a minimize, a restore
    {
        PressOn();
        AppModel m;
        HWND s = OneWindowBehind(m, 320);
        SetFront(s);   // its window is the active one: the click minimizes it
        Check(PressFor(m, s), "unsure, minimize (0-3): fixture");
        Answer(m, Res(Outcome::Unsure, L"toggle", kTimeout), 0.4);
        Check(m.maybes.size() == 1 && m.maybes[0].wantDown && !m.maybes[0].downAtClick && m.watch.window == s && m.watch.wantDown && !m.watch.downAtClick && m.watch.why &&
              !wcscmp(m.watch.why, L"its window is the active one: minimizing it"), "unsure, minimize (0-3): the maybe and the watch want it down");
    }
    {
        PressOn();
        AppModel m;
        HWND s = OneWindowBehind(m, 325);
        Edit(s, [](FakeWin& w) { w.iconic = true; });
        Check(PressFor(m, s), "pressed, restore (0-3): fixture");
        Answer(m, Res(Outcome::Pressed, L"invoke", S_OK, 0.01, 0.01, 3), 0.1);
        Check(m.watch.window == s && !m.watch.wantDown && m.watch.downAtClick && m.watch.why && !wcscmp(m.watch.why, L"restoring its minimized window"),
              "pressed, restore (0-3): the watch wants it up, minimized at the click");
    }
    // ---- a pinned app started by its button: pressed, unsure, counts differ
    {
        PressOn();
        AppModel m;
        m.pressUse.use = Use::Works;
        auto pin = Pinned(m, L"Pinned", L"c:\\fake\\p.exe", L"", L"c:\\fake\\p.exe");
        m.Click(*pin, 2);
        Check(m.presses.size() == 1 && fakepress::begun.size() == 1 && !fakepress::begun[0].running && m.presses[0].d.action == appclick::Action::Launch,
              "launch (0-3): fixture - a pinned app not running, its button looked up for a start");
        size_t mark = LogMark();
        double t = Answer(m, Res(Outcome::Pressed, L"invoke", S_OK, 0.03, 0.04, 5), 0.1);
        auto lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: Pinned - pressed its button on the taskbar (invoke; found in 30 ms among 5 app buttons, pressed in 40 ms)", "launch (0-3): pressed - its line");
        Check(pin->launchStarted == t && pin->launchDock == 2 && pin->launchWindowCount == 0 && m.launchChecks.size() == 1 && m.launchChecks[0].item == pin->id &&
              m.launchChecks[0].at == t && m.launchChecks[0].windows == 0 && !m.launchChecks[0].maybe, "launch (0-3): pressed - the launch bounce, a launch check");
        Check(!m.watch.window && m.maybes.empty() && started.empty() && m.slowUntil == 0, "launch (0-3): pressed - no watch, no maybe, no start of the dock's");
        m.launchChecks.clear();
        pin->launchStarted = 0;
        m.Click(*pin, 1);
        Check(m.presses.size() == 1, "launch (0-3): fixture - a second click");
        mark = LogMark();
        t = Answer(m, Res(Outcome::Unsure, L"invoke", E_FAIL), 0.2);
        lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: Pinned - unsure whether its taskbar button's press arrived (invoke, 80004005): nothing more sent for that click, and no second start for up to 10 s",
                  "launch (0-3): unsure - its line");
        Check(pin->launchStarted == t && pin->launchDock == 1 && m.launchChecks.size() == 1 && m.launchChecks[0].maybe && m.slowUntil == t + taskbarapps::kSlowPause &&
              m.maybes.empty() && !m.watch.window && started.empty(), "launch (0-3): unsure - launched all the same, a launch check that may miss, the pause");
        m.launchChecks.clear();
        m.slowUntil = 0;
        pin->launchStarted = 0;
        m.Click(*pin, 0);
        Check(m.presses.size() == 1, "launch (0-3): fixture - a third click");
        auto r = Res(Outcome::CountDiffers, L"", S_OK, 0.02, 0, 5);
        r.count = 2;
        mark = LogMark();
        t = Answer(m, r, 0.2);
        lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: Pinned - its taskbar button counts 2 windows, the dock 0: the dock does it itself, and looks again after 60 s",
                  "launch (0-3): counts differ - its line (the dock counts 0 for a start)");
        Check(m.noButtonUntil.count(pin->id) && m.noButtonUntil[pin->id] == t + taskbarapps::kNoButtonAgain && started.size() == 1 && started[0].first == L"c:\\fake\\p.exe" &&
              m.launchChecks.empty() && pin->launchStarted > 0, "launch (0-3): counts differ - no lookup for kNoButtonAgain, the dock starts it itself");
    }
    // ---- abandoned: the stage, the pause, the dock's way; its line once a run an app, again after another outcome
    {
        PressOn();
        AppModel m;
        HWND s = OneWindowBehind(m, 330);
        Check(PressFor(m, s), "abandoned (0-3): fixture");
        if (m.presses.empty()) return;
        auto stage = m.presses[0].stage;
        const double c = m.presses[0].clicked;
        const int calls = FrontCallsFor(s);
        const size_t mark = LogMark();
        m.PollPresses(c + 0.9);
        Check(m.presses.size() == 1 && *stage == taskbarapps::kLooking && LogsSince(mark).empty(), "abandoned (0-3): within kLookupWait - waited for");
        m.PollPresses(c + 1.3);
        auto lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: single - its taskbar button was not found and pressed within 1.3 s (Explorer slow to answer): the dock does it itself, and every app click for 30 s",
                  "abandoned (0-3): its line");
        auto a = ItemOf(m, s);
        Check(*stage == taskbarapps::kAbandoned && m.slowUntil == (c + 1.3) + taskbarapps::kSlowPause && DockItself(m, s, calls) && a && m.saidPress[a->id] == 9 &&
              m.pressUse.use == Use::Try && m.maybes.empty(), "abandoned (0-3): the stage abandoned, the pause, the dock's own way at once (kept: Abandoned + 1)");
        auto abandonAgain = [&] {
            m.watch = {}; m.slowUntil = 0;
            if (!PressFor(m, s)) return false;
            const double again = m.presses[0].clicked;
            m.PollPresses(again + 1.3);
            return m.presses.empty() && m.slowUntil == (again + 1.3) + taskbarapps::kSlowPause && m.watch.window == s && !m.watch.byTaskbar;
        };
        Check(abandonAgain() && CountIn(LogsSince(mark), L"was not found and pressed within") == 1, "abandoned (0-3): again - the pause and the dock's way again, no second line");
        m.watch = {}; m.slowUntil = 0;
        Check(PressFor(m, s), "abandoned (0-3): fixture - another outcome in between");
        Answer(m, Res(Outcome::Failed, L"refused", E_ACCESSDENIED), 0.1);
        Check(abandonAgain() && CountIn(LogsSince(mark), L"was not found and pressed within") == 2, "abandoned (0-3): after another outcome for the app - its line again");
    }
    // ---- the dock does it itself: no taskbar, failed (slow or not), counts differ, no button - each line, and what is kept
    {
        PressOn();
        AppModel m;
        HWND s = OneWindowBehind(m, 340);
        auto a = ItemOf(m, s);
        struct Got { double t = -1; std::vector<std::wstring> lines; bool itself = false; };
        auto itself = [&](taskbarapps::Result r) {
            Got g;
            m.watch = {}; m.slowUntil = 0; m.noButtonUntil.clear();
            const int calls = FrontCallsFor(s);
            if (!PressFor(m, s)) return g;
            const size_t mark = LogMark();
            g.t = Answer(m, r, 0.1);
            g.lines = LogsSince(mark);
            g.itself = DockItself(m, s, calls);
            return g;
        };
        auto r = Res(Outcome::NoTaskbar, L"no taskbar", S_OK);
        r.slow = true;   // never a pause for no taskbar
        Got g = itself(r);
        CheckLine(g.lines, 0, L"app click: single - its taskbar button: outcome (no taskbar, 00000000): the dock does it itself", "no taskbar (0-3): its line");
        Check(g.itself && m.slowUntil == 0 && m.noButtonUntil.empty() && m.pressUse.use == Use::Try, "no taskbar (0-3): the dock's way at once, no pause even when slow, nothing learnt");
        r = Res(Outcome::Failed, L"lookup timed out", kTimeout);
        r.slow = true;
        g = itself(r);
        CheckLine(g.lines, 0, L"app click: single - its taskbar button: outcome (lookup timed out, 800705b4): the dock does it itself, and every app click for 30 s",
                  "failed, slow (0-3): its line");
        Check(g.itself && m.slowUntil == g.t + taskbarapps::kSlowPause && m.pressUse.use == Use::Try, "failed, slow (0-3): the dock's way at once, the pause");
        g = itself(Res(Outcome::Failed, L"refused", E_ACCESSDENIED));
        Check(g.lines.empty() && g.itself && m.slowUntil == 0, "failed (0-3): failed again (not slow) - no line (once a run an app), no pause, the dock's way");
        r = Res(Outcome::CountDiffers, L"", S_OK, 0.02, 0, 6);
        r.count = -1;
        g = itself(r);
        CheckLine(g.lines, 0, L"app click: single - its taskbar button counts no window (a pinned button), the dock 1: the dock does it itself, and looks again after 60 s",
                  "counts differ (0-3): its line, a pinned button");
        Check(g.itself && a && m.noButtonUntil.count(a->id) && m.noButtonUntil[a->id] == g.t + taskbarapps::kNoButtonAgain && m.slowUntil == 0,
              "counts differ (0-3): the dock's way, no lookup for kNoButtonAgain");
        r.count = 3;
        g = itself(r);
        Check(g.lines.empty() && g.itself && a && m.noButtonUntil.count(a->id), "counts differ (0-3): again (3 windows) - no line, no lookup for a while again");
        g = itself(Res(Outcome::NoButton, L"", S_OK, 0.02, 0, 4));
        CheckLine(g.lines, 0, L"app click: single - outcome (it was looked for as path c:\\fake\\single.exe; 4 app buttons there): the dock does it itself, and looks again after 60 s",
                  "no button (0-3): its line, looked for by its path");
        Check(g.itself && a && m.noButtonUntil[a->id] == g.t + taskbarapps::kNoButtonAgain && m.saidPress[a->id] == 6 && m.pressUse.use == Use::Try,
              "no button (0-3): the dock's way, no lookup for kNoButtonAgain (kept: NoButton + 1)");
    }
    // ---- the buttons have no press for the run: classic taskbar, no way to press
    {
        PressOn();
        AppModel m;
        HWND s = OneWindowBehind(m, 350);
        Check(PressFor(m, s), "classic (0-3): fixture");
        const int calls = FrontCallsFor(s);
        const size_t mark = LogMark();
        Answer(m, Res(Outcome::Classic, L"classic", S_OK), 0.1);
        auto lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: Windows' classic taskbar has no app buttons to press: the dock's own way for this run", "classic (0-3): its line");
        Check(DockItself(m, s, calls) && m.pressUse.use == Use::Off && m.slowUntil == 0 && m.noButtonUntil.empty(), "classic (0-3): the dock's way at once, and for the run");
        m.watch = {};
        Check(!PressFor(m, s) && fakepress::begun.size() == 1, "classic (0-3): ...the next click looks up nothing");
    }
    {
        PressOn();
        AppModel m;
        HWND s = OneWindowBehind(m, 355);
        Check(PressFor(m, s), "no way (0-3): fixture");
        const int calls = FrontCallsFor(s);
        const size_t mark = LogMark();
        Answer(m, Res(Outcome::NoWay, L"no invoke or toggle", S_OK, 0.012, 0, 7), 0.1);
        auto lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: single - its taskbar button was found (in 12 ms among 7 app buttons) but not pressed: no invoke or toggle. The dock does it itself, "
                            L"and goes its own way for the rest of this run", "no way (0-3): its line");
        Check(DockItself(m, s, calls) && m.pressUse.use == Use::Off && m.slowUntil == 0 && m.noButtonUntil.empty(), "no way (0-3): the dock's way at once, and for the run");
    }
    // ---- looked for as the window's id, the app's and the program: the no-button line names all three, in that order
    {
        PressOn();
        const std::wstring mailExe = L"c:\\program files\\windowsapps\\contoso.mail_2.0.0.0_x64__8wekyb3d8bbwe\\mail.exe";
        Proc(360, mailExe, L"Contoso.Mail_8wekyb3d8bbwe!Calendar");
        Proc(361, L"c:\\fake\\other.exe");
        HWND cal = Win(360, L"Calendar"), o = Win(361, L"Other", true);
        SetFront(o);
        AppModel m;
        auto mail = Pinned(m, L"Mail", L"", L"Contoso.Mail_8wekyb3d8bbwe!Mail", L"shell:AppsFolder\\Contoso.Mail_8wekyb3d8bbwe!Mail");
        m.Refresh();
        Check(ItemOf(m, cal) == mail && PressFor(m, cal), "no button, ids (0-3): fixture - a store app's window under another id of its package");
        if (m.presses.empty()) return;
        Check(m.presses[0].aumid == L"Contoso.Mail_8wekyb3d8bbwe!Calendar" && m.presses[0].otherAumid == L"Contoso.Mail_8wekyb3d8bbwe!Mail" && m.presses[0].exe == mailExe,
              "no button, ids (0-3): looked for as the window's id, the app's id, the window's program");
        const size_t mark = LogMark();
        Answer(m, Res(Outcome::NoButton, L"", S_OK, 0.02, 0, 9), 0.1);
        auto lines = LogsSince(mark);
        CheckLine(lines, 0, L"app click: Mail - outcome (it was looked for as id Contoso.Mail_8wekyb3d8bbwe!Calendar or id Contoso.Mail_8wekyb3d8bbwe!Mail or path " + mailExe +
                            L"; 9 app buttons there): the dock does it itself, and looks again after 60 s", "no button, ids (0-3): its line names the three");
    }
    // ---- the app left the dock before its result: no line, nothing kept for it - the run's own pause and learning all the same
    {
        PressOn();
        AppModel m;
        HWND s = OneWindowBehind(m, 370);
        auto gone = [&](HWND h) {   // its press under way, its app off the dock
            if (!PressFor(m, h)) return -1;
            auto a = ItemOf(m, h);
            m.items.erase(std::find(m.items.begin(), m.items.end(), a));
            return a->id;
        };
        int id = gone(s);
        size_t mark = LogMark();
        Answer(m, Res(Outcome::NoButton, L"", S_OK, 0.02, 0, 2), 0.1);
        Check(id > 0 && m.presses.empty() && LogsSince(mark).empty() && !m.noButtonUntil.count(id) && !m.saidPress.count(id) && !m.watch.window && FrontCallsFor(s) == 0,
              "app gone (0-3): no button - no line, nothing kept, nothing done");
        Proc(372, L"c:\\fake\\second.exe");
        HWND s2 = Win(372, L"Second");
        m.Refresh();
        id = gone(s2);
        const double c = m.presses.empty() ? 0 : m.presses[0].clicked;
        mark = LogMark();
        m.PollPresses(c + 1.3);
        Check(id > 0 && m.presses.empty() && LogsSince(mark).empty() && m.slowUntil == (c + 1.3) + taskbarapps::kSlowPause && !m.saidPress.count(id) && FrontCallsFor(s2) == 0,
              "app gone (0-3): abandoned - no line, the pause all the same");
        m.slowUntil = 0;
        Proc(374, L"c:\\fake\\third.exe");
        HWND s3 = Win(374, L"Third");
        m.Refresh();
        id = gone(s3);
        mark = LogMark();
        Answer(m, Res(Outcome::Classic, L"classic", S_OK), 0.1);
        Check(id > 0 && m.presses.empty() && LogsSince(mark).empty() && m.pressUse.use == taskbarapps::Use::Off && FrontCallsFor(s3) == 0,
              "app gone (0-3): classic - no line, the taskbar's way off all the same");
    }
    // ---- a watch of another window clicked later stays (a maybe all the same); an older one, or the same window's, is replaced
    {
        PressOn();
        AppModel m;
        HWND o = nullptr;
        HWND s = OneWindowBehind(m, 380, &o);
        Check(PressFor(m, s), "watch kept (0-3): fixture");
        if (m.presses.empty()) return;
        double c = m.presses[0].clicked;
        m.watch.window = o; m.watch.clicked = c + 1; m.watch.name = L"kept";
        Answer(m, Res(Outcome::Unsure, L"toggle", kTimeout), 0.3);
        Check(m.watch.window == o && m.watch.name == L"kept" && m.maybes.size() == 1 && m.maybes[0].window == s,
              "watch kept (0-3): another window clicked after it is watched - that watch stays, a maybe for this one all the same");
        m.watch = {}; m.maybes.clear(); m.slowUntil = 0;
        Check(PressFor(m, s), "watch kept (0-3): fixture - a second click");
        c = m.presses.empty() ? 0 : m.presses[0].clicked;
        m.watch.window = o; m.watch.clicked = c - 1; m.watch.name = L"older";
        Answer(m, Res(Outcome::Pressed, L"invoke", S_OK, 0.01, 0.01, 2), 0.1);
        Check(m.watch.window == s && m.watch.name == L"single" && m.watch.byTaskbar, "watch kept (0-3): another window's watch from before the click - replaced");
        m.watch = {};
        Check(PressFor(m, s), "watch kept (0-3): fixture - a third click");
        c = m.presses.empty() ? 0 : m.presses[0].clicked;
        m.watch.window = s; m.watch.clicked = c + 1; m.watch.name = L"same";
        Answer(m, Res(Outcome::Pressed, L"invoke", S_OK, 0.01, 0.01, 2), 0.1);
        Check(m.watch.window == s && m.watch.name == L"single" && m.watch.byTaskbar, "watch kept (0-3): the same window's watch, clicked later - replaced");
        m.watch = {};
        Check(PressFor(m, s), "watch kept (0-3): fixture - a fourth click");
        c = m.presses.empty() ? 0 : m.presses[0].clicked;
        m.watch.window = o; m.watch.clicked = c; m.watch.name = L"as old";
        Answer(m, Res(Outcome::Pressed, L"invoke", S_OK, 0.01, 0.01, 2), 0.1);
        Check(m.watch.window == s && m.watch.name == L"single", "watch kept (0-3): another window's watch clicked at the same moment - replaced (only a later one stays)");
    }
}

// ---------------------------------------------------------------- pins.txt: the bytes earlier versions and this one write
static std::string FileBytes(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static void WriteBytes(const std::wstring& path, const std::string& bytes) { std::ofstream(path.c_str(), std::ios::binary | std::ios::trunc) << bytes; }
static bool CheckBytes(const std::string& got, const std::string& want, const char* what) {
    const bool ok = got == want;
    if (!ok) {
        size_t i = 0;
        while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
        std::printf("  bytes differ at %zu (got %zu bytes, want %zu)\n", i, got.size(), want.size());
    }
    return Check(ok, what);
}

static void PinsGoldenTests() {
    Reset();
    const std::wstring path = DataFile(L"pins.txt");
    // Written by hand as the versions wrote them: the original (0.2.0: a text-mode ofstream, "\n" -> CRLF, fields not
    // cleaned), mod.23 on (CRLF, a tab or line break in a field a space) - and what LoadPins takes besides.
    const std::string old =
        "Google Chrome\t\tc:\\hunt-apps_model\\chrome\\chrome.exe\tC:\\hunt-apps_model\\Pinned\\Google Chrome.lnk\r\n"   // a shortcut's pin
        "\xEB\xA9\x94\xEB\xAA\xA8\xEC\x9E\xA5\t\tc:\\hunt-apps_model\\memo.exe\tC:\\hunt-apps_model\\Memo.exe\r\n"         // a Korean name (UTF-8, no BOM)
        "Notes\tContoso.Notes_8wekyb3d8bbwe!App\t\tshell:AppsFolder\\Contoso.Notes_8wekyb3d8bbwe!App\r\n"                   // a store app: an id, no path
        "Upper Tool\t\tC:\\Hunt-Apps_Model\\UPPER.EXE\tC:\\Hunt-Apps_Model\\UPPER.EXE\r\n"                                  // a path not lowercased
        "new\t\tC:\\Hunt-Apps_Model\\NEW.exe\tC:\\Hunt-Apps_Model\\New.exe\r\n"                                             // before mod.23: the lowercased stem
        "tool\t\tc:\\hunt-apps_model\\tool.exe\tC:\\hunt-apps_model\\tool.lnk\r\n"                                          // ...but a shortcut's own name
        "LF only\t\tc:\\hunt-apps_model\\lf.exe\tc:\\hunt-apps_model\\lf.exe\n"                                             // a bare LF
        "Five\t\tc:\\hunt-apps_model\\five.exe\tc:\\hunt-apps_model\\five.exe\tfifth field\r\n"                             // a fifth field
        "one field only\r\n"
        "two\tfields\r\n"
        "three\tfields\there\r\n"
        "\r\n"
        "\n"
        "Tab\tin name\t\tc:\\hunt-apps_model\\tab.exe\tc:\\hunt-apps_model\\tab.exe\r\n"                                    // 0.2.0: a tab in a name shifted the fields
        "Line\r\nbreak\t\tc:\\hunt-apps_model\\line.exe\tc:\\hunt-apps_model\\line.exe\r\n"                                 // 0.2.0: a line break split the line
        "\t\tc:\\hunt-apps_model\\Nameless.exe\tC:\\hunt-apps_model\\Nameless.exe\r\n"                                      // no name
        "Trailing\t\tc:\\hunt-apps_model\\trail.exe\t\r\n"                                                                  // an empty fourth field
        " Spaced \t\tc:\\hunt-apps_model\\spaced.exe\tc:\\hunt-apps_model\\spaced.exe\r\n"                                  // spaces kept
        "Bad\xFF" "byte\t\tc:\\hunt-apps_model\\bad.exe\tc:\\hunt-apps_model\\bad.exe\r\n"                                  // not UTF-8
        "CR\rinside\t\tc:\\hunt-apps_model\\cr.exe\tc:\\hunt-apps_model\\cr.exe\r\n"                                        // a lone CR in a name
        "Last\t\tc:\\hunt-apps_model\\last.exe\tc:\\hunt-apps_model\\last.exe";                                             // no line end
    WriteBytes(path, old);
    AppModel m;
    m.LoadPins();
    struct Want { const wchar_t *name, *aumid, *exe, *launch; };
    const Want want[] = {
        { L"Google Chrome", L"", L"c:\\hunt-apps_model\\chrome\\chrome.exe", L"C:\\hunt-apps_model\\Pinned\\Google Chrome.lnk" },
        { L"메모장", L"", L"c:\\hunt-apps_model\\memo.exe", L"C:\\hunt-apps_model\\Memo.exe" },
        { L"Notes", L"Contoso.Notes_8wekyb3d8bbwe!App", L"", L"shell:AppsFolder\\Contoso.Notes_8wekyb3d8bbwe!App" },
        { L"Upper Tool", L"", L"c:\\hunt-apps_model\\upper.exe", L"C:\\Hunt-Apps_Model\\UPPER.EXE" },
        { L"New", L"", L"c:\\hunt-apps_model\\new.exe", L"C:\\Hunt-Apps_Model\\New.exe" },
        { L"tool", L"", L"c:\\hunt-apps_model\\tool.exe", L"C:\\hunt-apps_model\\tool.lnk" },
        { L"LF only", L"", L"c:\\hunt-apps_model\\lf.exe", L"c:\\hunt-apps_model\\lf.exe" },
        { L"Five", L"", L"c:\\hunt-apps_model\\five.exe", L"c:\\hunt-apps_model\\five.exe" },
        { L"Tab", L"in name", L"", L"c:\\hunt-apps_model\\tab.exe" },
        { L"break", L"", L"c:\\hunt-apps_model\\line.exe", L"c:\\hunt-apps_model\\line.exe" },
        { L"Nameless", L"", L"c:\\hunt-apps_model\\nameless.exe", L"C:\\hunt-apps_model\\Nameless.exe" },
        { L"Trailing", L"", L"c:\\hunt-apps_model\\trail.exe", L"" },
        { L" Spaced ", L"", L"c:\\hunt-apps_model\\spaced.exe", L"c:\\hunt-apps_model\\spaced.exe" },
        { L"Bad\xFFFD" L"byte", L"", L"c:\\hunt-apps_model\\bad.exe", L"c:\\hunt-apps_model\\bad.exe" },
        { L"CR\rinside", L"", L"c:\\hunt-apps_model\\cr.exe", L"c:\\hunt-apps_model\\cr.exe" },
        { L"Last", L"", L"c:\\hunt-apps_model\\last.exe", L"c:\\hunt-apps_model\\last.exe" },
    };
    const size_t n = sizeof(want) / sizeof(want[0]);
    Check(m.items.size() == n, "pins golden (0-3): 16 pins of the 22 lines (lines of fewer than four fields, empty ones and a split name's first half left out)");
    for (size_t i = 0; i < n && i < m.items.size(); ++i) {
        const AppItem& a = *m.items[i];
        char what[160];
        std::snprintf(what, sizeof(what), "pins golden (0-3): pin %zu - name, id, path and launch as read (path lowercased, a name made of it spelled again)", i + 1);
        const bool ok = a.name == want[i].name && a.aumid == want[i].aumid && a.exe == want[i].exe && a.launch == want[i].launch && a.pinned && a.windows.empty();
        if (!ok) std::printf("  got: [%s] [%s] [%s] [%s]\n", ToUtf8(a.name).c_str(), ToUtf8(a.aumid).c_str(), ToUtf8(a.exe).c_str(), ToUtf8(a.launch).c_str());
        Check(ok, what);
    }
    Check(FileBytes(path) == old, "pins golden (0-3): reading leaves pins.txt as it was");
    m.SavePins();
    const std::string again =
        "Google Chrome\t\tc:\\hunt-apps_model\\chrome\\chrome.exe\tC:\\hunt-apps_model\\Pinned\\Google Chrome.lnk\r\n"
        "\xEB\xA9\x94\xEB\xAA\xA8\xEC\x9E\xA5\t\tc:\\hunt-apps_model\\memo.exe\tC:\\hunt-apps_model\\Memo.exe\r\n"
        "Notes\tContoso.Notes_8wekyb3d8bbwe!App\t\tshell:AppsFolder\\Contoso.Notes_8wekyb3d8bbwe!App\r\n"
        "Upper Tool\t\tc:\\hunt-apps_model\\upper.exe\tC:\\Hunt-Apps_Model\\UPPER.EXE\r\n"
        "New\t\tc:\\hunt-apps_model\\new.exe\tC:\\Hunt-Apps_Model\\New.exe\r\n"
        "tool\t\tc:\\hunt-apps_model\\tool.exe\tC:\\hunt-apps_model\\tool.lnk\r\n"
        "LF only\t\tc:\\hunt-apps_model\\lf.exe\tc:\\hunt-apps_model\\lf.exe\r\n"
        "Five\t\tc:\\hunt-apps_model\\five.exe\tc:\\hunt-apps_model\\five.exe\r\n"
        "Tab\tin name\t\tc:\\hunt-apps_model\\tab.exe\r\n"
        "break\t\tc:\\hunt-apps_model\\line.exe\tc:\\hunt-apps_model\\line.exe\r\n"
        "Nameless\t\tc:\\hunt-apps_model\\nameless.exe\tC:\\hunt-apps_model\\Nameless.exe\r\n"
        "Trailing\t\tc:\\hunt-apps_model\\trail.exe\t\r\n"
        " Spaced \t\tc:\\hunt-apps_model\\spaced.exe\tc:\\hunt-apps_model\\spaced.exe\r\n"
        "Bad\xEF\xBF\xBD" "byte\t\tc:\\hunt-apps_model\\bad.exe\tc:\\hunt-apps_model\\bad.exe\r\n"
        "CR inside\t\tc:\\hunt-apps_model\\cr.exe\tc:\\hunt-apps_model\\cr.exe\r\n"
        "Last\t\tc:\\hunt-apps_model\\last.exe\tc:\\hunt-apps_model\\last.exe\r\n";
    CheckBytes(FileBytes(path), again, "pins golden (0-3): written again - four fields, CRLF, lowercased paths, names spelled again, a bad byte U+FFFD, a CR a space");
    AppModel back;
    back.LoadPins();
    bool same = back.items.size() == n;
    for (size_t i = 0; same && i < n; ++i) same = back.items[i]->name == (i == 14 ? std::wstring(L"CR inside") : std::wstring(want[i].name)) && back.items[i]->exe == want[i].exe;
    Check(same, "pins golden (0-3): ...and read back as the same pins");
    // a byte order mark (a pins.txt saved by an editor): kept in the first name, and written again
    const std::string bom = "\xEF\xBB\xBF" "Bom\t\tc:\\hunt-apps_model\\bom.exe\tc:\\hunt-apps_model\\bom.exe\r\n";
    WriteBytes(path, bom);
    AppModel b;
    b.LoadPins();
    Check(b.items.size() == 1 && b.items[0]->name == L"\xFEFF" L"Bom" && b.items[0]->exe == L"c:\\hunt-apps_model\\bom.exe", "pins golden (0-3): a BOM stays in the first name");
    b.SavePins();
    CheckBytes(FileBytes(path), bom, "pins golden (0-3): ...and is written again");
    // this version's writer: only pinned apps, in order; tab, CR and LF in any field a space; nothing else changed
    AppModel w;
    Pinned(w, L"A\tB", L"c:\\x\ty.exe", L"Id\r\n", L"L\nx");
    auto running = w.NewItem();
    running->name = L"Running"; running->exe = L"c:\\fake\\running.exe"; running->windows = { NewHandle() };
    w.items.push_back(running);
    Pinned(w, L"Store", L"", L"Contoso.App_8wekyb3d8bbwe!App", L"shell:AppsFolder\\Contoso.App_8wekyb3d8bbwe!App");
    Pinned(w, L"메모장", L"C:\\Mixed\\Case.exe", L"", L"");
    w.SavePins();
    CheckBytes(FileBytes(path),
               "A B\tId  \tc:\\x y.exe\tL x\r\n"
               "Store\tContoso.App_8wekyb3d8bbwe!App\t\tshell:AppsFolder\\Contoso.App_8wekyb3d8bbwe!App\r\n"
               "\xEB\xA9\x94\xEB\xAA\xA8\xEC\x9E\xA5\t\tC:\\Mixed\\Case.exe\t\r\n",
               "pins golden (0-3): the writer - pinned only, name, id, path, launch, tab/CR/LF a space, the path as held");
    AppModel none;
    none.SavePins();
    Check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && FileBytes(path).empty(), "pins golden (0-3): no pins - an empty pins.txt (0 bytes)");
}

// ---------------------------------------------------------------- icon pixels handed to Direct2D (apps.cpp)
// Bitmaps and icons made here, of this test's own: 3 x 2 pixels, rows top to bottom, 0xAARRGGBB.
static HBITMAP Dib(int w, int h, int bpp, bool topDown, const std::vector<uint32_t>& px) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = topDown ? -h : h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = (WORD)bpp; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm || !bits) return hbm;
    const int stride = ((w * bpp + 31) / 32) * 4;
    for (int y = 0; y < h; ++y) {
        BYTE* row = (BYTE*)bits + stride * (topDown ? y : h - 1 - y);
        for (int x = 0; x < w; ++x) {
            const uint32_t p = px[y * w + x];
            if (bpp == 32) memcpy(row + x * 4, &p, 4);
            else { row[x * 3] = BYTE(p); row[x * 3 + 1] = BYTE(p >> 8); row[x * 3 + 2] = BYTE(p >> 16); }
        }
    }
    GdiFlush();
    return hbm;
}
static HBITMAP Ddb(int w, int h, const std::vector<uint32_t>& px) { return CreateBitmap(w, h, 1, 32, px.data()); }   // not a DIB section
static HBITMAP MaskBits(int w, int h, std::initializer_list<int> set) {   // 1 bpp, rows padded to 16 bits; set: the pixels whose bit is 1
    const int stride = ((w + 15) / 16) * 2;
    std::vector<BYTE> b(stride * h, 0);
    for (int i : set) b[(i / w) * stride + (i % w) / 8] |= BYTE(0x80 >> ((i % w) % 8));
    return CreateBitmap(w, h, 1, 1, b.data());
}
static HICON MakeIcon(HBITMAP color, HBITMAP mask) {
    ICONINFO ii{};
    ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON i = CreateIconIndirect(&ii);
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return i;
}
// the bitmap made last (index i): its size, a pitch of 4 bytes a pixel, premultiplied BGRA at 96 DPI, and these pixels
static bool MadeAs(size_t i, UINT32 w, UINT32 h, const std::vector<uint32_t>& px) {
    if (i >= iconrec::made.size()) { std::printf("  no bitmap made (%zu so far)\n", iconrec::made.size()); return false; }
    const auto& m = iconrec::made[i];
    const bool ok = m.w == w && m.h == h && m.pitch == w * 4 && m.format == DXGI_FORMAT_B8G8R8A8_UNORM && m.alpha == D2D1_ALPHA_MODE_PREMULTIPLIED &&
                    m.options == D2D1_BITMAP_OPTIONS_NONE && m.dpiX == 96.0f && m.dpiY == 96.0f && m.px == px;
    if (!ok) {
        std::printf("  got %ux%u pitch %u format %d alpha %d options %d dpi %.1f,%.1f:", m.w, m.h, m.pitch, (int)m.format, (int)m.alpha, (int)m.options, m.dpiX, m.dpiY);
        for (auto p : m.px) std::printf(" %08x", p);
        std::printf("\n");
    }
    return ok;
}

static void IconPixelTests() {
    Reset();
    using V = std::vector<uint32_t>;
    const V straight = { 0xFF102030, 0x80FF8040, 0x00FFFFFF, 0x40804020, 0x01FFFFFF, 0xC80A0A0A };       // straight alpha
    const V premul = { 0xFF102030, 0x80804020, 0x00000000, 0x40201008, 0x01010101, 0xC8070707 };          // ...premultiplied: c * a / 255, rounded down
    const V noAlpha = { 0x00102030, 0x00FF8040, 0x00FFFFFF, 0x00804020, 0x00000000, 0x000A0A0A };
    const V opaque = { 0xFF102030, 0xFFFF8040, 0xFFFFFFFF, 0xFF804020, 0xFF000000, 0xFF0A0A0A };
    const V mixed = { 0x00FFFFFF, 0x80404040, 0xFFFFFFFF, 0x10101010, 0x20000000, 0x7F7F7F7F };           // one pixel not premultiplied (a 0, colour)
    const V mixedOut = { 0x00000000, 0x80202020, 0xFFFFFFFF, 0x10010101, 0x20000000, 0x7F3F3F3F };       // ...every pixel premultiplied
    const V twice = { 0xFF102030, 0x80402010, 0x00000000, 0x40080402, 0x01000000, 0xC8050505 };          // premul premultiplied again
    auto bitmap = [](HBITMAP hbm) { auto b = BitmapFromHBITMAP(hbm); if (hbm) DeleteObject(hbm); return b; };
    auto icon = [](HICON hi) { auto b = IconFromHICON(hi); if (hi) DestroyIcon(hi); return b; };
    // the rows of a 3 x 2 bitmap the other way up: GetObject gives a top-down DIB section's height as positive (checked on
    // Windows 11 26H2), so BitmapFromHBITMAP reads one as bottom-up - upside down (kept as it is: what the code does now)
    auto flip = [](const V& v) { return V{ v[3], v[4], v[5], v[0], v[1], v[2] }; };
    // BitmapFromHBITMAP, a 32 bpp DIB section (the shell's image factory gives one)
    size_t at = iconrec::made.size();
    bitmap(Dib(3, 2, 32, false, noAlpha));
    Check(MadeAs(at, 3, 2, opaque), "icon pixels (0-3): a bottom-up 32 bpp DIB section without alpha - opaque, colours as they are, top row first");
    at = iconrec::made.size();
    bitmap(Dib(3, 2, 32, true, noAlpha));
    Check(MadeAs(at, 3, 2, flip(opaque)), "icon pixels (0-3): a top-down 32 bpp DIB section without alpha - opaque, its rows upside down");
    at = iconrec::made.size();
    bitmap(Dib(3, 2, 32, false, straight));
    Check(MadeAs(at, 3, 2, premul), "icon pixels (0-3): a bottom-up DIB section with straight alpha - top row first, premultiplied (rounded down)");
    at = iconrec::made.size();
    bitmap(Dib(3, 2, 32, true, premul));
    Check(MadeAs(at, 3, 2, flip(premul)), "icon pixels (0-3): a top-down DIB section already premultiplied (no colour over its alpha) - left as it is, upside down");
    at = iconrec::made.size();
    bitmap(Dib(3, 2, 32, false, premul));
    Check(MadeAs(at, 3, 2, premul), "icon pixels (0-3): a bottom-up DIB section already premultiplied - left as it is");
    at = iconrec::made.size();
    bitmap(Dib(3, 2, 32, false, mixed));
    Check(MadeAs(at, 3, 2, mixedOut), "icon pixels (0-3): one pixel's colour over its alpha - every pixel premultiplied, the premultiplied ones too");
    at = iconrec::made.size();
    bitmap(Dib(3, 2, 32, true, mixed));
    Check(MadeAs(at, 3, 2, flip(mixedOut)), "icon pixels (0-3): ...the same, top-down - upside down");
    // ...not a 32 bpp DIB section: through GetDIBits - opaque without alpha, straight alpha left straight (no premultiply)
    at = iconrec::made.size();
    bitmap(Dib(3, 2, 24, false, noAlpha));
    Check(MadeAs(at, 3, 2, opaque), "icon pixels (0-3): a 24 bpp DIB section - through GetDIBits, opaque");
    at = iconrec::made.size();
    bitmap(Ddb(3, 2, noAlpha));
    Check(MadeAs(at, 3, 2, opaque), "icon pixels (0-3): a 32 bpp bitmap that is no DIB section, without alpha - opaque");
    at = iconrec::made.size();
    bitmap(Ddb(3, 2, straight));
    Check(MadeAs(at, 3, 2, straight), "icon pixels (0-3): ...with straight alpha - handed on not premultiplied (unlike a DIB section)");
    at = iconrec::made.size();
    Check(!bitmap(nullptr) && iconrec::made.size() == at, "icon pixels (0-3): no bitmap - none made");
    // IconFromHICON: the mask's rule when the colour has no alpha, then always premultiplied
    at = iconrec::made.size();
    icon(MakeIcon(Dib(3, 2, 32, true, straight), MaskBits(3, 2, { 2, 4 })));
    Check(MadeAs(at, 3, 2, premul), "icon pixels (0-3): an icon with alpha - its alpha (the mask not looked at), premultiplied");
    at = iconrec::made.size();
    icon(MakeIcon(Dib(3, 2, 32, true, premul), MaskBits(3, 2, {})));
    Check(MadeAs(at, 3, 2, twice), "icon pixels (0-3): an icon premultiplied already - premultiplied again (unlike a DIB section)");
    at = iconrec::made.size();
    icon(MakeIcon(Dib(3, 2, 32, true, noAlpha), MaskBits(3, 2, { 1, 3 })));
    Check(MadeAs(at, 3, 2, V{ 0xFF102030, 0x00000000, 0xFFFFFFFF, 0x00000000, 0xFF000000, 0xFF0A0A0A }),
          "icon pixels (0-3): an icon without alpha - the mask's set bits transparent (colour gone), the rest opaque");
    at = iconrec::made.size();
    icon(MakeIcon(Dib(3, 2, 24, true, noAlpha), MaskBits(3, 2, { 0, 5 })));
    Check(MadeAs(at, 3, 2, V{ 0x00000000, 0xFFFF8040, 0xFFFFFFFF, 0xFF804020, 0xFF000000, 0x00000000 }), "icon pixels (0-3): an icon of a 24 bpp colour bitmap - the mask's rule too");
    at = iconrec::made.size();
    HICON mono = MakeIcon(nullptr, MaskBits(3, 4, { 0, 1, 9 }));   // a monochrome icon: AND mask over XOR mask, no colour bitmap
    Check(mono && !icon(mono) && iconrec::made.size() == at, "icon pixels (0-3): a monochrome icon (no colour bitmap) - no bitmap made");
    Check(!IconFromHICON(nullptr) && iconrec::made.size() == at, "icon pixels (0-3): no icon - none made");
    // IconFromFile: the largest icon (256) of the index asked, through IconFromHICON
    extractGives = MakeIcon(Dib(3, 2, 32, true, straight), MaskBits(3, 2, {}));
    at = iconrec::made.size();
    IconFromFile(L"c:\\hunt-apps_model\\icon.exe", 5);
    Check(extractAsked.size() == 1 && extractAsked[0].file == L"c:\\hunt-apps_model\\icon.exe" && extractAsked[0].index == 5 && extractAsked[0].cx == 256 &&
          extractAsked[0].cy == 256 && extractAsked[0].count == 1 && MadeAs(at, 3, 2, premul), "icon pixels (0-3): a file's icon - asked at 256 for its index, made as an icon");
    IconFromFile(L"", 0);
    Check(extractAsked.size() == 1, "icon pixels (0-3): no file - nothing asked");
    DestroyIcon(extractGives);
    extractGives = nullptr;
}

// ---------------------------------------------------------------- "the same app": Refresh and PinFile answer differently
// Refresh groups a window by its id first over all apps, then its package, then its program while one of the two has
// no id; PinFile takes the first app that has the id or is the program while one has none, and knows no package.
static void SameAppRuleTests() {
    Reset();
    const std::wstring cmd = System32(L"cmd.exe");
    const std::wstring lnk = ExeDir() + L"\\hunt-apps_model 0-3 Profile 1.lnk", cal = ExeDir() + L"\\hunt-apps_model 0-3 Calendar.lnk";
    Check(MakeShortcut(lnk, cmd, L"/k title p1", L"Hunt.Fake.Profile1") && MakeShortcut(cal, cmd, L"/k title cal", L"Contoso.Mail_8wekyb3d8bbwe!Calendar"),
          "same app (0-3): fixture shortcuts with ids of their own");
    Proc(390, cmd);
    HWND w = Win(390, L"Profile 1");
    Edit(w, [](FakeWin& f) { f.aumid = L"Hunt.Fake.Profile1"; });
    {
        AppModel r;
        auto x = Pinned(r, L"X", cmd, L"", cmd);
        auto y = Pinned(r, L"Y", cmd, L"Hunt.Fake.Profile1", cmd);
        r.Refresh();
        Check(ItemOf(r, w) == y && x->windows.empty(), "same app (0-3): Refresh - a window with an id goes to the app of that id (Y), not the program's app without one listed first (X)");
    }
    {
        AppModel p;
        auto x = Pinned(p, L"X", cmd, L"", cmd);
        auto y = Pinned(p, L"Y", cmd, L"Hunt.Fake.Profile1", cmd);
        const int at = p.PinFile(lnk, 2);
        Check(p.items.size() == 2 && Names(p) == L"Y,X" && at == 1 && x->launch == cmd && y->launch == cmd,
              "same app (0-3): PinFile - a shortcut with that id moves the program's first app without one (X), not the app of the id (Y)");
    }
    Proc(391, cmd);
    HWND cw = Win(391, L"Calendar");
    Edit(cw, [](FakeWin& f) { f.aumid = L"Contoso.Mail_8wekyb3d8bbwe!Calendar"; });
    {
        AppModel r;
        auto mail = Pinned(r, L"Mail", L"", L"Contoso.Mail_8wekyb3d8bbwe!Mail", L"shell:AppsFolder\\Contoso.Mail_8wekyb3d8bbwe!Mail");
        r.Refresh();
        Check(ItemOf(r, cw) == mail, "same app (0-3): Refresh - a window with another id of a package goes to the package's app");
    }
    {
        AppModel p;
        auto mail = Pinned(p, L"Mail", L"", L"Contoso.Mail_8wekyb3d8bbwe!Mail", L"shell:AppsFolder\\Contoso.Mail_8wekyb3d8bbwe!Mail");
        const int at = p.PinFile(cal, 1);
        Check(p.items.size() == 2 && at == 1 && p.items[0] == mail && p.items[1]->aumid == L"Contoso.Mail_8wekyb3d8bbwe!Calendar" && p.items[1]->launch == cal &&
              p.items[1]->pinned, "same app (0-3): PinFile - a shortcut with another id of that package is a pin of its own");
    }
    DeleteFileW(lnk.c_str());
    DeleteFileW(cal.c_str());
}

// ---------------------------------------------------------------- the press's watch, judged (apps_press.cpp CheckPress)
// The verdicts' lines and what each leaves behind. The watch is set as PollPresses leaves it, its times on a made-up
// clock (CheckPress is handed its now): clicked at kT, the press answered at kT + 0.5.
static constexpr double kT = 1000.0;
static void WatchPress(AppModel& m, HWND h, HWND before, HWND bar) {
    m.watch = {};
    auto& w = m.watch;
    w.window = h; w.before = before; w.name = L"single"; w.why = L"bringing up its window"; w.windows = 2;
    w.clicked = kT; w.pressedAt = kT + 0.5; w.next = kT + 0.75; w.byTaskbar = true; w.taskbar = bar;
}
static std::shared_ptr<std::atomic<int>> Answers(int v) { return std::make_shared<std::atomic<int>>(v); }

static void CheckPressTests() {
    using taskbarapps::Verdict;
    using taskbarapps::Use;
    const std::wstring works = L"app click: the taskbar's app buttons work: apps with one window, and pinned apps not running, go through them from now on";
    PressOn();
    AppModel m;
    HWND o = nullptr;
    HWND s = OneWindowBehind(m, 500, &o);
    Proc(502, L"c:\\fake\\explorer.exe");
    HWND bar = Win(502, L"");   // the taskbar it was pressed on: a fake window (asked whether it answers - never Explorer's)
    Proc(503, L"c:\\fake\\third.exe");
    HWND third = Win(503, L"Third");
    HWND s2 = Win(500, L"Single 2");   // another window of its program
    Proc(504, L"c:\\fake\\taskbar.exe");
    HWND tray = Win(504, L"tray");
    Edit(tray, [](FakeWin& f) { f.cls = L"Shell_TrayWnd"; });   // only the fake table's class text: no window of that class is made
    const double later = NowSeconds() + 100;   // a maybe that has not run out (BringUp asks it at the real time)
    std::vector<std::wstring> lines;
    auto run = [&](double now) {
        const size_t mark = LogMark();
        const Verdict v = m.CheckPress(now);
        lines = LogsSince(mark);
        return v;
    };
    // ---- done: it came up - the run's first press that worked says so; its own line only over 1 s from the click
    WatchPress(m, s, o, bar);
    SetFront(s);
    m.maybes = { { s, kT + 50, false, false }, { third, kT + 50, false, false } };
    auto handled = std::make_shared<RestoreJob>();
    handled->state = 1; handled->handledAt = kT;
    auto waiting = std::make_shared<RestoreJob>();
    m.restores = { { s, handled }, { s, waiting }, { o, handled } };
    Verdict v = run(kT + 0.9);
    Check(v == Verdict::Done && !m.watch.window, "check press (0-3): done - it came up, the watch ends");
    CheckLine(lines, 0, works, "check press (0-3): done - the run's first press that worked: its line");
    Check(lines.size() == 1, "check press (0-3): done 0.9 s after the click - no line of its own");
    Check(m.pressUse.use == Use::Works && m.lastUp.window == s && m.lastUp.at == kT + 0.9, "check press (0-3): done - learnt: works; the window that came up, at now");
    Check(m.maybes.size() == 1 && m.maybes[0].window == third, "check press (0-3): done - its maybe forgotten, another window's kept");
    Check(m.restores.size() == 2 && m.restores[0].second == waiting && m.restores[1].first == o,
          "check press (0-3): done - its handled restore command forgotten, its unanswered one and another window's kept");
    // ...1.1 s from the click but 0.6 s from the answer: its line, timed from the click; Explorer busy; works already - no second
    WatchPress(m, s, o, bar);
    m.watch.explorerBusy = true;
    v = run(kT + 1.1);
    CheckLine(lines, 0, L"app click: single came up after 1.1 s, by its taskbar button (Explorer was busy)",
              "check press (0-3): done 1.1 s after the click (0.6 s after the answer) - its line, timed from the click");
    Check(v == Verdict::Done && lines.size() == 1, "check press (0-3): done - the buttons' line only once (learnt already)");
    // ...a minimize that went down, forced: no window that came up, nothing learnt
    m.pressUse = {};
    m.lastUp.window = nullptr; m.lastUp.at = -1e9;
    m.restores = { { s, handled } };
    WatchPress(m, s, o, bar);
    m.watch.wantDown = true; m.watch.forced = true;
    Edit(s, [](FakeWin& f) { f.iconic = true; });
    SetFront(o);
    v = run(kT + 1.5);
    CheckLine(lines, 0, L"app click: single went down after 1.5 s, by its taskbar button", "check press (0-3): done, went down - its line");
    Check(v == Verdict::Done && lines.size() == 1 && m.pressUse.use == Use::Try && !m.lastUp.window && m.restores.size() == 1,
          "check press (0-3): done, went down, forced - nothing learnt, no window come up, its restores kept");
    // ---- toggled: a bring-up that went down - learnt even when forced
    WatchPress(m, s, o, bar);
    m.watch.forced = true;
    m.maybes = { { s, kT + 50, false, false } };
    v = run(kT + 0.7);
    Check(v == Verdict::Toggled && !m.watch.window && m.maybes.empty() && m.pressUse.use == Use::Works, "check press (0-3): toggled - learnt (forced too), its maybe forgotten");
    CheckLine(lines, 0, works, "check press (0-3): toggled - the buttons' line first");
    CheckLine(lines, 1, L"app click: single - its taskbar button minimized it instead of bringing it up (Windows took it for the active app; in front: other.exe FakeAppWindow): left so",
              "check press (0-3): toggled - its line");
    Edit(s, [](FakeWin& f) { f.iconic = false; });
    // ---- behind: restored, not in front - the dock brings it up itself (no restore command); timed from the answer
    auto behind = [&](bool busy, bool forced) {
        WatchPress(m, s, o, bar);
        m.watch.downAtClick = true; m.watch.explorerBusy = busy; m.watch.forced = forced;
        m.maybes = { { s, later, false, false } };
        m.restores.clear();
        SetFront(o);
        const int calls = FrontCallsFor(s);
        const Verdict b = run(kT + 1.8);
        return b == Verdict::Behind && m.watch.window == s && !m.watch.byTaskbar && m.watch.why && !wcscmp(m.watch.why, L"bringing up its window") &&
               m.watch.windows == 2 && FrontCallsFor(s) > calls && m.restores.empty() && m.maybes.empty();
    };
    m.pressUse = {};
    Check(behind(false, false), "check press (0-3): behind - its maybe forgotten, the dock brings it up (its why and windows), no restore command");
    CheckLine(lines, 0, L"app click: single - its taskbar button restored it, but Windows did not put it in front in 1.3 s (in front: other.exe FakeAppWindow): the dock does that itself",
              "check press (0-3): behind - its line, timed from the answer");
    Check(lines.size() == 1 && m.pressUse.use == Use::Try && m.pressUse.duds == 1, "check press (0-3): behind - one toward two in a row");
    m.pressUse = {};
    Check(behind(true, false), "check press (0-3): behind, Explorer busy - the dock brings it up");
    CheckLine(lines, 0, L"app click: single - its taskbar button restored it, but Windows did not put it in front in 1.3 s (in front: other.exe FakeAppWindow; Explorer was busy): the dock does that itself",
              "check press (0-3): behind, Explorer busy - its line");
    Check(m.pressUse.duds == 0, "check press (0-3): behind, Explorer busy - nothing learnt");
    m.pressUse = {};
    Check(behind(false, true) && m.pressUse.duds == 0, "check press (0-3): behind, forced - nothing learnt");
    m.pressUse.use = Use::Works; m.pressUse.duds = 1;
    Check(behind(false, false) && m.pressUse.use == Use::Off, "check press (0-3): behind, the second in a row - the taskbar's way off");
    CheckLine(lines, 0, L"app click: single - its taskbar button restored it, but Windows did not put it in front in 1.3 s (in front: other.exe FakeAppWindow): the dock does that "
                        L"itself; apps go the dock's own way for the rest of this run", "check press (0-3): behind, turned off - its line says so");
    // ---- stop: gone, or another program in front; the taskbar's own window in front is not the user going on
    m.pressUse = {};
    HWND dead = Win(500, L"Dead");
    WatchPress(m, dead, o, bar);
    m.maybes = { { dead, kT + 50, false, false } };
    Edit(dead, [](FakeWin& f) { f.alive = false; });
    v = run(kT + 0.7);
    Check(v == Verdict::Stop && !m.watch.window && m.maybes.empty() && lines.empty(), "check press (0-3): stop, gone - its maybe forgotten, no line");
    WatchPress(m, s, o, bar);
    m.maybes = { { s, kT + 50, false, false } };
    SetFront(third);
    int calls = FrontCallsFor(s);
    v = run(kT + 0.7);
    Check(v == Verdict::Stop && !m.watch.window && m.maybes.empty() && lines.empty() && m.pressUse.use == Use::Try && FrontCallsFor(s) == calls,
          "check press (0-3): stop, another program in front - its maybe forgotten, nothing learnt, nothing done");
    WatchPress(m, s, o, bar);
    SetFront(tray);
    v = run(kT + 0.7);
    Check(v == Verdict::Wait && m.watch.window == s && m.watch.next == kT + 0.7 + appclick::kLook && lines.empty(),
          "check press (0-3): the taskbar's window in front (its class) - not the user going on: waited for, looked at after kLook");
    // ---- wait, ask: before kPressCheck it waits; then its program and the taskbar it was pressed on are asked
    WatchPress(m, s, o, bar);
    SetFront(o);
    v = run(kT + 0.8);
    Check(v == Verdict::Wait && m.watch.window == s && m.watch.next == kT + 0.8 + appclick::kLook && !m.watch.answers, "check press (0-3): wait - looked at after kLook");
    v = run(kT + 1.8);
    Check(v == Verdict::Ask && m.watch.answers && m.watch.next == kT + 1.8 + appclick::kLook && lines.empty(), "check press (0-3): ask - asked, looked at after kLook");
    const auto asked = m.watch.answers;
    Check(asked && WaitFor([&] { return asked->load() == 1; }, 3000), "check press (0-3): ask - the fake taskbar and its program both answer (1)");
    // ---- another window of its program in front at the click: that one in front is no sign of the press
    WatchPress(m, s, o, bar);
    m.watch.frontWasItsProgram = true;
    SetFront(s2);
    v = run(kT + 0.8);
    Check(v == Verdict::Wait && m.watch.window == s, "check press (0-3): its program's other window in front, it was at the click - not come");
    m.watch.frontWasItsProgram = false;
    v = run(kT + 0.9);
    Check(v == Verdict::Done, "check press (0-3): its program's other window in front, it was not at the click - come");
    // ---- give up: nothing in kBusyWait from a busy Explorer or program
    auto giveUp = [&](bool busy, int answers, bool down) {
        WatchPress(m, s, o, bar);
        m.watch.explorerBusy = busy; m.watch.wantDown = down;
        if (answers >= 0) m.watch.answers = Answers(answers);
        m.maybes = { { s, kT + 50, false, false } };
        m.slowUntil = 0;
        SetFront(o);
        return run(kT + 9.0) == Verdict::GiveUp && !m.watch.window && m.maybes.empty();
    };
    Check(giveUp(true, -1, false) && m.slowUntil == kT + 9.0 + taskbarapps::kSlowPause, "check press (0-3): give up, Explorer busy - its maybe forgotten, no presses for kSlowPause");
    CheckLine(lines, 0, L"app click: single - its taskbar button was pressed and it did not come up in 8.5 s (Explorer was busy): nothing more sent (window: FakeAppWindow 800x600; "
                        L"in front: other.exe FakeAppWindow)", "check press (0-3): give up, Explorer busy - its line (the window, then the one in front)");
    Check(giveUp(false, 3, false) && m.slowUntil == kT + 9.0 + taskbarapps::kSlowPause, "check press (0-3): give up, Explorer did not answer (3) - the pause too");
    CheckLine(lines, 0, L"app click: single - its taskbar button was pressed and it did not come up in 8.5 s (Explorer was busy): nothing more sent (window: FakeAppWindow 800x600; "
                        L"in front: other.exe FakeAppWindow)", "check press (0-3): give up, Explorer did not answer (3) - Explorer named busy");
    Check(giveUp(false, 2, true) && m.slowUntil == 0, "check press (0-3): give up, its program did not answer (2) - no pause");
    CheckLine(lines, 0, L"app click: single - its taskbar button was pressed and it did not go down in 8.5 s (its program does not answer): nothing more sent (window: FakeAppWindow "
                        L"800x600; in front: other.exe FakeAppWindow)", "check press (0-3): give up, a minimize, its program busy - its line");
    // ---- dud: its program answers and nothing happened - the dock does it itself
    auto dud = [&](bool down) {
        WatchPress(m, s, o, bar);
        m.watch.wantDown = down;
        m.watch.answers = Answers(1);
        m.maybes.clear();
        SetFront(o);
        return run(kT + 1.8) == Verdict::Dud;
    };
    m.pressUse = {};
    calls = FrontCallsFor(s);
    Check(dud(false) && m.pressUse.use == Use::Off && m.watch.window == s && !m.watch.byTaskbar && m.watch.windows == 2 && FrontCallsFor(s) > calls,
          "check press (0-3): dud - the taskbar's way off (not working yet), the dock brings it up");
    CheckLine(lines, 0, L"app click: single - its taskbar button was pressed and nothing happened in 1.3 s while its program answers: the dock does it itself; apps go the dock's "
                        L"own way for the rest of this run", "check press (0-3): dud, turned off - its line");
    m.pressUse.use = Use::Works; m.pressUse.duds = 0;
    Check(dud(false) && m.pressUse.use == Use::Works && m.pressUse.duds == 1, "check press (0-3): dud once the way works - one in a row");
    CheckLine(lines, 0, L"app click: single - its taskbar button was pressed and nothing happened in 1.3 s while its program answers: the dock does it itself",
              "check press (0-3): dud, not turned off - its line");
    calls = FrontCallsFor(s);
    const int downs = PostedTo(s, WM_SYSCOMMAND, SC_MINIMIZE);
    Check(dud(true) && !m.watch.window && PostedTo(s, WM_SYSCOMMAND, SC_MINIMIZE) == downs + 1 && FrontCallsFor(s) == calls, "check press (0-3): dud, a minimize - the dock minimizes it");
    Edit(s, [](FakeWin& f) { f.iconic = false; });
    // ---- a state the dock does not reach today (a maybe is only for a press Explorer was busy with): kept as it is
    m.pressUse = {};
    WatchPress(m, s, o, bar);
    m.watch.answers = Answers(1);
    m.maybes = { { s, later, false, false } };
    calls = FrontCallsFor(s);
    v = run(kT + 1.8);
    Check(v == Verdict::Dud && m.maybes.size() == 1 && !m.watch.window && FrontCallsFor(s) == calls,
          "check press (0-3): dud with a maybe (a state not met today) - the maybe kept, the dock's bring-up held by it");
    m.maybes.clear();
    // ---- clicked again while its press brings it up: counted, and put in front by the dock (forced)
    auto a = ItemOf(m, s);
    WatchPress(m, s, o, bar);
    m.watch.clicked = m.watch.pressedAt = NowSeconds();   // Click looks at the real time
    SetFront(o);
    calls = FrontCallsFor(s);
    m.ActivateWindow(s);
    Check(m.watch.window == s && m.watch.byTaskbar && m.watch.clicks == 2 && FrontCallsFor(s) == calls, "check press (0-3): picked again while its press brings it up - counted, nothing sent");
    if (a) m.Click(*a, 0);
    Check(m.watch.window == s && m.watch.byTaskbar && m.watch.clicks == 3 && m.watch.forced && FrontCallsFor(s) > calls && m.presses.empty(),
          "check press (0-3): clicked again - counted, put in front by the dock, forced (its coming teaches nothing)");
}

// ---------------------------------------------------------------- the dock's own watch: its lines (apps_click.cpp CheckWatch, BringUp)
static void WatchDock(AppModel& m, HWND h, HWND before) {
    m.watch = {};
    auto& w = m.watch;
    w.window = h; w.before = before; w.name = L"single"; w.why = L"bringing up its window"; w.windows = 2; w.clicked = kT; w.next = kT + 0.25;
}
static void WatchLineTests() {
    Reset();
    AppModel m;
    HWND o = nullptr;
    HWND s = OneWindowBehind(m, 520, &o);
    Proc(525, L"c:\\fake\\searchhost.exe");
    HWND ghost = Win(525, L"Search");
    Edit(ghost, [](FakeWin& f) { f.cloaked = true; });
    auto look = [&](double now) { const size_t mark = LogMark(); m.CheckWatch(now); return LogsSince(mark); };
    auto job = [](int state, double handledAt = 0, bool ignored = false, unsigned long error = 0) {
        auto j = std::make_shared<RestoreJob>();
        j->state = state; j->handledAt = handledAt; j->ignored = ignored; j->error = error;
        return j;
    };
    // ---- it came up: the line's tail - the restore its program left undone, else its program busy, else over a closed Start
    WatchDock(m, s, o);
    m.watch.restore = job(1, kT + 0.1, true); m.watch.saidBusy = true; m.watch.ghostFront = true;
    m.restores = { { s, m.watch.restore } };
    SetFront(s);
    auto lines = look(kT + 0.3);
    CheckLine(lines, 0, L"app click: single came up after 0.3 s (its program left the restore command undone: the system's restore brought it)",
              "watch lines (0-3): came up, the restore left undone - its line under 1 s, that tail first");
    Check(!m.watch.window && m.lastUp.window == s && m.lastUp.at == kT + 0.3 && m.restores.empty(), "watch lines (0-3): came up - the window that came, its handled restore forgotten");
    WatchDock(m, s, o);
    m.watch.saidBusy = true; m.watch.ghostFront = true;
    lines = look(kT + 0.4);
    CheckLine(lines, 0, L"app click: single came up after 0.4 s (its program was busy)", "watch lines (0-3): came up, its program was busy - its line under 1 s, before the closed Start's tail");
    WatchDock(m, s, o);
    m.watch.saidBusy = true;
    lines = look(kT + 0.4);
    CheckLine(lines, 0, L"app click: single came up after 0.4 s (its program was busy)", "watch lines (0-3): came up, its program was busy (and nothing else) - its line under 1 s");
    WatchDock(m, s, o);
    m.watch.ghostFront = true;
    lines = look(kT + 0.5);
    CheckLine(lines, 0, L"app click: single came up after 0.5 s (over the closed Start or Search that kept the front)", "watch lines (0-3): came up over a closed Start - its line");
    WatchDock(m, s, o);
    lines = look(kT + 1.3);
    CheckLine(lines, 0, L"app click: single came up after 1.3 s", "watch lines (0-3): came up after 1.3 s - its line, no tail");
    WatchDock(m, s, o);
    lines = look(kT + 0.9);
    Check(lines.empty() && !m.watch.window, "watch lines (0-3): came up in 0.9 s - no line");
    // ---- its program busy with the restore command: said once
    WatchDock(m, s, o);
    m.watch.restore = job(0);
    Edit(s, [](FakeWin& f) { f.iconic = true; });
    SetFront(o);
    lines = look(kT + 0.6);
    CheckLine(lines, 0, L"app click: single is busy - its program has not answered the restore command in 0.6 s (Windows still calls it responding); waiting up to 8 s",
              "watch lines (0-3): busy - its line");
    Check(m.watch.saidBusy && m.watch.next == kT + 0.6 + appclick::kLook && look(kT + 0.9).empty(), "watch lines (0-3): busy - said once, looked at after kLook");
    // ---- gave up: the line whole, each part in its place
    auto giveUp = [&](double now) { lines = look(now); return !m.watch.window; };
    WatchDock(m, s, o);
    m.watch.restore = job(0);
    Check(giveUp(kT + 8.2), "watch lines (0-3): gave up - its program did not answer in kBusyWait");
    CheckLine(lines, 0, L"app click: could not bring up single in 8.2 s - its program did not answer the restore command (window: FakeAppWindow 800x600 minimized; clicked 1 times; "
                        L"bringing up its window; 2 windows; in front: other.exe FakeAppWindow; the dock's right to change the front window: had it)",
              "watch lines (0-3): gave up, no answer - its line");
    WatchDock(m, s, o);
    m.watch.restore = job(2, kT + 0.2, false, 1460);
    Check(giveUp(kT + 0.4), "watch lines (0-3): gave up - the restore command not delivered");
    CheckLine(lines, 0, L"app click: could not bring up single in 0.4 s - the restore command was not delivered (error 1460) (window: FakeAppWindow 800x600 minimized; clicked 1 times; "
                        L"bringing up its window; 2 windows; in front: other.exe FakeAppWindow; the dock's right to change the front window: had it)",
              "watch lines (0-3): gave up, not delivered - its line");
    WatchDock(m, s, o);
    m.watch.restore = job(1, kT + 0.2);
    Check(giveUp(kT + 0.8), "watch lines (0-3): gave up - still down after its program handled the restore");
    CheckLine(lines, 0, L"app click: could not bring up single in 0.8 s - still minimized after its program handled the restore command (window: FakeAppWindow 800x600 minimized; "
                        L"clicked 1 times; bringing up its window; 2 windows; in front: other.exe FakeAppWindow; the dock's right to change the front window: had it)",
              "watch lines (0-3): gave up, still down - its line");
    Edit(s, [](FakeWin& f) { f.iconic = false; f.visible = false; });
    WatchDock(m, s, o);
    Check(giveUp(kT + 0.3), "watch lines (0-3): gave up - hidden by its program");
    CheckLine(lines, 0, L"app click: could not bring up single in 0.3 s - hidden (window: FakeAppWindow 800x600 hidden; clicked 1 times; bringing up its window; 2 windows; in front: "
                        L"other.exe FakeAppWindow; the dock's right to change the front window: had it)", "watch lines (0-3): gave up, hidden - its line");
    Edit(s, [](FakeWin& f) { f.visible = true; });
    // up behind a closed Start that keeps the front, every part of the line there: clicks, why and windows apart, the right at the click apart
    WatchDock(m, s, ghost);
    {
        auto& w = m.watch;
        w.clicks = 3; w.refocused = appclick::kRefocusTries; w.rightAtClick = (int)ForegroundRight::Refused; w.right = (int)ForegroundRight::Had; w.attachError = 5;
        w.hadPopup = true; w.popupEsc = 1; w.ghostFront = true; w.cloakAtClick = 2; w.launcherAtClick = 1; w.frontFirst = 0; w.frontSecond = 0; w.frontError = 5; w.overAdmin = 2;
    }
    launcherShown = 0;
    SetFront(ghost);
    Check(giveUp(kT + 1.5), "watch lines (0-3): gave up - up, behind a window that stays");
    CheckLine(lines, 0, L"app click: could not bring up single in 1.5 s - up, but another window stays in front (window: FakeAppWindow 800x600; clicked 3 times; bringing up its window; "
                        L"2 windows; in front: searchhost.exe FakeAppWindow cloaked; the dock's right to change the front window: had it (at the click: refused); attaching to it failed (5); "
                        L"Start or Search was in front at the click: closed with Esc; a closed Start or Search kept the front at the click (not shown: no Esc); cloak 2 at the click, Start "
                        L"shown: yes then, no now; SetForegroundWindow said 0, then 0 (error 5); handed to the admin helper 2 times (an administrator's window in front))",
              "watch lines (0-3): gave up - every part of its line, in order");
    launcherShown = -1;
    WatchDock(m, s, o);
    m.watch.windows = 1; m.watch.refocused = appclick::kRefocusTries; m.watch.frontFirst = 1; m.watch.popupEsc = 0; m.watch.hadPopup = true;
    Elevate(521);
    SetFront(o);
    Check(giveUp(kT + 1.5), "watch lines (0-3): gave up - behind an administrator's window");
    CheckLine(lines, 0, L"app click: could not bring up single in 1.5 s - up, but another window stays in front (window: FakeAppWindow 800x600; clicked 1 times; bringing up its window; "
                        L"1 windows; in front: other.exe FakeAppWindow (administrator); the dock's right to change the front window: had it; Start or Search was in front at the click: "
                        L"it went before an Esc was sent; SetForegroundWindow said 1; Windows does not let the dock take the front from a window that runs as administrator - the admin "
                        L"helper can (settings: admin app windows))", "watch lines (0-3): gave up behind an administrator's window - its line");
    { std::lock_guard<std::mutex> lk(mu); procs[521].elevated = false; }
    // ---- up behind another window: put in front again - its flashes quiet, the calls of a look (not of the click)
    WatchDock(m, s, o);
    m.quietFlashes.clear();
    rightGives = ForegroundRight::Taken;
    const int calls = FrontCallsFor(s), allows = allowFrontCalls;
    lines = look(kT + 0.3);
    rightGives = ForegroundRight::Had;
    Check(m.watch.window == s && m.watch.refocused == 1 && m.watch.right == (int)ForegroundRight::Taken && FrontCallsFor(s) > calls && allowFrontCalls == allows &&
          m.watch.next == kT + 0.3 + appclick::kLook && lines.empty(), "watch lines (0-3): refocus - put in front again, the right taken anew, its program not handed the right");
    Check(m.quietFlashes.size() == 1 && m.quietFlashes[0].window == s && m.quietFlashes[0].pid == 520 && m.quietFlashes[0].until == kT + 0.3 + appclick::kQuietFlash,
          "watch lines (0-3): refocus - its flashes quiet for kQuietFlash");
    // ---- the right at the click kept apart from the last try's
    m.watch = {};
    rightGives = ForegroundRight::Refused;
    m.ActivateWindow(s);
    rightGives = ForegroundRight::Had;
    Check(m.watch.window == s && m.watch.rightAtClick == (int)ForegroundRight::Refused && m.watch.right == (int)ForegroundRight::Refused,
          "watch lines (0-3): a bring-up - the right at the click kept (and as the last try's)");
    // ---- Start in front: it stayed after two Esc - the line timed from the click
    HWND start = StartMenu(527);
    WatchDock(m, s, o);
    m.watch.popup = start; m.watch.hadPopup = true; m.watch.popupClosed = 2; m.watch.popupEsc = 1; m.watch.popupClosedAt = kT + 0.6;
    SetFront(start);
    lines = look(kT + 1.2);
    CheckLine(lines, 0, L"app click: could not bring up single - startmenuexperiencehost.exe FakeAppWindow stayed in front after Esc 1 times (not sent at the other tries: it had gone "
                        L"from the front or was not shown by then) (1.2 s after the click)", "watch lines (0-3): Start stayed - its line, timed from the click");
    Check(!m.watch.window, "watch lines (0-3): Start stayed - the watch ends");
    // ---- Start in front with a restore command of an earlier watch unanswered: that one is waited for, no second
    Edit(s, [](FakeWin& f) { f.iconic = true; });
    auto unanswered = job(0);
    m.restores = { { s, unanswered } };
    m.watch = {};
    SetFront(start);
    m.ActivateWindow(s);
    Check(m.watch.window == s && m.watch.popup == start && m.watch.restore == unanswered && m.restores.size() == 1,
          "watch lines (0-3): Start in front - the earlier restore command taken into the watch");
    SetFront(o);
    m.CheckWatch(m.watch.clicked + 0.2);
    Check(m.watch.window == s && !m.watch.popup && m.watch.before == o && m.watch.restore == unanswered && m.restores.size() == 1 && State(s).restoreSends == 0,
          "watch lines (0-3): ...Start gone - brought up without a second restore command");
    m.watch = {};
    m.restores.clear();
    Edit(s, [](FakeWin& f) { f.iconic = false; });
}

// ---------------------------------------------------------------- what a pin starts, as the code makes it (apps_pins.cpp, apps_click.cpp Launch)
static void PinLaunchTests() {
    Reset();
    const std::wstring path = DataFile(L"pins.txt");
    const std::wstring notes = L"Contoso.Notes_8wekyb3d8bbwe!App", mail = L"Contoso.Mail_8wekyb3d8bbwe!Mail";
    // ---- the taskbar's store pin, imported (no pins.txt): its line in pins.txt
    EmptyPinFolder();
    DeleteFileW(path.c_str());
    UseBlob(Blob({ { notes, true } }));
    appsFolder[notes] = { L"Notes", nullptr };
    {
        AppModel m;
        m.LoadPins();
        CheckBytes(FileBytes(path), "Notes\tContoso.Notes_8wekyb3d8bbwe!App\t\tshell:AppsFolder\\Contoso.Notes_8wekyb3d8bbwe!App\r\n",
                   "pin launch (0-3): an imported store pin - shell:AppsFolder\\ and its id, its Start menu name");
        Check(m.items.size() == 1 && m.structureVersion == 0, "pin launch (0-3): imported (no pins.txt) - the structure version not bumped");
    }
    // ---- a store pin added to pins of an older version (pinsVersion 1)
    WriteBytes(path, "Tool\t\tc:\\hunt-apps_model\\tool.exe\tc:\\hunt-apps_model\\tool.exe\r\n");
    UseBlob(Blob({ { mail, false } }));
    appsFolder[mail] = { L"Mail", nullptr };
    {
        AppModel m;
        m.LoadPins();
        Check(m.structureVersion == 1, "pin launch (0-3): pins read from pins.txt - the structure version bumped once");
        g_settings.pinsVersion = 1;
        m.MigratePins();
        CheckBytes(FileBytes(path), "Tool\t\tc:\\hunt-apps_model\\tool.exe\tc:\\hunt-apps_model\\tool.exe\r\nMail\tContoso.Mail_8wekyb3d8bbwe!Mail\t\tshell:AppsFolder\\Contoso.Mail_8wekyb3d8bbwe!Mail\r\n",
                   "pin launch (0-3): a store pin migrated - after the pins, shell:AppsFolder\\ and its id");
        Check(m.structureVersion == 2 && g_settings.pinsVersion == 2, "pin launch (0-3): migrated - bumped again, the pins' version 2");
    }
    g_settings.pinsVersion = 2;
    { std::lock_guard<std::mutex> lk(mu); haveTaskband = false; }
    // ---- a running app pinned (SetPinned, MoveTo): a Start menu entry's id, else its program; a launch it has stays
    const std::wstring notesExe = L"c:\\program files\\windowsapps\\contoso.notes_1.0.0.0_x64__8wekyb3d8bbwe\\notes.exe";
    auto running = [&](AppModel& m, const std::wstring& id, const std::wstring& launch) {
        auto a = m.NewItem();
        a->name = L"Running"; a->aumid = id; a->exe = notesExe; a->launch = launch; a->windows = { NewHandle() };
        m.items.push_back(a);
        return a;
    };
    {
        AppModel m;
        auto a = running(m, notes, L"");
        knownFolderAsked.clear();
        m.SetPinned(*a, true);
        Check(a->pinned && a->launch == L"shell:AppsFolder\\" + notes && knownFolderAsked.size() == 1 && knownFolderAsked[0] == notes,
              "pin launch (0-3): pinned, its id a Start menu entry - shell:AppsFolder\\ and its id");
        CheckBytes(FileBytes(path), "Running\tContoso.Notes_8wekyb3d8bbwe!App\t" + ToUtf8(notesExe) + "\tshell:AppsFolder\\Contoso.Notes_8wekyb3d8bbwe!App\r\n",
                   "pin launch (0-3): pinned - its line");
        auto b = running(m, L"Contoso.Other_8wekyb3d8bbwe!App", L"");
        m.SetPinned(*b, true);
        Check(b->launch == notesExe, "pin launch (0-3): pinned, its id no Start menu entry - its program");
        auto c = running(m, notes, L"c:\\hunt-apps_model\\keep.lnk");
        m.SetPinned(*c, true);
        Check(c->launch == L"c:\\hunt-apps_model\\keep.lnk", "pin launch (0-3): pinned, a launch of its own - kept");
        auto d = running(m, notes, L"");
        m.MoveTo(*d, 0, true);
        Check(d->pinned && d->launch == L"shell:AppsFolder\\" + notes && m.items[0] == d, "pin launch (0-3): moved among the pins - shell:AppsFolder\\ and its id");
        auto e = running(m, L"Contoso.Other_8wekyb3d8bbwe!App", L"");
        m.MoveTo(*e, 0, true);
        Check(e->launch == notesExe, "pin launch (0-3): moved among the pins, no Start menu entry - its program");
        auto f = running(m, L"", L"");
        m.SetPinned(*f, true);
        Check(f->launch == notesExe, "pin launch (0-3): pinned, no id - its program");
    }
    // ---- a file pinned: saved at once
    DeleteFileW(path.c_str());
    {
        AppModel m;
        const int at = m.PinFile(L"C:\\Hunt-Apps_Model\\Tool.EXE", 0);
        Check(at == 0 && m.items.size() == 1, "pin launch (0-3): a file pinned");
        CheckBytes(FileBytes(path), "Tool\t\tc:\\hunt-apps_model\\tool.exe\tC:\\Hunt-Apps_Model\\Tool.EXE\r\n", "pin launch (0-3): a file pinned - saved, its path lowercased, the launch as dropped");
    }
    // ---- a store app with no launch of its own started
    {
        AppModel m;
        auto a = Pinned(m, L"Notes", L"", notes, L"");
        m.Launch(*a, 4);
        Check(started.size() == 1 && started[0].first == L"shell:AppsFolder\\" + notes && started[0].second.empty() && a->launchDock == 4,
              "pin launch (0-3): a store app without a launch started - shell:AppsFolder\\ and its id");
    }
    // ---- pins.txt read as text: a Ctrl+Z byte (0x1A) ends it
    WriteBytes(path, "A\t\tc:\\hunt-apps_model\\a.exe\tc:\\hunt-apps_model\\a.exe\r\nB\x1A" "b\t\tc:\\hunt-apps_model\\b.exe\tc:\\hunt-apps_model\\b.exe\r\n"
                     "C\t\tc:\\hunt-apps_model\\c.exe\tc:\\hunt-apps_model\\c.exe\r\n");
    {
        AppModel m;
        m.LoadPins();
        Check(m.items.size() == 1 && m.items[0]->name == L"A", "pin launch (0-3): a 0x1A byte in pins.txt - the pins after it not read (text mode)");
    }
    DeleteFileW(path.c_str());
}

// ---------------------------------------------------------------- "the same app": the finer points (apps.cpp Refresh, apps_pins.cpp PinFile)
static void SameAppDetailTests() {
    Reset();
    const std::wstring cmd = System32(L"cmd.exe");
    const std::wstring lnk = ExeDir() + L"\\hunt-apps_model 0-3 case.lnk", missing = ExeDir() + L"\\hunt-apps_model 0-3 missing.lnk",
                       env = ExeDir() + L"\\hunt-apps_model 0-3 env.lnk";
    DeleteFileW(missing.c_str());
    Check(MakeShortcut(lnk, cmd, L"/k title case", L"hunt.fake.PROFILE1") && MakeShortcut(env, L"%SystemRoot%\\system32\\cmd.exe", L"/k"),
          "same app details (0-3): fixture shortcuts");
    // ids compared without case
    Proc(540, L"c:\\fake\\profile.exe");
    HWND w = Win(540, L"Profile");
    Edit(w, [](FakeWin& f) { f.aumid = L"hunt.fake.PROFILE1"; });
    {
        AppModel r;
        auto p = Pinned(r, L"P", L"c:\\fake\\pinned.exe", L"Hunt.Fake.Profile1", L"c:\\fake\\pinned.exe");
        r.Refresh();
        Check(ItemOf(r, w) == p && r.items.size() == 1, "same app details (0-3): Refresh - an id that differs in case only is the same app");
    }
    {
        AppModel p;
        Pinned(p, L"Q", L"c:\\fake\\q.exe", L"", L"c:\\fake\\q.exe");
        auto x = Pinned(p, L"P", L"c:\\fake\\pinned.exe", L"Hunt.Fake.Profile1", L"c:\\fake\\pinned.exe");
        const int at = p.PinFile(lnk, 0);
        Check(p.items.size() == 2 && Names(p) == L"P,Q" && at == 0 && x->launch == L"c:\\fake\\pinned.exe",
              "same app details (0-3): PinFile - a shortcut whose id differs in case only moves that app");
    }
    // a shortcut that gives no program and no id (it cannot be read) is no app's: not a store app's (no path either)
    {
        AppModel p;
        auto store = Pinned(p, L"Notes", L"", L"Contoso.Notes_8wekyb3d8bbwe!App", L"shell:AppsFolder\\Contoso.Notes_8wekyb3d8bbwe!App");
        const int at = p.PinFile(missing, 1);
        Check(p.items.size() == 2 && at == 1 && p.items[0] == store && p.items[1]->launch == missing && p.items[1]->exe.empty() && p.items[1]->aumid.empty(),
              "same app details (0-3): PinFile - a shortcut with no program and no id pinned on its own (not moved onto a store app)");
    }
    // the package from the process's id first: a window with an id of its own not of a package's form
    Proc(545, L"c:\\fake\\host.exe", L"Contoso.Mail_8wekyb3d8bbwe!Mail");
    HWND h = Win(545, L"Host");
    Edit(h, [](FakeWin& f) { f.aumid = L"Hunt.Window.Own"; });
    {
        AppModel r;
        auto cal = Pinned(r, L"Calendar", L"", L"Contoso.Mail_8wekyb3d8bbwe!Calendar", L"shell:AppsFolder\\Contoso.Mail_8wekyb3d8bbwe!Calendar");
        r.Refresh();
        Check(ItemOf(r, h) == cal, "same app details (0-3): Refresh - the package of the process's id groups a window whose own id names none");
    }
    // a shortcut's target with an environment variable: the program's path expanded, lowercased
    {
        AppModel p;
        p.PinFile(env, 0);
        Check(p.items.size() == 1 && p.items[0]->exe == cmd, "same app details (0-3): PinFile - a shortcut to %SystemRoot%\\...: its program expanded and lowercased");
    }
    // a new running app: its line
    Edit(w, [](FakeWin& f) { f.alive = false; });
    Edit(h, [](FakeWin& f) { f.alive = false; });
    Proc(548, L"c:\\fake\\newapp.exe", L"Contoso.New_8wekyb3d8bbwe!App");
    Win(548, L"New app");
    {
        AppModel r;
        const size_t mark = LogMark();
        r.Refresh();
        auto lines = LogsSince(mark);
        CheckLine(lines, 0, L"running app: newapp (aumid Contoso.New_8wekyb3d8bbwe!App, package contoso.new_8wekyb3d8bbwe)", "same app details (0-3): a new running app - its line");
    }
    DeleteFileW(lnk.c_str());
    DeleteFileW(env.c_str());
}

// ---------------------------------------------------------------- icons: the bitmap made is the one returned, and where it is asked for (apps.cpp)
static void IconSourceTests() {
    Reset();
    using V = std::vector<uint32_t>;
    const V straight = { 0xFF102030, 0x80FF8040, 0x00FFFFFF, 0x40804020, 0x01FFFFFF, 0xC80A0A0A };
    const V premul = { 0xFF102030, 0x80804020, 0x00000000, 0x40201008, 0x01010101, 0xC8070707 };
    const V noAlpha = { 0x00102030, 0x00FF8040, 0x00FFFFFF, 0x00804020, 0x00000000, 0x000A0A0A };
    const V opaque = { 0xFF102030, 0xFFFF8040, 0xFFFFFFFF, 0xFF804020, 0xFF000000, 0xFF0A0A0A };
    iconrec::giveBack = true;
    auto last = [] { return iconrec::made.empty() ? nullptr : iconrec::made.back().given; };
    {
        HBITMAP hbm = Dib(3, 2, 32, false, straight);
        auto b = BitmapFromHBITMAP(hbm);
        DeleteObject(hbm);
        Check(b && b.Get() == last(), "icon sources (0-3): a DIB section's bitmap - the one made is returned");
        hbm = Ddb(3, 2, noAlpha);
        auto d = BitmapFromHBITMAP(hbm);
        DeleteObject(hbm);
        Check(d && d.Get() == last(), "icon sources (0-3): a bitmap through GetDIBits - the one made is returned");
        HICON hi = MakeIcon(Dib(3, 2, 32, true, straight), MaskBits(3, 2, {}));
        auto i = IconFromHICON(hi);
        DestroyIcon(hi);
        Check(i && i.Get() == last(), "icon sources (0-3): an icon's bitmap - the one made is returned");
    }
    // ---- a shortcut's icon location first (expanded, its index), the program's file icon only when that gives none
    const std::wstring lnk = ExeDir() + L"\\hunt-apps_model 0-3 icon.lnk";
    bool made = false;
    {
        ComPtr<IShellLinkW> sl;
        ComPtr<IPersistFile> pf;
        made = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) && SUCCEEDED(sl->SetPath(System32(L"cmd.exe").c_str())) &&
               SUCCEEDED(sl->SetIconLocation(L"%SystemRoot%\\system32\\shell32.dll", 3)) && SUCCEEDED(sl.As(&pf)) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    }
    Check(made, "icon sources (0-3): fixture - a shortcut with an icon location of %SystemRoot%\\system32\\shell32.dll, 3");
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring shell32 = std::wstring(win) + L"\\system32\\shell32.dll";
    extractGives = MakeIcon(Dib(3, 2, 32, true, straight), MaskBits(3, 2, {}));
    AppModel m;
    auto pin = Pinned(m, L"Iconic", L"c:\\hunt-apps_model\\iconic.exe", L"", lnk);
    m.LoadIconAndName(*pin, nullptr);
    Check(extractAsked.size() == 1 && !_wcsicmp(extractAsked[0].file.c_str(), shell32.c_str()) && extractAsked[0].index == 3 && pin->icon && pin->icon.Get() == last(),
          "icon sources (0-3): a shortcut's pin - its icon location asked (expanded, its index), that icon taken, the program not asked");
    if (!extractAsked.empty()) extractRefuses = { extractAsked[0].file };   // the location as it was asked gives none now
    extractAsked.clear();
    pin->icon.Reset();
    m.LoadIconAndName(*pin, nullptr);
    Check(extractAsked.size() == 2 && extractAsked[0].index == 3 && extractAsked[1].file == L"c:\\hunt-apps_model\\iconic.exe" && extractAsked[1].index == 0 && pin->icon &&
          pin->icon.Get() == last(), "icon sources (0-3): ...its icon location gives none - the program's file icon, index 0");
    extractRefuses.clear();
    extractAsked.clear();
    parsedAsked.clear();
    auto store = Pinned(m, L"Store", L"c:\\program files\\windowsapps\\contoso.x_1.0.0.0_x64__8wekyb3d8bbwe\\x.exe", L"", L"");
    m.LoadIconAndName(*store, nullptr);
    Check(extractAsked.empty() && parsedAsked.size() == 1 && parsedAsked[0] == store->exe, "icon sources (0-3): a program under windowsapps - no file icon asked, its shell item");
    DestroyIcon(extractGives);
    extractGives = nullptr;
    DeleteFileW(lnk.c_str());
    // ---- a shell item's image: asked at 256, icon only, bigger allowed; the shortcut's item before the program's
    auto image = [&] { return Dib(3, 2, 32, false, straight); };
    parsedAsked.clear();
    parsedItems[L"c:\\hunt-apps_model\\viewer.exe"] = { L"Viewer", image };
    auto viewer = Pinned(m, L"Viewer", L"c:\\hunt-apps_model\\viewer.exe", L"", L"c:\\hunt-apps_model\\viewer.lnk");
    size_t at = iconrec::made.size();
    m.LoadIconAndName(*viewer, nullptr);
    Check(parsedAsked.size() == 2 && parsedAsked[0] == L"c:\\hunt-apps_model\\viewer.lnk" && parsedAsked[1] == L"c:\\hunt-apps_model\\viewer.exe",
          "icon sources (0-3): shell items - the shortcut's asked before the program's");
    Check(imageAsked.size() == 1 && imageAsked[0].size.cx == 256 && imageAsked[0].size.cy == 256 && imageAsked[0].flags == (SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK) &&
          MadeAs(at, 3, 2, premul) && viewer->icon && viewer->icon.Get() == last(), "icon sources (0-3): a shell item's image - asked at 256, icon only, bigger allowed; made and returned");
    // a store app: its Start menu entry's image and name
    imageAsked.clear();
    appsFolder[L"Contoso.Notes_8wekyb3d8bbwe!App"] = { L"Notes", image };
    auto notes = Pinned(m, L"", L"", L"Contoso.Notes_8wekyb3d8bbwe!App", L"shell:AppsFolder\\Contoso.Notes_8wekyb3d8bbwe!App");
    at = iconrec::made.size();
    m.LoadIconAndName(*notes, nullptr);
    Check(notes->name == L"Notes" && imageAsked.size() == 1 && MadeAs(at, 3, 2, premul) && notes->icon && notes->icon.Get() == last(),
          "icon sources (0-3): a store app - its Start menu entry's image and name");
    // a window's own icon, the big one
    Proc(550, L"");
    HWND sample = Win(550, L"Its title");
    windowIcon = MakeIcon(Dib(3, 2, 32, true, noAlpha), MaskBits(3, 2, {}));
    auto bare = Pinned(m, L"", L"", L"", L"");
    at = iconrec::made.size();
    m.LoadIconAndName(*bare, sample);
    Check(getIconAsked.size() == 1 && getIconAsked[0] == ICON_BIG && MadeAs(at, 3, 2, opaque) && bare->icon && bare->icon.Get() == last() && bare->name == L"Its title",
          "icon sources (0-3): no file, no item - the window's big icon (WM_GETICON ICON_BIG), its title for a name");
    DestroyIcon(windowIcon);
    windowIcon = nullptr;
    for (auto& it : m.items) it->icon.Reset();   // the fake bitmaps go before giveBack is off
    iconrec::giveBack = false;
}

// ---------------------------------------------------------------- what a press takes from its click (apps_press.cpp StartPress, PollPresses)
static void PressClickValueTests() {
    using taskbarapps::Outcome;
    PressOn();
    // an app with a click-through overlay besides its window: one usable window of two
    Proc(560, L"c:\\fake\\overlaid.exe");
    Proc(561, L"c:\\fake\\other.exe");
    Proc(562, L"c:\\fake\\third.exe");
    HWND main = Win(560, L"Main"), overlay = Win(560, L"Overlay"), o = Win(561, L"Other", true), third = Win(562, L"Third");
    Edit(overlay, [](FakeWin& f) { f.exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT; });
    SetFront(o);
    AppModel m;
    m.Refresh();
    auto a = ItemOf(m, main);
    Check(a && a->windows.size() == 2 && ItemOf(m, overlay) == a && PressFor(m, main, 1), "press values (0-3): fixture - an app with a window and an overlay, its button looked up");
    if (m.presses.empty()) return;
    Check(m.presses[0].usable == 1 && (HWND)(uintptr_t)m.presses[0].d.window == main && m.presses[0].before == o, "press values (0-3): the press - one usable window, the one in front at the click");
    SetFront(third);   // the front changes before the answer: the press's own doing, maybe
    Answer(m, Res(Outcome::Pressed, L"invoke", S_OK, 0.01, 0.01, 4), 0.1);
    Check(m.watch.window == main && m.watch.windows == 1 && m.watch.before == o, "press values (0-3): pressed - the watch counts the usable windows, takes the front at the click");
    Check(!m.watch.restore && m.watch.right == 0 && m.watch.rightAtClick == 0 && m.watch.refocused == 0 && !m.watch.saidBusy && !m.watch.popup && !m.watch.hadPopup &&
          m.watch.popupClosed == 0 && !m.watch.ghostFront && m.watch.frontFirst == -1 && m.watch.frontSecond == -1 && m.watch.attachError == 0 && m.watch.overAdmin == 0,
          "press values (0-3): pressed - nothing of the dock's own watch (no restore, right, refocus, pop-up or front calls)");
    m.watch = {};
    SetFront(o);
    Check(PressFor(m, main), "press values (0-3): fixture - a second click");
    auto r = Res(Outcome::CountDiffers, L"", S_OK, 0.02, 0, 4);
    r.count = 3;
    size_t mark = LogMark();
    Answer(m, r, 0.1);
    auto lines = LogsSince(mark);
    CheckLine(lines, 0, L"app click: overlaid - its taskbar button counts 3 windows, the dock 1: the dock does it itself, and looks again after 60 s",
              "press values (0-3): counts differ - the dock's count is its usable windows");
    // an earlier restore of the dock's own still on its way (a state not met today: no press goes out then): not taken into the press's watch
    m.watch = {}; m.noButtonUntil.clear();
    Check(PressFor(m, main), "press values (0-3): fixture - a third click");
    m.restores = { { main, std::make_shared<RestoreJob>() } };
    Answer(m, Res(Outcome::Pressed, L"invoke", S_OK, 0.01, 0.01, 4), 0.1);
    Check(m.watch.window == main && m.watch.byTaskbar && !m.watch.restore, "press values (0-3): a restore under way at the answer (a state not met today) - not in the press's watch");
    m.restores.clear();
    // the flashes kept quiet for a bring-up only, not a minimize
    m.watch = {}; m.quietFlashes.clear();
    SetFront(main);
    Check(PressFor(m, main) && m.presses[0].d.action == appclick::Action::Minimize && m.quietFlashes.empty(), "press values (0-3): a minimize looked up - no flashes kept quiet");
    m.presses.clear();
    SetFront(o);
    Check(PressFor(m, main) && m.quietFlashes.size() == 1 && m.quietFlashes[0].window == main && m.quietFlashes[0].until == m.presses[0].clicked + appclick::kQuietFlash,
          "press values (0-3): a bring-up looked up - its flashes quiet for kQuietFlash from the click");
    // the waits' edges: kLookupWait and kPressAnswer on the dot are still waited for
    if (m.presses.empty()) return;
    m.presses[0].clicked = 100.0;
    m.PollPresses(101.0);
    Check(m.presses.size() == 1 && *m.presses[0].stage == taskbarapps::kLooking, "press values (0-3): a lookup kLookupWait on the dot - waited for");
    *m.presses[0].stage = taskbarapps::kPressing;
    m.PollPresses(105.0);
    Check(m.presses.size() == 1, "press values (0-3): a press kPressAnswer on the dot - waited for");
    m.presses.clear();
    // a pinned app's start: the dock's own start when its button counts otherwise - for the dock clicked; a window come meanwhile counted
    m.pressUse.use = taskbarapps::Use::Works;
    auto pin = Pinned(m, L"Pinned", L"c:\\fake\\p.exe", L"", L"c:\\fake\\p.exe");
    m.Click(*pin, 3);
    Check(m.presses.size() == 1, "press values (0-3): fixture - a pinned app's start looked up from dock 3");
    r = Res(Outcome::CountDiffers, L"", S_OK, 0.02, 0, 4);
    r.count = 2;
    Answer(m, r, 0.1);
    Check(started.size() == 1 && pin->launchDock == 3, "press values (0-3): counts differ - the dock starts it itself, for the dock clicked");
    m.noButtonUntil.clear();
    pin->launchStarted = 0;
    m.Click(*pin, 2);
    Check(m.presses.size() == 1, "press values (0-3): fixture - a second start looked up");
    pin->windows = { NewHandle() };   // a window of it came before the answer
    Answer(m, Res(Outcome::Pressed, L"invoke", S_OK, 0.01, 0.01, 4), 0.1);
    Check(pin->launchWindowCount == 1 && m.launchChecks.size() == 1 && m.launchChecks[0].windows == 1, "press values (0-3): pressed - its windows at the answer counted for the launch");
    pin->windows.clear();
    // the launch check's lines
    m.launchChecks = { { pin->id, kT, 0, true } };
    mark = LogMark();
    m.CheckLaunches(kT + 10.5);
    m.launchChecks = { { pin->id, kT, 0, false } };
    m.CheckLaunches(kT + 10.5);
    m.launchChecks = { { pin->id, kT, 0, false } };
    m.CheckLaunches(kT + 10.5);
    lines = LogsSince(mark);
    CheckLine(lines, 0, L"app click: Pinned - its pinned taskbar button was pressed (the press may not have arrived) and no window of it came in 10 s (nothing started again)",
              "press values (0-3): launch check, a press that may not have arrived - its line");
    CheckLine(lines, 1, L"app click: Pinned - its pinned taskbar button was pressed and no window of it came in 10 s (nothing started again)", "press values (0-3): launch check, a miss - its line");
    CheckLine(lines, 2, L"app click: Pinned - its pinned taskbar button was pressed and no window of it came in 10 s (nothing started again); the dock starts pinned apps itself for "
                        L"the rest of this run", "press values (0-3): launch check, the second miss - its line says the dock starts them");
}

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    roaming = ExeDir() + L"\\roaming";
    CreateDirectoryW(roaming.c_str(), nullptr);
    RefreshTests();
    AttentionTests();
    ClickTests();
    RestoreAfterGoingOnTests();
    PinsFileTests();
    ImportTests();
    PinTests();
    CloseTests();
    MaybeTests();
    SameProgramTests();
    PopupTests();
    AdminFrontTests();
    QuietFlashTests();
    PressTests();
    HelperTests();
    PressOutcomeTests();
    PinsGoldenTests();
    IconPixelTests();
    SameAppRuleTests();
    CheckPressTests();
    WatchLineTests();
    PinLaunchTests();
    SameAppDetailTests();
    IconSourceTests();
    PressClickValueTests();
    Reset();
    return Summary("apps_model_test");
}
