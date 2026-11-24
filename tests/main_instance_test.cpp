// The dock process's other lives (src/main_instance.cpp), compiled under fakes: the watchdog copy that waits for the
// dock and puts the taskbar back after a crash (RunWatchdog, RestoreTaskbarWhenIdle), the dock starting it
// (StartWatchdog, WatchdogReady), a start as administrator handed to Explorer to run as the user (RestartUnelevated,
// the desktop's IShellDispatch2 - fake COM objects here), and the crash handler. Characterization test:
// it fixes what each path calls, in which order, with which arguments, what it returns and the dock.log
// lines it writes, so a change of main_instance.cpp that changes any of it fails here.
// Everything that would reach the PC is a fake: the instance mutex (the real dock holds LiquidDock.SingleInstance
// while this runs - no real CreateMutexW/OpenMutexW is made, the fakes only look at the name), the registry record
// HKCU\Software\LiquidDock\TaskbarState, processes (OpenProcess, CreateProcessW of the exe with --watchdog), events,
// the token, the shell's COM objects, the taskbar (g_taskbar.Restore), the clock and Sleep. Nothing is started,
// opened, read or written outside this process.
// Linked without user32, advapi32 and ole32 (and /NODEFAULTLIB for them): a call of theirs that no fake catches is then a
// link error, not a real call from a test that passes (oleaut32 stays: the real SysAllocString, SysFreeString, VariantInit and
// VariantClear, on this process's memory only).
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\main_instance_test.cpp /link /NODEFAULTLIB:user32.lib /NODEFAULTLIB:advapi32.lib /NODEFAULTLIB:ole32.lib oleaut32.lib uuid.lib
#include "../src/common.h"
#include "../src/system.h"
#include "../src/main_internal.h"
#include <shellapi.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shlguid.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "portable_check.h"

// ---------------------------------------------------------------- the test's world
namespace mi {

std::vector<std::wstring> calls;   // every faked call in order, with its arguments
std::vector<std::wstring> logs;    // dock.log lines

static void Call(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a); va_end(a);
    calls.push_back(buf);
}

// fake handles, named for the trace
std::map<HANDLE, std::wstring> handleNames;
int handleCount = 0;
static HANDLE NewHandle(const wchar_t* name) {
    HANDLE h = (HANDLE)(ULONG_PTR)(0x5000 + 16 * ++handleCount);
    handleNames[h] = name;
    return h;
}
static std::wstring NameOf(HANDLE h) {
    if (!h) return L"null";
    auto it = handleNames.find(h);
    return it == handleNames.end() ? L"?" : it->second;
}

enum class Shell {   // the step of the shell chain that fails (RestartUnelevated -> ShellExecuteAsUser)
    None, CoCreate, FindWindowFails, FindWindowSFalse, FindWindowNoDesktop, QiServices, QueryService, ActiveView,
    ItemObject, QiFolderView, Application, QiShellDispatch2, Execute
};

struct World {
    DWORD lastError = 0;
    // the dock's process (RunWatchdog)
    bool processOpens = true; DWORD openError = ERROR_ACCESS_DENIED;
    DWORD processWait = WAIT_OBJECT_0;           // WaitForSingleObject(process, INFINITE)
    bool eventOpens = true, eventSets = true;    // OpenEventW / SetEvent of --ready
    // the taskbar record (HKCU\Software\LiquidDock\TaskbarState)
    bool record = false;
    // the instance mutex
    bool mutexCreates = true; DWORD mutexError = ERROR_ACCESS_DENIED;
    DWORD mutexWait = WAIT_OBJECT_0;             // WaitForSingleObject(mutex, 0): WAIT_TIMEOUT = another dock holds it
    bool mutexHeld = false;                      // the test's own view: taken and not released
    bool sleptHolding = false, restoredUnheld = false;
    int mutexOpensAt = -1;                       // OpenMutexW finds the copy's mutex from this try (-1: never)
    int mutexOpens = 0;
    // the taskbar
    std::function<bool(int)> restore = [](int) { return true; };   // the n-th Restore (from 0) works
    int restores = 0;
    // the clock
    ULONGLONG tick = 1000000;
    // the token
    bool tokenOpens = true; TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeLimited;
    // the shell
    Shell shellFails = Shell::None;
    std::wstring shFile, shArgs, shDir, shVerb; VARTYPE shArgsVt = 0, shDirVt = 0, shVerbVt = 0, shShowVt = 0; long shShow = -1;
    // StartWatchdog
    bool guidMade = true, eventMade = true, processMade = true;
    DWORD readyWait = WAIT_OBJECT_0;             // WaitForMultipleObjects({ready, process}, 5000)
    DWORD watchdogAlive = WAIT_TIMEOUT;          // WaitForSingleObject(watchdog, 0)
    std::wstring cmdLine;
};
World w;

static void Reset() {
    w = World{};
    calls.clear(); logs.clear(); handleNames.clear(); handleCount = 0;
    g_watchdogProcess = nullptr;
}

static std::string U8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), o.data(), n, nullptr, nullptr);
    return o;
}

// The calls (or lines) as recorded, against the expected ones; on a difference both are printed.
static bool Same(const std::vector<std::wstring>& got, std::initializer_list<const wchar_t*> want, const char* what) {
    std::vector<std::wstring> v(want.begin(), want.end());
    const bool ok = got == v;
    if (!ok) {
        std::printf("  got:\n");
        for (auto& s : got) std::printf("    %s\n", U8(s).c_str());
        std::printf("  expected:\n");
        for (auto& s : v) std::printf("    %s\n", U8(s).c_str());
    }
    return Check(ok, what);
}

static int Count(const wchar_t* call) {
    int n = 0;
    for (auto& c : calls) if (c == call) ++n;
    return n;
}

}   // namespace mi
using mi::w;
using mi::Call;

// dock.log (common.cpp is not linked)
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a); va_end(a);
    mi::logs.push_back(buf);
    mi::calls.push_back(std::wstring(L"Log: ") + buf);
}

// ---------------------------------------------------------------- the fakes (named in the #define list below)
static DWORD WINAPI MiGetLastError() { return w.lastError; }

