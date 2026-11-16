// Edge cases of the production tray host (src/tray_host*.cpp) that tests/tray_host_test.cpp does not reach,
// with the same fakes: a fake Explorer taskbar and fake tray programs inside this process.
//   - messages shorter than the full structure, a tooltip without its terminating NUL, unknown versions
//   - version 3 clicks, version 4 clicks and icon rectangles on monitors left of and above the primary
//   - pictures without an alpha channel (the mask decides) and black-and-white (monochrome) icons
//   - a modify for an icon Explorer does not have, the elevated host's click rules for message-only
//     windows and controls, a host that stopped late or ended on its own, and what a low-integrity
//     process can pass on to Explorer
//   - dock.log lines written while the dock's Shown() goes on (a log write that stalls)
// Nothing real is touched: class names, the settings key and the broadcast carry "edge", and the
// host's link to Explorer points at the fake one. A low-integrity copy of this program is started twice
// (it only sends four messages to this program's own windows). About 20 s: a host that stops late needs
// a widget that hangs longer than Stop() waits (10-12.5 s).
// Windows: cl /std:c++20 /EHsc tests\tray_host_edge_test.cpp user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib dwmapi.lib
#include "../src/explorer_taskbar.h"
#include <sddl.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>

#define LD_TRAY_CLASS L"LiquidDockTest_TrayWnd_edge"
#define LD_TRAY_SETTINGS_KEY L"Software\\LiquidDockTest-edge\\NotifyIconSettings"
#define LD_TRAY_SETTINGS_PARENT L"Software\\LiquidDockTest-edge"
#define LD_TRAY_CREATED_MESSAGE L"LiquidDockTest-edge.TaskbarCreated"
static bool g_testElevated = false;
#define LD_TRAY_ELEVATED g_testElevated
static HWND g_fakeExplorer = nullptr;
static HWND TestExplorerTaskbar() { return g_fakeExplorer; }
#define ExplorerTaskbar TestExplorerTaskbar
#include "../src/tray_host.cpp"
#include "../src/tray_host_icons.cpp"
#include "../src/tray_host_settings.cpp"
#include "../src/tray_host_widget.cpp"
#undef ExplorerTaskbar

static std::mutex g_logMu;
static std::vector<std::wstring> g_logs;
// a line containing g_slowLine takes g_slowLogMs to write (a disk that stalls); g_slowLogIn: being written
static std::atomic<int> g_slowLogMs{ 0 };
static const wchar_t* g_slowLine = L"";
static HANDLE g_slowLogIn = CreateEventW(nullptr, TRUE, FALSE, nullptr);
void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_slowLogMs && wcsstr(buf, g_slowLine)) { SetEvent(g_slowLogIn); Sleep(g_slowLogMs); }
    char utf8[4096];
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), nullptr, nullptr);
    std::printf("  log: %s\n", utf8);
    std::lock_guard<std::mutex> lk(g_logMu);
    g_logs.push_back(buf);
}
static int LoggedCount(const wchar_t* part) {
    std::lock_guard<std::mutex> lk(g_logMu);
    int n = 0;
    for (auto& l : g_logs) if (l.find(part) != std::wstring::npos) ++n;
    return n;
}
static bool Logged(const wchar_t* part) { return LoggedCount(part) > 0; }
double NowSeconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(f.QuadPart);
}

static int g_failures = 0, g_checks = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// ---------------------------------------------------------------- fake Explorer taskbar
static std::mutex g_recMu;
static std::vector<std::pair<UINT, WPARAM>> g_explorerGot;   // message, wParam (WM_COPYDATA: dwData)
static std::map<ULONG_PTR, LRESULT> g_explorerReply;          // per WM_COPYDATA dwData

static LRESULT CALLBACK FakeExplorerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA) {
        auto* cds = (COPYDATASTRUCT*)l;
        std::lock_guard<std::mutex> lk(g_recMu);
        g_explorerGot.push_back({ m, cds->dwData });
        return g_explorerReply.count(cds->dwData) ? g_explorerReply[cds->dwData] : 0;
    }
    if (m == WM_COMMAND) {
        std::lock_guard<std::mutex> lk(g_recMu);
        g_explorerGot.push_back({ m, w });
        return 4242;
    }
    return DefWindowProcW(h, m, w, l);
}
static int ExplorerGot(UINT msg, WPARAM w) {   // WM_COMMAND: by its id; WM_COPYDATA: by its dwData
    std::lock_guard<std::mutex> lk(g_recMu);
    int n = 0;
    for (auto& [m, got] : g_explorerGot) if (m == msg && got == w) ++n;
    return n;
}

// ---------------------------------------------------------------- fake tray program
constexpr UINT kCallback = WM_APP + 77;
static std::vector<std::pair<WPARAM, LPARAM>> g_appGot;
static LRESULT CALLBACK FakeAppProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kCallback) { std::lock_guard<std::mutex> lk(g_recMu); g_appGot.push_back({ w, l }); return 0; }
    if (m == WM_APP + 99) { Sleep((DWORD)w); return 0; }   // the program hangs for wParam ms
    return DefWindowProcW(h, m, w, l);
}

