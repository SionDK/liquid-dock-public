@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem Compiles and links, and never runs, the tests that run_regressions.bat does not build: the live tests on the
rem real taskbar, Start, windows and DWM, the probes, and the ones no script runs (overflow_live_test,
rem overflow_workspace_test, desktop_shell_test). A source refactoring that renames or moves what one of them
rem uses then fails here, not on the day someone runs that test on a PC.
rem Each with the same cl and link line as its own script (named after it), into build\tests\live-build\<test>\,
rem so the programs the live scripts build and run are not overwritten. Nothing is started: no window, no input.
rem Run build.bat first: dock_layout_live_probe links build.bat's objects (PROBEOBJ, tests\lists.bat); without
rem them it is reported as not built and the rest still build. About 40 seconds.
rem When a live script's cl or link line changes, change its line here too.
rem Not here: start_menu_worker_test (run_start_menu_live.bat; run_regressions.bat builds it with the same line),
rem window_preview_test (run_window_preview.bat; the same), dock_interaction_test and settings_options_test (their
rem own scripts build and run them), watchdog_handshake_test.ps1 (not compiled).
rem Exit code 0 = every one built; the last line says how many.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
call "%~dp0lists.bat"
cd /d "%~dp0.."
set "CF=/nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE"
set "OUT=build\tests\live-build"
if not exist %OUT% mkdir %OUT%
set /a TOTAL=0
set /a BUILT=0
set "FAILED="
rem          test                         sources besides tests\<test>.cpp        after /link /Brepro
rem run_admin_live.bat
call :build admin_live_pipe_test        ""                                       "user32.lib advapi32.lib shell32.lib ole32.lib version.lib"
rem run_apps_model_live.bat
call :build apps_model_live_test        "src\common.cpp"                         "user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib shlwapi.lib propsys.lib version.lib advapi32.lib dwmapi.lib windowsapp.lib"
rem run_dock_layout_live.bat (build.bat's objects)
set "NOOBJ="
for %%O in (%PROBEOBJ%) do if not exist %%O set "NOOBJ=%%O"
if defined NOOBJ (
  echo dock_layout_live_probe: NOT BUILT - %NOOBJ% is missing, run build.bat first
  set /a TOTAL+=1
  set "FAILED=%FAILED% dock_layout_live_probe"
) else (
  call :build dock_layout_live_probe "%PROBEOBJ%" "/SUBSYSTEM:CONSOLE %PROBELIB%"
)
rem run_dock_render_live.bat
call :build dock_render_live_wheel      ""                                       "/SUBSYSTEM:CONSOLE user32.lib gdi32.lib"
rem run_fluent_live.bat
call :build fluent_live_probe           ""                                       "user32.lib gdi32.lib shell32.lib ole32.lib shcore.lib"
rem run_ghost_front_live.bat
call :build ghost_front_live_test       ""                                       "user32.lib dwmapi.lib"
rem build_preview_live.bat (run_preview_live.bat)
call :build preview_live_test           ""                                       "user32.lib gdi32.lib dwmapi.lib ole32.lib"
rem run_preview_live_dpi.bat
call :build preview_live_dpi            ""                                       "user32.lib gdi32.lib dwmapi.lib"
rem run_start_ghost_live.bat
call :build start_ghost_live_test       "src\common.cpp"                         "user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib shlwapi.lib propsys.lib version.lib advapi32.lib dwmapi.lib windowsapp.lib"
rem run_start_menu_visibility_live.bat
call :build start_menu_visibility_live  ""                                       "user32.lib ole32.lib advapi32.lib"
rem run_system_live.bat
call :build system_live_hwinfo          "src\hwinfo.cpp"                         "advapi32.lib"
rem run_taskbar_apps_diag.bat
call :build taskbar_apps_diag           ""                                       "user32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib dwmapi.lib gdi32.lib"
rem run_taskbar_apps_live.bat
call :build taskbar_apps_live_test      ""                                       "user32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib dwmapi.lib gdi32.lib"
rem run_taskbar_apps_live_lookup.bat
call :build taskbar_apps_live_lookup    ""                                       "user32.lib gdi32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib dwmapi.lib advapi32.lib"
rem run_taskbar_probe.bat
call :build taskbar_probe               ""                                       "user32.lib gdi32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib shell32.lib"
rem run_tray_live.bat
call :build tray_live_coords            ""                                       "user32.lib shell32.lib"
rem run_tray_uid_live.bat
call :build tray_uid_live               ""                                       "user32.lib shell32.lib advapi32.lib"
rem build_desktop_shell_test.bat (no script runs it)
call :build desktop_shell_test          "src\native_tray.cpp src\common.cpp"     "/SUBSYSTEM:CONSOLE user32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib"
rem build_overflow_live_test.bat (no script runs it)
call :build overflow_live_test          "src\native_tray.cpp %OVERFLOWSRC%"      "user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib iphlpapi.lib advapi32.lib"
rem build_overflow_workspace.bat (no script runs it)
call :build overflow_workspace_test     "%OVERFLOWSRC%"                          "user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib iphlpapi.lib advapi32.lib"
if defined FAILED (
  echo live tests built: %BUILT% of %TOTAL% - NOT BUILT:%FAILED%
  exit /b 1
)
echo live tests built: %BUILT% of %TOTAL%, none run
exit /b 0

rem %1 the test (tests\%1.cpp), %2 the other sources or objects, %3 what follows /link /Brepro
:build
set /a TOTAL+=1
if not exist %OUT%\%1 mkdir %OUT%\%1
cl %CF% /Fo%OUT%\%1\ /Fe%OUT%\%1\%1.exe tests\%1.cpp %~2 /link /Brepro %~3 || (
  echo %1: NOT BUILT
  set "FAILED=%FAILED% %1"
  exit /b 1
)
set /a BUILT+=1
exit /b 0