static HANDLE WINAPI MiOpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    Call(L"OpenProcess(0x%lx, %d, %lu)", access, inherit, pid);
    if (!w.processOpens) { w.lastError = w.openError; return nullptr; }
    return mi::NewHandle(L"process");
}
static HANDLE WINAPI MiOpenEventW(DWORD access, BOOL inherit, LPCWSTR name) {
    Call(L"OpenEventW(0x%lx, %d, %ls)", access, inherit, name ? name : L"null");
    return w.eventOpens ? mi::NewHandle(L"ready") : nullptr;
}
static BOOL WINAPI MiSetEvent(HANDLE h) {
    Call(L"SetEvent(%ls)", mi::NameOf(h).c_str());
    return w.eventSets;
}
static DWORD WINAPI MiWaitForSingleObject(HANDLE h, DWORD ms) {
    const std::wstring n = mi::NameOf(h);
    if (ms == INFINITE) Call(L"WaitForSingleObject(%ls, INFINITE)", n.c_str());
    else Call(L"WaitForSingleObject(%ls, %lu)", n.c_str(), ms);
    if (n == L"process") return w.processWait;
    if (n == L"mutex") {
        if (w.mutexWait == WAIT_OBJECT_0 || w.mutexWait == WAIT_ABANDONED) w.mutexHeld = true;
        return w.mutexWait;
    }
    if (n == L"watchdog") return w.watchdogAlive;
    return WAIT_FAILED;
}
static DWORD WINAPI MiWaitForMultipleObjects(DWORD count, const HANDLE* hs, BOOL all, DWORD ms) {
    std::wstring list;
    for (DWORD i = 0; i < count; ++i) list += (i ? L", " : L"") + mi::NameOf(hs[i]);
    Call(L"WaitForMultipleObjects(%lu, {%ls}, %d, %lu)", count, list.c_str(), all, ms);
    return w.readyWait;
}
static BOOL WINAPI MiCloseHandle(HANDLE h) {
    Call(L"CloseHandle(%ls)", mi::NameOf(h).c_str());
    if (mi::NameOf(h) == L"mutex" && w.mutexHeld) Call(L"  (closed while held)");
    return TRUE;
}
static LSTATUS WINAPI MiRegGetValueW(HKEY key, LPCWSTR sub, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD size) {
    Call(L"RegGetValueW(%ls, %ls, %ls, 0x%lx, type %ls, size %lu)", key == HKEY_CURRENT_USER ? L"HKCU" : L"other",
         sub ? sub : L"null", value ? value : L"null", flags, type ? L"asked" : L"null", size ? *size : 0);
    if (!w.record) return ERROR_FILE_NOT_FOUND;
    if (data) *(DWORD*)data = 3;
    return ERROR_SUCCESS;
}
static HANDLE WINAPI MiCreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCWSTR name) {
    Call(L"CreateMutexW(%ls, %d, %ls)", sa ? L"sa" : L"null", owner, name ? name : L"null");
    if (!w.mutexCreates) { w.lastError = w.mutexError; return nullptr; }
    return mi::NewHandle(L"mutex");
}
static HANDLE WINAPI MiOpenMutexW(DWORD access, BOOL inherit, LPCWSTR name) {
    Call(L"OpenMutexW(0x%lx, %d, %ls)", access, inherit, name ? name : L"null");
    const int n = w.mutexOpens++;
    return w.mutexOpensAt >= 0 && n >= w.mutexOpensAt ? mi::NewHandle(L"copy's mutex") : nullptr;
}
static BOOL WINAPI MiReleaseMutex(HANDLE h) {
    Call(L"ReleaseMutex(%ls)", mi::NameOf(h).c_str());
    w.mutexHeld = false;
    return TRUE;
}
static void WINAPI MiSleep(DWORD ms) {
    Call(L"Sleep(%lu)", ms);
    if (w.mutexHeld) w.sleptHolding = true;
    w.tick += ms;
}
static ULONGLONG WINAPI MiGetTickCount64() { Call(L"GetTickCount64"); return w.tick; }
static HANDLE WINAPI MiGetCurrentProcess() { return (HANDLE)(LONG_PTR)-1; }
static DWORD WINAPI MiGetCurrentProcessId() { return 4242; }
static BOOL WINAPI MiOpenProcessToken(HANDLE process, DWORD access, PHANDLE token) {
    Call(L"OpenProcessToken(%ls, 0x%lx)", process == (HANDLE)(LONG_PTR)-1 ? L"this process" : L"other", access);
    if (!w.tokenOpens) return FALSE;
    *token = mi::NewHandle(L"token");
    return TRUE;
}
static BOOL WINAPI MiGetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS cls, LPVOID info, DWORD len, PDWORD ret) {
    Call(L"GetTokenInformation(%ls, %d, %lu)", mi::NameOf(token).c_str(), (int)cls, len);
    if (cls != TokenElevationType || len < sizeof(TOKEN_ELEVATION_TYPE)) return FALSE;
    *(TOKEN_ELEVATION_TYPE*)info = w.elevation;
    if (ret) *ret = sizeof(TOKEN_ELEVATION_TYPE);
    return TRUE;
}
static const wchar_t kExe[] = L"C:\\Users\\Test User\\AppData\\Local\\Programs\\LiquidDock\\LiquidDock.exe";
static DWORD WINAPI MiGetModuleFileNameW(HMODULE m, LPWSTR out, DWORD size) {
    Call(L"GetModuleFileNameW(%ls, %lu)", m ? L"module" : L"null", size);
    wcscpy_s(out, size, kExe);
    return (DWORD)wcslen(kExe);
}
static const GUID kGuid = { 0x00112233, 0x4455, 0x6677, { 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF } };
static HRESULT WINAPI MiCoCreateGuid(GUID* g) {
    Call(L"CoCreateGuid");
    if (!w.guidMade) return E_OUTOFMEMORY;
    *g = kGuid;
    return S_OK;
}
static int WINAPI MiStringFromGUID2(REFGUID g, LPOLESTR out, int size) {
    Call(L"StringFromGUID2(%ls, %d)", IsEqualGUID(g, kGuid) ? L"the guid made" : L"another", size);
    static const wchar_t s[] = L"{00112233-4455-6677-8899-AABBCCDDEEFF}";
    if (size < (int)_countof(s)) return 0;
    wcscpy_s(out, size, s);
    return (int)_countof(s);
}
static HANDLE WINAPI MiCreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name) {
    Call(L"CreateEventW(%ls, %d, %d, %ls)", sa ? L"sa" : L"null", manual, initial, name ? name : L"null");
    return w.eventMade ? mi::NewHandle(L"ready") : nullptr;
}
static BOOL WINAPI MiCreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit,
                                    DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    w.cmdLine = cmd ? cmd : L"";
    Call(L"CreateProcessW(%ls, [%ls], %ls, %ls, %d, 0x%lx, %ls, %ls, si.cb %lu)", app ? app : L"null", cmd ? cmd : L"null",
         pa ? L"pa" : L"null", ta ? L"ta" : L"null", inherit, flags, env ? L"env" : L"null", dir ? dir : L"null", si ? si->cb : 0);
    if (!w.processMade) return FALSE;
    pi->hProcess = mi::NewHandle(L"watchdog");
    pi->hThread = mi::NewHandle(L"thread");
    pi->dwProcessId = 5151; pi->dwThreadId = 5152;
    return TRUE;
}
// never called by main_instance.cpp now; a part moved here later is recorded (or, ExitProcess, stops the test)
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI MiSetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER) { Call(L"SetUnhandledExceptionFilter"); return nullptr; }
static BOOL WINAPI MiTerminateProcess(HANDLE h, UINT code) { Call(L"TerminateProcess(%ls, %u)", mi::NameOf(h).c_str(), code); return TRUE; }
[[noreturn]] static void WINAPI MiExitProcess(UINT code) {
    std::printf("FAIL: main_instance.cpp called ExitProcess(%u) - the test process would end\n", code);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

// the taskbar (NativeTaskbar, native_taskbar.cpp): only Restore is used here
struct MiTaskbar {
    bool hidden = false;
    bool Restore() {
        const int n = w.restores++;
        Call(w.mutexHeld ? L"Restore" : L"Restore (mutex not held)");
        if (!w.mutexHeld) w.restoredUnheld = true;
        return w.restore(n);
    }
};
MiTaskbar miTaskbar;

// ---------------------------------------------------------------- the shell's COM objects (ShellExecuteAsUser)
#define MI_NI { return E_NOTIMPL; }
#define MI_UNKNOWN \
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; } \
    ULONG STDMETHODCALLTYPE Release() override { if (--refs == 0) Call(L"released %ls", name); return refs; }
