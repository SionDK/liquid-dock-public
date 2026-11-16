@echo off
rem Sets up the Visual Studio C++ tools (x64) for the calling script: Build Tools 2022 at its usual
rem place, else any Visual Studio with the C++ tools that vswhere finds. Called without setlocal on
rem purpose, so the settings stay in the caller's environment. Exit code 2: no C++ tools found.
if defined VCToolsInstallDir exit /b 0
set "VSDEV=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSDEV%" if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDEV=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSDEV%" (
  echo Visual Studio C++ tools not found. Install "Build Tools for Visual Studio 2022" with "Desktop development with C++".
  exit /b 2
)
call "%VSDEV%" >nul || exit /b 2
exit /b 0
