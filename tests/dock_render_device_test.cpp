// The production Dock (dock*.cpp, dock_record.cpp through dock_record.obj)
// on its real Direct3D / Direct2D device, for what happens around a GPU reset and the built-in recorder.
// The fixture is the one of tests/dock_interaction_test.cpp (tests\dock_fixture_*.h: stubs for the shell, OLED,
// theme, audio, Start and the taskbar; fake monitors; a fake shell's app bars): no input is sent, no window is shown, nothing outside this
// test's folder (build\tests\dock-render\data) is written. Exit code: number of failed checks.
#include "../src/common.h"
#include "../src/gfx.h"
#include "../src/capture.h"
#include "../src/apps.h"
#include "../src/system.h"
#include "../src/sensor_shared.h"
#include "../src/native_tray.h"
#include <shellscalingapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d2d1effects.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <map>
#include <limits>
#include <functional>
#include <set>
#define private public
#include "../src/sensors.h"
#include "../src/dock.h"
#undef private

Settings g_settings;
HWND g_mainWnd = nullptr;
// fake monitors, the fake window in front (fg*) and the other fakes of the fixture's window system
#include "dock_fixture_matrix.h"
static BOOL WINAPI MatrixPopupMenu(HMENU,UINT,int,int,int,HWND,const RECT*){return 0;}
// OLED mode stubs: the matrix never changes the real wallpaper or desktop icons
static bool oledOn=false;
namespace oled {
bool Active(){return oledOn;}
bool CoversMonitor(HMONITOR){return oledOn;}   // the matrix's OLED mode covers every monitor
bool Enable(HMONITOR){oledOn=true;return true;}
bool Disable(){oledOn=false;return true;}
void Reapply(){}
}
// the shape window never opens in the matrix
#include "../src/settings_window.h"
namespace settingswindow { void Show(HMONITOR){} void Close(){} bool IsOpen(){return false;} bool PreTranslate(MSG*){return false;} }
// the desktop, theme, shell, Recycle Bin, audio and Start stubs that only answer; the taskbar's Start button and app
// buttons; the shell's app bars (no real app bar is registered)
#include "dock_fixture_quiet.h"
#include "dock_fixture_taskbar.h"
#include "dock_fixture_appbar.h"
// the Dock's parts under the fixture's fakes
#include "dock_fixture_parts.h"

static int checks = 0, failures = 0;
static void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) ++failures;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
}
static void Apps(int count) {
    g_apps.items.clear(); g_apps.attentionEvents.clear(); ++g_apps.structureVersion;
    for (int i = 0; i < count; ++i) {
        auto item = std::make_shared<AppItem>(); item->id = i + 1; item->name = L"Test App " + std::to_wstring(i + 1); item->pinned = true;
        g_apps.items.push_back(item);
    }
}
// What DirectWrite says right now, without any cache.
static D2D1_SIZE_F Fresh(IDWriteTextFormat* f, const std::wstring& s) {
    ComPtr<IDWriteTextLayout> tl;
    if (!f || FAILED(g_gfx.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 200, &tl))) return { -1, -1 };
    DWRITE_TEXT_METRICS tm{}; tl->GetMetrics(&tm);
    return { tm.widthIncludingTrailingWhitespace, tm.height };
}
static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }
// The panel's height in DIP as the dock's own Dock::PanelHeightDip gave it (the dock no longer has it: only this test
// asked it): vram the VRAM block, vramTemp its temperature row (sensorlogic::PanelHeightDip's 0 / 1 / 2)
static float DockPanelHeightDip(int dimmRows, int driveRows, bool vram, bool vramTemp = true) {
    return sensorlogic::PanelHeightDip(dimmRows, driveRows, !vram ? 0 : vramTemp ? 2 : 1);
}
static std::string ReadAll(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
static long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return -1;
    return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

// Test monitors: a laptop's 2880x1800 (scale 1.25), a desktop's 2560x1440 (1.0), and a
// 1920x1080 external monitor (0.75) - the dock's scale comes from the screen's short side, not from Windows' 150 %.
static const HMONITOR kLaptop = (HMONITOR)(uintptr_t)0x2001, kExternal = (HMONITOR)(uintptr_t)0x2002, kDesktop = (HMONITOR)(uintptr_t)0x2003;

// Dock::TextSize (dock_layout.cpp) caches text metrics by the IDWriteTextFormat's address. Gfx::Reset (a GPU
// reset or driver update) keeps the text formats (dock_render-4: it freed them, and EnsureFonts made new ones
// that the heap often put at the addresses of freed ones of another size - another monitor's scale - so the
// cell widths measured from "CPU 100°", "100%" and the clock came from the wrong font until the cache happened
// to be cleared).
static void ResetMetrics(std::initializer_list<HMONITOR> mons, const char* what) {
    g_settings = Settings{};
    g_sys.SetBattery(battery::FromRaw({ 0, 1, 57, 0, 7200 }));   // a laptop on battery: the battery cell is measured too
    Apps(6);
    std::vector<std::unique_ptr<Dock>> docks;
    for (HMONITOR m : mons) docks.push_back(std::make_unique<Dock>(m, (int)docks.size(), m == *mons.begin()));
    printf("  %s:\n", what);
    int wrongSensor = 0, wrongBattery = 0, wrongClock = 0, rounds = 0;
    for (int round = 0; round < 12; ++round, ++rounds) {
        for (auto& d : docks) {
            d->Metrics();                                   // EnsureFonts(scale) and the measurements, as BuildDocks
            const auto m = d->MeasureMetrics(1);
            const float sensor = Fresh(g_gfx.fmtClockSmall.Get(), L"CPU 100°").width + 16 * d->scale;
            const auto pct = Fresh(g_gfx.fmtClockSmall.Get(), L"100%");
            const float battery = std::max(BatteryIconWidth(m.icon, d->scale), pct.width) + 14 * d->scale;
            const float clock = std::max(Fresh(d->ClockFont(), g_sys.timeText).width, Fresh(d->ClockFont(true), g_sys.dateText).width) + 12 * d->scale;
            if (!Near(m.sensor, sensor) && !wrongSensor) printf("  e.g. sensor cell at scale %.2f: %.1f px, measured now %.1f px\n", d->scale, m.sensor, sensor);
            wrongSensor += !Near(m.sensor, sensor); wrongBattery += !Near(m.battery, battery); wrongClock += !Near(m.clock, clock);
        }
        if (!g_gfx.Reset()) { printf("GPU reset failed\n"); break; }   // what main does on a lost device (no dock owns device resources here)
    }
    printf("  %d GPU resets: wrong sensor cell %d, battery cell %d, clock cell %d (of %d docks each)\n", rounds, wrongSensor, wrongBattery, wrongClock, rounds * (int)docks.size());
    const std::string tail = std::string(" - ") + what;
    Check(wrongSensor == 0, ("after a GPU reset the sensor cell is measured with the new font (CPU 100°)" + tail).c_str());
    Check(wrongBattery == 0, ("after a GPU reset the battery cell is measured with the new font (100%)" + tail).c_str());
    Check(wrongClock == 0, ("after a GPU reset the clock cell is measured with the new font" + tail).c_str());
    g_sys.SetBattery({});
}

// The built-in recorder (developer builds): what a start that cannot open its file leaves behind, and that
// a normal recording writes the frames it counts.
static void Recorder() {
    g_settings = Settings{};
    Apps(6);
    const std::wstring rec = AppDataDir() + L"\\rec", raw = rec + L"\\frames.raw", info = rec + L"\\info.txt";
    CreateDirectoryW(rec.c_str(), nullptr);
    DeleteFileW(raw.c_str()); DeleteFileW(info.c_str());
    Dock::carryRecording = false; Dock::carryFrames = 0;
    {
        Dock d(kLaptop, 0, true);
        if (!d.Create()) { Check(false, "recorder fixture dock created"); return; }
        d.nextCaptureTry = std::numeric_limits<double>::max();
        // frames.raw cannot be opened (a directory here; in real use: ffmpeg still reading the last one)
        CreateDirectoryW(raw.c_str(), nullptr);
        d.StartRecording({ 0, 900, 2880, 1800 }, 0.5f);
        Check(!d.Recording(), "a recording whose file cannot be opened does not start");
        Check(!Dock::carryRecording, "a recording that never started is not carried over to rebuilt docks");
        Check(!d.rec.fullTex.tex && !d.rec.dockFrame.tex && !d.rec.rt.tex && !d.rec.target, "a recording that never started keeps no monitor-sized textures");
        RemoveDirectoryW(raw.c_str());
        Dock::carryRecording = false;

        d.StartRecording({ 0, 900, 2880, 1800 }, 0.5f);
        Check(d.Recording() && d.rec.w == 1440 && d.rec.h == 450, "recording at half size of the cropped half");
        double t = NowSeconds();
        for (int i = 0; i < 6; ++i) { d.reveal.snap(1); d.dirty = true; d.Render(); t += 1.0 / 30; d.RecordTick(t); }
        d.StopRecording();
        const long long frameBytes = 1440LL * 450 * 4;
        std::string text = ReadAll(info);
        int w = 0, h = 0, fps = 0, frames = -1;
        sscanf_s(text.c_str(), "%d %d %d %d", &w, &h, &fps, &frames);
        Check(w == 1440 && h == 450 && fps == 30 && frames == 4, "info.txt: size, 30 fps and the frames written (two frames late)");
        Check(FileSize(raw) == frames * frameBytes, "frames.raw holds exactly the counted frames");
        Check(!d.Recording() && !Dock::carryRecording && !d.rec.fullTex.tex && !d.rec.rt.tex, "stopping releases the recorder's textures");

        // docks rebuilt while recording: the new dock appends and the count goes on
        d.StartRecording({ 0, 900, 2880, 1800 }, 0.5f);
        t = NowSeconds();
        for (int i = 0; i < 4; ++i) { d.dirty = true; d.Render(); t += 1.0 / 30; d.RecordTick(t); }
        d.SuspendRecording();
        Check(Dock::carryRecording && Dock::carryFrames == 2, "a rebuild carries the recording and its frame count");
    }
    {
        Dock d(kLaptop, 1, true);
        if (!d.Create()) { Check(false, "second recorder fixture dock created"); return; }
        d.nextCaptureTry = std::numeric_limits<double>::max();
        d.StartRecording(Dock::carryCrop, Dock::carryScale, true);
        double t = NowSeconds();
        for (int i = 0; i < 4; ++i) { d.reveal.snap(1); d.dirty = true; d.Render(); t += 1.0 / 30; d.RecordTick(t); }
        d.StopRecording();
        int w = 0, h = 0, fps = 0, frames = -1;
        sscanf_s(ReadAll(info).c_str(), "%d %d %d %d", &w, &h, &fps, &frames);
        Check(frames == 4 && FileSize(raw) == 4LL * 1440 * 450 * 4, "after a rebuild the frames are appended and counted together");
    }
    // docks rebuilt while recording, and frames.raw held by another program (ffmpeg reading it) when the new
    // dock opens it again: the recording ends there, with an info.txt for the frames written before
    {
        Dock d(kLaptop, 2, true);
        if (!d.Create()) { Check(false, "third recorder fixture dock created"); return; }
        d.nextCaptureTry = std::numeric_limits<double>::max();
        d.StartRecording({ 0, 900, 2880, 1800 }, 0.5f);
        double t = NowSeconds();
        for (int i = 0; i < 5; ++i) { d.reveal.snap(1); d.dirty = true; d.Render(); t += 1.0 / 30; d.RecordTick(t); }
        d.SuspendRecording();   // 3 frames written (two frames late)
    }
    {
        Dock d(kLaptop, 3, true);
        if (!d.Create()) { Check(false, "fourth recorder fixture dock created"); return; }
        d.nextCaptureTry = std::numeric_limits<double>::max();
        DeleteFileW(info.c_str());
        HANDLE held = CreateFileW(raw.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);   // no writer may share it
        d.StartRecording(Dock::carryCrop, Dock::carryScale, true);
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        int w = 0, h = 0, fps = 0, frames = -1;
        sscanf_s(ReadAll(info).c_str(), "%d %d %d %d", &w, &h, &fps, &frames);
        Check(held != INVALID_HANDLE_VALUE && !d.Recording() && !Dock::carryRecording && !d.rec.fullTex.tex && !d.rec.rt.tex,
              "a rebuilt dock that cannot open frames.raw again does not record, keeps no textures and carries nothing on");
        Check(w == 1440 && h == 450 && frames == 3 && Dock::carryFrames == 0, "...and info.txt counts the frames written before the rebuild");
    }
    // --rec-stop (every dock's StopRecording) also drops a recording carried over that no dock took up
    Dock::carryRecording = true;
    { Dock d(kLaptop, 4, true); d.StopRecording(); }
    Check(!Dock::carryRecording, "stopping drops a carried recording even when no dock records");
    DeleteFileW(raw.c_str()); DeleteFileW(info.c_str());
}

// Drawing at the laptop's and the desktop monitor's scale on every edge, then a GPU reset and the same again: the
// shared Direct2D context is left as it was found (no target, identity transform) and EndDraw never fails.
static void RenderAround() {
    for (int pass = 0; pass < 2; ++pass) {
        bool clean = true, drew = true;
        for (HMONITOR m : { kLaptop, kDesktop }) for (int edge = 0; edge < 4; ++edge) {
            g_settings = Settings{}; g_settings.edge = (Edge)edge;
            g_settings.trayButtons = "desktop,theme,oled,awake,screenshot,quiet,trash,downloads,audio,sff";
            Apps(10);
            Dock d(m, edge, m == kLaptop);
            if (!d.Create()) { drew = false; continue; }
            d.nextCaptureTry = std::numeric_limits<double>::max();
            d.reveal.snap(1); d.sbPeek.snap(0); d.panelAlpha.snap(1); d.tooltipText = L"Test App 1"; d.tooltipAlpha.snap(1);
            d.Layout(); d.ComputePanelRect(); d.dirty = true; d.Render();
            ComPtr<ID2D1Image> target; g_gfx.dc->GetTarget(&target);
            D2D1_MATRIX_3X2_F tr; g_gfx.dc->GetTransform(&tr);
            clean &= !target && tr.m11 == 1 && tr.m22 == 1 && tr.m12 == 0 && tr.m21 == 0 && tr.dx == 0 && tr.dy == 0;
            d.reveal.snap(0); d.dirty = true; d.Render();   // hidden: cleared once
            clean &= d.clearedWhileHidden;
        }
        Check(drew, pass ? "docks are created again on the new device" : "docks are created on every edge at 1.0 and 1.25");
        Check(clean, "the shared Direct2D context has no target and an identity transform after each frame");
        if (pass == 0) Check(g_gfx.Reset(), "GPU reset re-creates the device");
    }
    Check(ReadAll(AppDataDir() + L"\\dock.log").find("EndDraw") == std::string::npos, "no frame fails in EndDraw");
}

// The Start icon and the OLED button fill with linear gradients. They made a new stop collection (a texture on
// the GPU) and brush every frame, about 0.1 ms each; Gfx::GradientBrush keeps the brushes of the last few stop
// sets. What they draw is compared pixel by pixel with the drawing as it was (new brushes each time, below).
static void OldStartIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, D2D1_RECT_F r, float alpha) {
    float sz = r.right - r.left;
    float rad = sz * 0.225f;
    D2D1_GRADIENT_STOP stops[2] = { { 0, D2D1::ColorF(0.30f, 0.70f, 1.0f, alpha) }, { 1, D2D1::ColorF(0.05f, 0.33f, 0.88f, alpha) } };
    ComPtr<ID2D1GradientStopCollection> sc;
    dc->CreateGradientStopCollection(stops, 2, &sc);
    ComPtr<ID2D1LinearGradientBrush> gb;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ r.left, r.top }, { r.right, r.bottom }), sc.Get(), &gb);
    D2D1_ROUNDED_RECT rr = { r, rad, rad };
    dc->FillRoundedRectangle(rr, gb.Get());
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.28f * alpha));
    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{ { r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f }, rad, rad }, brush, 1.0f);
    float lg = sz * 0.42f, g = sz * 0.03f, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    float q = (lg - g) / 2;
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.97f * alpha));
    for (int i = 0; i < 4; ++i) {
        float x = cx - lg / 2 + (i % 2) * (q + g), y = cy - lg / 2 + (i / 2) * (q + g);
        dc->FillRectangle({ x, y, x + q, y + q }, brush);
    }
}
static void OldOledIcon(ID2D1DeviceContext* dc, D2D1_RECT_F r, float scale, float zoom, D2D1_COLOR_F ink, bool oled) {
    D2D1_POINT_2F c = { (r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f };
    float s = scale * zoom;
    float w = 13 * s, h = 15 * s;
    D2D1_RECT_F b = { c.x - w / 2, c.y - h / 2, c.x + w / 2, c.y + h / 2 };
    ComPtr<ID2D1RoundedRectangleGeometry> shape;
    if (FAILED(g_gfx.d2dFactory->CreateRoundedRectangleGeometry({ b, 2.6f * s, 2.6f * s }, &shape))) return;
    D2D1_COLOR_F col[3];
    if (oled) { col[0] = D2D1::ColorF(0xFF3B30, ink.a); col[1] = D2D1::ColorF(0x5BC22A, ink.a); col[2] = D2D1::ColorF(0x4459F0, ink.a); }
    else { const float a[3] = { 0.55f, 0.75f, 0.95f }; for (int i = 0; i < 3; ++i) { col[i] = ink; col[i].a *= a[i]; } }
    D2D1_GRADIENT_STOP stops[6] = { { 0, col[0] }, { 1 / 3.0f, col[0] }, { 1 / 3.0f, col[1] }, { 2 / 3.0f, col[1] }, { 2 / 3.0f, col[2] }, { 1, col[2] } };
    ComPtr<ID2D1GradientStopCollection> coll; ComPtr<ID2D1LinearGradientBrush> fill;
    if (FAILED(dc->CreateGradientStopCollection(stops, 6, &coll)) ||
        FAILED(dc->CreateLinearGradientBrush({ { b.left, c.y }, { b.right, c.y } }, coll.Get(), &fill))) return;
    D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
    dc->SetTransform(D2D1::Matrix3x2F::Skew(-35, 0, c) * old);
    dc->FillGeometry(shape.Get(), fill.Get());
    dc->SetTransform(old);
}
// One frame drawn by `draw` on an offscreen target, its pixels read back (premultiplied BGRA).
template <class F> static std::vector<uint32_t> Pixels(int w, int h, F draw) {
    auto* dc = g_gfx.dc.Get();
    const auto pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pf);
    auto rp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pf);
    ComPtr<ID2D1Bitmap1> target, read;
    if (FAILED(dc->CreateBitmap({ (UINT32)w, (UINT32)h }, nullptr, 0, &bp, &target)) ||
        FAILED(dc->CreateBitmap({ (UINT32)w, (UINT32)h }, nullptr, 0, &rp, &read))) return {};
    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    draw();
    const HRESULT hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    D2D1_MAPPED_RECT m{};
    if (FAILED(hr) || FAILED(read->CopyFromBitmap(nullptr, target.Get(), nullptr)) || FAILED(read->Map(D2D1_MAP_OPTIONS_READ, &m))) return {};
    std::vector<uint32_t> px((size_t)w * h);
    for (int y = 0; y < h; ++y) memcpy(&px[(size_t)y * w], m.bits + (size_t)y * m.pitch, (size_t)w * 4);
    read->Unmap();
    return px;
}
static int Lit(const std::vector<uint32_t>& px) { int n = 0; for (uint32_t p : px) n += p != 0; return n; }
// The pixels of a frame's rows y0 .. y1 that are clearly green ('g': a load bar in the GPU's colour), warm ('r': a
// warm or hot temperature, orange or red), orange only ('o': warm) or red only ('R': hot); white text and the grey
// tracks are none of them.
static int Tinted(const std::vector<uint32_t>& px, int w, int y0, int y1, char which) {
    int n = 0;
    for (int y = std::max(0, y0); y < y1 && (size_t)(y + 1) * w <= px.size(); ++y) for (int x = 0; x < w; ++x) {
        const uint32_t p = px[(size_t)y * w + x];
        const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        n += which == 'g' ? (g > r + 40 && g > b + 40) : which == 'o' ? (r > g + 40 && g > b + 60) :
             which == 'R' ? (r > g + 120 && r > b + 120) : (r > g + 60 && r > b + 60);
    }
    return n;
}

