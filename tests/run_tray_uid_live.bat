@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem How Windows keeps its tray icon settings (HKCU\Control Panel\NotifyIconSettings) for a program whose icon id
rem changes from run to run, as MSI Afterburner's does (tests\tray_uid_live.cpp): one icon of the test's own is added,
rem removed, and added again with another id from another window; then two at once. It prints the test program's
rem entries whenever they change and a VERDICT line. Windows 11 25H2 (2026-10-28): one entry for the
rem program, keeping the first id; later ids are shown by it. The dock (mod.23 on) follows that entry, and its dock.log
rem line for the test's icon says "entry ... of its program, made for uid ...".
rem It only reads the registry; the icons are its own; no other program's window or icon is touched; no input is
rem sent. About 30 s; the mouse and keyboard may be used meanwhile. Windows keeps the test program's entry afterwards,
rem as for any program that once showed an icon.
setlocal
cd /d "%~dp0.."
call "%~dp0vsenv.bat" || exit /b 2
if not exist build\tests\tray-uid-live mkdir build\tests\tray-uid-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\tray-uid-live\ /Febuild\tests\tray-uid-live\tray_uid_live.exe tests\tray_uid_live.cpp /link /Brepro user32.lib shell32.lib advapi32.lib || exit /b 1
build\tests\tray-uid-live\tray_uid_live.exe
exit /b %errorlevel%
