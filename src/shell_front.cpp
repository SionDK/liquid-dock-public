// The shell input helpers (shell_front.h): keys, the right to the foreground, the shell's pop-ups, Start shown.
#include "shell_front.h"
#include "app_visibility.h"

ForegroundRight TakeForegroundRight() {
    const DWORD me = GetCurrentProcessId();
    if (AllowSetForegroundWindow(me)) return ForegroundRight::Had;
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = 0xE8;   // "unassigned" in the virtual-key table: no program acts on it
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const bool inserted = SendInput(2, in, sizeof(INPUT)) == 2;
    if (AllowSetForegroundWindow(me)) return ForegroundRight::Taken;
    return inserted ? ForegroundRight::Refused : ForegroundRight::NotInserted;
}

const wchar_t* ForegroundRightText(ForegroundRight r) {
    switch (r) {
    case ForegroundRight::Had: return L"had it";
    case ForegroundRight::Taken: return L"taken with an unassigned key";
    case ForegroundRight::Refused: return L"refused even after an unassigned key";
    case ForegroundRight::NotInserted: return L"not taken: the key was not inserted";
    }
    return L"?";
}

bool SendKeyCombo(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    in.reserve(keys.size() * 2);
    for (WORD k : keys) { INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.end(); it != keys.begin();) { --it; INPUT i = {}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i); }
    return SendInput((UINT)in.size(), in.data(), sizeof(INPUT)) == in.size();
}

bool IsShellPopupWindow(HWND w) {
    static HWND last = nullptr; static bool lastResult = false;   // the foreground window rarely changes
    if (w == last) return lastResult;
    last = w; lastResult = false;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!p) return false;
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
        for (const wchar_t* shell : { L"StartMenuExperienceHost.exe", L"SearchHost.exe", L"SearchApp.exe",
                                      L"ShellExperienceHost.exe", L"ShellHost.exe" })
            if (!_wcsicmp(name, shell)) { lastResult = true; break; }
    }
    CloseHandle(p);
    return lastResult;
}

// IAppVisibility: app_visibility.h (start_menu.cpp asks the same)
int LauncherVisible() {
    static IAppVisibilityDecl* const vis = [] {
        IAppVisibilityDecl* v = nullptr;
        if (FAILED(CoCreateInstance(kAppVisibility, nullptr, CLSCTX_ALL, kAppVisibilityIid, reinterpret_cast<void**>(&v)))) v = nullptr;
        return v;
    }();
    BOOL shown = FALSE;
    if (!vis || FAILED(vis->IsLauncherVisible(&shown))) return -1;
    return shown ? 1 : 0;
}
