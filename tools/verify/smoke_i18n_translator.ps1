# ============================================================================
#  smoke_i18n_translator.ps1 —— 外部语言包插件（translator.main）端到端冒烟
#
#  断言（每一步都备份/恢复 config.json、bin\files\lang\、用户语言包目录）：
#    1. 官方包（bin\files\lang\ja-JP.json，priority 200）与用户包
#       （%LOCALAPPDATA%\screen-shot\langs\ja-JP.json，priority 300）都被加载，
#       日志有「语言包已加载」；
#    2. 坏包（用户目录里的 broken.json，非法 JSON）只 WARN 跳过，近期日志无 [error；
#       文件名与 @meta.tag 不一致的包（de-DE.json 里写 "@meta":"fr-FR"）只 WARN，
#       并按 @meta.tag 加载（同时验证 @meta 支持字符串形式）；
#    3. @language/ja-JP 声明进入语言目录（日志「语言目录变化」），i18n_list 重建并
#       发布 lists.changed；
#    4. 托盘菜单 label 是包里的译文：同一个 key 用户包胜出（設定（ユーザー）），
#       官方包独有的 key 也生效（撮影（公式）），官方包被覆盖的同 key 文本不出现；
#    5. translator.main/enabled=0 + core/language=en-US 再启动：不再加载语言包，
#       回退英文内置文案（Open settings）；
#    6. 结束恢复所有状态（config、官方包目录、用户包目录）。
#
#  为什么用托盘日志断言包文本：tray 在 lists.changed 时重建菜单并把渲染结果
#  （[label→event]）写进日志，这是"包文本真的到了消费者手里"最直接的证据，不用截图/点菜单。
#
#  用法： powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify/smoke_i18n_translator.ps1
# ============================================================================
. (Join-Path $PSScriptRoot "common.ps1")
Initialize-PmAwareness

$script:LangTag = "ja-JP"
$script:OfficialDir = Join-Path $script:RepoRoot "build\Windows\bin\files\lang"
$script:UserDir = Join-Path $env:LOCALAPPDATA "screen-shot\langs"
$script:BackupRoot = Join-Path $script:OutDir "i18n_translator_backup"
$script:OfficialBackup = Join-Path $script:BackupRoot "official"
$script:UserBackup = Join-Path $script:BackupRoot "user"
$script:OfficialAbsent = Join-Path $script:BackupRoot "official.absent"
$script:UserAbsent = Join-Path $script:BackupRoot "user.absent"
$script:BackedUp = $false

# ---- 临时语言包 -----------------------------------------------------------
# 两个包都声明 ja-JP 且都写 settings.open（用户包应胜出）；capture.request 只有
# 官方包提供，用来证明"合并是逐 key 的"，而不是简单地把官方包整包丢掉。
$officialPack = @'
{
  "@meta": { "tag": "ja-JP", "name": "日本語（公式）", "fallback": "en-US" },
  "texts": {
    "core": {
      "settings.open": "設定（公式）",
      "capture.request": "撮影（公式）"
    }
  }
}
'@

$userPack = @'
{
  "@meta": { "tag": "ja-JP", "name": "日本語", "fallback": "en-US" },
  "texts": {
    "core": {
      "settings.open": "設定（ユーザー）"
    }
  }
}
'@

# 故意截断的 JSON：translator 必须只 WARN 跳过，不能让插件加载失败。
$brokenPack = @'
{ "@meta": { "tag": "xx-YY" }, "texts": {
'@

# 文件名与 @meta.tag 不一致 + @meta 用字符串形式：必须 WARN 并以 @meta.tag 为准
# （放进语言目录的 fr-FR，而不是文件名暗示的 de-DE）。
$mismatchPack = @'
{
  "@meta": "fr-FR",
  "texts": {
    "core": { "app.quit": "Quitter" }
  }
}
'@

function Write-Utf8File([string]$Path, [string]$Text) {
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $utf8)
}

function Backup-Directory([string]$Path, [string]$BackupPath, [string]$AbsentMarker) {
    Remove-Item $AbsentMarker -Force -ErrorAction SilentlyContinue
    Remove-Item $BackupPath -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $Path) {
        New-Item -ItemType Directory -Force (Split-Path $BackupPath) | Out-Null
        Copy-Item $Path $BackupPath -Recurse -Force
    } else {
        New-Item -ItemType File -Force $AbsentMarker | Out-Null
    }
}

