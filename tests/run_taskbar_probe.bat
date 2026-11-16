@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem Shows what UI Automation sees of Explorer's taskbars: where their Start button and app buttons are and how
rem they could be pressed (what the dock's start button relies on: src\taskbar_start.cpp). Reads only: nothing
rem is pressed, moved or changed. Run it while the dock runs (the taskbar hidden, as the dock keeps it); once
rem more with the dock closed (the taskbar shown) tells what changes when the taskbar hides.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\probe mkdir build\tests\probe
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\probe\ /Febuild\tests\probe\taskbar_probe.exe tests\taskbar_probe.cpp ^
 /link /Brepro user32.lib gdi32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib shell32.lib || exit /b 1
build\tests\probe\taskbar_probe.exe
exit /b %errorlevel%
