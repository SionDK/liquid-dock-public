// Which "Shell_TrayWnd" is Explorer's taskbar (src/explorer_taskbar.h), what the start button presses into
// and the dock hides: the dock's own tray host (tray_host.h) owns one too, kept in front of Explorer's, and
// another program may run one (RetroBar). The window list, the shell's window and the tray host's mark are
// fakes here: no real window is looked at.
//   cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE tests\start_menu_explorer_taskbar_test.cpp /link user32.lib
#include "../src/common.h"
#include "portable_check.h"
#include <cstdint>
#include <vector>

// ---- the top-level windows of class Shell_TrayWnd, front to back, and the shell's process
struct FakeWnd { HWND h; DWORD pid; bool host; };
static std::vector<FakeWnd> g_trays;
static DWORD g_shellPid = 0;   // 0: no desktop window (Explorer restarting)
static int g_calls = 0;        // windows asked for
static HWND Fake(uintptr_t n) { return reinterpret_cast<HWND>(n); }
static const FakeWnd* Find(HWND h) { for (const auto& w : g_trays) if (w.h == h) return &w; return nullptr; }
HWND WINAPI FakeShellWindow() { return g_shellPid ? Fake(0x9000) : nullptr; }
HWND WINAPI FakeFindWindowW(LPCWSTR cls, LPCWSTR) {
    ++g_calls;
    return !wcscmp(cls, L"Shell_TrayWnd") && !g_trays.empty() ? g_trays[0].h : nullptr;
}
HWND WINAPI FakeFindWindowExW(HWND parent, HWND after, LPCWSTR cls, LPCWSTR) {
    ++g_calls;
    if (parent || wcscmp(cls, L"Shell_TrayWnd")) return nullptr;
    for (size_t i = 0; i + 1 < g_trays.size(); ++i) if (g_trays[i].h == after) return g_trays[i + 1].h;
    return nullptr;
}
HANDLE WINAPI FakeGetPropW(HWND h, LPCWSTR name) {
    const FakeWnd* w = Find(h);
    return w && w->host && !wcscmp(name, L"LiquidDock.TrayHost") ? reinterpret_cast<HANDLE>(1) : nullptr;
}
DWORD WINAPI FakeGetWindowThreadProcessId(HWND h, LPDWORD pid) {
    DWORD p = h == Fake(0x9000) ? g_shellPid : 0;
    if (const FakeWnd* w = Find(h)) p = w->pid;
    if (pid) *pid = p;
    return p ? 1 : 0;
}
#define GetShellWindow FakeShellWindow
#define FindWindowW FakeFindWindowW
#define FindWindowExW FakeFindWindowExW
#define GetPropW FakeGetPropW
#define GetWindowThreadProcessId FakeGetWindowThreadProcessId
#include "../src/explorer_taskbar.h"
#undef GetShellWindow
#undef FindWindowW
#undef FindWindowExW
#undef GetPropW
#undef GetWindowThreadProcessId

int main() {
    constexpr DWORD explorer = 100, dock = 200, retrobar = 300;
    const HWND host = Fake(0x10), ex = Fake(0x20), rb = Fake(0x30), ex2 = Fake(0x40);
    g_shellPid = explorer;

    g_trays = { { host, dock, true }, { ex, explorer, false } };
    Check(ExplorerTaskbar() == ex, "the dock's tray host in front of Explorer's taskbar: Explorer's is found");
    g_trays = { { rb, retrobar, false }, { ex, explorer, false } };
    Check(ExplorerTaskbar() == ex, "another program's tray window in front: Explorer's is found (the shell's process)");
    g_trays = { { host, dock, true }, { rb, retrobar, false }, { ex, explorer, false } };
    Check(ExplorerTaskbar() == ex, "...both in front of it: Explorer's");
    g_trays = { { ex, explorer, false }, { host, dock, true } };
    Check(ExplorerTaskbar() == ex, "Explorer's in front: Explorer's");
    g_trays = { { host, dock, true } };
    Check(ExplorerTaskbar() == nullptr, "only the dock's tray host (Explorer gone): none - the dock never takes its own for Explorer's");
    g_trays = {};
    Check(ExplorerTaskbar() == nullptr, "no tray window at all: none");

    g_shellPid = 0;   // Explorer restarting: no desktop yet
    g_trays = { { host, dock, true }, { ex2, explorer + 1, false } };
    Check(ExplorerTaskbar() == ex2, "Explorer restarting (no desktop yet): its new taskbar, never the dock's tray host");
    g_trays = { { host, dock, true } };
    Check(ExplorerTaskbar() == nullptr, "...and with only the tray host: none");

    g_shellPid = explorer;   // a long list (programs that each run a tray window): bounded, no endless walk
    g_trays.clear();
    for (uintptr_t i = 0; i < 40; ++i) g_trays.push_back({ Fake(0x1000 + i), dock, true });
    g_calls = 0;
    Check(ExplorerTaskbar() == nullptr && g_calls <= 17, "forty tray hosts: the walk stops after 16 windows");
    return Summary("start_menu_explorer_taskbar_test");
}
