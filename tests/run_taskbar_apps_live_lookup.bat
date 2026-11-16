@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem Which taskbar button the dock would press, read from Windows' own taskbars (tests\taskbar_apps_live_lookup.cpp):
rem every taskbar (the main one and each monitor's), the order a click on each monitor's dock looks at them in, the
rem ways each app button offers to press it, and for each running app with one window the button a click would press
rem from each monitor's dock, or why none - with the taskbar settings that change the buttons. It only reads: nothing is
rem pressed, no key or click goes out, no window is made, moved or activated. Run it with the dock running (its taskbars
rem hidden); on a desktop with two monitors it shows whether each dock finds its own monitor's taskbar first. Once
rem more with "Combine taskbar buttons: Never" shows the buttons' names then. A second or two; the mouse and keyboard may
rem be used meanwhile.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\taskbar-apps-lookup mkdir build\tests\taskbar-apps-lookup
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\taskbar-apps-lookup\ /Febuild\tests\taskbar-apps-lookup\taskbar_apps_live_lookup.exe tests\taskbar_apps_live_lookup.cpp /link /Brepro user32.lib gdi32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib dwmapi.lib advapi32.lib || exit /b 1
build\tests\taskbar-apps-lookup\taskbar_apps_live_lookup.exe
exit /b %errorlevel%