#define MI_DISPATCH \
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*) override MI_NI \
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override MI_NI \
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override MI_NI \
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override MI_NI
#define MI_OLEWINDOW \
    HRESULT STDMETHODCALLTYPE GetWindow(HWND*) override MI_NI \
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override MI_NI

// each step's failure has its own HRESULT, so the logged code tells the step
static HRESULT FailHr(mi::Shell s) { return MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x200 + (int)s); }
static bool Fails(mi::Shell s) { return w.shellFails == s; }
template <class T> static HRESULT Give(T* p, void** out) { *out = p; p->AddRef(); return S_OK; }

struct MiApp : IShellDispatch2 {   // the desktop's Shell.Application
    long refs = 0; const wchar_t* name = L"app";
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == __uuidof(IShellDispatch)) return Give(this, pp);
        if (riid == __uuidof(IShellDispatch2)) {
            Call(L"app: QueryInterface(IShellDispatch2)");
            if (Fails(mi::Shell::QiShellDispatch2)) { *pp = nullptr; return FailHr(mi::Shell::QiShellDispatch2); }
            return Give(this, pp);
        }
        *pp = nullptr; return E_NOINTERFACE;
    }
    MI_UNKNOWN MI_DISPATCH
    HRESULT STDMETHODCALLTYPE get_Application(IDispatch**) override MI_NI
    HRESULT STDMETHODCALLTYPE get_Parent(IDispatch**) override MI_NI
    HRESULT STDMETHODCALLTYPE NameSpace(VARIANT, Folder**) override MI_NI
    HRESULT STDMETHODCALLTYPE BrowseForFolder(long, BSTR, long, VARIANT, Folder**) override MI_NI
    HRESULT STDMETHODCALLTYPE Windows(IDispatch**) override MI_NI
    HRESULT STDMETHODCALLTYPE Open(VARIANT) override MI_NI
    HRESULT STDMETHODCALLTYPE Explore(VARIANT) override MI_NI
    HRESULT STDMETHODCALLTYPE MinimizeAll() override MI_NI
    HRESULT STDMETHODCALLTYPE UndoMinimizeALL() override MI_NI
    HRESULT STDMETHODCALLTYPE FileRun() override MI_NI
    HRESULT STDMETHODCALLTYPE CascadeWindows() override MI_NI
    HRESULT STDMETHODCALLTYPE TileVertically() override MI_NI
    HRESULT STDMETHODCALLTYPE TileHorizontally() override MI_NI
    HRESULT STDMETHODCALLTYPE ShutdownWindows() override MI_NI
    HRESULT STDMETHODCALLTYPE Suspend() override MI_NI
    HRESULT STDMETHODCALLTYPE EjectPC() override MI_NI
    HRESULT STDMETHODCALLTYPE SetTime() override MI_NI
    HRESULT STDMETHODCALLTYPE TrayProperties() override MI_NI
    HRESULT STDMETHODCALLTYPE Help() override MI_NI
    HRESULT STDMETHODCALLTYPE FindFiles() override MI_NI
    HRESULT STDMETHODCALLTYPE FindComputer() override MI_NI
    HRESULT STDMETHODCALLTYPE RefreshMenu() override MI_NI
    HRESULT STDMETHODCALLTYPE ControlPanelItem(BSTR) override MI_NI
    HRESULT STDMETHODCALLTYPE IsRestricted(BSTR, BSTR, long*) override MI_NI
    HRESULT STDMETHODCALLTYPE ShellExecute(BSTR file, VARIANT args, VARIANT dir, VARIANT verb, VARIANT show) override {
        Call(L"app: ShellExecute");
        w.shFile = file ? file : L"(null)";
        w.shArgsVt = args.vt; w.shArgs = args.vt == VT_BSTR && args.bstrVal ? args.bstrVal : L"";
        w.shDirVt = dir.vt; w.shDir = dir.vt == VT_BSTR && dir.bstrVal ? dir.bstrVal : L"(none)";
        w.shVerbVt = verb.vt; w.shVerb = verb.vt == VT_BSTR && verb.bstrVal ? verb.bstrVal : L"(none)";
        w.shShowVt = show.vt; w.shShow = show.vt == VT_I4 ? show.lVal : -1;
        return Fails(mi::Shell::Execute) ? FailHr(mi::Shell::Execute) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindPrinter(BSTR, BSTR, BSTR) override MI_NI
    HRESULT STDMETHODCALLTYPE GetSystemInformation(BSTR, VARIANT*) override MI_NI
    HRESULT STDMETHODCALLTYPE ServiceStart(BSTR, VARIANT, VARIANT*) override MI_NI
    HRESULT STDMETHODCALLTYPE ServiceStop(BSTR, VARIANT, VARIANT*) override MI_NI
    HRESULT STDMETHODCALLTYPE IsServiceRunning(BSTR, VARIANT*) override MI_NI
    HRESULT STDMETHODCALLTYPE CanStartStopService(BSTR, VARIANT*) override MI_NI
    HRESULT STDMETHODCALLTYPE ShowBrowserBar(BSTR, VARIANT, VARIANT*) override MI_NI
};
MiApp miApp;

struct MiBackground : IShellFolderViewDual {   // the desktop view's background object
    long refs = 0; const wchar_t* name = L"background";
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch) return Give(this, pp);
        if (riid == __uuidof(IShellFolderViewDual)) {
            Call(L"background: QueryInterface(IShellFolderViewDual)");
            if (Fails(mi::Shell::QiFolderView)) { *pp = nullptr; return FailHr(mi::Shell::QiFolderView); }
            return Give(this, pp);
        }
        *pp = nullptr; return E_NOINTERFACE;
    }
    MI_UNKNOWN MI_DISPATCH
    HRESULT STDMETHODCALLTYPE get_Application(IDispatch** app) override {
        Call(L"background: get_Application");
        if (Fails(mi::Shell::Application)) { *app = nullptr; return FailHr(mi::Shell::Application); }
        return Give(static_cast<IDispatch*>(&miApp), (void**)app);
    }
    HRESULT STDMETHODCALLTYPE get_Parent(IDispatch**) override MI_NI
    HRESULT STDMETHODCALLTYPE get_Folder(Folder**) override MI_NI
    HRESULT STDMETHODCALLTYPE SelectedItems(FolderItems**) override MI_NI
    HRESULT STDMETHODCALLTYPE get_FocusedItem(FolderItem**) override MI_NI
    HRESULT STDMETHODCALLTYPE SelectItem(VARIANT*, int) override MI_NI
    HRESULT STDMETHODCALLTYPE PopupItemMenu(FolderItem*, VARIANT, VARIANT, BSTR*) override MI_NI
    HRESULT STDMETHODCALLTYPE get_Script(IDispatch**) override MI_NI
    HRESULT STDMETHODCALLTYPE get_ViewOptions(long*) override MI_NI
};
MiBackground miBackground;

