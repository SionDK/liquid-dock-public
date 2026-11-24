// The app model (apps*.cpp) against the real Windows, two things the fakes of tests\apps_model_test.cpp stand in for:
//   pins     - read only: the taskbar's own pin list on this PC (HKCU Taskband\Favorites, the User Pinned folder),
//              walked item by item as the shell reads it, against the dock's first-run import order (which finds the
//              pins' names in it as text); where the dock found each name (odd or even byte offset) is shown too.
//   restore  - a window of this test's own, maximized then minimized, its program busy for 3 s: the dock brings it up
//              (BringUp), the watch ends as when the user goes on to another program, the dock brings it up again.
//              Its program then handles every restore command it got: one leaves it maximized, two make it normal size.
// Nothing else is touched: no other program's window, no key or mouse input (the right to the front window is not
// taken: TakeForegroundRight is a stub), the elevated helper and the taskbar's buttons are stubs, nothing is written
// to the registry. tests\run_apps_model_live.bat builds and runs it.
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/admin_helper.h"
#include "../src/system.h"
#include "../src/taskbar_apps.h"
#include <shlobj.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#define private public
#include "../src/apps.h"
#undef private

Settings g_settings;
HWND g_mainWnd = nullptr;
Gfx g_gfx;   // never used: no icon is loaded
ForegroundRight TakeForegroundRight() { return ForegroundRight::Had; }   // no key input
const wchar_t* ForegroundRightText(ForegroundRight) { return L"not taken (test)"; }
namespace adminhelper {
bool Activate(HWND, const std::wstring&) { return false; }
bool Minimize(HWND, const std::wstring&) { return false; }
bool Close(HWND, const std::wstring&) { return false; }
bool ActivateOver(HWND, const std::wstring&) { return false; }
}
bool IsShellPopupWindow(HWND) { return false; }   // the shell's pop-ups: not looked for (no key is sent)
int LauncherVisible() { return -1; }              // ...nor Start
bool SendKeyCombo(std::initializer_list<WORD>) { return false; }
#include "stub_taskbar_apps.h"
#include "../src/apps.cpp"
#include "../src/apps_pins.cpp"
#include "../src/apps_click.cpp"
#include "../src/apps_press.cpp"

