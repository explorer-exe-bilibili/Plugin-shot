# ============================================================================
#  smoke_cursor_capture.ps1 —— 抓屏合成鼠标光标冒烟（screen_capture + include_cursor）
#
#  断言（全部在交互桌面上真机验证）：
#    · 把光标 SetCursorPos 到主屏内两个已知点，各跑一次 `Plugin-shot.exe --capture`；
#    · 两次都 exit 0、都产生了新的截图文件；
#    · 两张截图尺寸都 = 物理虚拟桌面（多屏时是整块桌面尺寸，脚本不硬编码数字）；
#    · 两张图在"光标所在位置"附近的小窗口内逐像素比较存在差异 —— 说明 BitBlt
#      之后真的把光标合成进来了（CAPTUREBLT 自己不含光标，见 AGENTS §9 backlog #4）。
#
#  用法： powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify/smoke_cursor_capture.ps1
#  前提： 已构建（bash tools/verify/build.sh --target screen_capture）；有交互式桌面；
#         同一时间不要跑其它宿主 e2e（会抢前台焦点与 config.json）。
#
#  脚本会把 `screen_capture.main/include_cursor` 固定成 "1"（已经是 1 则不动），
#  跑完按 common.ps1 的约定恢复 config.json；原本就没有配置文件时，把测试期间
#  生成的那份连同备份一起删掉，不给下次运行留脏配置。
# ============================================================================
. (Join-Path $PSScriptRoot "common.ps1")
Initialize-PmAwareness

$configKey = "screen_capture.main/include_cursor"

# 光标比较窗口：以光标热点为中心取一个方窗。箭头光标从热点向右下方展开，
# 96px 半宽足够覆盖 150% DPI 下的系统光标（约 48px）和少量阴影。
$windowRadius = 96

# 窗口内至少要有这么多像素不同才认定"有差异"：光标是实心图形，正常远大于这个值；
# 留阈值是为了躲开 DWM 抗锯齿或极端情况下 1~2 个像素的噪声。
$minDiffPixels = 10

# ---------------------------------------------------------------------------
#  临时配置修改：只动 include_cursor 一项，其余配置原样保留。
#  为什么要手写 JSON：PowerShell 5.1 的 ConvertFrom-Json/ConvertTo-Json 对单元素
#  数组、属性顺序都有坑；这里"解析 → 改一项 → 写回"，并强制 UTF-8 无 BOM
#  （宿主用 nlohmann 解析，不赌它对 BOM 的处理）。
# ---------------------------------------------------------------------------
function Set-ConfigValue {
    param(
        [string]$Key,
        [string]$Value
    )
    $doc = $null
    if (Test-Path $script:ConfigPath) {
        $raw = Get-Content $script:ConfigPath -Raw -Encoding UTF8
        try { $doc = $raw | ConvertFrom-Json } catch { $doc = $null }
    }
    if ($null -eq $doc) {
        # 没有配置文件（或文件已损坏）：造一份只有 configItems 的最小文档，其余默认值宿主会补。
        $doc = [pscustomobject]@{ configItems = @() }
    }
    $items = New-Object System.Collections.ArrayList
    if ($null -ne $doc.configItems) {
        foreach ($item in @($doc.configItems)) {
            if ($null -ne $item) { [void]$items.Add($item) }
        }
    }
    $found = $false
    foreach ($item in $items) {
        if ($item.name -eq $Key) {
            $item.value = $Value
            $found = $true
        }
    }
    if (-not $found) {
        [void]$items.Add([pscustomobject]@{ name = $Key; value = $Value })
    }
    $doc | Add-Member -NotePropertyName configItems -NotePropertyValue @($items) -Force
    $json = ConvertTo-Json -InputObject $doc -Depth 10
    [System.IO.File]::WriteAllText($script:ConfigPath, $json, (New-Object System.Text.UTF8Encoding($false)))
}

# 截图保存目录里当前的文件名集合（跑宿主之前记一份，之后据此找"本次新产生的"）。
function Get-ScreenshotNames {
    param([string]$Directory)
    return @(Get-ChildItem -Path $Directory -Filter "shot_*.*" -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Name })
}