struct MiView : IShellView {   // the desktop's active shell view
    long refs = 0; const wchar_t* name = L"view";
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IShellView) return Give(this, pp);
        *pp = nullptr; return E_NOINTERFACE;
    }
    MI_UNKNOWN MI_OLEWINDOW
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) override MI_NI
    HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override MI_NI
    HRESULT STDMETHODCALLTYPE UIActivate(UINT) override MI_NI
    HRESULT STDMETHODCALLTYPE Refresh() override MI_NI
    HRESULT STDMETHODCALLTYPE CreateViewWindow(IShellView*, LPCFOLDERSETTINGS, IShellBrowser*, RECT*, HWND*) override MI_NI
    HRESULT STDMETHODCALLTYPE DestroyViewWindow() override MI_NI
    HRESULT STDMETHODCALLTYPE GetCurrentInfo(LPFOLDERSETTINGS) override MI_NI
    HRESULT STDMETHODCALLTYPE AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override MI_NI
    HRESULT STDMETHODCALLTYPE SaveViewState() override MI_NI
    HRESULT STDMETHODCALLTYPE SelectItem(PCUITEMID_CHILD, SVSIF) override MI_NI
    HRESULT STDMETHODCALLTYPE GetItemObject(UINT item, REFIID riid, void** pp) override {
        Call(L"view: GetItemObject(%ls, %ls)", item == SVGIO_BACKGROUND ? L"SVGIO_BACKGROUND" : L"other",
             riid == IID_IDispatch ? L"IDispatch" : L"other");
        if (Fails(mi::Shell::ItemObject)) { *pp = nullptr; return FailHr(mi::Shell::ItemObject); }
        return Give(static_cast<IDispatch*>(&miBackground), pp);
    }
};
MiView miView;

struct MiBrowser : IShellBrowser {   // the desktop's top-level browser
    long refs = 0; const wchar_t* name = L"browser";
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IShellBrowser) return Give(this, pp);
        *pp = nullptr; return E_NOINTERFACE;
    }
    MI_UNKNOWN MI_OLEWINDOW
    HRESULT STDMETHODCALLTYPE InsertMenusSB(HMENU, LPOLEMENUGROUPWIDTHS) override MI_NI
    HRESULT STDMETHODCALLTYPE SetMenuSB(HMENU, HOLEMENU, HWND) override MI_NI
    HRESULT STDMETHODCALLTYPE RemoveMenusSB(HMENU) override MI_NI
    HRESULT STDMETHODCALLTYPE SetStatusTextSB(LPCWSTR) override MI_NI
    HRESULT STDMETHODCALLTYPE EnableModelessSB(BOOL) override MI_NI
    HRESULT STDMETHODCALLTYPE TranslateAcceleratorSB(MSG*, WORD) override MI_NI
    HRESULT STDMETHODCALLTYPE BrowseObject(PCUIDLIST_RELATIVE, UINT) override MI_NI
    HRESULT STDMETHODCALLTYPE GetViewStateStream(DWORD, IStream**) override MI_NI
    HRESULT STDMETHODCALLTYPE GetControlWindow(UINT, HWND*) override MI_NI
    HRESULT STDMETHODCALLTYPE SendControlMsg(UINT, UINT, WPARAM, LPARAM, LRESULT*) override MI_NI
    HRESULT STDMETHODCALLTYPE QueryActiveShellView(IShellView** view) override {
        Call(L"browser: QueryActiveShellView");
        if (Fails(mi::Shell::ActiveView)) { *view = nullptr; return FailHr(mi::Shell::ActiveView); }
        return Give(&miView, (void**)view);
    }
    HRESULT STDMETHODCALLTYPE OnViewWindowActive(IShellView*) override MI_NI
    HRESULT STDMETHODCALLTYPE SetToolbarItems(LPTBBUTTONSB, UINT, UINT) override MI_NI
};
MiBrowser miBrowser;

struct MiDesktop : IDispatch, IServiceProvider {   // the desktop window's dispatch (FindWindowSW)
    long refs = 0; const wchar_t* name = L"desktop";
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch) return Give(static_cast<IDispatch*>(this), pp);
        if (riid == IID_IServiceProvider) {
            Call(L"desktop: QueryInterface(IServiceProvider)");
            if (Fails(mi::Shell::QiServices)) { *pp = nullptr; return FailHr(mi::Shell::QiServices); }
            return Give(static_cast<IServiceProvider*>(this), pp);
        }
        *pp = nullptr; return E_NOINTERFACE;
    }
    MI_UNKNOWN MI_DISPATCH
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID service, REFIID riid, void** pp) override {
        Call(L"desktop: QueryService(%ls, %ls)", service == SID_STopLevelBrowser ? L"SID_STopLevelBrowser" : L"other",
             riid == IID_IShellBrowser ? L"IShellBrowser" : L"other");
        if (Fails(mi::Shell::QueryService)) { *pp = nullptr; return FailHr(mi::Shell::QueryService); }
        return Give(&miBrowser, pp);
    }
};
MiDesktop miDesktop;

struct MiShellWindows : IShellWindows {   // CoCreateInstance(CLSID_ShellWindows)
    long refs = 0; const wchar_t* name = L"windows";
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IShellWindows) return Give(this, pp);
        *pp = nullptr; return E_NOINTERFACE;
    }
    MI_UNKNOWN MI_DISPATCH
    HRESULT STDMETHODCALLTYPE get_Count(long*) override MI_NI
    HRESULT STDMETHODCALLTYPE Item(VARIANT, IDispatch**) override MI_NI
    HRESULT STDMETHODCALLTYPE _NewEnum(IUnknown**) override MI_NI
    HRESULT STDMETHODCALLTYPE Register(IDispatch*, long, int, long*) override MI_NI
    HRESULT STDMETHODCALLTYPE RegisterPending(long, VARIANT*, VARIANT*, int, long*) override MI_NI
    HRESULT STDMETHODCALLTYPE Revoke(long) override MI_NI
    HRESULT STDMETHODCALLTYPE OnNavigate(long, VARIANT*) override MI_NI
    HRESULT STDMETHODCALLTYPE OnActivated(long, VARIANT_BOOL) override MI_NI
    HRESULT STDMETHODCALLTYPE FindWindowSW(VARIANT* loc, VARIANT* root, int cls, long* hwnd, int options, IDispatch** out) override {
        Call(L"windows: FindWindowSW(vt %d %ld, root vt %d, class %d, options 0x%x)", loc ? loc->vt : -1, loc ? loc->lVal : -1,
             root ? root->vt : -1, cls, options);
        if (hwnd) *hwnd = 0x10010;
        *out = nullptr;
        if (Fails(mi::Shell::FindWindowFails)) return FailHr(mi::Shell::FindWindowFails);
        if (Fails(mi::Shell::FindWindowSFalse)) return S_FALSE;
        if (Fails(mi::Shell::FindWindowNoDesktop)) return S_OK;
        return Give(static_cast<IDispatch*>(&miDesktop), (void**)out);
    }
    HRESULT STDMETHODCALLTYPE OnCreated(long, IUnknown*) override MI_NI
    HRESULT STDMETHODCALLTYPE ProcessAttachDetach(VARIANT_BOOL) override MI_NI
};
MiShellWindows miShellWindows;

