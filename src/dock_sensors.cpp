#include "dock.h"
#include "system.h"
#include "sensors.h"
#include "oled.h"
#include "theme.h"
#include <d2d1effects.h>

static std::wstring TempStr(float t, bool unit = false) {
    wchar_t b[16];
    if (std::isnan(t)) wcscpy_s(b, unit ? L"--°C" : L"--°");
    else swprintf_s(b, unit ? L"%d°C" : L"%d°", (int)std::lround(t));
    return b;
}

static D2D1_COLOR_F SevColor(int sev, D2D1_COLOR_F normal, float a) {
    if (sev == 2) return D2D1::ColorF(1.0f, 0.34f, 0.30f, a);
    if (sev == 1) return D2D1::ColorF(1.0f, 0.66f, 0.22f, a);
    return normal;
}

void Dock::DrawSensorCell(float v0, float v1, float vis) {
    auto* dc = g_gfx.dc.Get();
    if (sbPeek.x < 0.001f) { v0 += sbOff; v1 += sbOff; }   // overheat hop while the dock is open
    float cu = (cells[HitSensor].u0 + cells[HitSensor].u1) * 0.5f;
    float ink = InkAt(cu);
    auto draw = [&](const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F col) {
        if (ink < 0.95f) {
            brush->SetColor(D2D1::ColorF(0, 0, 0, 0.35f * vis * (1 - ink)));
            D2D1_RECT_F rs = { r.left, r.top + scale, r.right, r.bottom + scale };
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, rs, brush.Get());
        }
        brush->SetColor(col);
        dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, brush.Get());
    };
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
            draw(L"CPU " + cs, g_gfx.fmtClockSmall.Get(), RectUV(cells[HitSensor].u0, cells[HitSensor].u1, mid - 8.5f * scale, mid + 8.5f * scale), cc);
            return;
        }
        draw(L"CPU " + cs, g_gfx.fmtClockSmall.Get(), RectUV(cells[HitSensor].u0, cells[HitSensor].u1, mid, mid + 17 * scale), cc);
        draw(L"GPU " + gs, g_gfx.fmtClockSmall.Get(), RectUV(cells[HitSensor].u0, cells[HitSensor].u1, mid - 17 * scale, mid), gc);
    } else {
        // four slots (label, value, label, value) in the middle 4/4.4 of the cell; the CPU's two alone, centred
        const float len = cells[HitSensor].u1 - cells[HitSensor].u0, h = len / 4.4f;
        float u = cells[HitSensor].u0 + (key.gpuLine ? h * 0.2f : (len - 2 * h) * 0.5f);
        auto lab = Ink(cu, vis * 0.62f);
        draw(L"CPU", g_gfx.fmtTiny.Get(), RectUV(u, u + h, v0, v1), lab); u += h;
        draw(cs, g_gfx.fmtClock.Get(), RectUV(u, u + h, v0, v1), cc); u += h;
        if (!key.gpuLine) return;
        draw(L"GPU", g_gfx.fmtTiny.Get(), RectUV(u, u + h, v0, v1), lab); u += h;
        draw(gs, g_gfx.fmtClock.Get(), RectUV(u, u + h, v0, v1), gc);
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
    if (shadow > 0) {
        brush->SetColor(D2D1::ColorF(0, 0, 0, shadow));
        dc->DrawTextW(pct.c_str(), (UINT32)pct.size(), g_gfx.fmtClockSmall.Get(), { tr.left, tr.top + scale, tr.right, tr.bottom + scale }, brush.Get());
    }
    brush->SetColor(battery::LevelOf(st) == battery::Level::Normal ? Ink(u, vis) : BatteryColor(st, Ink(u, vis)));
    dc->DrawTextW(pct.c_str(), (UINT32)pct.size(), g_gfx.fmtClockSmall.Get(), tr, brush.Get());
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
    float cu = (cells[HitSensor].u0 + cells[HitSensor].u1) * 0.5f;
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

// ---- the "PC 상태" panel ------------------------------------------------------------------------------------
// Laid out in DIP (times scale) after the reference picture (926 x 671 px, about 2.06 px a DIP at 450 DIP):
//   header: "PC 상태", what it covers                   0 .. 55
//   CPU and GPU cards side by side                      55 .. 139  (temperature, model, load and its bar;
//                                                                   the GPU hidden: the CPU's card the whole width)
//   RAM: used / total, load, bar                        139 .. 183
//   memory modules, two a row                           27 a row
//   VRAM (a GPU with memory of its own): used / total,  44, as the RAM's line and bar (in the GPU's colour),
//     load, bar; its temperature                        + 27, a row as a memory module's
//   a divider (drives after memory rows or VRAM)        8
//   drives, two a row                                   27 a row
//   footer: how many report a temperature, the pace     33
// On a vertical dock 320 DIP wide, one module / drive a row (Dock::PanelWidthDip, dock.h). Boxes, cards and
// bars on whole pixels from the panel's corner (ComputePanelRect rounds it): sharp at 125 and 150 %.
// What the sensor settings hide (sensorlogic::ShowItems) is left out: the GPU's card, the VRAM block or its
// temperature row, the module rows, the drives; the CPU card's temperature (its place left blank). The header,
// the footer and the heights follow (sensor_logic.h: PanelHeader, PanelFooter, PanelHeightDip, FitPanel).
namespace panelui {
constexpr float kPad = 18, kHeaderMid = 30, kCardTop = 55, kCardH = 84, kCardGap = 12, kCardInset = 14, kCardPad = 12;
constexpr float kRamMid = 160, kBarTop = 176, kBarH = 4, kListTop = 183, kRowH = 27, kDividerRoom = 8, kFooterH = 33;
constexpr float kColGap = 24, kNameOff = 18, kLine = 22;
// the VRAM block: its line and bar as the RAM's from the cards' bottom (the line's middle at 21, the bar at 37,
// 44 in all - DrawPanel takes the RAM block's own pixel distances), then its temperature row
constexpr float kVramLineH = kListTop - (kCardTop + kCardH), kVramH = kVramLineH + kRowH;
// the panel's colours from before mod.30, kept as they were: white at a few strengths over the dark grey glass -
// text, dim text, values, hints, cards, bar tracks - and each load in its old sparkline's colour
constexpr float kText = 0.95f, kDim = 0.55f, kValue = 0.97f, kHint = 0.6f, kUnknown = 0.35f, kCard = 0.07f, kTrack = 0.10f;
constexpr uint32_t kCpu = 0x61BDFF, kGpu = 0x73E085, kRam = 0xD199FF;   // blue, green, purple
}

static_assert(panelui::kListTop == sensorlogic::kPanelListTop && panelui::kRowH == sensorlogic::kPanelRowH &&
              panelui::kVramLineH == sensorlogic::kPanelVramLineH && panelui::kDividerRoom == sensorlogic::kPanelDividerRoom &&
              panelui::kFooterH == sensorlogic::kPanelFooterH, "the panel's drawing and its heights (sensor_logic.h) agree");

float Dock::PanelHeightDip(int dimmRows, int driveRows, bool vram, bool vramTemp) {
    return sensorlogic::PanelHeightDip(dimmRows, driveRows, !vram ? 0 : vramTemp ? 2 : 1);
}

// What the panel shows now in the room its window was built with (panelH): the VRAM block when the GPU has
// memory of its own (a fixed height, in a window built with room for it: panelVramRoom), the drives next (as
// many rows as fit), then the memory rows in what is left (four at most); drives and modules that do not fit
// are counted in the footer ("SSD 외 N개", "RAM 외 N개"). In a window built without room for the block (no GPU
// with memory of its own known then) the block is shown only where it fits beside every row shown without
// it - it pushes nothing out; the next window built has its room (PanelRoomHint). What the sensor settings
// hide is not there at all (sensorlogic::FitPanel).
Dock::PanelRows Dock::PanelRowsNow() const {
    PanelRows pr;
    if (scale <= 0) return pr;
    // DIP. panelH was rounded to whole pixels: up to half a pixel under the room asked for, which is more
    // than half a DIP on a screen under 1440 px (scale 0.72 - 1), so the half pixel is given back in DIP.
    const float room = (panelH + 0.5f) / scale + 0.01f;
    const PanelData& p = g_sensors.Panel();
    if (!g_sensors.Live() || !p.sampled) { pr.height = panelH / scale; return pr; }   // "측정 중…" in the room kept
    const sensorlogic::ShowItems show = g_sensors.Items();
    const int cols = PanelColumns(IsVertical(g_settings.edge));
    const int drives = show.ssd ? (int)p.drives.size() : 0, dimms = show.DimmsShown() ? (int)p.dimms.size() : 0;
    const int wantVram = !p.vram.present || !show.vram ? 0 : show.VramTempShown(g_sensors.VramNvmlTempSeen()) ? 2 : 1;
    const int roomVram = !panelVramRoom ? 0 : panelVramTempRoom ? 2 : 1;
    const sensorlogic::PanelFit f = sensorlogic::FitPanel(room, cols, drives, dimms, kPanelMaxDimmRows, wantVram, roomVram);
    pr.dimmRows = f.dimmRows; pr.driveRows = f.driveRows;
    pr.vram = f.vram > 0; pr.vramTemp = f.vram == 2;
    pr.drivesShown = f.drivesShown; pr.drivesHidden = f.drivesHidden;
    pr.dimmsShown = f.dimmsShown; pr.dimmsHidden = f.dimmsHidden;
    pr.height = f.height;
    return pr;
}

// The GPU's name: NVML's; without NVML ("GPU") the name of the adapter the dock draws with, read once.
static std::wstring PanelGpuName(const std::wstring& n) {
    if (!n.empty() && n != L"GPU") return n;
    static const std::wstring adapter = [] {
        std::wstring s;
        ComPtr<IDXGIAdapter> ad;
        DXGI_ADAPTER_DESC desc{};
        if (g_gfx.dxgi && SUCCEEDED(g_gfx.dxgi->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc))) s = desc.Description;
        for (const wchar_t* junk : { L"(R)", L"(TM)" }) { size_t p; while ((p = s.find(junk)) != std::wstring::npos) s.erase(p, wcslen(junk)); }
        return s;
    }();
    return adapter;
}

