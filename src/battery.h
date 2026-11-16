#pragma once
// The battery cell between the system monitor and the buttons (dock_layout.cpp, dock_sensors.cpp):
// what Windows reports (GetSystemPowerStatus) turned into what the dock shows. No Windows headers, so
// it can be tested anywhere (tests/battery_test.cpp, run by tests/run_portable.sh).
#include <cstdint>
#include <string>

namespace battery {

// SYSTEM_POWER_STATUS as Windows fills it.
struct Raw {
    uint8_t acLine = 255;             // 0 offline, 1 online, 255 unknown
    uint8_t flag = 255;               // 1 high, 2 low, 4 critical, 8 charging, 128 no system battery, 255 unknown
    uint8_t percent = 255;            // 0-100, 255 unknown
    uint8_t systemStatus = 0;         // 1: battery saver (energy saver) on
    uint32_t lifeTime = 0xFFFFFFFFu;  // seconds left on battery, 0xFFFFFFFF unknown
};

struct State {
    bool present = false;   // the PC has a battery whose charge Windows knows
    int percent = 0;        // 0-100
    bool charging = false;
    bool plugged = false;   // on mains power: charging, full, or held (a charge limit)
    bool saver = false;     // battery saver / energy saver on
    int secondsLeft = -1;   // on battery, when Windows has an estimate
    bool operator==(const State&) const = default;
};

inline State FromRaw(const Raw& r) {
    State s;
    if (r.flag == 255 || (r.flag & 128) || r.percent > 100) return s;   // a desktop, or nothing known
    s.present = true;
    s.percent = r.percent;
    s.charging = (r.flag & 8) != 0;
    s.plugged = r.acLine == 1 || s.charging;
    s.saver = r.systemStatus == 1;
    if (!s.plugged && r.lifeTime != 0xFFFFFFFFu && r.lifeTime < 7u * 24 * 3600) s.secondsLeft = (int)r.lifeTime;
    return s;
}

// The colour of the charge: low and critical only count on battery.
enum class Level { Normal, Low, Critical };
inline Level LevelOf(const State& s) {
    if (s.plugged) return Level::Normal;
    if (s.percent <= 10) return Level::Critical;
    if (s.percent <= 20) return Level::Low;
    return Level::Normal;
}

// How much of the icon's body is filled, 0..1. Any charge shows at least a sliver.
inline float FillOf(const State& s) {
    if (s.percent <= 0) return 0.0f;
    float f = s.percent / 100.0f;
    return f < 0.06f ? 0.06f : (f > 1.0f ? 1.0f : f);
}

inline std::wstring PercentText(const State& s) { return std::to_wstring(s.percent) + L"%"; }

// "약 3시간 12분", "약 45분", "약 2시간"
inline std::wstring TimeLeftText(int seconds) {
    int minutes = (seconds + 30) / 60;
    if (minutes < 1) minutes = 1;
    std::wstring t = L"약 ";
    if (minutes >= 60) {
        t += std::to_wstring(minutes / 60) + L"시간";
        if (minutes % 60) t += L" " + std::to_wstring(minutes % 60) + L"분";
    } else {
        t += std::to_wstring(minutes) + L"분";
    }
    return t;
}

// The tooltip: "배터리 87% · 충전 중", "배터리 64% · 약 3시간 12분 남음 · 절전 모드"
inline std::wstring Tooltip(const State& s) {
    if (!s.present) return L"배터리 없음";
    std::wstring t = L"배터리 " + PercentText(s);
    if (s.charging) t += L" · 충전 중";
    else if (s.plugged) t += s.percent >= 100 ? L" · 완전히 충전됨" : L" · 전원 연결됨, 충전 안 함";
    else if (s.secondsLeft >= 0) t += L" · " + TimeLeftText(s.secondsLeft) + L" 남음";
    if (s.saver) t += L" · 절전 모드";
    return t;
}

}   // namespace battery