static HRESULT WINAPI MiCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID* pp) {
    Call(L"CoCreateInstance(%ls, %ls, 0x%lx, %ls)", clsid == CLSID_ShellWindows ? L"CLSID_ShellWindows" : L"other",
         outer ? L"outer" : L"null", ctx, riid == IID_IShellWindows ? L"IShellWindows" : L"other");
    if (Fails(mi::Shell::CoCreate)) { *pp = nullptr; return FailHr(mi::Shell::CoCreate); }
    return Give(&miShellWindows, pp);
}
static long ShellRefs() { return miApp.refs + miBackground.refs + miView.refs + miBrowser.refs + miDesktop.refs + miShellWindows.refs; }

// ---------------------------------------------------------------- the source, under the fakes
#define GetLastError MiGetLastError
#define OpenProcess MiOpenProcess
#define OpenEventW MiOpenEventW
#define SetEvent MiSetEvent
#define WaitForSingleObject MiWaitForSingleObject
#define WaitForMultipleObjects MiWaitForMultipleObjects
#define CloseHandle MiCloseHandle
#define RegGetValueW MiRegGetValueW
#define CreateMutexW MiCreateMutexW
#define OpenMutexW MiOpenMutexW
#define ReleaseMutex MiReleaseMutex
#define Sleep MiSleep
#define GetTickCount64 MiGetTickCount64
#define GetCurrentProcess MiGetCurrentProcess
#define GetCurrentProcessId MiGetCurrentProcessId
#define OpenProcessToken MiOpenProcessToken
#define GetTokenInformation MiGetTokenInformation
#define GetModuleFileNameW MiGetModuleFileNameW
#define CoCreateGuid MiCoCreateGuid
#define StringFromGUID2 MiStringFromGUID2
#define CreateEventW MiCreateEventW
#define CreateProcessW MiCreateProcessW
#define CoCreateInstance MiCoCreateInstance
#define SetUnhandledExceptionFilter MiSetUnhandledExceptionFilter
#define TerminateProcess MiTerminateProcess
#define ExitProcess MiExitProcess
#define g_taskbar miTaskbar
#include "../src/main_instance.cpp"
#undef g_taskbar
#undef ExitProcess
#undef TerminateProcess
#undef SetUnhandledExceptionFilter
#undef CoCreateInstance
#undef CreateProcessW
#undef CreateEventW
#undef StringFromGUID2
#undef CoCreateGuid
#undef GetModuleFileNameW
#undef GetTokenInformation
#undef OpenProcessToken
#undef GetCurrentProcessId
#undef GetCurrentProcess
#undef GetTickCount64
#undef Sleep
#undef ReleaseMutex
#undef OpenMutexW
#undef CreateMutexW
#undef RegGetValueW
#undef CloseHandle
#undef WaitForMultipleObjects
#undef WaitForSingleObject
#undef SetEvent
#undef OpenEventW
#undef OpenProcess
#undef GetLastError

using mi::Same;