// windows of the test's own thread: a message sent to them is handled before Send() returns
static std::atomic<int> g_ownGot{ 0 };
static LRESULT CALLBACK OwnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kCallback) { ++g_ownGot; return 0; }
    return DefWindowProcW(h, m, w, l);
}

struct WindowThread {
    HWND hwnd = nullptr;
    std::thread t;
    DWORD tid = 0;
    void Run(const wchar_t* cls, WNDPROC proc) {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        t = std::thread([this, cls, proc, ready] {
            tid = GetCurrentThreadId();
            WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls;
            RegisterClassExW(&wc);
            hwnd = CreateWindowExW(0, cls, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
            SetEvent(ready);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
        });
        WaitForSingleObject(ready, 5000);
        CloseHandle(ready);
    }
    void Quit() {
        if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
        if (t.joinable()) t.join();
        hwnd = nullptr;
    }
};

// colour bitmap: 32 bpp (top-down) or 24 bpp; mask: 1 bit per pixel, a set bit = transparent
static HICON MakeIcon(int w, int h, int bpp, const std::function<uint32_t(int, int)>& px, const std::function<bool(int, int)>& masked) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = (WORD)bpp; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    const int stride = ((w * bpp + 31) / 32) * 4;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t v = px(x, y);
            BYTE* row = (BYTE*)bits + (size_t)y * stride;
            if (bpp == 32) ((uint32_t*)row)[x] = v;
            else { row[x * 3] = (BYTE)v; row[x * 3 + 1] = (BYTE)(v >> 8); row[x * 3 + 2] = (BYTE)(v >> 16); }
        }
    const int mstride = ((w + 15) / 16) * 2;
    std::vector<BYTE> mbits((size_t)mstride * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (masked(x, y)) mbits[(size_t)y * mstride + x / 8] |= (BYTE)(0x80 >> (x % 8));
    HBITMAP mask = CreateBitmap(w, h, 1, 1, mbits.data());
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return icon;
}

static LRESULT SendTray(ULONG_PTR dwData, const void* data, DWORD size, HWND from) {
    COPYDATASTRUCT cds{ dwData, size, (void*)data };
    DWORD_PTR r = 0;
    SendMessageTimeoutW(trayhost::g_wnd, WM_COPYDATA, (WPARAM)from, (LPARAM)&cds, SMTO_NORMAL, 5000, &r);
    return (LRESULT)r;
}
static trayhost::TrayNotifyData Notify(DWORD message, HWND app, UINT uid, UINT flags) {
    trayhost::TrayNotifyData d{};
    d.signature = 0x34753423; d.message = message;
    d.nid.cbSize = sizeof(d.nid);
    d.nid.hWnd = (DWORD)(ULONG_PTR)app; d.nid.uID = uid; d.nid.uFlags = flags;
    d.nid.uCallbackMessage = kCallback;
    return d;
}
static LRESULT SendNotify(const trayhost::TrayNotifyData& d, HWND from) { return SendTray(1, &d, sizeof(d), from); }

static std::vector<std::pair<WPARAM, LPARAM>> AppMessages(size_t want) {
    for (int i = 0; i < 200; ++i) {
        { std::lock_guard<std::mutex> lk(g_recMu); if (g_appGot.size() >= want) break; }
        Sleep(10);
    }
    Sleep(50);   // anything extra would have arrived by now
    std::lock_guard<std::mutex> lk(g_recMu);
    auto out = g_appGot;
    g_appGot.clear();
    return out;
}
static uint64_t KeyOf(HWND app, UINT uid) {
    std::lock_guard<std::mutex> lk(trayhost::g_mu);
    for (auto& ic : trayhost::g_icons) if (ic.hwnd == app && ic.uid == uid) return ic.key;
    return 0;
}
static const trayhost::IconView* FindShown(const std::vector<trayhost::IconView>& v, uint64_t key) {
    for (auto& i : v) if (i.key == key) return &i;
    return nullptr;
}
static bool WaitFor(const std::function<bool()>& cond, int ms) {
    for (int i = 0; i < ms / 20; ++i) { if (cond()) return true; Sleep(20); }
    return cond();
}
static void SetEntry(HKEY root, const wchar_t* name, const std::wstring& exe, DWORD uid, DWORD promoted) {
    HKEY k = nullptr;
    RegCreateKeyExW(root, name, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr);
    RegSetValueExW(k, L"ExecutablePath", 0, REG_SZ, (const BYTE*)exe.c_str(), DWORD((exe.size() + 1) * 2));
    RegSetValueExW(k, L"UID", 0, REG_DWORD, (const BYTE*)&uid, 4);
    RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD, (const BYTE*)&promoted, 4);
    RegCloseKey(k);
}
static bool Picture(uint64_t key, int& w, int& h, std::vector<uint32_t>& px) {
    uint32_t ver = 0;
    return trayhost::Image(key, ver, w, h, px);
}
// How long the dock's Shown() waits while the host writes a dock.log line containing `line` (0.4 s each),
// which `cause` (on a thread of its own) sets off; -1: no such line came.
static double ShownWhileLogging(const wchar_t* line, const std::function<void()>& cause) {
    g_slowLine = line;
    ResetEvent(g_slowLogIn);
    g_slowLogMs = 400;
    std::thread t(cause);
    double waited = -1;
    if (WaitForSingleObject(g_slowLogIn, 5000) == WAIT_OBJECT_0) {
        double t0 = NowSeconds();
        trayhost::Shown();
        waited = NowSeconds() - t0;
    }
    t.join();
    g_slowLogMs = 0;
    std::printf("  Shown() while a line with \"%ls\" is written: %.0f ms\n", line, waited * 1000);
    return waited;
}

