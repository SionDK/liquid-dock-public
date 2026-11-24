#include "dock.h"
#include "system.h"
#include <d2d1effects.h>

struct GlassStyle { float sigma, tint, sat, refr, bezel, chroma, highlight, bright; };
static const GlassStyle kStyles[3] = {
    { 1.5f, 0.05f, 1.30f, 18, 18, 0.10f, 0.42f, 1.03f },   // clear
    { 6.0f, 0.10f, 1.40f, 16, 18, 0.08f, 0.36f, 1.04f },   // normal
    { 16.f, 0.20f, 1.30f, 12, 18, 0.05f, 0.30f, 1.05f },   // frosted
};

// ---------------------------------------------------------------- drawing

float Dock::InkAt(float u) const {
    float w = std::max(1.0f, barU1 - barU0);
    int i = std::clamp((int)((u - barU0) / w * 16), 0, 15);
    return inkCol[i];
}

D2D1_COLOR_F Dock::Ink(float u, float alpha) const {
    float k = InkAt(u);
    return D2D1::ColorF(lerpf(1.0f, 0.07f, k), lerpf(1.0f, 0.07f, k), lerpf(1.0f, 0.09f, k), alpha * lerpf(0.95f, 0.86f, k));
}

void Dock::DrawStartIcon(D2D1_RECT_F r, float alpha) {
    auto* dc = g_gfx.dc.Get();
    float sz = r.right - r.left;
    float rad = sz * 0.225f;
    D2D1_GRADIENT_STOP stops[2] = { { 0, D2D1::ColorF(0.30f, 0.70f, 1.0f, alpha) }, { 1, D2D1::ColorF(0.05f, 0.33f, 0.88f, alpha) } };
    D2D1_ROUNDED_RECT rr = { r, rad, rad };
    if (auto* gb = g_gfx.GradientBrush(stops, 2, { r.left, r.top }, { r.right, r.bottom })) dc->FillRoundedRectangle(rr, gb);   // kept while alpha stays
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.28f * alpha));
    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{ { r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f }, rad, rad }, brush.Get(), 1.0f);
    float lg = sz * 0.42f, g = sz * 0.03f, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    float q = (lg - g) / 2;
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.97f * alpha));
    for (int i = 0; i < 4; ++i) {
        float x = cx - lg / 2 + (i % 2) * (q + g), y = cy - lg / 2 + (i / 2) * (q + g);
        dc->FillRectangle({ x, y, x + q, y + q }, brush.Get());
    }
}

void Dock::DrawIcon(Slot& s, float u, float vb, float size, float alpha) {
    auto* dc = g_gfx.dc.Get();
    D2D1_RECT_F r = RectUV(u - size / 2, u + size / 2, vb, vb + size);
    if (!s.app) { DrawStartIcon(r, alpha); if (s.press.x > 0.02f) { brush->SetColor(D2D1::ColorF(0, 0, 0, 0.3f * s.press.x)); dc->FillRoundedRectangle({ r, size * 0.225f, size * 0.225f }, brush.Get()); } return; }
    auto* bmp = s.app->icon.Get();
    if (!bmp) {
        brush->SetColor(D2D1::ColorF(0.6f, 0.6f, 0.65f, 0.9f * alpha));
        dc->FillRoundedRectangle({ r, size * 0.22f, size * 0.22f }, brush.Get());
        return;
    }
    if (s.press.x > 0.02f) {
        float k = 1 - 0.45f * s.press.x;
        D2D1_MATRIX_5X4_F mtx = { k,0,0,0, 0,k,0,0, 0,0,k,0, 0,0,0,alpha, 0,0,0,0 };
        darken->SetInput(0, bmp);
        darken->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, mtx);
        auto bs = bmp->GetPixelSize();
        dc->SetTransform(D2D1::Matrix3x2F::Scale(size / bs.width, size / bs.height) * D2D1::Matrix3x2F::Translation(r.left, r.top));
        dc->DrawImage(darken.Get(), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
    } else {
        dc->DrawBitmap(bmp, r, alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    }
}

void Dock::DrawTooltip(const std::wstring& text, float u, float vBase) {
    auto* dc = g_gfx.dc.Get();
    float a = tooltipAlpha.x;
    if (a < 0.01f || text.empty()) return;
    float w = TextWidth(g_gfx.fmtTooltip.Get(), text) + 24 * scale;
    float h = 28 * scale;
    bool vert = IsVertical(g_settings.edge);
    float across = vert ? h : w, depth = vert ? w : h;
    u = clampf(u, across / 2 + 4 * scale, len - across / 2 - 4 * scale);
    float lift = (1 - a) * 6 * scale;
    D2D1_RECT_F r = RectUV(u - across / 2, u + across / 2, vBase - lift, vBase - lift + depth);
    D2D1_ROUNDED_RECT rr = { r, h / 2, h / 2 };
    brush->SetColor(D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.80f * a));
    dc->FillRoundedRectangle(rr, brush.Get());
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.16f * a));
    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{ { r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f }, h / 2, h / 2 }, brush.Get(), 1);
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.96f * a));
    dc->DrawTextW(text.c_str(), (UINT32)text.size(), g_gfx.fmtTooltip.Get(), r, brush.Get());
}