// ---------------------------------------------------------------- RunWatchdog
static void WatchdogTests() {
    Check(std::wstring(kInstanceMutex) == L"LiquidDock.SingleInstance", "the instance mutex's name is LiquidDock.SingleInstance (only the fakes see it here)");

    mi::Reset();   // the dock exits cleanly: its own exit removed the record
    int rc = RunWatchdog(4242, nullptr);
    Check(rc == 0, "watchdog: the dock exits cleanly (no record) - exit code 0");
    Same(mi::calls, { L"OpenProcess(0x100000, 0, 4242)", L"WaitForSingleObject(process, INFINITE)", L"CloseHandle(process)",
                      L"RegGetValueW(HKCU, Software\\LiquidDock, TaskbarState, 0x10, type null, size 4)" },
         "...it opens the dock for SYNCHRONIZE, waits for it without end, closes it, reads HKCU\\Software\\LiquidDock TaskbarState (a DWORD) - and restores nothing");
    Check(w.restores == 0 && mi::logs.empty(), "...no taskbar restore, no dock.log line");

    mi::Reset(); w.record = true;   // killed (Task Manager) or a crash the handler missed: the record is still there
    rc = RunWatchdog(4242, nullptr);
    Check(rc == 0, "watchdog: the dock gone with the record left - the taskbar restored, exit code 0");
    Same(mi::calls, { L"OpenProcess(0x100000, 0, 4242)", L"WaitForSingleObject(process, INFINITE)", L"CloseHandle(process)",
                      L"RegGetValueW(HKCU, Software\\LiquidDock, TaskbarState, 0x10, type null, size 4)", L"GetTickCount64",
                      L"CreateMutexW(null, 0, LiquidDock.SingleInstance)", L"WaitForSingleObject(mutex, 0)", L"Restore",
                      L"ReleaseMutex(mutex)", L"CloseHandle(mutex)" },
         "...through RestoreTaskbarWhenIdle: the instance mutex taken without waiting, Restore while holding it, released and closed");
    Check(mi::logs.empty(), "...no dock.log line from the watchdog");

    mi::Reset(); w.record = true; w.mutexWait = WAIT_TIMEOUT;   // a new dock started meanwhile and holds the mutex
    rc = RunWatchdog(4242, nullptr);
    Check(rc == 0 && w.restores == 0, "watchdog: another dock holds the instance mutex - no restore, exit code 0 (the new dock owns recovery)");
    Check(mi::calls.size() >= 2 && mi::calls[mi::calls.size() - 2] == L"WaitForSingleObject(mutex, 0)" && mi::calls.back() == L"CloseHandle(mutex)" &&
          mi::Count(L"ReleaseMutex(mutex)") == 0, "...the mutex handle closed, nothing released");

    mi::Reset(); w.record = true; w.mutexWait = WAIT_ABANDONED;   // the dock died holding it
    rc = RunWatchdog(4242, nullptr);
    Check(rc == 0 && w.restores == 1 && mi::Count(L"ReleaseMutex(mutex)") == 1, "watchdog: an abandoned mutex counts as taken - restored, released, exit code 0");

    mi::Reset(); w.record = true; w.restore = [](int) { return false; };   // Explorer does not come back within 30 s
    rc = RunWatchdog(4242, nullptr);
    Check(rc == ERROR_RETRY && w.restores == 121, "watchdog: the restore fails for 30 s - exit code ERROR_RETRY (1237), the record left for the restore shortcut");

    mi::Reset(); w.record = true; w.mutexCreates = false; w.mutexError = ERROR_ACCESS_DENIED;
    rc = RunWatchdog(4242, nullptr);
    Check(rc == ERROR_ACCESS_DENIED && w.restores == 0, "watchdog: the mutex cannot be made - exit code is its error (5), no restore");

    mi::Reset(); w.processOpens = false; w.openError = ERROR_INVALID_PARAMETER;   // the dock is already gone
    rc = RunWatchdog(4242, nullptr);
    Same(mi::calls, { L"OpenProcess(0x100000, 0, 4242)", L"RegGetValueW(HKCU, Software\\LiquidDock, TaskbarState, 0x10, type null, size 4)" },
         "watchdog: the dock's pid no longer runs (ERROR_INVALID_PARAMETER) - no wait, straight to the record");
    Check(rc == 0, "...no record: exit code 0");
    mi::Reset(); w.processOpens = false; w.openError = ERROR_INVALID_PARAMETER; w.record = true;
    rc = RunWatchdog(4242, nullptr);
    Check(rc == 0 && w.restores == 1, "...with the record: restored, exit code 0");

    mi::Reset(); w.processOpens = false; w.openError = ERROR_ACCESS_DENIED;
    rc = RunWatchdog(4242, nullptr);
    Same(mi::calls, { L"OpenProcess(0x100000, 0, 4242)" }, "watchdog: the dock cannot be opened (access denied) - exit code 1 at once, nothing else");
    Check(rc == 1, "...exit code 1");

    mi::Reset(); w.processWait = WAIT_FAILED; w.record = true;
    rc = RunWatchdog(4242, nullptr);
    Same(mi::calls, { L"OpenProcess(0x100000, 0, 4242)", L"WaitForSingleObject(process, INFINITE)", L"CloseHandle(process)" },
         "watchdog: the wait for the dock fails - exit code 1, the record not read");
    Check(rc == 1 && w.restores == 0, "...exit code 1, no restore");

    // --ready <event>: the dock waits for this handshake (StartWatchdog) before it hides the taskbar
    mi::Reset();
    rc = RunWatchdog(4242, L"Local\\LiquidDock.WatchdogReady.{X} with spaces");
    Same(mi::calls, { L"OpenProcess(0x100000, 0, 4242)", L"OpenEventW(0x2, 0, Local\\LiquidDock.WatchdogReady.{X} with spaces)",
                      L"SetEvent(ready)", L"CloseHandle(ready)", L"WaitForSingleObject(process, INFINITE)", L"CloseHandle(process)",
                      L"RegGetValueW(HKCU, Software\\LiquidDock, TaskbarState, 0x10, type null, size 4)" },
         "watchdog --ready: the event opened for EVENT_MODIFY_STATE by its whole name, set and closed before the wait for the dock");
    Check(rc == 0, "...exit code 0");

    mi::Reset(); w.eventOpens = false; w.record = true;   // tests\watchdog_handshake_test.ps1: a missing readiness event
    rc = RunWatchdog(4242, L"Local\\Missing");
    Same(mi::calls, { L"OpenProcess(0x100000, 0, 4242)", L"OpenEventW(0x2, 0, Local\\Missing)", L"CloseHandle(process)" },
         "watchdog --ready: the event cannot be opened - the dock closed, exit code 1 without any recovery (record or not)");
    Check(rc == 1 && w.restores == 0, "...exit code 1, no restore");

    mi::Reset(); w.eventSets = false; w.record = true;
    rc = RunWatchdog(4242, L"Local\\X");
    Same(mi::calls, { L"OpenProcess(0x100000, 0, 4242)", L"OpenEventW(0x2, 0, Local\\X)", L"SetEvent(ready)", L"CloseHandle(ready)",
                      L"CloseHandle(process)" }, "watchdog --ready: SetEvent fails - event and dock closed, exit code 1");
    Check(rc == 1 && w.restores == 0, "...exit code 1, no restore");

    mi::Reset(); w.processOpens = false; w.openError = ERROR_INVALID_PARAMETER; w.record = true;
    rc = RunWatchdog(4242, L"Local\\X");
    Same(mi::calls, { L"OpenProcess(0x100000, 0, 4242)", L"OpenEventW(0x2, 0, Local\\X)", L"CloseHandle(ready)" },
         "watchdog --ready with the dock already gone - the event opened and closed unset, exit code 1 (the dock never hears it is ready)");
    Check(rc == 1 && w.restores == 0, "...exit code 1, no restore");
}

// ---------------------------------------------------------------- RestoreTaskbarWhenIdle
static void RestoreWhenIdleTests() {
    mi::Reset(); w.restore = [](int) { return false; };   // Explorer restarting: every try fails
    int rc = RestoreTaskbarWhenIdle(30000);
    Check(rc == ERROR_RETRY, "restore when idle: every try fails for 30 s - ERROR_RETRY (the record stays for later)");
    std::printf("  30 s of failing tries: %d tries, %d sleeps, %d mutexes made\n", w.restores, mi::Count(L"Sleep(250)"), mi::Count(L"CreateMutexW(null, 0, LiquidDock.SingleInstance)"));
    Check(w.restores == 121 && mi::Count(L"Sleep(250)") == 120, "...121 tries 250 ms apart (at 0, 0.25 ... 30 s), 120 sleeps of 250 ms");
    Check(mi::Count(L"CreateMutexW(null, 0, LiquidDock.SingleInstance)") == 121 && mi::Count(L"ReleaseMutex(mutex)") == 121 &&
          mi::Count(L"CloseHandle(mutex)") == 121, "...the mutex made, released and closed on every try");
    Check(!w.sleptHolding && !w.restoredUnheld, "...the mutex released between tries (a fresh dock can start while Explorer restarts), held during each Restore");
    {   // one try: its calls in order
        std::vector<std::wstring> first(mi::calls.begin(), mi::calls.begin() + std::min<size_t>(mi::calls.size(), 9));
        Same(first, { L"GetTickCount64", L"CreateMutexW(null, 0, LiquidDock.SingleInstance)", L"WaitForSingleObject(mutex, 0)", L"Restore",
                      L"ReleaseMutex(mutex)", L"CloseHandle(mutex)", L"GetTickCount64", L"Sleep(250)", L"CreateMutexW(null, 0, LiquidDock.SingleInstance)" },
             "...a try: the clock read once at the start, then mutex, Restore, release, close, the clock, Sleep(250)");
    }

    mi::Reset(); w.restore = [](int n) { return n == 3; };
    rc = RestoreTaskbarWhenIdle(30000);
    Check(rc == 0 && w.restores == 4 && mi::Count(L"Sleep(250)") == 3, "restore when idle: the fourth try works - 0, after three sleeps");

    mi::Reset(); w.restore = [](int) { return false; };
    rc = RestoreTaskbarWhenIdle(5000);   // the restore shortcut and --quit (main.cpp)
    Check(rc == ERROR_RETRY && w.restores == 21 && mi::Count(L"Sleep(250)") == 20, "restore when idle (5 s, the restore shortcut): 21 tries, then ERROR_RETRY");

    mi::Reset(); w.restore = [](int) { return false; };
    rc = RestoreTaskbarWhenIdle(0);
    Check(rc == ERROR_RETRY && w.restores == 1 && mi::Count(L"Sleep(250)") == 0, "restore when idle (0 s): one try, no sleep");

    mi::Reset();
    w.restore = [](int n) { if (n == 0) w.mutexWait = WAIT_TIMEOUT; return false; };   // a dock takes the mutex after the first try
    rc = RestoreTaskbarWhenIdle(30000);
    Check(rc == ERROR_BUSY && w.restores == 1 && mi::Count(L"Sleep(250)") == 1, "restore when idle: a dock takes the mutex between tries - ERROR_BUSY on the next try, no Restore then");

    mi::Reset(); w.mutexCreates = false; w.mutexError = ERROR_INVALID_HANDLE;
    rc = RestoreTaskbarWhenIdle(30000);
    Check(rc == ERROR_INVALID_HANDLE && w.restores == 0, "restore when idle: the mutex cannot be made - its error code, no Restore");

    mi::Reset(); w.restore = [](int n) { if (n == 1) w.mutexCreates = false, w.mutexError = ERROR_NOT_ENOUGH_MEMORY; return false; };
    rc = RestoreTaskbarWhenIdle(30000);
    Check(rc == ERROR_NOT_ENOUGH_MEMORY && w.restores == 2, "...also on a later try (after two failed ones): its error code at once");
}

