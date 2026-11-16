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
Gfx g_gfx;   // never used: no fake gives an icon, so no bitmap is made

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
static BOOL WINAPI FakeAllowFront(DWORD) { return TRUE; }
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
static LRESULT WINAPI FakeSendTimeout(HWND h, UINT msg, WPARAM wp, LPARAM, UINT, UINT timeout, PDWORD_PTR result) {
    if (result) *result = 0;
    std::unique_lock<std::mutex> lk(mu);
    auto* w = Find(h);
    if (!w || !w->alive) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
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
static UINT WINAPI FakeExtractIcons(LPCWSTR, int, int, int, HICON*, UINT*, UINT, UINT) { return 0; }
static HRESULT WINAPI FakeItemInKnownFolder(REFKNOWNFOLDERID, DWORD, PCWSTR, REFIID, void** ppv) { *ppv = nullptr; return E_FAIL; }
static HRESULT WINAPI FakeItemFromName(PCWSTR, IBindCtx*, REFIID, void** ppv) { *ppv = nullptr; return E_FAIL; }
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
ForegroundRight TakeForegroundRight() { ++rightTaken; return ForegroundRight::Had; }
const wchar_t* ForegroundRightText(ForegroundRight) { return L"had it"; }
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
#include APPS_MODEL_CPP
#include APPS_MODEL_PINS_CPP
#include APPS_MODEL_CLICK_CPP
#include APPS_MODEL_PRESS_CPP
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
    { std::lock_guard<std::mutex> lg(logMu); logLines.clear(); }
    fakepress::beginWorks = false; fakepress::begun.clear(); fakepress::ready.clear();
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
    Reset();
    return Summary("apps_model_test");
}
