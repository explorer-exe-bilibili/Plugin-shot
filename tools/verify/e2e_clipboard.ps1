# ============================================================================
#  e2e_clipboard.ps1 —— 截图 → 剪贴板
#
#  断言：常驻宿主 + 按配置的"截屏热键"截屏后，clipboard 插件日志出现
#        "图片已写入剪贴板"，且系统剪贴板里确实有一张图片。
#
#  用法： powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify/e2e_clipboard.ps1
#  前提： clipboard 插件已加载、clipboard.main/enabled=1。
#  注意： 会覆盖你当前的剪贴板内容；运行期间不要并行跑其它 e2e。
# ============================================================================
. (Join-Path $PSScriptRoot "common.ps1")
Initialize-PmAwareness
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Write-Step "截图 → 剪贴板 e2e"
Stop-Host
Backup-Config

$enabled = Get-ConfigValue "clipboard.main/enabled"
# 配置文件可能不存在（首次运行 / 本次 e2e 前被清掉）：clipboard 的默认值就是 enabled=1。
if ($enabled -eq "<no-config>" -or $enabled -eq "<missing>") { $enabled = "1" }
Write-Host ("clipboard.main/enabled = {0}" -f $enabled)
Check "clipboard 已启用（enabled=1）" ($enabled -eq "1") ("当前值：{0}" -f $enabled)

# 先清空剪贴板，避免把上一次的图当成这次结果。
try { [System.Windows.Forms.Clipboard]::Clear() } catch { }

# 日志游标要在启动前记录：热键注册等日志发生在 warmup 期间。
$mark = Get-LogMark
$process = Start-Host -WarmupSeconds 6
Check "宿主仍在运行" (-not $process.HasExited)

$loaded = Wait-LogPattern "clipboard" 10 -SinceLine $mark
Check "clipboard 插件出现在日志里" ($null -ne $loaded) ([string]$loaded)

$hotkeySpec = Get-ConfigValue "hotkey.main/capture"
if (-not $hotkeySpec -or $hotkeySpec -eq "<missing>" -or $hotkeySpec -eq "<no-config>") {
    $hotkeySpec = Get-ConfigValue "hotkey.main/capture_hotkey"   # 旧键：迁移前的配置
}
if (-not $hotkeySpec -or $hotkeySpec -eq "<missing>" -or $hotkeySpec -eq "<no-config>") { $hotkeySpec = "Ctrl+Alt+S" }
$virtualKeys = Convert-HotkeyToVirtualKeys $hotkeySpec
if ($null -eq $virtualKeys) {
    Check "热键可解析（能注入按键）" $false ("无法解析热键文本：{0}" -f $hotkeySpec)
} else {
    Send-KeyCombo $virtualKeys
    $written = Wait-LogPattern "图片已写入剪贴板" 25 -SinceLine $mark
    Check "插件报告已写入剪贴板" ($null -ne $written) ([string]$written)
}

$image = $null
try { $image = [System.Windows.Forms.Clipboard]::GetImage() } catch { }
Check "剪贴板里有一张图片" ($null -ne $image) $(if ($image) { "{0}x{1}" -f $image.Width, $image.Height } else { "GetImage 返回空" })
if ($image) { $image.Dispose() }

Stop-Host
Restore-Config
Complete-Verification
