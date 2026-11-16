@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem The window preview's popup going between monitors of different scales (tests\preview_live_dpi.cpp): the
rem 150 %% screen of a laptop with a 100 %% monitor connected, say. It makes one window of its own on each monitor, opens the
rem dock's preview popup for it on each monitor in turn and checks that the popup has the place and size the preview
rem chose (it is moved with SetWindowPos and has no WM_DPICHANGED of its own). With one monitor, or all at one
rem scale, it only prints the scales. No mouse or keyboard input; the running dock is not touched. About 2 s.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\preview-live-dpi mkdir build\tests\preview-live-dpi
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\preview-live-dpi\ /Febuild\tests\preview-live-dpi\preview_live_dpi.exe tests\preview_live_dpi.cpp /link /Brepro user32.lib gdi32.lib dwmapi.lib || exit /b 1
build\tests\preview-live-dpi\preview_live_dpi.exe
exit /b %errorlevel%
