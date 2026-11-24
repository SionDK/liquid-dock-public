#include "settings_window.h"
#include "settings_window_internal.h"
#include "gfx.h"
#include "main.h"          // g_mainWnd, WM_APP_*
#include "dock_shared.h"   // g_settingsHold: the docks held up while this window can be seen
#include "theme.h"
#include "buttons.h"
#include "version.h"
#include <dwmapi.h>
#include <map>

namespace settingswindow {

HWND g_wnd = nullptr;
HMONITOR g_monitor = nullptr;
std::vector<Page> g_pages;

// ---------------------------------------------------------------- window state
// What only this file uses is in the unnamed namespaces; what the other parts use too (settings_window_internal.h) is
// outside them. The objects keep the order they were defined in (made in it, destroyed the other way round).

namespace {
int g_page = 0;
}
int g_hover = -1;                      // control (or -10 - nav item / -2 reset button) under the pointer
namespace {
int g_hoverSub = -1;                   // its segment
int g_drag = -1;                       // slider being dragged
// The mouse wheel: a gesture - the pointer resting where it was, on the same page - stays with what it
// began on, the page or the slider under the pointer, so a slider that the scrolling page brings under the
// pointer does not take the rest of a swipe (a touchpad sends dozens of messages for one).
struct WheelGesture {
    POINT at{};       // screen pixels
    int page = -1;    // -1: none yet
    int slider = -1;  // the control it works, -1 the page
    int rest = 0;     // the part of a notch not yet a slider step (touchpads and smooth-scrolling mice send parts)
};
WheelGesture g_wheel;
bool g_drawLater = false;   // a wheel message left the drawing to the one waiting after it (WheelDrawn)
float g_scroll = 0, g_contentH = 0;
float g_dpi = 96;
bool g_acrylic = false;
}
bool g_dark = true;
D2D1_COLOR_F g_accent = ColorF(0x4C9AFF);

namespace {
ComPtr<IDXGISwapChain1> g_swap;
ComPtr<ID2D1Bitmap1> g_target;
ComPtr<IDCompositionTarget> g_compTarget;
ComPtr<IDCompositionVisual> g_visual;
}
ComPtr<ID2D1SolidColorBrush> g_brush;
namespace { ComPtr<IDWriteTextFormat> g_fTitle, g_fNavTitle, g_fNav; }
ComPtr<IDWriteTextFormat> g_fLabel;
namespace { ComPtr<IDWriteTextFormat> g_fValue; }
ComPtr<IDWriteTextFormat> g_fHint;
namespace { ComPtr<IDWriteTextFormat> g_fGlyph, g_fSeg, g_fSegSmall; }
ComPtr<IDWriteTextFormat> g_fChip, g_fTiny;
std::map<int, ComPtr<IDWriteTextFormat>> g_glyphFonts;   // icon font by size (x10), for the button preview

namespace {
// layout in DIPs
constexpr float kW = 660, kH = 600, kNav = 176, kPad = 32;
}

std::vector<Hot> g_hots;
namespace {
D2D1_RECT_F g_sliderTrack[32];         // per control, for dragging
}

D2D1_COLOR_F Text(float a) { return g_dark ? ColorF(0.96f, 0.96f, 0.97f, a) : ColorF(0.10f, 0.10f, 0.11f, a); }
D2D1_COLOR_F Muted() { return g_dark ? ColorF(0.70f, 0.71f, 0.75f) : ColorF(0.36f, 0.37f, 0.40f); }
D2D1_COLOR_F Wash(float a) { return g_dark ? ColorF(1, 1, 1, a) : ColorF(0, 0, 0, a); }

namespace {

void ReadAccent() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        g_accent = ColorF((v & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, ((v >> 16) & 0xFF) / 255.f);   // ABGR
    // on a dark backdrop a dark accent loses contrast: lift it
    if (g_dark) {
        float l = 0.2126f * g_accent.r + 0.7152f * g_accent.g + 0.0722f * g_accent.b;
        if (l < 0.45f) { float k = (0.45f - l) / (1 - l); g_accent.r += (1 - g_accent.r) * k; g_accent.g += (1 - g_accent.g) * k; g_accent.b += (1 - g_accent.b) * k; }
    }
}

}   // namespace

ComPtr<IDWriteTextFormat> Font(float size, DWRITE_FONT_WEIGHT w, const wchar_t* family) {
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(g_gfx.dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ko-kr", &f)))
        g_gfx.dw->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ko-kr", &f);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return f;
}

namespace {

bool CreateTarget() {
    g_target.Reset();
    ComPtr<IDXGISurface> surf;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&surf)))) return false;
    auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), g_dpi, g_dpi);
    return SUCCEEDED(g_gfx.dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &g_target));
}

