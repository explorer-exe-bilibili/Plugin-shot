# ============================================================================
#  smoke_settings.ps1 —— 设置页冒烟
#
#  断言：`--settings` 能启动、出现 GLFW 设置窗口、窗口可见、日志有"设置窗口已显示"、
#        能截到一张窗口截图（PNG 落 build\verify\settings_window.png）。
#
#  用法： powershell -NoProfile -File tools/verify/smoke_settings.ps1
# ============================================================================
. (Join-Path $PSScriptRoot "common.ps1")
Initialize-PmAwareness

Write-Step "设置页冒烟"
Stop-Host
Backup-Config

$process = Start-Host @("--settings") -WarmupSeconds 6
Check "宿主进程仍在运行" (-not $process.HasExited)

$window = Find-SettingsWindow 20
Check "找到设置窗口（GLFW30）" ($window -ne [IntPtr]::Zero) ([string]$window)
if ($window -ne [IntPtr]::Zero) {
    Focus-Window $window
    $shot = Join-Path $script:OutDir "settings_window.png"
    Save-ScreenShot $shot | Out-Null
    Check "截到设置窗口画面" (Test-Path $shot) $shot
}

$shownLine = Wait-LogPattern "设置窗口已显示" 10
Check "日志有「设置窗口已显示」" ($null -ne $shownLine) ([string]$shownLine)

$errorLine = Get-LogTail 80 | Select-String -Pattern "\[error"
Check "近期日志无 error" ($null -eq $errorLine) ([string]$errorLine)

Stop-Host
Restore-Config
Complete-Verification
