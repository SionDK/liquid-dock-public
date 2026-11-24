// Drives the production tray host (src/tray_host*.cpp) with the bytes Shell_NotifyIcon sends,
// against a fake Explorer taskbar and a fake tray program, all inside this process:
//   - every message reaches "Explorer" unchanged, and the program gets the answer it expects
//   - icons, tooltips, pictures, glyph detection, hidden state, versions, deletion, dead programs
//   - clicks and hovers arrive as Explorer sends them (version 0 and version 4 formats)
//   - Shell_NotifyIconGetRect answers with the dock's position of the icon
//   - which icons are shown follows HKCU\Control Panel\NotifyIconSettings (IsPromoted, UIOrderList)
// Nothing real is touched: the host runs under its own class name, settings key and broadcast message
// (so real programs never find it), and its link to Explorer is pointed at the fake one.
// Windows (or Wine): cl /std:c++20 /EHsc tests\tray_host_test.cpp user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib dwmapi.lib
#include "../src/explorer_taskbar.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>

#define LD_TRAY_CLASS L"LiquidDockTest_TrayWnd"
#define LD_TRAY_SETTINGS_KEY L"Software\\LiquidDockTest\\NotifyIconSettings"
#define LD_TRAY_SETTINGS_PARENT L"Software\\LiquidDockTest"
#define LD_TRAY_CREATED_MESSAGE L"LiquidDockTest.TaskbarCreated"
static bool g_testElevated = false;
#define LD_TRAY_ELEVATED g_testElevated
static HWND g_fakeExplorer = nullptr;
static HWND TestExplorerTaskbar() { return g_fakeExplorer; }
// Explorer's process, for the shared block of an app bar message (ForwardAppBar): a stand-in process of the test's own,
// so the block really is made for another process (the fake Explorer reads it from there)
static DWORD g_explorerPid = 0;
#define LD_TRAY_EXPLORER_PROCESS(wnd) g_explorerPid
#define ExplorerTaskbar TestExplorerTaskbar
#include "parts_tray_host.inc"
#undef ExplorerTaskbar

static std::mutex g_logMu;
static std::vector<std::wstring> g_logs;
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    char utf8[4096];   // printf only: wide and narrow output on one stream do not mix
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    std::printf("  log: %s\n", utf8);
    std::lock_guard<std::mutex> lk(g_logMu);
    g_logs.push_back(buf);
}
static bool Logged(const wchar_t* part) {
    std::lock_guard<std::mutex> lk(g_logMu);
    for (auto& l : g_logs) if (l.find(part) != std::wstring::npos) return true;
    return false;
}
static size_t LoggedCount(const wchar_t* part) {
    std::lock_guard<std::mutex> lk(g_logMu);
    size_t n = 0;
    for (auto& l : g_logs) if (l.find(part) != std::wstring::npos) ++n;
    return n;
}
static void ClearLog() { std::lock_guard<std::mutex> lk(g_logMu); g_logs.clear(); }
#define TEST_SUPPORT_QPC_NOW
#include "test_support.h"

static int g_failures = 0, g_checks = 0;   // g_checks: Check()s run (the last line's count)
static void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    ++g_checks;
    if (!ok) ++g_failures;
}

// ---------------------------------------------------------------- fake Explorer taskbar
struct Received { UINT msg; WPARAM wp; LPARAM lp; ULONG_PTR dwData = 0; std::vector<BYTE> bytes; HWND to = nullptr; };
static std::mutex g_recMu;
static std::vector<Received> g_explorerGot;
static std::map<ULONG_PTR, LRESULT> g_explorerReply;   // per WM_COPYDATA dwData

// an app bar message's block, as Explorer reads it (SHLockShared with the process the message names) and answers in it
static bool g_answerAppBar = false;
static std::atomic<bool> g_appBarRead{ false };
static trayhost::AppBarData32 g_appBarSeen{};
static LRESULT CALLBACK FakeExplorerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA) {
        auto* cds = (COPYDATASTRUCT*)l;
        Received r{ m, w, l, cds->dwData, std::vector<BYTE>((BYTE*)cds->lpData, (BYTE*)cds->lpData + cds->cbData), h };
        if (g_answerAppBar && cds->dwData == 0 && cds->cbData == sizeof(trayhost::AppBarMessage)) {
            trayhost::AppBarMessage msg{};
            memcpy(&msg, cds->lpData, sizeof(msg));
            if (auto* abd = (trayhost::AppBarData32*)SHLockShared((HANDLE)(ULONG_PTR)msg.hSharedMemory, msg.dwProcessId)) {
                g_appBarSeen = *abd;
                g_appBarRead = true;
                abd->rc.top -= 48;   // the answer: the strip moved up (another bar below it)
                abd->rc.bottom -= 48;
                SHUnlockShared(abd);
            }
        }
        std::lock_guard<std::mutex> lk(g_recMu);
        g_explorerGot.push_back(r);
        return g_explorerReply.count(cds->dwData) ? g_explorerReply[cds->dwData] : 0;
    }
    if (m == WM_COMMAND || (m >= WM_USER && m < 0xC000)) {
        std::lock_guard<std::mutex> lk(g_recMu);
        g_explorerGot.push_back({ m, w, l });
        return 4242;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- fake tray program
constexpr UINT kCallback = WM_APP + 77;
static std::vector<std::pair<WPARAM, LPARAM>> g_appGot;
static UINT g_taskbarCreated = RegisterWindowMessageW(LD_TRAY_CREATED_MESSAGE);
static std::atomic<int> g_taskbarCreatedSeen{ 0 };
static std::atomic<int> g_messageOnlySeen{ 0 };
static LRESULT CALLBACK MessageOnlyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_taskbarCreated) { ++g_messageOnlySeen; return 0; }
    return DefWindowProcW(h, m, w, l);
}
static LRESULT CALLBACK FakeAppProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kCallback) { std::lock_guard<std::mutex> lk(g_recMu); g_appGot.push_back({ w, l }); return 0; }
    if (m == g_taskbarCreated) { ++g_taskbarCreatedSeen; return 0; }
    return DefWindowProcW(h, m, w, l);
}

