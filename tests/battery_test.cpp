// The battery cell's reading of GetSystemPowerStatus (src/battery.h): which PCs have a battery,
// charging and plugged in, the colour levels, the fill and the tooltip. No Windows headers.
//   cl /std:c++20 /EHsc /utf-8 tests\battery_test.cpp
//   g++ -std=c++20 tests/battery_test.cpp -o battery_test
#include "../src/battery.h"
#include "portable_check.h"

using namespace battery;

namespace {
Raw R(uint8_t ac, uint8_t flag, uint8_t pct, uint8_t sys = 0, uint32_t life = 0xFFFFFFFFu) {
    Raw r; r.acLine = ac; r.flag = flag; r.percent = pct; r.systemStatus = sys; r.lifeTime = life;
    return r;
}
}

int main() {
    // ---- which PCs have one
    Check(!FromRaw(R(1, 128, 255)).present, "a desktop: 'no system battery'");
    Check(!FromRaw(R(255, 255, 255)).present, "Windows does not know: no cell");
    Check(!FromRaw(R(1, 1, 255)).present, "an unknown charge: no cell");
    Check(FromRaw(R(0, 1, 87)).present, "a laptop on battery");

    // ---- states
    State s = FromRaw(R(1, 8 | 1, 64));
    Check(s.charging && s.plugged && s.percent == 64, "charging");
    s = FromRaw(R(1, 1, 85));
    Check(!s.charging && s.plugged, "plugged in, held at a charge limit (a laptop's battery protection)");
    s = FromRaw(R(0, 2, 15, 1, 5400));
    Check(!s.plugged && s.saver && s.secondsLeft == 5400, "on battery, saver on, with Windows' estimate");
    Check(FromRaw(R(1, 1, 90, 0, 5400)).secondsLeft == -1, "no time left on mains power");
    Check(FromRaw(R(0, 1, 90, 0, 0xFFFFFFFFu)).secondsLeft == -1, "no estimate yet");
    Check(FromRaw(R(0, 1, 90)) == FromRaw(R(0, 1, 90)) && !(FromRaw(R(0, 1, 90)) == FromRaw(R(0, 1, 89))), "states compare (redraw on change)");

    // ---- colours: low and critical only on battery
    Check(LevelOf(FromRaw(R(0, 1, 21))) == Level::Normal, "21 %: normal");
    Check(LevelOf(FromRaw(R(0, 2, 20))) == Level::Low, "20 %: low");
    Check(LevelOf(FromRaw(R(0, 4, 10))) == Level::Critical, "10 %: critical");
    Check(LevelOf(FromRaw(R(1, 8, 5))) == Level::Normal, "5 % but charging: not alarming");

    // ---- fill
    Check(FillOf(FromRaw(R(0, 1, 100))) == 1.0f && FillOf(FromRaw(R(0, 1, 50))) == 0.5f, "fill follows the charge");
    Check(FillOf(FromRaw(R(0, 4, 2))) > 0.05f, "a nearly empty battery still shows a sliver");
    Check(FillOf(FromRaw(R(0, 4, 0))) == 0.0f, "0 %: empty");

    // ---- text
    Check(PercentText(FromRaw(R(0, 1, 87))) == L"87%", "percentage under the icon");
    Check(Tooltip(FromRaw(R(1, 8, 64))) == L"배터리 64% · 충전 중", "tooltip: charging");
    Check(Tooltip(FromRaw(R(1, 1, 100))) == L"배터리 100% · 완전히 충전됨", "tooltip: full");
    Check(Tooltip(FromRaw(R(1, 1, 85))) == L"배터리 85% · 전원 연결됨, 충전 안 함", "tooltip: held at a limit");
    Check(Tooltip(FromRaw(R(0, 1, 64, 1, 11520))) == L"배터리 64% · 약 3시간 12분 남음 · 절전 모드", "tooltip: on battery with an estimate and saver");
    Check(TimeLeftText(7200) == L"약 2시간" && TimeLeftText(2700) == L"약 45분" && TimeLeftText(10) == L"약 1분", "time left wording");
    Check(Tooltip(FromRaw(R(0, 1, 50))) == L"배터리 50%", "no estimate: just the charge");

    // ---- the edges of what Windows reports
    Check(FromRaw(R(0, 0, 50)).present && !FromRaw(R(0, 0, 50)).plugged, "flag 0 (between low and high, not charging): a battery on battery power");
    Check(FromRaw(R(1, 0, 50)).plugged && !FromRaw(R(1, 0, 50)).charging, "...and on mains power, not charging");
    Check(!FromRaw(R(1, 128 | 8, 50)).present, "'no system battery' wins over any other bit (a UPS or a desktop that says charging)");
    Check(!FromRaw(R(0, 1, 101)).present, "a charge over 100: not believed, no cell");
    Check(FromRaw(R(0, 4, 0)).present && FromRaw(R(0, 4, 0)).percent == 0, "0 %: still a battery");
    Check(FromRaw(R(0, 8, 40)).plugged, "charging while the AC line reads offline: on mains power all the same");
    Check(!FromRaw(R(255, 1, 40)).plugged && LevelOf(FromRaw(R(255, 1, 15))) == Level::Low, "AC line unknown: taken as on battery, so a low charge warns");
    Check(FromRaw(R(0, 1, 60, 0, 7u * 24 * 3600 - 1)).secondsLeft == 7 * 24 * 3600 - 1, "an estimate just under a week is shown");
    Check(FromRaw(R(0, 1, 60, 0, 7u * 24 * 3600)).secondsLeft == -1, "...a week or more is no estimate");
    Check(FromRaw(R(0, 1, 60, 2)).saver == false, "a system status other than 1: saver off");
    Check(LevelOf(FromRaw(R(0, 2, 11))) == Level::Low && LevelOf(FromRaw(R(0, 4, 0))) == Level::Critical, "11 %: low; 0 %: critical");
    Check(LevelOf(FromRaw(R(1, 1, 3))) == Level::Normal, "3 % plugged in but held (not charging): not alarming");
    Check(FillOf(FromRaw(R(0, 4, 1))) == 0.06f && FillOf(FromRaw(R(0, 4, 6))) == 0.06f && FillOf(FromRaw(R(0, 1, 7))) == 0.07f,
          "the sliver: 1 % to 6 % show 6 %, then the charge");
    Check(TimeLeftText(0) == L"약 1분" && TimeLeftText(89) == L"약 1분" && TimeLeftText(90) == L"약 2분", "under a minute shows a minute; rounded to the nearest minute");
    Check(TimeLeftText(3569) == L"약 59분" && TimeLeftText(3570) == L"약 1시간" && TimeLeftText(3 * 3600 + 30 * 60) == L"약 3시간 30분",
          "rounding up to the hour says hours");
    Check(Tooltip(State{}) == L"배터리 없음", "no battery: its tooltip says so");
    Check(Tooltip(FromRaw(R(1, 8, 30, 1))) == L"배터리 30% · 충전 중 · 절전 모드", "charging with saver on");
    Check(Tooltip(FromRaw(R(1, 8, 100))) == L"배터리 100% · 충전 중", "100 % and still charging (Windows tops it up): charging");
    Check(Tooltip(FromRaw(R(0, 1, 0, 0, 0))) == L"배터리 0% · 약 1분 남음", "0 % with an estimate of 0 s: about a minute");
    return Summary("battery_test");
}
