@echo off
rem ---------------------------------------------------------------------------
rem  Launcher for the verification scripts in this folder.
rem  Usage:  run.bat smoke_settings.ps1 [args...]
rem
rem  Why: some machines' PowerShell execution policy refuses unsigned scripts
rem  from newly created folders. We always start the child process with
rem  -ExecutionPolicy Bypass so the scripts here run without touching the
rem  machine-wide policy. Keep this file ASCII-only.
rem ---------------------------------------------------------------------------
if "%~1"=="" (
    echo Usage: run.bat ^<script.ps1^> [args...]
    exit /b 2
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0%~1" %2 %3 %4 %5 %6 %7 %8 %9
exit /b %ERRORLEVEL%