static void GradientsDrawn();
static void Gradients() {
    GradientsDrawn();   // its dock (and that dock's device resources) gone first
    // a GPU reset drops them with the device
    Check(!g_gfx.gradients.empty() && g_gfx.Reset() && g_gfx.gradients.empty(), "a GPU reset drops the kept gradient brushes");
    g_settings = Settings{};
}
static void GradientsDrawn() {
    g_settings = Settings{};
    Apps(6);
    Dock d(kLaptop, 0, true);
    if (!d.Create()) { Check(false, "gradient fixture dock created"); return; }
    d.nextCaptureTry = std::numeric_limits<double>::max();
    auto* dc = g_gfx.dc.Get();
    const float sz = 56;
    // the Start icon: steady, fading, and twice in one frame (a brush kept is moved between the two)
    const D2D1_RECT_F a = { 8, 8, 8 + sz, 8 + sz }, b = { 90, 20, 90 + sz * 0.8f, 20 + sz * 0.8f };
    const auto oldStart = Pixels(160, 90, [&] { OldStartIcon(dc, d.brush.Get(), a, 1); OldStartIcon(dc, d.brush.Get(), b, 1); OldStartIcon(dc, d.brush.Get(), b, 0.37f); });
    g_gfx.gradients.clear();
    const int made0 = g_gfx.gradientsMade;
    const auto newStart = Pixels(160, 90, [&] { d.DrawStartIcon(a, 1); d.DrawStartIcon(b, 1); d.DrawStartIcon(b, 0.37f); });
    const auto keptStart = Pixels(160, 90, [&] { d.DrawStartIcon(a, 1); d.DrawStartIcon(b, 1); d.DrawStartIcon(b, 0.37f); });
    Check(!oldStart.empty() && Lit(oldStart) > 1000 && newStart == oldStart && keptStart == oldStart,
          "the Start icon is drawn pixel for pixel as with a new gradient brush each time (steady, faded, twice a frame)");
    Check(g_gfx.gradientsMade - made0 == 2, "two frames of the Start icon at two alphas realize two stop collections (were six)");
    // the OLED button off and on, in two inks and at the settings preview's zoom, several in one frame
    const float scale = 1.25f;
    const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 0.9f), dark = D2D1::ColorF(0.07f, 0.07f, 0.09f, 0.86f);
    struct Case { D2D1_RECT_F r; float zoom; D2D1_COLOR_F ink; bool on; };
    const Case cases[] = { { { 4, 4, 54, 54 }, 1, white, false }, { { 60, 4, 110, 54 }, 1, white, true }, { { 116, 4, 166, 54 }, 0.8f, dark, false },
                           { { 4, 60, 54, 110 }, 1, dark, true }, { { 60, 60, 110, 110 }, 0.8f, white, false }, { { 116, 60, 166, 110 }, 1, white, true } };
    const auto oldOled = Pixels(170, 114, [&] { for (auto& c : cases) OldOledIcon(dc, c.r, scale, c.zoom, c.ink, c.on); });
    auto oled = [&] {
        for (auto& c : cases) { IconState st; st.oled = c.on; DrawButtonIcon(ButtonId::Oled, dc, d.brush.Get(), g_gfx.fmtGlyph.Get(), c.r, scale, c.zoom, c.ink, 0, st); }
    };
    g_gfx.gradients.clear();
    const int made1 = g_gfx.gradientsMade;
    const auto newOled = Pixels(170, 114, oled);
    const auto keptOled = Pixels(170, 114, oled);
    Check(!oldOled.empty() && Lit(oldOled) > 1000 && newOled == oldOled && keptOled == oldOled,
          "the OLED button is drawn pixel for pixel as with new gradient brushes (off and on, two inks, several a frame)");
    Check(g_gfx.gradientsMade - made1 == 4, "two frames of six OLED buttons in four colourings realize four stop collections (were twelve)");
    // a dock's frames while it stays as it is: the Start icon's and the OLED button's brushes are made once
    g_settings.trayButtons = "oled";
    d.Layout(); d.reveal.snap(1);
    g_gfx.gradients.clear();
    const int made2 = g_gfx.gradientsMade;
    for (int i = 0; i < 100; ++i) { d.dirty = true; d.Render(); }
    const int steady = g_gfx.gradientsMade - made2;
    printf("  100 frames of a shown dock with the OLED button: %d stop collections realized\n", steady);
    Check(steady == 2, "100 frames of a shown dock (Start icon, OLED button): two stop collections, not 200");
}

// Which window in front is a fullscreen app (dock.cpp's FullscreenMonitor), for a browser maximized on the laptop
// (150 %: its frame hangs 11 px over) and on the desktop monitor left of it (100 %, 8 px, negative coordinates):
// F11 or a page's fullscreen video keeps it maximized, without caption or frame, exactly the monitor (mod.24).
static void MaximizedFront() {
    const LONG_PTR f11 = WS_OVERLAPPED | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE | WS_MAXIMIZE;
    const LONG_PTR maximized = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_MAXIMIZE;
    bool f11Seen = true, windowsNot = true;
    for (auto [m, frame] : { std::pair{ kLaptop, 11 }, std::pair{ kDesktop, 8 } }) {
        RECT mon{};
        for (auto& e : monitors) if (e.handle == m) mon = e.rect;
        const RECT over = { mon.left - frame, mon.top - frame, mon.right + frame, mon.bottom + frame };
        RECT shortClient = mon; shortClient.bottom -= 2;
        auto seen = [&](LONG_PTR style, RECT window, RECT client, bool infoFails = false) {
            fgEnabled = true; fgMonitor = m; fgMaximized = true; fgStyle = style; fgRect = window; fgClient = client; fgInfoFails = infoFails;
            RECT r{};
            const HMONITOR fs = FullscreenMonitor(nullptr, MatrixForeground(), &r);
            return fs == m && EqualRect(&r, &mon);
        };
        f11Seen &= seen(f11, mon, mon);
        windowsNot &= !seen(maximized, over, mon) && !seen(f11 | WS_THICKFRAME, mon, mon) && !seen(WS_POPUP | WS_VISIBLE | WS_MAXIMIZE, mon, mon) &&
                      !seen(f11, mon, shortClient) && !seen(f11, mon, mon, true);
    }
    fgEnabled = fgMaximized = fgInfoFails = false; fgStyle = WS_POPUP; fgRect = fgClient = {}; fgMonitor = nullptr;
    Check(f11Seen, "a browser maximized and then in F11 counts as fullscreen on its monitor (laptop, desktop left of it)");
    Check(windowsNot, "maximized windows over the monitor by their frame, with a frame, WS_POPUP, a short client area or no GetWindowInfo do not");
}

