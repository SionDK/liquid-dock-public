#include "common.h"
#include "system.h"          // g_taskbar
#include "main_internal.h"
#include <shellapi.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlobj.h>
#include <shlguid.h>

HANDLE g_watchdogProcess = nullptr;

bool WatchdogReady() {
    return g_watchdogProcess && WaitForSingleObject(g_watchdogProcess, 0) == WAIT_TIMEOUT;
}

// ---------------------------------------------------------------- started as administrator
// Windows keeps ordinary (unelevated) programs from reaching into an elevated one (UIPI): FluentFlyout
// cannot put its widget into the dock's window, tray programs' clicks and requests fall short. The
// logon task starts the dock unelevated; a start from an elevated window (an admin terminal, "run as
// administrator") is handed to Explorer to start it as the user instead (Raymond Chen's way: the
// desktop's IShellDispatch2), and this copy leaves. --keep-admin keeps it elevated.
static HRESULT ShellExecuteAsUser(const std::wstring& file, const std::wstring& params) {
    ComPtr<IShellWindows> windows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
    if (FAILED(hr)) return hr;
    VARIANT where; VariantInit(&where); where.vt = VT_I4; where.lVal = CSIDL_DESKTOP;
    VARIANT none; VariantInit(&none);
    long hwnd = 0;
    ComPtr<IDispatch> desktop;
    hr = windows->FindWindowSW(&where, &none, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop);
    if (hr != S_OK || !desktop) return FAILED(hr) ? hr : E_FAIL;
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background, app;
    ComPtr<IShellFolderViewDual> folderView;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(hr = desktop.As(&services)) || FAILED(hr = services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(hr = browser->QueryActiveShellView(&view)) || FAILED(hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) ||
        FAILED(hr = background.As(&folderView)) || FAILED(hr = folderView->get_Application(&app)) || FAILED(hr = app.As(&shell)))
        return hr;
    BSTR bfile = SysAllocString(file.c_str());
    VARIANT args; VariantInit(&args); args.vt = VT_BSTR; args.bstrVal = SysAllocString(params.c_str());
    VARIANT dir; VariantInit(&dir); dir.vt = VT_BSTR; dir.bstrVal = SysAllocString(L"");
    VARIANT verb; VariantInit(&verb); verb.vt = VT_BSTR; verb.bstrVal = SysAllocString(L"");
    VARIANT show; VariantInit(&show); show.vt = VT_I4; show.lVal = SW_SHOWNORMAL;
    hr = shell->ShellExecute(bfile, args, dir, verb, show);
    SysFreeString(bfile);
    VariantClear(&args); VariantClear(&dir); VariantClear(&verb);
    return hr;
}

bool RestartUnelevated(const std::wstring& args) {
    HANDLE token = nullptr;
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD n = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &n);
        CloseHandle(token);
    }
    if (type != TokenElevationTypeFull) return false;   // not elevated, or UAC off (everything runs alike)
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring params = args;
    while (!params.empty() && params.back() == L' ') params.pop_back();
    params += params.empty() ? L"--from-admin" : L" --from-admin";
    HRESULT hr = ShellExecuteAsUser(exe, params);
    if (FAILED(hr)) { Log(L"started as administrator; starting as the user failed (%08x): staying elevated", hr); return false; }
    // the new copy takes the instance mutex once it runs; leave only then
    for (int i = 0; i < 100; ++i) {
        if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutex)) { CloseHandle(m); return true; }
        Sleep(100);
    }
    Log(L"started as administrator; the copy started as the user did not come up: staying elevated");
    return false;
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {
    g_taskbar.Restore();
    return EXCEPTION_CONTINUE_SEARCH;
}

// A second copy of the exe that waits for the dock to exit. If the dock was killed (Task Manager,
// crash the handler missed), the saved taskbar state is still there: restore the taskbar.
// Serialize recovery with the next dock instance, but release the mutex between retries
// so an Explorer restart cannot prevent the user starting a fresh dock.
int RestoreTaskbarWhenIdle(DWORD timeoutMs) {
    ULONGLONG start = GetTickCount64();
    do {
        HANDLE m = CreateMutexW(nullptr, FALSE, kInstanceMutex);
        if (!m) return (int)GetLastError();
        DWORD wait = WaitForSingleObject(m, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(m); return ERROR_BUSY; }
        bool ok = g_taskbar.Restore();
        ReleaseMutex(m); CloseHandle(m);
        if (ok) return 0;
        if (GetTickCount64() - start >= timeoutMs) break;
        Sleep(250);
    } while (true);
    // The registry backup remains available to the restore shortcut or a later run.
    return ERROR_RETRY;
}

int RunWatchdog(DWORD pid, const wchar_t* readyName) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p && GetLastError() != ERROR_INVALID_PARAMETER) return 1;
    if (readyName) {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
        if (!p || !ready) { if (p) CloseHandle(p); if (ready) CloseHandle(ready); return 1; }
        BOOL signaled = SetEvent(ready); CloseHandle(ready);
        if (!signaled) { CloseHandle(p); return 1; }
    }
    if (p) {
        DWORD wait = WaitForSingleObject(p, INFINITE);
        CloseHandle(p);
        if (wait != WAIT_OBJECT_0) return 1;
    }
    DWORD st = 0, sz = sizeof(st);
    if (RegGetValueW(HKEY_CURRENT_USER, LD_TASKBAR_STATE_KEY, LD_TASKBAR_STATE_VALUE, RRF_RT_REG_DWORD, nullptr, &st, &sz) != ERROR_SUCCESS)
        return 0;   // clean exit: already restored
    int rc = RestoreTaskbarWhenIdle(30000);
    return rc == ERROR_BUSY ? 0 : rc;   // a new dock owns recovery now
}

bool StartWatchdog() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GUID id; wchar_t guid[40] = {};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, guid, ARRAYSIZE(guid))) return false;
    std::wstring name = L"Local\\LiquidDock.WatchdogReady." + std::wstring(guid);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    if (!ready) return false;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --watchdog " + std::to_wstring(GetCurrentProcessId()) + L" --ready " + name;
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        HANDLE waits[] = { ready, pi.hProcess };
        if (WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0) g_watchdogProcess = pi.hProcess;
        else CloseHandle(pi.hProcess);
    }
    CloseHandle(ready);
    return WatchdogReady();
}
