// Which top-level "Shell_TrayWnd" is Explorer's taskbar (src/explorer_taskbar.h), with the window lookups
// faked: the dock's own tray host (marked), another program's tray window (RetroBar), Explorer restarting
// (no desktop window yet), a replacement shell. No real window is created or looked at.
//   cl /std:c++20 /EHsc /utf-8 tests\tray_explorer_taskbar_test.cpp
#include "../src/common.h"
#include "portable_check.h"
#include <cwchar>

#define kTrayHostPropName L"LiquidDock.TrayHost"   // explorer_taskbar.h defines kTrayHostProp only after the fakes

namespace fake {
struct Wnd { HWND h; DWORD pid; bool host; };
std::vector<Wnd> trays;    // the top-level "Shell_TrayWnd" windows, in z-order
HWND shell = nullptr;      // GetShellWindow(): the desktop
DWORD shellPid = 0;
const Wnd* Of(HWND h) { for (auto& w : trays) if (w.h == h) return &w; return nullptr; }
}

static HWND WINAPI FakeFindWindowW(LPCWSTR cls, LPCWSTR) {
    return cls && !wcscmp(cls, L"Shell_TrayWnd") && !fake::trays.empty() ? fake::trays.front().h : nullptr;
}
static HWND WINAPI FakeFindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR) {
    if (parent || !cls || wcscmp(cls, L"Shell_TrayWnd")) return nullptr;
    for (size_t i = 0; i + 1 < fake::trays.size(); ++i) if (fake::trays[i].h == after) return fake::trays[i + 1].h;
    return nullptr;
}
static HANDLE WINAPI FakeGetPropW(HWND h, LPCWSTR name) {
    const auto* w = fake::Of(h);
    return w && w->host && !wcscmp(name, kTrayHostPropName) ? (HANDLE)1 : nullptr;
}
static HWND WINAPI FakeGetShellWindow() { return fake::shell; }
static DWORD WINAPI FakeGetWindowThreadProcessId(HWND h, LPDWORD pid) {
    DWORD p = h == fake::shell ? fake::shellPid : fake::Of(h) ? fake::Of(h)->pid : 0;
    if (pid) *pid = p;
    return p ? 1 : 0;
}
#define FindWindowW FakeFindWindowW
#define FindWindowExW FakeFindWindowExW
#define GetPropW FakeGetPropW
#define GetShellWindow FakeGetShellWindow
#define GetWindowThreadProcessId FakeGetWindowThreadProcessId
#include "../src/explorer_taskbar.h"

int main() {
    const HWND explorer = (HWND)0x100, host = (HWND)0x200, retrobar = (HWND)0x300, desktop = (HWND)0x10;
    const DWORD explorerPid = 1000, dockPid = 2000, retrobarPid = 3000;
    Check(!wcscmp(kTrayHostProp, kTrayHostPropName), "the mark the fake knows is the dock's");

    fake::shell = desktop; fake::shellPid = explorerPid;
    fake::trays = { { explorer, explorerPid, false } };
    Check(ExplorerTaskbar() == explorer, "Explorer's taskbar alone");
    fake::trays = { { host, dockPid, true }, { explorer, explorerPid, false } };
    Check(ExplorerTaskbar() == explorer, "the dock's tray host in front of it: skipped");
    fake::trays = { { host, dockPid, true }, { retrobar, retrobarPid, false }, { explorer, explorerPid, false } };
    Check(ExplorerTaskbar() == explorer, "another program's tray window in front: the one of the desktop's process wins");
    fake::trays = { { host, dockPid, true } };
    Check(ExplorerTaskbar() == nullptr, "only the dock's tray host: none");
    fake::trays = {};
    Check(ExplorerTaskbar() == nullptr, "no tray window at all: none");

    fake::shell = nullptr; fake::shellPid = 0;   // Explorer is (re)starting: no desktop yet
    fake::trays = { { host, dockPid, true }, { explorer, explorerPid, false } };
    Check(ExplorerTaskbar() == explorer, "no desktop yet: the first tray window that is not the dock's");

    fake::shell = desktop; fake::shellPid = 4000;   // a replacement shell owns the desktop
    fake::trays = { { host, dockPid, true }, { retrobar, retrobarPid, false } };
    Check(ExplorerTaskbar() == retrobar, "a replacement shell: the first tray window that is not the dock's");
    return Summary("tray_explorer_taskbar_test");
}
