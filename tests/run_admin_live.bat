@echo off
if /i not "%~1"=="--logged" (call "%~dp0logged.bat" "%~f0" & exit /b)
rem The installed elevated helper's window service, live (tests\admin_live_pipe_test.cpp): from outside, as any
rem program of the user, it checks that the helper in %ProgramFiles%\LiquidDock has the window service (and says
rem how old it is next to the copy the dock ships), that it is the one answering on the pipe, that it refuses
rem programs other than Liquid Dock, that the pipe's name cannot be taken next to it, that a low-integrity program
rem can neither send on the pipe nor (unless the installed helper's file still holds the older pipe label, kept
rem while it has the window service) open it at all, that the helper runs
rem elevated (it cannot be ended or written to from here), that its sensor memory is read-only from here, and how
rem long one request takes.
rem Needs the dock running with its option for administrators' app windows on (settings, system): the helper
rem running. It sends only Hello requests - no window is touched, no mouse or keyboard input - each one a
rem "refused a request" line in %ProgramFiles%\LiquidDock\sensors.log. A click on an administrator's app in the
rem dock may wait up to 1.5 s while it runs. About 5 s, of them about 1 s with every processor busy (the test's
rem own threads, to see whether the helper - started below normal priority - still answers in time); the mouse
rem and keyboard may be used meanwhile.
setlocal
call "%~dp0vsenv.bat" || exit /b 2
cd /d "%~dp0.."
if not exist build\tests\admin-live mkdir build\tests\admin-live
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /Brepro /DUNICODE /D_UNICODE /Fobuild\tests\admin-live\ /Febuild\tests\admin-live\admin_live_pipe_test.exe tests\admin_live_pipe_test.cpp /link /Brepro user32.lib advapi32.lib shell32.lib ole32.lib version.lib || exit /b 1
build\tests\admin-live\admin_live_pipe_test.exe
exit /b %errorlevel%
