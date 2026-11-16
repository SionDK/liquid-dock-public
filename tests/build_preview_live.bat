@echo off
rem Builds tests\preview_live_test.cpp (the window preview with real windows and real DWM) without running it;
rem tests\run_preview_live.bat builds and runs it. Needs no build.bat: the test compiles the preview's own code.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\preview-live mkdir build\tests\preview-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\preview-live\ /Febuild\tests\preview-live\preview_live_test.exe tests\preview_live_test.cpp /link /Brepro user32.lib gdi32.lib dwmapi.lib ole32.lib || exit /b 1
exit /b 0