void Dock::Render() {
    if (!dirty || !swap) return;
    g_gfx.EnsureFonts(scale);
    dirty = false;
    bool peeking = sbPeek.x > 0.001f;
    for (auto& s : slots) if (s.peek.x > 0.001f) peeking = true;
    bool glassVisible = reveal.x > 0.002f;
    if (!glassVisible && !peeking) {
        if (clearedWhileHidden) return;
        float zero[4] = { 0, 0, 0, 0 };
        g_gfx.ctx->ClearRenderTargetView(rtv.Get(), zero);
        if (rec.recording) g_gfx.ctx->ClearRenderTargetView(rec.dockFrame.rtv.Get(), zero);
        swap->Present(0, 0);
        clearedWhileHidden = true;
        return;
    }
    clearedWhileHidden = false;

    float zero[4] = { 0, 0, 0, 0 };
    g_gfx.ctx->ClearRenderTargetView(rtv.Get(), zero);

    DrawGlass(glassVisible);

    auto* dc = g_gfx.dc.Get();
    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    float vis = clampf(reveal.x * 1.4f, 0, 1);

    DrawSlotIcons(dc, vis);
    if (vis > 0.01f) DrawBarContents(dc, vis);
    DrawOverlays(glassVisible);

    HRESULT hr = dc->EndDraw();
    dc->SetTarget(nullptr);
    if (FAILED(hr)) Log(L"EndDraw %08x", hr);
    if (snapshotPending) { snapshotPending = false; SaveSnapshot(); }
    if (rec.recording) {
        ComPtr<ID3D11Texture2D> bb; swap->GetBuffer(0, IID_PPV_ARGS(&bb));
        g_gfx.ctx->CopyResource(rec.dockFrame.tex.Get(), bb.Get());
    }
    swap->Present(0, 0);
}

