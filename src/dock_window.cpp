#include "dock.h"
#include "system.h"
#include "sensors.h"
#include "native_tray.h"
#include "start_menu.h"
#include <shellscalingapi.h>
#include <thread>
#include <shellapi.h>
#include <d2d1effects.h>
#include <dwmapi.h>
#include "oled.h"
#include "theme.h"
#include "explorer_taskbar.h"

static const wchar_t* kDockClass = L"LiquidDockBar";

// Explorer announces a (new) taskbar with it - also the tray host when it starts (tray_host.cpp)
UINT TaskbarCreatedMessage() { static const UINT m = RegisterWindowMessageW(L"TaskbarCreated"); return m; }

static LRESULT CALLBACK DockProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = (Dock*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        auto cs = (CREATESTRUCTW*)l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(h, m, w, l);
    }
    if (d) return d->WndProc(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

Dock::Dock(HMONITOR m, int i, bool p) : monitor(m), index(i), primary(p) {}

Dock::~Dock() {
    SuspendRecording();
    UpdateAppBar(false);
    windowPreview.Dismiss();
    capture.Stop();
    if (hwnd) DestroyWindow(hwnd);
}

void Dock::Metrics() {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    mon = mi.rcMonitor;
    // Sized relative to the screen, not to Windows' DPI setting: the dock (icons, text, margins)
    // takes the same share of every monitor. 1.0 = a screen whose short side is 1440 px.
    float shortSide = (float)std::min(mon.right - mon.left, mon.bottom - mon.top);
    // floor: on small laptop screens (768 px) pure proportion would make text unreadable
    scale = std::max(0.72f, shortSide / 1440.0f);
    baseNominal = std::round(g_settings.iconSize * scale);
    margin = std::round(std::clamp(g_settings.edgeGap, 0, 40) * scale);
    mag = std::max(1.0f, g_settings.magnification);
    g_gfx.EnsureFonts(scale);
    fit = 1;
    ApplyFit(0);
    bool vert = IsVertical(g_settings.edge);
    len = (float)(vert ? (mon.bottom - mon.top) : (mon.right - mon.left));
    float headroom = std::max(base * (mag - 1), base * 0.65f) + (vert ? 240 * scale : 46 * scale);
    // The panel: room for the memory rows, the VRAM block and the drive rows known now and shown by the
    // sensor settings (at least one drive row while the drives are shown; at most four memory rows and six
    // drive rows - more are counted in its footer). The window's thickness T follows from it on a horizontal
    // dock (what the settings hide makes it thinner), from panelW on a vertical one.
    {
        const auto room = g_sensors.PanelRoomHint();
        const auto rows = sensorlogic::RoomRows(room, g_sensors.Items().ssd, PanelColumns(vert), kPanelMaxDimmRows, kPanelMaxDriveRows);
        panelW = std::round(PanelWidthDip(vert) * scale);
        panelH = std::round(sensorlogic::PanelHeightDip(rows.dimmRows, rows.driveRows, rows.vram) * scale);
        panelVramRoom = room.vram;
        panelVramTempRoom = room.vram && room.vramTemp;
    }
    // The window is sized for the largest edge gap and thickness, so those settings can change live
    // (Relayout) without recreating the window, its swap chain and the screen capture.
    float marginMax = std::round(40 * scale), thickMax = MeasureMetrics(1, 150).thickness;
    float panelNeed = marginMax + thickMax + 14 * scale + (vert ? panelW : panelH) + 28 * scale;
    T = std::ceil(std::max(marginMax + thickMax + headroom + 10 * scale, panelNeed));
    int t = (int)T;
    switch (g_settings.edge) {
    case Edge::Bottom: winX = mon.left; winY = mon.bottom - t; winW = mon.right - mon.left; winH = t; break;
    case Edge::Top:    winX = mon.left; winY = mon.top;        winW = mon.right - mon.left; winH = t; break;
    case Edge::Left:   winX = mon.left; winY = mon.top;        winW = t; winH = mon.bottom - mon.top; break;
    case Edge::Right:  winX = mon.right - t; winY = mon.top;   winW = t; winH = mon.bottom - mon.top; break;
    }
    g_gfx.EnsureFonts(scale);
}

bool Dock::AlwaysShown() const {
    bool want = !g_settings.autoHide || (!primary && g_settings.secondaryAlwaysShown);
    return want && !oled::CoversMonitor(monitor);   // a monitor in OLED mode always hides its dock
}

void Dock::UpdateAppBar(bool want, double now) {
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = hwnd;
    if (!want || !hwnd) {
        if (appBar) { SHAppBarMessage(ABM_REMOVE, &abd); appBar = false; appBarRect = {}; }
        nextAppBarTry = 0; appBarFailing = false;   // wanted again later: asked at once, a failure logged anew
        return;
    }
    // the strip from the screen edge to the far side of the glass, in steps of 4 px so a crowded
    // dock shrinking by a pixel does not make every window on the screen move again
    int band = ((int)std::ceil(margin + barThick) + 3) / 4 * 4;
    RECT r = mon;
    UINT e = ABE_BOTTOM;
    switch (g_settings.edge) {
    case Edge::Bottom: e = ABE_BOTTOM; r.top = r.bottom - band; break;
    case Edge::Top:    e = ABE_TOP;    r.bottom = r.top + band; break;
    case Edge::Left:   e = ABE_LEFT;   r.right = r.left + band; break;
    case Edge::Right:  e = ABE_RIGHT;  r.left = r.right - band; break;
    }
    if (appBar && EqualRect(&r, &appBarRect)) return;
    if (!appBar) {
        // No Explorer taskbar yet (early logon, Explorer restarting): asked again every 2 s, not on every
        // frame. dock.log gets the failure once and the registration that ends it once (when the strip was
        // there again). Explorer's TaskbarCreated asks again at once (WndProc).
        if (now < nextAppBarTry) return;
        abd.uCallbackMessage = WM_APP_APPBAR;
        if (!SHAppBarMessage(ABM_NEW, &abd)) {
            if (!appBarFailing) Log(L"appbar: dock %d register failed (no Explorer taskbar?), asking every 2 s", index);
            appBarFailing = true;
            nextAppBarTry = now + 2.0;
            return;
        }
        appBar = true; nextAppBarTry = 0;
        appBarShell = ExplorerTaskbar();
        if (appBarFailing) Log(L"appbar: dock %d registered after all (Explorer's taskbar %p)", index, (void*)appBarShell);
        appBarFailing = false;
    }
    abd.uEdge = e;
    abd.rc = r;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    switch (e) {   // keep the dock's own thickness from whatever edge the shell returned
    case ABE_BOTTOM: abd.rc.top = abd.rc.bottom - band; break;
    case ABE_TOP:    abd.rc.bottom = abd.rc.top + band; break;
    case ABE_LEFT:   abd.rc.right = abd.rc.left + band; break;
    case ABE_RIGHT:  abd.rc.left = abd.rc.right - band; break;
    }
    SHAppBarMessage(ABM_SETPOS, &abd);
    appBarRect = r;
}

void Dock::BringToFront() {
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    raiseWidget = true;   // FluentFlyout's widget window above the glass again (PlaceWidget)
}

// ---------------------------------------------------------------- FluentFlyout's widget
// The slot opens when FluentFlyout shows something (its own rules: "hide completely when no media is
// playing" closes it while nothing plays) and has laid it out for the host window; it is as wide as
// what FluentFlyout shows, in steps so a new song title rarely moves the dock.
void Dock::UpdateWidgetSlot() {
    auto ws = fluentflyout::WidgetState();
    widgetHere = ws.monitor == monitor && !IsVertical(g_settings.edge);
    widgetOpen = widgetHere && ws.open && (ws.fitted || widgetOpen);
    if (widgetOpen) widgetBox = ws.box;
    const float step = 12 * scale;
    widgetWidth.target = widgetOpen ? std::ceil((widgetBox.right - widgetBox.left) / step) * step + gap : 0;
    if (!widgetOpen) widgetShowing = false;
    else if (!widgetShowing && widgetWidth.x >= widgetWidth.target - 1.5f) widgetShowing = true;
}

// FluentFlyout's widget window over the slot: it slides with the glass and is cut at the monitor's
// edge like the dock. Hidden, it waits where the slot is when the dock is up - on this monitor, so
// FluentFlyout keeps choosing it, and laid out for its size.
void Dock::PlaceWidget(bool fullscreen) {
    if (!widgetHere) return;
    const bool show = widgetShowing && !fullscreen && (reveal.x > 0.002f || reveal.target > 0.5f);
    const float off = show ? hideOff : 0;
    const auto r = RectUV(widgetU0, widgetU1, margin - off, margin + barThick - off);
    const int hostW = fluentflyout::HostWidth(scale), hostH = std::max(1, (int)std::lround(barThick));
    const float boxMid = IsRectEmpty(&widgetBox) ? hostW * 0.5f : (widgetBox.left + widgetBox.right) * 0.5f;
    const int x = winX + (int)std::lround((r.left + r.right) * 0.5f - boxMid);
    const int y = winY + (int)std::lround(r.top);
    RECT rect = { x, y, x + hostW, y + hostH };
    trayhost::Place(show, rect, raiseWidget && show, mon, hwnd);
    if (show) raiseWidget = false;
    widgetPlaced = true;
    widgetLastRect = rect;
}

bool Dock::Relayout() {
    int x = winX, y = winY, w = winW, h = winH;
    float keepFit = fit;   // a crowded dock keeps its current shrink instead of animating it again
    Metrics();
    if (x != winX || y != winY || w != winW || h != winH) return false;
    fit = keepFit; ApplyFit(0);
    backdropDirty = dirty = true;
    return true;
}

bool Dock::Create() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = DockProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kDockClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    Metrics();
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_ACCEPTFILES,
                           kDockClass, L"Liquid Dock", WS_POPUP, winX, winY, winW, winH,
                           nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) return false;
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    ChangeWindowMessageFilterEx(hwnd, TaskbarCreatedMessage(), MSGFLT_ALLOW, nullptr);   // from Explorer also when elevated (WndProc)
    SetWindowRgn(hwnd, CreateRectRgn(0, 0, 0, 0), FALSE);
    curRegion = { 0, 0, 0, 0 };
    if (!CreateDeviceResources()) return false;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    reveal.snap(settingsOpen ? 1.0f : 0.0f);   // recreated while the shape settings are open: no pop-in
    magAmount.snap(0);
    trailingGap.snap(0);
    SyncSlots();
    for (auto& s : slots) { s.appear.snap(1); s.shrink.snap(1); }
    return true;
}

