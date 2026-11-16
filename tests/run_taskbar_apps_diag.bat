@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" %1 & exit /b)
rem What a press on a taskbar app button really does (tests\taskbar_apps_diag.cpp). It presses the button of a
rem window of its own only - three times, from three starting states it makes itself (behind the console, in front,
rem minimized) - and records whether Explorer got the right to the front, every way the app buttons offer to press
rem them, the shell's flashes and activations, the foreground changes and the messages that reach its window. Like
rem the dock, it takes the right to change the front window with a key no program acts on (0xE8). By default it
rem presses by the accessibility default action, which the dock no longer uses (on a 25H2 build it answered
rem S_OK and did nothing); "run_taskbar_apps_diag.bat dock" (with --logged: "... --logged dock") presses the
rem dock's own way instead. To tell the hidden taskbar from the shown one, run it once with the dock running and once
rem with it closed (LiquidDock.exe --quit; start it again from its logon task). About 15 s: keep this window in front
rem and leave the mouse and keyboard alone meanwhile. No other program's window is pressed or moved.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\taskbar-apps-diag mkdir build\tests\taskbar-apps-diag
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\taskbar-apps-diag\ /Febuild\tests\taskbar-apps-diag\taskbar_apps_diag.exe tests\taskbar_apps_diag.cpp /link /Brepro user32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib dwmapi.lib gdi32.lib || exit /b 1
build\tests\taskbar-apps-diag\taskbar_apps_diag.exe %2
exit /b %errorlevel%
