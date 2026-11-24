@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem Builds the dock (build.bat) and runs every test: regressions (the window preview's among them), the dock's
rem interaction matrix and the settings options. Exit code 0 = all passed. About 6-7 minutes.
rem The running dock is not touched (the build goes to build\, the tests use their own windows).
rem .\build.bat, not build.bat: where NoDefaultCurrentDirectoryInExePath is set (some shells set it), a name without a
rem path is not looked for in the current folder.
setlocal
cd /d "%~dp0.."
call .\build.bat || (echo BUILD FAILED & exit /b 1)
call tests\run_regressions.bat || (echo REGRESSION TESTS FAILED & exit /b 1)
call tests\run_dock_interactions.bat || (echo DOCK INTERACTION TESTS FAILED & exit /b 1)
call tests\run_settings_options.bat || (echo SETTINGS OPTIONS TEST FAILED & exit /b 1)
echo ALL TESTS PASSED
exit /b 0
