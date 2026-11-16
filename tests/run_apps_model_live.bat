@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem The dock's app model against the real Windows (tests\apps_model_live_test.cpp), two checks:
rem   pins    - read only: the taskbar's own pin list (HKCU ...\Taskband Favorites) walked item by item as the shell
rem             reads it, against the dock's first-run import order - every pin there, each in its place. Also where
rem             the dock found each pin's name (an odd or even byte offset). A list not laid out as known is reported
rem             as not checked. Nothing is written. Worth running on every PC (PINS lines).
rem   restore - a window of the test's own, maximized then minimized, its program busy for 3 s: the dock's way brings
rem             it up, the watch ends as when the user goes on, the dock brings it up again; then its program handles
rem             every restore command it got. One: it comes back maximized; two: normal size (RESTORE lines).
rem No other program's window is touched, no key or mouse input is sent. The restore check shows a window of its own
rem for about 5 s: keep this window in front and leave the mouse and keyboard alone meanwhile. The running dock is not
rem touched (it may show the test's window as an app for a moment).
rem   run_apps_model_live.bat --logged pins      only the read-only check
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\apps-model-live mkdir build\tests\apps-model-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\apps-model-live\ /Febuild\tests\apps-model-live\apps_model_live_test.exe tests\apps_model_live_test.cpp src\common.cpp /link /Brepro user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib shlwapi.lib propsys.lib version.lib advapi32.lib dwmapi.lib windowsapp.lib || exit /b 1
set "WHAT=%~2"
if "%WHAT%"=="" set "WHAT=all"
build\tests\apps-model-live\apps_model_live_test.exe %WHAT%
exit /b %errorlevel%
