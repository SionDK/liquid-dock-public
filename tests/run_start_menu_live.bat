@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem Opens and closes the real Start menu a few times through the dock's start button code (src\start_menu.cpp):
rem first by the taskbar's own Start button (src\taskbar_start.cpp), then by the shell's request as without it.
rem A click opens Start without the Windows key, quick clicks do not toggle it shut, a click with Start open
rem closes it, another monitor's button opens Start on that monitor. Hands off the mouse and keyboard for
rem about 30 seconds. The running dock is not touched.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\start-menu mkdir build\tests\start-menu
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\start-menu\ /Febuild\tests\start-menu\start_menu_worker_test.exe tests\start_menu_worker_test.cpp ^
 /link /Brepro user32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib || exit /b 1
build\tests\start-menu\start_menu_worker_test.exe --live
exit /b %errorlevel%