// ---------------------------------------------------------------- pins (read only)
// A name for the console, in its code page (a Korean shortcut name reads as such there and in the log).
static std::string Narrow(const std::wstring& s) {
    UINT cp = GetConsoleOutputCP();
    if (!cp) cp = CP_ACP;
    const int n = WideCharToMultiByte(cp, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(cp, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// The taskbar's pin list walked item by item, as the shell reads it - apart from the dock's import, which looks for
// names in it as text (apps_pins.cpp TaskbandBlob). The layout seen on 25H2 (2026-10-28): a version byte 0, then per pin
// a DWORD byte count, that many bytes of an item ID list (its IDs, then a 0 word) and one byte (0; 0xff after the
// last). Each list's name is the shell's (SIGDN_PARENTRELATIVEPARSING): a shortcut's file name, a store app's id; ""
// when the shell gives none. Empty, and why, when the blob is not laid out so.
static std::vector<std::wstring> WalkedPins(const std::vector<BYTE>& raw, std::string& why) {
    std::vector<std::wstring> names;
    if (raw.empty()) { why = "there is no pin list"; return names; }
    if (raw[0] != 0) { why = "its first byte is not 0"; return names; }
    for (size_t o = 1; o < raw.size();) {
        if (o == 1 && raw.size() == 2 && raw[1] == 0xff) break;   // the end mark alone: no pin
        DWORD cb = 0;
        if (o + 4 > raw.size()) { why = "it ends inside a byte count"; names.clear(); return names; }
        memcpy(&cb, raw.data() + o, 4);
        if (cb < 2 || cb > raw.size() - o - 4) { why = "a byte count runs past its end"; names.clear(); return names; }
        std::vector<BYTE> list(raw.begin() + o + 4, raw.begin() + o + 4 + cb);   // aligned, as the shell wants it
        size_t q = 0;
        for (USHORT c = 0; q + 2 <= cb; q += c) {
            memcpy(&c, list.data() + q, 2);
            if (!c) break;
            if (c < 2 || c > cb - q) { q = cb; break; }
        }
        if (q + 2 > cb) { why = "an item ID list in it is not whole"; names.clear(); return names; }
        std::wstring name;
        ComPtr<IShellItem> si;
        if (SUCCEEDED(SHCreateItemFromIDList((PCIDLIST_ABSOLUTE)list.data(), IID_PPV_ARGS(&si)))) {
            PWSTR n = nullptr;
            if (SUCCEEDED(si->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &n)) && n) name = n;
            CoTaskMemFree(n);
        }
        names.push_back(name);
        o += 4 + cb + 1;
    }
    return names;
}

// Whether the dock's first-run import (apps_pins.cpp TaskbarPins, as ImportTaskbarPins takes it) has every pin the walk
// found, each once, in the walk's order - and nothing the walk did not find (a name found inside another pin's).
static int Pins() {
    std::vector<BYTE> raw;
    DWORD size = 0;
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband";
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"Favorites", RRF_RT_REG_BINARY, nullptr, nullptr, &size) == ERROR_SUCCESS && size) {
        raw.resize(size);
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"Favorites", RRF_RT_REG_BINARY, nullptr, raw.data(), &size) != ERROR_SUCCESS) raw.clear();
        else raw.resize(size);
    }
    printf("PINS blob: %zu bytes\n", raw.size());
    const std::vector<TaskbarPin> dock = TaskbarPins();
    auto nameOf = [](const TaskbarPin& p) { return p.lnk.empty() ? p.aumid : p.lnk.substr(p.lnk.find_last_of(L'\\') + 1); };
    // the dock's order, and where it found each name (to read: a name at an odd byte is one the import used to miss)
    for (size_t i = 0; i < dock.size(); ++i)
        printf("PIN dock #%zu: %s %s - %s\n", i + 1, dock[i].lnk.empty() ? "store app" : "shortcut", Narrow(nameOf(dock[i])).c_str(),
               dock[i].at == SIZE_MAX ? "not named in the blob: put last (the taskbar's order unknown)"
                                      : ("at byte " + std::to_string(dock[i].at) + (dock[i].at % 2 ? " (odd)" : " (even)")).c_str());
    std::string why;
    const std::vector<std::wstring> walked = WalkedPins(raw, why);
    if (!why.empty()) {
        printf("PINS VERDICT: not checked - %s, so it could not be walked item by item (the dock's import above may still be right)\n",
               why.c_str());
        return 1;
    }
    int missed = 0, counted = 0;
    std::vector<bool> matched(dock.size());
    size_t last = 0;    // the dock's place of the last pin found in place
    bool any = false;   // ...there was one
    for (size_t k = 0; k < walked.size(); ++k) {
        const std::wstring& w = walked[k];
        const bool shortcut = EndsWithI(w, L".lnk"), store = w.find(L'!') != std::wstring::npos;
        if (w.empty()) {
            ++counted; ++missed;
            printf("PIN taskbar #%zu: the shell gives no name for it - not checked\n", k + 1);
            continue;
        }
        if (!shortcut && !store) {   // the dock imports shortcuts and store apps only
            printf("PIN taskbar #%zu: %s - neither a shortcut nor a store app: the dock's import leaves it\n", k + 1, Narrow(w).c_str());
            continue;
        }
        ++counted;
        size_t i = 0;
        while (i < dock.size() && !(_wcsicmp(nameOf(dock[i]).c_str(), w.c_str()) == 0 && dock[i].lnk.empty() == !shortcut)) ++i;
        const char* how;
        if (i == dock.size()) {
            ++missed;
            how = shortcut ? "MISSED by the dock's import (no such shortcut in the User Pinned folder?)" : "MISSED by the dock's import";
        } else if (dock[i].at == SIZE_MAX || matched[i] || (any && i < last)) {
            ++missed;
            how = "the dock's import puts it OUT OF PLACE";
        } else {
            how = "the dock's import has it in place";
            last = i;
            any = true;
        }
        if (i < dock.size()) matched[i] = true;
        printf("PIN taskbar #%zu: %s %s - %s%s\n", k + 1, shortcut ? "shortcut" : "store app", Narrow(w).c_str(), how,
               i < dock.size() ? (" (dock #" + std::to_string(i + 1) + ")").c_str() : "");
    }
    for (size_t i = 0; i < dock.size(); ++i)
        if (!matched[i] && dock[i].at != SIZE_MAX) {   // found as text, but no pin of the taskbar's
            ++missed;
            printf("PIN dock #%zu: %s - the dock's import takes it for a pin the taskbar's list does not have\n", i + 1, Narrow(nameOf(dock[i])).c_str());
        }
    printf("PINS VERDICT: %d of %d taskbar pins %s\n", missed, counted,
           missed ? "are missed or put out of order by the dock's first-run import (apps_model-2), or not checked"
                  : "missed - the dock's import reads this PC's pins in the taskbar's order");
    return missed;
}

