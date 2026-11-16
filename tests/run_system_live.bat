@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" %1 & exit /b)
rem HWiNFO's Gadget key on this PC, watched while you change HWiNFO's settings (tests\system_live_hwinfo.cpp):
rem whether HWiNFO rewrites HKCU\Software\HWiNFO64\VSB in place or deletes and makes it again, how long it stays
rem quiet, and what the dock's own reader makes of it meanwhile. Only reads the key: no input is sent, no window
rem is touched, the running dock is left alone. While it runs (90 s, or the seconds given after --logged), in
rem HWiNFO: Sensors > Configure Sensors > HWiNFO Gadget - untick and tick again "Enable reporting to Gadget",
rem untick and tick a reported value, and close HWiNFO and start it again. Exit code 1: the dock's reader did
rem not come back from a read (an endless scan of a deleted key).
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\system-live mkdir build\tests\system-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\system-live\ /Febuild\tests\system-live\system_live_hwinfo.exe tests\system_live_hwinfo.cpp src\hwinfo.cpp /link /Brepro advapi32.lib || exit /b 1
build\tests\system-live\system_live_hwinfo.exe %2
exit /b %errorlevel%