// ---------------------------------------------------------------- a copy of this program at low integrity
// Sends WM_COMMAND and an app-bar WM_COPYDATA (dwData 0) to the fake Explorer directly and to the host.
// Exit code bits: 1 / 4 = the direct WM_COMMAND / WM_COPYDATA arrived, 2 / 8 = the host took it and
// answered with Explorer's answer (4242 / 5151).
constexpr LRESULT kAppBarReply = 5151;
static int LowIntegrityChild(HWND host, HWND explorer) {
    int rc = 0;
    DWORD_PTR r = 0;
    if (SendMessageTimeoutW(explorer, WM_COMMAND, 4199, 0, SMTO_NORMAL, 3000, &r)) rc |= 1;
    r = 0;
    if (SendMessageTimeoutW(host, WM_COMMAND, 4199, 0, SMTO_NORMAL, 3000, &r) && r == 4242) rc |= 2;
    BYTE appbar[48] = { 0x77 };
    COPYDATASTRUCT cds{ 0, sizeof(appbar), appbar };
    r = 0;
    if (SendMessageTimeoutW(explorer, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_NORMAL, 3000, &r)) rc |= 4;
    r = 0;
    if (SendMessageTimeoutW(host, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_NORMAL, 3000, &r) && r == kAppBarReply) rc |= 8;
    return rc;
}

// -1: the low-integrity copy could not be started
static int RunAtLowIntegrity(HWND host, HWND explorer) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --edge-low " + std::to_wstring((unsigned long long)(ULONG_PTR)host) + L" " +
                       std::to_wstring((unsigned long long)(ULONG_PTR)explorer);
    HANDLE token = nullptr, low = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY, &token)) return -1;
    BOOL ok = DuplicateTokenEx(token, 0, nullptr, SecurityImpersonation, TokenPrimary, &low);
    CloseHandle(token);
    if (!ok) return -1;
    PSID sid = nullptr;
    ok = ConvertStringSidToSidW(L"S-1-16-4096", &sid);   // low mandatory level
    if (ok) {
        TOKEN_MANDATORY_LABEL tml{};
        tml.Label.Attributes = SE_GROUP_INTEGRITY; tml.Label.Sid = sid;
        ok = SetTokenInformation(low, TokenIntegrityLevel, &tml, (DWORD)(sizeof(tml) + GetLengthSid(sid)));
        LocalFree(sid);
    }
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi{};
    ok = ok && CreateProcessAsUserW(low, nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(low);
    if (!ok) return -1;
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 99);   // only the copy this test started
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}

