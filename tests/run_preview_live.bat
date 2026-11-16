@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem The window preview with real windows and real DWM (tests\preview_live_test.cpp). For each monitor and dock
rem edge it makes 12 windows of its own - live ones, one protected, two started minimized, two minimized after
rem they were on screen - opens the dock's preview popup for them and reads its pixels from the screen: a live
rem window shows its live contents, a minimized one the image Windows kept of it (as on the taskbar), one that
rem was never on screen the card Windows makes for it (its icon, as on the taskbar); a card's close button and
rem a middle click close two of its windows.
rem Clicks go to the popup as window messages, no mouse or keyboard input.
rem The running dock is not touched; it may show the test's ordinary windows as an app for a moment. About 4 s
rem per monitor and edge: keep this window in front and leave the mouse alone meanwhile. Pictures of the popup:
rem build\tests\preview-live\preview-m<monitor>-e<edge>.bmp
setlocal
cd /d "%~dp0.."
call "%~dp0build_preview_live.bat" || exit /b 1
build\tests\preview-live\preview_live_test.exe front build\tests\preview-live
exit /b %errorlevel%