function Restore-Directory([string]$Path, [string]$BackupPath, [string]$AbsentMarker) {
    if (Test-Path $AbsentMarker) {
        Remove-Item $Path -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item $AbsentMarker -Force -ErrorAction SilentlyContinue
        return
    }
    if (Test-Path $BackupPath) {
        Remove-Item $Path -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
        Copy-Item $BackupPath $Path -Recurse -Force
    }
}

# 取"mark 之后新出现的日志行"，断言只看本轮运行的输出，不吃上一轮的旧日志。
function Get-FreshLogLines([int]$SinceLine) {
    $lines = Get-LogLines
    if ($lines.Count -le $SinceLine) {
        return @()
    }
    return @($lines[$SinceLine..($lines.Count - 1)])
}

try {
    Stop-Host
    Backup-Config
    Backup-Directory $script:OfficialDir $script:OfficialBackup $script:OfficialAbsent
    Backup-Directory $script:UserDir $script:UserBackup $script:UserAbsent
    $script:BackedUp = $true

    # ---- 1. 启用 translator：官方包 + 用户包 + 坏包 -----------------------
    Write-Step "translator 开启：官方包/用户包/坏包"
    New-Item -ItemType Directory -Force $script:OfficialDir | Out-Null
    New-Item -ItemType Directory -Force $script:UserDir | Out-Null
    Write-Utf8File (Join-Path $script:OfficialDir "$script:LangTag.json") $officialPack
    Write-Utf8File (Join-Path $script:UserDir "$script:LangTag.json") $userPack
    Write-Utf8File (Join-Path $script:UserDir "broken.json") $brokenPack
    Write-Utf8File (Join-Path $script:UserDir "de-DE.json") $mismatchPack
    Set-ConfigValue "translator.main/enabled" "1"
    Set-ConfigValue "core/language" $script:LangTag

    $mark = Get-LogMark
    $process = Start-Host @("--settings") -WarmupSeconds 8
    try {
        Check "宿主进程仍在运行（translator 开）" (-not $process.HasExited)

        $window = Find-SettingsWindow 20
        Check "找到设置窗口（--settings 已生效）" ($window -ne [IntPtr]::Zero) ([string]$window)

        $loaded = Wait-LogPattern "语言包已加载" 20 $mark
        Check "日志：语言包已加载" ($null -ne $loaded) ([string]$loaded)

        $loadLines = @(Get-FreshLogLines $mark | Select-String -Pattern "语言包已加载" -SimpleMatch)
        Check "官方包 + 用户包都加载（至少 2 条）" ($loadLines.Count -ge 2) ("命中 " + $loadLines.Count + " 条")

        $bad = Wait-LogPattern "语言包不是合法 JSON 对象" 15 $mark
        Check "坏包只 WARN 跳过（不是合法 JSON 对象）" ($null -ne $bad) ([string]$bad)

        # 文件名 de-DE.json 里写 "@meta":"fr-FR"：字符串形式的 @meta 可用，且以 @meta 为准。
        $mismatch = Wait-LogPattern "与 @meta.tag fr-FR 不一致" 15 $mark
        Check "文件名与 @meta.tag 不一致时 WARN 并以 @meta 为准" ($null -ne $mismatch) ([string]$mismatch)

        $metaString = Wait-LogPattern "语言 fr-FR" 15 $mark
        Check "@meta 字符串形式可用（包按 fr-FR 加载）" ($null -ne $metaString) ([string]$metaString)

        $catalog = Wait-LogPattern "语言目录变化" 20 $mark
        Check "@language/ja-JP 声明进入语言目录" ($null -ne $catalog) ([string]$catalog)

        $i18nChanged = Wait-LogPattern "发布 lists.changed: list=i18n_list" 20 $mark
        Check "i18n_list 重建并发布 lists.changed" ($null -ne $i18nChanged) ([string]$i18nChanged)

        # 托盘菜单 label 走 display(owner,label) = i18n_list[(owner,label)]，是最直接的消费者证据。
        $userText = Wait-LogPattern "[設定（ユーザー）→settings.open]" 20 $mark
        Check "托盘 label = 用户包文本（同 key 用户包胜出）" ($null -ne $userText) ([string]$userText)

        $officialText = Wait-LogPattern "[撮影（公式）→capture.request]" 20 $mark
        Check "托盘 label = 官方包文本（未被覆盖的 key 生效）" ($null -ne $officialText) ([string]$officialText)

        $fresh = Get-FreshLogLines $mark
        $officialSettings = @($fresh | Select-String -Pattern "[設定（公式）→settings.open]" -SimpleMatch)
        Check "官方包的 settings.open 已被用户包覆盖（日志里不出现）" ($officialSettings.Count -eq 0) (($officialSettings | ForEach-Object { $_.Line }) -join " | ")

        $errors = @($fresh | Select-String -Pattern "\[error")
        Check "translator 开：近期日志无 error" ($errors.Count -eq 0) (($errors | ForEach-Object { $_.Line }) -join " | ")
    } finally {
        Stop-Host
    }

    # ---- 2. 关闭 translator：回退内置英文文案 ----------------------------
    Write-Step "translator 关闭：回退内置文案"
    # 语言偏好设回 en-US：配置文件里留着 ja-JP 时，关掉插件后它会因为目录里没有
    # 这个标签而落到 auto/系统语言，结果取决于机器，脚本没法定死断言。
    Set-ConfigValue "translator.main/enabled" "0"
    Set-ConfigValue "core/language" "en-US"

    $mark2 = Get-LogMark
    $process2 = Start-Host @("--settings") -WarmupSeconds 7
    try {
        Check "宿主进程仍在运行（translator 关）" (-not $process2.HasExited)

        $anchor = Wait-LogPattern "界面语言 = en-US" 10 $mark2
        Check "日志：界面语言 = en-US" ($null -ne $anchor) ([string]$anchor)

        $trayEn = Wait-LogPattern "[Open settings→settings.open]" 20 $mark2
        Check "托盘 label 回退英文内置文案（Open settings）" ($null -ne $trayEn) ([string]$trayEn)

        $fresh2 = Get-FreshLogLines $mark2
        $loads2 = @($fresh2 | Select-String -Pattern "语言包已加载" -SimpleMatch)
        Check "关闭后不再加载语言包" ($loads2.Count -eq 0) ("命中 " + $loads2.Count + " 条")

        $errors2 = @($fresh2 | Select-String -Pattern "\[error")
        Check "translator 关：近期日志无 error" ($errors2.Count -eq 0) (($errors2 | ForEach-Object { $_.Line }) -join " | ")
    } finally {
        Stop-Host
    }
} finally {
    # 无论断言成败都恢复现场：config、官方包目录、用户包目录三处都要回到运行前。
    Stop-Host
    if ($script:BackedUp) {
        Restore-Config
        Restore-Directory $script:OfficialDir $script:OfficialBackup $script:OfficialAbsent
        Restore-Directory $script:UserDir $script:UserBackup $script:UserAbsent
        Remove-Item $script:BackupRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Step "临时语言包已清理"
Check "官方目录里没有残留的临时包" (-not (Test-Path (Join-Path $script:OfficialDir "$script:LangTag.json")))
Check "用户目录里没有残留的坏包/错名包" (-not (Test-Path (Join-Path $script:UserDir "broken.json")) -and -not (Test-Path (Join-Path $script:UserDir "de-DE.json")))

Complete-Verification