void Dock::DrawPanel() {
    using namespace panelui;
    using M = SensorClient::Metric;
    auto* dc = g_gfx.dc.Get();
    const float a = clampf(panelAlpha.x, 0, 1), s = scale;
    const D2D1_RECT_F r = panelRect;
    const bool vert = IsVertical(g_settings.edge);
    const int cols = PanelColumns(vert);
    auto col = [&](uint32_t rgb, float k = 1) { return D2D1::ColorF(rgb, k * a); };
    auto white = [&](float k) { return D2D1::ColorF(1, 1, 1, k * a); };
    // one line of text centred on `mid` (the formats centre it in its box), cut at x1
    auto text = [&](const std::wstring& t, IDWriteTextFormat* f, float x0, float x1, float mid, D2D1_COLOR_F c, float h = kLine) {
        if (t.empty() || x1 <= x0) return;
        brush->SetColor(c);
        dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, { x0, mid - h * s * 0.5f, x1, mid + h * s * 0.5f }, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    auto pct = [](float v) {
        wchar_t b[16];
        if (std::isnan(v)) wcscpy_s(b, L"--"); else swprintf_s(b, L"%d%%", (int)std::lround(v));
        return std::wstring(b);
    };
    // a load bar on whole pixels: the track, and the fill from the left (none for an unknown load)
    auto bar = [&](float x0, float x1, float top, float load, uint32_t fill) {
        x0 = std::round(x0); x1 = std::round(x1); top = std::round(top);
        const float h = std::max(2.0f, std::round(kBarH * s)), rad = h * 0.5f;
        brush->SetColor(white(kTrack));
        dc->FillRoundedRectangle({ { x0, top, x1, top + h }, rad, rad }, brush.Get());
        if (!(load > 0)) return;
        const float w = std::max(h, std::round((x1 - x0) * clampf(load / 100, 0, 1)));
        brush->SetColor(col(fill, 0.95f));
        dc->FillRoundedRectangle({ { x0, top, x0 + w, top + h }, rad, rad }, brush.Get());
    };
    const float x0 = r.left + std::round(kPad * s), x1 = r.right - std::round(kPad * s);
    auto Y = [&](float dip) { return r.top + std::round(dip * s); };

    // what it covers: "VRAM" with the block it shows (before the first sample: the block this window keeps
    // room for); without what the sensor settings hide
    const PanelData& p = g_sensors.Panel();
    const PanelRows rows = PanelRowsNow();
    const sensorlogic::ShowItems show = g_sensors.Items();
    const bool coversVram = g_sensors.Live() && p.sampled ? rows.vram : panelVramRoom;
    text(L"PC 상태", g_gfx.fmtPanelTitle.Get(), x0, x1, Y(kHeaderMid), white(kText), 30);
    text(sensorlogic::PanelHeader(show.gpu, coversVram, show.ssd), g_gfx.fmtPanelSmallR.Get(), x0, x1, Y(kHeaderMid), white(kDim));
    if (!g_sensors.Live()) {
        text(L"측정 중…", g_gfx.fmtPanelText.Get(), x0, x1, Y(kCardTop + 20), white(kDim));
        return;
    }
    const auto& d = g_sensors.Data();

    // CPU and GPU: the temperature (white; orange / red near or over the alert's threshold; "--°C" unknown),
    // the model, the load and its bar. Without a temperature the card says how to get one (CPU: a click
    // here, dock_input.cpp; GPU: NVIDIA only): beside the load when it fits there, otherwise in the model's
    // line, the card's whole width (the helper's own reason, "CPU 온도: 지원하지 않는 CPU이거나 PawnIO가
    // 없습니다", is longer than that too: cut, with its start telling what it is about).
    // The CPU's temperature switched off, or temperatures off: its place left blank, no hint. The GPU hidden:
    // its card is not there and the CPU's takes the whole width.
    struct Card { const wchar_t* title; std::wstring model; bool tempShown; float temp; int sev; std::wstring hint; float load; uint32_t fill; };
    std::wstring cpuHint;
    if (show.CpuTempShown() && !g_sensors.CpuTempAvailable()) {
        if (show.ReadsHWiNFO())   // HWiNFO runs without a CPU temperature (a click shows how), or it does not run
            cpuHint = g_sensors.HWiNFORunning() ? L"HWiNFO 설정: 클릭" : L"HWiNFO Gadget 필요";
        else   // PawnIO: the helper says why it has none
            cpuHint = d.status[0] ? sensorlogic::CpuCardStatus(d.status) : L"온도: 도우미 시작 중…";
    }
    const Card cards[2] = {
        { L"CPU", sensorlogic::PanelCpuName(d.cpuName), show.CpuTempShown(), g_sensors.ShownTemp(M::CPU), g_sensors.Severity(M::CPU), cpuHint, d.cpuLoad, kCpu },
        { L"GPU", PanelGpuName(d.gpuName), true, g_sensors.ShownTemp(M::GPU), g_sensors.Severity(M::GPU),
          (d.flags & LD_HAS_GPU) ? L"" : L"온도: NVIDIA만 지원", (d.flags & LD_HAS_GPU_LOAD) ? d.gpuLoad : NAN, kGpu },
    };
    const int cardCount = show.gpu ? 2 : 1;
    const float cw = ((r.right - r.left) / s - 2 * kCardInset - (cardCount - 1) * kCardGap) / cardCount;
    for (int i = 0; i < cardCount; ++i) {
        const Card& k = cards[i];
        const float cl = std::round(r.left + (kCardInset + i * (cw + kCardGap)) * s), cr = std::round(cl + cw * s);
        const float ct = Y(kCardTop), cb = ct + std::round(kCardH * s);
        brush->SetColor(white(kCard));
        dc->FillRoundedRectangle({ { cl, ct, cr, cb }, 9 * s, 9 * s }, brush.Get());
        const float ix0 = cl + std::round(kCardPad * s), ix1 = cr - std::round(kCardPad * s);
        const bool known = !std::isnan(k.temp);
        text(k.title, g_gfx.fmtPanelSmall.Get(), ix0, ix1, ct + 16 * s, white(kDim));
        if (k.tempShown)
            text(TempStr(k.temp, true), g_gfx.fmtPanelTemp.Get(), ix0, ix1, ct + 24 * s, known ? SevColor(k.sev, white(kValue), a) : white(kUnknown), 34);
        const std::wstring use = L"사용률  " + pct(k.load);
        const float hintX = ix0 + TextSize(g_gfx.fmtPanelText.Get(), use).width + 8 * s;
        const bool hintBeside = !k.hint.empty() && hintX + TextSize(g_gfx.fmtPanelSmall.Get(), k.hint).width <= ix1;
        if (!k.hint.empty() && !hintBeside) text(k.hint, g_gfx.fmtPanelSmall.Get(), ix0, ix1, ct + 40 * s, white(kHint));
        else text(k.model, g_gfx.fmtPanelSmall.Get(), ix0, ix1, ct + 40 * s, white(kDim));
        text(use, g_gfx.fmtPanelText.Get(), ix0, ix1, ct + 57 * s, white(kText));
        if (hintBeside) text(k.hint, g_gfx.fmtPanelSmallR.Get(), hintX, ix1, ct + 57 * s, white(kHint));
        bar(ix0, ix1, ct + 70 * s, k.load, k.fill);
    }

    // RAM: used of total, the load and its bar
    wchar_t b[96];
    swprintf_s(b, L"%.1f GB / %.1f GB", d.ramUsed, d.ramTotal);
    text(L"RAM", g_gfx.fmtPanelText.Get(), x0, x1, Y(kRamMid), white(kDim));
    text(b, g_gfx.fmtPanelText.Get(), x0 + 57 * s, x1 - 50 * s, Y(kRamMid), white(kText));
    text(pct(d.ramLoad), g_gfx.fmtPanelTextR.Get(), x0, x1, Y(kRamMid), white(kValue));
    bar(x0, x1, Y(kBarTop), d.ramLoad, kRam);

    // memory modules and drives, PanelColumns a row (two: left, right, then the next row; one on a vertical dock)
    const float gap = std::round(kColGap * s), colW = std::round((x1 - x0 - (cols - 1) * gap) / cols);
    auto cell = [&](int i, float top, float& c0, float& c1, float& mid) {
        c0 = x0 + (i % cols) * (colW + gap); c1 = (i % cols == cols - 1) ? x1 : c0 + colW;
        mid = top + ((i / cols) + 0.5f) * kRowH * s;
    };
    float y = Y(kListTop), c0, c1, mid;
    for (int i = 0; i < rows.dimmsShown; ++i) {
        const auto& m = p.dimms[i];
        cell(i, y, c0, c1, mid);
        const bool known = !std::isnan(m.temp);
        if (known) swprintf_s(b, L"%.1f°C", m.temp); else wcscpy_s(b, L"--°C");
        text(m.label, g_gfx.fmtPanelText.Get(), c0, c1 - 56 * s, mid, white(kDim));
        text(b, g_gfx.fmtPanelTextR.Get(), c0, c1, mid,
             known ? SevColor(SensorClient::SeverityOf(m.temp, SensorClient::kDimmHot), white(kValue), a) : white(kUnknown));
    }
    y += std::round(rows.dimmRows * kRowH * s);
    // VRAM, the GPU's own memory: used of total, the load and its bar as the RAM's (the bar in the GPU card's
    // colour), then its temperature in a row as a memory module's (not there when the settings hide it, or
    // when neither HWiNFO nor NVML can give it). Without a temperature the row says how to get one when HWiNFO
    // is the source (its Gadget key: hwinfo.h) - in the right-hand column, beside the name on a vertical dock.
    // The line, the bar and the row sit at the RAM block's own pixel distances from the cards' bottom, so the
    // two blocks are alike at every scale (rounded apart they differ by a pixel at 1.5).
    if (rows.vram) {
        const auto& v = p.vram;
        const float cardB = Y(kCardTop) + std::round(kCardH * s);
        const float vmid = y + (Y(kRamMid) - cardB);
        if (std::isnan(v.usedGB)) swprintf_s(b, L"-- / %.1f GB", v.totalGB);
        else swprintf_s(b, L"%.1f GB / %.1f GB", v.usedGB, v.totalGB);
        text(L"VRAM", g_gfx.fmtPanelText.Get(), x0, x1, vmid, white(kDim));
        text(b, g_gfx.fmtPanelText.Get(), x0 + 57 * s, x1 - 50 * s, vmid, white(std::isnan(v.usedGB) ? kUnknown : kText));
        text(pct(v.load), g_gfx.fmtPanelTextR.Get(), x0, x1, vmid, white(std::isnan(v.load) ? kUnknown : kValue));
        bar(x0, x1, y + (Y(kBarTop) - cardB), v.load, kGpu);
        const float top = y + (Y(kListTop) - cardB);
        y = top;
        if (rows.vramTemp) {
            const std::wstring name = L"VRAM 온도";
            cell(0, top, c0, c1, mid);
            const bool known = !std::isnan(v.temp);
            if (known) swprintf_s(b, L"%.1f°C", v.temp); else wcscpy_s(b, L"--°C");
            text(name, g_gfx.fmtPanelText.Get(), c0, c1 - 56 * s, mid, white(kDim));
            text(b, g_gfx.fmtPanelTextR.Get(), c0, c1, mid,
                 known ? SevColor(SensorClient::SeverityOf(v.temp, SensorClient::kVramHot), white(kValue), a) : white(kUnknown));
            if (!known && show.ReadsHWiNFO()) {
                const wchar_t* hint = L"HWiNFO Gadget 필요";
                if (cols > 1) { float h0, h1; cell(1, top, h0, h1, mid); text(hint, g_gfx.fmtPanelSmall.Get(), h0, h1, mid, white(kHint)); }
                else text(hint, g_gfx.fmtPanelSmall.Get(), c0 + TextSize(g_gfx.fmtPanelText.Get(), name).width + 10 * s, c1 - 56 * s, mid, white(kHint));
            }
            y = top + std::round(kRowH * s);
        }
    }
    if (sensorlogic::PanelDivider(rows.dimmRows, rows.vram ? 1 : 0, rows.driveRows)) {
        brush->SetColor(white(kTrack));
        dc->FillRectangle({ x0, std::round(y + s), x1, std::round(y + s) + std::max(1.0f, std::round(s)) }, brush.Get());
        y += std::round(kDividerRoom * s);
    }
    for (int i = 0; i < rows.drivesShown; ++i) {
        const auto& dr = p.drives[i];
        cell(i, y, c0, c1, mid);
        const bool known = !std::isnan(dr.temp);
        const wchar_t letter[] = { dr.letter, L':', 0 };
        text(letter, g_gfx.fmtPanelText.Get(), c0, c1, mid, white(kText));
        text(dr.model, g_gfx.fmtPanelText.Get(), c0 + kNameOff * s, c1 - 44 * s, mid, white(kText));
        text(TempStr(dr.temp, true), g_gfx.fmtPanelTextBoldR.Get(), c0, c1, mid,
             known ? SevColor(SensorClient::SeverityOf(dr.temp, SensorClient::kDriveHot), white(kValue), a) : white(kUnknown));
    }

    // footer: the drives and modules that report a temperature, the pace; on the right what was left out for
    // want of room (the counts on the left include it); nothing of what the settings hide
    const sensorlogic::PanelFooterText ft = sensorlogic::PanelFooter(p.sampled, show.ssd, (int)p.drives.size(), p.drivesWithTemp, rows.drivesHidden,
                                                                     show.DimmsShown(), p.modules, p.dimmsWithTemp, rows.dimmsHidden);
    const std::wstring& foot = ft.counts;
    const std::wstring& left = ft.leftOut;
    const float footMid = r.bottom - (kFooterH - 16) * s;
    const float leftW = left.empty() ? 0 : TextSize(g_gfx.fmtPanelSmall.Get(), left).width + 10 * s;
    text(foot, g_gfx.fmtPanelSmall.Get(), x0, x1 - leftW, footMid, white(kDim));
    if (!left.empty()) text(left, g_gfx.fmtPanelSmallR.Get(), x0, x1, footMid, white(kDim));
}
