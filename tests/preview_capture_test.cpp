// The dock's background capture (capture.cpp) on its GDI path - Windows 10, or LIQUIDDOCK_GDI_CAPTURE set (this
// test sets it for itself, so Windows.Graphics.Capture is never started). The screen's DC and the copy from it are
// replaced: nothing on the real screen is read. Frames go to a Direct3D 11 device of the test's own (WARP, no
// window). One PASS/FAIL line per check; the exit code is the number of failures.
#include "../src/common.h"
#include "../src/capture.h"
#include "../src/gfx.h"
#include <thread>
#include <cstdio>
#include <cstdarg>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include "portable_check.h"

// ---- what common.cpp and gfx.cpp give the dock: a clock this test moves, its log, the device
static double g_now = 100;
double NowSeconds() { return g_now; }
static std::vector<std::wstring> logs;
void Log(const wchar_t* fmt, ...) {
    wchar_t line[1024]; va_list ap; va_start(ap, fmt); _vsnwprintf_s(line, _TRUNCATE, fmt, ap); va_end(ap);
    logs.push_back(line);
}
Gfx g_gfx;

// ---- the screen: a monitor right of the primary one, its DC never touched
static const RECT kMonitor{2560, -300, 5120, 1140};
static bool failDib = false;   // CreateDIBSection fails (out of memory or GDI objects)
static int blits = 0;
static POINT blitFrom{};
static HBITMAP WINAPI FakeDib(HDC dc, const BITMAPINFO* bi, UINT usage, void** bits, HANDLE section, DWORD offset) {
    if (failDib) { if (bits) *bits = nullptr; return nullptr; }
    return CreateDIBSection(dc, bi, usage, bits, section, offset);
}
static HDC WINAPI FakeScreen(HWND) { return (HDC)0x5C; }
static int WINAPI FakeRelease(HWND, HDC) { return 1; }
static BOOL WINAPI FakeBlit(HDC, int, int, int, int, HDC from, int x, int y, DWORD) {
    if (from != (HDC)0x5C) return FALSE;
    ++blits; blitFrom = {x, y}; return TRUE;   // the frame stays as it was (black, see-through until made opaque)
}
static BOOL WINAPI FakeMonitorInfo(HMONITOR, LPMONITORINFO mi) { mi->rcMonitor = mi->rcWork = kMonitor; return TRUE; }
#define CreateDIBSection FakeDib
#define GetDC FakeScreen
#define ReleaseDC FakeRelease
#define BitBlt FakeBlit
#define GetMonitorInfoW FakeMonitorInfo
#include "../src/capture.cpp"
#undef CreateDIBSection
#undef GetDC
#undef ReleaseDC
#undef BitBlt
#undef GetMonitorInfoW

