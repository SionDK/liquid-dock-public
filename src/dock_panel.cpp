#include "dock.h"
#include "sensors.h"

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
// The power draw (settings powerView, mod.40; sensor_power.h) adds nothing to the height: 그래프 draws each card's last
// 60 s behind its text and its watts beside the load, 요약 the watts at the footer's right (Cards, Spark, Footer).
namespace panelui {
constexpr float kPad = 18, kHeaderMid = 30, kCardTop = 55, kCardH = 84, kCardGap = 12, kCardInset = 14, kCardPad = 12;
constexpr float kRamMid = 160, kBarTop = 176, kBarH = 4, kListTop = 183, kRowH = 27, kDividerRoom = 8, kFooterH = 33;
constexpr float kColGap = 24, kNameOff = 18, kLine = 22;
// the VRAM block: its line and bar as the RAM's from the cards' bottom (the line's middle at 21, the bar at 37,
// 44 in all - DrawPanel takes the RAM block's own pixel distances), then its temperature row
constexpr float kVramLineH = kListTop - (kCardTop + kCardH), kVramH = kVramLineH + kRowH;
// the panel's colours from before mod.30, kept as they were: white at a few strengths over the dark grey glass -
// text, dim text, values, hints, cards, bar tracks - and each load in its old sparkline's colour
// the 그래프 power view's sparkline in a card: from under the model's line - below its descenders too (the 10 DIP
// small font centred at 40: its baseline about 44, y / p / g down to about 46.6) - to just above the bar at 70. The
// mockup's 36 .. 66 DIP put a line near its top through the model's name (struck through, in the pictures at 1.5),
// 44 still along its baseline (underlined); the auto scale keeps the line under 5/6 of the box too (PowerScale).
constexpr float kSparkTop = 48, kSparkBottom = 66;
constexpr float kText = 0.95f, kDim = 0.55f, kValue = 0.97f, kHint = 0.6f, kUnknown = 0.35f, kCard = 0.07f, kTrack = 0.10f;
constexpr uint32_t kCpu = 0x61BDFF, kGpu = 0x73E085, kRam = 0xD199FF;   // blue, green, purple
}

static_assert(panelui::kListTop == sensorlogic::kPanelListTop && panelui::kRowH == sensorlogic::kPanelRowH &&
              panelui::kVramLineH == sensorlogic::kPanelVramLineH && panelui::kDividerRoom == sensorlogic::kPanelDividerRoom &&
              panelui::kFooterH == sensorlogic::kPanelFooterH, "the panel's drawing and its heights (sensor_logic.h) agree");