// ---------------------------------------------------------------- StartWatchdog, WatchdogReady
static void StartWatchdogTests() {
    mi::Reset();
    Check(!WatchdogReady() && mi::calls.empty(), "WatchdogReady: no watchdog - false, nothing asked");

    mi::Reset();
    bool ok = StartWatchdog();
    Check(ok && g_watchdogProcess && mi::NameOf(g_watchdogProcess) == L"watchdog", "StartWatchdog: the watchdog acknowledged - ready, its process handle kept");
    Same(mi::calls, { L"GetModuleFileNameW(null, 260)", L"CoCreateGuid", L"StringFromGUID2(the guid made, 40)",
                      L"CreateEventW(null, 1, 0, Local\\LiquidDock.WatchdogReady.{00112233-4455-6677-8899-AABBCCDDEEFF})",
                      L"CreateProcessW(null, [\"C:\\Users\\Test User\\AppData\\Local\\Programs\\LiquidDock\\LiquidDock.exe\" --watchdog 4242 --ready Local\\LiquidDock.WatchdogReady.{00112233-4455-6677-8899-AABBCCDDEEFF}], null, null, 0, 0x8, null, null, si.cb 104)",
                      L"CloseHandle(thread)", L"WaitForMultipleObjects(2, {ready, watchdog}, 0, 5000)", L"CloseHandle(ready)",
                      L"WaitForSingleObject(watchdog, 0)" },
         "StartWatchdog: a manual-reset event Local\\LiquidDock.WatchdogReady.<guid>, the exe itself run detached with --watchdog <pid> --ready <event>, 5 s for the event or the process");
    Check(w.cmdLine == L"\"C:\\Users\\Test User\\AppData\\Local\\Programs\\LiquidDock\\LiquidDock.exe\" --watchdog 4242 --ready Local\\LiquidDock.WatchdogReady.{00112233-4455-6677-8899-AABBCCDDEEFF}",
          "...the command line: the quoted exe path, --watchdog, this pid, --ready and the event's whole name");
    mi::calls.clear();
    Check(WatchdogReady(), "WatchdogReady: the watchdog runs (its wait times out) - true");
    Same(mi::calls, { L"WaitForSingleObject(watchdog, 0)" }, "...one zero wait on its process");
    w.watchdogAlive = WAIT_OBJECT_0;
    Check(!WatchdogReady(), "WatchdogReady: the watchdog has exited - false");
    w.watchdogAlive = WAIT_FAILED;
    Check(!WatchdogReady(), "WatchdogReady: the wait fails - false");

    mi::Reset(); w.readyWait = WAIT_OBJECT_0 + 1;   // the watchdog exited before it said ready
    ok = StartWatchdog();
    Check(!ok && !g_watchdogProcess, "StartWatchdog: the watchdog exits before the event - not ready, no handle kept");
    Check(mi::Count(L"CloseHandle(watchdog)") == 1 && mi::Count(L"CloseHandle(thread)") == 1 && mi::Count(L"CloseHandle(ready)") == 1 &&
          mi::Count(L"WaitForSingleObject(watchdog, 0)") == 0, "...process, thread and event closed; WatchdogReady asks nothing");
    mi::Reset(); w.readyWait = WAIT_TIMEOUT;
    ok = StartWatchdog();
    Check(!ok && !g_watchdogProcess && mi::Count(L"CloseHandle(watchdog)") == 1, "StartWatchdog: no answer within 5 s - not ready, the process handle closed");

    mi::Reset(); w.processMade = false;
    ok = StartWatchdog();
    Check(!ok && !g_watchdogProcess && mi::Count(L"WaitForMultipleObjects(2, {ready, watchdog}, 0, 5000)") == 0 && mi::calls.back() == L"CloseHandle(ready)",
          "StartWatchdog: the process cannot be started - not ready, the event closed, no wait");
    mi::Reset(); w.eventMade = false;
    ok = StartWatchdog();
    Check(!ok && mi::calls.back().rfind(L"CreateEventW(", 0) == 0, "StartWatchdog: the event cannot be made - not ready, nothing started");
    mi::Reset(); w.guidMade = false;
    ok = StartWatchdog();
    Same(mi::calls, { L"GetModuleFileNameW(null, 260)", L"CoCreateGuid" }, "StartWatchdog: no GUID - not ready, nothing made or started");
    Check(!ok, "...false");
}

