#include "admin_helper.h"
#include "sensor_shared.h"
#include <shellapi.h>

// The elevated helper's own process: its install (one UAC prompt) and its scheduled task (schtasks.exe), for the
// sensors (sensors.cpp) and the windows of programs run as administrator (admin_helper.cpp). The same steps for
// both (HelperPath is not ShippedPath: a cut path gives the folder left, not the bare name).

static std::wstring HelperPath() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\') + 1) + L"LiquidDockSensors.exe";
}

static int RunHidden(const std::wstring& cmd, DWORD timeoutMs) {
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring c = cmd;
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

namespace adminhelper {

// Must run on the UI thread right after a user click: Windows only shows a UAC prompt in
// front for the foreground app (otherwise it just flashes on the - hidden - taskbar).
bool InstallHelper(HWND owner) {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    std::wstring path = HelperPath();
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = path.c_str();
    sei.lpParameters = L"--install";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) { Log(L"sensor helper install not started (%lu)", GetLastError()); return false; }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

// Start the elevated helper through its scheduled task (no UAC prompt once installed).
void StartHelper() {
    if (GetFileAttributesW(HelperPath().c_str()) == INVALID_FILE_ATTRIBUTES) return;
    int rc = RunHidden(SystemExe(L"schtasks.exe") + L" /Run /TN " LD_SENSOR_TASK, 8000);
    Log(L"sensor task run rc=%d", rc);
}

bool HelperTaskRegistered() {
    return RunHidden(SystemExe(L"schtasks.exe") + L" /Query /TN " LD_SENSOR_TASK, 8000) == 0;
}

}   // namespace adminhelper