// the backdrop blurred, the bar's glass, the luminance probe for the ink, the panel's glass over it
void Dock::DrawGlass(bool glassVisible) {
    const GlassStyle& gs = kStyles[std::clamp(g_settings.glassStyle, 0, 2)];
    D2D1_RECT_F bar = RectUV(barU0, barU1, BarV0(), BarV1());
    if (glassVisible && (capture.gotFirstFrame || glassKept)) {
        GlassCB c = {};
        c.misc[0] = (float)winW; c.misc[1] = (float)winH;
        if (backdropDirty) {
            float sigmaHalf = std::max(0.5f, gs.sigma * scale * 0.5f);
            int taps = std::min(32, (int)std::ceil(sigmaHalf * 2.5f));
            g_gfx.UpdateCB(c);
            g_gfx.FullscreenPass(halfA.rtv.Get(), halfA.w, halfA.h, g_gfx.psDown.Get(), backdrop.srv.Get(), nullptr);
            c.blur[0] = 1.0f / halfA.w; c.blur[1] = 0; c.blur[2] = sigmaHalf; c.blur[3] = (float)taps;
            g_gfx.UpdateCB(c);
            g_gfx.FullscreenPass(halfB.rtv.Get(), halfB.w, halfB.h, g_gfx.psBlur.Get(), halfA.srv.Get(), nullptr);
            c.blur[0] = 0; c.blur[1] = 1.0f / halfA.h;
            g_gfx.UpdateCB(c);
            g_gfx.FullscreenPass(halfA.rtv.Get(), halfA.w, halfA.h, g_gfx.psBlur.Get(), halfB.srv.Get(), nullptr);
            backdropDirty = false;
        }
        c.rect[0] = bar.left; c.rect[1] = bar.top; c.rect[2] = bar.right - bar.left; c.rect[3] = bar.bottom - bar.top;
        c.shape[0] = barThick * 0.40f;
        c.shape[1] = gs.bezel * scale;
        c.shape[2] = gs.refr * scale;
        c.shape[3] = clampf(reveal.x * 1.6f, 0, 1);
        c.look[0] = gs.chroma; c.look[1] = gs.tint; c.look[2] = gs.sat; c.look[3] = gs.highlight;
        c.misc[2] = 0.20f; c.misc[3] = 9 * scale;
        c.tint[0] = 1; c.tint[1] = 1; c.tint[2] = 1; c.tint[3] = gs.bright;
        g_gfx.UpdateCB(c);
        g_gfx.FullscreenPass(rtv.Get(), winW, winH, g_gfx.psGlass.Get(), nullptr, halfA.srv.Get());

        // luminance probe (read back one frame late, never stalls)
        g_gfx.FullscreenPass(lumRT.rtv.Get(), 16, 1, g_gfx.psLum.Get(), nullptr, halfA.srv.Get());
        g_gfx.ctx->CopyResource(lumStage[lumFrame & 1].Get(), lumRT.tex.Get());
        D3D11_MAPPED_SUBRESOURCE m;
        if (lumFrame > 0 && SUCCEEDED(g_gfx.ctx->Map(lumStage[(lumFrame + 1) & 1].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            const uint32_t* px = (const uint32_t*)m.pData;
            for (int i = 0; i < 16; ++i) {
                uint32_t p = px[i];
                float r = ((p >> 16) & 255) / 255.f, g = ((p >> 8) & 255) / 255.f, b = (p & 255) / 255.f;
                lumCol[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            }
            g_gfx.ctx->Unmap(lumStage[(lumFrame + 1) & 1].Get(), 0);
        }
        lumFrame++;

        // system monitor popover: darker liquid glass blended over the dock (its colour from before mod.30, kept as it
        // was; the corners follow the new layout's cards)
        if (panelAlpha.x > 0.01f) {
            GlassCB p = c;
            p.rect[0] = panelRect.left; p.rect[1] = panelRect.top;
            p.rect[2] = panelRect.right - panelRect.left; p.rect[3] = panelRect.bottom - panelRect.top;
            p.shape[0] = 16 * scale; p.shape[1] = 12 * scale; p.shape[2] = 8 * scale;
            p.shape[3] = clampf(panelAlpha.x, 0, 1);
            p.look[0] = 0.05f; p.look[1] = 0.42f; p.look[2] = 1.25f; p.look[3] = 0.28f;
            p.misc[2] = 0.28f; p.misc[3] = 14 * scale;
            p.tint[0] = 0.06f; p.tint[1] = 0.06f; p.tint[2] = 0.08f; p.tint[3] = 0.92f;
            g_gfx.UpdateCB(p);
            g_gfx.FullscreenPass(rtv.Get(), winW, winH, g_gfx.psGlass.Get(), nullptr, halfA.srv.Get(), true);
        }
    }
}

// the apps' icons (a peeking one over the screen edge) and their running / attention dots
void Dock::DrawSlotIcons(ID2D1DeviceContext2* dc, float vis) {
    for (size_t i = 0; i < slots.size(); ++i) {
        auto& s = slots[i];
        if (dragging && (int)i == pressSlot) continue;
        float sz = s.size * clampf(s.appear.x, 0, 1.2f) * clampf(s.shrink.x, 0, 1);
        if (sz < 1) continue;
        float vNormal = margin + pad - hideOff + s.bounceOff;
        float vb = vNormal;
        float alpha = vis;
        if (s.peek.x > 0.001f) {
            float vPeek = lerpf(-s.size - 10 * scale, margin + 2 * scale, clampf(s.peek.x, 0, 1.1f)) + s.bounceOff;
            if (vPeek > vb) { vb = vPeek; alpha = 1; }
        }
        if (alpha < 0.01f) continue;
        DrawIcon(s, s.u, vb, sz, s.ghost ? alpha * clampf(s.appear.x, 0, 1) : alpha);

        // running / attention indicator
        if (s.app && s.app->Running() && vis > 0.01f) {
            float rr = 2.3f * scale;
            auto p = W(s.u, margin + pad * 0.42f - hideOff);
            bool att = s.app->attention;
            float k = InkAt(s.u);
            brush->SetColor(D2D1::ColorF(0, 0, 0, 0.30f * vis * (1 - k)));
            dc->FillEllipse(D2D1::Ellipse(p, rr + 1, rr + 1), brush.Get());
            brush->SetColor(att ? D2D1::ColorF(1.0f, 0.55f, 0.10f, vis) : Ink(s.u, vis));
            dc->FillEllipse(D2D1::Ellipse(p, rr, rr), brush.Get());
        }
    }
}

// what sits on the bar (Render: while it shows): separators, the hover highlight, the cells, the tray icons,
// the clock, the small buttons
void Dock::DrawBarContents(ID2D1DeviceContext2* dc, float vis) {
    float v0 = BarV0(), v1 = BarV1();
    // separator
    float su = sepU + gap * 0.5f;
    auto sr = RectUV(su, su + 1, v0 + pad * 1.2f, v1 - pad * 1.2f);
    auto sc = Ink(su, vis); sc.a *= 0.42f;
    brush->SetColor(sc);
    dc->FillRectangle(sr, brush.Get());
    if (widgetWidth.x > 1) {   // after FluentFlyout's widget, fading in as its slot opens
        float wu = widgetU1 + gap * 0.5f;
        auto wc = Ink(wu, vis); wc.a *= 0.42f * clampf(widgetWidth.x / (gap * 2 + 1), 0, 1);
        brush->SetColor(wc);
        dc->FillRectangle(RectUV(wu, wu + 1, v0 + pad * 1.2f, v1 - pad * 1.2f), brush.Get());
    }

    DrawHoverHighlight(dc, v0, v1, vis);
    DrawSensorCell(v0, v1, vis);
    DrawBatteryCell(v0, v1, vis);

    // "show hidden icons" chevron points away from the screen edge
    const wchar_t* chevG = g_settings.edge == Edge::Bottom ? L"" : g_settings.edge == Edge::Top ? L""
                         : g_settings.edge == Edge::Left ? L"" : L"";
    const Cell& tray = cells[HitTray];
    DrawShadowText(chevG, g_gfx.fmtGlyph.Get(), RectUV(tray.u0, tray.u1, v0, v1), tray.Mid(), vis * 0.85f);
    DrawNotifyIcons(vis);
    const Cell& clk = cells[HitClock];
    if (!IsVertical(g_settings.edge)) {
        auto r = RectUV(clk.u0, clk.u1, v0, v1);
        float timeH = TextSize(ClockFont(), ClockTime()).height;
        float dateH = TextSize(ClockFont(true), g_sys.dateText).height;
        float top = (r.top + r.bottom - timeH - dateH) * 0.5f;
        DrawShadowText(ClockTime(), ClockFont(), { r.left, top, r.right, top + timeH }, clk.Mid(), vis);
        DrawShadowText(g_sys.dateText, ClockFont(true), { r.left, top + timeH, r.right, top + timeH + dateH }, clk.Mid(), vis);
    } else {
        DrawShadowText(g_sys.timeShort, ClockFont(), RectUV(clk.u0, clk.u1, v0, v1), clk.Mid(), vis);
    }
    // the small buttons: the same drawing as the settings preview (buttons.cpp)
    const IconState st = CurrentIconState(NowSeconds());
    for (int i = 0; i < kButtonCount; ++i) {
        auto r = ButtonRect((ButtonId)i);
        if (r.right <= r.left) continue;
        float u = Cell_((ButtonId)i).Mid(), ink = InkAt(u);
        DrawButtonIcon((ButtonId)i, dc, brush.Get(), g_gfx.fmtGlyph.Get(), r, scale, ButtonIconScale(), Ink(u, vis),
                       ink < 0.95f ? 0.35f * vis * (1 - ink) : 0, st);
    }
}

void Dock::DrawHoverHighlight(ID2D1DeviceContext2* dc, float v0, float v1, float vis) {
    // hover highlight
    D2D1_RECT_F hr{}; float hu = 0;
    if (hover.kind == HitButton) {
        hr = ButtonRect(hover.button);
        float inset = (stackButtons ? 1.5f : 2.0f) * scale;
        hr.left += inset; hr.right -= inset; hr.top += inset; hr.bottom -= inset;
        hu = Cell_(hover.button).Mid();
    } else if (hover.kind == HitNotify && NotifyKey(hover.notify)) {
        hr = NotifyRect(hover.notify);
        float inset = (notifyCells[hover.notify].half >= 0 ? 1.5f : 2.0f) * scale;
        hr.left += inset; hr.right -= inset; hr.top += inset; hr.bottom -= inset;
        hu = notifyCells[hover.notify].Mid();
    } else if (hover.kind == HitTray || hover.kind == HitClock || hover.kind == HitSensor || hover.kind == HitBattery) {
        const Cell& c = cells[hover.kind];
        hr = RectUV(c.u0 + 1, c.u1 - 1, v0 + pad * 0.6f, v1 - pad * 0.6f);
        hu = c.Mid();
    }
    if (hr.right > hr.left) {
        float ink = InkAt(hu);
        brush->SetColor(D2D1::ColorF(1 - ink, 1 - ink, 1 - ink, lerpf(0.16f, 0.08f, ink) * vis));
        dc->FillRoundedRectangle({ hr, 8 * scale, 8 * scale }, brush.Get());
    }
}

// over everything: the dragged icon, the tooltip, the panel, the overheat peek
void Dock::DrawOverlays(bool glassVisible) {
    // dragged icon follows the cursor
    if (dragging && pressSlot >= 0 && pressSlot < (int)slots.size()) {
        auto& s = slots[pressSlot];
        float vb = clampf(cursorV - base / 2, -base, T - base);
        DrawIcon(s, cursorU, vb, base, dropRemove ? 0.45f : 0.95f);
    }

    DrawTooltip(tooltipText, tooltipU, tooltipV);
    if (panelAlpha.x > 0.01f && glassVisible) DrawPanel();
    if (sbPeek.x > 0.001f) DrawSensorPeek();
}

// Debug: write (dock frame composited over the captured backdrop) to %APPDATA%\LiquidDock\snap_N.png
void Dock::SaveSnapshot() {
    auto readback = [&](ID3D11Texture2D* src, std::vector<uint32_t>& out) {
        D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> st;
        if (FAILED(g_gfx.d3d->CreateTexture2D(&d, nullptr, &st))) return;
        g_gfx.ctx->CopyResource(st.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_gfx.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        out.resize(d.Width * d.Height);
        for (UINT y = 0; y < d.Height; ++y) memcpy(&out[y * d.Width], (BYTE*)m.pData + y * m.RowPitch, d.Width * 4);
        g_gfx.ctx->Unmap(st.Get(), 0);
    };
    ComPtr<ID3D11Texture2D> bb; swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    std::vector<uint32_t> fg, bg;
    readback(bb.Get(), fg);
    readback(backdrop.tex.Get(), bg);
    if (fg.size() != (size_t)winW * winH) return;
    if (bg.size() != fg.size()) bg.assign(fg.size(), 0xff808080);
    std::vector<uint32_t> rawBg = bg;
    for (size_t i = 0; i < fg.size(); ++i) {
        uint32_t f = fg[i], b = bg[i], a = f >> 24, out = 0xff000000;
        for (int sh = 0; sh < 24; sh += 8) {
            uint32_t c = ((f >> sh) & 255) + ((b >> sh) & 255) * (255 - a) / 255;
            out |= std::min(c, 255u) << sh;
        }
        fg[i] = out;
    }
    auto save = [&](const std::wstring& path, std::vector<uint32_t>& px) {
        for (auto& p : px) p |= 0xff000000;
        ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> frame;
        g_gfx.wic->CreateStream(&stream);
        if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return;
        g_gfx.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
        enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        enc->CreateNewFrame(&frame, nullptr);
        frame->Initialize(nullptr);
        frame->SetSize(winW, winH);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&fmt);
        frame->WritePixels(winH, winW * 4, (UINT)(px.size() * 4), (BYTE*)px.data());
        frame->Commit();
        enc->Commit();
    };
    save(AppDataDir() + L"\\snap_" + std::to_wstring(index) + L".png", fg);
    save(AppDataDir() + L"\\snap_bg_" + std::to_wstring(index) + L".png", rawBg);
    Log(L"snapshot saved; layout (screen) edge=%d win=%d,%d bar=%.0f-%.0f apps-end=%.0f sens=%.0f-%.0f thick=%.0f margin=%.0f",
        (int)g_settings.edge, winX, winY, barU0, barU1, sepU, cells[HitSensor].u0, cells[HitSensor].u1, barThick, margin);
    for (auto& s : slots) if (s.app && s.app->exe.find(L"kakaotalk") != std::wstring::npos) Log(L"kakao u=%.0f", s.u);
    for (int i = 0; i < kButtonCount; ++i) {   // where each button is on screen (recordings script their clicks)
        auto r = ButtonRect((ButtonId)i);
        if (r.right > r.left) Log(L"button %S at %.0f,%.0f", Info((ButtonId)i).key, winX + (r.left + r.right) / 2, winY + (r.top + r.bottom) / 2);
    }
}

void Dock::DrawShadowText(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, float u, float a) {
    DrawShadowText(t, f, r, InkAt(u), a, Ink(u, a));
}

// Text in `color` over its shadow: black, one scale's pixel below, at 0.35 of `a` - less over a light backdrop (ink:
// InkAt where it is), none over the lightest (ink 0.95 and up). `a` is the cell's alpha, not the text's own (the
// sensor cell's labels are dimmer than their shadow).
void Dock::DrawShadowText(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, float ink, float a, D2D1_COLOR_F color) {
    auto* dc = g_gfx.dc.Get();
    if (ink < 0.95f) {
        brush->SetColor(D2D1::ColorF(0, 0, 0, 0.35f * a * (1 - ink)));
        D2D1_RECT_F rs = { r.left, r.top + scale, r.right, r.bottom + scale };
        dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, rs, brush.Get());
    }
    brush->SetColor(color);
    dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, brush.Get());
}