// The "PC 상태" panel (dock_panel.cpp): the window keeps room for the drives, the memory rows and the VRAM block
// known when it is built, the panel is as high as what it shows, more drives than the room are counted ("외 N개"),
// a PC without GPU memory of its own gets the panel as it was, and it is drawn at the laptop's and the desktop monitor's
// scale on every edge. (How it looks is checked by eye, not here.)
static void SensorPanel() {
    auto drive = [](wchar_t l, float t) { sensorlogic::DriveRow r; r.letter = l; r.model = L"WD SN850X"; r.temp = t; return r; };
    auto dimm = [](int k, float t) { sensorlogic::DimmRow r; r.slot = k == 1 ? L"A2" : L"B2"; r.label = sensorlogic::DimmLabel(k, r.slot); r.temp = t; return r; };
    // vram: 0 none (an integrated GPU), 1 the block with its temperature, 2 its temperature and amount unknown, 3 hot,
    // 4 warm (93 °C: orange from 90, red only from SensorClient::kVramHot, 100 - not from the modules' 85)
    auto fill = [&](int drives, int dimms, int vram = 1) {
        g_sensors.live = true;
        g_sensors.d = {};
        g_sensors.d.flags = LD_HAS_CPU_TEMP | LD_HAS_GPU | LD_HAS_GPU_LOAD;
        g_sensors.d.cpuTemp = 82; g_sensors.d.cpuLoad = 100; g_sensors.d.gpuTemp = 36; g_sensors.d.ramUsed = 14.2f; g_sensors.d.ramTotal = 62.8f; g_sensors.d.ramLoad = 23;
        wcscpy_s(g_sensors.d.cpuName, L"Ryzen 7 7800X3D"); wcscpy_s(g_sensors.d.gpuName, L"NVIDIA GeForce RTX 5080");
        PanelData p;
        for (int i = 0; i < drives; ++i) p.drives.push_back(drive(wchar_t(L'C' + i), i == 1 ? NAN : 40.f + i));
        for (int i = 0; i < dimms; ++i) p.dimms.push_back(dimm(i + 1, i ? NAN : 32.8f));
        p.drivesWithTemp = sensorlogic::CountWithTemp(p.drives); p.modules = dimms; p.dimmsWithTemp = sensorlogic::CountWithTemp(p.dimms); p.sampled = true;
        if (vram) {
            p.vram.present = true; p.vram.totalGB = 16.0f;
            if (vram != 2) { p.vram.usedGB = 6.2f; p.vram.load = 38.75f; p.vram.temp = vram == 3 ? 104.0f : vram == 4 ? 93.0f : 62.0f; }
        }
        g_sensors.panel = p;
    };
    bool sized = true, fits = true, counted = true, drivesFirst = true, waiting = true, inside = true, whole = true, drawn = true;
    bool without = true, vramLater = true, vramDrawn = true, vramColours = true, smallScreen = true;
    for (HMONITOR m : { kLaptop, kDesktop }) for (int edge = 0; edge < 4; ++edge) {
        g_settings = Settings{}; g_settings.edge = (Edge)edge;
        const bool vert = IsVertical(g_settings.edge);
        const int cols = Dock::PanelColumns(vert);
        auto rowsOf = [&](int n) { return (n + cols - 1) / cols; };
        Apps(8);
        fill(5, 2);   // a PC with five drives (three rows; five on a vertical dock), two modules (one row; two), a GPU with memory
        // the room: what the panel held, never less than this PC's own count (PanelRoomHint)
        const auto room = g_sensors.PanelRoomHint();
        Dock d(m, edge, m == kLaptop);
        if (!d.Create()) { drawn = false; continue; }
        d.nextCaptureTry = std::numeric_limits<double>::max();
        d.reveal.snap(1); d.hideOff = 0; d.panelAlpha.snap(1); d.panelAlpha.target = 1;
        d.Layout(); d.ComputePanelRect();
        const float ph = vert ? d.panelRect.bottom - d.panelRect.top : d.panelRect.bottom - d.panelRect.top;   // upright on every edge
        auto rows = d.PanelRowsNow();
        const int roomDimmRows = std::min(Dock::kPanelMaxDimmRows, rowsOf(room.dimms));
        const int roomDriveRows = std::clamp(rowsOf(room.drives), 1, Dock::kPanelMaxDriveRows);
        sized &= room.drives >= 5 && room.dimms >= 2 && room.vram && d.panelW == std::round(Dock::PanelWidthDip(vert) * d.scale) &&
                 d.panelH == std::round(DockPanelHeightDip(roomDimmRows, roomDriveRows, true) * d.scale) &&
                 Near(d.panelRect.right - d.panelRect.left, d.panelW) && Near(ph, std::round(DockPanelHeightDip(rowsOf(2), rowsOf(5), true) * d.scale)) &&
                 rows.dimmRows == rowsOf(2) && rows.driveRows == rowsOf(5) && rows.drivesShown == 5 && rows.drivesHidden == 0 && rows.dimmsShown == 2 && rows.vram;
        if (!vert) sized &= Near(DockPanelHeightDip(1, 3, false), 332) && Near(DockPanelHeightDip(1, 3, true), 403) && Near(Dock::PanelWidthDip(false), 450);
        else sized &= Near(Dock::PanelWidthDip(true), 320);
        sized &= Near(DockPanelHeightDip(0, 1, false), 243) && Near(DockPanelHeightDip(0, 1, true), 322);   // the block 71 DIP, and a divider without memory rows
        inside &= d.panelRect.left >= 0 && d.panelRect.top >= 0 && d.panelRect.right <= d.winW && d.panelRect.bottom <= d.winH;
        whole &= d.panelRect.left == std::round(d.panelRect.left) && d.panelRect.top == std::round(d.panelRect.top) &&
                 d.panelRect.right == std::round(d.panelRect.right) && d.panelRect.bottom == std::round(d.panelRect.bottom);
        // a GPU fixture's frame with the panel: text and cards drawn, the context left clean
        const auto px = Pixels((int)d.winW, (int)d.winH, [&] { g_gfx.EnsureFonts(d.scale); d.DrawPanel(); });
        drawn &= !px.empty() && Lit(px) > 2000;
        const float topWith = d.panelRect.top;
        // a PC whose GPU has no memory of its own (an integrated GPU): no block, the panel as high as before it
        fill(5, 2, 0);
        d.ComputePanelRect();
        rows = d.PanelRowsNow();
        const float phNo = d.panelRect.bottom - d.panelRect.top;
        without &= !rows.vram && rows.dimmRows == rowsOf(2) && rows.driveRows == rowsOf(5) && rows.drivesHidden == 0 &&
                   Near(phNo, std::round(DockPanelHeightDip(rowsOf(2), rowsOf(5), false) * d.scale)) && phNo < ph;
        const auto pxNo = Pixels((int)d.winW, (int)d.winH, [&] { d.DrawPanel(); });
        const int litNo = Lit(pxNo);
        // the block's rows in the frame with it (below the RAM bar and the memory rows: 183 + 27 a row, 71 DIP high):
        // its bar's fill in the GPU's green there, no red; without the block nothing green in those rows (drives).
        // Each frame's rows from its own top: on a bottom dock the panel's top moves with its height.
        const int bandOff = (int)(std::round(183 * d.scale) + std::round(rowsOf(2) * 27 * d.scale)), bandH = (int)std::round(71 * d.scale);
        const int bandTop = (int)topWith + bandOff, bandEnd = bandTop + bandH, bandTopNo = (int)d.panelRect.top + bandOff;
        vramDrawn &= litNo > 2000 && Lit(px) > litNo && Tinted(px, d.winW, bandTop, bandEnd, 'g') > 150 && Tinted(px, d.winW, bandTop, bandEnd, 'r') == 0 &&
                     Tinted(pxNo, d.winW, bandTopNo, bandTopNo + bandH, 'g') == 0;
        // its temperature and amount unknown (the hint), and hot: drawn in the same room
        for (int v : { 2, 3, 4 }) {
            fill(5, 2, v);
            d.ComputePanelRect();
            const auto pxV = Pixels((int)d.winW, (int)d.winH, [&] { d.DrawPanel(); });
            vramDrawn &= d.PanelRowsNow().vram && Near(d.panelRect.bottom - d.panelRect.top, ph) && Lit(pxV) > litNo;
            // unknown: no bar fill, nothing red; hot (104 °C): its temperature in red, the bar as before
            if (v == 2) vramDrawn &= Tinted(pxV, d.winW, bandTop, bandEnd, 'g') == 0 && Tinted(pxV, d.winW, bandTop, bandEnd, 'r') == 0;
            else vramDrawn &= Tinted(pxV, d.winW, bandTop, bandEnd, 'g') > 150 && Tinted(pxV, d.winW, bandTop, bandEnd, 'r') > 10;
            // 104 °C in red, 93 °C in orange and no red (kVramHot: hot at 100, warm from 90)
            if (v == 3) vramColours &= Tinted(pxV, d.winW, bandTop, bandEnd, 'R') > 10 && Tinted(pxV, d.winW, bandTop, bandEnd, 'o') == 0;
            if (v == 4) vramColours &= Tinted(pxV, d.winW, bandTop, bandEnd, 'o') > 10 && Tinted(pxV, d.winW, bandTop, bandEnd, 'R') == 0;
        }
        // fewer drives later: the panel is as high as its rows, within the room
        fill(2, 2);
        d.ComputePanelRect();
        rows = d.PanelRowsNow();
        const float ph2 = d.panelRect.bottom - d.panelRect.top;
        fits &= rows.driveRows == rowsOf(2) && rows.drivesHidden == 0 && rows.vram && Near(ph2, std::round(DockPanelHeightDip(rowsOf(2), rowsOf(2), true) * d.scale)) && ph2 < ph;
        // more drives than the window was built for: the drives take every row there is beside the VRAM block (the
        // modules then have none: counted, as the drives left over)
        fill(11, 2);
        rows = d.PanelRowsNow();
        const float roomDip = d.panelH / d.scale + 0.5f;
        const int fitDrives = (int)((roomDip - (DockPanelHeightDip(0, 1, true) - 27)) / 27);   // the block, the divider, the footer
        counted &= rows.vram && rows.driveRows == std::min(rowsOf(11), fitDrives) && rows.drivesShown == std::min(11, rows.driveRows * cols) &&
                   rows.drivesShown + rows.drivesHidden == 11 && rows.drivesHidden > 0 && rows.dimmsShown + rows.dimmsHidden == 2 &&
                   rows.height <= roomDip && (rows.dimmRows == 0 || DockPanelHeightDip(rows.dimmRows, rows.driveRows, true) <= roomDip);
        // a window built without memory rows (no modules known), memory temperatures later (HWiNFO): the drives
        // keep their rows, the memory is counted ("RAM 외 2개") - not the drives pushed out by it
        const float keptH = d.panelH;
        d.panelH = std::round(DockPanelHeightDip(0, rowsOf(3), true) * d.scale);
        fill(3, 2);
        rows = d.PanelRowsNow();
        drivesFirst &= rows.vram && rows.driveRows == rowsOf(3) && rows.drivesShown == 3 && rows.drivesHidden == 0 && rows.dimmRows == 0 && rows.dimmsHidden == 2;
        drawn &= Lit(Pixels((int)d.winW, (int)d.winH, [&] { d.ComputePanelRect(); d.DrawPanel(); })) > 2000;
        // a window built without room for the VRAM block (no GPU with memory known then), the block later: left
        // out - the drives and the memory rows keep their rows and the panel stays within its room
        d.panelVramRoom = false;
        d.panelH = std::round(DockPanelHeightDip(0, rowsOf(3), false) * d.scale);
        rows = d.PanelRowsNow();
        vramLater &= !rows.vram && rows.driveRows == rowsOf(3) && rows.drivesHidden == 0 && rows.height <= d.panelH / d.scale + 0.5f;
        const auto pxLeftOut = Pixels((int)d.winW, (int)d.winH, [&] { d.ComputePanelRect(); d.DrawPanel(); });
        drawn &= Lit(pxLeftOut) > 2000;
        // ...and its header does not name a block it does not show: the frame of a PC without one, pixel for pixel
        // (where the block is shown the frames differ: vramDrawn)
        fill(3, 2, 0);
        vramLater &= !pxLeftOut.empty() && pxLeftOut == Pixels((int)d.winW, (int)d.winH, [&] { d.ComputePanelRect(); d.DrawPanel(); });
        fill(3, 2);
        // ...a room that would hold the block with one drive row (four drive rows' on a vertical dock; five drives
        // and two modules'): still every row as without it, nothing counted as left out
        for (auto [nDrives, nDimms] : { std::pair{ 4, 0 }, std::pair{ 5, 2 }, std::pair{ 3, 1 } }) {
            d.panelH = std::round(DockPanelHeightDip(rowsOf(nDimms), rowsOf(nDrives), false) * d.scale);
            fill(nDrives, nDimms);
            rows = d.PanelRowsNow();
            vramLater &= !rows.vram && rows.driveRows == rowsOf(nDrives) && rows.dimmRows == rowsOf(nDimms) && rows.drivesHidden == 0 && rows.dimmsHidden == 0 &&
                         Near(rows.height, DockPanelHeightDip(rowsOf(nDimms), rowsOf(nDrives), false));
        }
        // ...and shown where the room holds it beside everything else (fewer rows now than the window was built for)
        d.panelH = std::round(DockPanelHeightDip(rowsOf(2), rowsOf(5), false) * d.scale);
        fill(1, 0);
        rows = d.PanelRowsNow();
        vramLater &= rows.vram && rows.driveRows == 1 && rows.drivesHidden == 0 && Near(rows.height, DockPanelHeightDip(0, 1, true));
        d.panelVramRoom = true;
        // a screen under 1440 px (scale 0.72 - 1): half a pixel of the room's rounding is more than half a DIP there,
        // and the rows the room was made for are all shown all the same (1680x1050: 349 DIP -> 254 px = 348.3 DIP;
        // 1366x768: 484 DIP -> 348 px = 483.3 DIP)
        const float keptScale = d.scale;
        for (float sc : { 1050 / 1440.0f, 0.72f, 0.75f, 0.9f }) for (auto [nDrives, nDimms] : { std::pair{ 2, 2 }, std::pair{ 5, 2 }, std::pair{ 1, 0 }, std::pair{ 6, 4 } }) {
            d.scale = sc;
            d.panelH = std::round(DockPanelHeightDip(rowsOf(nDimms), rowsOf(nDrives), true) * sc);
            fill(nDrives, nDimms);
            rows = d.PanelRowsNow();
            smallScreen &= rows.vram && rows.driveRows == rowsOf(nDrives) && rows.dimmRows == rowsOf(nDimms) && rows.drivesHidden == 0 && rows.dimmsHidden == 0;
            fill(nDrives + cols, nDimms);   // one drive row more than the room: counted, not squeezed in
            rows = d.PanelRowsNow();
            smallScreen &= rows.driveRows + rows.dimmRows <= rowsOf(nDrives) + rowsOf(nDimms) && rows.drivesHidden + rows.dimmsHidden > 0;
        }
        d.scale = keptScale;
        d.panelH = keptH;
        fill(3, 2);
        // opened before its first sample: the room kept, "측정 중…"
        g_sensors.panel.sampled = false;
        d.ComputePanelRect();
        const float ph3 = d.panelRect.bottom - d.panelRect.top;
        waiting &= d.PanelRowsNow().driveRows == 0 && !d.PanelRowsNow().vram && Near(ph3, d.panelH);
        drawn &= Lit(Pixels((int)d.winW, (int)d.winH, [&] { d.DrawPanel(); })) > 500;
    }
    Check(sized, "the window keeps room for the drives, the modules and the VRAM block known when it is built (450 x 403 DIP for five drives, two modules, a GPU with memory; 320 DIP wide, one a row, on a vertical dock)");
    Check(inside, "the panel stays inside the dock window on every edge at 1.0 and 1.25");
    Check(whole, "...on whole pixels (its cards and bars sharp at 1.25)");
    Check(without, "a GPU without memory of its own: no VRAM block, the panel as high as before it (332 DIP for the same rows)");
    Check(vramDrawn, "the VRAM block is drawn (more than the panel without it), also with its temperature unknown and hot, in the same room");
    Check(vramColours, "its temperature is red at 104 °C and orange, not red, at 93 °C (hot at 100, warm from 90)");
    Check(fits, "with fewer drives the panel is only as high as its rows");
    Check(counted, "more drives than the room: the drives take every row beside the VRAM block, the rest counted");
    Check(drivesFirst, "memory rows that came later do not push the drives out: the modules counted instead");
    Check(vramLater, "a VRAM block that came later than the window's room for it pushes no drive or memory row out: shown only beside them all");
    Check(smallScreen, "on a screen under 1440 px (scale 0.72 - 0.9) the rows the room was made for are all shown, one more is counted");
    Check(waiting, "before its first sample the panel keeps the room it was built with");
    Check(drawn, "the panel is drawn on every edge at 1.0 and 1.25 (cards, rows, footer)");
    g_sensors.live = false; g_sensors.d = {}; g_sensors.panel = PanelData{};
    g_settings = Settings{};
}

// ---- pixel hashes (a characterization) -----------------------------------------------------------------------
// What the dock draws today, pinned so a change of Dock::Render (glass, apps, the bar's contents, what goes
// over them), DrawPanel, the shadowed texts, the bounce or DrawButtonIcon shows: whole frames of the production Render
// (the glass over a fixed backdrop, icons, tray icons, cells, clock, tooltip, panel) at 1.0, 1.25 and 1.5 - screens
// with short sides of 1440, 1800 and 2160 px: the dock's scale comes from them, not from this PC's DPI - on the
// bottom and left edges (a few on the top and right too), the "PC 상태" panel and the cells' shadowed texts on their
// own, the hops frame by frame on a fake time, every button icon in its states and the battery icon.
// Each picture is one line "HASH: dock_pixels/<scene> <hex>" (FNV-1a over its premultiplied BGRA pixels). They hold
// the fonts and the GPU driver of the PC they ran on: tools\refactor\gate.py compares them on one PC before and
// after a change; they are not expected values written here. What can be written here - the hop heights, the
// helpers' texts and colours - is checked as usual. Nothing reads the clock or the locale: the clock's text is
// fixed (wmain), the hops run on a fake time, no tooltip asks Windows for today's date. Every picture of Direct2D
// alone is drawn twice and compared (a picture that differs between two draws of one scene would not make a hash);
// a whole frame, with the glass of the GPU's pixel shader, three times in a row and compared in all 8 bits a channel
// (PixFrameSteady): a frame whose three draws are not alike is not hashed but printed as one line
// "UNSTABLE: dock_pixels/<scene> (<n> pixels, <x>,<y>)" - the gate leaves such a scene out on both sides.
// --snap: each picture also as data\snap\<scene>.bmp (over grey) next to the exe, to see what a changed hash shows
// (all of them about 2 GB); --snap=<text>: only the pictures whose name has <text> in it.
static bool pixSnap = false;
static std::string pixSnapOnly;
static int pixDrawn = 0, pixBlank = 0, pixDup = 0, pixUnstable = 0;
static int pixWhole = 0, pixWholeUnstable = 0;
static std::map<std::string, uint64_t> pixHashes;
static std::set<std::string> pixNames;   // every picture's name, hashed or UNSTABLE
static std::string pixAt;                // the scene being drawn (for the frame-state check's message)
static bool pixFlagsOk = true;           // what every frame of PixFrame left in the dock's state (see there)
static int pixFlagFrames = 0;
static const HMONITOR kUhd = (HMONITOR)(uintptr_t)0x2004;   // 3840x2160: scale 1.5

