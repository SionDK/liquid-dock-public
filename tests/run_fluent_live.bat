@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" %1 & exit /b)
rem FluentFlyout with the dock on this PC, observed only (tests\fluent_live_probe.cpp): the monitors in
rem FluentFlyout's numbering, Explorer's taskbars and the dock's host window, FluentFlyout's settings file,
rem where its widget window is and whether it fits there, its flyouts for 25 s, and the dock's log lines
rem about FluentFlyout. Nothing is moved, clicked or sent; the running dock and FluentFlyout are not touched.
rem Needs FluentFlyout running with its taskbar widget on, and the dock running.
rem While it watches (25 s): bring the dock up, press play/pause or a volume key once, wait until the flyout
rem starts to fade (about 3 s) and press it again right then - two or three times.
rem A number sets how long it watches: run_fluent_live.bat 40, or cmd /c "tests\run_fluent_live.bat --logged 40"
rem (0: no watching). Exit code: the number of FAIL verdicts, 2 when FluentFlyout is not running.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\fluent-live mkdir build\tests\fluent-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\fluent-live\ /Febuild\tests\fluent-live\fluent_live_probe.exe tests\fluent_live_probe.cpp /link /Brepro user32.lib gdi32.lib shell32.lib ole32.lib shcore.lib || exit /b 1
build\tests\fluent-live\fluent_live_probe.exe %2
exit /b %errorlevel%