// ---------------------------------------------------------------- restore (a window of the test's own)
static const wchar_t kClass[] = L"LiquidDock.hunt-apps_model.LiveRestore";
static std::atomic<int> restoresSeen{ 0 };
static HWND live = nullptr;
static HANDLE ready = nullptr, goBusy = nullptr;
static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_RESTORE) ++restoresSeen;
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}
static void WindowThread() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kClass; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    live = CreateWindowExW(0, kClass, L"Liquid Dock test - maximized, minimized, busy", WS_OVERLAPPEDWINDOW, 200, 200, 640, 400, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(live, SW_SHOWMAXIMIZED);
    ShowWindow(live, SW_MINIMIZE);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    SetEvent(ready);
    WaitForSingleObject(goBusy, 10000);
    Sleep(3000);   // its program busy: the restore commands wait
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
static int RestoreCheck() {
    ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    goBusy = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread t(WindowThread);
    WaitForSingleObject(ready, 10000);
    printf("RESTORE start: %s, %s\n", IsIconic(live) ? "minimized" : "NOT minimized", (GetWindowLongPtrW(live, GWL_STYLE) & WS_MAXIMIZE) || IsZoomed(live) ? "maximized" : "restores maximized");
    SetEvent(goBusy);
    Sleep(100);
    AppModel m;
    m.BringUp(live, L"live test", 1);       // the click: one restore command, unanswered
    Sleep(300);
    m.watch = {};                           // the watch ends as when the user goes on (CheckWatch: Step::Stop)
    m.BringUp(live, L"live test again", 1); // the user comes back to it within kBusyWait
    for (int i = 0; i < 100 && (IsIconic(live) || restoresSeen == 0); ++i) Sleep(50);
    Sleep(800);   // every restore command its program got is handled by now
    const bool zoomed = IsZoomed(live) != FALSE, iconic = IsIconic(live) != FALSE;
    printf("RESTORE restore commands its program got: %d; now %s\n", restoresSeen.load(), iconic ? "still minimized" : zoomed ? "maximized" : "normal size");
    const bool ok = restoresSeen == 1 && zoomed && !iconic;
    printf("RESTORE VERDICT: %s\n", ok ? "PASS - one restore, the window came back maximized"
                                       : "FAIL - a second restore command went out while its program was busy; the window did not come back maximized (apps_model-1)");
    PostMessageW(live, WM_CLOSE, 0, 0);
    t.join();
    return ok ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring what = argc > 1 ? argv[1] : L"all";
    int failures = 0;
    if (what == L"pins" || what == L"all") failures += Pins() ? 1 : 0;
    if (what == L"restore" || what == L"all") failures += RestoreCheck();
    printf("apps_model_live: %d failure%s\n", failures, failures == 1 ? "" : "s");
    return failures;
}
