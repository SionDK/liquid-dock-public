// The OLED mode (oled*.cpp) with an in-memory desktop: monitors, Explorer's wallpaper API (IDesktopWallpaper),
// the desktop windows and a z-order. No real window is made, moved or covered, no wallpaper, desktop icon or
// other program is touched: every Windows call that would reach them is replaced below. Its record (oled.ini)
// and black picture go to preview-oled-data next to this test's exe, and are deleted at the end.
// One PASS/FAIL line per check; the exit code is the number of failures.
#include "../src/common.h"
#include "../src/oled.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shlguid.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <fstream>
#include <map>
#include <cstdio>
#include <cstdarg>
#include "portable_check.h"

// ---- what common.cpp gives the dock: a clock this test moves, its log, its data folder
static double g_now = 1000;
double NowSeconds() { return g_now; }
static std::vector<std::wstring> logs;
void Log(const wchar_t* fmt, ...) {
    wchar_t line[1024]; va_list ap; va_start(ap, fmt); _vsnwprintf_s(line, _TRUNCATE, fmt, ap); va_end(ap);
    logs.push_back(line);
}
std::wstring AppDataDir() {
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        dir = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\')) + L"\\preview-oled-data";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}
std::wstring ToLower(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size()); return s; }
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr); return s;
}
std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n); return w;
}
// as common.cpp: to <path>.tmp, flushed, swapped in; the old file stays when either step fails
bool SaveFileWhole(const std::wstring& path, const std::string& text, const wchar_t* what) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) &&
                    wrote == text.size() && FlushFileBuffers(f);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) { DeleteFileW(tmp.c_str()); Log(L"%s: could not be written", what); return false; }
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    Log(L"%s: could not replace", what);
    return false;
}

// ---- monitors, as the wallpaper API and the window manager see them
struct Monitor { HMONITOR handle; RECT rect; std::wstring id; bool attached = true; std::wstring wallpaper; };
static std::vector<Monitor> monitors;
static Monitor* ById(LPCWSTR id) {
    for (auto& m : monitors) if (id && m.id == id) return &m;
    return nullptr;
}
static BOOL WINAPI FakeEnumMonitors(HDC, LPCRECT, MONITORENUMPROC proc, LPARAM lp) {
    for (auto& m : monitors) if (m.attached && !proc(m.handle, nullptr, &m.rect, lp)) break;
    return TRUE;
}
static BOOL WINAPI FakeMonitorInfo(HMONITOR h, LPMONITORINFO mi) {
    for (auto& m : monitors) if (m.attached && m.handle == h) { mi->rcMonitor = mi->rcWork = m.rect; return TRUE; }
    return FALSE;
}
static HMONITOR WINAPI FakeMonitorFromRect(LPCRECT r, DWORD flags) {
    HMONITOR best = nullptr; LONG area = 0;
    for (auto& m : monitors) {
        RECT both{};
        if (m.attached && IntersectRect(&both, r, &m.rect) && (both.right - both.left) * (both.bottom - both.top) > area) {
            area = (both.right - both.left) * (both.bottom - both.top); best = m.handle;
        }
    }
    if (!best && flags != MONITOR_DEFAULTTONULL) for (auto& m : monitors) if (m.attached) return m.handle;
    return best;
}
static int WINAPI FakeMetrics(int index) {
    RECT all{};
    for (auto& m : monitors) if (m.attached) UnionRect(&all, &all, &m.rect);
    switch (index) {
    case SM_XVIRTUALSCREEN: return all.left;
    case SM_YVIRTUALSCREEN: return all.top;
    case SM_CXVIRTUALSCREEN: return all.right - all.left;
    case SM_CYVIRTUALSCREEN: return all.bottom - all.top;
    }
    return 0;
}

