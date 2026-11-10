# ============================================================================
#  e2e_hotkey_capture.ps1 —— 常驻模式 + 全局热键 + 采集链路
#
#  断言：常驻宿主启动 → 按配置里登记的"截屏热键"注入一次 → 日志出现
#        "CapturePipeline: 已保存" → 产生新的截图文件。
#
#  用例覆盖：hotkey 插件（注册/触发）、capture 流水线（worker 线程）、image_saver。
#  热键从 config.json 的 hotkey.main/capture_hotkey 读，默认 Ctrl+Alt+S；
#  解析不了的写法会跳过按键（只验证注册日志），不会误判。
#
#  用法： powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify/e2e_hotkey_capture.ps1
#  注意： 会占用键盘 1~2 秒；运行期间不要并行跑其它 e2e。
# ============================================================================
. (Join-Path $PSScriptRoot "common.ps1")
Initialize-PmAwareness

Write-Step "常驻模式 + 热键截屏 e2e"
Stop-Host
Backup-Config
Check "插件目录存在" (Test-Path $script:PluginDir) $script:PluginDir

$saveDir = Get-SaveDirectory
$before = Get-NewestSavedFile $saveDir
$beforeName = if ($before) { $before.Name } else { "" }

# 日志游标要在启动前记录：热键注册等日志发生在 warmup 期间。
$mark = Get-LogMark
$process = Start-Host -WarmupSeconds 6
Check "宿主仍在运行" (-not $process.HasExited)

$hotkeySpec = Get-ConfigValue "hotkey.main/capture_hotkey"
if (-not $hotkeySpec -or $hotkeySpec -eq "<missing>" -or $hotkeySpec -eq "<no-config>") {
    $hotkeySpec = "Ctrl+Alt+S"
}
Write-Host ("截屏热键：{0}" -f $hotkeySpec)

$registered = Wait-LogPattern "已注册截屏热键" 10 -SinceLine $mark
Check "热键已注册" ($null -ne $registered) ([string]$registered)

$virtualKeys = Convert-HotkeyToVirtualKeys $hotkeySpec
if ($null -eq $virtualKeys) {
    Check "热键可解析（能注入按键）" $false ("无法解析热键文本：{0}" -f $hotkeySpec)
} else {
    Send-KeyCombo $virtualKeys
    $savedLine = Wait-LogPattern "CapturePipeline: 已保存" 25 -SinceLine $mark
    Check "热键触发采集并落盘" ($null -ne $savedLine) ([string]$savedLine)
}

$newFile = Get-NewestSavedFile $saveDir $beforeName
Check "产生新的截图文件" ($null -ne $newFile) $(if ($newFile) { $newFile.Name } else { "目录：$saveDir" })
if ($newFile) {
    $size = Get-ImageSize $newFile.FullName
    Check "截图尺寸非零" ($size.Width -gt 0 -and $size.Height -gt 0) ("{0}x{1}" -f $size.Width, $size.Height)
}

Stop-Host
Restore-Config
Complete-Verification
