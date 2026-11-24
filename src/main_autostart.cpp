#include "common.h"
#include "logon_task.h"
#include "main_internal.h"

// Starting with Windows (settings: autostart; WM_APP_AUTOSTART from the settings window, and at every start of the dock
// while it is on): the Run value, and the per-user logon task made or deleted on a thread of its own (schtasks).
void SetAutostart(bool on) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (on) {
            std::wstring v = L"\"" + std::wstring(exe) + L"\"";
            RegSetValueExW(k, L"LiquidDock", 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
        } else RegDeleteValueW(k, L"LiquidDock");
        RegCloseKey(k);
    }
    // The Run key starts ~10+ s after logon (Windows' startup delay), leaving the native taskbar on
    // screen meanwhile. A per-user logon task (no admin needed) starts right away; the Run key stays
    // as a fallback - a second instance just exits.
    std::wstring path = exe;
    const bool runs = TryDetach([on, path] {
        auto run = [](std::wstring cmd) {
            STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {};
            if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
            WaitForSingleObject(pi.hProcess, 15000);
            DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            return (int)code;
        };
        if (!on) { run(SystemExe(L"schtasks.exe") + L" /Delete /F /TN LiquidDock"); return; }
        wchar_t user[256] = {}, dom[256] = {};
        GetEnvironmentVariableW(L"USERNAME", user, 256);
        GetEnvironmentVariableW(L"USERDOMAIN", dom, 256);
        std::wstring xml = LogonTaskXml(user, dom, path);
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        std::wstring file = std::wstring(tmp) + L"LiquidDockLogon.xml";
        HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD bw; WORD bom = 0xFEFF;
        WriteFile(f, &bom, 2, &bw, nullptr);
        WriteFile(f, xml.data(), DWORD(xml.size() * 2), &bw, nullptr);
        CloseHandle(f);
        int rc = run(SystemExe(L"schtasks.exe") + L" /Create /F /TN LiquidDock /XML \"" + file + L"\"");
        DeleteFileW(file.c_str());
        Log(L"logon task rc=%d", rc);
    });
    if (!runs) Log(L"logon task: no thread to %ls it on: the Run key alone", on ? L"make" : L"delete");
}