static uint64_t PixHash(const std::vector<uint32_t>& px, int w, int h) {
    uint64_t x = 14695981039346656037ULL;
    auto mix = [&](uint32_t v) { x ^= v; x *= 1099511628211ULL; };
    mix((uint32_t)w); mix((uint32_t)h);
    for (uint32_t p : px) mix(p);
    return x ^ (x >> 31);
}
static void PixSave(const std::string& name, const std::vector<uint32_t>& px, int w, int h) {
    const std::wstring dir = AppDataDir() + L"\\snap";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<uint32_t> out(px.size());
    for (size_t i = 0; i < px.size(); ++i) {   // premultiplied: over 50 % grey
        const uint32_t p = px[i], a = p >> 24;
        uint32_t o = 0xff000000;
        for (int sh = 0; sh < 24; sh += 8) o |= std::min(255u, ((p >> sh) & 255) + 128 * (255 - a) / 255) << sh;
        out[i] = o;
    }
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + (DWORD)(out.size() * 4);
    std::ofstream f(dir + L"\\" + std::wstring(name.begin(), name.end()) + L".bmp", std::ios::binary);
    f.write((const char*)&fh, sizeof(fh)); f.write((const char*)&ih, sizeof(ih));
    f.write((const char*)out.data(), (std::streamsize)(out.size() * 4));
}
// One picture: drawn twice (a and b, the same scene), its hash printed
static void PixScene(const std::string& name, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int w, int h) {
    const bool drawn = !a.empty() && a.size() == (size_t)w * h && Lit(a) > 0;
    pixDrawn += drawn;
    if (!drawn) { ++pixBlank; printf("  not drawn: %s\n", name.c_str()); }
    if (a != b) {
        ++pixUnstable;
        int n = 0, x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) if (a[i] != b[i]) {
            ++n; const int x = (int)(i % w), y = (int)(i / w);
            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
        }
        const size_t at = (size_t)y0 * w + x0;
        printf("  drawn twice, different pixels: %s (%d pixels in %d,%d - %d,%d of %dx%d, e.g. %08x / %08x)\n", name.c_str(), n, x0, y0, x1, y1, w, h,
               at < a.size() ? a[at] : 0u, at < b.size() ? b[at] : 0u);
    }
    const uint64_t v = PixHash(a, w, h);
    if (!pixNames.insert(name).second) ++pixDup;
    pixHashes[name] = v;
    printf("HASH: dock_pixels/%s %016llx\n", name.c_str(), (unsigned long long)v);
    if (pixSnap && drawn && (pixSnapOnly.empty() || name.find(pixSnapOnly) != std::string::npos)) PixSave(name, a, w, h);
}
template <class F> static void PixDraw(const std::string& name, int w, int h, F draw) {
    const auto a = Pixels(w, h, draw), b = Pixels(w, h, draw);
    PixScene(name, a, b, w, h);
}
// A whole frame (PixFrameSteady's): hashed when its three draws in a row were alike, otherwise one UNSTABLE line
struct PixSteady { std::vector<uint32_t> px; bool stable = true; int diff = 0, x = -1, y = -1; };
static void PixSceneWhole(const std::string& name, const PixSteady& s, int w, int h) {
    ++pixWhole;
    if (s.stable) { PixScene(name, s.px, s.px, w, h); return; }
    ++pixWholeUnstable;
    const bool drawn = !s.px.empty() && Lit(s.px) > 0;
    pixDrawn += drawn;
    if (!drawn) { ++pixBlank; printf("  not drawn: %s\n", name.c_str()); }
    if (!pixNames.insert(name).second) ++pixDup;
    printf("UNSTABLE: dock_pixels/%s (%d pixels, %d,%d)\n", name.c_str(), s.diff, s.x, s.y);
}
// Two pictures alike within one step a channel (the glass pass's rounding, PixFrameSteady): for the checks that compare
// two whole frames of one run
static bool PixClose(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i])
        for (int sh = 0; sh < 32; sh += 8) if (std::abs(int((a[i] >> sh) & 255) - int((b[i] >> sh) & 255)) > 1) return false;
    return true;
}

// One frame of the production Dock::Render into a texture of this test's instead of the swap chain's buffer (Render
// clears it, draws the glass passes and then Direct2D into it as into its own), read back (premultiplied BGRA). The
// texture is magenta before (kPixFill): what Render clears is cleared by Render, not by a new texture's zeros. What the
// frame leaves in the dock's state is checked every time (pixFlagsOk): dirty off; where the glass is drawn the backdrop's
// blur done (backdropDirty off) and the luminance probe's frame counted once, elsewhere both as they were;
// clearedWhileHidden on only for a hidden dock with nothing peeking.
static const uint32_t kPixFill = 0xffff00ffu;
static std::vector<uint32_t> PixFrame(Dock& d) {
    RTTexture rt;
    if (!rt.Create(d.winW, d.winH, true) || !rt.rtv) return {};
    ComPtr<IDXGISurface> surf; ComPtr<ID2D1Bitmap1> bmp;
    const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt.tex.As(&surf)) || FAILED(g_gfx.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &bmp))) return {};
    { const float fill[4] = { 1, 0, 1, 1 }; g_gfx.ctx->ClearRenderTargetView(rt.rtv.Get(), fill); }
    bool peeking = d.sbPeek.x > 0.001f;
    for (auto& s : d.slots) if (s.peek.x > 0.001f) peeking = true;
    const bool shown = d.reveal.x > 0.002f, glass = shown && (d.capture.gotFirstFrame || d.glassKept);
    const int lum0 = d.lumFrame; const bool blur0 = d.backdropDirty;
    std::swap(d.rtv, rt.rtv); std::swap(d.target, bmp);
    d.dirty = true; d.Render();
    std::swap(d.rtv, rt.rtv); std::swap(d.target, bmp);
    ++pixFlagFrames;
    const bool flags = !d.dirty && d.lumFrame == lum0 + (glass ? 1 : 0) && d.backdropDirty == (glass ? false : blur0) &&
                       d.clearedWhileHidden == (!shown && !peeking);
    if (!flags && pixFlagsOk) printf("  after a frame of %s: dirty %d, backdropDirty %d (before %d), lumFrame %+d, clearedWhileHidden %d (glass %d, shown %d, peeking %d)\n",
                                     pixAt.c_str(), (int)d.dirty, (int)d.backdropDirty, (int)blur0, d.lumFrame - lum0, (int)d.clearedWhileHidden, (int)glass, (int)shown, (int)peeking);
    pixFlagsOk &= flags;
    D3D11_TEXTURE2D_DESC desc; rt.tex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_gfx.d3d->CreateTexture2D(&desc, nullptr, &st))) return {};
    g_gfx.ctx->CopyResource(st.Get(), rt.tex.Get());
    if (FAILED(g_gfx.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return {};
    std::vector<uint32_t> px((size_t)d.winW * d.winH);
    for (int y = 0; y < d.winH; ++y) memcpy(&px[(size_t)y * d.winW], (const BYTE*)m.pData + (size_t)y * m.RowPitch, (size_t)d.winW * 4);
    g_gfx.ctx->Unmap(st.Get(), 0);
    return px;
}
// A whole frame for its hash: drawn three times in a row and compared in all 8 bits a channel; a frame whose three draws
// are not alike is not hashed: one UNSTABLE line instead, its count of pixels and the first. A whole frame is not always
// exact on a GPU (seen on a test PC, 11-20): the glass pass (gfx.cpp's PSGlass) gave one pixel of the bar's
// rounded corner one step more or less in a channel, about one frame in thirty, in whichever scene; in
// glass_frosted_s1.00_bottom one pixel (1675,531) is ...a1... in the first draw and ...a2... after when the test runs
// alone (UNSTABLE every time), and ...a1... in all three at times while other programs use the GPU (hashed then: a value
// the gate can see change without any change of the code); the icons drawn by Direct2D in magnify_s1.00_right came out
// one step different in nine pixels in the third draw (with no glass too; seen in earlier runs, not since the
// scenes were added). No bits are dropped, no draw outvotes another and no frame is drawn again until three agree: a
// pixel one step apart crosses any mask, and a vote or a redraw can come out either way from run to run.
static PixSteady PixFrameSteady(Dock& d) {
    PixSteady r;
    auto a = PixFrame(d), b = PixFrame(d), c = PixFrame(d);
    if (a.empty() || a.size() != b.size() || a.size() != c.size()) return r;   // not drawn (empty): PixScene says so
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i] || a[i] != c[i]) {
        if (!r.diff++) { r.x = (int)(i % d.winW); r.y = (int)(i / d.winW); }
    }
    r.px = std::move(c);
    r.stable = r.diff == 0;
    return r;
}

// A picture for the icons: a disc of stripes in colours of its seed (the app icons' bitmaps; the tray icons'), or a
// white shape (a tray icon drawn for the taskbar's theme, which the dock tints in its ink)
static ComPtr<ID2D1Bitmap1> PixIcon(int seed, int n, bool glyph = false) {
    std::vector<uint32_t> px((size_t)n * n);
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        const float dx = x - (n - 1) * 0.5f, dy = y - (n - 1) * 0.5f;
        const bool in = dx * dx + dy * dy < (n * 0.47f) * (n * 0.47f);
        uint32_t r = (x * 4 + seed * 40) & 255, g = (y * 4 + seed * 70) & 255, b = ((x + y) * 2 + seed * 20) & 255;
        if (glyph) r = g = b = 255;
        px[(size_t)y * n + x] = !in ? 0 : glyph && ((x / 4 + y / 4) & 1) ? 0 : 0xff000000 | (r << 16) | (g << 8) | b;
    }
    ComPtr<ID2D1Bitmap1> b;
    const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    g_gfx.dc->CreateBitmap(D2D1::SizeU(n, n), px.data(), n * 4, bp, &b);
    return b;
}
// seven pinned apps (slots 1 - 7 after the start button): bitmaps but on the third and sixth (drawn as grey squares),
// the first, second and fourth running
static void PixApps() {
    Apps(7);
    for (int i = 0; i < 7; ++i) {
        auto& a = *g_apps.items[i];
        if (i != 2 && i != 5) a.icon = PixIcon(i, 64);
        if (i == 0 || i == 1 || i == 3) a.windows = { (HWND)(uintptr_t)(0x7100 + i) };
    }
}
// the taskbar's tray icons as the tray host would give them: three, the second a glyph (tinted in the ink)
static const uint64_t kPixTray[3] = { 0x5101, 0x5102, 0x5103 };
static void PixTrayBitmaps() {
    for (int i = 0; i < 3; ++i) {
        auto& e = g_notifyBitmaps[kPixTray[i]];
        const int n = i == 2 ? 24 : 32;
        e.bmp = PixIcon(10 + i, n, i == 1); e.imageVersion = 0; e.w = e.h = n; e.tint.Reset();
    }
}
static std::vector<Dock::NotifyCell> PixTrayCells() {
    std::vector<Dock::NotifyCell> cells(3);
    for (int i = 0; i < 3; ++i) { cells[i].key = kPixTray[i]; cells[i].tip = L"Tray icon " + std::to_wstring(i + 1); cells[i].glyph = i == 1; }
    return cells;
}
// The sensors: what the cell and the panel show, by kind
// (kPixLoads: loads at the bars' ends - 0.4 % (the fill's least width), none, over 100 %)
enum PixSensorKind { kPixMain, kPixHot, kPixWarm, kPixNoVram, kPixVramUnknown, kPixDrives11, kPixDimms4, kPixNoSample, kPixOffline,
                     kPixCpuHint, kPixGpuNone, kPixLoads, kPixKinds };
static const char* const kPixSensorNames[kPixKinds] = { "main", "hot", "warm", "novram", "vram_unknown", "drives11", "dimms4",
                                                        "nosample", "offline", "cpu_hint", "gpu_none", "loads" };
static void PixSensors(int kind) {
    g_sensors.live = kind != kPixOffline;
    g_sensors.d = {};
    auto& d = g_sensors.d;
    d.flags = LD_HAS_CPU_TEMP | LD_HAS_GPU | LD_HAS_GPU_LOAD;
    if (kind == kPixCpuHint) d.flags &= ~LD_HAS_CPU_TEMP;
    if (kind == kPixGpuNone) d.flags &= ~(LD_HAS_GPU | LD_HAS_GPU_LOAD);
    const bool hot = kind == kPixHot, warm = kind == kPixWarm;
    d.cpuTemp = hot ? 97.f : warm ? 84.f : 62.f; d.cpuLoad = 37; d.gpuTemp = hot ? 88.f : warm ? 76.f : 48.f; d.gpuLoad = 71;
    d.ramUsed = 14.2f; d.ramTotal = 31.9f; d.ramLoad = 44.5f;
    if (kind == kPixLoads) { d.cpuLoad = 0.4f; d.gpuLoad = 0; d.ramLoad = 104; }
    wcscpy_s(d.cpuName, L"AMD Ryzen 7 7800X3D 8-Core Processor"); wcscpy_s(d.gpuName, L"NVIDIA GeForce RTX 4060 Ti");   // never "GPU": the adapter's name
    PanelData p;
    const wchar_t* models[] = { L"Samsung SSD 990 PRO 2TB", L"WD Blue SN580", L"Crucial P3" };
    const int drives = kind == kPixDrives11 ? 11 : 3;
    for (int i = 0; i < drives; ++i) {
        sensorlogic::DriveRow r; r.letter = wchar_t(L'C' + i); r.disk = i; r.system = i == 0; r.model = models[i % 3];
        r.temp = i % 3 == 1 ? NAN : hot ? 72.f + i : warm ? 63.f + i : 41.f + i;
        p.drives.push_back(r);
    }
    const int dimms = kind == kPixDimms4 ? 4 : 2;
    for (int i = 0; i < dimms; ++i) {
        sensorlogic::DimmRow r; r.slot = i % 2 ? L"B2" : L"A2"; r.label = sensorlogic::DimmLabel(i + 1, r.slot);
        r.temp = i == 1 ? NAN : hot ? 86.f : warm ? 78.f : 32.8f + i;
        p.dimms.push_back(r);
    }
    p.drivesWithTemp = sensorlogic::CountWithTemp(p.drives); p.modules = dimms; p.dimmsWithTemp = sensorlogic::CountWithTemp(p.dimms);
    p.sampled = kind != kPixNoSample;
    if (kind != kPixNoVram && kind != kPixGpuNone) {
        p.vram.present = true; p.vram.totalGB = 16.0f;
        if (kind != kPixVramUnknown) { p.vram.usedGB = 6.2f; p.vram.load = kind == kPixLoads ? 0.3f : 38.75f; p.vram.temp = hot ? 104.f : warm ? 93.f : 62.f; }
    }
    g_sensors.panel = p;
    g_sensors.alertPending = false;
}
// The battery: 0 none (a desktop), 1 on battery, 2 charging, 3 low, 4 critical, 5 battery saver
static void PixBattery(int kind) {
    battery::State b;
    if (kind) {
        b.present = true; b.percent = kind == 2 ? 64 : kind == 3 ? 12 : kind == 4 ? 5 : kind == 5 ? 45 : 57;
        b.charging = b.plugged = kind == 2; b.saver = kind == 5; b.secondsLeft = kind == 2 ? -1 : 7200;
    }
    g_sys.SetBattery(b);
}