// ---- Explorer's wallpaper API (IDesktopWallpaper): per monitor, by device path
static bool wallpaperApi = true;   // false: Explorer is not running (CoCreateInstance fails)
static COLORREF background = RGB(10, 20, 30);
static DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
static int setWallpaperCalls = 0;
static int wallpaperAsks = 0, rectAsks = 0;   // the wallpaper API made (a call into Explorer) and monitor rectangles asked of it
static LPWSTR Copy(const std::wstring& s) {
    auto p = (LPWSTR)CoTaskMemAlloc((s.size() + 1) * sizeof(wchar_t)); wcscpy_s(p, s.size() + 1, s.c_str()); return p;
}
// For the checks in MoreRecordChecks: what the fakes below saw. Reset puts each back.
static std::wstring slideshowFolder;             // not empty: a slideshow runs from this folder (GetStatus, GetSlideshow)
static std::vector<std::wstring> slideshowSets;  // the folders SetSlideshow was given, in order
static bool slideshowStopped = false;            // GetStatus says no slideshow though GetSlideshow still has its folder
static bool parseFails = false;                  // SHCreateItemFromParsingName fails (the slideshow's folder is gone)
static std::wstring setIds;                      // the device paths SetWallpaper was given ("(all)" for none), '|' after each
static std::wstring failSetFor;                  // SetWallpaper fails for this device path
static bool touchArmed = false;                  // true: the next change to the desktop (colour, wallpaper) copies oled.ini ...
static std::string recordAtFirstTouch;           // ... here, as it was on the disk at that moment
static void Touch() {
    if (!touchArmed) return;
    touchArmed = false;
    std::ifstream in(AppDataDir() + L"\\oled.ini", std::ios::binary);
    recordAtFirstTouch.assign(std::istreambuf_iterator<char>(in), {});
}
// a shell item and an array of one, by path only (no real shell item is made)
struct FakeItem : IShellItem {
    std::wstring path;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IShellItem)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*, REFGUID, REFIID, void** out) override { *out = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(IShellItem** out) override { *out = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDisplayName(SIGDN sigdn, LPWSTR* name) override {
        if (sigdn != SIGDN_FILESYSPATH) { *name = nullptr; return E_INVALIDARG; }
        *name = Copy(path); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAttributes(SFGAOF, SFGAOF*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Compare(IShellItem*, SICHINTF, int*) override { return E_NOTIMPL; }
};
struct FakeItems : IShellItemArray {
    IShellItem* item = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IShellItemArray)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE BindToHandler(IBindCtx*, REFGUID, REFIID, void** out) override { *out = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPropertyStore(GETPROPERTYSTOREFLAGS, REFIID, void** out) override { *out = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPropertyDescriptionList(REFPROPERTYKEY, REFIID, void** out) override { *out = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAttributes(SIATTRIBFLAGS, SFGAOF, SFGAOF*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD* n) override { *n = item ? 1 : 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetItemAt(DWORD i, IShellItem** out) override {
        *out = nullptr; if (i != 0 || !item) return E_INVALIDARG;
        *out = item; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumItems(IEnumShellItems** out) override { *out = nullptr; return E_NOTIMPL; }
};
static FakeItem slideshowItem, parsedItem;
static FakeItems slideshowItems, parsedItems;
// SHCreateItemFromParsingName and SHCreateShellItemArrayFromShellItem for Disable's slideshow: the fake items above
static HRESULT WINAPI FakeParseName(PCWSTR path, IBindCtx*, REFIID riid, void** out) {
    *out = nullptr;
    if (parseFails || !path) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    parsedItem.path = path; return parsedItem.QueryInterface(riid, out);
}
static HRESULT WINAPI FakeArrayFromItem(IShellItem* item, REFIID riid, void** out) {
    *out = nullptr;
    if (!item) return E_INVALIDARG;
    parsedItems.item = item; return parsedItems.QueryInterface(riid, out);
}
struct FakeWallpaper : IDesktopWallpaper {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDesktopWallpaper)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetWallpaper(LPCWSTR id, LPCWSTR path) override {
        ++setWallpaperCalls;
        Touch(); setIds += id ? id : L"(all)"; setIds += L"|";
        if (id && !failSetFor.empty() && failSetFor == id) return E_FAIL;
        if (!id) { for (auto& m : monitors) m.wallpaper = path; return S_OK; }
        Monitor* m = ById(id);
        if (!m || !m->attached) return E_FAIL;
        m->wallpaper = path; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetWallpaper(LPCWSTR id, LPWSTR* path) override {
        Monitor* m = ById(id);
        if (!m) return E_FAIL;
        *path = Copy(m->wallpaper); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMonitorDevicePathAt(UINT i, LPWSTR* id) override {
        if (i >= monitors.size()) return E_INVALIDARG;
        *id = Copy(monitors[i].id); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMonitorDevicePathCount(UINT* n) override { *n = (UINT)monitors.size(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMonitorRECT(LPCWSTR id, RECT* r) override {
        ++rectAsks;
        Monitor* m = ById(id);
        if (!m || !m->attached) { *r = {}; return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED); }   // known, not attached
        *r = m->rect; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(COLORREF c) override { Touch(); background = c; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(COLORREF* c) override { *c = background; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPosition(DESKTOP_WALLPAPER_POSITION p) override { position = p; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPosition(DESKTOP_WALLPAPER_POSITION* p) override { *p = position; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetSlideshow(IShellItemArray* items) override {   // the folder of its first item, noted
        IShellItem* item = nullptr; LPWSTR path = nullptr; std::wstring got = L"(no folder)";
        if (items && SUCCEEDED(items->GetItemAt(0, &item)) && item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            got = path; CoTaskMemFree(path);
        }
        if (item) item->Release();
        slideshowSets.push_back(got); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSlideshow(IShellItemArray** items) override {
        if (slideshowFolder.empty()) { *items = nullptr; return E_NOTIMPL; }
        slideshowItem.path = slideshowFolder; slideshowItems.item = &slideshowItem; *items = &slideshowItems; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSlideshowOptions(DESKTOP_SLIDESHOW_OPTIONS, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSlideshowOptions(DESKTOP_SLIDESHOW_OPTIONS*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AdvanceSlideshow(LPCWSTR, DESKTOP_SLIDESHOW_DIRECTION) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStatus(DESKTOP_SLIDESHOW_STATE* state) override {   // no slideshow unless slideshowFolder is set
        *state = slideshowFolder.empty() || slideshowStopped ? (DESKTOP_SLIDESHOW_STATE)0 : DSS_ENABLED; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Enable(BOOL) override { return E_NOTIMPL; }
};
static FakeWallpaper wallpaper;

// ---- the desktop's folder view (the icons), reached as oled.cpp's DesktopView does: ShellWindows -> its desktop's
// dispatch -> IServiceProvider -> the top-level IShellBrowser -> its IShellView -> IFolderView2. There only while
// iconViewThere (the checks before MoreRecordChecks never reach it). Only these flags change.
static bool iconViewThere = false;
static DWORD folderFlags = 0;     // FWF_NOICONS: the desktop icons hidden
static int flagSets = 0;          // SetCurrentFolderFlags calls
static bool flagsFail = false;    // SetCurrentFolderFlags fails
struct FakeFolderView : IFolderView2 {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IFolderView) || riid == __uuidof(IFolderView2)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetCurrentFolderFlags(DWORD* flags) override { *flags = folderFlags; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCurrentFolderFlags(DWORD mask, DWORD flags) override {
        ++flagSets;
        if (flagsFail) return E_FAIL;
        folderFlags = (folderFlags & ~mask) | (flags & mask); return S_OK;
    }
    // the rest is never asked for
    HRESULT STDMETHODCALLTYPE GetCurrentViewMode(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetCurrentViewMode(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFolder(REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Item(int, PITEMID_CHILD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ItemCount(UINT, int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Items(UINT, REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSelectionMarkedItem(int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFocusedItem(int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetItemPosition(PCUITEMID_CHILD, POINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSpacing(POINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultSpacing(POINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAutoArrange() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SelectItem(int, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SelectAndPositionItems(UINT, PCUITEMID_CHILD_ARRAY, POINT*, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetGroupBy(REFPROPERTYKEY, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGroupBy(PROPERTYKEY*, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetViewProperty(PCUITEMID_CHILD, REFPROPERTYKEY, REFPROPVARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetViewProperty(PCUITEMID_CHILD, REFPROPERTYKEY, PROPVARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTileViewProperties(PCUITEMID_CHILD, LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetExtendedTileViewProperties(PCUITEMID_CHILD, LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetText(FVTEXTTYPE, LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSortColumnCount(int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetSortColumns(const SORTCOLUMN*, int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSortColumns(SORTCOLUMN*, int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetItem(int, REFIID, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVisibleItem(int, BOOL, int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSelectedItem(int, int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSelection(BOOL, IShellItemArray**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSelectionState(PCUITEMID_CHILD, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InvokeVerbOnSelection(LPCSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetViewModeAndIconSize(FOLDERVIEWMODE, int) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetViewModeAndIconSize(FOLDERVIEWMODE*, int*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetGroupSubsetCount(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGroupSubsetCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRedraw(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsMoveInSameFolder() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DoRename() override { return E_NOTIMPL; }
};
static FakeFolderView folderView;
struct FakeShellView : IShellView {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IOleWindow) || riid == __uuidof(IShellView)) { *out = this; return S_OK; }
        if (riid == __uuidof(IFolderView) || riid == __uuidof(IFolderView2)) return folderView.QueryInterface(riid, out);
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UIActivate(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Refresh() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateViewWindow(IShellView*, LPCFOLDERSETTINGS, IShellBrowser*, RECT*, HWND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DestroyViewWindow() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentInfo(LPFOLDERSETTINGS) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SaveViewState() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SelectItem(PCUITEMID_CHILD, SVSIF) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetItemObject(UINT, REFIID, void**) override { return E_NOTIMPL; }
};
static FakeShellView shellView;
struct FakeBrowser : IShellBrowser {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IOleWindow) || riid == __uuidof(IShellBrowser)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryActiveShellView(IShellView** view) override { *view = &shellView; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InsertMenusSB(HMENU, LPOLEMENUGROUPWIDTHS) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMenuSB(HMENU, HOLEMENU, HWND) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RemoveMenusSB(HMENU) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetStatusTextSB(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnableModelessSB(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TranslateAcceleratorSB(MSG*, WORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BrowseObject(PCUIDLIST_RELATIVE, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetViewStateStream(DWORD, IStream**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetControlWindow(UINT, HWND*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendControlMsg(UINT, UINT, WPARAM, LPARAM, LRESULT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnViewWindowActive(IShellView*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetToolbarItems(LPTBBUTTONSB, UINT, UINT) override { return E_NOTIMPL; }
};
static FakeBrowser browser;
struct FakeServices : IServiceProvider {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IServiceProvider)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID riid, void** out) override {
        if (service == SID_STopLevelBrowser) return browser.QueryInterface(riid, out);
        *out = nullptr; return E_NOINTERFACE;
    }
};
static FakeServices services;
struct FakeShellWindows : IShellWindows {   // also the desktop's dispatch FindWindowSW gives
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDispatch) || riid == __uuidof(IShellWindows)) { *out = this; return S_OK; }
        if (riid == __uuidof(IServiceProvider)) return services.QueryInterface(riid, out);
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE FindWindowSW(VARIANT* loc, VARIANT*, int swClass, long* hwnd, int, IDispatch** disp) override {
        *disp = nullptr;
        if (swClass != SWC_DESKTOP || !loc || loc->vt != VT_I4 || loc->lVal != CSIDL_DESKTOP) return S_FALSE;
        *hwnd = 0x10010; *disp = this; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_Count(long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Item(VARIANT, IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE _NewEnum(IUnknown**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Register(IDispatch*, long, int, long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterPending(long, VARIANT*, VARIANT*, int, long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Revoke(long) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnNavigate(long, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnActivated(long, VARIANT_BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnCreated(long, IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessAttachDetach(VARIANT_BOOL) override { return E_NOTIMPL; }
};
static FakeShellWindows shellWindows;
// The wallpaper API, or nothing (Explorer not running); the desktop's folder view only while iconViewThere (the
// checks before MoreRecordChecks never have it: the icons stay as they are).
static HRESULT WINAPI FakeCoCreate(REFCLSID clsid, LPUNKNOWN, DWORD, REFIID riid, LPVOID* out) {
    *out = nullptr;
    if (clsid == __uuidof(DesktopWallpaper)) ++wallpaperAsks;
    if (clsid == __uuidof(DesktopWallpaper) && wallpaperApi) return wallpaper.QueryInterface(riid, out);
    if (clsid == CLSID_ShellWindows && iconViewThere) return shellWindows.QueryInterface(riid, out);
    return REGDB_E_CLASSNOTREG;
}

// ---- top-level windows and their z-order (top first). The shell's desktop windows stay under HWND_BOTTOM.
struct Win { std::wstring cls; bool visible = true, iconic = false, cloaked = false, desktop = false, defView = false; RECT rect{}; DWORD pid = 500; };
static std::map<HWND, Win> wins;
static std::vector<HWND> z;
static INT_PTR nextHandle = 0x10000;
static HWND shell = nullptr;
static int hooks = 0, unhooks = 0, coverMoves = 0;
static std::vector<std::wstring> events;   // covers made and destroyed, in order
static int Index(HWND h) { auto it = std::find(z.begin(), z.end(), h); return it == z.end() ? -1 : (int)(it - z.begin()); }
static HWND Make(const wchar_t* cls, RECT r, bool desktop = false, DWORD pid = 500) {
    HWND h = (HWND)(nextHandle += 0x10); Win w; w.cls = cls; w.rect = r; w.desktop = desktop; w.pid = pid;
    wins[h] = w; z.push_back(h); return h;
}
static DWORD registerFails = 0, createFails = 0;   // a Windows error: the cover class or windows cannot be made
static HWND WINAPI FakeCreate(DWORD, LPCWSTR cls, LPCWSTR, DWORD style, int x, int y, int cx, int cy, HWND, HMENU, HINSTANCE, LPVOID) {
    if (createFails) { SetLastError(createFails); return nullptr; }
    HWND h = (HWND)(nextHandle += 0x10); Win w; w.cls = cls; w.visible = (style & WS_VISIBLE) != 0; w.rect = {x, y, x + cx, y + cy};
    w.pid = GetCurrentProcessId(); wins[h] = w; z.insert(z.begin(), h);   // a new window starts at the top of its band
    events.push_back(L"make " + std::to_wstring((INT_PTR)h)); return h;
}
static BOOL WINAPI FakeDestroy(HWND h) {
    if (!wins.erase(h)) return FALSE;
    z.erase(std::find(z.begin(), z.end(), h)); events.push_back(L"destroy " + std::to_wstring((INT_PTR)h)); return TRUE;
}
static BOOL WINAPI FakeIsWindow(HWND h) { return wins.count(h) != 0; }
static BOOL WINAPI FakeVisible(HWND h) { auto it = wins.find(h); return it != wins.end() && it->second.visible; }
static BOOL WINAPI FakeIconic(HWND h) { auto it = wins.find(h); return it != wins.end() && it->second.iconic; }
static BOOL WINAPI FakeRect(HWND h, LPRECT r) { auto it = wins.find(h); if (it == wins.end()) return FALSE; *r = it->second.rect; return TRUE; }
static int WINAPI FakeClass(HWND h, LPWSTR out, int n) {
    auto it = wins.find(h); if (it == wins.end() || n <= 0) return 0;
    std::wstring c = it->second.cls.substr(0, n - 1); wcscpy_s(out, n, c.c_str()); return (int)c.size();
}
static DWORD WINAPI FakePid(HWND h, LPDWORD pid) { auto it = wins.find(h); if (pid) *pid = it == wins.end() ? 0 : it->second.pid; return 1; }
static HRESULT WINAPI FakeAttribute(HWND h, DWORD what, PVOID value, DWORD) {
    auto it = wins.find(h); if (it == wins.end()) return E_HANDLE;
    if (what == DWMWA_CLOAKED) { *(DWORD*)value = it->second.cloaked ? 1 : 0; return S_OK; }
    return E_INVALIDARG;
}
static HRESULT WINAPI FakeSetAttribute(HWND, DWORD, LPCVOID, DWORD) { return S_OK; }
static BOOL WINAPI FakeLayered(HWND, COLORREF, BYTE, DWORD) { return TRUE; }
static HWND WINAPI FakeShell() { return shell; }
static HWND WINAPI FakeGetWindow(HWND h, UINT cmd) {
    int i = Index(h); if (i < 0) return nullptr;
    if (cmd == GW_HWNDNEXT) return i + 1 < (int)z.size() ? z[i + 1] : nullptr;
    if (cmd == GW_HWNDPREV) return i > 0 ? z[i - 1] : nullptr;
    return nullptr;
}
static HWND WINAPI FakeFind(HWND parent, HWND after, LPCWSTR cls, LPCWSTR) {
    if (parent) {   // the only child asked for: the desktop's icon view
        auto it = wins.find(parent);
        return it != wins.end() && it->second.defView && cls && wcscmp(cls, L"SHELLDLL_DefView") == 0 ? (HWND)((INT_PTR)parent + 1) : nullptr;
    }
    if (after && Index(after) < 0) return nullptr;
    for (int j = after ? Index(after) + 1 : 0; j < (int)z.size(); ++j)   // the windows below `after`
        if (wins[z[j]].cls == cls) return z[j];
    return nullptr;
}
static bool IsCoverWindow(HWND h) { auto it = wins.find(h); return it != wins.end() && it->second.cls == L"LiquidDockOledCover"; }
static BOOL WINAPI FakePos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!wins.count(h)) return FALSE;
    Win& w = wins[h];
    if (!(flags & SWP_NOMOVE)) OffsetRect(&w.rect, x - w.rect.left, y - w.rect.top);
    if (!(flags & SWP_NOSIZE)) { w.rect.right = w.rect.left + cx; w.rect.bottom = w.rect.top + cy; }
    if (flags & SWP_SHOWWINDOW) w.visible = true;
    if (flags & SWP_NOZORDER) return TRUE;
    if (after != HWND_BOTTOM && after != HWND_TOP && Index(after) < 0) return FALSE;
    if (after == h) return TRUE;
    z.erase(std::find(z.begin(), z.end(), h));
    size_t at = 0;
    if (after == HWND_BOTTOM) { at = z.size(); while (at > 0 && wins[z[at - 1]].desktop) --at; }
    else if (after != HWND_TOP) at = (size_t)Index(after) + 1;
    z.insert(z.begin() + at, h);
    if (IsCoverWindow(h)) ++coverMoves;
    return TRUE;
}
static HWINEVENTHOOK WINAPI FakeHook(DWORD, DWORD, HMODULE, WINEVENTPROC, DWORD, DWORD, DWORD) { ++hooks; return (HWINEVENTHOOK)0x77; }
static BOOL WINAPI FakeUnhook(HWINEVENTHOOK) { ++unhooks; return TRUE; }
static ATOM WINAPI FakeRegisterClass(const WNDCLASSEXW*) {
    if (registerFails) { SetLastError(registerFails); return 0; }
    return (ATOM)0xC001;
}

#define CoCreateInstance FakeCoCreate
#define EnumDisplayMonitors FakeEnumMonitors
#define GetMonitorInfoW FakeMonitorInfo
#define MonitorFromRect FakeMonitorFromRect
#define GetSystemMetrics FakeMetrics
#define CreateWindowExW FakeCreate
#define DestroyWindow FakeDestroy
#define IsWindow FakeIsWindow
#define IsWindowVisible FakeVisible
#define IsIconic FakeIconic
#define GetWindowRect FakeRect
#define GetClassNameW FakeClass
#define GetWindowThreadProcessId FakePid
#define DwmGetWindowAttribute FakeAttribute
#define DwmSetWindowAttribute FakeSetAttribute
#define SetLayeredWindowAttributes FakeLayered
#define GetShellWindow FakeShell
#define GetWindow FakeGetWindow
#define FindWindowExW FakeFind
#define SetWindowPos FakePos
#define SetWinEventHook FakeHook
#define UnhookWinEvent FakeUnhook
#define RegisterClassExW FakeRegisterClass
#define SHCreateItemFromParsingName FakeParseName
#define SHCreateShellItemArrayFromShellItem FakeArrayFromItem
#include "../src/oled.cpp"
#include "../src/oled_covers.cpp"

static const HMONITOR kMain = (HMONITOR)0x101, kSide = (HMONITOR)0x102;
static const RECT kMainRect{0, 0, 2560, 1440}, kSideRect{-1440, -560, 0, 2000};   // a desktop's two monitors, one of them portrait
static std::wstring Black() { return AppDataDir() + L"\\oled_black.bmp"; }
static std::wstring Record() { return AppDataDir() + L"\\oled.ini"; }
static bool HasRecord() { return GetFileAttributesW(Record().c_str()) != INVALID_FILE_ATTRIBUTES; }
static void Reset() {
    DeleteFileW(Record().c_str()); DeleteFileW((Record() + L".tmp").c_str()); DeleteFileW(Black().c_str());
    monitors = {{kMain, kMainRect, L"\\\\?\\DISPLAY#QDO#main", true, L"C:\\Wallpapers\\main.jpg"},
                {kSide, kSideRect, L"\\\\?\\DISPLAY#SIDE#side", true, L"C:\\Wallpapers\\side.jpg"}};
    wallpaperApi = true; position = DWPOS_FILL; background = RGB(10, 20, 30);
    // the fakes MoreRecordChecks reads: no slideshow, no icon view, nothing failing (as the checks before them had)
    slideshowFolder.clear(); slideshowStopped = false; parseFails = false; failSetFor.clear(); touchArmed = false;
    iconViewThere = false; folderFlags = 0; flagSets = 0; flagsFail = false;
}
// The dock's main loop for `seconds`, 60 passes a second: oled::Tick each pass, then `each` (another program's doing).
static void Run(double seconds, const std::function<void()>& each = {}) {
    for (double end = g_now + seconds; g_now < end; g_now += 1.0 / 60) { oled::Tick(g_now); if (each) each(); }
}
static std::vector<HWND> Covers() { std::vector<HWND> c; for (HWND h : z) if (IsCoverWindow(h)) c.push_back(h); return c; }
static int CountLogs(const wchar_t* part) { int n = 0; for (auto& l : logs) n += l.find(part) != std::wstring::npos; return n; }
// The covers lie together, right above `host`, with every window in `above` over them.
static bool RestRightAbove(HWND host, std::initializer_list<HWND> above) {
    int at = Index(host); auto covers = Covers();
    if (covers.empty() || at < (int)covers.size()) return false;
    for (int k = 1; k <= (int)covers.size(); ++k) if (!IsCoverWindow(z[at - k])) return false;
    for (HWND w : above) if (Index(w) > at - (int)covers.size()) return false;
    return true;
}

// ---- oled.ini and oled_black.bmp byte for byte: the record an older dock left must still put the wallpapers back
// (the round trip above passes when writing and reading change together). Every value is what mod.35 does;
// oled.ini's form has not changed since mod.21 base.
static std::string ReadBytes(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
static void WriteBytes(const std::wstring& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << bytes;
}
static bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string Escaped(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else if (c == '\t') o += "\\t";
        else if (c < 0x20 || c >= 0x7F) { char b[8]; snprintf(b, sizeof b, "\\x%02X", c); o += b; } else o += (char)c;
    }
    return o;
}
static void CheckSame(const std::string& got, const std::string& want, const char* what) {
    if (!Check(got == want, what)) std::printf("  got:  %s\n  want: %s\n", Escaped(got).c_str(), Escaped(want).c_str());
}
static std::string DumpState(const oled::State& s) {
    std::string d = "active=" + std::to_string(s.active) + " iconsHidden=" + std::to_string(s.iconsHidden) +
                    " allMonitors=" + std::to_string(s.allMonitors) + " background=" + std::to_string((unsigned long)s.background) +
                    " slideshow=[" + ToUtf8(s.slideshow) + "]";
    for (auto& [id, path] : s.wallpapers) d += " {" + ToUtf8(id) + "}=[" + ToUtf8(path) + "]";
    return d;
}
static std::string LoadBytes(const std::string& bytes) { WriteBytes(Record(), bytes); return DumpState(oled::Load()); }
static std::string Logs() { std::string all; for (auto& l : logs) all += ToUtf8(l) + "|"; return all; }

// the two monitors' record after the mode came on for both (Reset's wallpapers, background RGB(10, 20, 30))
static const char kBothRecord[] =
    "active=1\niconsHidden=0\nallMonitors=1\nbackground=1971210\nslideshow=\n"
    "wallpaper=\\\\?\\DISPLAY#QDO#main\tC:\\Wallpapers\\main.jpg\nwallpaper=\\\\?\\DISPLAY#SIDE#side\tC:\\Wallpapers\\side.jpg\n";

static void RecordGoldenChecks() {
    // ---- what Enable writes
    {
        Reset(); logs.clear();
        oled::Enable(nullptr);
        CheckSame(ReadBytes(Record()), kBothRecord, "oled.ini golden: every monitor - LF lines, background in decimal, wallpapers by device path");
        CheckSame(Logs(), "oled: desktop view unavailable|oled: on (all, 2 monitor(s), wallpaper 1, icons -1)|",
                  "...dock.log: the icon view could not be reached, then the on line");
        Check(!Exists(Record() + L".tmp"), "...no temporary record left");
        logs.clear(); oled::Disable();
        CheckSame(Logs(), "oled: off|", "...off: dock.log says only that");
        Reset(); oled::Enable(kMain);
        CheckSame(ReadBytes(Record()),
                  "active=1\niconsHidden=0\nallMonitors=0\nbackground=1971210\nslideshow=\nwallpaper=\\\\?\\DISPLAY#QDO#main\tC:\\Wallpapers\\main.jpg\n",
                  "oled.ini golden: one monitor - allMonitors=0 and its wallpaper alone");
        oled::Disable();
        Reset(); position = DWPOS_SPAN; oled::Enable(kMain);
        CheckSame(ReadBytes(Record()), kBothRecord, "oled.ini golden: one monitor asked for with a spanned wallpaper - recorded as every monitor");
        Check(monitors[0].wallpaper == Black() && monitors[1].wallpaper == Black(), "...and both made black");
        oled::Disable();
    }
    // ---- what Save writes for every field: map order (by device path, upper case first), UTF-8, a tab inside a path
    {
        oled::State st;
        st.active = true; st.iconsHidden = true; st.allMonitors = false; st.background = 0xFFFFFFFF;
        st.slideshow = L"D:\\사진\\슬라이드";
        st.wallpapers[L"b"] = L"x"; st.wallpapers[L"B"] = L"y"; st.wallpapers[L"a"] = L"C:\\탭\tpath.jpg"; st.wallpapers[L"e"] = L"";
        Reset();
        Check(oled::Save(st), "Save of every field: written");
        const std::string want = "active=1\niconsHidden=1\nallMonitors=0\nbackground=4294967295\nslideshow=D:\\사진\\슬라이드\n"
                                 "wallpaper=B\ty\nwallpaper=a\tC:\\탭\tpath.jpg\nwallpaper=b\tx\nwallpaper=e\t\n";
        CheckSame(ReadBytes(Record()), want, "oled.ini golden: every field, wallpapers sorted by path, Korean in UTF-8");
        CheckSame(DumpState(oled::Load()), DumpState(st), "...read back whole: the tab after the first one stays in the path, the empty path too");
        oled::State none;
        Check(oled::Save(none), "Save of an empty state: written");
        CheckSame(ReadBytes(Record()), "active=0\niconsHidden=0\nallMonitors=1\nbackground=0\nslideshow=\n", "oled.ini golden: an empty state");
        DeleteFileW(Record().c_str());
    }
    // ---- reading: the bytes an older dock left, edited ones, broken ones
    {
        Reset();
        const std::string both = "active=1 iconsHidden=0 allMonitors=1 background=1971210 slideshow=[] "
                                 "{\\\\?\\DISPLAY#QDO#main}=[C:\\Wallpapers\\main.jpg] {\\\\?\\DISPLAY#SIDE#side}=[C:\\Wallpapers\\side.jpg]";
        CheckSame(LoadBytes(kBothRecord), both, "oled.ini read: the record's every field");
        std::string crlf; for (char c : std::string(kBothRecord)) { if (c == '\n') crlf += '\r'; crlf += c; }
        CheckSame(LoadBytes(crlf), both, "oled.ini read: CRLF lines read the same");
        std::string noEnd = kBothRecord; noEnd.pop_back();
        CheckSame(LoadBytes(noEnd), both, "oled.ini read: no line end after the last line");
        CheckSame(LoadBytes(std::string("\xEF\xBB\xBF") + kBothRecord),
                  "active=0 iconsHidden=0 allMonitors=1 background=1971210 slideshow=[] "
                  "{\\\\?\\DISPLAY#QDO#main}=[C:\\Wallpapers\\main.jpg] {\\\\?\\DISPLAY#SIDE#side}=[C:\\Wallpapers\\side.jpg]",
                  "oled.ini read: a UTF-8 BOM is not skipped - the first key (active) is lost, the rest read");
        CheckSame(LoadBytes(""), "active=0 iconsHidden=0 allMonitors=1 background=0 slideshow=[]", "oled.ini read: an empty file is the mode off");
        CheckSame(LoadBytes("active=1\niconsHidden=1\nallMon"), "active=1 iconsHidden=1 allMonitors=1 background=0 slideshow=[]",
                  "oled.ini read: cut off in the middle (power loss): what came before is kept, the rest is its default");
        CheckSame(LoadBytes("active=yes\niconsHidden=2\nallMonitors=0x\nbackground=abc\nslideshow=a=b\n=x\nnothing\nActive=1\n"
                            "wallpaper=no tab\nwallpaper=X\tB\tC\nwallpaper=A\tfirst\nwallpaper=A\tsecond\nwallpaper=\tno id\n"),
                  "active=0 iconsHidden=1 allMonitors=0 background=0 slideshow=[a=b] {}=[no id] {A}=[second] {X}=[B\tC]",
                  "oled.ini read: malformed numbers keep the default, a line without a tab is skipped, the last of a path wins");
        CheckSame(LoadBytes("background=4294967295\n"), "active=0 iconsHidden=0 allMonitors=1 background=4294967295 slideshow=[]",
                  "oled.ini read: background 4294967295 (white with the top byte)");
        CheckSame(LoadBytes("background=-1\n"), "active=0 iconsHidden=0 allMonitors=1 background=4294967295 slideshow=[]",
                  "oled.ini read: background -1 wraps to 4294967295");
        CheckSame(LoadBytes("background=7\nbackground=4294967296\n"), "active=0 iconsHidden=0 allMonitors=1 background=7 slideshow=[]",
                  "oled.ini read: background 4294967296 is too large: the last good value stays");
        CheckSame(LoadBytes("background= 255junk\nactive=-3\n"), "active=1 iconsHidden=0 allMonitors=1 background=255 slideshow=[]",
                  "oled.ini read: a leading blank and trailing junk are taken; -3 is on");
        CheckSame(LoadBytes("background=0x10\n"), "active=0 iconsHidden=0 allMonitors=1 background=0 slideshow=[]",
                  "oled.ini read: background in hex is read as 0");
        CheckSame(LoadBytes("wallpaper=M\t\xFF.jpg\nslideshow=\xC3\n"), "active=0 iconsHidden=0 allMonitors=1 background=0 slideshow=[\xEF\xBF\xBD] {M}=[\xEF\xBF\xBD.jpg]",
                  "oled.ini read: bytes that are not UTF-8 become U+FFFD");
        CheckSame(LoadBytes("active=1\r\r\nslideshow=S\r\r\n"), "active=1 iconsHidden=0 allMonitors=1 background=0 slideshow=[S\r]",
                  "oled.ini read: CR CR LF - read as bytes, one CR is dropped and the other stays in a text value");
        DeleteFileW(Record().c_str());
    }
    // ---- an older dock's record puts the wallpapers back (the mode was on through an update)
    {
        Reset();
        std::wstring upper = Black(); CharUpperBuffW(upper.data(), (DWORD)upper.size());
        monitors[0].wallpaper = Black(); monitors[1].wallpaper = upper; background = RGB(0, 0, 0);
        WriteBytes(Record(), "active=1\r\niconsHidden=0\r\nallMonitors=1\r\nbackground=1971210\r\nslideshow=\r\n"
                             "wallpaper=\\\\?\\DISPLAY#GONE#old\tC:\\Wallpapers\\gone.jpg\r\n"
                             "wallpaper=\\\\?\\DISPLAY#QDO#main\tC:\\Wallpapers\\옛 배경.jpg\r\n"
                             "wallpaper=\\\\?\\DISPLAY#SIDE#side\tC:\\Wallpapers\\side.jpg\r\n");
        logs.clear();
        Check(oled::Load().active && oled::Load().wallpapers.size() == 3, "fixture: the older dock's record in place, three monitors in it");
        const bool off = oled::Disable();
        Check(off && monitors[0].wallpaper == L"C:\\Wallpapers\\옛 배경.jpg" && monitors[1].wallpaper == L"C:\\Wallpapers\\side.jpg",
              "an older dock's record (CRLF, a monitor gone): off puts each wallpaper back, a black one in other letter case too");
        Check(background == RGB(10, 20, 30) && !HasRecord(), "...the colour back, the record gone");
        CheckSame(Logs(), "oled: off|", "...and dock.log says only that");
        // the user picked another wallpaper meanwhile on one monitor: only the black one is put back
        Reset(); monitors[0].wallpaper = L"C:\\Wallpapers\\new.jpg"; monitors[1].wallpaper = Black();
        WriteBytes(Record(), kBothRecord);
        oled::Disable();
        Check(monitors[0].wallpaper == L"C:\\Wallpapers\\new.jpg" && monitors[1].wallpaper == L"C:\\Wallpapers\\side.jpg",
              "...a monitor whose wallpaper is not the black picture keeps its own");
        // a record without the active line (or active=0): off does nothing, the record stays
        Reset(); monitors[0].wallpaper = Black(); const int calls = setWallpaperCalls;
        WriteBytes(Record(), "iconsHidden=0\nwallpaper=\\\\?\\DISPLAY#QDO#main\tC:\\Wallpapers\\main.jpg\n");
        Check(oled::Disable() && setWallpaperCalls == calls && HasRecord() && monitors[0].wallpaper == Black(),
              "a record without active=1: off says done, changes nothing and leaves the file");
        DeleteFileW(Record().c_str());
    }
    // ---- oled_black.bmp: made when missing, before the record; never rewritten
    {
        Reset();
        oled::Enable(nullptr);
        const std::string bmp = ReadBytes(Black());
        const std::string header("BM" "\x36\x03\x00\x00" "\x00\x00\x00\x00" "\x36\x00\x00\x00"
                                 "\x28\x00\x00\x00" "\x10\x00\x00\x00" "\x10\x00\x00\x00" "\x01\x00" "\x18\x00" "\x00\x00\x00\x00"
                                 "\x00\x03\x00\x00" "\x00\x00\x00\x00" "\x00\x00\x00\x00" "\x00\x00\x00\x00" "\x00\x00\x00\x00", 54);
        CheckSame(bmp.substr(0, 54), header, "oled_black.bmp golden: a 16x16 24-bit bottom-up BMP header (822 bytes, pixels at 54)");
        Check(bmp.size() == 822 && bmp.find_first_not_of('\0', 54) == std::string::npos, "...768 bytes of black pixels, nothing more");
        oled::Disable();
        Check(ReadBytes(Black()) == bmp, "off leaves the black picture in place");
        // a file of that name is taken as it is, whatever it holds
        Reset(); WriteBytes(Black(), "junk");
        Check(oled::Enable(nullptr) && ReadBytes(Black()) == "junk" && monitors[0].wallpaper == Black(),
              "an oled_black.bmp already there is used as it is, never rewritten");
        oled::Disable();
        // no wallpaper API: nothing is made
        Reset(); wallpaperApi = false; logs.clear();
        Check(!oled::Enable(nullptr) && !Exists(Black()) && !HasRecord(), "no wallpaper API: on fails, no picture and no record made");
        CheckSame(Logs(), "oled: wallpaper api unavailable 80040154|", "...dock.log says why");
        // the API there but no monitor in it: the picture is made, the mode does not come on
        Reset(); monitors.clear(); logs.clear();
        Check(!oled::Enable(nullptr) && Exists(Black()) && ReadBytes(Black()).size() == 822 && !HasRecord(),
              "no monitor in the wallpaper API: on fails after the picture is made, no record");
        CheckSame(Logs(), "", "...and dock.log has no line for it");
        // already on (the record says so): nothing is made or changed
        Reset(); WriteBytes(Record(), "active=1\n"); const int calls = setWallpaperCalls;
        Check(oled::Enable(nullptr) && !Exists(Black()) && setWallpaperCalls == calls && ReadBytes(Record()) == "active=1\n",
              "the record already says on: on is done at once, no picture made, the record as it was");
        DeleteFileW(Record().c_str());
        // the record cannot be written: nothing is changed
        Reset(); CreateDirectoryW((Record() + L".tmp").c_str(), nullptr); logs.clear();
        Check(!oled::Enable(nullptr) && !HasRecord() && monitors[0].wallpaper == L"C:\\Wallpapers\\main.jpg" && background == RGB(10, 20, 30),
              "the record cannot be written: on fails before any wallpaper or the colour is changed");
        CheckSame(Logs(), "oled: could not be written|oled: could not save state|", "...dock.log: the save, then why the mode stays off");
        Check(Exists(Black()), "...the black picture is made before the record");
        RemoveDirectoryW((Record() + L".tmp").c_str());
    }
    Reset(); g_now += 3; Run(0.1);
    Check(Covers().empty() && !oled::Active(), "record checks: left with the mode off and no cover");
}

// ---- MoreRecordChecks: the record against the desktop's changes, the icon view, the slideshow, and the
// branches of Enable and Disable that had a check on one side only. Every value is what mod.35 does.
static const char kMainAllRecord[] =   // every monitor, when only the main one is there
    "active=1\niconsHidden=0\nallMonitors=1\nbackground=1971210\nslideshow=\nwallpaper=\\\\?\\DISPLAY#QDO#main\tC:\\Wallpapers\\main.jpg\n";
static const char kMainOneRecord[] =   // the main monitor asked for
    "active=1\niconsHidden=0\nallMonitors=0\nbackground=1971210\nslideshow=\nwallpaper=\\\\?\\DISPLAY#QDO#main\tC:\\Wallpapers\\main.jpg\n";
static std::string WithLine(std::string record, const std::string& from, const std::string& to) {
    record.replace(record.find(from), from.size(), to); return record;
}

static void MoreRecordChecks() {
    // ---- the record is on the disk, whole and saying on, before the desktop is changed (oled.cpp: "before touching anything")
    {
        Reset(); touchArmed = true; recordAtFirstTouch = "(nothing changed)";
        oled::Enable(nullptr);
        CheckSame(recordAtFirstTouch, kBothRecord, "on: oled.ini on the disk when the colour is first changed is the whole record, active=1");
        oled::Disable();
        Reset(); position = DWPOS_SPAN; touchArmed = true; recordAtFirstTouch = "(nothing changed)";
        oled::Enable(kMain);
        CheckSame(recordAtFirstTouch, kBothRecord, "...the same with a spanned wallpaper");
        oled::Disable();
    }
    // ---- dock.log lines of the other branches, whole
    {
        Reset(); logs.clear(); oled::Enable(kMain);
        CheckSame(Logs(), "oled: desktop view unavailable|oled: on (one, 1 monitor(s), wallpaper 1, icons -1)|",
                  "dock.log on for one monitor: 'one, 1 monitor(s)'");
        wallpaperApi = false; logs.clear();
        Check(!oled::Disable() && HasRecord(), "off without the wallpaper API: fails, the record stays");
        CheckSame(Logs(), "oled: wallpaper api unavailable 80040154|oled: off failed (no wallpaper api) - kept on for the next try|",
                  "...dock.log: the API's error, then the whole off-failed line");
        wallpaperApi = true; oled::Disable();
    }
    // ---- which SetWallpaper calls: one without a monitor for a spanned wallpaper, else one per monitor by device path
    {
        Reset(); position = DWPOS_SPAN; setIds.clear(); const int calls = setWallpaperCalls;
        oled::Enable(kMain);
        Check(setWallpaperCalls - calls == 1 && setIds == L"(all)|", "spanned: on makes one SetWallpaper call, with no monitor named");
        setIds.clear(); oled::Disable();
        CheckSame(ToUtf8(setIds), "\\\\?\\DISPLAY#QDO#main|\\\\?\\DISPLAY#SIDE#side|", "...off puts each monitor's own back by its device path, in the record's order");
        Reset(); setIds.clear(); oled::Enable(nullptr);
        CheckSame(ToUtf8(setIds), "\\\\?\\DISPLAY#QDO#main|\\\\?\\DISPLAY#SIDE#side|", "not spanned, every monitor: one call each, in the wallpaper API's order");
        oled::Disable();
    }
    // ---- the monitors: one Explorer knows but not attached, one Windows does not know, a PC with one monitor
    {
        Reset(); monitors[1].attached = false; logs.clear();
        oled::Enable(nullptr);
        CheckSame(ReadBytes(Record()), kMainAllRecord, "a monitor Explorer knows but that is not attached: left out of the record");
        CheckSame(Logs(), "oled: desktop view unavailable|oled: on (all, 1 monitor(s), wallpaper 1, icons -1)|", "...and of dock.log's count");
        Check(monitors[1].wallpaper == L"C:\\Wallpapers\\side.jpg", "...its wallpaper left alone");
        oled::Disable();
        Reset(); oled::Enable((HMONITOR)0x999);
        CheckSame(ReadBytes(Record()), kBothRecord, "a monitor asked for that Windows does not know: every monitor");
        oled::Disable();
        Reset(); monitors.resize(1); logs.clear(); oled::Enable(nullptr);
        CheckSame(ReadBytes(Record()), kMainAllRecord, "a PC with one monitor, the mode for all: allMonitors=1");
        CheckSame(Logs(), "oled: desktop view unavailable|oled: on (all, 1 monitor(s), wallpaper 1, icons -1)|", "...dock.log 'all, 1 monitor(s)'");
        oled::Disable();
        Reset(); monitors.resize(1); oled::Enable(kMain);
        CheckSame(ReadBytes(Record()), kMainOneRecord, "a PC with one monitor, that one asked for: allMonitors=0");
        oled::Disable();
    }
    // ---- a wallpaper that cannot be set: on all the same, dock.log says wallpaper 0
    {
        Reset(); failSetFor = L"\\\\?\\DISPLAY#SIDE#side"; logs.clear();
        Check(oled::Enable(nullptr) && HasRecord() && monitors[0].wallpaper == Black() && monitors[1].wallpaper == L"C:\\Wallpapers\\side.jpg",
              "a wallpaper that cannot be set: on all the same, the other monitor black");
        CheckSame(Logs(), "oled: desktop view unavailable|oled: on (all, 2 monitor(s), wallpaper 0, icons -1)|", "...dock.log says wallpaper 0");
        CheckSame(ReadBytes(Record()), kBothRecord, "...the record holds both monitors");
        failSetFor.clear();
        Check(oled::Disable() && monitors[0].wallpaper == L"C:\\Wallpapers\\main.jpg" && monitors[1].wallpaper == L"C:\\Wallpapers\\side.jpg",
              "...off puts the black one back, the other was never changed");
    }
    // ---- a record without a background line: off sets the colour 0 (black)
    {
        Reset(); monitors[0].wallpaper = Black(); background = RGB(1, 2, 3);
        WriteBytes(Record(), "active=1\niconsHidden=0\nallMonitors=1\nwallpaper=\\\\?\\DISPLAY#QDO#main\tC:\\Wallpapers\\main.jpg\n");
        Check(oled::Disable() && background == RGB(0, 0, 0) && monitors[0].wallpaper == L"C:\\Wallpapers\\main.jpg" && !HasRecord(),
              "a record without a background line: off sets the colour 0, the wallpaper back, the record gone");
    }
    // ---- the icons without the icon view: iconsHidden in the record decides whether off and Reapply reach for it
    {
        Reset(); monitors[0].wallpaper = Black();
        WriteBytes(Record(), WithLine(kMainAllRecord, "iconsHidden=0", "iconsHidden=1"));
        logs.clear(); oled::Reapply();
        CheckSame(Logs(), "oled: desktop view unavailable|", "Reapply, on with the icons it hid: reaches for the icon view (none here)");
        logs.clear();
        Check(oled::Disable() && !HasRecord(), "off with iconsHidden=1 and no icon view: off all the same");
        CheckSame(Logs(), "oled: desktop view unavailable|oled: off|", "...dock.log: the icon view first, then the off line");
        WriteBytes(Record(), "active=1\niconsHidden=0\n"); logs.clear(); oled::Reapply();
        CheckSame(Logs(), "", "Reapply with iconsHidden=0: nothing");
        WriteBytes(Record(), "active=0\niconsHidden=1\n"); logs.clear(); oled::Reapply();
        CheckSame(Logs(), "", "Reapply with the mode off: nothing");
        DeleteFileW(Record().c_str()); logs.clear(); oled::Reapply();
        CheckSame(Logs(), "", "Reapply with no record: nothing");
        logs.clear();
        Check(!oled::SetDesktopIconsVisible(true), "SetDesktopIconsVisible without the icon view: false");
        CheckSame(Logs(), "oled: desktop view unavailable|", "...dock.log says the icon view is not there");
    }
    // ---- the icon view there (fake): on hides the icons and records that, off and Reapply follow the record
    {
        const std::string hidden = WithLine(kBothRecord, "iconsHidden=0", "iconsHidden=1");
        Reset(); iconViewThere = true; folderFlags = FWF_AUTOARRANGE; logs.clear();
        Check(oled::Enable(nullptr) && folderFlags == (FWF_AUTOARRANGE | FWF_NOICONS) && flagSets == 1,
              "icon view, icons shown: on hides them (FWF_NOICONS alone set, one call)");
        CheckSame(ReadBytes(Record()), hidden, "...the record written again after it: iconsHidden=1");
        CheckSame(Logs(), "oled: on (all, 2 monitor(s), wallpaper 1, icons 1)|", "...dock.log 'icons 1', no other line");
        oled::Reapply();
        Check(flagSets == 1, "...Reapply while they are hidden: nothing set");
        folderFlags = FWF_AUTOARRANGE; oled::Reapply();   // Explorer restarted: its desktop shows the icons again
        Check(folderFlags == (FWF_AUTOARRANGE | FWF_NOICONS) && flagSets == 2, "...Explorer restarted with its icons shown: Reapply hides them again");
        logs.clear();
        Check(oled::Disable() && folderFlags == FWF_AUTOARRANGE && flagSets == 3, "...off shows the icons it hid");
        CheckSame(Logs(), "oled: off|", "...dock.log says only that");
        // hidden already by the user: not the mode's doing, left hidden
        Reset(); iconViewThere = true; folderFlags = FWF_NOICONS; logs.clear();
        Check(oled::Enable(nullptr) && folderFlags == FWF_NOICONS && flagSets == 0, "icon view, icons hidden already: on sets nothing");
        CheckSame(ReadBytes(Record()), kBothRecord, "...the record says iconsHidden=0");
        CheckSame(Logs(), "oled: on (all, 2 monitor(s), wallpaper 1, icons 0)|", "...dock.log 'icons 0'");
        folderFlags = 0; oled::Reapply();
        Check(folderFlags == 0 && flagSets == 0, "...Reapply does not hide icons the mode did not hide");
        folderFlags = FWF_NOICONS;
        Check(oled::Disable() && folderFlags == FWF_NOICONS && flagSets == 0, "...off leaves them hidden");
        // the flag cannot be set: icons -1, not recorded as hidden
        Reset(); iconViewThere = true; flagsFail = true; logs.clear();
        Check(oled::Enable(nullptr) && folderFlags == 0 && flagSets == 1, "icon view whose flag cannot be set: on all the same");
        CheckSame(ReadBytes(Record()), kBothRecord, "...the record says iconsHidden=0 here too");
        CheckSame(Logs(), "oled: on (all, 2 monitor(s), wallpaper 1, icons -1)|", "...dock.log 'icons -1', no other line");
        flagsFail = false; oled::Disable();
        Check(flagSets == 1, "...off does not reach for the icons");
        // SetDesktopIconsVisible (the developer's message): true whenever the view answered and the flag is as asked
        Reset(); iconViewThere = true;
        Check(oled::SetDesktopIconsVisible(false) && folderFlags == FWF_NOICONS && flagSets == 1, "SetDesktopIconsVisible(false): hidden, true");
        Check(oled::SetDesktopIconsVisible(false) && folderFlags == FWF_NOICONS && flagSets == 1, "...again while hidden: true, nothing set");
        Check(oled::SetDesktopIconsVisible(true) && folderFlags == 0 && flagSets == 2, "...(true): shown, true");
        flagsFail = true;
        Check(!oled::SetDesktopIconsVisible(false) && folderFlags == 0, "...the flag cannot be set: false");
        Reset();
    }
    // ---- the slideshow (fake): its folder recorded at on, started again from it at off
    {
        Reset(); slideshowFolder = L"D:\\사진\\슬라이드쇼"; slideshowSets.clear();
        oled::Enable(nullptr);
        CheckSame(ReadBytes(Record()), WithLine(kBothRecord, "slideshow=\n", "slideshow=D:\\사진\\슬라이드쇼\n"),
                  "a slideshow running: on records its folder (UTF-8)");
        slideshowFolder.clear();   // the black picture ended it
        Check(oled::Disable() && slideshowSets.size() == 1 && slideshowSets[0] == L"D:\\사진\\슬라이드쇼", "...off starts it again from that folder, once");
        Reset(); slideshowSets.clear(); oled::Enable(nullptr);
        Check(oled::Disable() && slideshowSets.empty(), "no slideshow: off starts none");
        Reset(); slideshowFolder = L"D:\\old"; slideshowStopped = true; slideshowSets.clear(); oled::Enable(nullptr);
        CheckSame(ReadBytes(Record()), kBothRecord, "a slideshow folder left from before but not running: not recorded");
        Check(oled::Disable() && slideshowSets.empty(), "...and off starts none");
        // the folder gone meanwhile: off goes on without it
        Reset(); parseFails = true; slideshowSets.clear(); monitors[0].wallpaper = Black(); logs.clear();
        WriteBytes(Record(), WithLine(kBothRecord, "slideshow=\n", "slideshow=D:\\gone\n"));
        Check(oled::Disable() && slideshowSets.empty() && !HasRecord() && monitors[0].wallpaper == L"C:\\Wallpapers\\main.jpg",
              "the slideshow's folder gone: off puts the wallpaper back without it, the record goes");
        CheckSame(Logs(), "oled: off|", "...dock.log says only 'oled: off'");
    }
    // ---- the covers from a record written by hand (oled_covers.cpp reads allMonitors and the wallpaper lines too)
    {
        Reset();
        WriteBytes(Record(), "active=1\nallMonitors=0\nwallpaper=\\\\?\\DISPLAY#SIDE#side\tC:\\Wallpapers\\side.jpg\n");
        oled::InvalidateCovers(); g_now += 3; Run(0.2);
        Check(Covers().size() == 1 && EqualRect(&wins[Covers()[0]].rect, &kSideRect) && oled::CoversMonitor(kSide) && !oled::CoversMonitor(kMain),
              "a record of one monitor (allMonitors=0 and its wallpaper line): one cover, over that monitor");
        WriteBytes(Record(), "active=1\n"); oled::InvalidateCovers(); Run(0.2);
        Check(Covers().size() == 2 && oled::CoversMonitor(kMain) && oled::CoversMonitor(kSide),
              "a record of active=1 alone (allMonitors 1 by default): every monitor covered");
        DeleteFileW(Record().c_str()); g_now += 3; Run(0.1);
        Check(Covers().empty() && !oled::Active(), "...the record gone: no cover");
    }
    Reset(); g_now += 3; Run(0.1);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // a crash keeps the lines so far (piped output is otherwise lost with the buffer)
    // ---- turning it on and off: the record is written first and is what puts the wallpapers back
    {
        Reset();
        Check(oled::Enable(nullptr), "the mode comes on for every monitor");
        Check(HasRecord() && monitors[0].wallpaper == Black() && monitors[1].wallpaper == Black(), "...the record kept, both wallpapers black");
        Check(oled::Active() && background == RGB(0, 0, 0), "...it is on, the background colour black");
        Check(oled::Enable(nullptr) && setWallpaperCalls == 2, "on again: nothing more is changed");
        monitors[1].wallpaper = L"C:\\Wallpapers\\picked meanwhile.jpg";   // the user chose a new one in the meantime
        Check(oled::Disable() && !HasRecord() && !oled::Active(), "off: the record goes with it");
        Check(monitors[0].wallpaper == L"C:\\Wallpapers\\main.jpg" && background == RGB(10, 20, 30), "...the first monitor's wallpaper and the colour come back");
        Check(monitors[1].wallpaper == L"C:\\Wallpapers\\picked meanwhile.jpg", "...a wallpaper picked meanwhile is kept");
    }
    {   // the record survives a restart: a new process reads it back
        Reset();
        oled::Enable(kMain);
        std::ifstream f(Record()); std::string all((std::istreambuf_iterator<char>(f)), {});
        Check(all.find("active=1") != std::string::npos && all.find("allMonitors=0") != std::string::npos &&
              all.find("main.jpg") != std::string::npos && all.find("side.jpg") == std::string::npos, "one monitor: only its wallpaper is recorded");
        f.close();
        oled::Disable();
        Check(monitors[0].wallpaper == L"C:\\Wallpapers\\main.jpg" && monitors[1].wallpaper == L"C:\\Wallpapers\\side.jpg", "...and only it was changed and put back");
    }
    {   // Explorer is not there when the mode is turned off (the restore shortcut is the way out when the shell is broken)
        Reset();
        oled::Enable(nullptr);
        wallpaperApi = false; logs.clear();
        const bool off = oled::Disable();
        Check(HasRecord(), "off while the wallpaper API is unavailable: the record that can still undo it is kept");
        Check(!off && oled::Active() && monitors[0].wallpaper == Black() && CountLogs(L"oled: off failed") == 1,
              "...it says it could not (the dock's button asks to try again), stays on, nothing half undone, dock.log says why");
        wallpaperApi = true;
        g_now += 3;
        const bool offNow = oled::Disable();
        Check(monitors[0].wallpaper == L"C:\\Wallpapers\\main.jpg" && monitors[1].wallpaper == L"C:\\Wallpapers\\side.jpg",
              "...so that turning it off once Explorer is back puts the wallpapers back");
        Check(offNow && !HasRecord() && !oled::Active() && background == RGB(10, 20, 30), "...and then it is off, the colour back, the record gone");
        oled::Disable(); DeleteFileW(Record().c_str()); g_now += 3; oled::Tick(g_now);
    }
    {   // the covers' window class cannot be registered (no USER objects or desktop heap left), before any cover was made
        Reset(); logs.clear();
        registerFails = ERROR_NOT_ENOUGH_MEMORY;
        oled::Enable(nullptr); Run(60);
        Check(Covers().empty() && CountLogs(L"oled: cover class failed 8") == 1,
              "no cover class for a minute: nothing covered, and dock.log says so once, not every 2 s");
        registerFails = ERROR_NOT_ENOUGH_QUOTA; Run(2.1);
        Check(CountLogs(L"oled: cover class failed 1816") == 1, "...a new reason is said");
        oled::InvalidateCovers(); Run(0.1);
        Check(CountLogs(L"oled: cover class failed 1816") == 2, "...and so is the same one after a display change");
        registerFails = 0; oled::Disable(); Run(0.1);
    }

    // ---- the covers: one per monitor in the mode, right above the desktop, under every window
    Reset();
    z.clear(); wins.clear();
    const HWND app = Make(L"Chrome_WidgetWin_1", {100, 100, 1800, 1200});
    const HWND side = Make(L"Notepad", {-1300, 0, -200, 900});
    shell = Make(L"Progman", {-1440, -560, 2560, 2000}, true);   // 24H2+: Progman holds the icons, the live wallpaper inside it
    wins[shell].defView = true;
    {
        oled::Enable(kMain);
        Check(oled::CoversMonitor(kMain) && oled::CoversMonitor(kSide), "just turned on: every dock stays hidden until the covers are worked out");
        Run(0.1);
        Check(Covers().size() == 1 && EqualRect(&wins[Covers()[0]].rect, &kMainRect), "one monitor in the mode: one cover, over it");
        Check(oled::CoversMonitor(kMain) && !oled::CoversMonitor(kSide), "...its dock hidden, the other one's free");
        Check(RestRightAbove(shell, {app, side}) && wins[Covers()[0]].visible, "...shown right above the desktop, under every window");
        Check(hooks == 1, "...with the foreground watched");
        oled::Disable(); Run(0.1);
        Check(Covers().empty() && unhooks == 1 && !oled::CoversMonitor(kMain), "off: the cover and the hook go");
    }
    {   // a monitor unplugged while the mode covers just it (an OLED monitor switched off at its button leaves the desktop)
        Reset();
        oled::Enable(kMain); Run(0.5);
        Check(Covers().size() == 1, "fixture: the main monitor covered");
        logs.clear();
        monitors[0].attached = false; oled::InvalidateCovers();   // as BuildDocks does after the display change
        Run(60);
        Check(Covers().empty() && !oled::CoversMonitor(kSide), "unplugged: no cover, the other monitor's dock shows");
        const int lines = CountLogs(L"oled: 0 cover(s)");
        std::printf("note: %d \"oled: 0 cover(s)\" lines in a minute with nothing to cover (dock.log stops at 20000 lines)\n", lines);
        Check(lines <= 1, "...and dock.log says so once, not every 2 s (the log stops for good after 20000 lines)");
        Check(lines == 1, "...but it does say so");
        logs.clear(); monitors[0].attached = true; oled::InvalidateCovers(); Run(0.1);
        Check(Covers().size() == 1 && oled::CoversMonitor(kMain), "plugged in again: covered again");
        Check(CountLogs(L"oled: 1 cover(s)") == 1, "...and dock.log says so");
        // Explorer gone for a while: in this mode the wallpaper API says which monitor is covered
        logs.clear(); wallpaperApi = false; oled::InvalidateCovers(); Run(60);
        std::printf("note: Explorer gone for a minute: %d \"wallpaper api unavailable\" and %d \"0 cover(s)\" lines\n",
                    CountLogs(L"wallpaper api unavailable"), CountLogs(L"oled: 0 cover(s)"));
        Check(CountLogs(L"wallpaper api unavailable") <= 1 && CountLogs(L"oled: 0 cover(s)") <= 1, "Explorer gone for a minute: dock.log says so once, not every 2 s");
        Check(CountLogs(L"wallpaper api unavailable") == 1 && CountLogs(L"oled: 0 cover(s)") == 1, "...but it does say so");
        logs.clear();
        Check(!oled::Disable() && CountLogs(L"wallpaper api unavailable") == 1 && CountLogs(L"oled: off failed") == 1,
              "...turning the mode off meanwhile fails, and dock.log says why, whatever the retries said before");
        logs.clear(); wallpaperApi = true; Run(2.1);   // Explorer back (TaskbarCreated): no display change, the retry finds it
        Check(Covers().size() == 1 && CountLogs(L"oled: 1 cover(s)") == 1, "Explorer back: within 2 s the monitor is covered again, and dock.log says so");
        wallpaperApi = true; oled::InvalidateCovers(); Run(0.1);
        Check(Covers().size() == 1, "...and the monitor is covered again once it is back");
        oled::Disable(); Run(0.1);
    }
    {   // the monitor the mode covers switched off for an hour: it comes back with a display change, which rebuilds the
        // covers at once - after 30 s Explorer is asked about it once a minute, not every 2 s
        Reset();
        oled::Enable(kMain); Run(0.5);
        Check(Covers().size() == 1, "fixture: the main monitor covered");
        logs.clear();
        monitors[0].attached = false; oled::InvalidateCovers();
        const int asks = wallpaperAsks, rects = rectAsks;
        Run(3600);
        std::printf("note: an hour with the covered monitor switched off: %d wallpaper API calls, %d monitor rectangles asked of Explorer\n",
                    wallpaperAsks - asks, rectAsks - rects);
        Check(Covers().empty() && !oled::CoversMonitor(kSide) && wallpaperAsks - asks <= 80,
              "an hour with the mode's monitor switched off: no cover, the other dock free, Explorer asked some 75 times, not 1800");
        Check(wallpaperAsks - asks >= 70 && CountLogs(L"oled: 0 cover(s)") == 1, "...still looked for once a minute; dock.log says it once");
        logs.clear(); monitors[0].attached = true; oled::InvalidateCovers(); Run(0.1);
        Check(Covers().size() == 1 && CountLogs(L"oled: 1 cover(s)") == 1, "...switched on again (a display change): covered at once, and dock.log says so");
        // back without a display change (none came): the minute's look finds it
        monitors[0].attached = false; oled::InvalidateCovers(); Run(120);
        monitors[0].attached = true;
        double from = g_now;
        while (Covers().empty() && g_now < from + 120) Run(1.0 / 60);
        Check(Covers().size() == 1 && g_now - from <= 60.05, "...back with no display change: covered within a minute");
        // in the first 30 s after a change every 2 s, as before (Explorer may know a new monitor a moment after Windows)
        monitors[0].attached = false; oled::InvalidateCovers(); Run(25);
        monitors[0].attached = true; from = g_now;
        while (Covers().empty() && g_now < from + 10) Run(1.0 / 60);
        Check(Covers().size() == 1 && g_now - from <= 2.05, "...back within 30 s of the change: covered within 2 s, as before");
        // Explorer away for five minutes: no event when it is back (its TaskbarCreated does not rebuild the covers), so every 2 s
        wallpaperApi = false; oled::InvalidateCovers(); Run(300);
        wallpaperApi = true; from = g_now;
        while (Covers().empty() && g_now < from + 10) Run(1.0 / 60);
        Check(Covers().size() == 1 && g_now - from <= 2.05, "Explorer away for five minutes: covered within 2 s of its return, as before");
        oled::Disable(); Run(0.1);
    }
    {   // the cover windows cannot be made (no USER objects or desktop heap left): nothing covered, the 2 s retry goes on
        Reset(); logs.clear();
        createFails = ERROR_NOT_ENOUGH_MEMORY;
        oled::Enable(nullptr); Run(60);
        std::printf("note: %d \"cover window failed\" lines in a minute with no cover window to be made (2 monitors)\n",
                    CountLogs(L"cover window failed"));
        Check(Covers().empty() && CountLogs(L"oled: cover window failed 8") == 2 && CountLogs(L"oled: 0 cover(s)") == 1,
              "no cover window for a minute: dock.log says so once for each monitor, not every 2 s");
        createFails = ERROR_NOT_ENOUGH_QUOTA; Run(2.1);
        Check(CountLogs(L"oled: cover window failed 1816") == 2, "...a new reason is said");
        logs.clear(); oled::InvalidateCovers(); Run(0.1);
        Check(CountLogs(L"oled: cover window failed 1816") == 2, "...and so is the same one after a display change");
        logs.clear(); createFails = 0; Run(2.1);
        Check(Covers().size() == 2 && CountLogs(L"oled: 2 cover(s)") == 1 && CountLogs(L"cover window failed") == 0,
              "...once they can be made again: covered within 2 s, and dock.log says so");
        oled::Disable(); Run(0.1);
    }
    {   // monitors changed: the new covers are up before the old ones go
        Reset();
        oled::Enable(nullptr); Run(0.1);
        auto before = Covers();
        Check(before.size() == 2, "every monitor: two covers");
        events.clear(); logs.clear(); oled::InvalidateCovers(); Run(0.1);
        Check(CountLogs(L"oled: 2 cover(s)") == 1, "rebuilt with as many covers as before: dock.log has its line, as after every change");
        Check(events.size() == 4 && events[0].rfind(L"make", 0) == 0 && events[1].rfind(L"make", 0) == 0 &&
              events[2].rfind(L"destroy", 0) == 0 && events[3].rfind(L"destroy", 0) == 0, "rebuilt: the new covers are made before the old ones go");
        Check(Covers().size() == 2 && RestRightAbove(shell, {app, side}), "...and rest above the desktop like the old ones");
        // the record was taken away by another process (--restore): within the 2 s cache the covers go
        DeleteFileW(Record().c_str());
        Run(1.0); Check(Covers().size() == 2, "the record gone: the cache still says on for a moment");
        Run(1.5); Check(Covers().empty(), "...and within 2 s the covers are gone");
    }
    {   // "show desktop" lifts the desktop over the windows: the covers follow, and come down again after
        Reset();
        oled::Enable(nullptr); Run(0.2);
        // the desktop is lifted over the windows (and over the covers lying at the bottom)
        z.erase(std::find(z.begin(), z.end(), shell)); z.insert(z.begin(), shell);
        oled::OnForeground(nullptr, EVENT_SYSTEM_FOREGROUND, shell, 0, 0, 0, 0);
        Check(Index(Covers()[0]) == 0 && Index(Covers()[1]) == 1 && Index(shell) == 2 && oled::g_raised, "show desktop: the covers right above the lifted desktop");
        Run(1.0);
        Check(Index(shell) == 2 && Index(app) > 2 && oled::g_raised, "...and stay there while it lasts");
        // it ends: the desktop goes back to the bottom; the windows are over it, the covers left on top of them
        z.erase(std::find(z.begin(), z.end(), shell)); z.push_back(shell);
        oled::OnForeground(nullptr, EVENT_SYSTEM_FOREGROUND, app, 0, 0, 0, 0);
        Check(RestRightAbove(shell, {app, side}) && !oled::g_raised, "show desktop over: the covers back under the windows");
        // windows under the covers that the black would hide: minimized, cloaked or hidden ones do not count
        const HWND hidden = Make(L"Hidden", {0, 0, 500, 500}), cloaked = Make(L"Cloaked", {0, 0, 500, 500}), minimized = Make(L"Mini", {-32000, -32000, -31840, -31972});
        wins[hidden].visible = false; wins[cloaked].cloaked = true; wins[minimized].iconic = true;
        for (HWND h : {hidden, cloaked, minimized}) { z.erase(std::find(z.begin(), z.end(), h)); z.insert(z.end() - 1, h); }
        const int moves = coverMoves; Run(1.0);
        Check(coverMoves == moves, "hidden, cloaked and minimized windows under the covers leave them where they are");
        for (HWND h : {hidden, cloaked, minimized}) DestroyWindow(h);
        oled::Disable(); Run(0.1);
    }
    {   // another program keeps putting its window at the very bottom: 20 times back, then 30 s of rest
        Reset();
        oled::Enable(nullptr); Run(0.2);
        const HWND widget = Make(L"RainmeterMeterWindow", {2200, 100, 2500, 400});
        auto sink = [&] { if (Index(widget) + 1 != Index(shell)) { z.erase(std::find(z.begin(), z.end(), widget)); z.insert(z.end() - 1, widget); } };
        sink(); logs.clear();
        const int start = coverMoves;
        Run(3.0, sink);
        Check((coverMoves - start) / 2 == 20 && CountLogs(L"covers paused for 30 s") == 1, "a window that keeps taking the bottom: put back 20 times, then the covers pause");
        const int paused = coverMoves;
        Run(28.0, sink);
        Check(coverMoves == paused, "...for 30 s nothing is moved");
        Run(3.0, sink);
        Check(coverMoves > paused, "...then they look again");
        DestroyWindow(widget);
        oled::Disable(); Run(0.1);
    }
    {   // before 24H2, with a live wallpaper: the icons in a WorkerW of their own over the wallpaper's WorkerW
        Reset();
        z.erase(std::find(z.begin(), z.end(), shell)); wins[shell].defView = false; z.push_back(shell);
        const DWORD explorer = wins[shell].pid;
        const HWND icons = Make(L"WorkerW", {-1440, -560, 2560, 2000}, true, explorer), wall = Make(L"WorkerW", {-1440, -560, 2560, 2000}, true, explorer);
        wins[icons].defView = true;
        for (HWND h : {icons, wall}) z.erase(std::find(z.begin(), z.end(), h));
        z.insert(z.end() - 1, icons); z.insert(z.end() - 1, wall);   // ... icons, wallpaper, Progman
        oled::Enable(nullptr); Run(0.2);
        Check(RestRightAbove(icons, {app, side}), "before 24H2: the covers right above the icons' WorkerW, under every window");
        const int moves = coverMoves; Run(1.0);
        Check(coverMoves == moves, "...and left there");
        oled::Disable(); Run(0.1);
        for (HWND h : {icons, wall}) DestroyWindow(h);
    }
    {   // Explorer restarting: nothing to put the covers above, nothing is moved
        Reset();
        oled::Enable(nullptr); Run(0.2);
        const HWND was = shell; shell = nullptr;
        const int moves = coverMoves; Run(1.0);
        Check(coverMoves == moves && Covers().size() == 2, "no desktop (Explorer restarting): the covers stay and nothing is moved");
        shell = was;
        oled::Disable(); Run(0.1);
    }
    RecordGoldenChecks();
    MoreRecordChecks();
    DeleteFileW(Record().c_str()); DeleteFileW(Black().c_str()); RemoveDirectoryW(AppDataDir().c_str());
    return Summary("preview_oled_test");
}
