@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem How Windows tells the dock's start button whether Start is up (IAppVisibility, tests\start_menu_visibility_live.cpp):
rem how long it takes, and whether the object the dock keeps from its first press still sees Start after Explorer
rem restarts. YOU do the steps it prints (about 2 minutes): open and close Start with the Windows key, restart
rem Explorer from Task Manager (Processes, "Windows Explorer", Restart), open and close Start again, optionally
rem Win+S and Esc. The program only reads: it presses, sends and changes nothing. The running dock is not touched
rem (it handles an Explorer restart as it always does).
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\start-menu-visibility mkdir build\tests\start-menu-visibility
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\start-menu-visibility\ /Febuild\tests\start-menu-visibility\start_menu_visibility_live.exe tests\start_menu_visibility_live.cpp ^
 /link /Brepro user32.lib ole32.lib advapi32.lib || exit /b 1
build\tests\start-menu-visibility\start_menu_visibility_live.exe
exit /b %errorlevel%