bool CreateResources() {
    RECT rc; GetClientRect(g_wnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = std::max<LONG>(1, rc.right); sd.Height = std::max<LONG>(1, rc.bottom);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(g_gfx.factory->CreateSwapChainForComposition(g_gfx.d3d.Get(), &sd, nullptr, &g_swap))) return false;
    if (!CreateTarget()) return false;
    g_gfx.dcomp->CreateTargetForHwnd(g_wnd, TRUE, &g_compTarget);
    g_gfx.dcomp->CreateVisual(&g_visual);
    g_visual->SetContent(g_swap.Get());
    g_compTarget->SetRoot(g_visual.Get());
    g_gfx.dcomp->Commit();
    g_gfx.dc->CreateSolidColorBrush(ColorF(1, 1, 1), &g_brush);
    g_fTitle = Font(26, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"Segoe UI Variable Display");
    g_fNavTitle = Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fNav = Font(15, DWRITE_FONT_WEIGHT_NORMAL);
    g_fLabel = Font(15, DWRITE_FONT_WEIGHT_NORMAL);
    g_fValue = Font(15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fValue->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_fHint = Font(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fSeg = Font(14, DWRITE_FONT_WEIGHT_NORMAL);
    g_fSeg->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fSegSmall = Font(12, DWRITE_FONT_WEIGHT_NORMAL);   // long choices (key combinations) in narrow segments
    g_fSegSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fChip = Font(13.5f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fTiny = Font(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fTiny->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    // the icon font the dock resolved (Segoe Fluent Icons, or MDL2 Assets on Windows 10)
    wchar_t family[64] = L"Segoe Fluent Icons";
    if (g_gfx.fmtGlyph) g_gfx.fmtGlyph->GetFontFamilyName(family, ARRAYSIZE(family));
    g_fGlyph = Font(16, DWRITE_FONT_WEIGHT_NORMAL, family);
    return true;
}

void Release() {
    g_target.Reset(); g_swap.Reset(); g_visual.Reset(); g_compTarget.Reset(); g_brush.Reset();
    g_fTitle.Reset(); g_fNavTitle.Reset(); g_fNav.Reset(); g_fLabel.Reset(); g_fValue.Reset(); g_fHint.Reset(); g_fSeg.Reset(); g_fSegSmall.Reset(); g_fGlyph.Reset(); g_fChip.Reset(); g_fTiny.Reset(); g_glyphFonts.clear();
}

}   // namespace

// ---------------------------------------------------------------- drawing

void Fill(D2D1_RECT_F r, D2D1_COLOR_F c, float radius) {
    g_brush->SetColor(c);
    if (radius > 0) g_gfx.dc->FillRoundedRectangle({ r, radius, radius }, g_brush.Get());
    else g_gfx.dc->FillRectangle(r, g_brush.Get());
}

void Draw(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    g_brush->SetColor(c);
    g_gfx.dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

namespace {

float RowHeight(const Control& c) {
    float h = c.type == Control::Segmented ? 84.f : c.type == Control::Slider ? 68.f : c.type == Control::Group ? 58.f : 56.f;   // room for the "기본" marks
    return h + (c.hint ? 18 : 0);
}

// ---- Render's parts, in the order it draws them. Each draws one piece of the window and adds its hot areas
// (g_hots). x0, x1: the page's column; y: the top of the control's row; hot: the pointer is on it (all from Render).

// navigation pane
void DrawNav() {
    Fill({ 0, 0, kNav, kH }, Wash(g_dark ? 0.035f : 0.03f));
    Draw(L"Liquid Dock", g_fNavTitle.Get(), { 22, 18, kNav, 44 }, Text());
    Draw(L"버전 " LD_VERSION_WSTR, g_fHint.Get(), { 22, 42, kNav, 60 }, Muted());
    for (int i = 0; i < (int)g_pages.size(); ++i) {
        D2D1_RECT_F r = { 10, 78 + i * 44.f, kNav - 10, 78 + i * 44.f + 38 };
        bool sel = i == g_page, hot = g_hover == -10 - i;
        if (sel || hot) Fill(r, Wash(sel ? 0.09f : 0.05f), 6);
        if (sel) Fill({ r.left, r.top + 10, r.left + 3, r.bottom - 10 }, g_accent, 1.5f);
        Draw(g_pages[i].glyph, g_fGlyph.Get(), { r.left + 14, r.top, r.left + 40, r.bottom }, sel ? Text() : Muted());
        Draw(g_pages[i].title, g_fNav.Get(), { r.left + 44, r.top, r.right, r.bottom }, sel ? Text() : Text(0.85f));
        g_hots.push_back({ r, -10 - i, 0 });
    }
}

void DrawSegmented(const Control& c, int ci, bool hot, float x0, float x1, float y) {
    Draw(c.label, g_fLabel.Get(), { x0, y, x1, y + 22 }, Text());
    float top = y + 28;
    if (c.hint) { Draw(c.hint, g_fHint.Get(), { x0, top - 4, x1, top + 14 }, Muted()); top += 18; }
    D2D1_RECT_F bar = { x0, top, x1, top + 36 };
    Fill(bar, Wash(0.06f), 6);
    int n = (int)c.options.size(), cur = c.index();
    float w = (bar.right - bar.left - 4) / n;
    for (int i = 0; i < n; ++i) {
        D2D1_RECT_F r = { bar.left + 2 + i * w, bar.top + 2, bar.left + 2 + (i + 1) * w, bar.bottom - 2 };
        if (i == cur) Fill(r, g_dark ? ColorF(1, 1, 1, 0.16f) : ColorF(1, 1, 1, 0.95f), 4);
        else if (hot && g_hoverSub == i) Fill(r, Wash(0.05f), 4);
        Draw(c.options[i], (r.right - r.left) < wcslen(c.options[i]) * 8.5f ? g_fSegSmall.Get() : g_fSeg.Get(), r, i == cur ? Text() : Text(0.8f));
        if (i == cur) Fill({ (r.left + r.right) / 2 - 8, r.bottom - 3, (r.left + r.right) / 2 + 8, r.bottom - 1 }, g_accent, 1);
        if (i == (int)c.def) Draw(L"기본", g_fTiny.Get(), { r.left, bar.bottom + 2, r.right, bar.bottom + 17 }, Muted());
        g_hots.push_back({ r, ci, i });
    }
}

void DrawSlider(ID2D1DeviceContext2* dc, const Control& c, int ci, bool hot, float x0, float x1, float y) {
    float v = c.value();
    Draw(c.label, g_fLabel.Get(), { x0, y, x1 - 140, y + 22 }, Text());
    bool atDefault = !std::isnan(c.def) && std::fabs(v - c.def) < c.step * 0.5f;
    Draw(c.text(v) + (atDefault ? L" (기본)" : L""), g_fValue.Get(), { x1 - 200, y, x1, y + 22 }, g_accent);
    float top = y + 26;
    if (c.hint) { Draw(c.hint, g_fHint.Get(), { x0, top - 2, x1, top + 16 }, Muted()); top += 18; }
    float cy = top + 16, t = (std::clamp(v, c.lo, c.hi) - c.lo) / (c.hi - c.lo);
    D2D1_RECT_F track = { x0 + 10, cy - 2, x1 - 10, cy + 2 };
    float kx = track.left + (track.right - track.left) * t;
    Fill(track, Wash(0.22f), 2);
    Fill({ track.left, track.top, kx, track.bottom }, g_accent, 2);
    if (!std::isnan(c.def)) {
        float dx = track.left + (track.right - track.left) * (std::clamp(c.def, c.lo, c.hi) - c.lo) / (c.hi - c.lo);
        Fill({ dx - 1, cy - 7, dx + 1, cy + 7 }, Muted(), 1);
        Draw(L"기본", g_fTiny.Get(), { dx - 30, cy + 10, dx + 30, cy + 25 }, Muted());
    }
    float kr = (hot || g_drag == ci) ? 10.f : 9.f;
    g_brush->SetColor(g_dark ? ColorF(0.27f, 0.27f, 0.29f) : ColorF(1, 1, 1));
    dc->FillEllipse({ { kx, cy }, kr, kr }, g_brush.Get());
    g_brush->SetColor(Wash(0.12f));
    dc->DrawEllipse({ { kx, cy }, kr, kr }, g_brush.Get(), 1);
    g_brush->SetColor(g_accent);
    float inner = g_drag == ci ? 4.5f : hot ? 6 : 5;
    dc->FillEllipse({ { kx, cy }, inner, inner }, g_brush.Get());
    if (ci < 32) g_sliderTrack[ci] = track;
    g_hots.push_back({ { x0, cy - 16, x1, cy + 16 }, ci, 0 });
}

void DrawToggle(ID2D1DeviceContext2* dc, const Control& c, int ci, bool hot, float x0, float x1, float y) {
    bool on = c.on();
    float sw = 44, sh = 22, tx = x1 - sw, ty = y + 6;
    Draw(c.label, g_fLabel.Get(), { x0, y, tx - 12, y + 22 }, Text());
    if (c.hint) Draw(c.hint, g_fHint.Get(), { x0, y + 24, tx - 12, y + 42 }, Muted());
    D2D1_RECT_F r = { tx, ty, tx + sw, ty + sh };
    if (!std::isnan(c.def)) Draw(c.def > 0.5f ? L"기본 켬" : L"기본 끔", g_fTiny.Get(), { tx - 8, ty + sh + 4, tx + sw + 8, ty + sh + 19 }, Muted());
    if (on) Fill(r, g_accent, sh / 2);
    else { Fill(r, Wash(hot ? 0.08f : 0.03f), sh / 2); g_brush->SetColor(Muted()); dc->DrawRoundedRectangle({ { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, sh / 2, sh / 2 }, g_brush.Get(), 1); }
    float kx = on ? r.right - sh / 2 : r.left + sh / 2, kr = hot ? 7.f : 6.f;
    g_brush->SetColor(on ? (g_dark ? ColorF(0, 0, 0, 0.85f) : ColorF(1, 1, 1)) : Muted());
    dc->FillEllipse({ { kx, ty + sh / 2 }, kr, kr }, g_brush.Get());
    g_hots.push_back({ { x0, y - 4, x1, y + RowHeight(c) - 12 }, ci, 0 });
}

// the whole header is the button; the chevron at its right end shows which way it goes. Its panel
// starts 10 DIPs down so it stays clear of the row above (the "기본" marks under a segmented control).
// Returns the y of the next row (the folded controls are skipped by Render).
float DrawGroupHeader(const Page& page, const Control& c, int ci, bool hot, float x0, float x1, float y) {
    const float top = y;
    y += 10;
    D2D1_RECT_F r = { x0 - 10, y - 10, x1 + 10, y + (c.hint ? 52.f : 32.f) };
    Fill(r, Wash(hot ? 0.09f : 0.05f), 8);
    Draw(c.label, g_fLabel.Get(), { x0, y, x1 - 40, y + 22 }, Text());
    if (c.hint) Draw(c.hint, g_fHint.Get(), { x0, y + 24, x1 - 40, y + 42 }, Muted());
    if (!c.open) {   // folded: say so when something inside is not at its default
        int changed = 0;
        for (int i = ci + 1; i <= ci + c.count && i < (int)page.controls.size(); ++i) {
            const Control& k = page.controls[i];
            if (k.type == Control::Slider && !std::isnan(k.def) && std::fabs(k.value() - k.def) >= k.step * 0.5f) ++changed;
            else if (k.type == Control::Toggle && k.hasDefault && !std::isnan(k.def) && (k.on() ? 1.f : 0.f) != k.def) ++changed;
        }
        if (changed) Draw(L"바꾼 값 " + Num((float)changed, 0, L"개"), g_fValue.Get(), { x1 - 200, y, x1 - 36, y + 22 }, g_accent);
    }
    Draw(c.open ? L"" : L"", g_fGlyph.Get(), { x1 - 22, r.top + 10, x1 + 4, r.bottom - 10 }, hot ? Text() : Text(0.8f));   // ChevronUp / ChevronDown
    g_hots.push_back({ r, ci, 0 });
    return top + RowHeight(c);
}

void DrawButtonRow(const Control& c, int ci, bool hot, float x0, float x1, float y) {
    Draw(c.label, g_fLabel.Get(), { x0, y, x1, y + 22 }, Text());
    if (c.hint) Draw(c.hint, g_fHint.Get(), { x0, y + 24, x1, y + 42 }, Muted());
    D2D1_RECT_F r = { x1 - 96, y + 2, x1, y + 34 };
    Fill(r, Wash(hot ? 0.12f : 0.07f), 6);
    Draw(L"종료", g_fSeg.Get(), r, Text());
    g_hots.push_back({ r, ci, 0 });
}

void Render() {
    g_drawLater = false;
    if (!g_target) return;
    auto* dc = g_gfx.dc.Get();
    float oldX, oldY; dc->GetDpi(&oldX, &oldY);
    dc->SetTarget(g_target.Get());
    // the shared context works in pixels for the docks; this window is laid out in DIPs
    auto oldUnits = dc->GetUnitMode();
    dc->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    dc->SetDpi(g_dpi, g_dpi);
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    // over the acrylic only a light tint (as the taskbar); opaque where there is no backdrop
    dc->Clear(g_acrylic ? (g_dark ? ColorF(0.11f, 0.11f, 0.12f, 0.30f) : ColorF(0.96f, 0.96f, 0.97f, 0.35f))
                        : (g_dark ? ColorF(0.13f, 0.13f, 0.14f) : ColorF(0.95f, 0.95f, 0.96f)));
    g_hots.clear();

    DrawNav();

    // page
    const Page& page = g_pages[g_page];
    float x0 = kNav + kPad, x1 = kW - kPad;
    dc->PushAxisAlignedClip({ kNav, 0, kW, kH }, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = 26 - g_scroll;
    Draw(page.title, g_fTitle.Get(), { x0, y, x1, y + 40 }, Text());
    y += 62;
    for (int ci = 0; ci < (int)page.controls.size(); ++ci) {
        const Control& c = page.controls[ci];
        if (!c.Shown()) continue;   // nothing drawn, no room, no hot area
        bool hot = g_hover == ci;
        switch (c.type) {
        case Control::Segmented: DrawSegmented(c, ci, hot, x0, x1, y); break;
        case Control::Slider: DrawSlider(dc, c, ci, hot, x0, x1, y); break;
        case Control::Toggle: DrawToggle(dc, c, ci, hot, x0, x1, y); break;
        case Control::Customizer:
            y += DrawCustomizer(x0, y, x1) + 8;
            continue;
        case Control::Group:
            y = DrawGroupHeader(page, c, ci, hot, x0, x1, y);
            if (!c.open) ci += c.count;   // its controls are not drawn: no hot areas, no tracks
            continue;
        case Control::Button: DrawButtonRow(c, ci, hot, x0, x1, y); break;
        }
        y += RowHeight(c);
    }
    if (page.reset) {
        y += 4;
        D2D1_RECT_F r = { x0, y, x0 + 150, y + 34 };
        Fill(r, Wash(g_hover == -2 ? 0.12f : 0.07f), 6);
        Draw(L"기본값으로", g_fSeg.Get(), r, Text());
        g_hots.push_back({ r, -2, 0 });
        y += 50;
    }
    g_contentH = y + g_scroll;
    dc->PopAxisAlignedClip();

    HRESULT hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    dc->SetDpi(oldX, oldY);
    dc->SetUnitMode(oldUnits);
    if (FAILED(hr)) { Log(L"settings EndDraw %08x", hr); return; }
    g_swap->Present(1, 0);
}

// ---------------------------------------------------------------- input

float ClientH() { RECT rc; GetClientRect(g_wnd, &rc); return rc.bottom * 96.f / g_dpi; }
void ClampScroll() { g_scroll = std::clamp(g_scroll, 0.0f, std::max(0.0f, g_contentH - ClientH())); }

void Changed(const Control& c, bool released) {
    switch (c.apply) {
    case Apply::Live: PostMessageW(g_mainWnd, WM_APP_RELAYOUT, 0, 0); break;
    case Apply::Rebuild: PostMessageW(g_mainWnd, WM_APP_REBUILD, 0, 0); break;
    case Apply::OnRelease: if (released) PostMessageW(g_mainWnd, WM_APP_REBUILD, 0, 0); break;
    case Apply::None: break;
    }
    if (released) g_settings.Save();
}

// The end of a slider drag (let go, capture lost, window closed): the value applied and saved as on a release. Before
// any ReleaseCapture: the WM_CAPTURECHANGED that sends finds no drag, so the value is not applied twice.
void EndSliderDrag() {
    Changed(g_pages[g_page].controls[g_drag], true);
    g_drag = -1;
}

void SetSlider(int ci, float x) {
    const Control& c = g_pages[g_page].controls[ci];
    const auto& t = g_sliderTrack[ci];
    float v = c.lo + std::clamp((x - t.left) / (t.right - t.left), 0.0f, 1.0f) * (c.hi - c.lo);
    v = c.lo + std::round((v - c.lo) / c.step) * c.step;
    v = std::clamp(std::round(v * 1000) / 1000, c.lo, c.hi);
    if (std::fabs(v - c.value()) < c.step * 0.01f) return;
    c.set(v);
    Changed(c, false);
}

const Hot* HotAt(float x, float y) {
    for (auto& h : g_hots) if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return &h;
    return nullptr;
}

// The end of a wheel message (changed: it scrolled the page or moved a slider): the page is drawn, unless another
// wheel message is already waiting - then that one draws. A touchpad sends one for every part of a notch, often
// several before the loop comes round, and Present waits for the screen's next frame once one is queued: drawn one
// by one they held the docks' loop a frame apiece, and the page fell behind the fingers. Until then the hot areas
// are the last frame's, so any other message draws first (Proc), and so does a wheel message that starts a gesture.
LRESULT WheelDrawn(HWND h, bool changed) {
    if (!changed && !g_drawLater) return 0;
    MSG next;
    if (PeekMessageW(&next, h, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_NOREMOVE | PM_NOYIELD | PM_QS_INPUT | PM_QS_POSTMESSAGE)) g_drawLater = true;
    else Render();
    return 0;
}

// The rows of a page Render gives room to now (its shown controls, a folded group's not; the customizer's own height
// left out: no press changes it). Press compares it before and after a value changed: a control that shows or hides
// others (Control::shown, e.g. '전력 소비량' and its two toggles) changes the page's length.
float ShownRowsH(const Page& page) {
    float h = 0;
    for (int ci = 0; ci < (int)page.controls.size(); ++ci) {
        const Control& c = page.controls[ci];
        if (!c.Shown() || c.type == Control::Customizer) continue;
        h += RowHeight(c);
        if (c.type == Control::Group && !c.open) ci += c.count;
    }
    return h;
}

void Press(const Hot& h, float x) {
    if (IsNav(h.control)) { g_page = -10 - h.control; g_scroll = 0; return; }
    if (h.control <= kHotDock) {   // a button in the preview or the list: may become a drag
        g_bd = {}; g_bd.armed = true; g_bd.fromDock = h.control > kHotPalette;
        g_bd.id = g_bd.fromDock ? ChosenButtons()[kHotDock - h.control] : (ButtonId)(kHotPalette - h.control);
        SetCapture(g_wnd);
        return;
    }
    const Page& page = g_pages[g_page];
    // a value changed may show or hide rows: the page's new length now, so the scroll is clamped before the one frame
    // drawn after the click (as a group's fold below; scrolled down, a frame at the old scroll would show the page jump)
    const float rowsBefore = ShownRowsH(page);
    auto relength = [&] {
        const float d = ShownRowsH(page) - rowsBefore;
        if (d != 0) { g_contentH += d; ClampScroll(); }
    };
    if (h.control == -2) { page.reset(); relength(); g_settings.Save(); PostMessageW(g_mainWnd, WM_APP_REBUILD, 0, 0); return; }
    const Control& c = page.controls[h.control];
    switch (c.type) {
    case Control::Segmented: if (c.index() != h.sub) { c.choose(h.sub); relength(); Changed(c, true); } break;
    case Control::Slider: g_drag = h.control; SetCapture(g_wnd); SetSlider(h.control, x); break;
    case Control::Toggle: c.toggle(!c.on()); relength(); Changed(c, true); break;
    case Control::Button: c.press(); break;
    case Control::Group: {
        c.open = !c.open;
        g_wheel = {};   // the page under the pointer changed: a new wheel gesture
        // the page's new length now, so the scroll is clamped before the one frame drawn after the click (folded
        // while scrolled down, a frame at the old scroll would show the page jump)
        float rows = 0;
        for (int i = h.control + 1; i <= h.control + c.count && i < (int)page.controls.size(); ++i)
            if (page.controls[i].Shown()) rows += RowHeight(page.controls[i]);
        g_contentH += c.open ? rows : -rows;
        ClampScroll();
        break;
    }
    }
}

// ---------------------------------------------------------------- size

// Only a close button: no minimize box (with no maximize box either, Windows shows neither). A minimized settings
// window kept every dock up with nothing on the screen to say why (seen 2026-11-06).
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, kExStyle = WS_EX_NOREDIRECTIONBITMAP;
HMONITOR g_scaledFor = nullptr;

// Same rule as the dock: sized by the screen, not by Windows' scale setting (1.0 = a 1440 px short side).
float ContentDpi(HMONITOR m) {
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(m, &mi);
    float shortSide = (float)std::min(mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    return 96 * std::max(0.72f, shortSide / 1440.0f);
}

// Scales the content for monitor m and resizes the window around it, keeping its top-left at (x, y)
// unless centre is set. The title bar keeps Windows' own size for that monitor.
void ScaleFor(HMONITOR m, int x, int y, bool centre) {
    g_scaledFor = m;
    g_dpi = ContentDpi(m);
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(m, &mi);
    float workH = (mi.rcWork.bottom - mi.rcWork.top) * 96.f / g_dpi - 60;
    RECT rc = { 0, 0, (LONG)std::ceil(kW * g_dpi / 96), (LONG)std::ceil(std::min(kH, workH) * g_dpi / 96) };
    AdjustWindowRectExForDpi(&rc, kStyle, FALSE, kExStyle, GetDpiForWindow(g_wnd));
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (centre) { x = (mi.rcWork.left + mi.rcWork.right - w) / 2; y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2; }
    SetWindowPos(g_wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_swap) { g_target.Reset(); CreateTarget(); ClampScroll(); Render(); }   // new DPI even if the size stayed
}

// ---- Proc's handlers for the pointer and the wheel. Each returns what Proc returns for its message (all of them
// end the message here: none goes on to DefWindowProc). h: the settings window; w, l: the message's.

// A pointer message's position, in DIPs.
void Dip(LPARAM p, float& x, float& y) { x = GET_X_LPARAM(p) * 96.f / g_dpi; y = GET_Y_LPARAM(p) * 96.f / g_dpi; }

LRESULT OnMouseMove(HWND h, LPARAM l) {
    float x, y; Dip(l, x, y);
    if (g_drag >= 0) { SetSlider(g_drag, x); Render(); return 0; }
    if (g_bd.armed) {
        g_bd.x = x; g_bd.y = y;
        if (!g_bd.active && std::hypot(x - g_bd.sx, y - g_bd.sy) > 4) g_bd.active = true;
        if (g_bd.active) Render();
        return 0;
    }
    const Hot* hot = HotAt(x, y);
    int nh = hot ? hot->control : -1, ns = hot ? hot->sub : -1;
    if (nh != g_hover || ns != g_hoverSub) { g_hover = nh; g_hoverSub = ns; Render(); }
    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 }; TrackMouseEvent(&tme);
    return 0;
}

LRESULT OnButtonDown(LPARAM l) {
    float x, y; Dip(l, x, y);
    if (const Hot* hot = HotAt(x, y)) { Hot copy = *hot; Press(copy, x); }
    if (g_bd.armed) { g_bd.sx = g_bd.x = x; g_bd.sy = g_bd.y = y; }
    Render();
    // a folded group can leave the page shorter than where it was scrolled
    if (const float was = g_scroll; (ClampScroll(), g_scroll != was)) Render();
    return 0;
}

LRESULT OnButtonUp(LPARAM l) {
    if (g_bd.armed) {   // drop, or a plain click on a listed button: add it at the end
        ButtonDrag d = g_bd;
        float x, y; Dip(l, x, y); g_bd.x = x; g_bd.y = y;
        std::vector<ButtonId> list = d.active ? ShownList() : ChosenButtons();
        if (!d.active && !d.fromDock) list.push_back(d.id);
        g_bd = {};
        ReleaseCapture();
        if (d.active || !d.fromDock) CommitButtons(list);
        Render();
        return 0;
    }
    if (g_drag >= 0) { EndSliderDrag(); ReleaseCapture(); Render(); }
    return 0;
}

LRESULT OnCaptureChanged(HWND h, LPARAM l) {
    if (g_bd.armed && (HWND)l != h) { g_bd = {}; Render(); }
    if (g_drag >= 0 && (HWND)l != h) { EndSliderDrag(); Render(); }
    return 0;
}

LRESULT OnWheel(HWND h, WPARAM w, LPARAM l) {
    const POINT at = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    const int delta = GET_WHEEL_DELTA_WPARAM(w);
    if (g_wheel.page != g_page || std::abs(at.x - g_wheel.at.x) > 2 || std::abs(at.y - g_wheel.at.y) > 2) {
        if (g_drawLater) Render();             // the hot areas of the page as it is now
        POINT p = at; ScreenToClient(h, &p);   // the pointer moved, or another page: a new gesture
        const Hot* hot = HotAt(p.x * 96.f / g_dpi, p.y * 96.f / g_dpi);
        g_wheel.slider = hot && hot->control >= 0 && g_pages[g_page].controls[hot->control].type == Control::Slider ? hot->control : -1;
        g_wheel.rest = 0;
    }
    g_wheel.at = at; g_wheel.page = g_page;
    if (g_wheel.slider >= 0) {
        // the wheel nudges the slider one step a notch; parts of a notch add up
        if ((g_wheel.rest < 0) != (delta < 0)) g_wheel.rest = 0;   // turned the other way
        g_wheel.rest += delta;
        const int notches = g_wheel.rest / WHEEL_DELTA;
        g_wheel.rest -= notches * WHEEL_DELTA;
        if (!notches) return WheelDrawn(h, false);   // no whole step yet (a value from settings.ini beyond the slider stays as it is)
        const Control& c = g_pages[g_page].controls[g_wheel.slider];
        const float v = std::round(std::clamp(c.value() + notches * c.step, c.lo, c.hi) * 1000) / 1000;
        if (std::fabs(v - c.value()) < c.step * 0.01f) return WheelDrawn(h, false);   // at its end: nothing to save or rebuild
        c.set(v);
        Changed(c, true);
    } else {
        const float was = g_scroll;
        g_scroll -= delta * 60.f / WHEEL_DELTA;   // in proportion, so a touchpad's parts of a notch scroll smoothly
        ClampScroll();
        if (g_scroll == was) return WheelDrawn(h, false);   // at the end of the page
    }
    return WheelDrawn(h, true);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_drawLater && m != WM_MOUSEWHEEL && m != WM_DESTROY) Render();   // WheelDrawn: everything else sees the page as it is now
    switch (m) {
    case WM_MOUSEMOVE: return OnMouseMove(h, l);
    case WM_MOUSELEAVE: if (g_hover != -1) { g_hover = g_hoverSub = -1; Render(); } return 0;
    case WM_LBUTTONDOWN: return OnButtonDown(l);
    case WM_LBUTTONUP: return OnButtonUp(l);
    case WM_RBUTTONUP: {
        float x, y; Dip(l, x, y);
        const Hot* hot = HotAt(x, y);
        if (hot && hot->control <= kHotDock) { POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &p); ButtonMenu(hot->control, p); Render(); }
        return 0;
    }
    case WM_CAPTURECHANGED: return OnCaptureChanged(h, l);
    case WM_MOUSEWHEEL: return OnWheel(h, w, l);
    case WM_KEYDOWN: if (w == VK_ESCAPE) DestroyWindow(h); return 0;
    case WM_ACTIVATE: case WM_SETTINGCHANGE: case WM_DWMCOLORIZATIONCOLORCHANGED:
        if (m != WM_ACTIVATE) ReadAccent();
        Render();
        break;
    case WM_DPICHANGED: {   // moved to a monitor with another scale: size for that monitor's resolution
        auto r = (RECT*)l;
        ScaleFor(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), r->left, r->top, false);
        return 0;
    }
    case WM_EXITSIZEMOVE: {   // moved to a monitor with the same scale but another resolution
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        if (mon != g_scaledFor) { RECT r; GetWindowRect(h, &r); ScaleFor(mon, r.left, r.top, false); }
        return 0;
    }
    case WM_SIZE:
        // Win+D or the dock's show-desktop button still minimizes it: then it closes (saved, like any close), so
        // no settings window is left open off the screen holding the docks up. Posted: not destroyed inside WM_SIZE.
        if (w == SIZE_MINIMIZED) {
            g_settingsHold.open = false;
            PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        if (g_swap) {
            g_target.Reset();
            g_swap->ResizeBuffers(0, std::max(1, (int)LOWORD(l)), std::max(1, (int)HIWORD(l)), DXGI_FORMAT_UNKNOWN, 0);
            CreateTarget(); ClampScroll(); Render();
        }
        return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Render(); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (g_drag >= 0) EndSliderDrag();
        g_settings.Save();
        g_settingsHold.open = false;
        g_settingsHold.wnd = nullptr;
        Release();
        g_wnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}   // namespace

bool IsOpen() { return g_wnd != nullptr; }

void PlaceBottomRight() {
    if (!g_wnd) return;
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT r; GetWindowRect(g_wnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    SetWindowPos(g_wnd, nullptr, mi.rcMonitor.right - w - 24, mi.rcMonitor.bottom - h - 24, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    POINT o = { 0, 0 }; ClientToScreen(g_wnd, &o);
    Log(L"settings placed: client origin %d,%d content dpi %.1f", o.x, o.y, g_dpi);
}
void Close() { if (g_wnd) DestroyWindow(g_wnd); }
bool PreTranslate(MSG* msg) {
    if (!g_wnd || msg->hwnd != g_wnd || msg->message != WM_KEYDOWN || msg->wParam != VK_ESCAPE) return false;
    if (g_bd.armed) { g_bd = {}; ReleaseCapture(); Render(); return true; }   // Esc cancels a drag first
    DestroyWindow(g_wnd);
    return true;
}

void Show(HMONITOR monitor) {
    if (g_wnd) { ShowWindow(g_wnd, SW_RESTORE); SetForegroundWindow(g_wnd); return; }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"LiquidDockSettings";
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        registered = true;
    }
    g_monitor = monitor;
    g_dark = !theme::IsLight();
    ReadAccent();
    BuildPages();
    g_page = 0; g_scroll = 0; g_hover = g_hoverSub = g_drag = -1; g_wheel = {};

    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(monitor, &mi);
    // created on its monitor first (so Windows picks that monitor's title bar size), then sized
    g_wnd = CreateWindowExW(kExStyle, L"LiquidDockSettings", L"Liquid Dock 설정", kStyle,
                            mi.rcWork.left + 40, mi.rcWork.top + 40, 200, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_wnd) return;
    ScaleFor(monitor, 0, 0, true);
    BOOL dark = g_dark;
    DwmSetWindowAttribute(g_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    // the acrylic of the taskbar and flyouts behind the whole window, title bar included (Windows 11)
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_wnd, &margins);
    int backdrop = 3 /* DWMSBT_TRANSIENTWINDOW */;
    g_acrylic = SUCCEEDED(DwmSetWindowAttribute(g_wnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof(backdrop)));
    if (!CreateResources()) { Log(L"settings window: no swap chain"); DestroyWindow(g_wnd); return; }
    g_settingsHold.open = true;
    g_settingsHold.wnd = g_wnd;
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    Render();
}

}   // namespace settingswindow
