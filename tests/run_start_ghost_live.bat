@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem An app icon clicked right after the dock opened Start, live (tests\start_ghost_live_test.cpp): opens the real Start
rem the dock's way three times, writes down what holds the front (program, cloaked, IAppVisibility), brings a test window
rem of another process up the dock's raw way and with the dock's own code (apps_click.cpp BringUp: Start shown - Esc
rem first), and closes Start once with the dock's start button - the Windows key since mod.36 - to see whether a
rem closed shell window keeps the front.
rem Keys: Esc only while the shell's window is in front and Start shows, the unassigned key for the right, the Windows
rem key once (the close in C; also where the dock's ladder or the tidying up falls back on it). No mouse.
rem About 20 s. Hands off the mouse and keyboard meanwhile, the console in front at the start. The running dock is
rem not touched (it may come up with Start).
rem   run_start_ghost_live.bat --logged click   instead: Start opened, then a window like the dock's (no activation) at
rem             the top left clicked with the real mouse - the case seen on a test PC (2026-11-17); then one way out each round
rem             (none, Esc to the shell's window in front, SwitchToThisWindow, the click activating that window). Mouse
rem             input only on the test's own windows (checked with WindowFromPoint), the pointer put back. About 30 s.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\start-ghost mkdir build\tests\start-ghost
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\start-ghost\ /Febuild\tests\start-ghost\start_ghost_live_test.exe tests\start_ghost_live_test.cpp src\common.cpp /link /Brepro user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib shlwapi.lib propsys.lib version.lib advapi32.lib dwmapi.lib windowsapp.lib || exit /b 1
build\tests\start-ghost\start_ghost_live_test.exe %2
exit /b %errorlevel%