bool Dock::CreateDeviceResources() {
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = winW; sd.Height = winH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    HRESULT hr = g_gfx.factory->CreateSwapChainForComposition(g_gfx.d3d.Get(), &sd, nullptr, &swap);
    if (FAILED(hr)) { Log(L"swapchain failed %08x", hr); return false; }
    ComPtr<ID3D11Texture2D> bb;
    swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_gfx.d3d->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
    ComPtr<IDXGISurface> surf;
    swap->GetBuffer(0, IID_PPV_ARGS(&surf));
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    g_gfx.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target);

    g_gfx.dcomp->CreateTargetForHwnd(hwnd, TRUE, &compTarget);
    g_gfx.dcomp->CreateVisual(&visual);
    visual->SetContent(swap.Get());
    compTarget->SetRoot(visual.Get());
    g_gfx.dcomp->Commit();

    backdrop.Create(winW, winH, false);
    halfA.Create((winW + 1) / 2, (winH + 1) / 2, true);
    halfB.Create((winW + 1) / 2, (winH + 1) / 2, true);

    lumRT.Create(16, 1, true);
    for (auto& st : lumStage) {
        D3D11_TEXTURE2D_DESC d; lumRT.tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_gfx.d3d->CreateTexture2D(&d, nullptr, &st);
    }
    for (int i = 0; i < 16; ++i) lumCol[i] = 0.3f;

    g_gfx.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);
    g_gfx.dc->CreateEffect(CLSID_D2D1ColorMatrix, &darken);
    return true;
}