// What the panel shows now in the room its window was built with (panelH): the VRAM block when the GPU has
// memory of its own (a fixed height, in a window built with room for it: panelVramRoom), the drives next (as
// many rows as fit), then the memory rows in what is left (four at most); drives and modules that do not fit
// are counted in the footer ("SSD 외 N개", "RAM 외 N개"). In a window built without room for the block (no GPU
// with memory of its own known then) the block is shown only where it fits beside every row shown without
// it - it pushes nothing out; the next window built has its room (PanelRoomHint). What the sensor settings
// hide is not there at all (sensorlogic::FitPanel).
sensorlogic::PanelFit Dock::PanelRowsNow() const {
    sensorlogic::PanelFit pr;
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
    return sensorlogic::FitPanel(room, cols, drives, dimms, kPanelMaxDimmRows, wantVram, roomVram);
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

// DrawPanel's picture: what it is drawn with, worked out once as DrawPanel begins (the alpha, the scale, the
// panel's rectangle, its columns and text margins), the helpers its blocks share, and the
// blocks, drawn in DrawPanel's order (cards, RAM, memory modules, VRAM, the divider and drives, the footer).
struct Dock::PanelPaint {
    Dock& dock;
    ID2D1DeviceContext2* dc;
    const ComPtr<ID2D1SolidColorBrush>& brush;
    float a, s;
    D2D1_RECT_F r;
    int cols;
    float x0, x1;
    float gap = 0, colW = 0;   // the columns of the module and drive rows (DrawPanel, before the first row)

    auto col(uint32_t rgb, float k = 1) const { return D2D1::ColorF(rgb, k * a); }
    auto white(float k) const { return D2D1::ColorF(1, 1, 1, k * a); }
    // one line of text centred on `mid` (the formats centre it in its box), cut at x1
    void text(const std::wstring& t, IDWriteTextFormat* f, float x0, float x1, float mid, D2D1_COLOR_F c, float h = panelui::kLine) const {
        if (t.empty() || x1 <= x0) return;
        brush->SetColor(c);
        dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, { x0, mid - h * s * 0.5f, x1, mid + h * s * 0.5f }, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    static std::wstring pct(float v) {
        wchar_t b[16];
        if (std::isnan(v)) wcscpy_s(b, L"--"); else swprintf_s(b, L"%d%%", (int)std::lround(v));
        return std::wstring(b);
    }
    // a load bar on whole pixels: the track, and the fill from the left (none for an unknown load)
    void bar(float x0, float x1, float top, float load, uint32_t fill) const {
        using namespace panelui;
        x0 = std::round(x0); x1 = std::round(x1); top = std::round(top);
        const float h = std::max(2.0f, std::round(kBarH * s)), rad = h * 0.5f;
        brush->SetColor(white(kTrack));
        dc->FillRoundedRectangle({ { x0, top, x1, top + h }, rad, rad }, brush.Get());
        if (!(load > 0)) return;
        const float w = std::max(h, std::round((x1 - x0) * clampf(load / 100, 0, 1)));
        brush->SetColor(col(fill, 0.95f));
        dc->FillRoundedRectangle({ { x0, top, x0 + w, top + h }, rad, rad }, brush.Get());
    }
    float Y(float dip) const { return r.top + std::round(dip * s); }
    // a module's or drive's place in the rows from `top`: its left and right, the middle of its row
    void cell(int i, float top, float& c0, float& c1, float& mid) const {
        using namespace panelui;
        c0 = x0 + (i % cols) * (colW + gap); c1 = (i % cols == cols - 1) ? x1 : c0 + colW;
        mid = top + ((i / cols) + 0.5f) * kRowH * s;
    }
    D2D1_SIZE_F TextSize(IDWriteTextFormat* f, const std::wstring& t) const { return dock.TextSize(f, t); }

    void Spark(int i, const sensorlogic::PowerSeries& ps, uint64_t atMs, D2D1_RECT_F box, uint32_t rgb) const;
    void Cards(const LDSensorBlock& d, const sensorlogic::ShowItems& show) const;
    void Ram(const LDSensorBlock& d) const;
    void Dimms(const PanelData& p, const sensorlogic::PanelFit& rows, float& y) const;
    void Vram(const PanelData& p, const sensorlogic::PanelFit& rows, const sensorlogic::ShowItems& show, float& y) const;
    void Drives(const PanelData& p, const sensorlogic::PanelFit& rows, float& y) const;
    void Footer(const PanelData& p, const sensorlogic::PanelFit& rows, const sensorlogic::ShowItems& show) const;
};

// The 그래프 power view's sparkline of card i (0 CPU, 1 GPU) in `box` (whole pixels: left the oldest reading of the
// last 60 s, right the newest; top the graph's top - the GPU's power limit, the CPU's auto scale -, bottom its floor):
// the area under the line at .12 of the card's colour, the line at .45, one figure a run of the line
// (sensorlogic::PowerSpark). The paths are built from the box's top-left corner (0 .. w, 0 .. h) and drawn moved
// there, so they are built again only when a pass brought a new sample (powerVersion) or the box's size changed
// (another scale, the GPU hidden): about 60 points, not rebuilt while the panel slides in its fade or follows the
// magnified sensor cell (the box's corner is on whole pixels: the move keeps the line as sharp).
void Dock::PanelPaint::Spark(int i, const sensorlogic::PowerSeries& ps, uint64_t atMs, D2D1_RECT_F box, uint32_t rgb) const {
    PanelSpark& sp = dock.panelSpark[i];
    const float w = box.right - box.left, h = box.bottom - box.top;
    if (sp.version != g_sensors.powerVersion || sp.w != w || sp.h != h) {
        sp.version = g_sensors.powerVersion; sp.w = w; sp.h = h;
        sp.fill.Reset(); sp.line.Reset();
        const std::vector<sensorlogic::SparkPoint> pts = sensorlogic::PowerSpark(ps, atMs);
        ComPtr<ID2D1PathGeometry> fill, line;
        ComPtr<ID2D1GeometrySink> fs, ls;
        if (!pts.empty() && SUCCEEDED(g_gfx.d2dFactory->CreatePathGeometry(&fill)) && SUCCEEDED(fill->Open(&fs)) &&
            SUCCEEDED(g_gfx.d2dFactory->CreatePathGeometry(&line)) && SUCCEEDED(line->Open(&ls))) {
            auto at = [&](const sensorlogic::SparkPoint& p) { return D2D1::Point2F(p.x * w, h - p.y * h); };
            for (size_t k = 0; k < pts.size();) {
                size_t end = k + 1;
                while (end < pts.size() && !pts[end].start) ++end;   // [k, end): one run of the line
                if (end - k >= 2) {
                    const D2D1_POINT_2F first = at(pts[k]), last = at(pts[end - 1]);
                    fs->BeginFigure({ first.x, h }, D2D1_FIGURE_BEGIN_FILLED);
                    ls->BeginFigure(first, D2D1_FIGURE_BEGIN_HOLLOW);
                    for (size_t j = k; j < end; ++j) { fs->AddLine(at(pts[j])); if (j > k) ls->AddLine(at(pts[j])); }
                    fs->AddLine({ last.x, h });
                    fs->EndFigure(D2D1_FIGURE_END_CLOSED);
                    ls->EndFigure(D2D1_FIGURE_END_OPEN);
                }
                k = end;
            }
            if (SUCCEEDED(fs->Close()) && SUCCEEDED(ls->Close())) { sp.fill = fill; sp.line = line; }
        }
    }
    if (!sp.fill || !sp.line) return;
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(box.left, box.top) * old);
    brush->SetColor(col(rgb, 0.12f));
    dc->FillGeometry(sp.fill.Get(), brush.Get());
    brush->SetColor(col(rgb, 0.45f));
    dc->DrawGeometry(sp.line.Get(), brush.Get(), std::max(1.0f, std::round(s)));
    dc->SetTransform(old);
}

// CPU and GPU: the temperature (white; orange / red near or over the alert's threshold; "--°C" unknown),
// the model, the load and its bar. Without a temperature the card says how to get one (CPU: a click
// here, dock_input.cpp; GPU: NVIDIA only): beside the load when it fits there, otherwise in the model's
// line, the card's whole width (the helper's own reason, "CPU 온도: 지원하지 않는 CPU이거나 PawnIO가
// 없습니다", is longer than that too: cut, with its start telling what it is about).
// The CPU's temperature switched off, or temperatures off: its place left blank, no hint. The GPU hidden:
// its card is not there and the CPU's takes the whole width.
// The power view 그래프 (mod.40, sensorlogic::ShowItems::PowerGraph): where a card's power is shown and known, the
// last 60 s as a faint area behind its lower half (kSparkTop .. kSparkBottom, 48 .. 66 DIP down the card, the inner width) and the watts at the
// right of the load's line ("71 W"; a temperature hint beside the load then goes to the model's line when the two do
// not fit). The CPU's shown but not reported by HWiNFO: the card's hint says so ("HWiNFO 전력: 클릭", a click on the panel shows how),
// unless its temperature hint is there already (one hint a card).
void Dock::PanelPaint::Cards(const LDSensorBlock& d, const sensorlogic::ShowItems& show) const {
    using namespace panelui;
    using M = SensorClient::Metric;
    struct Card { const wchar_t* title; std::wstring model; bool tempShown; float temp; int sev; std::wstring hint; float load; uint32_t fill;
                  bool powerShown; const sensorlogic::PowerSeries* power; };
    std::wstring cpuHint;
    if (show.CpuTempShown() && !g_sensors.CpuTempAvailable()) {
        if (show.ReadsHWiNFO())   // HWiNFO runs without a CPU temperature (a click shows how), or it does not run
            cpuHint = g_sensors.HWiNFORunning() ? L"HWiNFO 설정: 클릭" : L"HWiNFO Gadget 필요";
        else   // PawnIO: the helper says why it has none
            cpuHint = d.status[0] ? sensorlogic::CpuCardStatus(d.status) : L"온도: 도우미 시작 중…";
    }
    const sensorlogic::PowerData& power = g_sensors.Power();
    if (cpuHint.empty() && sensorlogic::CpuPowerHint(show, power)) cpuHint = sensorlogic::CpuPowerHintText(g_sensors.HWiNFORunning());
    const Card cards[2] = {
        { L"CPU", sensorlogic::PanelCpuName(d.cpuName), show.CpuTempShown(), g_sensors.ShownTemp(M::CPU), g_sensors.Severity(M::CPU), cpuHint, d.cpuLoad, kCpu,
          show.PowerGraph() && show.CpuPowerShown(), &power.cpu },
        { L"GPU", PanelGpuName(d.gpuName), true, g_sensors.ShownTemp(M::GPU), g_sensors.Severity(M::GPU),
          (d.flags & LD_HAS_GPU) ? L"" : L"온도: NVIDIA만 지원", (d.flags & LD_HAS_GPU_LOAD) ? d.gpuLoad : NAN, kGpu,
          show.PowerGraph() && show.GpuPowerShown(), &power.gpu },
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
        // 그래프: the sparkline first, under the card's text
        const bool watts = k.powerShown && k.power->Known();
        if (watts) Spark(i, *k.power, power.atMs, { ix0, ct + std::round(kSparkTop * s), ix1, ct + std::round(kSparkBottom * s) }, k.fill);
        text(k.title, g_gfx.fmtPanelSmall.Get(), ix0, ix1, ct + 16 * s, white(kDim));
        if (k.tempShown)
            text(TempStr(k.temp, true), g_gfx.fmtPanelTemp.Get(), ix0, ix1, ct + 24 * s, known ? SevColor(k.sev, white(kValue), a) : white(kUnknown), 34);
        const std::wstring use = L"사용률  " + pct(k.load);
        const std::wstring wattsText = watts ? sensorlogic::PowerText(k.power->watts) : std::wstring();
        const float wattsW = watts ? TextSize(g_gfx.fmtPanelText.Get(), wattsText).width + 8 * s : 0;
        const float hintX = ix0 + TextSize(g_gfx.fmtPanelText.Get(), use).width + 8 * s;
        const bool hintBeside = !k.hint.empty() && hintX + TextSize(g_gfx.fmtPanelSmall.Get(), k.hint).width <= ix1 - wattsW;
        if (!k.hint.empty() && !hintBeside) text(k.hint, g_gfx.fmtPanelSmall.Get(), ix0, ix1, ct + 40 * s, white(kHint));
        else text(k.model, g_gfx.fmtPanelSmall.Get(), ix0, ix1, ct + 40 * s, white(kDim));
        text(use, g_gfx.fmtPanelText.Get(), ix0, ix1, ct + 57 * s, white(kText));
        if (hintBeside) text(k.hint, g_gfx.fmtPanelSmallR.Get(), hintX, ix1 - wattsW, ct + 57 * s, white(kHint));
        if (watts) text(wattsText, g_gfx.fmtPanelTextR.Get(), ix0, ix1, ct + 57 * s, white(kValue));
        bar(ix0, ix1, ct + 70 * s, k.load, k.fill);
    }
}

// RAM: used of total, the load and its bar
void Dock::PanelPaint::Ram(const LDSensorBlock& d) const {
    using namespace panelui;
    wchar_t b[96];
    swprintf_s(b, L"%.1f GB / %.1f GB", d.ramUsed, d.ramTotal);
    text(L"RAM", g_gfx.fmtPanelText.Get(), x0, x1, Y(kRamMid), white(kDim));
    text(b, g_gfx.fmtPanelText.Get(), x0 + 57 * s, x1 - 50 * s, Y(kRamMid), white(kText));
    text(pct(d.ramLoad), g_gfx.fmtPanelTextR.Get(), x0, x1, Y(kRamMid), white(kValue));
    bar(x0, x1, Y(kBarTop), d.ramLoad, kRam);
}

// memory modules, PanelColumns a row (two: left, right, then the next row; one on a vertical dock); y: the
// list's top, then below its rows
void Dock::PanelPaint::Dimms(const PanelData& p, const sensorlogic::PanelFit& rows, float& y) const {
    using namespace panelui;
    wchar_t b[96];
    float c0, c1, mid;
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
}

// VRAM, the GPU's own memory: used of total, the load and its bar as the RAM's (the bar in the GPU card's
// colour), then its temperature in a row as a memory module's (not there when the settings hide it, or
// when neither HWiNFO nor NVML can give it). Without a temperature the row says how to get one when HWiNFO
// is the source (its Gadget key: hwinfo.h) - in the right-hand column, beside the name on a vertical dock.
// The line, the bar and the row sit at the RAM block's own pixel distances from the cards' bottom, so the
// two blocks are alike at every scale (rounded apart they differ by a pixel at 1.5).
void Dock::PanelPaint::Vram(const PanelData& p, const sensorlogic::PanelFit& rows, const sensorlogic::ShowItems& show, float& y) const {
    using namespace panelui;
    wchar_t b[96];
    float c0, c1, mid;
    if (rows.vram > 0) {
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
        if (rows.vram == 2) {
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
}

// the divider (drives after memory rows or VRAM), the drives as the modules
void Dock::PanelPaint::Drives(const PanelData& p, const sensorlogic::PanelFit& rows, float& y) const {
    using namespace panelui;
    float c0, c1, mid;
    if (sensorlogic::PanelDivider(rows.dimmRows, rows.vram > 0 ? 1 : 0, rows.driveRows)) {
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
}

// footer: the drives and modules that report a temperature, the pace; on the right what was left out for
// want of room (the counts on the left include it); nothing of what the settings hide.
// The power view (mod.40) puts its figures at the right end, the left out moving left of them: 그래프 the total of
// both cards' watts ("합계 358 W", only with both shown and known), 요약 a dot in each card's colour (6 DIP) with its
// name and watts ("● CPU 71 W  ● GPU 287 W", the unknown left out, nothing when neither is known). Where they do not
// all fit (the vertical dock's 284 DIP) sensorlogic::PlanPowerFooter shortens them: the power first (요약 without the
// names, 그래프 without the total), then the left out ("외 3개"), then the counts are cut. Without the power the footer
// is as before.
void Dock::PanelPaint::Footer(const PanelData& p, const sensorlogic::PanelFit& rows, const sensorlogic::ShowItems& show) const {
    using namespace panelui;
    const sensorlogic::PanelFooterText ft = sensorlogic::PanelFooter(p.sampled, show.ssd, (int)p.drives.size(), p.drivesWithTemp, rows.drivesHidden,
                                                                     show.DimmsShown(), p.modules, p.dimmsWithTemp, rows.dimmsHidden);
    const std::wstring& foot = ft.counts;
    const std::wstring& left = ft.leftOut;
    const float footMid = r.bottom - (kFooterH - 16) * s;
    IDWriteTextFormat* const fSmall = g_gfx.fmtPanelSmall.Get();
    auto width = [&](const std::wstring& t) { return t.empty() ? 0.0f : TextSize(fSmall, t).width; };

    const sensorlogic::PowerData& power = g_sensors.Power();
    const std::wstring total = sensorlogic::PowerTotalText(show, power);
    const std::vector<sensorlogic::PowerFooterItem> items = sensorlogic::PowerSummaryItems(show, power);
    if (total.empty() && items.empty()) {   // the footer as before mod.40
        const float leftW = left.empty() ? 0 : width(left) + 10 * s;
        text(foot, fSmall, x0, x1 - leftW, footMid, white(kDim));
        if (!left.empty()) text(left, g_gfx.fmtPanelSmallR.Get(), x0, x1, footMid, white(kDim));
        return;
    }
    // 요약's pieces in whole pixels: the dot, 4 DIP, the name with its space (dim), the watts (as a value); 10 DIP apart
    const float dot = std::max(4.0f, std::round(6 * s)), dotGap = std::round(4 * s), itemGap = std::round(10 * s), gap = std::round(10 * s);
    auto itemsWidth = [&](bool names) {
        float w = 0;
        for (size_t i = 0; i < items.size(); ++i)
            w += (i ? itemGap : 0) + dot + dotGap + (names ? std::ceil(width(items[i].name + L" ")) : 0) + std::ceil(width(items[i].value));
        return w;
    };
    const float powerW = !total.empty() ? std::ceil(width(total)) : itemsWidth(true);
    const float powerShortW = !total.empty() ? 0 : itemsWidth(false);
    const std::wstring leftShort = sensorlogic::PanelLeftOutShort(show.ssd, rows.drivesHidden, show.DimmsShown(), rows.dimmsHidden);
    const sensorlogic::PowerFooterPlan plan = sensorlogic::PlanPowerFooter(x1 - x0, gap, width(foot), width(left), width(leftShort), powerW, powerShortW);
    const float groupW = plan.powerShort ? powerShortW : powerW;
    if (!total.empty() && !plan.powerShort) text(total, g_gfx.fmtPanelSmallR.Get(), x0, x1, footMid, white(kDim));
    if (!items.empty()) {
        float x = x1 - groupW;
        const float dotTop = std::round(footMid - dot * 0.5f - 0.5f * s);
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            if (i) x += itemGap;
            brush->SetColor(col(it.device == 0 ? kCpu : kGpu, 0.95f));
            dc->FillEllipse({ { x + dot * 0.5f, dotTop + dot * 0.5f }, dot * 0.5f, dot * 0.5f }, brush.Get());
            x += dot + dotGap;
            if (!plan.powerShort) {
                const std::wstring name = it.name + L" ";
                text(name, fSmall, x, x1, footMid, white(kDim));
                x += std::ceil(width(name));
            }
            text(it.value, fSmall, x, x1, footMid, white(kValue));
            x += std::ceil(width(it.value));
        }
    }
    const float rightEnd = x1 - (groupW > 0 ? groupW + gap : 0);
    const std::wstring& leftShown = plan.leftOutShort ? leftShort : left;
    if (!leftShown.empty()) text(leftShown, g_gfx.fmtPanelSmallR.Get(), x0, rightEnd, footMid, white(kDim));
    text(foot, fSmall, x0, x0 + plan.countsRoom, footMid, white(kDim));
}

void Dock::DrawPanel() {
    using namespace panelui;
    auto* dc = g_gfx.dc.Get();
    const float a = clampf(panelAlpha.x, 0, 1), s = scale;
    const D2D1_RECT_F r = panelRect;
    const bool vert = IsVertical(g_settings.edge);
    const int cols = PanelColumns(vert);
    const float x0 = r.left + std::round(kPad * s), x1 = r.right - std::round(kPad * s);
    PanelPaint paint{ *this, dc, brush, a, s, r, cols, x0, x1 };

    // what it covers: "VRAM" with the block it shows (before the first sample: the block this window keeps
    // room for); without what the sensor settings hide
    const PanelData& p = g_sensors.Panel();
    const sensorlogic::PanelFit rows = PanelRowsNow();
    const sensorlogic::ShowItems show = g_sensors.Items();
    const bool coversVram = g_sensors.Live() && p.sampled ? rows.vram > 0 : panelVramRoom;
    paint.text(L"PC 상태", g_gfx.fmtPanelTitle.Get(), x0, x1, paint.Y(kHeaderMid), paint.white(kText), 30);
    paint.text(sensorlogic::PanelHeader(show.gpu, coversVram, show.ssd), g_gfx.fmtPanelSmallR.Get(), x0, x1, paint.Y(kHeaderMid), paint.white(kDim));
    if (!g_sensors.Live()) {
        paint.text(L"측정 중…", g_gfx.fmtPanelText.Get(), x0, x1, paint.Y(kCardTop + 20), paint.white(kDim));
        return;
    }
    const auto& d = g_sensors.Data();
    paint.Cards(d, show);
    paint.Ram(d);

    // memory modules and drives, PanelColumns a row (two: left, right, then the next row; one on a vertical dock)
    const float gap = std::round(kColGap * s), colW = std::round((x1 - x0 - (cols - 1) * gap) / cols);
    paint.gap = gap; paint.colW = colW;
    float y = paint.Y(kListTop);
    paint.Dimms(p, rows, y);
    paint.Vram(p, rows, show, y);
    paint.Drives(p, rows, y);
    paint.Footer(p, rows, show);
}
