@echo off
rem The live checks to run on a PC (for example one with several monitors, or one on a newer Windows build)
rem with Liquid Dock mod.23 installed and running, after the elevated helper was updated once
rem (as administrator: %LOCALAPPDATA%\Programs\LiquidDock\LiquidDockSensors.exe --install).
rem Everything goes to build\desktop-checks\<computer name>\ - zip that folder and hand it on.
rem   Part 1 (about 1 minute, the mouse and keyboard may be used): read-only probes and the test's own tray icons -
rem          the taskbar's app buttons per monitor, the taskbar's pins, tray icon ids (MSI Afterburner's case),
rem          FluentFlyout and its widget, the elevated helper.
rem   Part 2 (about 2 minutes, hands off, keep this window in front): the taskbar's app buttons, Start, the window
rem          preview on every monitor and edge, a busy app's restore. Only the tests' own windows are closed.
rem   Then dock.log, sensors.log and settings.ini are copied next to the results.
rem Needs Visual Studio 2022 Build Tools (the tests are built here), like build.bat. "run_desktop_checks.bat part1": part 1 only.
setlocal
cd /d "%~dp0.."
set "OUT=build\desktop-checks\%COMPUTERNAME%"
if not exist "%OUT%" mkdir "%OUT%"
echo Results go to %OUT%
echo.
echo ---- part 1: read only (the mouse and keyboard may be used)
call :run run_taskbar_apps_live_lookup ""
call :run run_apps_model_live pins
call :run run_tray_uid_live ""
call :run run_fluent_live 0
call :run run_admin_live ""
echo.
if /i "%~1"=="part1" goto :logs
echo ---- part 2: hands off from now (about 2 minutes) - keep this window in front
timeout /t 10
call :run run_taskbar_apps_live ""
call :run run_start_menu_live ""
call :run run_preview_live ""
call :run run_apps_model_live restore
echo.
:logs
copy /y "%LOCALAPPDATA%\Programs\LiquidDock\data\dock.log" "%OUT%\dock.log" >nul
copy /y "%LOCALAPPDATA%\Programs\LiquidDock\data\settings.ini" "%OUT%\settings.ini" >nul
copy /y "%ProgramFiles%\LiquidDock\sensors.log" "%OUT%\sensors.log" >nul 2>nul
echo Done. Zip %CD%\%OUT% and hand it on.
echo %cmdcmdline% | findstr /i /c:"%~nx0" >nul && pause
exit /b 0

:run
echo   %1 %~2 ...
call "%~dp0%1.bat" --logged %~2 > "%OUT%\%1_%~2.txt" 2>&1
echo     exit code %errorlevel%
exit /b 0
