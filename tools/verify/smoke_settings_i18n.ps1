# ============================================================================
#  smoke_settings_i18n.ps1 —— 多语言冒烟：设置页按 core/language 显示对应语言
#
#  断言（每一步都备份/恢复 config.json）：
#    1. 写 core/language=en-US → --settings 启动 → 设置窗口标题是英文（*Settings*）、
#       能截到画面（build\verify\settings_i18n_en.png）、日志锚点「界面语言 = en-US」；
#    2. 切回 core/language=zh-CN → 设置窗口标题是中文（*设置*）、截图
#       （build\verify\settings_i18n_zh.png）、日志锚点「界面语言 = zh-CN」；
#    3. 两次运行近期日志都没有 [error。
#
#  语言锚点来自宿主启动日志（src/hostRuntime.cpp）：语言 tag 保持 BCP-47 写法，
#  日志正文仍为中文（日志不翻译，见 document/I18N.md §4）。
#
#  用法： powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify/smoke_settings_i18n.ps1
# ============================================================================
. (Join-Path $PSScriptRoot "common.ps1")
Initialize-PmAwareness

Write-Step "多语言设置页冒烟"
Stop-Host
Backup-Config

# ---- 1. 英文界面 ----------------------------------------------------------
Set-ConfigValue "core/language" "en-US"
$processEn = Start-Host @("--settings") -WarmupSeconds 6
Check "宿主进程仍在运行（en-US）" (-not $processEn.HasExited)

$windowEn = Find-SettingsWindow 20
Check "找到设置窗口（英文标题 *Settings*）" ($windowEn -ne [IntPtr]::Zero) ([string]$windowEn)
if ($windowEn -ne [IntPtr]::Zero) {
    Focus-Window $windowEn
    $shotEn = Join-Path $script:OutDir "settings_i18n_en.png"
    Save-ScreenShot $shotEn | Out-Null
    Check "截到英文设置页画面" (Test-Path $shotEn) $shotEn
}

$anchorEn = Wait-LogPattern "界面语言 = en-US" 10
Check "日志锚点：界面语言 = en-US" ($null -ne $anchorEn) ([string]$anchorEn)
$errorEn = Get-LogTail 80 | Select-String -Pattern "\[error"
Check "英文界面近期日志无 error" ($null -eq $errorEn) ([string]$errorEn)
Stop-Host

# ---- 2. 切回中文界面 ------------------------------------------------------
Set-ConfigValue "core/language" "zh-CN"
$processZh = Start-Host @("--settings") -WarmupSeconds 6
Check "宿主进程仍在运行（zh-CN）" (-not $processZh.HasExited)

$windowZh = Find-SettingsWindow 20
Check "找到设置窗口（中文标题 *设置*）" ($windowZh -ne [IntPtr]::Zero) ([string]$windowZh)
if ($windowZh -ne [IntPtr]::Zero) {
    Focus-Window $windowZh
    $shotZh = Join-Path $script:OutDir "settings_i18n_zh.png"
    Save-ScreenShot $shotZh | Out-Null
    Check "截到中文设置页画面" (Test-Path $shotZh) $shotZh
}

$anchorZh = Wait-LogPattern "界面语言 = zh-CN" 10
Check "日志锚点：界面语言 = zh-CN" ($null -ne $anchorZh) ([string]$anchorZh)
$errorZh = Get-LogTail 80 | Select-String -Pattern "\[error"
Check "中文界面近期日志无 error" ($null -eq $errorZh) ([string]$errorZh)
Stop-Host

Restore-Config
Complete-Verification
