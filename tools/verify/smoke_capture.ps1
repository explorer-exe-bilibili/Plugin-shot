# ============================================================================
#  smoke_capture.ps1 —— 一次性截屏冒烟（`--capture`）
#
#  断言：宿主 exit 0、日志有 "Capture saved:"、落盘文件存在且尺寸 = 物理虚拟桌面
#        （双屏时是 3584x1600 这类整块桌面尺寸）。
#
#  用法： powershell -NoProfile -File tools/verify/smoke_capture.ps1
#  前提： 已构建（tools/verify/build.sh）；会先杀掉已有宿主进程并备份 config.json。
# ============================================================================
. (Join-Path $PSScriptRoot "common.ps1")
Initialize-PmAwareness

Write-Step "一次性截屏冒烟"
Stop-Host
Backup-Config

$virtual = Get-VirtualDesktopRect
Write-Host ("物理虚拟桌面：{0}x{1} @ ({2},{3})" -f $virtual.Width, $virtual.Height, $virtual.X, $virtual.Y)
$saveDir = Get-SaveDirectory
$before = Get-NewestSavedFile $saveDir
$beforeName = if ($before) { $before.Name } else { "" }

$process = Start-Host @("--capture") -WarmupSeconds 0
$finished = $process.WaitForExit(60000)
Check "宿主在 60s 内退出" $finished
$exitCode = if ($finished) { $process.ExitCode } else { -1 }
if (-not $finished) { Stop-Host }
Check "退出码 = 0" ($exitCode -eq 0) ("实际 {0}" -f $exitCode)

$savedLine = Wait-LogPattern "Capture saved:" 10
Check "日志有 Capture saved:" ($null -ne $savedLine) ([string]$savedLine)

$newFile = Get-NewestSavedFile $saveDir $beforeName
Check "产生了新的截图文件" ($null -ne $newFile) $(if ($newFile) { $newFile.FullName } else { "目录：$saveDir" })
if ($newFile) {
    $size = Get-ImageSize $newFile.FullName
    Check "截图尺寸 = 物理虚拟桌面" ($size.Width -eq $virtual.Width -and $size.Height -eq $virtual.Height) `
        ("{0}x{1}，期望 {2}x{3}" -f $size.Width, $size.Height, $virtual.Width, $virtual.Height)
}

Restore-Config
Stop-Host
Complete-Verification
