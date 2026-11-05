@echo off
setlocal EnableExtensions

set "NAME=%~1"
if "%NAME%"=="" set "NAME=Plugin-shot"
set "EXE_NAME=%NAME%.exe"

rem 先看进程在不在，不在就直接成功退出（避免 tasklist/taskkill 报错）
rem 注意用 findstr 而不是 find：装过 git-bash / MSYS2 的机器上 PATH 里的
rem POSIX find 会遮蔽 Windows 的 find.exe，导致这里永远判定"没在运行"。
tasklist /FI "IMAGENAME eq %EXE_NAME%" /NH 2>nul | findstr /I /C:"%EXE_NAME%" >nul
if errorlevel 1 (
    echo [kill_running] %EXE_NAME% is not running.
    exit /b 0
)

echo [kill_running] Stopping %EXE_NAME% ...
taskkill /F /T /IM "%EXE_NAME%" >nul 2>&1
if errorlevel 1 (
    echo [kill_running] taskkill returned an error, continuing anyway.
) else (
    echo [kill_running] %EXE_NAME% stopped.
)

rem 无论 taskkill 结果如何都返回 0，不阻断链接
exit /b 0