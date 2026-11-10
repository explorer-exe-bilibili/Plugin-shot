@echo off
rem ---------------------------------------------------------------------------
rem  Host-only helper: configure the MSVC environment then run `cmake --build`.
rem  Usage:  build.bat [--target <name>] [other cmake --build arguments]
rem
rem  Toolchain search order (first hit wins):
rem    1. %CMAKE_EXE% environment variable
rem    2. D:\src\cmake\bin\cmake.exe (original developer machine)
rem    3. cmake from PATH
rem  Visual Studio is located through vswhere.exe, with a hard-coded fallback.
rem  Keep this file ASCII-only: cmd.exe mangles UTF-8 batch files.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0..\.."

if not defined CMAKE_EXE set "CMAKE_EXE=cmake"
if exist "D:\src\cmake\bin\cmake.exe" set "CMAKE_EXE=D:\src\cmake\bin\cmake.exe"

if not defined VCINSTALLDIR (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "%VSWHERE%" (
        for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    )
    if not defined VSPATH if exist "C:\Program Files\Microsoft Visual Studio\18\Community" set "VSPATH=C:\Program Files\Microsoft Visual Studio\18\Community"
    if defined VSPATH call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)

"%CMAKE_EXE%" --build build\Windows %*
exit /b %ERRORLEVEL%
