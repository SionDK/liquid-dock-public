@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem What the dock's window-in-front rules see on the real desktop (tests\dock_layout_live_probe.cpp): the
rem production fullscreen rule of dock.cpp and the shell pop-up rule against the real window in front, each
rem monitor's work area (an always-shown dock's strip), GetCursorPos failing (lock screen, UAC) and the mouse
rem buttons as the dock's drag reads them. One line whenever something changes; about 60 s (pass another
rem number of seconds after --logged).
rem It only reads: no input is sent, no window is created, moved or activated, the running dock is not touched.
rem Meanwhile put things in front by hand: F11 in a browser, a video or a game fullscreen, Win+Tab, Start,
rem Alt+Tab, a maximised window, a window spanned across two monitors; Win+L and back; hold each mouse button.
rem Run build.bat first (the probe links its objects).
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
for %%S in (admin_helper apps apps_click apps_pins apps_press audio_devices buttons capture common dock_controls dock_input dock_layout dock_notify dock_record dock_render dock_sensors dock_window fluent_flyout fluent_media fluent_placement fluent_settings_file gfx hwinfo native_tray oled oled_covers sensors settings_window settings_window_customizer settings_window_pages start_menu system system_taskbar taskbar_apps taskbar_start theme tray_host tray_host_icons tray_host_settings tray_host_widget window_preview) do if not exist build\%%S.obj (
  echo Run build.bat before this test.
  exit /b 2
)
if not exist build\tests\dock-layout-live mkdir build\tests\dock-layout-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\dock-layout-live\ /Febuild\tests\dock-layout-live\dock_layout_live_probe.exe tests\dock_layout_live_probe.cpp ^
 build\admin_helper.obj build\apps.obj build\apps_click.obj build\apps_pins.obj build\apps_press.obj build\audio_devices.obj build\buttons.obj build\capture.obj build\common.obj ^
 build\dock_controls.obj build\dock_input.obj build\dock_layout.obj build\dock_notify.obj build\dock_record.obj build\dock_render.obj build\dock_sensors.obj build\dock_window.obj ^
 build\fluent_flyout.obj build\fluent_media.obj build\fluent_placement.obj build\fluent_settings_file.obj build\gfx.obj build\hwinfo.obj build\native_tray.obj build\oled.obj build\oled_covers.obj build\sensors.obj ^
 build\settings_window.obj build\settings_window_customizer.obj build\settings_window_pages.obj build\start_menu.obj build\system.obj build\system_taskbar.obj build\taskbar_apps.obj build\taskbar_start.obj ^
 build\theme.obj build\tray_host.obj build\tray_host_icons.obj build\tray_host_settings.obj build\tray_host_widget.obj build\window_preview.obj ^
 /link /Brepro /SUBSYSTEM:CONSOLE d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib d3dcompiler.lib dxguid.lib windowsapp.lib ^
 shell32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib dwmapi.lib shlwapi.lib propsys.lib version.lib ^
 iphlpapi.lib advapi32.lib shcore.lib windowscodecs.lib uuid.lib pdh.lib comctl32.lib uxtheme.lib || exit /b 1
set "SECS=%~2"
if "%SECS%"=="" set "SECS=60"
build\tests\dock-layout-live\dock_layout_live_probe.exe %SECS%
exit /b %errorlevel%
