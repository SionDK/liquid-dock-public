@echo off
rem Run build.bat first (the test links its objects: DOCKBUILDOBJ, tests\lists.bat). Test fixtures are hidden
rem and never inject mouse/keyboard input.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
call "%~dp0lists.bat"
cd /d "%~dp0.."
for %%O in (%DOCKBUILDOBJ%) do if not exist %%O (
  echo Run build.bat before this test.
  exit /b 2
)
if not exist build\tests\dock-interaction mkdir build\tests\dock-interaction
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE ^
 /Fobuild\tests\dock-interaction\ /Febuild\tests\dock-interaction\dock_interaction_test.exe tests\dock_interaction_test.cpp ^
 %DOCKBUILDOBJ% ^
 /link /Brepro /SUBSYSTEM:CONSOLE %DOCKLIB% || exit /b 1
build\tests\dock-interaction\dock_interaction_test.exe %*
exit /b %errorlevel%
