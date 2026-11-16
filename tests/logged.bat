@echo off
rem Runs a test script (%1, the caller) with its output saved to build\tests\<name>.log, shows that output
rem when it ends, and keeps the window open when the script was double-clicked (or started with "Run as
rem administrator"), so the result can be read and the log handed on. Exit code: the script's. %2, if given, is
rem passed on to the script after --logged (run_taskbar_apps_diag.bat dock).
setlocal
if not exist "%~dp0..\build\tests" mkdir "%~dp0..\build\tests"
set "LOGFILE=%~dp0..\build\tests\%~n1.log"
echo %~nx1: running - the output is shown when it ends and saved to build\tests\%~n1.log
call %1 --logged %2 > "%LOGFILE%" 2>&1
set "RC=%errorlevel%"
type "%LOGFILE%"
echo.
echo [%~nx1: exit code %RC% - saved to build\tests\%~n1.log]
echo %cmdcmdline% | findstr /i /c:"%~nx1" >nul && pause
exit /b %RC%
