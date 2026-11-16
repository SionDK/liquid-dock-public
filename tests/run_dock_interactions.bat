@echo off
rem Run build.bat first. Test fixtures are hidden and never inject mouse/keyboard input.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
for %%S in (admin_helper apps apps_click apps_pins apps_press buttons capture common dock_record fluent_flyout fluent_media fluent_placement fluent_settings_file gfx hwinfo native_tray sensors system system_taskbar tray_host tray_host_icons tray_host_settings tray_host_widget window_preview) do if not exist build\%%S.obj (
  echo Run build.bat before this test.
  exit /b 2
)
if not exist build\tests\dock-interaction mkdir build\tests\dock-interaction
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\dock-interaction\ /Febuild\tests\dock-interaction\dock_interaction_test.exe tests\dock_interaction_test.cpp ^
 build\admin_helper.obj build\apps.obj build\apps_click.obj build\apps_pins.obj build\apps_press.obj build\buttons.obj build\capture.obj build\common.obj build\dock_record.obj build\fluent_flyout.obj build\fluent_media.obj build\fluent_placement.obj build\fluent_settings_file.obj build\gfx.obj build\hwinfo.obj build\native_tray.obj build\sensors.obj build\system.obj build\system_taskbar.obj build\tray_host.obj build\tray_host_icons.obj build\tray_host_settings.obj build\tray_host_widget.obj build\window_preview.obj ^
 /link /Brepro /SUBSYSTEM:CONSOLE d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib d3dcompiler.lib dxguid.lib windowsapp.lib ^
 shell32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib dwmapi.lib shlwapi.lib propsys.lib version.lib ^
 iphlpapi.lib advapi32.lib shcore.lib windowscodecs.lib uuid.lib pdh.lib || exit /b 1
build\tests\dock-interaction\dock_interaction_test.exe %*
exit /b %errorlevel%
