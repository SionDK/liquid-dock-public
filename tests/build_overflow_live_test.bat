@echo off
rem Builds it only: running it touches the real taskbar and windows (mod.28).
rem tests\build_live_tests.bat builds it too, with the same lines.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
call "%~dp0lists.bat"
cd /d "%~dp0.."
if not exist build\tests\overflow-live mkdir build\tests\overflow-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\overflow-live\ /Febuild\tests\overflow-live\overflow_live_test.exe ^
 tests\overflow_live_test.cpp src\native_tray.cpp %OVERFLOWSRC% ^
 /link /Brepro user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib iphlpapi.lib advapi32.lib || exit /b 1
exit /b 0
