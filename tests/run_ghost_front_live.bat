@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem A closed Start or Search keeping the front after the monitors come back on, live (tests\ghost_front_live_test.cpp):
rem turns the monitors off, wakes them with a one-pixel mouse move, writes down what holds the front, then brings a
rem test window of another process up the dock's way (no Esc, no click) and says whether Windows let it in front.
rem About 20 s a run; a number after --logged runs it that many times. Hands off the mouse and keyboard meanwhile
rem (a move wakes the monitors early, a click takes the front), the console in front at the start.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\ghost-front mkdir build\tests\ghost-front
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\ghost-front\ /Febuild\tests\ghost-front\ghost_front_live_test.exe tests\ghost_front_live_test.cpp /link /Brepro user32.lib dwmapi.lib || exit /b 1
build\tests\ghost-front\ghost_front_live_test.exe %2
exit /b %errorlevel%
