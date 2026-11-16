@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem Unit and integration tests that need no running dock and touch no real taskbar, registry setting or
rem other program's window (each test works on its own fakes and names). One line per check; exit code
rem 0 = all passed. The portable ones also build without Windows: tests/run_portable.sh
rem About 4 minutes; the dock's own tests at the end need the GPU (Direct3D / Direct2D).
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests mkdir build\tests
set "CF=/nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\"
rem ---- portable (no Windows headers)
cl %CF% /Febuild\tests\fluent_logic_test.exe tests\fluent_logic_test.cpp || exit /b 1
cl %CF% /Febuild\tests\fluent_settings_test.exe tests\fluent_settings_test.cpp || exit /b 1
cl %CF% /Febuild\tests\icon_pixels_test.exe tests\icon_pixels_test.cpp || exit /b 1
cl %CF% /Febuild\tests\admin_protocol_test.exe tests\admin_protocol_test.cpp || exit /b 1
cl %CF% /Febuild\tests\app_click_test.exe tests\app_click_test.cpp || exit /b 1
cl %CF% /Febuild\tests\taskbar_apps_test.exe tests\taskbar_apps_test.cpp || exit /b 1
cl %CF% /Febuild\tests\tray_buttons_test.exe tests\tray_buttons_test.cpp || exit /b 1
cl %CF% /Febuild\tests\battery_test.exe tests\battery_test.cpp || exit /b 1
cl %CF% /Febuild\tests\start_menu_test.exe tests\start_menu_test.cpp || exit /b 1
cl %CF% /Febuild\tests\sys_poll_test.exe tests\sys_poll_test.cpp || exit /b 1
cl %CF% /Febuild\tests\idle_wait_test.exe tests\idle_wait_test.cpp || exit /b 1
cl %CF% /Febuild\tests\refreshed_test.exe tests\refreshed_test.cpp || exit /b 1
cl %CF% /Febuild\tests\sensor_logic_test.exe tests\sensor_logic_test.cpp || exit /b 1
cl %CF% /Febuild\tests\hwinfo_test.exe tests\hwinfo_test.cpp src\hwinfo.cpp /link /Brepro advapi32.lib || exit /b 1
rem ---- Windows integration
cl %CF% /Febuild\tests\taskbar_recovery_test.exe tests\taskbar_recovery_test.cpp /link /Brepro user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib iphlpapi.lib advapi32.lib || exit /b 1
cl %CF% /Febuild\tests\installer_cleanup_test.exe tests\installer_cleanup_test.cpp /link /Brepro user32.lib advapi32.lib shell32.lib ole32.lib comctl32.lib || exit /b 1
cl %CF% /Febuild\tests\tray_host_test.exe tests\tray_host_test.cpp /link /Brepro user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib dwmapi.lib || exit /b 1
cl %CF% /Febuild\tests\fluent_flyout_test.exe tests\fluent_flyout_test.cpp /link /Brepro user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib version.lib dwmapi.lib || exit /b 1
cl %CF% /Febuild\tests\admin_helper_test.exe tests\admin_helper_test.cpp /link /Brepro user32.lib advapi32.lib || exit /b 1
rem the start button's worker, safe mode: neither the taskbar's Start button nor the shell is touched and no key goes out
rem (tests\run_start_menu_live.bat: the real Start)
cl %CF% /Febuild\tests\start_menu_worker_test.exe tests\start_menu_worker_test.cpp /link /Brepro user32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib || exit /b 1
cl %CF% /Febuild\tests\sys_poller_test.exe tests\sys_poller_test.cpp /link /Brepro user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib iphlpapi.lib advapi32.lib || exit /b 1
cl %CF% /Febuild\tests\audio_devices_test.exe tests\audio_devices_test.cpp /link /Brepro ole32.lib oleaut32.lib uuid.lib propsys.lib user32.lib || exit /b 1
rem the window preview with fake windows and a fake DWM (tests\run_preview_live.bat: real windows, real DWM)
cl %CF% /Febuild\tests\window_preview_test.exe tests\window_preview_test.cpp /link /Brepro user32.lib gdi32.lib dwmapi.lib || exit /b 1
rem the preview's rules (window_preview.cpp), the OLED covers (oled.cpp, oled_covers.cpp: fake wallpaper API, monitors and z-order) and
rem the GDI capture (capture.cpp: fake screen DC, WARP device), each on fakes
cl %CF% /Febuild\tests\preview_rules_test.exe tests\preview_rules_test.cpp /link /Brepro user32.lib gdi32.lib dwmapi.lib || exit /b 1
cl %CF% /Febuild\tests\preview_oled_test.exe tests\preview_oled_test.cpp /link /Brepro user32.lib gdi32.lib ole32.lib shell32.lib uuid.lib || exit /b 1
cl %CF% /Febuild\tests\preview_capture_test.exe tests\preview_capture_test.cpp /link /Brepro d3d11.lib windowsapp.lib user32.lib gdi32.lib || exit /b 1
rem the app model (apps*.cpp) with a fake window system: no other program's window is looked at, nothing is started,
rem no registry value is read; pins and settings go to build\tests\data
cl %CF% /Febuild\tests\apps_model_test.exe tests\apps_model_test.cpp src\common.cpp /link /Brepro user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib shlwapi.lib propsys.lib version.lib advapi32.lib dwmapi.lib windowsapp.lib || exit /b 1
rem the taskbar app buttons' worker with fake UI Automation and a fake Explorer (tests\run_taskbar_apps_live.bat: the real taskbar)
cl %CF% /Febuild\tests\taskbar_apps_worker_test.exe tests\taskbar_apps_worker_test.cpp /link /Brepro user32.lib ole32.lib oleaut32.lib shell32.lib uuid.lib || exit /b 1
rem the start button: a click's run with keys, presses and Explorer faked; the Start button of a taskbar of the test's
rem own (UI Automation provider; Explorer's taskbar only read by Classic()); which Shell_TrayWnd is Explorer's (fake lookups)
cl %CF% /Febuild\tests\start_menu_run_test.exe tests\start_menu_run_test.cpp /link /Brepro user32.lib ole32.lib uuid.lib advapi32.lib || exit /b 1
cl %CF% /Febuild\tests\start_menu_taskbar_test.exe tests\start_menu_taskbar_test.cpp /link /Brepro user32.lib ole32.lib oleaut32.lib uuid.lib uiautomationcore.lib || exit /b 1
cl %CF% /Febuild\tests\start_menu_explorer_taskbar_test.exe tests\start_menu_explorer_taskbar_test.cpp /link /Brepro user32.lib || exit /b 1
rem the tray host's edge cases (test names; its own HKCU\Software\LiquidDockTest-edge, removed at the end; about 20 s),
rem the hidden-icons button against an off-screen fake taskbar and a fake SHAppBarMessage - and the calendar and quick
rem settings by its clock and network button, the taskbar hidden already (its tray under a hidden XAML island), the
rem keys only recorded -, Explorer's taskbar and the taskbar opacity guard with fake window calls
cl %CF% /Febuild\tests\tray_host_edge_test.exe tests\tray_host_edge_test.cpp /link /Brepro user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib dwmapi.lib || exit /b 1
cl %CF% /Febuild\tests\tray_overflow_test.exe tests\tray_overflow_test.cpp /link /Brepro user32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib uiautomationcore.lib || exit /b 1
cl %CF% /Febuild\tests\tray_explorer_taskbar_test.exe tests\tray_explorer_taskbar_test.cpp || exit /b 1
cl %CF% /Febuild\tests\tray_visibility_test.exe tests\tray_visibility_test.cpp || exit /b 1
rem FluentFlyout over its life: this test started again as a fake FluentFlyout, test class names and title, cloaked
rem flyouts (about 15 s)
cl %CF% /Febuild\tests\fluent_lifecycle_test.exe tests\fluent_lifecycle_test.cpp /link /Brepro user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib version.lib dwmapi.lib || exit /b 1
rem the elevated helper's service on the test's own pipe, and the dock's policy for elevated windows (fake pipe,
rem prompt and %ProgramFiles% - a folder of the test)
cl %CF% /Febuild\tests\admin_service_edge_test.exe tests\admin_service_edge_test.cpp /link /Brepro user32.lib advapi32.lib || exit /b 1
cl %CF% /Febuild\tests\admin_policy_test.exe tests\admin_policy_test.cpp /link /Brepro user32.lib advapi32.lib ole32.lib || exit /b 1
rem HWiNFO's values from a hive file of the test (RegLoadAppKey: HKCU is not written), system.cpp's shared state
rem (fake adapter lists and power status), the sensor sampler and the panel's drive list and memory slots (fake
rem drives, volumes and SMBIOS table)
cl %CF% /Febuild\tests\system_hwinfo_reader_test.exe tests\system_hwinfo_reader_test.cpp /link /Brepro advapi32.lib || exit /b 1
cl %CF% /Febuild\tests\system_state_test.exe tests\system_state_test.cpp /link /Brepro user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib iphlpapi.lib advapi32.lib || exit /b 1
cl %CF% /Febuild\tests\system_sampler_test.exe tests\system_sampler_test.cpp /link /Brepro advapi32.lib pdh.lib || exit /b 1
rem the elevated helper's sensor readings: the dock's side (sensors.cpp: fake shared memory, want event, helper start
rem and clock) and the helper's main loop (sensors_main.cpp: its own clock, fake driver and sampler; its main is not run)
cl %CF% /Febuild\tests\sensors_client_test.exe tests\sensors_client_test.cpp src\hwinfo.cpp /link /Brepro user32.lib advapi32.lib shell32.lib pdh.lib || exit /b 1
cl %CF% /Febuild\tests\sensors_loop_test.exe tests\sensors_loop_test.cpp /link /Brepro user32.lib advapi32.lib pdh.lib secur32.lib shell32.lib ole32.lib || exit /b 1
for %%T in (fluent_logic fluent_settings icon_pixels admin_protocol app_click taskbar_apps tray_buttons battery start_menu sys_poll idle_wait refreshed sensor_logic hwinfo taskbar_recovery installer_cleanup tray_host fluent_flyout admin_helper start_menu_worker sys_poller audio_devices window_preview preview_rules preview_oled preview_capture apps_model taskbar_apps_worker start_menu_run start_menu_taskbar start_menu_explorer_taskbar tray_host_edge tray_overflow tray_explorer_taskbar tray_visibility fluent_lifecycle admin_service_edge admin_policy system_hwinfo_reader system_state system_sampler sensors_client sensors_loop) do (
  build\tests\%%T_test.exe
  if errorlevel 1 (echo FAILED: %%T_test& exit /b 1)
)
rem ---- the production Dock (dock.cpp, dock_layout.cpp, dock_render.cpp, ...) with hidden windows of its own, fake
rem monitors and a fake shell, on the real GPU. Each in a folder of its own: its data folder (settings, pins, dock.log)
rem is next to the exe. The other modules are compiled once into build\tests\*.obj (dock_render_device_test's line)
rem and linked from there by the tests after it.
if not exist build\tests\dock-render mkdir build\tests\dock-render
if not exist build\tests\dock-layout mkdir build\tests\dock-layout
cl %CF% /Febuild\tests\dock-render\dock_render_settings_test.exe tests\dock_render_settings_test.cpp src\common.cpp /link /Brepro /SUBSYSTEM:CONSOLE user32.lib || exit /b 1
cl %CF% /Febuild\tests\dock-render\dock_render_device_test.exe tests\dock_render_device_test.cpp src\admin_helper.cpp src\apps.cpp src\apps_click.cpp src\apps_pins.cpp src\apps_press.cpp src\buttons.cpp src\capture.cpp src\common.cpp src\dock_record.cpp src\fluent_flyout.cpp src\fluent_media.cpp src\fluent_placement.cpp src\fluent_settings_file.cpp src\gfx.cpp src\hwinfo.cpp src\native_tray.cpp src\sensors.cpp src\system.cpp src\system_taskbar.cpp src\tray_host.cpp src\tray_host_icons.cpp src\tray_host_settings.cpp src\tray_host_widget.cpp src\window_preview.cpp /link /Brepro /SUBSYSTEM:CONSOLE d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib d3dcompiler.lib dxguid.lib windowsapp.lib shell32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib dwmapi.lib shlwapi.lib propsys.lib version.lib iphlpapi.lib advapi32.lib shcore.lib windowscodecs.lib uuid.lib pdh.lib || exit /b 1
set "DOCKOBJ=build\tests\admin_helper.obj build\tests\apps.obj build\tests\apps_click.obj build\tests\apps_pins.obj build\tests\apps_press.obj build\tests\buttons.obj build\tests\capture.obj build\tests\common.obj build\tests\dock_record.obj build\tests\fluent_flyout.obj build\tests\fluent_media.obj build\tests\fluent_placement.obj build\tests\fluent_settings_file.obj build\tests\gfx.obj build\tests\hwinfo.obj build\tests\native_tray.obj build\tests\sensors.obj build\tests\system.obj build\tests\system_taskbar.obj build\tests\tray_host.obj build\tests\tray_host_icons.obj build\tests\tray_host_settings.obj build\tests\tray_host_widget.obj build\tests\window_preview.obj"
set "DOCKLIB=d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib d3dcompiler.lib dxguid.lib windowsapp.lib shell32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib dwmapi.lib shlwapi.lib propsys.lib version.lib iphlpapi.lib advapi32.lib shcore.lib windowscodecs.lib uuid.lib pdh.lib"
cl %CF% /Febuild\tests\dock-render\dock_render_settings_window_test.exe tests\dock_render_settings_window_test.cpp %DOCKOBJ% /link /Brepro /SUBSYSTEM:CONSOLE %DOCKLIB% || exit /b 1
cl %CF% /Febuild\tests\dock-layout\dock_layout_test.exe tests\dock_layout_test.cpp %DOCKOBJ% /link /Brepro /SUBSYSTEM:CONSOLE %DOCKLIB% || exit /b 1
for %%T in (dock-render\dock_render_settings dock-render\dock_render_device dock-render\dock_render_settings_window dock-layout\dock_layout) do (
  build\tests\%%T_test.exe
  if errorlevel 1 (echo FAILED: %%T_test& exit /b 1)
)
rem ---- tests written with the local-LLM task pack (tools\llm-tasks, run_tasks.py --apply), if there are any
for %%F in (tests\llm\*_test.cpp) do (
  cl %CF% /I. /Febuild\tests\%%~nF.exe %%F src\hwinfo.cpp /link /Brepro advapi32.lib
  if errorlevel 1 exit /b 1
  build\tests\%%~nF.exe
  if errorlevel 1 (echo FAILED: %%~nF& exit /b 1)
)
echo all regression tests passed
exit /b 0