// windows of the test's own thread: a message sent to them is handled before Send() returns
static std::atomic<int> g_ownGot{ 0 }, g_ownSystemGot{ 0 };
static LRESULT CALLBACK OwnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kCallback) { ++g_ownGot; return 0; }
    if (m == WM_COMMAND) { ++g_ownSystemGot; return 0; }
    return DefWindowProcW(h, m, w, l);
}

#include "tray_host_harness.h"

static size_t ExplorerCount() { std::lock_guard<std::mutex> lk(g_recMu); return g_explorerGot.size(); }
static Received LastExplorer() { std::lock_guard<std::mutex> lk(g_recMu); return g_explorerGot.empty() ? Received{} : g_explorerGot.back(); }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    RegDeleteTreeW(HKEY_CURRENT_USER, LD_TRAY_SETTINGS_PARENT);   // start like Windows 10: no settings
    HKEY parent = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, LD_TRAY_SETTINGS_PARENT, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &parent, nullptr);
    RegCloseKey(parent);

    // the stand-in for Explorer's process (suspended, ended at the end): app bar blocks are made for it
    STARTUPINFOW standInSi{ sizeof(standInSi) };
    PROCESS_INFORMATION standIn{};
    wchar_t standInCmd[] = L"cmd.exe /c exit";
    if (CreateProcessW(nullptr, standInCmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &standInSi, &standIn))
        g_explorerPid = standIn.dwProcessId;
    Check(g_explorerPid != 0, "a stand-in for Explorer's process");

    WindowThread explorer, app, app2, messageOnly;
    explorer.Run(L"FakeExplorerTray", FakeExplorerProc);
    app.Run(L"TestTrayApp", FakeAppProc);
    messageOnly.Run(L"TestMessageOnly", MessageOnlyProc, true);
    g_fakeExplorer = explorer.hwnd;
    Check(explorer.hwnd && app.hwnd, "test windows");

    // ---- start
    Check(trayhost::Start() && trayhost::Running(), "host starts");
    HWND host = trayhost::g_wnd;
    Check(host && GetPropW(host, kTrayHostProp), "host window carries the tray-host mark");
    wchar_t cls[64] = {}; GetClassNameW(host, cls, 64);
    Check(!wcscmp(cls, LD_TRAY_CLASS) && !IsWindowVisible(host), "host is an invisible tray window");
    Check(WaitFor([&] { return FindWindowW(LD_TRAY_CLASS, nullptr) == host; }, 1000), "programs find the host first");
    Check(WaitFor([&] { return g_messageOnlySeen.load() > 0; }, 2000), "message-only windows are asked to re-add their icons too");
    // (that the dock's own lookup of Explorer's taskbar skips a window with the tray-host mark is checked
    // with faked window lookups: tests/tray_explorer_taskbar_test.cpp - a test window of the real class
    // here would, while it lived, be the tray of every program on this desktop)

    // ---- add: forwarded byte for byte, taken although "Explorer" refuses (it already has it)
    g_explorerReply[1] = FALSE;
    HICON colorful = MakeIcon(16, 16, [](int x, int) { return x < 8 ? 0xffff0000u : 0x800000ffu; });   // red | half-transparent blue
    auto add = Notify(NIM_ADD, app.hwnd, 1, NIF_MESSAGE | NIF_ICON | NIF_TIP);
    add.nid.hIcon = (DWORD)(ULONG_PTR)colorful;
    wcscpy_s(add.nid.szTip, L"Test icon");
    size_t before = ExplorerCount();
    LRESULT r = SendNotify(add, app.hwnd);
    Check(r == TRUE, "NIM_ADD succeeds for the program even when Explorer refuses a duplicate");
    Check(ExplorerCount() == before + 1, "NIM_ADD reached Explorer");
    {
        auto got = LastExplorer();
        Check(got.msg == WM_COPYDATA && got.dwData == 1 && got.wp == (WPARAM)app.hwnd &&
              got.bytes.size() == sizeof(add) && !memcmp(got.bytes.data(), &add, sizeof(add)), "NIM_ADD forwarded unchanged");
    }
    uint64_t k1 = KeyOf(app.hwnd, 1);
    auto shown = trayhost::Shown();
    Check(k1 && FindShown(shown, k1) && FindShown(shown, k1)->tip == L"Test icon", "icon shown with its tooltip (no settings key: all shown)");
    Check(!FindShown(shown, k1)->glyph, "a coloured icon is not a glyph");
    {
        uint32_t ver = 0; int w = 0, h = 0; std::vector<uint32_t> px;
        Check(trayhost::Image(k1, ver, w, h, px) && w == 16 && h == 16, "picture copied at its size");
        Check(px[0] == 0xffff0000u && px[15] == 0x80000080u, "picture premultiplied (opaque red, half blue)");
    }

    // ---- versions and tooltips
    g_explorerReply[1] = TRUE;
    auto ver4 = Notify(NIM_SETVERSION, app.hwnd, 1, 0);
    ver4.nid.uVersion = 4;
    Check(SendNotify(ver4, app.hwnd) == TRUE, "NIM_SETVERSION answered");
    Check(FindShown(trayhost::Shown(), k1)->tip.empty(), "version 4 without NIF_SHOWTIP: no standard tooltip");
    auto tip = Notify(NIM_MODIFY, app.hwnd, 1, NIF_TIP | NIF_SHOWTIP);
    wcscpy_s(tip.nid.szTip, L"Second tip");
    Check(SendNotify(tip, app.hwnd) == TRUE && FindShown(trayhost::Shown(), k1)->tip == L"Second tip", "NIF_SHOWTIP brings the tooltip back");

    // ---- clicks, version 4: wParam = pointer, lParam = event | id << 16
    RECT anchor{ 10, 20, 30, 40 };
    trayhost::Send(k1, trayhost::Mouse::LeftClick, POINT{ 100, 200 }, anchor);
    auto msgs = AppMessages(3, 0);
    Check(msgs.size() == 3 && msgs[0].first == (WPARAM)MAKELONG(100, 200) &&
          msgs[0].second == (LPARAM)MAKELONG(WM_LBUTTONDOWN, 1) && msgs[1].second == (LPARAM)MAKELONG(WM_LBUTTONUP, 1) &&
          msgs[2].second == (LPARAM)MAKELONG(NIN_SELECT, 1), "v4 left click: down, up, NIN_SELECT");
    trayhost::Send(k1, trayhost::Mouse::RightClick, POINT{ 100, 200 }, anchor);
    msgs = AppMessages(3, 0);
    Check(msgs.size() == 3 && msgs[0].second == (LPARAM)MAKELONG(WM_RBUTTONDOWN, 1) && msgs[1].second == (LPARAM)MAKELONG(WM_RBUTTONUP, 1) &&
          msgs[2].second == (LPARAM)MAKELONG(WM_CONTEXTMENU, 1), "v4 right click: down, up, WM_CONTEXTMENU");
    trayhost::Send(k1, trayhost::Mouse::LeftDoubleClick, POINT{ 100, 200 }, anchor);
    msgs = AppMessages(2, 0);
    Check(msgs.size() == 2 && msgs[0].second == (LPARAM)MAKELONG(WM_LBUTTONDBLCLK, 1) && msgs[1].second == (LPARAM)MAKELONG(WM_LBUTTONUP, 1), "v4 double click");
    trayhost::Send(k1, trayhost::Mouse::Enter, POINT{ 100, 200 }, anchor);
    msgs = AppMessages(2, 0);
    Check(msgs.size() == 2 && msgs[1].second == (LPARAM)MAKELONG(NIN_POPUPOPEN, 1), "v4 hover opens the program's pop-up");
    trayhost::Send(k1, trayhost::Mouse::Leave, POINT{ 100, 200 }, anchor);
    msgs = AppMessages(2, 0);
    Check(msgs.size() == 2 && msgs[1].second == (LPARAM)MAKELONG(NIN_POPUPCLOSE, 1), "v4 leave closes it");

    // ---- Shell_NotifyIconGetRect: the dock's anchor of the icon
    trayhost::IconIdentifier id{};
    id.message = 1; id.cbSize = sizeof(id); id.hWnd = (DWORD)(ULONG_PTR)app.hwnd; id.uID = 1;
    Check(SendTray(3, &id, sizeof(id), app.hwnd) == (LRESULT)MAKELONG(10, 20), "icon rect: top left from the dock");
    id.message = 2;
    Check(SendTray(3, &id, sizeof(id), app.hwnd) == (LRESULT)MAKELONG(30, 40), "icon rect: bottom right from the dock");
    g_explorerReply[3] = 0x777;
    id.uID = 99;   // unknown icon: Explorer's answer
    Check(SendTray(3, &id, sizeof(id), app.hwnd) == 0x777, "icon rect of an unknown icon comes from Explorer");

    // ---- version 0 icon: wParam = id, lParam = event, no NIN_SELECT
    HICON glyph = MakeIcon(16, 16, [](int x, int y) { return (x > 3 && x < 12 && y > 3 && y < 12) ? 0xffffffffu : 0x00000000u; });
    auto add2 = Notify(NIM_ADD, app.hwnd, 2, NIF_MESSAGE | NIF_ICON | NIF_TIP);
    add2.nid.hIcon = (DWORD)(ULONG_PTR)glyph;
    wcscpy_s(add2.nid.szTip, L"Glyph");
    Check(SendNotify(add2, app.hwnd) == TRUE, "second icon added");
    uint64_t k2 = KeyOf(app.hwnd, 2);
    Check(FindShown(trayhost::Shown(), k2) && FindShown(trayhost::Shown(), k2)->glyph, "a white shape is a glyph (tinted by the dock)");
    trayhost::Send(k2, trayhost::Mouse::LeftClick, POINT{ 5, 6 }, anchor);
    Sleep(150);   // anything extra (a NIN_SELECT) would have arrived by now
    msgs = AppMessages(2, 0);
    Check(msgs.size() == 2 && msgs[0].first == 2 && msgs[0].second == WM_LBUTTONDOWN && msgs[1].second == WM_LBUTTONUP, "v0 left click: id in wParam, no NIN_SELECT");

    // ---- an icon known by its GUID, added again under a new id (Electron numbers every tray icon it
    //      makes and ignores clicks that carry another number)
    {
        static const GUID kIconGuid = { 0x1b2c3d4e, 0x5f60, 0x4718, { 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09 } };
        auto g1 = Notify(NIM_ADD, app.hwnd, 3, NIF_MESSAGE | NIF_ICON | NIF_GUID);
        g1.nid.guidItem = kIconGuid;
        g1.nid.hIcon = (DWORD)(ULONG_PTR)colorful;
        Check(SendNotify(g1, app.hwnd) == TRUE, "an icon with a GUID (id 3)");
        uint64_t kg = KeyOf(app.hwnd, 3);
        auto g2 = Notify(NIM_ADD, app.hwnd, 4, NIF_MESSAGE | NIF_GUID);
        g2.nid.guidItem = kIconGuid;
        SendNotify(g2, app.hwnd);
        Check(kg && KeyOf(app.hwnd, 4) == kg && !KeyOf(app.hwnd, 3), "added again under its GUID with id 4: the same icon, now id 4");
        trayhost::Send(kg, trayhost::Mouse::LeftClick, POINT{ 5, 6 }, anchor);
        msgs = AppMessages(2, 0);
        Check(msgs.size() == 2 && msgs[0].first == 4 && msgs[0].second == WM_LBUTTONDOWN, "its clicks carry the new id");
        auto g3 = Notify(NIM_MODIFY, app.hwnd, 0, NIF_TIP | NIF_GUID);   // a modify naming the icon by GUID alone
        g3.nid.guidItem = kIconGuid;
        wcscpy_s(g3.nid.szTip, L"guid tip");
        SendNotify(g3, app.hwnd);
        Check(KeyOf(app.hwnd, 4) == kg, "a modify by GUID keeps the id");
        auto gd = Notify(NIM_DELETE, app.hwnd, 0, NIF_GUID);
        gd.nid.guidItem = kIconGuid;
        SendNotify(gd, app.hwnd);
        Check(!KeyOf(app.hwnd, 4), "deleted by GUID");
    }

    // ---- hidden state
    auto hide = Notify(NIM_MODIFY, app.hwnd, 2, NIF_STATE);
    hide.nid.dwState = NIS_HIDDEN; hide.nid.dwStateMask = NIS_HIDDEN;
    SendNotify(hide, app.hwnd);
    Check(!FindShown(trayhost::Shown(), k2), "NIS_HIDDEN hides it");
    hide.nid.dwState = 0;
    SendNotify(hide, app.hwnd);
    Check(FindShown(trayhost::Shown(), k2) != nullptr, "and it comes back");

    // ---- whatever else programs send the taskbar goes to Explorer
    BYTE appbar[64] = {}; for (int i = 0; i < 64; ++i) appbar[i] = (BYTE)i;
    g_explorerReply[0] = 0x55;
    Check(SendTray(0, appbar, sizeof(appbar), app.hwnd) == 0x55 && LastExplorer().dwData == 0 &&
          LastExplorer().bytes.size() == 64 && !memcmp(LastExplorer().bytes.data(), appbar, 64), "app-bar message forwarded with Explorer's answer");
    DWORD_PTR cr = 0;
    SendMessageTimeoutW(host, WM_COMMAND, 419, 0, SMTO_NORMAL, 3000, &cr);
    Check(cr == 4242 && LastExplorer().msg == WM_COMMAND && LastExplorer().wp == 419, "WM_COMMAND forwarded");
    SendMessageTimeoutW(host, WM_USER + 13, 7, 8, SMTO_NORMAL, 3000, &cr);
    Check(cr == 4242 && LastExplorer().msg == WM_USER + 13 && LastExplorer().wp == 7, "private WM_USER message forwarded");
    {   // The dock's own app bar: SHAppBarMessage found the host first, in the dock's process, and made the block for that
        // process - which is no handle but a block in its own memory (as SHAllocShared does for the calling process).
        // Explorer cannot read that: the host makes one for Explorer's process, passes that on, and copies the answer back.
        trayhost::AppBarData32 abd{ sizeof(abd), (DWORD)(ULONG_PTR)app.hwnd, WM_APP + 40, ABE_BOTTOM, { -1440, 1936, 0, 2000 }, 0 };
        const DWORD self = GetCurrentProcessId();
        HANDLE block = SHAllocShared(&abd, sizeof(abd), self);
        trayhost::AppBarMessage msg{ abd, ABM_SETPOS, 0, (UINT64)(ULONG_PTR)block, self, 0 };
        g_answerAppBar = true; g_appBarRead = false; g_explorerReply[0] = 1;
        LRESULT r = 0;
        {
            trayhost::OwnAppBar own(app.hwnd);   // as the dock names its window around its own QUERYPOS / SETPOS
            r = SendTray(0, &msg, sizeof(msg), app.hwnd);
        }
        trayhost::AppBarMessage got{};
        const Received last = LastExplorer();
        if (last.bytes.size() == sizeof(got)) memcpy(&got, last.bytes.data(), sizeof(got));
        Check(block && r == 1 && last.dwData == 0 && last.wp == (WPARAM)app.hwnd && got.dwMessage == ABM_SETPOS &&
              got.dwProcessId == g_explorerPid && got.hSharedMemory && got.hSharedMemory != msg.hSharedMemory &&
              !memcmp(&got.abd, &abd, sizeof(abd)),
              "the dock's own app bar block: passed on as one made for Explorer's process, the rest as it came");
        Check(g_appBarRead && EqualRect(&g_appBarSeen.rc, &abd.rc) && g_appBarSeen.uEdge == ABE_BOTTOM,
              "...Explorer reads the strip from it");
        auto* answer = (trayhost::AppBarData32*)SHLockShared(block, self);
        Check(answer && answer->rc.top == 1936 - 48 && answer->rc.bottom == 2000 - 48, "...and its answer is back in the sender's block");
        if (answer) SHUnlockShared(answer);
        SHFreeShared(block, self);

        // another program's message with a block (the host's process named, as SHAppBarMessage does): passed on as it
        // came - the host opens no other program's block (an elevated dock would do it with rights the program lacks)
        HANDLE other = SHAllocShared(&abd, sizeof(abd), g_explorerPid);   // a real handle, as a program's would be
        trayhost::AppBarMessage foreign{ abd, ABM_SETPOS, 0, (UINT64)(ULONG_PTR)other, self, 0 };
        g_appBarRead = false;
        SendTray(0, &foreign, sizeof(foreign), app.hwnd);
        Check(other && LastExplorer().bytes.size() == sizeof(foreign) && !memcmp(LastExplorer().bytes.data(), &foreign, sizeof(foreign)),
              "another program's app bar block is passed on as it came, unopened");
        {   // ...also while the dock names its window, when the message is for another window
            trayhost::OwnAppBar own(messageOnly.hwnd);
            SendTray(0, &foreign, sizeof(foreign), app.hwnd);
        }
        Check(LastExplorer().bytes.size() == sizeof(foreign) && !memcmp(LastExplorer().bytes.data(), &foreign, sizeof(foreign)),
              "...also while the dock sends its own, for another window");
        if (other) SHFreeShared(other, g_explorerPid);

        // a message with no block (ABM_NEW, ABM_REMOVE, ABM_GETSTATE) is passed on as it came
        trayhost::AppBarMessage plain{ abd, ABM_NEW, 0, 0, self, 0 };
        g_appBarRead = false;
        SendTray(0, &plain, sizeof(plain), app.hwnd);
        Check(!g_appBarRead && LastExplorer().bytes.size() == sizeof(plain) && !memcmp(LastExplorer().bytes.data(), &plain, sizeof(plain)),
              "an app bar message with no block is passed on as it came");
        g_answerAppBar = false; g_explorerReply.erase(0);
    }
    BYTE tiny[8] = {};
    before = ExplorerCount();
    SendTray(1, tiny, sizeof(tiny), app.hwnd);
    Check(ExplorerCount() == before + 1, "a malformed tray message is only forwarded");

    // ---- NotifyIconSettings: Windows' choice of shown icons and their order
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HKEY root = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, LD_TRAY_SETTINGS_KEY, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr);
    SetEntry(root, L"1001", exe, 1, 0);
    Check(WaitFor([&] { return !FindShown(trayhost::Shown(), k1) && !FindShown(trayhost::Shown(), k2); }, 4000),
          "with the settings key: icons not promoted (or without entry) are not shown");
    SetEntry(root, L"1001", exe, 1, 1);
    SetEntry(root, L"1002", exe, 2, 1);
    uint64_t order[2] = { 1002, 1001 };
    RegSetValueExW(root, L"UIOrderList", 0, REG_BINARY, (const BYTE*)order, sizeof(order));
    Check(WaitFor([&] { auto s = trayhost::Shown(); return s.size() == 2 && s[0].key == k2 && s[1].key == k1; }, 4000),
          "IsPromoted shows them, in UIOrderList order");
    Check(trayhost::ResolveExecutablePath(L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\cmd.exe").size() > 8 &&
          trayhost::Lower(trayhost::ResolveExecutablePath(L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\cmd.exe")).find(L"system32\\cmd.exe") != std::wstring::npos,
          "known-folder paths resolve");
    SetEntry(root, L"1001", exe, 1, 0);   // the user moves one into the hidden-icons flyout
    Check(WaitFor([&] { auto s = trayhost::Shown(); return s.size() == 1 && s[0].key == k2; }, 4000), "turning IsPromoted off hides it");

    // ---- Windows keeps stale duplicates: the entry in its order list decides
    SetEntry(root, L"2001", exe, 9, 0);
    SetEntry(root, L"2002", exe, 9, 1);
    uint64_t order3[3] = { 1002, 1001, 2002 };
    RegSetValueExW(root, L"UIOrderList", 0, REG_BINARY, (const BYTE*)order3, sizeof(order3));
    auto add9 = Notify(NIM_ADD, app.hwnd, 9, NIF_MESSAGE | NIF_ICON);
    add9.nid.hIcon = (DWORD)(ULONG_PTR)colorful;
    SendNotify(add9, app.hwnd);
    uint64_t k9 = KeyOf(app.hwnd, 9);
    Check(WaitFor([&] { return FindShown(trayhost::Shown(), k9) != nullptr; }, 4000), "of duplicate entries the live one (in UIOrderList) counts");
    SendNotify(Notify(NIM_DELETE, app.hwnd, 9, 0), app.hwnd);

    // ---- a program handing over a big icon: kept at 64 px at most
    HICON big = MakeIcon(128, 128, [](int x, int) { return x < 64 ? 0xff00ff00u : 0xff0000ffu; });
    auto add10 = Notify(NIM_ADD, app.hwnd, 10, NIF_MESSAGE | NIF_ICON);
    add10.nid.hIcon = (DWORD)(ULONG_PTR)big;
    SendNotify(add10, app.hwnd);
    {
        uint32_t ver = 0; int w = 0, h = 0; std::vector<uint32_t> px;
        uint64_t k10 = KeyOf(app.hwnd, 10);
        Check(trayhost::Image(k10, ver, w, h, px) && w == 64 && h == 64 && px[0] == 0xff00ff00u && px[63] == 0xff0000ffu,
              "a 128 px icon is kept at 64 px");
    }
    SendNotify(Notify(NIM_DELETE, app.hwnd, 10, 0), app.hwnd);
    DestroyIcon(big);

    // ---- removal and dead programs
    auto del = Notify(NIM_DELETE, app.hwnd, 2, 0);
    before = ExplorerCount();
    Check(SendNotify(del, app.hwnd) == TRUE && !FindShown(trayhost::Shown(), k2) && ExplorerCount() == before + 1, "NIM_DELETE removes and forwards");
    app2.Run(L"TestTrayApp2", FakeAppProc);
    auto add3 = Notify(NIM_ADD, app2.hwnd, 5, NIF_MESSAGE | NIF_ICON);
    add3.nid.hIcon = (DWORD)(ULONG_PTR)colorful;
    SendNotify(add3, app2.hwnd);
    uint64_t k3 = KeyOf(app2.hwnd, 5);
    Check(k3 && !FindShown(trayhost::Shown(), k3), "a new icon without a settings entry is not shown yet");
    SetEntry(root, L"1003", exe, 5, 1);   // as Explorer writes the entry of a new icon, promoted
    Check(WaitFor([&] { return FindShown(trayhost::Shown(), k3) != nullptr; }, 4000), "once Windows records it as shown, the dock shows it");
    int v0 = trayhost::Version();
    app2.Quit();
    Check(WaitFor([&] { return !KeyOf(app2.hwnd, 5) && trayhost::Version() != v0; }, 4000), "a dead program's icon goes away");
    auto modUnknown = Notify(NIM_MODIFY, app.hwnd, 77, NIF_TIP);
    g_explorerReply[1] = 0;
    Check(SendNotify(modUnknown, app.hwnd) == 0, "modify of an unknown icon: Explorer's answer");
    RegCloseKey(root);

    // ---- Explorer restarts: its new taskbar may have come up above the host and taken the re-adds
    WindowThread explorer2;
    explorer2.Run(L"FakeExplorerTray2", FakeExplorerProc);
    int seen = g_taskbarCreatedSeen.load();
    g_fakeExplorer = explorer2.hwnd;   // the old taskbar window goes away, a new one appears
    explorer.Quit();
    Check(WaitFor([&] { return g_taskbarCreatedSeen.load() > seen; }, 4000), "after an Explorer restart programs are asked to re-add their icons");
    {
        std::lock_guard<std::mutex> lk(g_recMu);
        g_explorerGot.clear();
    }
    auto mod = Notify(NIM_MODIFY, app.hwnd, 1, NIF_TIP);
    wcscpy_s(mod.nid.szTip, L"After restart");
    g_explorerReply[1] = TRUE;
    Check(SendNotify(mod, app.hwnd) == TRUE && ExplorerCount() == 1 && LastExplorer().to == explorer2.hwnd, "messages go to the new Explorer");

    // ---- stop
    trayhost::Stop();
    Check(!trayhost::Running() && trayhost::Shown().empty() && !IsWindow(host), "host stops and its window is gone");
    Check(FindWindowW(LD_TRAY_CLASS, nullptr) != host, "programs no longer find it");
    {   // a running program that Windows shows but whose icon never came is named in the log, with what to do
        ClearLog();
        trayhost::LogMissing({});
        Check(Logged(L"tray_host_test.exe is running and shown on the taskbar") && Logged(L"restart that program"),
              "dock.log names a shown program whose icon has not come");
        ClearLog();
        trayhost::LogMissing({ trayhost::Lower(exe) });
        Check(!Logged(L"is running"), "and stays quiet once it came");
    }
    Check(trayhost::Start() && trayhost::Running() && !trayhost::g_elevated, "restarts");
    trayhost::Stop();

    // ---- the dock runs as administrator: clicks go only where a tray icon may point
    g_testElevated = true;
    Check(trayhost::Start() && trayhost::g_elevated, "starts as administrator");
    {
        WNDCLASSEXW oc = { sizeof(oc) }; oc.lpfnWndProc = OwnProc; oc.hInstance = GetModuleHandleW(nullptr); oc.lpszClassName = L"TestOwnWnd";
        RegisterClassExW(&oc);
        HWND top = CreateWindowExW(0, L"TestOwnWnd", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, oc.hInstance, nullptr);
        HWND child = CreateWindowExW(0, L"TestOwnWnd", L"", WS_CHILD, 0, 0, 5, 5, top, nullptr, oc.hInstance, nullptr);
        auto addTop = Notify(NIM_ADD, top, 31, NIF_MESSAGE | NIF_ICON);
        auto addChild = Notify(NIM_ADD, child, 32, NIF_MESSAGE | NIF_ICON);
        auto addSystem = Notify(NIM_ADD, top, 33, NIF_MESSAGE | NIF_ICON);
        addSystem.nid.uCallbackMessage = WM_COMMAND;
        for (auto* a : { &addTop, &addChild, &addSystem }) { a->nid.hIcon = (DWORD)(ULONG_PTR)colorful; SendNotify(*a, top); }
        uint64_t kTop = KeyOf(top, 31), kChild = KeyOf(child, 32), kSystem = KeyOf(top, 33);
        Check(kTop && kChild && kSystem, "icons arrive as administrator");
        trayhost::Send(kTop, trayhost::Mouse::LeftClick, POINT{ 1, 1 }, RECT{});
        Check(g_ownGot.load() >= 2, "a click reaches the program's window");
        int got = g_ownGot.load();
        trayhost::Send(kChild, trayhost::Mouse::LeftClick, POINT{ 1, 1 }, RECT{});
        Check(g_ownGot.load() == got, "not a child window (an icon may name any window)");
        trayhost::Send(kSystem, trayhost::Mouse::RightClick, POINT{ 1, 1 }, RECT{});
        Check(g_ownSystemGot.load() == 0, "not a system message as the callback");
        DestroyWindow(child); DestroyWindow(top);
    }
    trayhost::Stop();
    g_testElevated = false;

    // ---- hosting: another program's widget (as FluentFlyout puts its own on the taskbar)
    int asked = g_taskbarCreatedSeen.load();
    Check(trayhost::Start(false) && trayhost::Running(), "starts without asking programs for their icons");
    Sleep(300);
    Check(g_taskbarCreatedSeen.load() == asked, "...and programs were not asked");
    HWND hostWnd = trayhost::Window();
    WindowThread widget;
    widget.Run(L"TestWidget", DefWindowProcW);
    // like FluentFlyout: a child window of whatever FindWindow("Shell_TrayWnd") returns
    SetWindowLongPtrW(widget.hwnd, GWL_STYLE, (GetWindowLongPtrW(widget.hwnd, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
    Check(FindWindowW(LD_TRAY_CLASS, nullptr) == hostWnd && SetParent(widget.hwnd, hostWnd) != nullptr, "the widget becomes a child of the host");
    trayhost::SetHosting(true);
    RECT place = { 100, 900, 100 + 700, 900 + 70 };
    trayhost::Place(true, place, true);
    Check(WaitFor([&] { RECT r; GetWindowRect(hostWnd, &r); return EqualRect(&r, &place) && IsWindowVisible(hostWnd); }, 2000),
          "the dock places the host window and shows it");
    {
        RECT nr{}, hr{};
        GetWindowRect(trayhost::g_notifyWnd, &nr); GetWindowRect(hostWnd, &hr);
        Check(nr.left == hr.right - 1, "its tray area sits at its right end (widgets set to the right align to it)");
    }
    {   // FluentFlyout's widget is a layered window (WPF, UpdateLayeredWindow): as a child it shows only in
        // a window with a surface of its own, see-through here, painted under its children too
        LONG_PTR ex = GetWindowLongPtrW(hostWnd, GWL_EXSTYLE), st = GetWindowLongPtrW(hostWnd, GWL_STYLE);
        Check(!(ex & 0x00200000L /* WS_EX_NOREDIRECTIONBITMAP */) && !(st & WS_CLIPCHILDREN),
              "the host window keeps a surface, painted under its children too (a layered widget shows only then)");
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = 4; bi.bmiHeader.biHeight = -4;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(dc, bmp);
        for (int i = 0; i < 16; ++i) ((uint32_t*)bits)[i] = 0xFFFFFFFFu;
        LRESULT erased = SendMessageW(hostWnd, WM_ERASEBKGND, (WPARAM)dc, 0);
        GdiFlush();
        Check(erased && ((uint32_t*)bits)[5] == 0, "its own pixels: black, alpha 0 (see-through)");
        SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc);
        Check((ex & WS_EX_NOACTIVATE) && SendMessageW(hostWnd, WM_MOUSEACTIVATE, (WPARAM)hostWnd, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) == MA_NOACTIVATE,
              "a click on the widget leaves the focus where it was");
    }
    // the widget lays itself out for the host's size and cuts itself to what it shows
    SetWindowPos(widget.hwnd, HWND_TOP, 0, 0, 700, 70, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowRgn(widget.hwnd, CreateRectRgn(20, 15, 220, 55), TRUE);
    Check(WaitFor([&] {
              auto h = trayhost::HostedState();
              return h.window == widget.hwnd && h.fitted && h.box.left == 20 && h.box.top == 15 && h.box.right == 220 && h.box.bottom == 55;
          }, 2000), "the widget's region is reported, laid out for the host");
    {
        HRGN hr = CreateRectRgn(0, 0, 0, 0);
        int type = GetWindowRgn(hostWnd, hr);
        RECT b{}; GetRgnBox(hr, &b);
        Check(type != ERROR && b.left == 20 && b.top == 15 && b.right == 220 && b.bottom == 55, "the host window is cut to the widget");
        DeleteObject(hr);
    }
    {   // the dock comes to the front on its own (shown again, a shell pop-up): the widget goes back on top
        WNDCLASSEXW dc = { sizeof(dc) };
        dc.lpfnWndProc = DefWindowProcW; dc.hInstance = GetModuleHandleW(nullptr); dc.lpszClassName = L"TestDockWnd";
        RegisterClassExW(&dc);
        HWND dock = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"TestDockWnd", L"", WS_POPUP,
                                    50, 850, 900, 150, nullptr, nullptr, dc.hInstance, nullptr);
        ShowWindow(dock, SW_SHOWNOACTIVATE);
        auto above = [](HWND a, HWND b) {   // a above b
            for (HWND w = GetWindow(b, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
                if (w == a) return true;
            return false;
        };
        trayhost::Place(true, place, true, RECT{}, dock);
        Check(WaitFor([&] { return above(hostWnd, dock); }, 2000), "the widget sits above the dock");
        SetWindowPos(dock, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Check(WaitFor([&] { return above(hostWnd, dock) && Logged(L"the dock had come over the widget"); }, 2000),
              "the dock coming up over it on its own: the widget goes back on top (and the log says so)");
        Check(Logged(L"tray host: widget seen") && Logged(L"cut to 20,15-220,55"), "the log describes the widget once seen");
        trayhost::Place(true, place, false, RECT{}, nullptr);
        DestroyWindow(dock);
    }
    auto regionEmpty = [&] {
        HRGN hr = CreateRectRgn(0, 0, 0, 0);
        int type = GetWindowRgn(hostWnd, hr);
        DeleteObject(hr);
        return type == NULLREGION;
    };
    trayhost::Place(false, place);
    Check(WaitFor([&] { return regionEmpty() && IsWindowVisible(hostWnd); }, 2000),
          "the dock keeps the widget away: cut to nothing, the window itself stays shown (for the widget's program)");
    ShowWindow(hostWnd, SW_HIDE);   // a "hide the taskbar" tool
    Check(WaitFor([&] { return IsWindowVisible(hostWnd) && regionEmpty(); }, 2000), "a tool hiding it is undone");
    trayhost::Place(true, place);
    Check(WaitFor([&] { return !regionEmpty(); }, 2000), "and the widget is seen again when the dock wants it");
    ShowWindow(widget.hwnd, SW_HIDE);   // the widget hides itself (FluentFlyout: paused, with auto-hide)
    Check(WaitFor([&] { auto h = trayhost::HostedState(); return !h.window && IsRectEmpty(&h.box); }, 2000), "a hidden widget shows nothing");
    ShowWindow(widget.hwnd, SW_SHOWNA);
    trayhost::SetHosting(false);
    Check(WaitFor([&] {
              RECT r, e; GetWindowRect(hostWnd, &r); GetWindowRect(explorer2.hwnd, &e);
              return !IsWindowVisible(hostWnd) && EqualRect(&r, &e);
          }, 2000), "hosting off: hidden again, mirroring Explorer's taskbar");
    ShowWindow(hostWnd, SW_SHOWNA);   // a "show the taskbar" tool
    Sleep(250);
    Check(!IsWindowVisible(hostWnd), "not hosting, nobody else shows it");
    trayhost::Start(true);   // tray icons turned on while the host runs
    Check(WaitFor([&] { return g_taskbarCreatedSeen.load() > asked; }, 2000), "programs are asked for their icons then");
    trayhost::SetHosting(true);
    trayhost::Stop();
    Check(IsWindow(widget.hwnd) && GetParent(widget.hwnd) == explorer2.hwnd, "on stop the widget goes back to Explorer's taskbar, not destroyed");
    {   // the host starts again and hosts the same widget window: the log described it once, a stopped host does
        // not make it describe it again (only a new widget window is described)
        const size_t seenBefore = LoggedCount(L"tray host: widget seen");
        Check(seenBefore >= 1 && trayhost::Start(false) && trayhost::Running(), "the host starts again, still hosting");
        HWND again = trayhost::Window();
        Check(SetParent(widget.hwnd, again) != nullptr, "the same widget window becomes its child again");
        trayhost::Place(true, place, true);
        Check(WaitFor([&] {
                  RECT r; GetWindowRect(again, &r);
                  HRGN hr = CreateRectRgn(0, 0, 0, 0);
                  const int type = GetWindowRgn(again, hr);   // the new window's own region, not the old one's
                  DeleteObject(hr);
                  return trayhost::HostedState().window == widget.hwnd && EqualRect(&r, &place) && type != ERROR && type != NULLREGION;
              }, 2000), "it is hosted and seen again");
        Sleep(300);   // the host thread's checks (every 100 ms) would have described it by now
        Check(LoggedCount(L"tray host: widget seen") == seenBefore, "the same widget window is not described in the log again");
        trayhost::Stop();
        Check(IsWindow(widget.hwnd) && GetParent(widget.hwnd) == explorer2.hwnd, "and on stop it goes back to Explorer's taskbar again");
    }
    trayhost::SetHosting(false);
    widget.Quit();

    RegDeleteTreeW(HKEY_CURRENT_USER, LD_TRAY_SETTINGS_PARENT);
    DestroyIcon(colorful); DestroyIcon(glyph);
    app.Quit(); explorer2.Quit(); messageOnly.Quit();
    if (standIn.hProcess) { TerminateProcess(standIn.hProcess, 0); CloseHandle(standIn.hProcess); CloseHandle(standIn.hThread); }
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    std::printf("tray_host_test: %s (%d checks, %d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_checks, g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