struct PixDock {
    Dock* d; Edge edge; std::string tag;
    std::vector<uint32_t> dark, light;   // the backdrops
    Settings base;                       // the settings it was built with (PixBaseSettings, or the other docks')
};
static Settings PixBaseSettings(Edge edge) {
    Settings s; s.edge = edge;
    s.trayButtons = "desktop,theme,oled,awake,screenshot,quiet,trash,downloads,audio,sff";
    return s;
}
static void PixInk(Dock& d, float k) { for (int i = 0; i < 16; ++i) d.lumCol[i] = d.inkTarget[i] = d.inkCol[i] = k; }
static void PixBackdrop(PixDock& f, bool light) {
    g_gfx.ctx->UpdateSubresource(f.d->backdrop.tex.Get(), 0, nullptr, (light ? f.light : f.dark).data(), f.d->winW * 4, 0);
    f.d->capture.gotFirstFrame = true; f.d->backdropDirty = true;
}
// The resting dock every scene starts from: shown, nothing hovered, pressed, dragged, magnified or bouncing, white ink
// over the dark backdrop, the fixture's sensors, battery, volume and network
static void PixRest(PixDock& f) {
    Dock& d = *f.d;
    g_settings = f.base;
    PixSensors(kPixMain); PixBattery(1);
    themeLight = false; oledOn = false; features::SetQuiet(0); Dock::debugForceShow = false;
    g_sys.net = 2; g_sys.volume = 0.62f; g_sys.muted = false;
    g_apps.attentionEvents.clear();
    for (auto& a : g_apps.items) { a->attention = false; a->launchStarted = 0; a->launchDock = -1; }
    d.reveal.snap(1); d.hideOff = 0; d.magAmount.snap(0); d.hoverU = 0; d.pinSide = 0; d.pinBlend.snap(0); d.pinOff = 0;
    d.tooltipAlpha.snap(0); d.tooltipText.clear(); d.tooltipU = d.tooltipV = 0; d.panelAlpha.snap(0);
    d.sbPeek.snap(0); d.sbOff = 0; d.sbCount = 0; d.sbStart = 0; d.sbHidden = false;
    d.dragging = false; d.pressSlot = -1; d.pressKind = Dock::HitNone; d.dropRemove = false; d.dropIndex = -1;
    d.hover.kind = Dock::HitNone; d.hover.slot = -1; d.hover.button = ButtonId::Count; d.hover.notify = -1;
    d.trailingGap.snap(0); d.cursorU = -1; d.cursorV = 1e9f; d.widgetWidth.snap(0);
    for (auto& s : d.slots) {
        s.appear.snap(1); s.shrink.snap(1); s.press.snap(0); s.peek.snap(0); s.gapBefore.snap(0); s.ghost = false;
        s.bounceStart = 0; s.bounceCount = 0; s.bounceLoop = false; s.bounceHiddenMode = false; s.bounceOff = 0;
    }
    PixInk(d, 0);
    d.notifyCells = PixTrayCells(); d.notifyVersion = 0;   // what SyncNotifyIcons would keep (trayIcons off: version 0)
    PixBackdrop(f, false);
    d.glassKept = false;
    // (clearedWhileHidden is left as the last frame left it: Render's own to keep, PixFrame checks it)
    d.Layout(); d.ComputePanelRect();
}
static std::string PixName(const char* scene, const PixDock& f) { return std::string(scene) + "_" + f.tag; }
// A whole frame of a scene: from rest, set, laid out, drawn three times (PixFrameSteady)
static std::vector<uint32_t> PixWhole(PixDock& f, const char* scene, const std::function<void(Dock&)>& set) {
    Dock& d = *f.d;
    PixRest(f);
    set(d);
    d.Layout(); d.ComputePanelRect();
    pixAt = PixName(scene, f);
    const auto a = PixFrameSteady(d);
    PixSceneWhole(pixAt, a, d.winW, d.winH);
    return a.px;
}

// The scenes of whole frames (Render's parts: glass, apps, the bar's contents and what goes over them)
struct PixSceneDef { const char* name; std::function<void(Dock&)> set; };
static std::vector<PixSceneDef> PixScenes(PixDock& f) {
    auto barTip = [](Dock& d) { return d.margin + d.barThick + 8 * d.scale - d.hideOff; };
    return {
        { "rest", [](Dock&) {} },
        { "light", [&f](Dock& d) { PixInk(d, 1); PixBackdrop(f, true); themeLight = true; } },   // dark ink over a light screen, light mode
        { "ink_mixed", [](Dock& d) { for (int i = 0; i < 16; ++i) d.lumCol[i] = d.inkTarget[i] = d.inkCol[i] = i / 15.f; } },
        { "glass_clear", [](Dock&) { g_settings.glassStyle = 0; } },
        { "glass_frosted", [](Dock&) { g_settings.glassStyle = 2; } },
        { "reveal_half", [](Dock& d) { d.reveal.snap(0.45f); d.hideOff = 0.55f * (d.margin + d.barThick + 34 * d.scale); } },
        { "magnify", [](Dock& d) { d.hoverU = d.slots[3].u; d.magAmount.snap(1); } },
        { "magnify_end", [](Dock& d) { d.hoverU = d.slots.back().u; d.magAmount.snap(0.6f); } },
        { "press", [](Dock& d) { d.slots[0].press.snap(1); d.slots[2].press.snap(0.7f); d.slots[3].press.snap(1); } },
        { "drag", [](Dock& d) {
            d.dragging = true; d.pressSlot = 2; d.cursorU = d.slots[5].u; d.cursorV = d.margin + d.barThick * 0.5f; d.slots[5].gapBefore.snap(1); } },
        { "drag_remove", [](Dock& d) {
            d.dragging = true; d.pressSlot = 4; d.cursorU = d.slots[2].u; d.cursorV = d.margin + d.barThick + d.base * 1.6f; d.dropRemove = true; } },
        { "attention", [](Dock&) { g_apps.items[1]->attention = true; g_apps.items[3]->attention = true; } },
        { "tooltip_app", [](Dock& d) {
            auto& s = d.slots[4];
            d.hover.kind = Dock::HitSlot; d.hover.slot = 4; d.tooltipText = s.app->name; d.tooltipU = s.u;
            d.tooltipV = d.margin + d.pad + s.size * s.weight + 10 * d.scale; d.tooltipAlpha.snap(1); } },
        { "tooltip_button", [barTip](Dock& d) {   // fading in: lifted
            d.hover.kind = Dock::HitButton; d.hover.button = ButtonId::Volume; d.tooltipText = L"볼륨 62%";
            d.tooltipU = d.Cell_(ButtonId::Volume).Mid(); d.tooltipV = barTip(d); d.tooltipAlpha.snap(0.5f); } },
        { "hover_clock", [barTip](Dock& d) {
            d.hover.kind = Dock::HitClock; d.tooltipText = L"2026년 10월 28일 수요일"; d.tooltipU = d.cells[Dock::HitClock].Mid();
            d.tooltipV = barTip(d); d.tooltipAlpha.snap(1); } },
        { "hover_sensor", [](Dock& d) { d.hover.kind = Dock::HitSensor; } },
        { "hover_battery", [](Dock& d) { d.hover.kind = Dock::HitBattery; } },
        { "hover_tray", [](Dock& d) { d.hover.kind = Dock::HitTray; } },
        { "hover_tray_icon", [barTip](Dock& d) {
            d.hover.kind = Dock::HitNotify; d.hover.notify = 1; d.tooltipText = d.notifyCells[1].tip;
            d.tooltipU = (d.notifyCells[1].u0 + d.notifyCells[1].u1) * 0.5f; d.tooltipV = barTip(d); d.tooltipAlpha.snap(1); } },
        { "sensor_hot", [](Dock&) { PixSensors(kPixHot); } },
        { "sensor_warm", [](Dock&) { PixSensors(kPixWarm); } },
        { "sensor_offline", [](Dock&) { PixSensors(kPixOffline); } },
        { "sensor_cpu_only", [](Dock&) { g_settings.showGpu = false; } },
        { "sensor_peek", [](Dock& d) { PixSensors(kPixHot); d.sbPeek.snap(1); } },
        { "battery_none", [](Dock&) { PixBattery(0); } },
        { "battery_charging", [](Dock&) { PixBattery(2); } },
        { "battery_low", [](Dock&) { PixBattery(3); } },
        { "battery_critical", [](Dock&) { PixBattery(4); } },
        { "battery_saver", [](Dock&) { PixBattery(5); } },
        { "buttons_on", [](Dock&) { themeLight = true; oledOn = true; features::SetQuiet(1e300); g_sys.net = 0; g_sys.muted = true; } },
        { "buttons_wired", [](Dock&) { g_sys.net = 1; g_sys.volume = 0.2f; } },
        { "panel", [](Dock& d) { d.panelAlpha.snap(1); } },
        { "panel_fading", [](Dock& d) { d.panelAlpha.snap(0.4f); } },
        { "panel_hot", [](Dock& d) { PixSensors(kPixHot); d.panelAlpha.snap(1); } },
        { "hidden_peek", [](Dock& d) { d.reveal.snap(0); d.hideOff = d.margin + d.barThick + 34 * d.scale; d.slots[4].peek.snap(0.9f); } },
        { "hidden_sensor_peek", [](Dock& d) {   // the overheat peek of a hidden dock: no glass, the peek card alone
            PixSensors(kPixHot); d.reveal.snap(0); d.hideOff = d.margin + d.barThick + 34 * d.scale; d.sbPeek.snap(1); d.sbHidden = true; } },
        { "widget_slot", [](Dock& d) { d.widgetWidth.snap(std::round(150 * d.scale)); } },   // FluentFlyout's slot before the start button, its separator
        // fading in or out with everything inside the window (vis 0.84): the icons, a pressed one, the running
        // and attention dots, the separator, the cells, the clock, the buttons; and as Update moves it out (hideOff)
        { "reveal_fade", [](Dock& d) { d.reveal.snap(0.6f); d.slots[2].press.snap(1); g_apps.items[3]->attention = true; } },
        { "reveal_rising", [](Dock& d) {
            d.reveal.snap(0.6f); d.hideOff = 0.4f * (d.margin + d.barThick + 34 * d.scale); d.slots[2].press.snap(1); g_apps.items[3]->attention = true; } },
        // what goes over the bar, in its order: the tooltip under the panel, the dragged icon over the clock cell
        { "panel_tooltip", [barTip](Dock& d) {
            d.hover.kind = Dock::HitClock; d.tooltipText = L"2026년 10월 28일 수요일"; d.tooltipU = d.cells[Dock::HitClock].Mid();
            d.tooltipV = barTip(d); d.tooltipAlpha.snap(1); d.panelAlpha.snap(0.6f); } },
        { "drag_over_clock", [](Dock& d) {
            d.dragging = true; d.pressSlot = 2; d.cursorU = d.cells[Dock::HitClock].Mid(); d.cursorV = d.margin + d.barThick * 0.5f; } },
        // icons coming and going: over-shooting in (1.15), half in, shrinking, a removed app's ghost fading out
        { "appearing", [](Dock& d) {
            d.slots[1].appear.snap(1.15f); d.slots[3].appear.snap(0.5f); d.slots[5].shrink.snap(0.6f); d.slots[6].ghost = true; d.slots[6].appear.snap(0.5f); } },
        // no captured frame yet: the backdrop kept (glassKept, the same as at rest) or none (no glass)
        { "glass_kept", [](Dock& d) { d.capture.gotFirstFrame = false; d.glassKept = true; } },
        { "no_glass", [](Dock& d) { d.capture.gotFirstFrame = false; d.glassKept = false; } },
        // a long tooltip at the dock's very end: drawn inside it
        { "tooltip_end", [barTip](Dock& d) {
            d.hover.kind = Dock::HitSlot; d.hover.slot = 0; d.tooltipText = L"A very long name of a window at the very end of the dock"; d.tooltipU = 0;
            d.tooltipV = barTip(d); d.tooltipAlpha.snap(1); } },
        { "widget_opening", [](Dock& d) { d.widgetWidth.snap(d.gap); } },   // the widget's separator fading in as its slot opens
        // the overheat card while the cell hops on a shown dock; the card's metric by its threshold, not its degrees (the
        // SSD at 69 of 70 over the CPU at 85 of 90); a card of a cool PC (orange all the same)
        { "sensor_peek_shown", [](Dock& d) { PixSensors(kPixHot); d.sbPeek.snap(0.5f); d.sbOff = d.base * 0.5f; } },
        { "sensor_peek_ssd", [](Dock& d) {
            auto& s = g_sensors.d; s.flags |= LD_HAS_SSD_TEMP; s.cpuTemp = 85; s.gpuTemp = 60; s.ssdTemp = 69; d.sbPeek.snap(1); } },
        { "sensor_peek_calm", [](Dock& d) { d.sbPeek.snap(1); } },
        // a hidden dock peeking an icon with the panel's alpha left up: no panel without the glass
        { "hidden_peek_panel", [](Dock& d) {
            d.reveal.snap(0); d.hideOff = d.margin + d.barThick + 34 * d.scale; d.slots[4].peek.snap(0.9f); d.panelAlpha.snap(1); } },
    };
}

// The shadowed texts (DrawShadowText, which the sensor cell and the battery's percentage use too): the clock,
// a glyph, the sensor cell and the battery cell drawn alone, cut out of the window around the tray, by ink and alpha
static void PixShadowTexts(PixDock& f) {
    Dock& d = *f.d;
    PixRest(f);
    g_gfx.EnsureFonts(d.scale);
    const float v0 = d.margin - d.hideOff, v1 = d.margin + d.barThick - d.hideOff;
    float u0 = d.cells[Dock::HitTray].u0, u1 = d.cells[Dock::HitTray].u1;
    for (int k : { Dock::HitClock, Dock::HitSensor, Dock::HitBattery }) { u0 = std::min(u0, d.cells[k].u0); u1 = std::max(u1, d.cells[k].u1); }
    const auto box = d.RectUV(u0, u1, v0, v1);
    const float m = 12 * d.scale;
    const float x0 = std::floor(box.left - m), y0 = std::floor(box.top - m);
    const int w = (int)(std::ceil(box.right + m) - x0), h = (int)(std::ceil(box.bottom + m) - y0);
    struct Case { const char* name; float ink, vis; int sensors, battery; };
    const Case cases[] = { { "ink0", 0, 1, kPixMain, 1 }, { "ink0.50", 0.5f, 1, kPixMain, 1 }, { "ink0.94", 0.94f, 1, kPixMain, 1 },
                           { "ink0.95", 0.95f, 1, kPixMain, 1 }, { "ink1", 1, 1, kPixMain, 1 }, { "ink0.50_vis0.60", 0.5f, 0.6f, kPixMain, 1 },
                           { "ink0.30_hot_low", 0.3f, 1, kPixHot, 3 }, { "ink0_charging", 0, 0.8f, kPixWarm, 2 } };
    for (const auto& c : cases) {
        PixInk(d, c.ink); PixSensors(c.sensors); PixBattery(c.battery);
        PixDraw(std::string("shadow_") + c.name + "_" + f.tag, w, h, [&] {
            g_gfx.dc->SetTransform(D2D1::Matrix3x2F::Translation(-x0, -y0));
            const auto& clk = d.cells[Dock::HitClock];
            if (!IsVertical(f.edge)) {
                const auto r = d.RectUV(clk.u0, clk.u1, v0, v1);
                const float mid = (r.top + r.bottom) * 0.5f;
                d.DrawShadowText(g_sys.timeText, d.ClockFont(), { r.left, r.top, r.right, mid }, clk.Mid(), c.vis);
                d.DrawShadowText(g_sys.dateText, d.ClockFont(true), { r.left, mid, r.right, r.bottom }, clk.Mid(), c.vis);
            } else d.DrawShadowText(g_sys.timeShort, d.ClockFont(), d.RectUV(clk.u0, clk.u1, v0, v1), clk.Mid(), c.vis);
            const auto& tray = d.cells[Dock::HitTray];
            d.DrawShadowText(L"", g_gfx.fmtGlyph.Get(), d.RectUV(tray.u0, tray.u1, v0, v1), tray.Mid(), c.vis * 0.85f);
            d.DrawSensorCell(v0, v1, c.vis);
            d.DrawBatteryCell(v0, v1, c.vis);
            g_gfx.dc->SetTransform(D2D1::Matrix3x2F::Identity());
        });
    }
}

