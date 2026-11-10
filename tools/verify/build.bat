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
rem
rem  NOTE: DO NOT wrap the vcvars setup in a ( ) block. Inside a parenthesized
rem  block cmd.exe expands %VAR% for the whole block when it is parsed, so a
rem  variable set by the for-loop below would still expand to "" in
rem  `call "%VSPATH%\..."` -> vcvars never runs -> cl.exe cannot find <cstdint>
rem  (observed: every compile fails with C1083). Use goto-based flow instead of
rem  EnableDelayedExpansion to keep this file simple and ASCII-only.
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0..\.."

if not defined CMAKE_EXE set "CMAKE_EXE=cmake"
if exist "D:\src\cmake\bin\cmake.exe" set "CMAKE_EXE=D:\src\cmake\bin\cmake.exe"

if defined VCINSTALLDIR goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :fallback_vs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

:fallback_vs
if not defined VSPATH if exist "C:\Program Files\Microsoft Visual Studio\18\Community" set "VSPATH=C:\Program Files\Microsoft Visual Studio\18\Community"
if defined VSPATH call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:build
"%CMAKE_EXE%" --build build\Windows %*
exit /b %ERRORLEVEL%
