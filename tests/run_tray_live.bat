@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem Tray icon clicks by hand (tests\tray_live_coords.cpp): two test icons - one of a window that is not DPI
rem aware, one of a per-monitor aware window - are added to the tray. You click each of them, left and right,
rem first in Windows' own hidden-icons window, then on the dock (after turning them on under Settings >
rem Personalization > Taskbar > Other system tray icons; the dock needs its tray icons setting on). Each click
rem logs the coordinates the program got, where the pointer is for that window and the icon's rectangle; the
rem verdict says whether the dock gives the same as Windows. Most useful on a 150% screen.
rem The test presses nothing itself; the prompts show in this window. Up to about 3.5 minutes.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\tray-live mkdir build\tests\tray-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\tray-live\ /Febuild\tests\tray-live\tray_live_coords.exe tests\tray_live_coords.cpp /link /Brepro user32.lib shell32.lib || exit /b 1
build\tests\tray-live\tray_live_coords.exe
exit /b %errorlevel%
