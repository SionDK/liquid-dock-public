#pragma once
// The tray host tests' harness (tray_host_test, tray_host_edge_test): what the two
// share, and two pieces where they differ taking it as an argument (MakeIcon's colour depth and mask,
// AppMessages' wait for anything extra). Each test keeps its own Log, Logged, Check, fake Explorer and fake programs.
// Include it after the tray host's parts (parts_tray_host.inc: trayhost::TrayNotifyData is a type of
// tray_host_icons.cpp) and after the test's own
//   constexpr UINT kCallback                                the fake tray program's callback message (Notify puts it in)
//   std::mutex g_recMu                                      guards g_appGot
//   std::vector<std::pair<WPARAM, LPARAM>> g_appGot         what the fake tray program got on kCallback
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// a window on its own thread with a message loop (messageOnly: under HWND_MESSAGE, without WS_POPUP)
struct WindowThread {
    HWND hwnd = nullptr;
    std::thread t;
    DWORD tid = 0;
    void Run(const wchar_t* cls, WNDPROC proc, bool messageOnly = false) {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        t = std::thread([this, cls, proc, ready, messageOnly] {
            tid = GetCurrentThreadId();
            WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls;
            RegisterClassExW(&wc);
            hwnd = CreateWindowExW(0, cls, L"", messageOnly ? 0 : WS_POPUP, 0, 0, 10, 10, messageOnly ? HWND_MESSAGE : nullptr, nullptr, wc.hInstance, nullptr);
            SetEvent(ready);
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
        });
        WaitForSingleObject(ready, 5000);
        CloseHandle(ready);
    }
    void Quit() {
        if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);   // DefWindowProc destroys it
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
// 32 bpp, nothing masked: the same bitmaps as tray_host_test's own MakeIcon had made
static HICON MakeIcon(int w, int h, const std::function<uint32_t(int, int)>& px) {
    return MakeIcon(w, h, 32, px, [](int, int) { return false; });
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

// what the fake tray program got: waits (up to 2 s) until `want` messages are in, then settleMs more for any
// extra one (0: none - tray_host_test; tray_host_edge_test waits 50 ms), and takes them all
static std::vector<std::pair<WPARAM, LPARAM>> AppMessages(size_t want, DWORD settleMs) {
    for (int i = 0; i < 200; ++i) {
        { std::lock_guard<std::mutex> lk(g_recMu); if (g_appGot.size() >= want) break; }
        Sleep(10);
    }
    if (settleMs) Sleep(settleMs);
    std::lock_guard<std::mutex> lk(g_recMu);
    auto out = g_appGot;
    g_appGot.clear();
    return out;
}

static const trayhost::IconView* FindShown(const std::vector<trayhost::IconView>& v, uint64_t key) {
    for (auto& i : v) if (i.key == key) return &i;
    return nullptr;
}

static uint64_t KeyOf(HWND app, UINT uid) {
    std::lock_guard<std::mutex> lk(trayhost::g_mu);
    for (auto& ic : trayhost::g_icons) if (ic.hwnd == app && ic.uid == uid) return ic.key;
    return 0;
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