// ---------------------------------------------------------------- RestartUnelevated, ShellExecuteAsUser
static void RestartTests() {
    mi::Reset(); w.elevation = TokenElevationTypeLimited;
    Check(!RestartUnelevated(L"--welcome"), "RestartUnelevated: not elevated (a limited token) - false, the dock runs on");
    Same(mi::calls, { L"OpenProcessToken(this process, 0x8)", L"GetTokenInformation(token, 18, 4)", L"CloseHandle(token)" },
         "...the token opened for TOKEN_QUERY, its elevation type read and closed - nothing else");
    mi::Reset(); w.elevation = TokenElevationTypeDefault;
    Check(!RestartUnelevated(L""), "RestartUnelevated: UAC off (the default type) - false");
    mi::Reset(); w.tokenOpens = false; w.elevation = TokenElevationTypeFull;
    Check(!RestartUnelevated(L"") && mi::calls.size() == 1, "RestartUnelevated: the token cannot be opened - false, taken as not elevated");

    mi::Reset(); w.elevation = TokenElevationTypeFull; w.mutexOpensAt = 2;
    bool ok = RestartUnelevated(L"--welcome --profile  ");
    Check(ok, "RestartUnelevated: elevated, the copy started as the user takes the mutex - true (this copy leaves)");
    Same(mi::calls, { L"OpenProcessToken(this process, 0x8)", L"GetTokenInformation(token, 18, 4)", L"CloseHandle(token)",
                      L"GetModuleFileNameW(null, 260)", L"CoCreateInstance(CLSID_ShellWindows, null, 0x4, IShellWindows)",
                      L"windows: FindWindowSW(vt 3 0, root vt 0, class 8, options 0x1)", L"desktop: QueryInterface(IServiceProvider)",
                      L"desktop: QueryService(SID_STopLevelBrowser, IShellBrowser)", L"browser: QueryActiveShellView",
                      L"view: GetItemObject(SVGIO_BACKGROUND, IDispatch)", L"background: QueryInterface(IShellFolderViewDual)",
                      L"background: get_Application", L"app: QueryInterface(IShellDispatch2)", L"app: ShellExecute",
                      L"released app", L"released background", L"released view", L"released browser", L"released desktop", L"released windows",
                      L"OpenMutexW(0x100000, 0, LiquidDock.SingleInstance)", L"Sleep(100)", L"OpenMutexW(0x100000, 0, LiquidDock.SingleInstance)",
                      L"Sleep(100)", L"OpenMutexW(0x100000, 0, LiquidDock.SingleInstance)", L"CloseHandle(copy's mutex)" },
         "...the desktop's shell found (ShellWindows out of process, FindWindowSW CSIDL_DESKTOP / SWC_DESKTOP / SWFO_NEEDDISPATCH), every object released (last first) before the wait, then the mutex looked for every 100 ms");
    Check(w.shFile == kExe && w.shArgsVt == VT_BSTR && w.shArgs == L"--welcome --profile --from-admin" && w.shDirVt == VT_BSTR && w.shDir.empty() &&
          w.shVerbVt == VT_BSTR && w.shVerb.empty() && w.shShowVt == VT_I4 && w.shShow == SW_SHOWNORMAL,
          "...ShellExecute(the exe, its arguments without trailing spaces + \" --from-admin\", dir \"\", verb \"\", SW_SHOWNORMAL)");
    Check(ShellRefs() == 0, "...no reference to the shell's objects kept");
    Check(mi::logs.empty(), "...no dock.log line");

    mi::Reset(); w.elevation = TokenElevationTypeFull; w.mutexOpensAt = 0;
    Check(RestartUnelevated(L"") && w.shArgs == L"--from-admin", "RestartUnelevated: no arguments - \"--from-admin\" alone");
    mi::Reset(); w.elevation = TokenElevationTypeFull; w.mutexOpensAt = 0;
    Check(RestartUnelevated(L"   ") && w.shArgs == L"--from-admin", "...only spaces - \"--from-admin\" alone");
    mi::Reset(); w.elevation = TokenElevationTypeFull; w.mutexOpensAt = 0;
    Check(RestartUnelevated(L" --settings") && w.shArgs == L" --settings --from-admin", "...a leading space kept: \" --settings --from-admin\"");

    mi::Reset(); w.elevation = TokenElevationTypeFull; w.mutexOpensAt = -1;   // the copy never comes up
    ok = RestartUnelevated(L"--welcome");
    Check(!ok, "RestartUnelevated: the copy never takes the mutex - false, this copy stays elevated");
    Check(mi::Count(L"OpenMutexW(0x100000, 0, LiquidDock.SingleInstance)") == 100 && mi::Count(L"Sleep(100)") == 100,
          "...100 looks, a Sleep(100) after each (10 s)");
    Same(mi::logs, { L"started as administrator; the copy started as the user did not come up: staying elevated" }, "...dock.log: 'the copy started as the user did not come up: staying elevated'");

    struct Step { mi::Shell s; const wchar_t* log; const char* what; };
    const Step steps[] = {
        { mi::Shell::CoCreate, L"started as administrator; starting as the user failed (80040201): staying elevated", "ShellWindows cannot be made" },
        { mi::Shell::FindWindowFails, L"started as administrator; starting as the user failed (80040202): staying elevated", "FindWindowSW fails" },
        { mi::Shell::FindWindowSFalse, L"started as administrator; starting as the user failed (80004005): staying elevated", "FindWindowSW finds no desktop (S_FALSE): E_FAIL" },
        { mi::Shell::FindWindowNoDesktop, L"started as administrator; starting as the user failed (80004005): staying elevated", "FindWindowSW gives no object: E_FAIL" },
        { mi::Shell::QiServices, L"started as administrator; starting as the user failed (80040205): staying elevated", "no IServiceProvider" },
        { mi::Shell::QueryService, L"started as administrator; starting as the user failed (80040206): staying elevated", "no top-level browser" },
        { mi::Shell::ActiveView, L"started as administrator; starting as the user failed (80040207): staying elevated", "no active shell view" },
        { mi::Shell::ItemObject, L"started as administrator; starting as the user failed (80040208): staying elevated", "no background object" },
        { mi::Shell::QiFolderView, L"started as administrator; starting as the user failed (80040209): staying elevated", "no IShellFolderViewDual" },
        { mi::Shell::Application, L"started as administrator; starting as the user failed (8004020a): staying elevated", "no Application" },
        { mi::Shell::QiShellDispatch2, L"started as administrator; starting as the user failed (8004020b): staying elevated", "no IShellDispatch2" },
        { mi::Shell::Execute, L"started as administrator; starting as the user failed (8004020c): staying elevated", "ShellExecute fails" },
    };
    for (const Step& s : steps) {
        mi::Reset(); w.elevation = TokenElevationTypeFull; w.mutexOpensAt = 0; w.shellFails = s.s;
        ok = RestartUnelevated(L"--x");
        const std::string what = std::string("RestartUnelevated: ") + s.what + " - false, its HRESULT in dock.log, no wait, nothing kept";
        Check(!ok && mi::logs.size() == 1 && mi::logs[0] == s.log && mi::Count(L"OpenMutexW(0x100000, 0, LiquidDock.SingleInstance)") == 0 &&
              ShellRefs() == 0, what.c_str());
        if (mi::logs.size() == 1 && mi::logs[0] != s.log) std::printf("  got: %s\n", mi::U8(mi::logs[0]).c_str());
    }
}

// ---------------------------------------------------------------- CrashHandler
static void CrashHandlerTests() {
    mi::Reset(); w.mutexHeld = true;   // (the fake's Restore names whether the mutex is held; the dock holds it)
    const LONG r = CrashHandler(nullptr);
    Check(r == EXCEPTION_CONTINUE_SEARCH, "CrashHandler: EXCEPTION_CONTINUE_SEARCH (Windows Error Reporting goes on)");
    Same(mi::calls, { L"Restore" }, "...it restores the taskbar and nothing else (no mutex, no log, no exit)");
    mi::Reset(); w.mutexHeld = true; w.restore = [](int) { return false; };
    Check(CrashHandler(nullptr) == EXCEPTION_CONTINUE_SEARCH && w.restores == 1, "...a failed restore: the same (the watchdog tries again)");
}

int main() {
    WatchdogTests();
    RestoreWhenIdleTests();
    StartWatchdogTests();
    RestartTests();
    CrashHandlerTests();
    return Summary("main_instance_test");
}
