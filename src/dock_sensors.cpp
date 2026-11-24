#include "dock.h"
#include "system.h"
#include "sensors.h"

std::wstring TempStr(float t, bool unit) {
    wchar_t b[16];
    if (std::isnan(t)) wcscpy_s(b, unit ? L"--°C" : L"--°");
    else swprintf_s(b, unit ? L"%d°C" : L"%d°", (int)std::lround(t));
    return b;
}

D2D1_COLOR_F SevColor(int sev, D2D1_COLOR_F normal, float a) {
    if (sev == 2) return D2D1::ColorF(1.0f, 0.34f, 0.30f, a);
    if (sev == 1) return D2D1::ColorF(1.0f, 0.66f, 0.22f, a);
    return normal;
}

void Dock::DrawSensorCell(float v0, float v1, float vis) {
    if (sbPeek.x < 0.001f) { v0 += sbOff; v1 += sbOff; }   // overheat hop while the dock is open
    float cu = cells[HitSensor].Mid();
    float ink = InkAt(cu);
    auto draw = [&](const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F col) { DrawShadowText(t, f, r, ink, vis, col); };
    // temperature when known, otherwise the load: drawn from the cell's key only (Dock::Update redraws for a
    // sensor sample when it changes, sensors.h cellVersion). The GPU hidden (key.gpuLine false): the CPU's line
    // alone, centred in the cell, whose width stays.
    const sensorlogic::CellKey key = g_sensors.CellKeyNow();
    const std::wstring cs = sensorlogic::CellText(key.cpu), gs = sensorlogic::CellText(key.gpu);
    auto cc = SevColor(key.cpu.sev, Ink(cu, vis), vis);
    auto gc = SevColor(key.gpu.sev, Ink(cu, vis), vis);
    if (!IsVertical(g_settings.edge)) {
        float mid = (v0 + v1) * 0.5f;
        if (!key.gpuLine) {
            draw(L"CPU " + cs, SensorFont(SensorText::Line), RectUV(cells[HitSensor].u0, cells[HitSensor].u1, mid - 8.5f * scale, mid + 8.5f * scale), cc);
            return;
        }
        draw(L"CPU " + cs, SensorFont(SensorText::Line), RectUV(cells[HitSensor].u0, cells[HitSensor].u1, mid, mid + 17 * scale), cc);
        draw(L"GPU " + gs, SensorFont(SensorText::Line), RectUV(cells[HitSensor].u0, cells[HitSensor].u1, mid - 17 * scale, mid), gc);
    } else {
        // four slots (label, value, label, value) in the middle 4/4.4 of the cell; the CPU's two alone, centred
        const float len = cells[HitSensor].u1 - cells[HitSensor].u0, h = len / 4.4f;
        float u = cells[HitSensor].u0 + (key.gpuLine ? h * 0.2f : (len - 2 * h) * 0.5f);
        auto lab = Ink(cu, vis * 0.62f);
        draw(L"CPU", SensorFont(SensorText::Label), RectUV(u, u + h, v0, v1), lab); u += h;
        draw(cs, SensorFont(SensorText::Value), RectUV(u, u + h, v0, v1), cc); u += h;
        if (!key.gpuLine) return;
        draw(L"GPU", SensorFont(SensorText::Label), RectUV(u, u + h, v0, v1), lab); u += h;
        draw(gs, SensorFont(SensorText::Value), RectUV(u, u + h, v0, v1), gc);
    }
}

// The battery after the sensor cell: its icon (larger than the small buttons') over the percentage,
// both in the dock's ink - the charge green on mains power, orange or red when low.
void Dock::DrawBatteryCell(float v0, float v1, float vis) {
    const Cell& c = cells[HitBattery];
    if (c.u1 - c.u0 < 1) return;
    const battery::State st = g_sys.Battery();
    if (!st.present) return;
    auto* dc = g_gfx.dc.Get();
    const D2D1_RECT_F r = RectUV(c.u0, c.u1, v0, v1);
    const float u = c.Mid(), ink = InkAt(u);
    const float iw = BatteryIconWidth(base, scale), ih = BatteryIconHeight(base, scale);
    const std::wstring pct = battery::PercentText(st);
    const float th = TextSize(g_gfx.fmtClockSmall.Get(), L"100%").height, gapY = 2 * scale;
    const float top = std::round((r.top + r.bottom - (ih + gapY + th)) * 0.5f), left = std::round((r.left + r.right - iw) * 0.5f);
    const float shadow = ink < 0.95f ? 0.35f * vis * (1 - ink) : 0;
    DrawBatteryIcon(dc, brush.Get(), { left, top, left + iw, top + ih }, scale, Ink(u, vis), shadow, st);
    const D2D1_RECT_F tr = { r.left, top + ih + gapY, r.right, top + ih + gapY + th };
    // its shadow as the icon's: ink < 0.95 is shadow > 0 here (drawn only when vis > 0.01, Render)
    DrawShadowText(pct, g_gfx.fmtClockSmall.Get(), tr, ink, vis,
                   battery::LevelOf(st) == battery::Level::Normal ? Ink(u, vis) : BatteryColor(st, Ink(u, vis)));
}

void Dock::DrawSensorPeek() {
    auto* dc = g_gfx.dc.Get();
    using M = SensorClient::Metric;
    // hottest metric relative to its threshold (what the sensor settings hide is not among them)
    int best = M::CPU; float bestR = -1;
    for (int m : { (int)M::CPU, (int)M::GPU, (int)M::SSD }) {
        float t = g_sensors.ShownTemp((M)m);
        if (std::isnan(t)) continue;
        float r = t / g_sensors.Threshold((M)m);
        if (r > bestR) { bestR = r; best = m; }
    }
    const wchar_t* names[] = { L"CPU", L"GPU", L"RAM", L"SSD" };
    bool vert = IsVertical(g_settings.edge);
    float across = vert ? 52 * scale : 112 * scale, depth = vert ? 84 * scale : 40 * scale;
    float cu = cells[HitSensor].Mid();
    float vb = lerpf(-depth - 12 * scale, margin + 2 * scale, clampf(sbPeek.x, 0, 1.1f)) + sbOff;
    D2D1_RECT_F r = RectUV(cu - across / 2, cu + across / 2, vb, vb + depth);
    float rad = std::min(r.right - r.left, r.bottom - r.top) * 0.5f;
    brush->SetColor(D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.92f));
    dc->FillRoundedRectangle({ r, rad, rad }, brush.Get());
    brush->SetColor(D2D1::ColorF(1, 0.4f, 0.35f, 0.55f));
    dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{ { r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f }, rad, rad }, brush.Get(), 1.2f * scale);
    auto col = SevColor(std::max(1, g_sensors.Severity((M)best)), D2D1::ColorF(1, 1, 1), 1);
    brush->SetColor(col);
    std::wstring t = vert ? std::wstring(names[best]) + L"\n" + TempStr(g_sensors.ShownTemp((M)best))
                          : std::wstring(names[best]) + L" " + TempStr(g_sensors.ShownTemp((M)best));
    g_gfx.fmtClock->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    dc->DrawTextW(t.c_str(), (UINT32)t.size(), g_gfx.fmtClock.Get(), r, brush.Get());
    g_gfx.fmtClock->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}