static ComPtr<ID3D11Texture2D> Texture(int w, int h, D3D11_USAGE usage = D3D11_USAGE_DEFAULT) {
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = usage;
    d.BindFlags = usage == D3D11_USAGE_STAGING ? 0 : D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : 0;
    ComPtr<ID3D11Texture2D> t; g_gfx.d3d->CreateTexture2D(&d, nullptr, &t); return t;
}
// Every pixel of the texture has this value.
static bool AllPixels(ID3D11Texture2D* t, int w, int h, uint32_t want) {
    auto staging = Texture(w, h, D3D11_USAGE_STAGING);
    if (!staging) return false;
    g_gfx.ctx->CopyResource(staging.Get(), t);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_gfx.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    bool all = true;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) all &= ((uint32_t*)((BYTE*)m.pData + y * m.RowPitch))[x] == want;
    g_gfx.ctx->Unmap(staging.Get(), 0);
    return all;
}
// A crash in Poll is a failed check, not the end of the test.
static int Guarded(MonitorCapture& c, ID3D11Texture2D* dst, int x, int y, int w, int h) {
    __try { return c.Poll(dst, x, y, w, h) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static DWORD GdiObjects() { return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS); }
static int Lines(const wchar_t* part) { int n = 0; for (auto& l : logs) n += l.find(part) != std::wstring::npos; return n; }

int main() {
    SetEnvironmentVariableW(L"LIQUIDDOCK_GDI_CAPTURE", L"1");
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_gfx.d3d, nullptr, &g_gfx.ctx))) {
        std::printf("FAIL: fixture: no WARP device\n"); return 1;
    }
    const DWORD objects = GdiObjects();
    Check(MonitorCapture::UseGdi() && logs.size() == 1 && logs[0].find(L"GDI") != std::wstring::npos, "the GDI path, said once in dock.log");
    {
        MonitorCapture c;
        Check(c.Start((HMONITOR)0x101) && c.Active() && !c.gotFirstFrame, "started on a monitor: active, no frame yet");
        auto dock = Texture(320, 40);
        Check(c.Poll(dock.Get(), 0, 1100, 320, 40) && c.gotFirstFrame, "the first poll brings a frame");
        Check(blits == 1 && blitFrom.x == kMonitor.left && blitFrom.y == kMonitor.top + 1100, "...of the dock's own place on that monitor");
        Check(AllPixels(dock.Get(), 320, 40, 0xff000000u), "...made opaque");
        Check(!c.Poll(dock.Get(), 0, 1100, 320, 40) && blits == 1, "polled again at once: no new frame (30 a second at most)");
        g_now += 0.034;
        Check(c.Poll(dock.Get(), 0, 1100, 320, 40) && blits == 2, "a 30th of a second later: the next one");
        // the dock's window changed size (a setting, a monitor change): a new bitmap for the new size
        auto resized = Texture(200, 30);   // ("small" is a macro of rpcndr.h)
        g_now += 0.034;
        Check(c.Poll(resized.Get(), 10, 1110, 200, 30) && blits == 3 && blitFrom.x == kMonitor.left + 10, "another size: a frame of that size");
        // no bitmap could be made (the process is out of GDI objects or memory)
        failDib = true; g_now += 0.034;
        const int r = Guarded(c, dock.Get(), 0, 1100, 320, 40);
        if (r < 0) std::printf("note: Poll crashed (access violation) when CreateDIBSection failed\n");
        Check(r == 0, "no bitmap for a frame: no frame, and no crash");
        g_now += 0.034;
        const int still = Guarded(c, dock.Get(), 0, 1100, 320, 40);
        Check(still == 0 && blits == 3 && Lines(L"capture bitmap failed") == 1, "...nor on the next poll (tried again, nothing copied), said once in dock.log");
        failDib = false; g_now += 0.034;
        const int again = Guarded(c, dock.Get(), 0, 1100, 320, 40);
        Check(again == 1, "...and the frame after it comes again");
        failDib = true; g_now += 0.034;
        const int later = Guarded(c, resized.Get(), 10, 1110, 200, 30);
        Check(later == 0 && Lines(L"capture bitmap failed") == 2, "a failure again after a bitmap was made: no frame, said again");
        failDib = false;
        c.Stop();
        Check(!c.Active(), "stopped: not active");
    }
    Check(GdiObjects() == objects, "started, polled at two sizes and stopped: no GDI object left behind");
    {   // the dock starts and stops its capture as it shows and hides: nothing piles up
        MonitorCapture c;
        auto wide = Texture(320, 40), narrow = Texture(304, 40);   // the frame's size is the texture's, as in the dock
        for (int i = 0; i < 200; ++i) {
            c.Start((HMONITOR)0x101); g_now += 0.034; c.Poll(narrow.Get(), 0, 1100, 304, 40); g_now += 0.034;
            c.Poll(wide.Get(), 0, 1100, 320, 40);
            if (i % 2) c.Stop();   // ...or Start stops the one before
        }
        c.Stop();
    }
    Check(GdiObjects() == objects, "200 starts and stops: no GDI object left behind");
    return Summary("preview_capture_test");
}
