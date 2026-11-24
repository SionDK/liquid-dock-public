@echo off
rem Builds it only: running it touches the real taskbar and windows (mod.28).
rem tests\build_live_tests.bat builds it too, with the same lines.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\desktop-shell mkdir build\tests\desktop-shell
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\desktop-shell\ /Febuild\tests\desktop-shell\desktop_shell_test.exe ^
 tests\desktop_shell_test.cpp src\native_tray.cpp src\common.cpp ^
 /link /Brepro /SUBSYSTEM:CONSOLE user32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib || exit /b 1
exit /b 0
