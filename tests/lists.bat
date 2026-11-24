@echo off
rem The lists of modules and libraries the dock's tests link, in one place. Called without setlocal
rem (call "%~dp0lists.bat" after the caller's own setlocal), so the variables stay in the caller's environment:
rem   DOCK_MODULES   the modules outside the Dock that the dock's own tests link
rem     DOCKSRC        src\<module>.cpp - run_regressions.bat compiles them once (dock_render_device_test's line)
rem     DOCKOBJ        build\tests\<module>.obj - what that line leaves, linked by the dock tests after it
rem     DOCKBUILDOBJ   build\<module>.obj - build.bat's, linked by run_dock_interactions.bat
rem   PROBE_MODULES  every module of src\ but main, main_autostart, main_instance and dock (dock_layout_live_probe includes
rem                  dock.cpp itself and links the rest of build.bat's objects)
rem     PROBEOBJ       build\<module>.obj (run_dock_layout_live.bat, build_live_tests.bat)
rem   DOCKLIB        the dock tests' libraries; PROBELIB the probe's (two more)
rem   OVERFLOW_MODULES / OVERFLOWSRC  what the tray overflow live tests compile besides native_tray.cpp
rem                  (build_overflow_live_test.bat, build_overflow_workspace.bat, build_live_tests.bat)
rem Keep each list in the order it has: the objects are linked in the order of the command line, and the
rem test programs are compared byte for byte between builds (/Brepro). One space between names.
rem A new .cpp in src\: a part of the Dock (dock*.cpp) goes into tests\parts_dock.inc (the tests include it)
rem and PROBE_MODULES, never into DOCK_MODULES (it would be defined twice); any other module the dock links goes into
rem DOCK_MODULES and PROBE_MODULES, at its place in the alphabet.
set "DOCK_MODULES=admin_helper admin_helper_process apps apps_click apps_pins apps_press buttons capture common dock_record fluent_flyout fluent_media fluent_placement fluent_settings_file gfx hwinfo native_taskbar native_tray sensors shell_front system tray_host tray_host_icons tray_host_settings tray_host_widget window_preview"
set "DOCKSRC=src\%DOCK_MODULES: =.cpp src\%.cpp"
set "DOCKOBJ=build\tests\%DOCK_MODULES: =.obj build\tests\%.obj"
set "DOCKBUILDOBJ=build\%DOCK_MODULES: =.obj build\%.obj"
set "PROBE_MODULES=admin_helper admin_helper_process apps apps_click apps_pins apps_press audio_devices buttons capture common dock_controls dock_input dock_layout dock_notify dock_panel dock_record dock_render dock_sensors dock_window fluent_flyout fluent_media fluent_placement fluent_settings_file gfx hwinfo native_taskbar native_tray oled oled_covers sensors settings_window settings_window_customizer settings_window_pages shell_front start_menu system taskbar_apps taskbar_start theme tray_host tray_host_icons tray_host_settings tray_host_widget window_preview"
set "PROBEOBJ=build\%PROBE_MODULES: =.obj build\%.obj"
set "DOCKLIB=d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib d3dcompiler.lib dxguid.lib windowsapp.lib shell32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib dwmapi.lib shlwapi.lib propsys.lib version.lib iphlpapi.lib advapi32.lib shcore.lib windowscodecs.lib uuid.lib pdh.lib"
set "PROBELIB=%DOCKLIB% comctl32.lib uxtheme.lib"
set "OVERFLOW_MODULES=system shell_front native_taskbar taskbar_start common"
set "OVERFLOWSRC=src\%OVERFLOW_MODULES: =.cpp src\%.cpp"
exit /b 0