# 找"这次新出现"的截图。
# 为什么不用 common.ps1 的 Get-NewestSavedFile：默认文件名模板只精确到秒，两次截图
# 落在同一秒时它靠后缀 -1 区分，但 LastWriteTime 可能相同；按名字集合判断更稳。
function Get-NewScreenshot {
    param(
        [string]$Directory,
        [string[]]$KnownNames
    )
    $files = @(Get-ChildItem -Path $Directory -Filter "shot_*.*" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    foreach ($file in $files) {
        if ($KnownNames -notcontains $file.Name) { return $file }
    }
    return $null
}

# 跑一次 `--capture`：等宿主退出，并取回本次新落盘的截图。
function Invoke-Capture {
    param(
        [string]$SaveDirectory,
        [string[]]$KnownNames
    )
    $process = Start-Host @("--capture") -WarmupSeconds 0
    $finished = $process.WaitForExit(60000)
    $exitCode = if ($finished) { $process.ExitCode } else { -1 }
    if (-not $finished) { Stop-Host }
    $picture = Get-NewScreenshot $SaveDirectory $KnownNames
    return [pscustomobject]@{
        Finished = $finished
        ExitCode = $exitCode
        File     = $picture
    }
}

# 比较两张截图在 (CenterX, CenterY) 附近方窗内的像素差异个数。
# 窗口很小（约 200x200），用 System.Drawing 逐像素比较即可，不引入第三方库依赖。
function Compare-WindowDifference {
    param(
        [string]$PathA,
        [string]$PathB,
        [int]$CenterX,
        [int]$CenterY,
        [int]$Radius
    )
    Add-Type -AssemblyName System.Drawing
    $bitmapA = [System.Drawing.Bitmap]::FromFile($PathA)
    $bitmapB = [System.Drawing.Bitmap]::FromFile($PathB)
    try {
        # 窗口夹进图像范围：光标点在主屏内部，正常不会越界；夹取防的是分辨率变化。
        $left = [Math]::Max(0, $CenterX - $Radius)
        $top = [Math]::Max(0, $CenterY - $Radius)
        $right = [Math]::Min([Math]::Min($bitmapA.Width, $bitmapB.Width) - 1, $CenterX + $Radius)
        $bottom = [Math]::Min([Math]::Min($bitmapA.Height, $bitmapB.Height) - 1, $CenterY + $Radius)
        $different = 0
        for ($y = $top; $y -le $bottom; $y++) {
            for ($x = $left; $x -le $right; $x++) {
                if ($bitmapA.GetPixel($x, $y).ToArgb() -ne $bitmapB.GetPixel($x, $y).ToArgb()) {
                    $different++
                }
            }
        }
        return $different
    } finally {
        $bitmapA.Dispose()
        $bitmapB.Dispose()
    }
}

Write-Step "准备：config、光标位置、输出目录"
Stop-Host
$hadConfig = Test-Path $script:ConfigPath
Backup-Config

# 把 include_cursor 固定成 1：脚本断言的是"有光标"那条路径，不能被用户配置里的 0 干扰。
Set-ConfigValue $configKey "1"
$configValue = Get-ConfigValue $configKey
Check "include_cursor 已就绪 = 1" ($configValue -eq "1") ("当前 {0}" -f $configValue)

$virtual = Get-VirtualDesktopRect
Write-Host ("物理虚拟桌面：{0}x{1} @ ({2},{3})" -f $virtual.Width, $virtual.Height, $virtual.X, $virtual.Y)

# 光标点必须落在主显示器里：系统坐标里主屏左上角固定是 (0,0)，而虚拟桌面矩形里
# 没有被任何显示器覆盖的黑色区域上 SetCursorPos 会被系统夹回去。
$primaryWidth = [DshVerifyDpi]::GetSystemMetrics(0)   # SM_CXSCREEN
$primaryHeight = [DshVerifyDpi]::GetSystemMetrics(1)  # SM_CYSCREEN
$pointA = [pscustomobject]@{ X = [int]($primaryWidth / 4); Y = [int]($primaryHeight / 3) }
$pointB = [pscustomobject]@{ X = [int]($primaryWidth * 3 / 4); Y = [int]($primaryHeight * 2 / 3) }
Write-Host ("光标 A = ({0},{1})，B = ({2},{3})（主屏 {4}x{5}）" -f `
    $pointA.X, $pointA.Y, $pointB.X, $pointB.Y, $primaryWidth, $primaryHeight)

$saveDir = Get-SaveDirectory
$beforeNames = @(Get-ScreenshotNames $saveDir)

Write-Step "第 1 次 --capture（光标在 A 点）"
$setA = [DshVerifyDpi]::SetCursorPos($pointA.X, $pointA.Y)
Start-Sleep -Milliseconds 400
Check "光标已移到 A 点" $setA
$captureA = Invoke-Capture $saveDir $beforeNames
Check "第 1 次宿主在 60s 内退出" $captureA.Finished
Check "第 1 次退出码 = 0" ($captureA.ExitCode -eq 0) ("实际 {0}" -f $captureA.ExitCode)
Check "第 1 次产生了新截图" ($null -ne $captureA.File) $(if ($captureA.File) { $captureA.File.FullName } else { "目录：$saveDir" })

$knownNames = @($beforeNames)
if ($captureA.File) { $knownNames += $captureA.File.Name }

Write-Step "第 2 次 --capture（光标在 B 点）"
$setB = [DshVerifyDpi]::SetCursorPos($pointB.X, $pointB.Y)
Start-Sleep -Milliseconds 400
Check "光标已移到 B 点" $setB
$captureB = Invoke-Capture $saveDir $knownNames
Check "第 2 次宿主在 60s 内退出" $captureB.Finished
Check "第 2 次退出码 = 0" ($captureB.ExitCode -eq 0) ("实际 {0}" -f $captureB.ExitCode)
Check "第 2 次产生了新截图" ($null -ne $captureB.File) $(if ($captureB.File) { $captureB.File.FullName } else { "目录：$saveDir" })

if ($captureA.File -and $captureB.File) {
    $sizeA = Get-ImageSize $captureA.File.FullName
    Check "第 1 张尺寸 = 物理虚拟桌面" ($sizeA.Width -eq $virtual.Width -and $sizeA.Height -eq $virtual.Height) `
        ("{0}x{1}，期望 {2}x{3}" -f $sizeA.Width, $sizeA.Height, $virtual.Width, $virtual.Height)
    $sizeB = Get-ImageSize $captureB.File.FullName
    Check "第 2 张尺寸 = 物理虚拟桌面" ($sizeB.Width -eq $virtual.Width -and $sizeB.Height -eq $virtual.Height) `
        ("{0}x{1}，期望 {2}x{3}" -f $sizeB.Width, $sizeB.Height, $virtual.Width, $virtual.Height)

    # 系统坐标 → 截图图像坐标：虚拟桌面原点可能不是 (0,0)，甚至为负，不能直接当图像坐标用。
    $imageA = [pscustomobject]@{ X = $pointA.X - $virtual.X; Y = $pointA.Y - $virtual.Y }
    $imageB = [pscustomobject]@{ X = $pointB.X - $virtual.X; Y = $pointB.Y - $virtual.Y }

    $diffA = Compare-WindowDifference $captureA.File.FullName $captureB.File.FullName `
        $imageA.X $imageA.Y $windowRadius
    Check "两张图在光标 A 位置附近有像素差异" ($diffA -ge $minDiffPixels) `
        ("不同像素 {0}，阈值 {1}" -f $diffA, $minDiffPixels)

    $diffB = Compare-WindowDifference $captureA.File.FullName $captureB.File.FullName `
        $imageB.X $imageB.Y $windowRadius
    Check "两张图在光标 B 位置附近有像素差异" ($diffB -ge $minDiffPixels) `
        ("不同像素 {0}，阈值 {1}" -f $diffB, $minDiffPixels)
}

Write-Step "收尾：恢复 config、停宿主"
if ($hadConfig) {
    Restore-Config
} else {
    # 原本没有配置文件：测试期间生成的那份和备份都不该留下，别让下次运行读到测试值。
    if (Test-Path $script:ConfigPath) { Remove-Item $script:ConfigPath -Force }
    if (Test-Path $script:ConfigBackup) { Remove-Item $script:ConfigBackup -Force }
}
Stop-Host
Complete-Verification
