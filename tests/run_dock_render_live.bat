@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem Which mouse-wheel messages this PC's touchpad and mouse really send (tests\dock_render_live_wheel.cpp), for
rem the settings window's wheel handling. A window of its own opens for 15 s: scroll over it slowly and quickly
rem with two fingers on the touchpad, then with a mouse wheel if there is one. It counts the messages that carry
rem less than one notch (the settings window scrolls its page by them in proportion and adds them up on a slider;
rem mod.22 and before dropped them) and those with the pointer moved since the previous one (a swipe keeps scrolling
rem the page only while the pointer stays put), and prints VERDICT lines. Unlike the other live tests, this one
rem needs your hands: no input is sent by the test, the running dock and other windows are not touched.
setlocal
cd /d "%~dp0.."
call "%~dp0vsenv.bat" || exit /b 2
if not exist build\tests\dock-render-live mkdir build\tests\dock-render-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\dock-render-live\ /Febuild\tests\dock-render-live\dock_render_live_wheel.exe tests\dock_render_live_wheel.cpp ^
 /link /Brepro /SUBSYSTEM:CONSOLE user32.lib gdi32.lib >nul || exit /b 1
build\tests\dock-render-live\dock_render_live_wheel.exe 15
exit /b %errorlevel%
