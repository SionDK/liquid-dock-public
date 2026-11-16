// The production Dock (dock*.cpp, dock_record.cpp through dock_record.obj)
// on its real Direct3D / Direct2D device, for what happens around a GPU reset and the built-in recorder.
// The fixture is the one of tests/dock_interaction_test.cpp (stubs for the shell, OLED, theme, audio,
// Start and the taskbar; fake monitors): no input is sent, no window is shown, nothing outside this
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
#define private public
#include "../src/sensors.h"
#include "../src/dock.h"
#undef private

Settings g_settings;
HWND g_mainWnd = nullptr;
struct MonitorCase { HMONITOR handle; RECT rect; bool primary; bool actual; const char* name; };
static std::vector<MonitorCase> monitors;
static bool fgEnabled=false, fgMaximized=false;
static RECT fgRect{};
static HMONITOR fgMonitor=nullptr;
static LONG_PTR fgStyle=WS_POPUP;
static const wchar_t* fgClass=L"MatrixFullscreen";
static HWND fgHandle=(HWND)(uintptr_t)0xF00DFACE;
static BOOL WINAPI MatrixMonitorInfo(HMONITOR m,LPMONITORINFO info) {
 for(const auto& entry:monitors) if(entry.handle==m) {info->rcMonitor=info->rcWork=entry.rect;info->dwFlags=entry.primary?MONITORINFOF_PRIMARY:0;return TRUE;}
 return ::GetMonitorInfoW(m,info);
}
static HWND WINAPI MatrixForeground(){return fgEnabled?fgHandle:nullptr;}
static HMONITOR WINAPI MatrixMonitorFromWindow(HWND h,DWORD flags){return h==fgHandle ? fgMonitor : ::MonitorFromWindow(h,flags);}
static BOOL WINAPI MatrixWindowRect(HWND h,LPRECT rect){if(h==fgHandle){*rect=fgRect;return TRUE;}return ::GetWindowRect(h,rect);}
static BOOL WINAPI MatrixIsZoomed(HWND h){return h==fgHandle ? fgMaximized : ::IsZoomed(h);}
static LONG_PTR WINAPI MatrixWindowLong(HWND h,int index){return h==fgHandle ? fgStyle : ::GetWindowLongPtrW(h,index);}
// the client area GetWindowInfo reports for the window in front (a maximized window is fullscreen only when it
// covers the monitor): empty unless a check sets it; fgInfoFails makes the call fail
static RECT fgClient{};
static bool fgInfoFails=false;
static BOOL WINAPI MatrixWindowInfo(HWND h,PWINDOWINFO wi){
 if(h!=fgHandle)return ::GetWindowInfo(h,wi);
 if(fgInfoFails)return FALSE;
 wi->rcWindow=fgRect;wi->rcClient=fgClient;wi->dwStyle=(DWORD)fgStyle;return TRUE;
}
static int WINAPI MatrixClassName(HWND h,LPWSTR text,int capacity){if(h==fgHandle){wcsncpy_s(text,capacity,fgClass,_TRUNCATE);return (int)wcslen(text);}return ::GetClassNameW(h,text,capacity);}
static BOOL WINAPI MatrixShowWindow(HWND,int){return TRUE;} // keep fixture windows invisible
static SHORT WINAPI MatrixKeyState(int){return 0;}
static int settingsMenus=0;
static BOOL WINAPI MatrixForegroundSet(HWND){return TRUE;}
static BOOL WINAPI MatrixPopupMenu(HMENU,UINT,int,int,int,HWND,const RECT*){++settingsMenus;return 0;}
static int desktopActions=0;
namespace nativetray {
static bool MatrixToggleDesktop(){++desktopActions;return true;}
}
// OLED mode stubs: the matrix never changes the real wallpaper or desktop icons
static int oledActions=0; static bool oledOn=false;
namespace oled {
bool Active(){return oledOn;}
bool CoversMonitor(HMONITOR){return oledOn;}   // the matrix's OLED mode covers every monitor
bool Enable(HMONITOR){++oledActions;oledOn=true;return true;}
bool Disable(){++oledActions;oledOn=false;return true;}
void Reapply(){}
}
// the shape window never opens in the matrix
#include "../src/settings_window.h"
static int settingsWindows=0;
namespace settingswindow { void Show(HMONITOR){++settingsWindows;} void Close(){} bool IsOpen(){return false;} bool PreTranslate(MSG*){return false;} }
static int themeActions=0; static bool themeLight=false;
namespace theme {
bool IsLight(){return themeLight;}
bool Toggle(){++themeActions;themeLight=!themeLight;return true;}
}
// Shell, Recycle Bin and audio stubs: the matrix never opens pages, deletes files or switches devices
static int shellOpens=0; static std::wstring lastShellOpen;
static HINSTANCE WINAPI MatrixShellExecute(HWND,LPCWSTR,LPCWSTR file,LPCWSTR,LPCWSTR,INT){++shellOpens;lastShellOpen=file?file:L"";return (HINSTANCE)42;}
static std::vector<std::wstring> lastDeleted; static UINT lastDeleteFunc=0; static FILEOP_FLAGS lastDeleteFlags=0;
static int MatrixFileOperation(LPSHFILEOPSTRUCTW op){
 lastDeleted.clear();lastDeleteFunc=op->wFunc;lastDeleteFlags=op->fFlags;
 for(const wchar_t* p=op->pFrom;p&&*p;p+=wcslen(p)+1)lastDeleted.push_back(p);
 return 0;
}
static HRESULT WINAPI MatrixEmptyRecycleBin(HWND,LPCWSTR,DWORD){return S_OK;}
#include "../src/audio_devices.h"
static int audioSwitches=0;
namespace audio {
std::vector<Device> Outputs(){return {{L"a",L"Speakers",true},{L"b",L"Headset",false}};}
bool SetDefault(const std::wstring&){++audioSwitches;return true;}
std::wstring DefaultOutputName(){return L"Speakers";}   // the audio button's tooltip (since mod.15)
}
// start button stubs: the matrix never opens Start or presses the Windows key
#include "../src/start_menu.h"
static int startClicks=0; static HMONITOR startClickMonitor=nullptr; static bool startClickInFront=false;
namespace startmenu {
bool InFront(){return false;}
void Click(HMONITOR m,bool startWasInFront){++startClicks;startClickMonitor=m;startClickInFront=startWasInFront;}
}
// the taskbar's Start button (system_taskbar.obj's taskbar hiding looks it up): the matrix never looks into the real taskbar
#include "../src/taskbar_start.h"
namespace taskbarstart { void Remember(HWND){} }
// app clicks through the taskbar's own buttons (apps_press.obj, taskbar_apps.h): the matrix never presses the real
// taskbar - no press can be started, so every click goes the dock's own way
#include "../src/taskbar_apps.h"
namespace taskbarapps {
bool Begin(const Job&){return false;}
bool Take(uint32_t,Result&){return false;}
const wchar_t* OutcomeText(Outcome){return L"";}
}
#define ShellExecuteW MatrixShellExecute
#define SHFileOperationW MatrixFileOperation
#define SHEmptyRecycleBinW MatrixEmptyRecycleBin
#define GetMonitorInfoW MatrixMonitorInfo
#define GetForegroundWindow MatrixForeground
#define MonitorFromWindow MatrixMonitorFromWindow
#define GetWindowRect MatrixWindowRect
#define IsZoomed MatrixIsZoomed
#define GetWindowLongPtrW MatrixWindowLong
#define GetWindowInfo MatrixWindowInfo
#define GetClassNameW MatrixClassName
#define ShowWindow MatrixShowWindow
#define GetAsyncKeyState MatrixKeyState
#define SetForegroundWindow MatrixForegroundSet
#define TrackPopupMenu MatrixPopupMenu
#define ToggleDesktop MatrixToggleDesktop
#include "../src/dock.cpp"
#include "../src/dock_window.cpp"
#include "../src/dock_input.cpp"
#include "../src/dock_layout.cpp"
#include "../src/dock_render.cpp"
#include "../src/dock_sensors.cpp"
#include "../src/dock_controls.cpp"
#include "../src/dock_notify.cpp"
#undef GetMonitorInfoW
#undef GetForegroundWindow
#undef MonitorFromWindow
#undef GetWindowRect
#undef IsZoomed
#undef GetWindowLongPtrW
#undef GetWindowInfo
#undef GetClassNameW
#undef ShowWindow
#undef GetAsyncKeyState
#undef SetForegroundWindow
#undef TrackPopupMenu
#undef ToggleDesktop

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
        Check(!d.fullTex.tex && !d.dockFrame.tex && !d.recRT.tex && !d.recTarget, "a recording that never started keeps no monitor-sized textures");
        RemoveDirectoryW(raw.c_str());
        Dock::carryRecording = false;

        d.StartRecording({ 0, 900, 2880, 1800 }, 0.5f);
        Check(d.Recording() && d.recW == 1440 && d.recH == 450, "recording at half size of the cropped half");
        double t = NowSeconds();
        for (int i = 0; i < 6; ++i) { d.reveal.snap(1); d.dirty = true; d.Render(); t += 1.0 / 30; d.RecordTick(t); }
        d.StopRecording();
        const long long frameBytes = 1440LL * 450 * 4;
        std::string text = ReadAll(info);
        int w = 0, h = 0, fps = 0, frames = -1;
        sscanf_s(text.c_str(), "%d %d %d %d", &w, &h, &fps, &frames);
        Check(w == 1440 && h == 450 && fps == 30 && frames == 4, "info.txt: size, 30 fps and the frames written (two frames late)");
        Check(FileSize(raw) == frames * frameBytes, "frames.raw holds exactly the counted frames");
        Check(!d.Recording() && !Dock::carryRecording && !d.fullTex.tex && !d.recRT.tex, "stopping releases the recorder's textures");

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
        Check(held != INVALID_HANDLE_VALUE && !d.Recording() && !Dock::carryRecording && !d.fullTex.tex && !d.recRT.tex,
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
            d.Layout(0); d.ComputePanelRect(); d.dirty = true; d.Render();
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
    d.Layout(0); d.reveal.snap(1);
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

// The "PC 상태" panel (dock_sensors.cpp): the window keeps room for the drives, the memory rows and the VRAM block
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
        d.Layout(0); d.ComputePanelRect();
        const float ph = vert ? d.panelRect.bottom - d.panelRect.top : d.panelRect.bottom - d.panelRect.top;   // upright on every edge
        auto rows = d.PanelRowsNow();
        const int roomDimmRows = std::min(Dock::kPanelMaxDimmRows, rowsOf(room.dimms));
        const int roomDriveRows = std::clamp(rowsOf(room.drives), 1, Dock::kPanelMaxDriveRows);
        sized &= room.drives >= 5 && room.dimms >= 2 && room.vram && d.panelW == std::round(Dock::PanelWidthDip(vert) * d.scale) &&
                 d.panelH == std::round(Dock::PanelHeightDip(roomDimmRows, roomDriveRows, true) * d.scale) &&
                 Near(d.panelRect.right - d.panelRect.left, d.panelW) && Near(ph, std::round(Dock::PanelHeightDip(rowsOf(2), rowsOf(5), true) * d.scale)) &&
                 rows.dimmRows == rowsOf(2) && rows.driveRows == rowsOf(5) && rows.drivesShown == 5 && rows.drivesHidden == 0 && rows.dimmsShown == 2 && rows.vram;
        if (!vert) sized &= Near(Dock::PanelHeightDip(1, 3, false), 332) && Near(Dock::PanelHeightDip(1, 3, true), 403) && Near(Dock::PanelWidthDip(false), 450);
        else sized &= Near(Dock::PanelWidthDip(true), 320);
        sized &= Near(Dock::PanelHeightDip(0, 1, false), 243) && Near(Dock::PanelHeightDip(0, 1, true), 322);   // the block 71 DIP, and a divider without memory rows
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
                   Near(phNo, std::round(Dock::PanelHeightDip(rowsOf(2), rowsOf(5), false) * d.scale)) && phNo < ph;
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
        fits &= rows.driveRows == rowsOf(2) && rows.drivesHidden == 0 && rows.vram && Near(ph2, std::round(Dock::PanelHeightDip(rowsOf(2), rowsOf(2), true) * d.scale)) && ph2 < ph;
        // more drives than the window was built for: the drives take every row there is beside the VRAM block (the
        // modules then have none: counted, as the drives left over)
        fill(11, 2);
        rows = d.PanelRowsNow();
        const float roomDip = d.panelH / d.scale + 0.5f;
        const int fitDrives = (int)((roomDip - (Dock::PanelHeightDip(0, 1, true) - 27)) / 27);   // the block, the divider, the footer
        counted &= rows.vram && rows.driveRows == std::min(rowsOf(11), fitDrives) && rows.drivesShown == std::min(11, rows.driveRows * cols) &&
                   rows.drivesShown + rows.drivesHidden == 11 && rows.drivesHidden > 0 && rows.dimmsShown + rows.dimmsHidden == 2 &&
                   rows.height <= roomDip && (rows.dimmRows == 0 || Dock::PanelHeightDip(rows.dimmRows, rows.driveRows, true) <= roomDip);
        // a window built without memory rows (no modules known), memory temperatures later (HWiNFO): the drives
        // keep their rows, the memory is counted ("RAM 외 2개") - not the drives pushed out by it
        const float keptH = d.panelH;
        d.panelH = std::round(Dock::PanelHeightDip(0, rowsOf(3), true) * d.scale);
        fill(3, 2);
        rows = d.PanelRowsNow();
        drivesFirst &= rows.vram && rows.driveRows == rowsOf(3) && rows.drivesShown == 3 && rows.drivesHidden == 0 && rows.dimmRows == 0 && rows.dimmsHidden == 2;
        drawn &= Lit(Pixels((int)d.winW, (int)d.winH, [&] { d.ComputePanelRect(); d.DrawPanel(); })) > 2000;
        // a window built without room for the VRAM block (no GPU with memory known then), the block later: left
        // out - the drives and the memory rows keep their rows and the panel stays within its room
        d.panelVramRoom = false;
        d.panelH = std::round(Dock::PanelHeightDip(0, rowsOf(3), false) * d.scale);
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
            d.panelH = std::round(Dock::PanelHeightDip(rowsOf(nDimms), rowsOf(nDrives), false) * d.scale);
            fill(nDrives, nDimms);
            rows = d.PanelRowsNow();
            vramLater &= !rows.vram && rows.driveRows == rowsOf(nDrives) && rows.dimmRows == rowsOf(nDimms) && rows.drivesHidden == 0 && rows.dimmsHidden == 0 &&
                         Near(rows.height, Dock::PanelHeightDip(rowsOf(nDimms), rowsOf(nDrives), false));
        }
        // ...and shown where the room holds it beside everything else (fewer rows now than the window was built for)
        d.panelH = std::round(Dock::PanelHeightDip(rowsOf(2), rowsOf(5), false) * d.scale);
        fill(1, 0);
        rows = d.PanelRowsNow();
        vramLater &= rows.vram && rows.driveRows == 1 && rows.drivesHidden == 0 && Near(rows.height, Dock::PanelHeightDip(0, 1, true));
        d.panelVramRoom = true;
        // a screen under 1440 px (scale 0.72 - 1): half a pixel of the room's rounding is more than half a DIP there,
        // and the rows the room was made for are all shown all the same (1680x1050: 349 DIP -> 254 px = 348.3 DIP;
        // 1366x768: 484 DIP -> 348 px = 483.3 DIP)
        const float keptScale = d.scale;
        for (float sc : { 1050 / 1440.0f, 0.72f, 0.75f, 0.9f }) for (auto [nDrives, nDimms] : { std::pair{ 2, 2 }, std::pair{ 5, 2 }, std::pair{ 1, 0 }, std::pair{ 6, 4 } }) {
            d.scale = sc;
            d.panelH = std::round(Dock::PanelHeightDip(rowsOf(nDimms), rowsOf(nDrives), true) * sc);
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

int wmain() {
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
    RenderAround();
    printf("dock_render_device: %s (%d checks, %d failures)\n", failures ? "FAILED" : "ALL PASSED", checks, failures);
    return failures;
}
