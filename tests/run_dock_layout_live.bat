@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem What the dock's window-in-front rules see on the real desktop (tests\dock_layout_live_probe.cpp): the
rem production fullscreen rule of dock.cpp and the shell pop-up rule against the real window in front, each
rem monitor's work area (an always-shown dock's strip), GetCursorPos failing (lock screen, UAC) and the mouse
rem buttons as the dock's drag reads them. One line whenever something changes; about 60 s (pass another
rem number of seconds after --logged).
rem It only reads: no input is sent, no window is created, moved or activated, the running dock is not touched.
rem Meanwhile put things in front by hand: F11 in a browser, a video or a game fullscreen, Win+Tab, Start,
rem Alt+Tab, a maximised window, a window spanned across two monitors; Win+L and back; hold each mouse button.
rem Run build.bat first (the probe links its objects: PROBEOBJ, tests\lists.bat).
setlocal
call "%~dp0vsenv.bat" || exit /b 2
call "%~dp0lists.bat"
cd /d "%~dp0.."
for %%O in (%PROBEOBJ%) do if not exist %%O (
  echo Run build.bat before this test.
  exit /b 2
)
if not exist build\tests\dock-layout-live mkdir build\tests\dock-layout-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\dock-layout-live\ /Febuild\tests\dock-layout-live\dock_layout_live_probe.exe tests\dock_layout_live_probe.cpp ^
 %PROBEOBJ% ^
 /link /Brepro /SUBSYSTEM:CONSOLE %PROBELIB% || exit /b 1
set "SECS=%~2"
if "%SECS%"=="" set "SECS=60"
build\tests\dock-layout-live\dock_layout_live_probe.exe %SECS%
exit /b %errorlevel%