int main(int argc, char** argv) {
    if (argc == 4 && !strcmp(argv[1], "--edge-low"))
        return LowIntegrityChild((HWND)(ULONG_PTR)_strtoui64(argv[2], nullptr, 10), (HWND)(ULONG_PTR)_strtoui64(argv[3], nullptr, 10));
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    RegDeleteTreeW(HKEY_CURRENT_USER, LD_TRAY_SETTINGS_PARENT);   // like Windows 10: no settings, every icon shown
    HKEY parent = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, LD_TRAY_SETTINGS_PARENT, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &parent, nullptr);
    RegCloseKey(parent);

    WindowThread explorer, app;
    explorer.Run(L"EdgeFakeExplorer", FakeExplorerProc);
    app.Run(L"EdgeApp", FakeAppProc);
    g_fakeExplorer = explorer.hwnd;
    Check(explorer.hwnd && app.hwnd && trayhost::Start() && trayhost::Running(), "test windows, host started");
    g_explorerReply[1] = TRUE;
    HICON opaque = MakeIcon(16, 16, 32, [](int, int) { return 0xff20c040u; }, [](int, int) { return false; });

    // ---- what Shell_NotifyIcon sends, at its edges
    {   // a message cut after hIcon (the shortest the host reads): still a delete
        auto add = Notify(NIM_ADD, app.hwnd, 1, NIF_MESSAGE | NIF_ICON);
        add.nid.hIcon = (DWORD)(ULONG_PTR)opaque;
        SendNotify(add, app.hwnd);
        uint64_t k = KeyOf(app.hwnd, 1);
        auto del = Notify(NIM_DELETE, app.hwnd, 1, 0);
        SendTray(1, &del, (DWORD)trayhost::kMinNotify, app.hwnd);
        Check(k && !KeyOf(app.hwnd, 1), "a delete cut after hIcon (32 bytes) still removes the icon");
    }
    {   // a tooltip filling all 128 characters, no NUL: read up to the end of its field only
        auto add = Notify(NIM_ADD, app.hwnd, 2, NIF_MESSAGE | NIF_ICON | NIF_TIP);
        add.nid.hIcon = (DWORD)(ULONG_PTR)opaque;
        for (auto& c : add.nid.szTip) c = L'x';
        for (auto& c : add.nid.szInfo) c = L'y';   // the next text field: must not be read into the tooltip
        SendNotify(add, app.hwnd);
        uint64_t k = KeyOf(app.hwnd, 2);
        auto shown = trayhost::Shown();
        const auto* v = FindShown(shown, k);
        Check(v && v->tip.size() == 128 && v->tip.find(L'y') == std::wstring::npos, "a tooltip without its NUL: its 128 characters, nothing after");
    }
    {   // version 5 does not exist: not taken, Explorer answers
        auto v5 = Notify(NIM_SETVERSION, app.hwnd, 2, 0);
        v5.nid.uVersion = 5;
        g_explorerReply[1] = FALSE;
        Check(SendNotify(v5, app.hwnd) == FALSE, "NIM_SETVERSION 5: Explorer's answer (refused)");
        g_explorerReply[1] = TRUE;
        RECT anchor{};
        trayhost::Send(KeyOf(app.hwnd, 2), trayhost::Mouse::LeftClick, POINT{ 1, 1 }, anchor);
        auto msgs = AppMessages(2);
        Check(msgs.size() == 2 && msgs[0].first == 2 && msgs[0].second == WM_LBUTTONDOWN, "...and the icon keeps talking version 0");
    }
    {   // version 3 (Windows 2000): wParam = id, lParam = event; NIN_SELECT and WM_CONTEXTMENU after the clicks
        auto v3 = Notify(NIM_SETVERSION, app.hwnd, 2, 0);
        v3.nid.uVersion = 3;
        SendNotify(v3, app.hwnd);
        uint64_t k = KeyOf(app.hwnd, 2);
        RECT anchor{};
        trayhost::Send(k, trayhost::Mouse::LeftClick, POINT{ 7, 8 }, anchor);
        auto msgs = AppMessages(3);
        Check(msgs.size() == 3 && msgs[0].first == 2 && msgs[0].second == WM_LBUTTONDOWN && msgs[1].second == WM_LBUTTONUP &&
              msgs[2].first == 2 && msgs[2].second == NIN_SELECT, "v3 left click: down, up, NIN_SELECT, the id in wParam");
        trayhost::Send(k, trayhost::Mouse::RightClick, POINT{ 7, 8 }, anchor);
        msgs = AppMessages(3);
        Check(msgs.size() == 3 && msgs[2].second == WM_CONTEXTMENU, "v3 right click ends with WM_CONTEXTMENU");
        trayhost::Send(k, trayhost::Mouse::Enter, POINT{ 7, 8 }, anchor);
        trayhost::Send(k, trayhost::Mouse::Leave, POINT{ 7, 8 }, anchor);
        msgs = AppMessages(2);
        bool popup = false;
        for (auto& m : msgs) if (m.second == NIN_POPUPOPEN || m.second == NIN_POPUPCLOSE) popup = true;
        Check(msgs.size() == 2 && !popup, "v3 hover: no NIN_POPUPOPEN / NIN_POPUPCLOSE (version 4 only)");
    }
    {   // version 4 on a monitor left of and above the primary one (negative screen coordinates)
        auto v4 = Notify(NIM_SETVERSION, app.hwnd, 2, 0);
        v4.nid.uVersion = 4;
        SendNotify(v4, app.hwnd);
        uint64_t k = KeyOf(app.hwnd, 2);
        RECT anchor{ -1440, -300, -1404, -264 };
        trayhost::Send(k, trayhost::Mouse::RightClick, POINT{ -1420, -280 }, anchor);
        auto msgs = AppMessages(3);
        Check(msgs.size() == 3 && GET_X_LPARAM(msgs[2].first) == -1420 && GET_Y_LPARAM(msgs[2].first) == -280 &&
              LOWORD(msgs[2].second) == WM_CONTEXTMENU && HIWORD(msgs[2].second) == 2,
              "v4 WM_CONTEXTMENU on a monitor left of / above the primary: signed x, y in wParam");
        trayhost::IconIdentifier id{};
        id.message = 1; id.cbSize = sizeof(id); id.hWnd = (DWORD)(ULONG_PTR)app.hwnd; id.uID = 2;
        LRESULT tl = SendTray(3, &id, sizeof(id), app.hwnd);
        id.message = 2;
        LRESULT br = SendTray(3, &id, sizeof(id), app.hwnd);
        Check(GET_X_LPARAM(tl) == -1440 && GET_Y_LPARAM(tl) == -300 && GET_X_LPARAM(br) == -1404 && GET_Y_LPARAM(br) == -264,
              "icon rect on that monitor: signed corners");
        auto hide = Notify(NIM_MODIFY, app.hwnd, 2, NIF_STATE);
        hide.nid.dwState = NIS_HIDDEN; hide.nid.dwStateMask = NIS_HIDDEN;
        SendNotify(hide, app.hwnd);
        g_explorerReply[3] = 0x1234;
        id.message = 1;
        Check(SendTray(3, &id, sizeof(id), app.hwnd) == 0x1234, "a hidden icon's rect: Explorer's answer");
        SendNotify(Notify(NIM_DELETE, app.hwnd, 2, 0), app.hwnd);
    }

    // ---- pictures
    {   // no alpha channel (24 bpp): the mask says what is transparent
        HICON icon = MakeIcon(16, 16, 24, [](int, int) { return 0x00ff0000u; }, [](int x, int) { return x >= 8; });
        auto add = Notify(NIM_ADD, app.hwnd, 3, NIF_MESSAGE | NIF_ICON);
        add.nid.hIcon = (DWORD)(ULONG_PTR)icon;
        SendNotify(add, app.hwnd);
        int w = 0, h = 0; std::vector<uint32_t> px;
        Check(Picture(KeyOf(app.hwnd, 3), w, h, px) && w == 16 && h == 16 && px[0] == 0xffff0000u && px[15] == 0,
              "24 bpp icon: opaque where the mask is clear, transparent where it is set");
        SendNotify(Notify(NIM_DELETE, app.hwnd, 3, 0), app.hwnd);
        DestroyIcon(icon);
    }
    {   // black and white (monochrome): AND mask above XOR mask
        BYTE andBits[16 * 2] = {}, xorBits[16 * 2] = {};
        // rows 0-3 transparent (AND 1, XOR 0); rows 4-7 white (AND 0, XOR 1); rows 8-11 black (0, 0); 12-15 inverted (1, 1)
        for (int y = 0; y < 16; ++y) {
            bool a = y < 4 || y >= 12, x = (y >= 4 && y < 8) || y >= 12;
            andBits[y * 2] = andBits[y * 2 + 1] = a ? 0xff : 0;
            xorBits[y * 2] = xorBits[y * 2 + 1] = x ? 0xff : 0;
        }
        HICON mono = CreateIcon(GetModuleHandleW(nullptr), 16, 16, 1, 1, andBits, xorBits);
        ICONINFO ii{};
        bool isMono = mono && GetIconInfo(mono, &ii) && !ii.hbmColor;
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        auto add = Notify(NIM_ADD, app.hwnd, 4, NIF_MESSAGE | NIF_ICON);
        add.nid.hIcon = (DWORD)(ULONG_PTR)mono;
        SendNotify(add, app.hwnd);
        int w = 0, h = 0; std::vector<uint32_t> px;
        bool got = Picture(KeyOf(app.hwnd, 4), w, h, px) && w == 16 && h == 16;
        Check(isMono && got && px[0] == 0 && px[16 * 5] == 0xffffffffu && px[16 * 9] == 0xff000000u && px[16 * 13] == 0xff000000u,
              "monochrome icon: transparent, white, black, inverted (as black)");
        SendNotify(Notify(NIM_DELETE, app.hwnd, 4, 0), app.hwnd);
        DestroyIcon(mono);
    }

    // ---- a modify for an icon Explorer does not have (the program deleted it; a timer still updates it)
    {
        auto add = Notify(NIM_ADD, app.hwnd, 5, NIF_MESSAGE | NIF_ICON | NIF_TIP);
        add.nid.hIcon = (DWORD)(ULONG_PTR)opaque;
        SendNotify(add, app.hwnd);
        SendNotify(Notify(NIM_DELETE, app.hwnd, 5, 0), app.hwnd);   // gone from Explorer and the dock
        g_explorerReply[1] = FALSE;   // Explorer: no such icon
        auto late = Notify(NIM_MODIFY, app.hwnd, 5, NIF_ICON | NIF_TIP);
        late.nid.hIcon = (DWORD)(ULONG_PTR)opaque;
        wcscpy_s(late.nid.szTip, L"late update");
        const int version = trayhost::Version(), made = LoggedCount(L"+ (by a modify)"), passed = ExplorerGot(WM_COPYDATA, 1);
        LRESULT r = SendNotify(late, app.hwnd);
        uint64_t ghost = KeyOf(app.hwnd, 5);
        Check(r == FALSE && !FindShown(trayhost::Shown(), ghost),
              "a modify Explorer refuses (no such icon) does not put a ghost icon on the dock");
        for (int i = 0; i < 5; ++i) SendNotify(late, app.hwnd);   // the program's timer keeps at it
        Check(!KeyOf(app.hwnd, 5) && trayhost::Version() == version && LoggedCount(L"+ (by a modify)") == made &&
              ExplorerGot(WM_COPYDATA, 1) == passed + 6,
              "...nor for a moment, again and again: no icon made, no redraw, no log line - each modify passed on once");
        g_explorerReply[1] = TRUE;
        if (KeyOf(app.hwnd, 5)) SendNotify(Notify(NIM_DELETE, app.hwnd, 5, 0), app.hwnd);
    }
    {   // ...while a modify for an icon the host missed (Explorer has it) still brings it to the dock
        auto mod = Notify(NIM_MODIFY, app.hwnd, 6, NIF_ICON | NIF_MESSAGE);
        mod.nid.hIcon = (DWORD)(ULONG_PTR)opaque;
        const int passed = ExplorerGot(WM_COPYDATA, 1);
        Check(SendNotify(mod, app.hwnd) == TRUE && FindShown(trayhost::Shown(), KeyOf(app.hwnd, 6)) && ExplorerGot(WM_COPYDATA, 1) == passed + 1,
              "a modify Explorer accepts for an icon the host missed: shown (passed on once)");
        SendNotify(Notify(NIM_DELETE, app.hwnd, 6, 0), app.hwnd);
    }
    {   // ...and so does one Explorer cannot answer (not there): kept, as before
        g_fakeExplorer = nullptr;
        trayhost::g_explorer = nullptr;
        auto mod = Notify(NIM_MODIFY, app.hwnd, 7, NIF_ICON | NIF_MESSAGE);
        mod.nid.hIcon = (DWORD)(ULONG_PTR)opaque;
        LRESULT r = SendNotify(mod, app.hwnd);
        g_fakeExplorer = explorer.hwnd;
        Check(r == FALSE && FindShown(trayhost::Shown(), KeyOf(app.hwnd, 7)),
              "a modify for an unknown icon while Explorer is away: kept on the dock (the program sees no success)");
        SendNotify(Notify(NIM_DELETE, app.hwnd, 7, 0), app.hwnd);
    }

    // ---- dock.log is written once the host has let go of its icons: the dock's thread (Shown, Image) never
    //      waits for a line - a file write (here one that stalls 0.4 s)
    {
        double waited = ShownWhileLogging(L" uid=8 picture=", [&] {
            auto add = Notify(NIM_ADD, app.hwnd, 8, NIF_MESSAGE | NIF_ICON);
            add.nid.hIcon = (DWORD)(ULONG_PTR)opaque;
            SendNotify(add, app.hwnd);
        });
        Check(waited >= 0 && waited < 0.15 && FindShown(trayhost::Shown(), KeyOf(app.hwnd, 8)) && Logged(L"tray host: + ") &&
              Logged(L" uid=8 picture=16x16 shown (no NotifyIconSettings: all shown)"),
              "a new icon's dock.log line: the dock's Shown() does not wait for it (the icon and its line are there)");
        SendNotify(Notify(NIM_DELETE, app.hwnd, 8, 0), app.hwnd);
    }

    // ---- what a low-integrity process can pass on to Explorer through the host
    g_explorerReply[0] = kAppBarReply;
    {
        int beforeCommand = ExplorerGot(WM_COMMAND, 4199), beforeAppBar = ExplorerGot(WM_COPYDATA, 0);
        int rc = RunAtLowIntegrity(trayhost::g_wnd, explorer.hwnd);
        Check(rc >= 0 && !(rc & 1), "precondition: a low-integrity process cannot send WM_COMMAND to the (fake) Explorer itself");
        Check(rc >= 0 && !(rc & 2) && ExplorerGot(WM_COMMAND, 4199) == beforeCommand,
              "a dock that is not elevated does not pass a low-integrity process's WM_COMMAND on to Explorer");
        Check(rc >= 0 && !(rc & 4), "precondition: nor WM_COPYDATA (an app-bar message)");
        Check(rc >= 0 && !(rc & 8) && ExplorerGot(WM_COPYDATA, 0) == beforeAppBar,
              "...and does not pass its WM_COPYDATA (app bars, tray icons) on either: it takes what Explorer takes");
    }
    trayhost::Stop();

    // ---- elevated: clicks go to top-level and message-only windows, not to a window inside another
    g_testElevated = true;
    Check(trayhost::Start() && trayhost::g_elevated, "starts as administrator");
    {   // the filters are open as administrator: ordinary programs reach the host (UIPI cannot tell a
        // low-integrity sender from them, so the copy at low integrity gets through too)
        int beforeCommand = ExplorerGot(WM_COMMAND, 4199), beforeAppBar = ExplorerGot(WM_COPYDATA, 0);
        int rc = RunAtLowIntegrity(trayhost::g_wnd, explorer.hwnd);
        Check(rc >= 0 && (rc & 2) && (rc & 8) && ExplorerGot(WM_COMMAND, 4199) == beforeCommand + 1 &&
              ExplorerGot(WM_COPYDATA, 0) == beforeAppBar + 1,
              "as administrator the host takes WM_COMMAND and WM_COPYDATA from lower integrity levels and passes them on");
    }
    {
        WNDCLASSEXW oc = { sizeof(oc) }; oc.lpfnWndProc = OwnProc; oc.hInstance = GetModuleHandleW(nullptr); oc.lpszClassName = L"EdgeOwnWnd";
        RegisterClassExW(&oc);
        // ATL's CWindowImpl::Create(HWND_MESSAGE) and others give a message-only window the WS_CHILD style
        HWND mo = CreateWindowExW(0, L"EdgeOwnWnd", L"", WS_CHILD, 0, 0, 0, 0, HWND_MESSAGE, nullptr, oc.hInstance, nullptr);
        HWND plain = CreateWindowExW(0, L"EdgeOwnWnd", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, oc.hInstance, nullptr);
        Check(mo && (GetWindowLongPtrW(mo, GWL_STYLE) & WS_CHILD), "precondition: a message-only window keeps its WS_CHILD style");
        auto addMo = Notify(NIM_ADD, mo, 41, NIF_MESSAGE | NIF_ICON);
        auto addPlain = Notify(NIM_ADD, plain, 42, NIF_MESSAGE | NIF_ICON);
        addMo.nid.hIcon = addPlain.nid.hIcon = (DWORD)(ULONG_PTR)opaque;
        SendNotify(addMo, mo); SendNotify(addPlain, plain);
        int got = g_ownGot.load();
        trayhost::Send(KeyOf(plain, 42), trayhost::Mouse::LeftClick, POINT{ 1, 1 }, RECT{});
        Check(g_ownGot.load() >= got + 2, "a message-only window without WS_CHILD gets its click");
        got = g_ownGot.load();
        trayhost::Send(KeyOf(mo, 41), trayhost::Mouse::LeftClick, POINT{ 1, 1 }, RECT{});
        Check(g_ownGot.load() >= got + 2, "a message-only window with WS_CHILD gets its click too (it is not another window's control)");
        // a control inside that message-only window, and a window put into another one without WS_CHILD
        HWND control = CreateWindowExW(0, L"EdgeOwnWnd", L"", WS_CHILD, 0, 0, 0, 0, mo, nullptr, oc.hInstance, nullptr);
        HWND top = CreateWindowExW(0, L"EdgeOwnWnd", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, oc.hInstance, nullptr);
        HWND moved = CreateWindowExW(0, L"EdgeOwnWnd", L"", WS_POPUP, 0, 0, 5, 5, nullptr, nullptr, oc.hInstance, nullptr);
        bool inside = control && top && moved && SetParent(moved, top) && !(GetWindowLongPtrW(moved, GWL_STYLE) & WS_CHILD);
        auto addControl = Notify(NIM_ADD, control, 43, NIF_MESSAGE | NIF_ICON);
        auto addMoved = Notify(NIM_ADD, moved, 44, NIF_MESSAGE | NIF_ICON);
        addControl.nid.hIcon = addMoved.nid.hIcon = (DWORD)(ULONG_PTR)opaque;
        SendNotify(addControl, control); SendNotify(addMoved, moved);
        got = g_ownGot.load();
        int refused = LoggedCount(L"click not passed on (a child window)");
        trayhost::Send(KeyOf(control, 43), trayhost::Mouse::LeftClick, POINT{ 1, 1 }, RECT{});
        trayhost::Send(KeyOf(moved, 44), trayhost::Mouse::LeftClick, POINT{ 1, 1 }, RECT{});
        Check(inside && KeyOf(control, 43) && KeyOf(moved, 44) && g_ownGot.load() == got &&
              LoggedCount(L"click not passed on (a child window)") == refused + 2,
              "not a window inside another one: a message-only window's control, or one moved in without WS_CHILD");
        DestroyWindow(moved); DestroyWindow(top);
        DestroyWindow(control); DestroyWindow(mo); DestroyWindow(plain);
    }
    trayhost::Stop();
    g_testElevated = false;

    // ---- a host that stops late (a hosted widget's program hangs while its window goes back to Explorer)
    {
        Check(trayhost::Start(false) && trayhost::Running(), "starts again (for a widget)");
        WindowThread widget;
        widget.Run(L"EdgeWidget", FakeAppProc);
        SetWindowLongPtrW(widget.hwnd, GWL_STYLE, (GetWindowLongPtrW(widget.hwnd, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
        SetParent(widget.hwnd, trayhost::Window());
        PostMessageW(widget.hwnd, WM_APP + 99, 16000, 0);   // its program hangs for 16 s (Stop() gives up after 10-12.5 s)
        Sleep(100);
        double t0 = NowSeconds();
        trayhost::Stop();
        double took = NowSeconds() - t0;
        bool late = Logged(L"did not stop in time");
        std::printf("  stop took %.1f s%s\n", took, late ? " (did not stop in time)" : "");
        HANDLE old = trayhost::g_thread;
        {   // while it still waits: asked to stop again (another setting turned off), and to start
            bool waiting = old && WaitForSingleObject(old, 0) == WAIT_TIMEOUT;
            t0 = NowSeconds();
            trayhost::Stop();
            double second = NowSeconds() - t0;
            bool started = trayhost::Start(false);
            Check(late && waiting && second < 1 && !started && !trayhost::Running() && trayhost::g_thread == old &&
                  Logged(L"the last one has not stopped yet"),
                  "while it still waits: a second Stop() does not wait again, Start() runs no second host next to it (and says why)");
        }
        bool ended = !old || WaitForSingleObject(old, 10000) == WAIT_OBJECT_0;   // the program recovers, the host thread ends
        Check(late && ended, "precondition: the host thread outlived Stop() and then ended");
        bool again = trayhost::Start(false) && trayhost::Running();
        Check(again, "once that host thread has ended, the host starts again");
        trayhost::Stop();
        widget.Quit();
    }
    {   // a host that ends on its own (something posts WM_QUIT to "the taskbar"): not running, started anew
        Check(trayhost::Start(false) && trayhost::Running(), "starts again");
        HANDLE t = trayhost::g_thread;
        PostMessageW(trayhost::g_wnd, WM_QUIT, 0, 0);
        bool ended = WaitForSingleObject(t, 5000) == WAIT_OBJECT_0;
        Check(ended && !trayhost::Running() && !trayhost::Window(), "a host thread that ended on its own (WM_QUIT): no longer Running()");
        bool again = trayhost::Start(false) && trayhost::Running() && trayhost::Window() && IsWindow(trayhost::Window());
        Check(again, "...and Start() runs a new one");
        trayhost::Stop();
    }
    {   // a program whose icon id changes from run to run (MSI Afterburner's looks like a window handle): Windows keeps
        // one entry for the program, with the id of the run that made it, and shows every later id by it
        // (tests\tray_uid_live.cpp on 25H2; a dock.log on 26H2)
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = trayhost::Lower(exe);
        HKEY root = nullptr;
        RegCreateKeyExW(HKEY_CURRENT_USER, LD_TRAY_SETTINGS_KEY, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr);
        SetEntry(root, L"3001", path, 525458, 1);
        Check(trayhost::Start() && trayhost::Running(), "starts with a settings key");
        auto add = Notify(NIM_ADD, app.hwnd, 196758, NIF_MESSAGE | NIF_ICON);
        add.nid.hIcon = (DWORD)(ULONG_PTR)opaque;
        SendNotify(add, app.hwnd);
        auto shown = [&] { return FindShown(trayhost::Shown(), KeyOf(app.hwnd, 196758)) != nullptr; };
        Check(KeyOf(app.hwnd, 196758) && WaitFor(shown, 2000),
              "an icon whose id has no entry follows its program's only entry (promoted: shown, as Windows shows it)");
        Check(Logged(L"entry 3001 of its program, made for uid 525458"), "...and dock.log says whose entry decided");
        double waited = ShownWhileLogging(L"tray host: now", [&] { SetEntry(root, L"3001", path, 525458, 0); });
        Check(WaitFor([&] { return !shown(); }, 4000), "...and is hidden when that entry is not promoted");
        Check(waited >= 0 && waited < 0.15 &&
              WaitFor([] { return Logged(L"tray host: now tray_host_edge_test.exe uid=196758 picture=ok not shown (entry 3001 of its program"); }, 2000),
              "...its \"now\" line in dock.log: the dock's Shown() does not wait for it either");
        SetEntry(root, L"3001", path, 525458, 1);
        Check(WaitFor(shown, 4000), "...shown again when it is");
        SetEntry(root, L"3002", path, 77, 0);   // a second entry of the program, for another id
        Check(WaitFor([&] { return !shown(); }, 4000),
              "with two entries of the program and none for its id: not shown (no telling which one Windows applies)");
        SetEntry(root, L"3003", path, 196758, 1);   // Windows records the id itself
        Check(WaitFor(shown, 4000), "...until Windows records an entry for the id itself");
        SendNotify(Notify(NIM_DELETE, app.hwnd, 196758, 0), app.hwnd);
        trayhost::Stop();
        RegCloseKey(root);
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, LD_TRAY_SETTINGS_PARENT);
    DestroyIcon(opaque);
    app.Quit(); explorer.Quit();
    std::printf("tray_host_edge_test: %s (%d checks, %d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}