// The "PC 상태" panel alone (DrawPanel's blocks), cut out at its rectangle, by what the sensors give and what the
// sensor settings hide, in the room the window was built with (the main fixture's)
static void PixPanels(PixDock& f) {
    Dock& d = *f.d;
    struct Case { std::string name; int kind; std::function<void()> set; float alpha; };
    std::vector<Case> cases;
    for (int k = 0; k < kPixKinds; ++k) cases.push_back({ kPixSensorNames[k], k, [] {}, 1 });
    cases.push_back({ "fading", kPixMain, [] {}, 0.5f });
    cases.push_back({ "gpu_hidden", kPixMain, [] { g_settings.showGpu = false; }, 1 });
    cases.push_back({ "ssd_hidden", kPixMain, [] { g_settings.showSsd = false; }, 1 });
    cases.push_back({ "vram_temp_hidden", kPixMain, [] { g_settings.showVramTemp = false; }, 1 });
    cases.push_back({ "pawnio", kPixCpuHint, [] { g_settings.sensorSource = 2; }, 1 });
    cases.push_back({ "temps_off", kPixMain, [] { g_settings.sensorSource = 0; }, 1 });
    for (const auto& c : cases) {
        PixRest(f);
        PixSensors(c.kind); c.set();
        d.panelAlpha.snap(c.alpha);
        d.ComputePanelRect();
        g_gfx.EnsureFonts(d.scale);
        const D2D1_RECT_F r = d.panelRect;
        const int w = (int)std::lround(r.right - r.left), h = (int)std::lround(r.bottom - r.top);
        PixDraw("pcpanel_" + c.name + "_" + f.tag, w, h, [&] {
            g_gfx.dc->SetTransform(D2D1::Matrix3x2F::Translation(-r.left, -r.top));
            d.DrawPanel();
            g_gfx.dc->SetTransform(D2D1::Matrix3x2F::Identity());
        });
    }
}

// The hops (one 0.56 s parabola for the icons and the sensor cell): Dock::Update on a fake time with the pointer
// off every monitor, the dock kept up (debugForceShow), the ink held; heights checked against 4·h·f·(1−f), frames hashed
struct PixHops { bool notify = true, ends = true, extend = true, launch = true, sensor = true, peek = true, peekEnds = true, sensorPeek = true, sensorPeekEnds = true; };
static void PixBounce(PixDock& f, PixHops& ok) {
    Dock& d = *f.d;
    const POINT away = { -32000, -32000 };
    auto update = [&](double now, float dt = 1 / 120.f) { PixInk(d, 0); d.Update(now, dt, away); };
    auto hop = [&](double start, double now, float k) {
        const double phase = (now - start) / 0.56;
        const float fr = float(phase - std::floor(phase));
        return d.base * k * 4 * fr * (1 - fr);
    };
    auto frame = [&](const std::string& scene) { pixAt = PixName(scene.c_str(), f); PixSceneWhole(pixAt, PixFrameSteady(d), d.winW, d.winH); };
    // a notification: three hops 0.9 of the icon size high
    PixRest(f); Dock::debugForceShow = true;
    auto& s = d.slots[2];   // the second app (running)
    const double t0 = 40;
    g_apps.attentionEvents.push_back({ s.app->id });
    update(t0);
    g_apps.attentionEvents.clear();
    ok.notify &= s.bounceStart == t0 && s.bounceCount == 3 && !s.bounceLoop && !s.bounceHiddenMode;
    for (double dt : { 0.07, 0.14, 0.28, 0.42, 0.63, 0.84, 1.26, 1.61 }) {
        update(t0 + dt);
        ok.notify &= Near(s.bounceOff, hop(t0, t0 + dt, 0.9f)) && s.bounceCount == 3;
        if (dt == 0.14 || dt == 0.28 || dt == 0.63) { char n[32]; sprintf_s(n, "bounce_notify_t%.2f", dt); frame(n); }
    }
    update(t0 + 0.56 * 3 + 0.01);
    ok.ends &= s.bounceCount == 0 && s.bounceOff == 0 && !s.bounceLoop;
    // ...and another during the second hop: as many more as asked from the hop under way (1.3 -> 2 + 3 - 1 = 4)
    const double t1 = 60;
    g_apps.attentionEvents.push_back({ s.app->id }); update(t1); g_apps.attentionEvents.clear();
    update(t1 + 0.56 * 1.3);
    g_apps.attentionEvents.push_back({ s.app->id }); update(t1 + 0.56 * 1.3 + 0.001); g_apps.attentionEvents.clear();
    ok.extend &= s.bounceCount == 4 && s.bounceStart == t1;
    update(t1 + 0.56 * 3.5);
    ok.extend &= s.bounceCount == 4 && Near(s.bounceOff, hop(t1, t1 + 0.56 * 3.5, 0.9f));
    update(t1 + 0.56 * 4 + 0.01);
    ok.extend &= s.bounceCount == 0 && s.bounceOff == 0;
    // a launch: small hops (0.5) until its window shows; the hop under way then ends, 0.9 high
    const double t2 = 80;
    auto& l = d.slots[5];   // the fifth app (not running)
    l.app->launchStarted = 1; l.app->launchDock = d.index;
    update(t2);
    ok.launch &= l.bounceLoop && l.bounceCount == 0 && l.bounceStart == t2;
    for (double dt : { 0.14, 0.28, 0.98 }) {
        update(t2 + dt);
        ok.launch &= l.bounceLoop && Near(l.bounceOff, hop(t2, t2 + dt, 0.5f));
        if (dt == 0.28) frame("bounce_launch_t0.28");
    }
    l.app->launchStarted = 0;
    update(t2 + 0.56 * 2.25);
    ok.launch &= !l.bounceLoop && l.bounceCount == 3 && Near(l.bounceOff, hop(t2, t2 + 0.56 * 2.25, 0.9f));
    update(t2 + 0.56 * 2.5);
    ok.launch &= Near(l.bounceOff, hop(t2, t2 + 0.56 * 2.5, 0.9f));
    frame("bounce_launch_end_t1.40");
    update(t2 + 0.56 * 3 + 0.01);
    ok.launch &= l.bounceCount == 0 && l.bounceOff == 0;
    // the sensor cell's overheat hop: three, 0.9 of the icon size
    const double t3 = 100;
    PixSensors(kPixHot); g_sensors.alertPending = true;
    update(t3);
    g_sensors.alertPending = false;
    ok.sensor &= d.sbCount == 3 && d.sbStart == t3 && !d.sbHidden;
    for (double dt : { 0.07, 0.28, 0.49, 0.70, 1.54 }) {
        update(t3 + dt);
        ok.sensor &= d.sbCount == 3 && Near(d.sbOff, hop(t3, t3 + dt, 0.9f));
        if (dt == 0.28 || dt == 0.70) { char n[32]; sprintf_s(n, "bounce_sensor_t%.2f", dt); frame(n); }
    }
    update(t3 + 0.56 * 3 + 0.01);
    ok.sensor &= d.sbCount == 0 && d.sbOff == 0;
    // a notification while the dock is hidden: the icon peeks out with its hops
    PixRest(f);
    d.reveal.snap(0); d.hideOff = d.margin + d.barThick + 34 * d.scale;
    const double t4 = 120;
    g_apps.attentionEvents.push_back({ s.app->id }); update(t4, 1 / 60.f); g_apps.attentionEvents.clear();
    ok.peek &= s.bounceHiddenMode && s.bounceCount == 3;
    for (int i = 1; i <= 25; ++i) update(t4 + i / 60.0, 1 / 60.f);
    ok.peek &= s.peek.x > 0.5f && Near(s.bounceOff, hop(t4, t4 + 25 / 60.0, 0.9f)) && d.reveal.x < 0.002f;
    frame("bounce_peek_t0.42");
    // ...its three over (1.68 s; 3.67 s here), the icon goes back behind the edge: no more peeking, the hidden mode off
    for (int i = 26; i <= 220; ++i) update(t4 + i / 60.0, 1 / 60.f);
    ok.peekEnds &= s.bounceCount == 0 && s.bounceOff == 0 && s.peek.target == 0 && s.peek.x < 0.01f && !s.bounceHiddenMode && d.reveal.x < 0.002f;
    // the overheat hop of a hidden dock: the card peeks out (sbHidden), hopping with the cell's hop, and goes back after the three
    PixRest(f);
    d.reveal.snap(0); d.hideOff = d.margin + d.barThick + 34 * d.scale;
    const double t5 = 140;
    PixSensors(kPixHot); g_sensors.alertPending = true;
    update(t5, 1 / 60.f);
    g_sensors.alertPending = false;
    ok.sensorPeek &= d.sbHidden && d.sbCount == 3 && d.sbStart == t5;
    for (int i = 1; i <= 25; ++i) update(t5 + i / 60.0, 1 / 60.f);
    ok.sensorPeek &= d.sbHidden && d.sbPeek.x > 0.5f && d.sbPeek.target == 1 && Near(d.sbOff, hop(t5, t5 + 25 / 60.0, 0.9f)) && d.sbOff > 0 && d.reveal.x < 0.002f;
    frame("bounce_sensor_peek_t0.42");
    for (int i = 26; i <= 220; ++i) update(t5 + i / 60.0, 1 / 60.f);
    ok.sensorPeekEnds &= d.sbCount == 0 && d.sbOff == 0 && d.sbPeek.target == 0 && d.sbPeek.x < 0.01f && !d.sbHidden && d.reveal.x < 0.002f;
    Dock::debugForceShow = false;
}

// The small buttons' icons (DrawButtonIcon, icon by icon): every button in each state it draws, at zoom 1 in a whole
// cell and 0.8 in a half one, with and without the shadow, in white and dark ink, at 1.0, 1.25 and 1.5; the battery icon
// in each of its colourings and fills
static void PixButtons() {
    ComPtr<ID2D1SolidColorBrush> brush;
    g_gfx.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);
    const D2D1_COLOR_F inks[2] = { D2D1::ColorF(1, 1, 1, 0.95f), D2D1::ColorF(0.07f, 0.07f, 0.09f, 0.86f) };   // Ink(u, 1) at 0 and 1
    struct State { std::string tag; IconState st; };
    auto states = [](ButtonId id) {
        std::vector<State> v;
        auto add = [&](const char* tag, auto f) { IconState s; f(s); v.push_back({ tag, s }); };
        switch (id) {
        case ButtonId::Network: add("none", [](IconState& s) { s.net = 0; }); add("wired", [](IconState& s) { s.net = 1; }); add("wifi", [](IconState& s) { s.net = 2; }); break;
        case ButtonId::Volume:
            add("muted", [](IconState& s) { s.muted = true; s.volume = 0.5f; });
            for (float vol : { 0.0f, 0.0099f, 0.01f, 0.2f, 0.339f, 0.34f, 0.5f, 0.669f, 0.67f, 1.0f }) {
                char t[16]; sprintf_s(t, "v%.4f", vol);
                IconState s; s.volume = vol; v.push_back({ t, s });
            }
            break;
        case ButtonId::Theme: add("dark", [](IconState& s) { s.light = false; }); add("light", [](IconState& s) { s.light = true; }); break;
        case ButtonId::Oled: add("off", [](IconState& s) { s.oled = false; }); add("on", [](IconState& s) { s.oled = true; }); break;
        case ButtonId::Awake: add("off", [](IconState& s) { s.awake = false; }); add("on", [](IconState& s) { s.awake = true; }); break;
        case ButtonId::Quiet: add("off", [](IconState& s) { s.quiet = false; }); add("on", [](IconState& s) { s.quiet = true; }); break;
        case ButtonId::Trash: add("empty", [](IconState& s) { s.trashFull = false; }); add("full", [](IconState& s) { s.trashFull = true; }); break;
        default: add("only", [](IconState&) {}); break;
        }
        return v;
    };
    bool statesDiffer = true;
    for (float sc : { 1.0f, 1.25f, 1.5f }) {
        g_gfx.EnsureFonts(sc);
        char st[16]; sprintf_s(st, "s%.2f", sc);
        const float cw = std::round(34 * sc), whole = std::round(64 * sc), half = std::round(32 * sc);
        for (int i = 0; i < kButtonCount; ++i) {
            const ButtonId id = (ButtonId)i;
            const auto list = states(id);
            const int w = (int)(cw * list.size()), h = (int)(4 * whole + 4 * half);
            auto draw = [&](int only) {   // only: one state's column (-1: all)
                float y = 0;
                for (float zoom : { 1.0f, 0.8f }) for (float shadow : { 0.0f, 0.3f }) for (const auto& ink : inks) {
                    const float ch = zoom == 1 ? whole : half;
                    for (size_t k = 0; k < list.size(); ++k) {
                        if (only >= 0 && (int)k != only) continue;
                        const D2D1_RECT_F r = { k * cw, y, (k + 1) * cw, y + ch };
                        DrawButtonIcon(id, g_gfx.dc.Get(), brush.Get(), g_gfx.fmtGlyph.Get(), r, sc, zoom, ink, shadow, list[k].st);
                    }
                    y += ch;
                }
            };
            PixDraw(std::string("button_") + Info(id).key + "_" + st, w, h, [&] { draw(-1); });
            // the states that should look different do (two volume states within one band draw alike)
            if (list.size() > 1 && id != ButtonId::Volume) {
                const auto a = Pixels(w, h, [&] { draw(0); });
                for (size_t k = 1; k < list.size(); ++k) {
                    auto b = Pixels(w, h, [&] { draw((int)k); });
                    statesDiffer &= !b.empty() && b != a;
                }
            }
        }
        // the battery icon: shadow 0 / 0.33 x white / dark ink, at the dock's icon size for this scale
        struct Bat { const char* tag; battery::State st; char colour; };   // colour: g green, s saver, l low, c critical, i the ink
        auto bat = [](int pct, bool charging, bool plugged, bool saver) { battery::State b; b.present = true; b.percent = pct; b.charging = charging; b.plugged = plugged; b.saver = saver; return b; };
        const Bat bats[] = { { "charging64", bat(64, true, true, false), 'g' }, { "charging8", bat(8, true, true, false), 'g' },
                             { "full100", bat(100, false, true, false), 'g' }, { "held80", bat(80, false, true, false), 'g' },
                             { "plugged_saver50", bat(50, false, true, true), 'g' }, { "battery57", bat(57, false, false, false), 'i' },
                             { "battery100", bat(100, false, false, false), 'i' }, { "saver45", bat(45, false, false, true), 's' },
                             { "battery21", bat(21, false, false, false), 'i' }, { "low20", bat(20, false, false, false), 'l' },
                             { "low_saver15", bat(15, false, false, true), 'l' }, { "low11", bat(11, false, false, false), 'l' },
                             { "critical10", bat(10, false, false, false), 'c' }, { "critical5", bat(5, false, false, false), 'c' },
                             { "sliver3", bat(3, false, false, false), 'c' }, { "empty0", bat(0, false, false, false), 'c' } };
        const float icon = std::max(1.0f, std::round(std::round(44 * sc) * 1.30f * 0.60f));
        const float iw = BatteryIconWidth(icon, sc), ih = BatteryIconHeight(icon, sc), pad = std::round(6 * sc);
        bool colours = true;
        for (const auto& b : bats) {
            PixDraw(std::string("battery_") + b.tag + "_" + st, (int)(4 * (iw + pad) + pad), (int)(ih + 2 * pad + sc), [&] {
                int k = 0;
                for (float shadow : { 0.0f, 0.33f }) for (const auto& ink : inks) {
                    const float x = pad + k++ * (iw + pad);
                    DrawBatteryIcon(g_gfx.dc.Get(), brush.Get(), { x, pad, x + iw, pad + ih }, sc, ink, shadow, b.st);
                }
            });
            for (const auto& ink : inks) {
                const auto c = BatteryColor(b.st, ink);
                const D2D1_COLOR_F want = b.colour == 'g' ? D2D1::ColorF(0.30f, 0.82f, 0.42f, ink.a) : b.colour == 's' ? D2D1::ColorF(1.0f, 0.80f, 0.26f, ink.a)
                                        : b.colour == 'l' ? D2D1::ColorF(1.0f, 0.66f, 0.22f, ink.a) : b.colour == 'c' ? D2D1::ColorF(1.0f, 0.34f, 0.30f, ink.a) : ink;
                colours &= c.r == want.r && c.g == want.g && c.b == want.b && c.a == want.a;
            }
        }
        if (sc == 1.0f) Check(colours, "dock pixels: the battery's colour - green on mains power, amber with battery saver, orange at 20 % and under, red at 10 % and under (on battery), otherwise the ink");
    }
    Check(statesDiffer, "dock pixels: each state of a button with states draws differently (network, theme, OLED, awake, quiet, trash)");
}

