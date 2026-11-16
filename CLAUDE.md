# Liquid Dock (개조판) - Claude Code 작업 규칙

C++20 Win32 독(Explorer 작업 표시줄 대체), sous1345/liquid-dock의 개조판. 버전: src\version.h (0.2.0-mod.N).
설치본(실행 중인 독): %LOCALAPPDATA%\Programs\LiquidDock

## 꼭 지킬 것
- 커밋과 push는 사용자가 요청할 때만.
- 실행 중인 독을 끄거나 설치·재설치하기 전에 먼저 묻는다.
- 실기기 테스트는 사용자에게 알리고, 사용자가 손을 뗀 뒤 돌린다.
- 리소스(CPU·GPU·깨어남 횟수)를 늘리는 변경은 이유와 비용을 같이 적는다.
- 좌표·크기·DPI 코드는 100%와 150% 모두에서 맞아야 한다.

## 빌드와 배치 파일
- 저장소 폴더에서 돌린다. 배치 파일이 cl 환경을 잡는다. build.bat -> build\LiquidDock.exe, LiquidDock-Setup.exe (src\*.cpp 전부).
- 경로 없는 이름은 못 찾을 수 있다(NoDefaultCurrentDirectoryInExePath=1): .\ 를 붙인다.
  - PowerShell: cmd /c .\build.bat  ·  cmd /c "tests\run_regressions.bat --logged"
  - Git Bash:   cmd //c '.\build.bat'  ·  cmd //c "tests\run_regressions.bat --logged"
- 설치: data\를 복사해 둔 뒤 build\LiquidDock-Setup.exe /silent. PowerShell `Start-Process -Wait`는 쓰지 않는다(Setup이 띄운 독까지 기다려
  끝나지 않음). 끝나면 dock.log 첫 줄의 버전과 설치된 exe의 해시로 확인한다.
- --logged: 로그 래퍼와 pause 없이 출력이 바로 나온다. 없으면 build\tests\<스크립트 이름>.log에 쓰인다(한글은 CP949).
- 새 테스트 exe는 링크까지 확인한다. 쓰는 함수의 라이브러리를 링크 줄에 다 넣는다(CreateSolidBrush -> gdi32.lib 등).
- tests\build_*.bat에 박힌 vcvars 경로가 없는 PC에서는 tests\vsenv.bat 아래에서 cl 줄만 돌린다. 'vswhere.exe' 메시지는 무해.

## 테스트
- tests\run_regressions.bat: 단위·통합, 약 4분, GPU 필요, 실제 작업 표시줄·다른 창을 건드리지 않음.
  tests\run_dock_interactions.bat: 독 상호작용 매트릭스(build.bat 뒤, 숨은 창만).
  - 알려진 버그 rr-tests-1(고치지 않음): 충돌로 끝난 테스트도 통과로 센다(`if errorlevel 1`이 음수 종료 코드를 놓침).
    끝 줄만 믿지 말고 테스트마다 결과 줄(`ALL PASSED (N checks, 0 failures)`, `RESULT ...`)을 본다.
- sh tests/run_portable.sh: 판단 로직만(g++ 필요, g++가 없는 PC에서는 돌리지 않는다).
  같은 테스트가 run_regressions.bat에서 MSVC로 돈다.
- 실기기 테스트(실제 창·작업 표시줄·시작 메뉴): 한 번에 하나만. 도는 동안 사용자는 입력하지 않고, 테스트 콘솔(--logged면
  Claude Code 터미널)이 앞에 있어야 한다.
  - tests\run_taskbar_apps_live.bat (앱 단추, 약 10초, 독 켠 채)
  - tests\run_taskbar_apps_diag.bat (앱 단추 계측, 약 15초. `--logged dock`이면 독의 방식. 독을 켠 채·끈 채 비교 - 끄기 전에 묻는다)
  - tests\run_start_menu_live.bat (약 30초) · tests\run_preview_live.bat (모니터·위치마다 약 4초)

## 로그와 문서
- dock.log: 설치본의 data\ 아래(app click:, start:, preview:, taskbar:, tray:, magnify: 줄). 독을 켤 때마다 새로 쓰니,
  끄거나 설치하기 전에 필요하면 복사해 둔다.
- 사용자에게 보이는 동작이 바뀌면 README.md도 고친다.

## 소스 구조 (src\ 한 폴더, 기능 접두어)
- 나뉜 기능은 부분끼리 쓰는 선언과 파일 지도가 X_internal.h에 있다(Dock은 dock.h, AppModel은 apps.h).
- 테스트는 fake(매크로, stub)를 정의한 뒤 `#include "../src/X.cpp"`로 소스를 넣는다. 한 기능의 부분 파일은 같은 fake 아래
  모두 include하고, 부분과 내부 헤더는 원래 파일이 쓰던 헤더만 include한다. *_logic.h는 Windows 헤더 없이 테스트된다.
- .cpp를 더하면 tests\run_regressions.bat(DOCKOBJ), run_dock_interactions.bat, run_dock_layout_live.bat,
  build_overflow_*.bat의 목록을 손으로 고친다(build.bat은 자동).

| 기능 | .cpp |
|---|---|
| 진입점 | main (주 루프, MainProc, BuildDocks, 단축키) · main_instance (관리자 실행 넘기기, 크래시, watchdog) |
| 공통 | common (로그, settings.ini, Spring) · gfx · theme |
| 독 Dock | dock (프레임 상태, 확대, 드래그) · dock_window (창, 앱 바, 위젯 자리) · dock_input (WndProc, Click) · dock_layout (배치, 히트 테스트) · dock_render (유리, 아이콘) · dock_sensors (센서·배터리 셀, 패널) · dock_controls (작은 버튼, 메뉴) · dock_notify (트레이 아이콘) · dock_record |
| 앱 AppModel | apps (창 추적, 아이콘) · apps_pins (고정) · apps_click (클릭, 창 띄우기) · apps_press (작업 표시줄 단추 누르기, worker는 taskbar_apps) |
| 시작·미리보기 | start_menu · taskbar_start · window_preview |
| Windows 작업 표시줄 | system_taskbar (NativeTaskbar 숨기기·복원) · native_tray (숨겨진 아이콘·달력·빠른 설정 열기) |
| 트레이 호스트 | tray_host (창, 스레드) · tray_host_icons · tray_host_settings · tray_host_widget |
| FluentFlyout | fluent_flyout (위젯 창, Tick) · fluent_settings_file · fluent_media · fluent_placement |
| 시스템·센서 | system (볼륨·네트워크·배터리, 키 입력) · audio_devices · sensors · hwinfo · capture |
| 버튼·설정 창 | buttons · settings_window · settings_window_pages · settings_window_customizer |
| OLED·관리자 창 | oled · oled_covers (덮개 창) · admin_helper |
