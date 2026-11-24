@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem Opens and closes the real Start menu a few times through the dock's start button code (src\start_menu.cpp):
rem first by the shell's request (the dock's first way since mod.36), then - the shell taken away - by the taskbar's
rem own Start button (src\taskbar_start.cpp) and by the Windows key, each by itself. A click opens Start, quick
rem clicks do not toggle it shut, a click with Start open closes it (the Windows key), another monitor's button
rem opens Start on that monitor (by the shell's request, then by that taskbar's button). It also prints measures
rem (no check on them): the taskbar button's toggle state while Start is closed and open, and the window in front
rem once the key closed Start. Hands off the mouse and keyboard for about 30 seconds. The running dock is not touched.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\start-menu mkdir build\tests\start-menu
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\start-menu\ /Febuild\tests\start-menu\start_menu_worker_test.exe tests\start_menu_worker_test.cpp ^
 /link /Brepro user32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib dwmapi.lib || exit /b 1
build\tests\start-menu\start_menu_worker_test.exe --live
exit /b %errorlevel%
