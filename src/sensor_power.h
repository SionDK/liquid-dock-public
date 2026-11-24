#pragma once
// The "PC 상태" panel's power draw, without Windows (sensor settings powerView, showCpuPower, showGpuPower;
// ShowItems::CpuPowerShown / GpuPowerShown in sensor_logic.h): when each device's power is read, its 60 s history
// placed by time, the graph's steady scale and its points, and the texts the panel draws from it. Used by the
// dock's sensor thread (hw::SensorThread::SamplePower, hw_panel.h), SensorClient (sensors.h) and the panel's
// drawing. Kept out of sensor_logic.h on purpose: that file is compiled into the elevated helper too (through
// hw_sample.h), and these helpers - their texts - changed the helper's bytes (a UAC prompt at the next install)
// although the helper never calls them; here the helper never sees them. Tested by tests/sensor_logic_test.cpp
// (portable: g++/clang/cl).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "sensor_logic.h"

namespace sensorlogic {

// ---- the power draw (ShowItems::CpuPowerShown / GpuPowerShown; read by hw::SensorThread::SamplePower) -----

// When a shown power is read (the sensor thread wakes every 2 s, every second while a panel is open: nothing
// here adds a wake): 그래프 on every pass, closed or open (its history must be there when the panel opens);
// 요약 only while a panel is open (the footer is all it feeds); never when it is not shown.
inline bool PowerReadDue(int powerView, bool shown, bool panelOpen) {
    return shown && (powerView == kPowerGraph || (powerView == kPowerSummary && panelOpen));
}

// The graph's span: the last 60 s, by the readings' own times (2 s apart while the panel is closed, 1 s while it
// is open: the points are placed by time, so both mix). A reading more than kPowerGapMs after the one before
// starts a new run of the line (the dock slept, the source gave nothing for a while).
constexpr uint64_t kPowerWindowMs = 60000, kPowerGapMs = 5000;

struct PowerPoint {
    uint64_t ms = 0;           // GetTickCount64() of the pass that read it
    float watts = 0;
};

// Adds a reading (at nowMs) and drops what is older than the window, keeping the newest point before it (the
// line then reaches the graph's left edge: PowerSpark cuts it there). A time before the last point's (cannot
// happen with GetTickCount64) starts the history afresh.
inline void TrimPower(std::vector<PowerPoint>& h, uint64_t nowMs, uint64_t windowMs = kPowerWindowMs) {
    const uint64_t from = nowMs > windowMs ? nowMs - windowMs : 0;
    size_t keep = 0;   // the first point to keep: the last one at or before `from`
    while (keep + 1 < h.size() && h[keep + 1].ms <= from) ++keep;
    if (keep > 0) h.erase(h.begin(), h.begin() + (std::ptrdiff_t)keep);
}
inline void AddPowerPoint(std::vector<PowerPoint>& h, uint64_t nowMs, float watts, uint64_t windowMs = kPowerWindowMs) {
    if (!h.empty() && nowMs < h.back().ms) h.clear();
    if (!h.empty() && nowMs == h.back().ms) h.back().watts = watts;
    else h.push_back({ nowMs, watts });
    TrimPower(h, nowMs, windowMs);
}
// The highest reading inside the window (the point kept before it does not count), 0 when none.
inline float PowerMax(const std::vector<PowerPoint>& h, uint64_t nowMs, uint64_t windowMs = kPowerWindowMs) {
    const uint64_t from = nowMs > windowMs ? nowMs - windowMs : 0;
    float m = 0;
    for (const auto& p : h) if (p.ms >= from && p.watts > m) m = p.watts;
    return m;
}

// The graph's top when no limit gives it (the CPU always - see PowerScale -, a GPU whose NVML gives no enforced
// limit): the window's highest reading rounded up to a step of 1, 1.2, 1.5, 2, 2.5, 3, 4, 5, 6, 8 x 10^k W
// (.., 30, 40, 50, 60, 80, 100, 120, 150, 200, ..), never below kPowerAutoFloorW.
constexpr float kPowerAutoFloorW = 30.0f;
inline float PowerNiceCeil(float w) {
    if (!(w > 0)) return 0;
    static const int steps[] = { 10, 12, 15, 20, 25, 30, 40, 50, 60, 80, 100 };   // tenths of the decade: exact figures
    double decade = 1.0;
    while (decade * 10.0 < w) decade *= 10.0;
    while (decade > w) decade /= 10.0;
    for (int s : steps) {
        const double v = s * decade / 10.0;
        if (v >= w * 0.9999) return (float)v;
    }
    return (float)(10.0 * decade);
}
// The auto scale, kept steady and with room over the line: it rises at once to the step that holds the window's
// highest reading with 20 % to spare (the line then stays under 1/1.2 = 83 % of the graph's height, clear of the
// model's line above the box - a top at the reading itself drew a steady load along the box's top edge), and comes
// down only to a step that leaves the highest 35 % of room (a load hovering around a step does not flip the scale
// every second - at 61 W the top is 80, and it comes back to 60 only once the minute's highest is 44 W or less;
// a peak leaving the 60 s window lowers it once, maybe by several steps).
//   CPU: always this. HWiNFO gives no package power limit in W to take as the top without more ticking: AMD
//   reports "CPU PPT Limit" in % of a limit it does not name, Intel's "PL1/PL2 Power Limit" are further readings
//   the user would have to report too (and PL2, a short burst's cap, would leave a steady load flat at the bottom).
//   GPU: NVML's enforced power limit when it gives one (PowerSeries::limitW), this otherwise.
constexpr float kPowerRiseMargin = 1.2f, kPowerShrinkMargin = 1.35f;   // shrink > rise: no flip right after a rise
struct PowerScale {
    float top = 0;
    float Update(float windowMax, float floorW = kPowerAutoFloorW) {
        auto atLeastFloor = [&](float w) { return w > floorW ? w : floorW; };
        if (top <= 0 || windowMax * kPowerRiseMargin > top) top = PowerNiceCeil(atLeastFloor(windowMax * kPowerRiseMargin));
        else {
            const float lower = PowerNiceCeil(atLeastFloor(windowMax * kPowerShrinkMargin));
            if (lower < top) top = lower;
        }
        return top;
    }
};

// One device's power as the panel draws it (PowerData::cpu, ::gpu).
struct PowerSeries {
    bool read = false;         // the sampler has asked for it under the settings in force (false: neither value nor hint yet)
    float watts = NAN;         // the last reading, W; NAN: no source gave one (HWiNFO: no CPU package power reported to
                               // its Gadget; NVML: none, or no answer at that pass)
    float limitW = NAN;        // GPU: NVML's enforced power limit, W (read again every 30 s and when a panel opens); CPU: NAN
    float top = 0;             // 그래프: the graph's top, W - limitW when known, otherwise the auto scale (PowerScale); 0 without history
    std::vector<PowerPoint> history;   // 그래프 only: oldest first, the last 60 s (and the one point before them)
    bool Known() const { return !std::isnan(watts); }
};

// The power draw the sensor thread read (SensorClient::Power(), sensors.h), handed to the UI thread as PanelData is.
struct PowerData {
    int view = kPowerOff;      // the view it was read for
    uint64_t atMs = 0;         // GetTickCount64() of the pass that made it: the graph's right edge
    PowerSeries cpu, gpu;
};

// The graph's points for one series: x 0 (atMs - window) .. 1 (atMs, the newest reading at the right), y 0 .. 1 of
// `top` (clamped: a GPU's short spike over its limit stays inside the card); start: a new run of the line begins
// here (the first point, and after a gap of more than kPowerGapMs). A point before the window is cut at x = 0 on
// the way to the next one. Empty without a top or without two points to draw between.
struct SparkPoint { float x = 0, y = 0; bool start = false; };
inline std::vector<SparkPoint> PowerSpark(const PowerSeries& s, uint64_t atMs, uint64_t windowMs = kPowerWindowMs) {
    std::vector<SparkPoint> out;
    if (!(s.top > 0) || s.history.size() < 2 || windowMs == 0) return out;
    const double from = (double)atMs - (double)windowMs;
    auto yOf = [&](float w) { float y = w / s.top; return y < 0 ? 0.0f : y > 1 ? 1.0f : y; };
    auto xOf = [&](double ms) { double x = (ms - from) / (double)windowMs; return (float)(x < 0 ? 0 : x > 1 ? 1 : x); };
    for (size_t i = 0; i < s.history.size(); ++i) {
        const PowerPoint& p = s.history[i];
        const bool gap = i > 0 && p.ms - s.history[i - 1].ms > kPowerGapMs;
        if ((double)p.ms < from) {
            if (i + 1 < s.history.size() && (double)s.history[i + 1].ms > from && s.history[i + 1].ms - p.ms <= kPowerGapMs) {
                const PowerPoint& n = s.history[i + 1];
                const double f = (from - (double)p.ms) / (double)(n.ms - p.ms);
                out.push_back({ 0.0f, yOf((float)(p.watts + (n.watts - p.watts) * f)), true });
            }
            continue;
        }
        out.push_back({ xOf((double)p.ms), yOf(p.watts), out.empty() || gap });
    }
    // fewer than two points: nothing to draw. (A run of one point inside - a reading alone between two gaps - stays
    // in the list as a start with no point after it: there is no line for it.)
    if (out.size() < 2) out.clear();
    return out;
}

// "71 W": whole watts as drawn (lround), "--" when unknown.
inline std::wstring PowerText(float watts) {
    if (std::isnan(watts)) return L"--";
    const long w = std::lround(watts < 0 ? 0 : watts);
    return std::to_wstring(w) + L" W";
}
// The 그래프 footer's "합계 358 W": only when both are shown and known (the sum of the rounded figures the cards
// show, so the three add up on screen); empty otherwise.
inline std::wstring PowerTotalText(const ShowItems& show, const PowerData& p) {
    if (!show.PowerGraph() || !show.CpuPowerShown() || !show.GpuPowerShown() || !p.cpu.Known() || !p.gpu.Known()) return {};
    const long sum = std::lround(p.cpu.watts < 0 ? 0 : p.cpu.watts) + std::lround(p.gpu.watts < 0 ? 0 : p.gpu.watts);
    return L"합계 " + std::to_wstring(sum) + L" W";
}
// The 요약 footer's entries, CPU first: the shown and known ones only (none: nothing drawn). name "CPU" / "GPU",
// value "71 W"; device 0 CPU (#61BDFF), 1 GPU (#73E085) for the dot's colour.
struct PowerFooterItem { int device = 0; std::wstring name, value; };
inline std::vector<PowerFooterItem> PowerSummaryItems(const ShowItems& show, const PowerData& p) {
    std::vector<PowerFooterItem> out;
    if (!show.PowerSummary()) return out;
    if (show.CpuPowerShown() && p.cpu.Known()) out.push_back({ 0, L"CPU", PowerText(p.cpu.watts) });
    if (show.GpuPowerShown() && p.gpu.Known()) out.push_back({ 1, L"GPU", PowerText(p.gpu.watts) });
    return out;
}
// The CPU card's hint in 그래프 ("HWiNFO 전력: 클릭"): its power is shown, was asked for, and HWiNFO reports none.
// (요약 leaves an unknown power out without a word.)
inline bool CpuPowerHint(const ShowItems& show, const PowerData& p) {
    return show.PowerGraph() && show.CpuPowerShown() && p.cpu.read && !p.cpu.Known();
}
// ...its text: HWiNFO up without the reading, its Gadget setting is what is missing - "HWiNFO 전력: 클릭", after the
// temperature's "HWiNFO 설정: 클릭" form: a click on the panel shows how (HWiNFOGuideWanted); HWiNFO not running,
// the temperature's own words ("HWiNFO Gadget 필요": a click opens Task Manager, as for the temperature). The card's
// temperature hint, when it has one, is drawn instead of this one (one hint a card: both lead to the same guide).
inline const wchar_t* CpuPowerHintText(bool hwinfoRunning) { return hwinfoRunning ? L"HWiNFO 전력: 클릭" : L"HWiNFO Gadget 필요"; }

// A click on the sensor cell or the panel (Dock::Click, dock_input.cpp) shows the HWiNFO guide (SensorClient::
// ShowHWiNFOGuide: its Gadget tab, the CPU temperature and package power items) when HWiNFO is the source and runs
// and something the dock says it lacks comes from that setting: the CPU's temperature shown and none (the cell and
// the card both say so: either click), or - on the panel only (fromPanel) - the 그래프 card's CPU power hint. The cell
// shows no power at all, so its click stays Task Manager for that: a CPU HWiNFO gives no package power for would
// otherwise have turned the cell's click into the guide for good, with nothing on the cell to say why.
// Anything else opens Task Manager.
inline bool HWiNFOGuideWanted(const ShowItems& show, bool cpuTempAvailable, bool hwinfoRunning, const PowerData& p, bool fromPanel) {
    if (!show.ReadsHWiNFO() || !hwinfoRunning) return false;
    return (show.CpuTempShown() && !cpuTempAvailable) || (fromPanel && CpuPowerHint(show, p));
}

// ---- the footer with the power draw (Dock::PanelPaint::Footer) ----------------------------------------------
// The footer's right end holds the power draw: in 그래프 the total ("합계 358 W"), in 요약 the dots with their watts
// ("● CPU 71 W  ● GPU 287 W"); left of it what was left out for want of room ("SSD 외 2개 · RAM 외 1개"); the counts
// on the left ("SSD 3/3 · RAM 2/2 · 1초 갱신"). On the vertical dock's 284 DIP they may not all fit (the counts about
// 130 DIP, the dots about 140, the left out up to about 110). Then, step by step, until they do:
//   1. the power shorter: 요약 without the names (the dots in the cards' colours say which is which: "● 71 W  ● 287 W");
//      그래프 without its total (the cards show both figures);
//   2. the left out as one count ("외 3개");
//   3. what is still too long of the counts is cut at the right (as before mod.40, with the left out alone).
// The power draw of 요약 is never dropped: the footer is the only place it shows.
// The short left out: all that was left out in one count ("외 3개"); empty when nothing was.
inline std::wstring PanelLeftOutShort(bool drivesShown, int drivesHidden, bool dimmsShown, int dimmsHidden) {
    const int n = (drivesShown && drivesHidden > 0 ? drivesHidden : 0) + (dimmsShown && dimmsHidden > 0 ? dimmsHidden : 0);
    return n > 0 ? L"외 " + std::to_wstring(n) + L"개" : std::wstring();
}
// The step the footer is drawn at (widths in one unit, pixels or DIP; 0: that piece is empty): gap between two pieces.
struct PowerFooterPlan {
    bool powerShort = false;     // step 1: 요약 without the names, 그래프 without the total
    bool leftOutShort = false;   // step 2: "외 N개"
    float countsRoom = 0;        // the room left for the counts (cut when they are wider: step 3)
};
inline PowerFooterPlan PlanPowerFooter(float avail, float gap, float counts, float leftOut, float leftOutShort, float power, float powerShort) {
    auto right = [&](float lo, float pw) { return (lo > 0 ? lo + gap : 0) + (pw > 0 ? pw + gap : 0); };
    PowerFooterPlan plan;
    if (counts + right(leftOut, power) <= avail) { plan.countsRoom = avail - right(leftOut, power); return plan; }
    plan.powerShort = power > 0;
    const float pw = plan.powerShort ? powerShort : power;
    if (counts + right(leftOut, pw) <= avail) { plan.countsRoom = avail - right(leftOut, pw); return plan; }
    plan.leftOutShort = leftOut > 0;
    const float lo = plan.leftOutShort ? leftOutShort : leftOut;
    plan.countsRoom = avail - right(lo, pw);
    if (plan.countsRoom < 0) plan.countsRoom = 0;
    return plan;
}

}   // namespace sensorlogic
