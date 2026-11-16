#include "settings_window.h"
#include "settings_window_internal.h"
#include "dock.h"
#include "theme.h"
#include "oled.h"
#include "buttons.h"
#include "sensors.h"
#include "admin_helper.h"
#include "version.h"
#include <dwmapi.h>
#include <map>
#include "system.h"

namespace settingswindow {

std::wstring Num(float v, int decimals, const wchar_t* unit) {
    wchar_t b[32]; swprintf_s(b, L"%.*f%s", decimals, v, unit);
    return b;
}

void BuildPages() {
    g_pages.clear();

    Page shape{ L"모양", L"" };
    shape.controls = {
        { Control::Segmented, L"위치", nullptr, Apply::Rebuild, { L"아래", L"왼쪽", L"오른쪽", L"위" },
          [] { const int order[] = { 0, 1, 2, 3 }; return order[(int)g_settings.edge]; },
          [](int i) { g_settings.edge = (Edge)i; } },
        { Control::Group, L"독 크기와 위치 조정", L"아이콘 크기·간격, 독 두께, 가장자리와 거리, 가장자리 따라 위치" },
        { Control::Slider, L"아이콘 크기", nullptr, Apply::OnRelease, {}, {}, {}, 32, 64, 2,
          [] { return g_settings.iconSize; }, [](float v) { g_settings.iconSize = v; },
          [](float v) { return Num(v, 0, L""); } },
        { Control::Slider, L"아이콘 간격", nullptr, Apply::Live, {}, {}, {}, 10, 60, 2,
          [] { constexpr int preset[] = { 22, 32, 44 }; return (float)(g_settings.iconGap > 0 ? g_settings.iconGap : preset[std::clamp(g_settings.iconSpacing, 0, 2)]); },
          [](float v) { g_settings.iconGap = (int)v; },
          [](float v) { return Num(v, 0, L"%"); } },
        { Control::Slider, L"독 두께", L"아이콘 크기는 그대로 두고 유리 두께만 바뀝니다", Apply::Live, {}, {}, {}, 75, 150, 5,
          [] { return (float)g_settings.dockThickness; }, [](float v) { g_settings.dockThickness = (int)v; },
          [](float v) { return Num(v, 0, L"%"); } },
        { Control::Slider, L"화면 가장자리와 거리", nullptr, Apply::Live, {}, {}, {}, 0, 40, 2,
          [] { return (float)g_settings.edgeGap; }, [](float v) { g_settings.edgeGap = (int)v; },
          [](float v) { return v == 0 ? std::wstring(L"붙임") : Num(v, 0, L" px"); } },
        { Control::Slider, L"가장자리 따라 위치", nullptr, Apply::Live, {}, {}, {}, -100, 100, 5,
          [] { return (float)g_settings.edgeOffset; }, [](float v) { g_settings.edgeOffset = (int)v; },
          [](float v) {
              if (v == 0) return std::wstring(L"가운데");
              bool vert = IsVertical(g_settings.edge);
              return std::wstring(v < 0 ? (vert ? L"위로 " : L"왼쪽으로 ") : (vert ? L"아래로 " : L"오른쪽으로 ")) + Num(std::fabs(v), 0, L"%");
          } },
        { Control::Segmented, L"유리 질감", nullptr, Apply::Live, { L"맑게", L"보통", L"뿌옇게" },
          [] { return std::clamp(g_settings.glassStyle, 0, 2); }, [](int i) { g_settings.glassStyle = i; } },
        { Control::Segmented, L"시계 글자", nullptr, Apply::Live, { L"보통", L"크게" },
          [] { return std::clamp(g_settings.clockSize, 0, 1); }, [](int i) { g_settings.clockSize = i; } },
    };
    shape.controls[1].count = 5;   // the five sliders after it
    shape.reset = [] {
        Settings d; auto& s = g_settings;
        s.iconSize = d.iconSize; s.iconGap = d.iconGap; s.iconSpacing = d.iconSpacing; s.dockThickness = d.dockThickness;
        s.edgeGap = d.edgeGap; s.edgeOffset = d.edgeOffset; s.glassStyle = d.glassStyle; s.clockSize = d.clockSize;
    };

    Page behaviour{ L"동작", L"" };
    behaviour.controls = {
        { Control::Toggle, L"자동 숨김", L"끄면 늘 보이고 창이 독을 가리지 않습니다 (OLED 모드 화면 제외)", Apply::Live, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.autoHide; }, [](bool v) { g_settings.autoHide = v; } },
        { Control::Toggle, L"보조 모니터에서는 항상 표시", L"메인이 아닌 모니터의 독은 숨지 않습니다", Apply::Live, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.secondaryAlwaysShown; }, [](bool v) { g_settings.secondaryAlwaysShown = v; } },
        { Control::Toggle, L"시작 메뉴와 함께 표시", L"Windows 키로 시작 메뉴, 검색, 빠른 설정, 알림을 열면 숨어 있던 독도 같이 나옵니다", Apply::Live, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.showWithStart; }, [](bool v) { g_settings.showWithStart = v; } },
        { Control::Group, L"확대 효과와 속도 조정", L"확대 효과, 나타나는 속도, 숨는 속도" },
        { Control::Slider, L"확대 효과", L"아이콘에 마우스를 올렸을 때 커지는 정도", Apply::OnRelease, {}, {}, {}, 1.0f, 2.1f, 0.1f,
          [] { return g_settings.magnification; }, [](float v) { g_settings.magnification = v; },
          [](float v) { return v < 1.05f ? std::wstring(L"끄기") : Num(v, 1, L"배"); } },
        { Control::Slider, L"나타나는 속도", L"화면 가장자리에 마우스를 대고 독이 나올 때까지", Apply::None, {}, {}, {}, 0, 0.6f, 0.05f,
          [] { return g_settings.showDelay; }, [](float v) { g_settings.showDelay = v; },
          [](float v) { return v < 0.025f ? std::wstring(L"즉시") : Num(v, 2, L"초"); } },
        { Control::Slider, L"숨는 속도", L"마우스가 독을 벗어나고 독이 들어갈 때까지", Apply::None, {}, {}, {}, 0.1f, 1.5f, 0.05f,
          [] { return g_settings.hideDelay; }, [](float v) { g_settings.hideDelay = v; },
          [](float v) { return Num(v, 2, L"초"); } },
    };
    behaviour.controls[3].count = 3;   // the three sliders after it
    behaviour.reset = [] { Settings d; g_settings.autoHide = d.autoHide; g_settings.secondaryAlwaysShown = d.secondaryAlwaysShown; g_settings.showWithStart = d.showWithStart; g_settings.magnification = d.magnification; g_settings.showDelay = d.showDelay; g_settings.hideDelay = d.hideDelay; };

    Page buttons{ L"버튼", L"" };
    buttons.controls.clear();
    buttons.controls.push_back({ Control::Customizer, L"독에 표시할 버튼" });
    buttons.controls.push_back({ Control::Toggle, L"트레이 아이콘 표시 (실험)", L"작업 표시줄 설정에서 켠 아이콘을 › 옆에 2줄로", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
        [] { return g_settings.trayIcons; }, [](bool v) { g_settings.trayIcons = v; PostMessageW(g_mainWnd, WM_APP_TRAYHOST, 0, 0); } });
    buttons.controls.push_back({ Control::Toggle, L"FluentFlyout 연동", L"실행 중이면 작업 표시줄 위젯을 시작 버튼 왼쪽에, 플라이아웃은 독 위에", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
        [] { return g_settings.fluentFlyout; }, [](bool v) { g_settings.fluentFlyout = v; PostMessageW(g_mainWnd, WM_APP_TRAYHOST, 0, 0); } });
    buttons.controls.push_back({ Control::Segmented, L"OLED 모드를 적용할 모니터", nullptr, Apply::None, { L"모든 모니터", L"설정을 연 모니터만" },
        [] { return g_settings.oledOneMonitor ? 1 : 0; }, [](int i) { g_settings.oledOneMonitor = i == 1; } });
    buttons.controls.push_back({ Control::Toggle, L"OLED 모드 켜기", L"지금 바로 검은 배경으로 바꿉니다", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
        [] { return oled::Active(); },
        [](bool v) { if (v) oled::Enable(g_settings.oledOneMonitor ? g_monitor : nullptr); else oled::Disable(); } });
    buttons.controls.back().hasDefault = false;   // an action, not a stored setting
    buttons.controls.push_back({ Control::Toggle, L"Windows 다크 모드", nullptr, Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
        [] { return !theme::IsLight(); }, [](bool v) { if (v == theme::IsLight()) theme::Toggle(); } });
    buttons.controls.back().hasDefault = false;
    buttons.reset = [] {
        Settings d; g_settings.trayButtons = d.trayButtons; g_settings.oledOneMonitor = d.oledOneMonitor;
        if (g_settings.trayIcons != d.trayIcons || g_settings.fluentFlyout != d.fluentFlyout) {
            g_settings.trayIcons = d.trayIcons; g_settings.fluentFlyout = d.fluentFlyout;
            PostMessageW(g_mainWnd, WM_APP_TRAYHOST, 0, 0);
        }
    };

    Page system{ L"시스템", L"" };
    system.controls = {
        { Control::Toggle, L"Windows 시작 시 자동 실행", nullptr, Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.autostart; },
          [](bool v) { g_settings.autostart = v; PostMessageW(g_mainWnd, WM_APP_AUTOSTART, 0, 0); } },
        { Control::Segmented, L"독 열기 단축키", L"다른 프로그램이 이미 쓰는 조합이면 동작하지 않습니다", Apply::None,
          { L"끄기", L"Ctrl+Alt+D", L"Ctrl+Shift+Space", L"Win+Shift+D" },
          [] { return std::clamp(g_settings.revealHotkey, 0, 3); },
          [](int i) { g_settings.revealHotkey = i; PostMessageW(g_mainWnd, WM_APP_HOTKEY, 0, 0); } },
        { Control::Toggle, L"시작 버튼 우클릭 메뉴", L"작업 관리자, 실행, 잠금·절전·다시 시작·종료", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.startMenuExtras; }, [](bool v) { g_settings.startMenuExtras = v; } },
        { Control::Toggle, L"Shift+클릭으로 관리자 권한 실행", L"독의 앱 아이콘을 Shift를 누른 채 클릭 (스토어 앱 제외)", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.shiftRunsAsAdmin; }, [](bool v) { g_settings.shiftRunsAsAdmin = v; } },
        { Control::Toggle, L"관리자 권한 앱 창 제어", L"OBS 등의 창 · 설치·업데이트할 때마다 Windows 확인 한 번", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {},
          [] { return g_settings.adminWindows == 1; },
          [](bool v) { if (!v) g_settings.adminWindows = 2; else adminhelper::Enable(g_wnd); } },
        { Control::Button, L"Liquid Dock 종료", L"Windows 기본 작업표시줄로 돌아갑니다", Apply::None, {}, {}, {}, 0, 1, 1, {}, {}, {}, {}, {},
          [] { PostMessageW(g_mainWnd, WM_CLOSE, 0, 0); } },
    };

    // The sensors (sensorlogic::ShowItems): where temperatures come from, and what the "PC 상태" panel and the
    // dock's cell show. Every change rebuilds the docks: a horizontal dock's thickness follows the panel's rows.
    Page sensors{ L"센서", L"\xE9D9" };   // Diagnostic
    auto show = [](const wchar_t* label, const wchar_t* hint, bool Settings::* member) {
        return Control{ Control::Toggle, label, hint, Apply::Rebuild, {}, {}, {}, 0, 1, 1, {}, {}, {},
                        [member] { return g_settings.*member; }, [member](bool v) { g_settings.*member = v; } };
    };
    sensors.controls = {
        { Control::Segmented, L"온도 읽기", L"온도를 어디서 읽을지 정합니다 (모든 온도 항목에 함께 적용)", Apply::Rebuild,
          { L"끄기", L"HWiNFO Gadget", L"PawnIO" },
          [] { return std::clamp(g_settings.sensorSource, 0, 2); },
          // PawnIO: its driver there and the helper installed (one UAC prompt) - otherwise the choice stays
          [](int i) { if (i == sensorlogic::kSourcePawnIO) g_sensors.EnablePawnIO(g_wnd); else g_settings.sensorSource = i; } },
        { Control::Group, L"PC 상태에 보일 항목", L"끈 항목은 패널과 독 칸에서 빠지고 과열 알림도 울리지 않습니다" },
        show(L"CPU 온도", L"끄면 독 칸에 CPU 사용률이 대신 보입니다", &Settings::showCpuTemp),
        show(L"GPU", L"GPU 카드와 독 칸의 GPU 줄", &Settings::showGpu),
        show(L"VRAM", L"VRAM 사용량 줄과 막대 (온도 줄까지)", &Settings::showVram),
        show(L"VRAM 온도", L"VRAM 줄 아래의 온도 줄", &Settings::showVramTemp),
        show(L"RAM 슬롯 온도", L"메모리 모듈마다의 온도 줄 (HWiNFO에서만)", &Settings::showRamTemp),
        show(L"SSD", L"드라이브 줄 (이름과 온도)", &Settings::showSsd),
    };
    sensors.controls[1].count = 6;   // the six switches after it
    sensors.reset = [] {
        Settings d; auto& s = g_settings;
        s.sensorSource = d.sensorSource;
        s.showCpuTemp = d.showCpuTemp; s.showGpu = d.showGpu; s.showVram = d.showVram;
        s.showVramTemp = d.showVramTemp; s.showRamTemp = d.showRamTemp; s.showSsd = d.showSsd;
    };
    g_pages = { shape, behaviour, buttons, system, sensors };

    // Each control shows its default: read it against fresh settings (the installer turns autostart on).
    Settings saved = g_settings;
    g_settings = Settings{};
    g_settings.autostart = true;
    for (auto& p : g_pages)
        for (auto& c : p.controls) {
            if (c.type == Control::Slider) c.def = c.value();
            else if (c.type == Control::Segmented) c.def = (float)c.index();
            else if (c.type == Control::Toggle && c.hasDefault) c.def = c.on() ? 1.f : 0.f;
        }
    g_settings = saved;
}

}   // namespace settingswindow