static void DockPixels() {
    printf("  dock pixels:\n");
    const size_t keepMonitors = monitors.size();
    monitors.push_back({ kUhd, { 0, 1800, 3840, 3960 }, false, false, "uhd-3840x2160" });
    // the helpers the shadowed texts and the panel share: their texts and colours
    {
        const auto n = D2D1::ColorF(0.2f, 0.3f, 0.4f, 0.5f);
        auto same = [](D2D1_COLOR_F a, D2D1_COLOR_F b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; };
        Check(TempStr(NAN) == L"--°" && TempStr(NAN, true) == L"--°C" && TempStr(62.4f) == L"62°" && TempStr(62.5f) == L"63°" &&
              TempStr(-0.5f) == L"-1°" && TempStr(99.6f, true) == L"100°C" && TempStr(0.f, true) == L"0°C",
              "dock pixels: TempStr - whole degrees rounded half away from zero, \"--°\" unknown, \"°C\" with the unit");
        Check(same(SevColor(2, n, 0.7f), D2D1::ColorF(1.0f, 0.34f, 0.30f, 0.7f)) && same(SevColor(1, n, 0.7f), D2D1::ColorF(1.0f, 0.66f, 0.22f, 0.7f)) &&
              same(SevColor(0, n, 0.7f), n) && same(SevColor(3, n, 0.7f), n),
              "dock pixels: SevColor - red when hot, orange when warm (at the alpha given), otherwise the colour given");
    }
    PixButtons();
    PixTrayBitmaps();
    PixHops hops;
    bool restAgain = true, hidden = true, hiddenAgain = true, differ = true, keptSame = true, lum = true, stacked = true;
    int docks = 0, others = 0;
    // a dock of the settings `base` on monitor m, its backdrops and its tag; false when it cannot be created
    auto make = [&](std::unique_ptr<Dock>& dock, PixDock& f, HMONITOR m, Edge edge, const Settings& base, const char* kind) {
        g_settings = base;
        PixApps(); PixSensors(kPixMain); PixBattery(1);   // the window's room: the main fixture's panel
        dock = std::make_unique<Dock>(m, 50 + docks + others, true);
        Dock& d = *dock;
        if (!d.Create()) return false;
        d.nextCaptureTry = std::numeric_limits<double>::max();
        char tag[48]; sprintf_s(tag, "%ss%.2f_%s", kind, d.scale, edge == Edge::Bottom ? "bottom" : edge == Edge::Left ? "left" : edge == Edge::Top ? "top" : "right");
        f.d = &d; f.edge = edge; f.tag = tag; f.base = base;
        f.dark.resize((size_t)d.winW * d.winH); f.light.resize(f.dark.size());
        for (int y = 0; y < d.winH; ++y) for (int x = 0; x < d.winW; ++x) {   // as RenderFixture's (dock_interaction_test), and a light one
            const unsigned grid = ((x / 80 + y / 80) & 1) ? 28 : 0;
            const unsigned r = 45 + grid + (90 * x / d.winW), g = 65 + grid + (80 * y / d.winH), b = 115 + grid;
            f.dark[(size_t)y * d.winW + x] = 0xff000000 | (r << 16) | (g << 8) | b;
            f.light[(size_t)y * d.winW + x] = 0xff000000 | ((r + 120) << 16) | ((g + 110) << 8) | (b + 100);
        }
        return true;
    };
    // the scenes `only` names (all when empty) as whole frames; the others checked against the resting dock (the first)
    auto scenesOf = [&](PixDock& f, std::initializer_list<const char*> only) {
        std::vector<uint32_t> rest;
        for (const auto& sc : PixScenes(f)) {
            bool want = only.size() == 0;
            for (const char* n : only) want |= !strcmp(sc.name, n);
            if (!want) continue;
            const auto px = PixWhole(f, sc.name, sc.set);
            if (!strcmp(sc.name, "rest")) rest = px;
            else if (!strcmp(sc.name, "glass_kept")) keptSame &= PixClose(px, rest);   // the backdrop kept: as with a captured frame
            else differ &= !PixClose(px, rest);
        }
        return rest;
    };
    for (HMONITOR m : { kDesktop, kLaptop, kUhd }) for (Edge edge : { Edge::Bottom, Edge::Left, Edge::Top, Edge::Right }) {
        std::unique_ptr<Dock> dock; PixDock f{};
        if (!make(dock, f, m, edge, PixBaseSettings(edge), "")) { Check(false, "dock pixels: fixture dock created"); continue; }
        ++docks;
        Dock& d = *dock;
        const bool side = edge == Edge::Top || edge == Edge::Right;   // a few scenes there; the rest on the bottom and left
        const auto rest = side ? scenesOf(f, { "rest", "magnify", "tooltip_button", "panel" }) : scenesOf(f, {});
        // hidden, nothing peeking: the frame is cleared (over the magenta PixFrame starts from), once - the next frames
        // leave the buffer as it is
        PixRest(f); d.reveal.snap(0); d.hideOff = d.margin + d.barThick + 34 * d.scale; d.Layout();
        pixAt = "hidden_" + f.tag;
        const auto none = PixFrame(d);
        hidden &= none.size() == (size_t)d.winW * d.winH && Lit(none) == 0 && d.clearedWhileHidden;
        for (int k = 0; k < 2; ++k) {
            const auto again = PixFrame(d);
            hiddenAgain &= again.size() == none.size() && std::all_of(again.begin(), again.end(), [](uint32_t p) { return p == kPixFill; });
        }
        // the luminance probe (the ink's): over a backdrop of one grey its 16 columns read that grey back, one frame late
        // (from 0.47 between the two inks' thresholds, 0.42 and 0.52)
        for (uint32_t grey : { 0x30u, 0xd0u }) {
            PixRest(f);
            pixAt = "lum_" + f.tag;
            const std::vector<uint32_t> one(f.dark.size(), 0xff000000u | grey * 0x010101u);
            g_gfx.ctx->UpdateSubresource(d.backdrop.tex.Get(), 0, nullptr, one.data(), d.winW * 4, 0);
            d.backdropDirty = true;
            for (float& v : d.lumCol) v = 0.47f;
            for (int k = 0; k < 3; ++k) PixFrame(d);
            for (float v : d.lumCol) lum &= std::fabs(v - grey / 255.f) < 0.01f;
        }
        if (!side) { PixShadowTexts(f); PixPanels(f); PixBounce(f, hops); }
        // the resting dock once more after every scene: nothing of them is left over
        PixRest(f);
        pixAt = "rest_again_" + f.tag;
        restAgain &= PixClose(PixFrameSteady(d).px, rest);
    }
    // two docks of other settings at 1.0, 1.25 and 1.5 on the bottom and left: small icons on a thin bar (iconSize 32,
    // dockThickness 75) and the large clock (clockSize 1). The small buttons are paired (zoom 0.8) on both: the clock's
    // two lines keep any bar 44 DIP or more (Metrics' padding), so the unpaired buttons (zoom 1) are not drawn today
    for (HMONITOR m : { kDesktop, kLaptop, kUhd }) for (Edge edge : { Edge::Bottom, Edge::Left }) for (int kind = 0; kind < 2; ++kind) {
        Settings base = PixBaseSettings(edge);
        if (kind == 0) { base.iconSize = 32; base.dockThickness = 75; } else base.clockSize = 1;
        std::unique_ptr<Dock> dock; PixDock f{};
        if (!make(dock, f, m, edge, base, kind == 0 ? "small_" : "clock_large_")) { Check(false, "dock pixels: fixture dock of other settings created"); continue; }
        ++others;
        scenesOf(f, { "rest", "tooltip_button", "hover_clock" });
        stacked &= dock->stackButtons && dock->barThick >= 44 * dock->scale;   // as laid out (Layout)
    }
    Check(docks == 12, "dock pixels: docks at 1.0, 1.25 and 1.5 on every edge");
    Check(others == 12 && stacked, "dock pixels: docks of small icons on a thin bar and of the large clock at 1.0, 1.25 and 1.5, bottom and left - the bar 44 DIP or more, the small buttons paired");
    Check(pixBlank == 0, "dock pixels: every hashed picture is drawn (read back, something on it)");
    Check(pixUnstable == 0, "dock pixels: every picture of Direct2D alone drawn twice in a row is the same, pixel for pixel");
    printf("  dock pixels: %d whole frames, %d UNSTABLE (not hashed)\n", pixWhole, pixWholeUnstable);
    Check(pixWhole > 300 && pixWholeUnstable * 10 <= pixWhole, "dock pixels: nine whole frames in ten or more come out alike in three draws in a row, 8 bits a channel (hashed; the others UNSTABLE)");
    Check(pixDup == 0, "dock pixels: every picture has a name of its own");
    Check(differ, "dock pixels: every scene of a whole frame differs from the resting dock (each scene shows what it sets)");
    Check(keptSame, "dock pixels: before a captured frame the backdrop kept (glassKept) draws the glass as at rest");
    Check(restAgain, "dock pixels: the resting dock drawn again after all its scenes is the same (nothing left over)");
    Check(hidden, "dock pixels: a hidden dock with nothing peeking draws a cleared frame");
    Check(hiddenAgain, "dock pixels: ...once: its next frames while hidden draw nothing (the buffer left as it is)");
    Check(pixFlagsOk && pixFlagFrames > 1000, "dock pixels: after every frame dirty is off, the glass's frames leave the blur done and count one luminance frame, clearedWhileHidden is on only for a hidden dock with nothing peeking");
    Check(lum, "dock pixels: the luminance probe reads back the grey of a backdrop of one grey (0x30, 0xd0), one frame late, on every edge");
    Check(hops.notify, "dock pixels: a notification's hops - 0.56 s each, 0.9 of the icon size high (4 h f (1 - f)), three");
    Check(hops.ends, "dock pixels: ...after the three the icon is back on the bar, the bounce over");
    Check(hops.extend, "dock pixels: a notification during the hops adds its three from the hop under way (1.3 -> 4 in all)");
    Check(hops.launch, "dock pixels: a launch hops 0.5 of the icon size high until its window shows, the hop under way then ends 0.9 high");
    Check(hops.sensor, "dock pixels: the sensor cell's overheat hop - three of 0.56 s, 0.9 of the icon size high");
    Check(hops.peek, "dock pixels: a notification while the dock is hidden peeks the icon out, hopping");
    Check(hops.peekEnds, "dock pixels: ...after its three the icon goes back behind the edge (peek target 0, the hidden mode off)");
    Check(hops.sensorPeek, "dock pixels: an overheat while the dock is hidden peeks the sensor card out (sbHidden), hopping with the cell's hop");
    Check(hops.sensorPeekEnds, "dock pixels: ...after its three the card goes back (sbPeek target 0, sbHidden off)");
    Check(ReadAll(AppDataDir() + L"\\dock.log").find("EndDraw") == std::string::npos, "dock pixels: no frame fails in EndDraw");
    printf("  dock pixels: %d pictures hashed\n", (int)pixHashes.size());
    Dock::ResetNotifyBitmaps();
    PixSensors(kPixMain); g_sensors.live = false; g_sensors.d = {}; g_sensors.panel = PanelData{};
    g_sys.SetBattery({}); themeLight = false; oledOn = false; features::SetQuiet(0);
    Apps(0);
    g_settings = Settings{};
    monitors.resize(keepMonitors);
}

