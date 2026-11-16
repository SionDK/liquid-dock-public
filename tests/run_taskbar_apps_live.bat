@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem The app buttons on Windows' own taskbar, for real (tests\taskbar_apps_live_test.cpp): first it only reads -
rem every app button, and which running apps' windows the dock would find a button for - then it presses the
rem button of a window of its own three times, as a click on the dock does: the window comes to the front, goes
rem down, comes back. Where Windows' app buttons offer no press that works (a 25H2 build: no Invoke or
rem Toggle), it presses nothing: it checks the window is left as it was (NO WAY), skips the other presses and still
rem runs the abandoned-press check. No other program's window is pressed or moved. Like the dock, it takes the
rem right to change the front window with a key no program acts on (0xE8). About 10 s: keep this window in front
rem and leave the mouse and keyboard alone meanwhile. Run it with the dock running (its taskbar hidden), as the
rem dock sees it. tests\run_taskbar_apps_diag.bat measures what a press does in more detail.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\taskbar-apps mkdir build\tests\taskbar-apps
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\taskbar-apps\ /Febuild\tests\taskbar-apps\taskbar_apps_live_test.exe tests\taskbar_apps_live_test.cpp /link /Brepro user32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib dwmapi.lib gdi32.lib || exit /b 1
build\tests\taskbar-apps\taskbar_apps_live_test.exe front
exit /b %errorlevel%