// ---- the power draw in the panel (mod.40: settings powerView, showCpuPower, showGpuPower; dock_panel.cpp's Cards,
// Spark, Footer). The sensor data of the pixel hashes' main fixture (PixSensors) and a power draw as the sampler gives
// it (sensorlogic::PowerData: a 60 s history of 2 s points, then 1 s ones since the panel opened 30 s ago).
// --power-png=<dir>: each picture also as <dir>\<name>.png (the panel over a dark grey, at its own pixels) - pictures
// to look at, outside the repo; nothing else is written outside the fixture's folder.
static std::wstring powerPngDir;
static bool PowerPng(const std::wstring& name, const std::vector<uint32_t>& px, int w, int h) {
    ComPtr<IWICImagingFactory> wic = g_gfx.wic;
    if (!wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICStream> st; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> fr;
    const std::wstring path = powerPngDir + L"\\" + name + L".png";
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(wic->CreateStream(&st)) || FAILED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr)) || FAILED(fr->SetSize(w, h)) ||
        FAILED(fr->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat32bppBGRA)
        return false;
    std::vector<uint32_t> out(px);   // opaque (drawn over the backdrop): premultiplied is straight
    return SUCCEEDED(fr->WritePixels(h, w * 4, (UINT)(out.size() * 4), (BYTE*)out.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
}
// The power draw: CPU about 70 W (its auto scale), GPU about 285 W under a 360 W limit; known: whether each was read
// and reported (false: read, no value - HWiNFO without the reading, a GPU without NVML)
static void PowerFixture(bool cpuKnown, bool gpuKnown, uint64_t atMs = 600000) {
    auto& pw = g_sensors.power;
    pw = {};
    pw.view = std::clamp(g_settings.powerView, 0, 2); pw.atMs = atMs;
    auto series = [&](sensorlogic::PowerSeries& ps, bool known, float base, float amp, float limit) {
        ps.read = true;
        if (!known) return;
        const bool graph = pw.view == sensorlogic::kPowerGraph;
        for (uint64_t t = atMs - 62000; t <= atMs; t += (atMs - t > 30000 ? 2000 : 1000)) {
            const float f = (float)(t % 100000) / 1000.0f;
            const float w = base + amp * std::sin(f * 0.45f) + amp * 0.4f * std::sin(f * 1.7f) - (std::fabs(f - 47) < 3 ? amp * 2.5f : 0);
            if (graph) sensorlogic::AddPowerPoint(ps.history, t, w);
            ps.watts = w;
        }
        ps.limitW = limit;
        if (graph) {
            sensorlogic::PowerScale sc;
            ps.top = limit > 0 ? limit : sc.Update(sensorlogic::PowerMax(ps.history, atMs));
        }
    };
    series(pw.cpu, cpuKnown && g_sensors.Items().CpuPowerShown(), 68, 5, NAN);
    series(pw.gpu, gpuKnown && g_sensors.Items().GpuPowerShown(), 285, 12, 360);
    if (!g_sensors.Items().CpuPowerShown()) pw.cpu = {};
    if (!g_sensors.Items().GpuPowerShown()) pw.gpu = {};
    ++g_sensors.powerVersion;
}
static void PowerPanel() {
    const size_t keepMonitors = monitors.size();
    monitors.push_back({ kUhd, { 0, 1800, 3840, 3960 }, false, false, "uhd-3840x2160" });
    bool offSame = true, sameHeight = true, sparkDrawn = true, sparkCards = true, summaryFooter = true, unknownNoSpark = true;
    bool cached = true, moved = true, rebuilt = true, fitsVertical = true, pngs = true, noGpu = true;
    int pictures = 0;
    for (HMONITOR m : { kDesktop, kLaptop, kUhd }) for (Edge edge : { Edge::Bottom, Edge::Left }) {
        g_settings = Settings{}; g_settings.edge = edge;
        Apps(6);
        PixSensors(kPixMain);
        g_sensors.d.flags |= LD_HWINFO;   // HWiNFO runs (the CPU power hint's words)
        g_sensors.power = {}; ++g_sensors.powerVersion;
        Dock d(m, 60 + (int)edge, m == kLaptop);
        if (!d.Create()) { sparkDrawn = false; continue; }
        d.nextCaptureTry = std::numeric_limits<double>::max();
        d.reveal.snap(1); d.hideOff = 0; d.panelAlpha.snap(1); d.panelAlpha.target = 1;
        d.Layout();
        const bool vert = IsVertical(edge);
        char tag[48]; snprintf(tag, sizeof tag, "%s_s%.2f", vert ? "left" : "bottom", d.scale);
        // the panel alone at its rectangle; over a dark grey for the pictures
        auto shot = [&](bool backdrop) {
            d.ComputePanelRect();
            g_gfx.EnsureFonts(d.scale);
            const D2D1_RECT_F r = d.panelRect;
            const int w = (int)std::lround(r.right - r.left), h = (int)std::lround(r.bottom - r.top);
            return std::make_pair(Pixels(w, h, [&] {
                if (backdrop) {
                    g_gfx.dc->Clear(D2D1::ColorF(0x1D2A33));
                    ComPtr<ID2D1SolidColorBrush> b;
                    g_gfx.dc->CreateSolidColorBrush(D2D1::ColorF(0x121217), &b);
                    if (b) g_gfx.dc->FillRoundedRectangle({ { 0, 0, (float)w, (float)h }, 16 * d.scale, 16 * d.scale }, b.Get());
                }
                g_gfx.dc->SetTransform(D2D1::Matrix3x2F::Translation(-r.left, -r.top));
                d.DrawPanel();
                g_gfx.dc->SetTransform(D2D1::Matrix3x2F::Identity());
            }), std::make_pair(w, h));
        };
        auto png = [&](const char* what, bool backdropOnly = true) {
            if (powerPngDir.empty()) return;
            const auto [px, wh] = shot(backdropOnly);
            const std::string n = std::string("power_") + what + "_" + tag;
            pngs &= PowerPng(std::wstring(n.begin(), n.end()), px, wh.first, wh.second);
            ++pictures;
        };
        const auto [none, size0] = shot(false);
        png("none");
        // the card's rows and the footer's, in the panel's pixels (the cards 55 + 84 DIP down, the sparkline 48 .. 66)
        const int ct = (int)std::round(55 * d.scale), sp0 = ct + (int)std::round(48 * d.scale), sp1 = ct + (int)std::round(66 * d.scale);
        const int foot0 = size0.second - (int)std::round(33 * d.scale), w0 = size0.first;
        auto cardHalf = [&](const std::vector<uint32_t>& px, int half, char which) {   // the CPU's (0) or the GPU's (1) card: its tinted pixels
            int n = 0;
            for (int y = sp0; y < sp1; ++y) for (int x = half ? w0 / 2 : 0; x < (half ? w0 : w0 / 2); ++x) {
                const uint32_t p = px[(size_t)y * w0 + x];
                const int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                n += which == 'b' ? (b > r + 20 && b > 30) : (g > r + 20 && g > b + 10 && g > 30);
            }
            return n;
        };
        auto rowsSame = [&](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int y0, int y1) {
            if (a.size() != b.size()) return false;
            for (int y = std::max(0, y0); y < y1; ++y) for (int x = 0; x < w0; ++x) if (a[(size_t)y * w0 + x] != b[(size_t)y * w0 + x]) return false;
            return true;
        };
        // 없음 with a power draw left in the data (never there: DropUnshownPower) draws as without one: pixel for pixel
        g_settings.powerView = sensorlogic::kPowerGraph;
        PowerFixture(true, true);
        g_settings.powerView = sensorlogic::kPowerOff;
        offSame &= shot(false).first == none;

        // 그래프: both cards' sparklines (blue in the CPU's on a horizontal dock, green in the GPU's), the panel as high
        g_settings.powerView = sensorlogic::kPowerGraph;
        PowerFixture(true, true);
        const auto [graph, size1] = shot(false);
        sameHeight &= size1 == size0;
        sparkCards &= cardHalf(graph, 0, 'b') > cardHalf(none, 0, 'b') + 50 && cardHalf(graph, 1, 'g') > cardHalf(none, 1, 'g') + 50;
        sparkDrawn &= graph != none && !rowsSame(graph, none, sp0, sp1);
        // the paths built once a sample: drawn again with the same powerVersion, the same geometry
        const ComPtr<ID2D1PathGeometry> f0 = d.panelSpark[0].fill;   // held: a new one cannot take its address
        shot(false);
        cached &= f0 && d.panelSpark[0].fill == f0 && d.panelSpark[0].version == g_sensors.powerVersion;
        // ...and while the panel slides (half way in its fade: some pixels off its resting place, the cards' boxes moved)
        {
            const D2D1_RECT_F rest = d.panelRect;
            d.panelAlpha.snap(0.5f); d.panelAlpha.target = 1;
            shot(false);
            moved &= d.panelRect.top != rest.top || d.panelRect.left != rest.left;
            cached &= d.panelSpark[0].fill == f0 && d.panelSpark[1].fill;
            d.panelAlpha.snap(1); d.panelAlpha.target = 1;
            shot(false);
            cached &= d.panelSpark[0].fill == f0;
        }
        PowerFixture(true, true, 601000);
        shot(false);
        rebuilt &= d.panelSpark[0].fill && d.panelSpark[0].fill != f0 && d.panelSpark[0].version == g_sensors.powerVersion;
        png("graph");
        // 그래프, the CPU's not reported by HWiNFO: no sparkline in its card (the hint "HWiNFO 전력: 클릭" instead)
        PowerFixture(false, true);
        const auto graphNoCpu = shot(false).first;
        unknownNoSpark &= cardHalf(graphNoCpu, 0, 'b') <= cardHalf(none, 0, 'b') + 5 && cardHalf(graphNoCpu, 1, 'g') > cardHalf(none, 1, 'g') + 50;
        png("graph_cpu_unknown");
        // ...the GPU not an NVIDIA one (no NVML: its temperature hint, no power)
        const auto keepFlags = g_sensors.d.flags;
        g_sensors.d.flags &= ~(LD_HAS_GPU | LD_HAS_GPU_LOAD);
        PowerFixture(true, false);
        const auto graphNoGpu = shot(false).first;
        noGpu &= cardHalf(graphNoGpu, 1, 'g') <= cardHalf(none, 1, 'g') + 5 && cardHalf(graphNoGpu, 0, 'b') > cardHalf(none, 0, 'b') + 50;
        png("graph_gpu_none");
        g_sensors.d.flags = keepFlags;
        // ...HWiNFO not running: one hint for the temperature and the power
        g_sensors.d.flags &= ~(LD_HWINFO | LD_HAS_CPU_TEMP);
        PowerFixture(false, true);
        png("graph_hwinfo_off");
        g_sensors.d.flags = keepFlags;
        // ...drives left out (eleven): the footer's total gives way on the vertical dock
        PixSensors(kPixDrives11); g_sensors.d.flags |= LD_HWINFO;
        PowerFixture(true, true);
        png("graph_leftout");
        PixSensors(kPixMain); g_sensors.d.flags |= LD_HWINFO;

        // 요약: the cards as 없음 draws them, the footer's dots
        g_settings.powerView = sensorlogic::kPowerSummary;
        PowerFixture(true, true);
        const auto [summary, size2] = shot(false);
        sameHeight &= size2 == size0;
        summaryFooter &= rowsSame(summary, none, 0, foot0) && !rowsSame(summary, none, foot0, size0.second);
        png("summary");
        PowerFixture(false, true);
        png("summary_cpu_unknown");
        g_sensors.d.flags &= ~(LD_HAS_GPU | LD_HAS_GPU_LOAD);
        PowerFixture(true, false);
        png("summary_gpu_none");
        g_sensors.d.flags = keepFlags;
        PowerFixture(false, false);
        summaryFooter &= shot(false).first == none;   // neither known: the footer as 없음's
        PixSensors(kPixDrives11); g_sensors.d.flags |= LD_HWINFO;
        PowerFixture(true, true);
        png("summary_leftout");
        // the footer's plan with this dock's fonts: on the vertical dock, eleven drives (and modules) left out with both
        // dots, the counts still whole (Footer cuts them only past step 2)
        {
            const float s = d.scale, x0 = std::round(18 * s), avail = (d.panelRect.right - d.panelRect.left) - 2 * x0;
            const auto rows = d.PanelRowsNow();
            const auto& p = g_sensors.panel;
            const auto ft = sensorlogic::PanelFooter(true, true, (int)p.drives.size(), p.drivesWithTemp, rows.drivesHidden, true, p.modules, p.dimmsWithTemp, rows.dimmsHidden + 1);
            auto W = [&](const std::wstring& t) { return t.empty() ? 0.0f : d.TextSize(g_gfx.fmtPanelSmall.Get(), t).width; };
            const float dot = std::round(6 * s), dg = std::round(4 * s), ig = std::round(10 * s);
            const float full = 2 * (dot + dg) + ig + std::ceil(W(L"CPU ")) + std::ceil(W(L"71 W")) + std::ceil(W(L"GPU ")) + std::ceil(W(L"287 W"));
            const float shortW = 2 * (dot + dg) + ig + std::ceil(W(L"71 W")) + std::ceil(W(L"287 W"));
            const auto plan = sensorlogic::PlanPowerFooter(avail, std::round(10 * s), W(ft.counts), W(ft.leftOut), W(L"외 9개"), full, shortW);
            if (vert) fitsVertical &= plan.countsRoom >= W(ft.counts) && rows.drivesHidden > 0;
            else fitsVertical &= !plan.powerShort && !plan.leftOutShort;   // 414 DIP: nothing shortened
            if (vert) printf("  power footer %s: counts %.0f, left out %.0f, dots %.0f / %.0f of %.0f px: names %s, left out %s\n", tag, W(ft.counts),
                             W(ft.leftOut), full, shortW, avail, plan.powerShort ? "dropped" : "kept", plan.leftOutShort ? "short" : "whole");
        }
        PixSensors(kPixMain);
        // CPU 전력 off under 요약: only the GPU's dot
        g_settings.showCpuPower = false;
        PowerFixture(true, true);
        png("summary_cpu_off");
        g_settings = Settings{};
    }
    printf("  power pictures: %d%s\n", pictures, powerPngDir.empty() ? " (none asked: --power-png=<dir>)" : "");
    Check(offSame, "power 없음: a power draw in the data changes no pixel of the panel (today's look)");
    Check(sameHeight, "그래프 and 요약 leave the panel as high as 없음 (+0 DIP) at 1.0, 1.25, 1.5 on both edges");
    Check(sparkDrawn && sparkCards, "그래프: the sparklines drawn in the cards' lower half, blue in the CPU's and green in the GPU's");
    Check(cached && moved && rebuilt, "그래프: the paths kept while powerVersion stays (frames of a fade, the panel moved), built again for a new sample");
    Check(unknownNoSpark, "그래프, the CPU's power unknown: no sparkline in its card, the GPU's still");
    Check(noGpu, "그래프, a GPU without NVML: no sparkline in its card, the CPU's still");
    Check(summaryFooter, "요약: the cards as 없음 draws them, only the footer differs; neither known: the panel as 없음's");
    Check(fitsVertical, "the footer with both dots and drives left out: whole counts on the vertical dock (shortened), nothing shortened on the horizontal");
    Check(pngs, "the power pictures written (when asked)");
    g_sensors.power = {}; g_sensors.live = false; g_sensors.d = {}; g_sensors.panel = PanelData{};
    g_settings = Settings{};
    Apps(0);
    monitors.resize(keepMonitors);
}

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!wcsncmp(argv[i], L"--power-png=", 12)) powerPngDir = argv[i] + 12;
        if (!wcscmp(argv[i], L"--snap")) pixSnap = true;
        else if (!wcsncmp(argv[i], L"--snap=", 7)) { pixSnap = true; for (const wchar_t* c = argv[i] + 7; *c; ++c) pixSnapOnly += (char)*c; }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    if (AppDataDir().find(L"\\tests\\dock-render\\") == std::wstring::npos) { puts("not in the fixture folder"); return 100; }
    LogStartSession();   // as the dock: "no frame fails in EndDraw" reads this run's dock.log
    if (!g_gfx.Init()) { puts("GPU initialization failed"); return 100; }
    g_sys.timeText = L"21:30"; g_sys.timeShort = L"21:30"; g_sys.dateText = L"2026-10-28";
    monitors.push_back({ kLaptop, { 0, 0, 2880, 1800 }, true, false, "laptop-2880x1800" });
    monitors.push_back({ kExternal, { 2880, 0, 4800, 1080 }, false, false, "external-1920x1080" });
    monitors.push_back({ kDesktop, { -2560, 0, 0, 1440 }, false, false, "desktop-2560x1440" });
    MaximizedFront();
    ResetMetrics({ kLaptop }, "the laptop alone, 1.25");
    ResetMetrics({ kDesktop }, "one desktop monitor, 1.0");
    ResetMetrics({ kLaptop, kExternal, kDesktop }, "three monitors, 1.25 / 0.75 / 1.0");
    Recorder();
    Gradients();
    SensorPanel();
    PowerPanel();
    RenderAround();
    DockPixels();
    printf("dock_render_device: %s (%d checks, %d failures)\n", failures ? "FAILED" : "ALL PASSED", checks, failures);
    return failures;
}
