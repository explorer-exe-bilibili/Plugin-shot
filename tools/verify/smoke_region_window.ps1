# ============================================================================
#  smoke_region_window.ps1 —— 区域框选：悬停窗口吸附 / 单击截窗口 / 拖动选区
#
#  断言：
#    1. 覆盖层能显示（日志"已截取覆盖层背景"）；
#    2. 鼠标停在目标窗口上时能看到选框，且边框/折角用主题色 #4C9AFF
#       （按截图像素抽检，避免只靠肉眼）；
#    3. 单击（位移 ≤4px、按住 ≤400ms）截出的图尺寸 = 目标窗口的 DWM 扩展边界；
#    4. 拖动时按手工选区截，尺寸 ≈ 拖动矩形（不做窗口吸附）。
#
#  做法：开一个 cmd 窗口钉在 (400,300) 900x600 当"目标窗口"，用配置里的区域截屏
#  热键触发覆盖层，再用 SetCursorPos / mouse_event 走真实交互路径。
#
#  用法： powershell -NoProfile -ExecutionPolicy Bypass -File tools/verify/smoke_region_window.ps1
#  注意： 会占用鼠标/键盘约 1~2 分钟；运行期间不要并行跑其它 e2e。
#         会临时改 hotkey.main/region_hotkey，结束时恢复 config.json。
# ============================================================================
. (Join-Path $PSScriptRoot "common.ps1")
Initialize-PmAwareness

if (-not ("RegionVerifyWin" -as [type])) {
    Add-Type -Namespace RegionVerify -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
[DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
'@
}

# 目标窗口：cmd 的窗口是经典 Win32 顶层窗口，类名固定、尺寸可控。
$script:TargetWidth = 900
$script:TargetHeight = 600
$script:TargetLeft = 400
$script:TargetTop = 300

function Get-ExtendedRect([IntPtr]$Hwnd) {
    $rect = New-Object RegionVerify.Win+RECT
    $hr = [RegionVerify.Win]::DwmGetWindowAttribute($Hwnd, 9, [ref]$rect, 16)
    if ($hr -ne 0) { [RegionVerify.Win]::GetWindowRect($Hwnd, [ref]$rect) | Out-Null }
    return $rect
}

function Click-At([int]$X, [int]$Y) {
    [RegionVerify.Win]::SetCursorPos($X, $Y) | Out-Null
    Start-Sleep -Milliseconds 200
    [RegionVerify.Win]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
    Start-Sleep -Milliseconds 120
    [RegionVerify.Win]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
}

# 在截图上沿目标窗口顶边抽检主题色像素：覆盖层的边框/折角画在选框边缘，
# 只靠"截图成功"断言不了主题色是否真的生效（document/SETTINGS.md §6）。
function Test-ThemeColorNearRect([string]$Path, $Rect) {
    Add-Type -AssemblyName System.Drawing
    $image = [System.Drawing.Image]::FromFile($Path)
    try {
        $expectedR = 0x4C
        $expectedG = 0x9A
        $expectedB = 0xFF
        $tolerance = 60
        $matches = 0
        $left = [Math]::Max(0, $Rect.Left)
        $right = [Math]::Min($image.Width - 1, $Rect.Right)
        for ($x = $left + 16; $x -le $right - 16; $x += 24) {
            for ($offset = -3; $offset -le 3; $offset++) {
                $y = $Rect.Top + $offset
                if ($y -lt 0 -or $y -ge $image.Height) { continue }
                $color = $image.GetPixel($x, $y)
                if ([Math]::Abs($color.R - $expectedR) -le $tolerance -and
                    [Math]::Abs($color.G - $expectedG) -le $tolerance -and
                    [Math]::Abs($color.B - $expectedB) -le $tolerance) {
                    $matches++
                }
            }
        }
        return $matches -ge 8
    } finally {
        $image.Dispose()
    }
}

Write-Step "区域框选：悬停吸附 / 单击截窗口 / 拖动选区"
Stop-Host
Backup-Config
Set-ConfigValue "hotkey.main/region_hotkey" "Ctrl+Alt+A"

# 控制台窗口现在通常由 Windows Terminal 承载（类名不再是 ConsoleWindowClass），
# 所以不按类名找：给 cmd 起唯一标题，再按标题枚举顶层窗口。
function Find-WindowByTitle([string]$TitleLike, [int]$TimeoutSeconds = 15) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $script:foundTitleWindow = [IntPtr]::Zero
        [DshVerifyWindow]::EnumWindows({
            param($hwnd, $lparam)
            if (-not [DshVerifyWindow]::IsWindowVisible($hwnd)) { return $true }
            $title = New-Object System.Text.StringBuilder 256
            [DshVerifyWindow]::GetWindowTextW($hwnd, $title, 256) | Out-Null
            if ($title.ToString() -like $TitleLike) { $script:foundTitleWindow = $hwnd }
            return $true
        }, [IntPtr]::Zero) | Out-Null
        if ($script:foundTitleWindow -ne [IntPtr]::Zero) { return $script:foundTitleWindow }
        Start-Sleep -Milliseconds 250
    }
    return [IntPtr]::Zero
}

# 用 /k 而不是 "/c ... && timeout"：后者在无交互控制台的场景下 timeout 会立刻失败、
# 窗口随即关闭，标题就找不到了。/k 让窗口一直留着，结束时由脚本杀掉进程。
$windowTag = "RegionTarget_" + [Guid]::NewGuid().ToString("N").Substring(0, 8)
$target = Start-Process cmd.exe -ArgumentList "/k title $windowTag" -PassThru
$targetHandle = Find-WindowByTitle "$windowTag*" 15
Check "目标窗口已创建" ($targetHandle -ne [IntPtr]::Zero) ("title=$windowTag hwnd=" + $targetHandle)
# 先置顶 + 激活，保证鼠标落点处它的 Z 序最靠前；再取消置顶，让后面弹出的覆盖层
# （自己也是 topmost）能盖在它上面，否则截出来的覆盖层会被这个窗口挡住。
[RegionVerify.Win]::SetWindowPos($targetHandle, [IntPtr](-1),
    $script:TargetLeft, $script:TargetTop, $script:TargetWidth, $script:TargetHeight, 0x0040) | Out-Null
[RegionVerify.Win]::SetForegroundWindow($targetHandle) | Out-Null
Start-Sleep -Milliseconds 300
# 保持目标窗口 topmost：桌面上随时可能压着别的窗口（浏览器/终端），不置顶的话
# 覆盖层的命中测试会选中真正最上面的那个，断言就全对不上了。覆盖层自己是后显示的
# topmost 窗口，Windows 会把它排到更前面，仍然盖得住目标窗口。
Start-Sleep -Milliseconds 400
$rect = Get-ExtendedRect $targetHandle
$windowWidth = $rect.Right - $rect.Left
$windowHeight = $rect.Bottom - $rect.Top
Write-Host ("目标窗口矩形：{0},{1} {2}x{3}" -f $rect.Left, $rect.Top, $windowWidth, $windowHeight)
Check "目标窗口尺寸合理" ($windowWidth -ge 600 -and $windowHeight -ge 400) ("{0}x{1}" -f $windowWidth, $windowHeight)

$saveDir = Get-SaveDirectory
$before = Get-NewestSavedFile $saveDir
$beforeName = if ($before) { $before.Name } else { "" }
$mark = Get-LogMark
$process = Start-Host -WarmupSeconds 6
Check "宿主仍在运行" (-not $process.HasExited)

# --- 用例 1：悬停 + 单击截窗口 ---
Send-KeyCombo (Convert-HotkeyToVirtualKeys "Ctrl+Alt+A")
$shown = Wait-LogPattern "已截取覆盖层背景" 20 -SinceLine $mark
Check "覆盖层已显示" ($null -ne $shown) ([string]$shown)

$centerX = [int](($rect.Left + $rect.Right) / 2)
$centerY = [int](($rect.Top + $rect.Bottom) / 2)
[RegionVerify.Win]::SetCursorPos($centerX, $centerY) | Out-Null
Start-Sleep -Milliseconds 900   # 等 100ms 指数平滑收敛
$hoverShot = Join-Path $script:OutDir "region_hover.png"
Save-ScreenShot $hoverShot | Out-Null
Check "悬停截图已保存" (Test-Path $hoverShot) $hoverShot
Check "边框/折角使用主题色" (Test-ThemeColorNearRect $hoverShot $rect) "#4C9AFF"

Click-At $centerX $centerY
$saved = Wait-LogPattern "CapturePipeline: 已保存" 25 -SinceLine $mark
Check "单击截取窗口" ($null -ne $saved) ([string]$saved)
$file = Get-NewestSavedFile $saveDir $beforeName
if ($file) {
    $size = Get-ImageSize $file.FullName
    Check "单击截图尺寸 = 窗口尺寸" (([Math]::Abs($size.Width - $windowWidth) -le 2) -and
        ([Math]::Abs($size.Height - $windowHeight) -le 2)) ("{0}x{1} vs {2}x{3}" -f $size.Width, $size.Height, $windowWidth, $windowHeight)
} else {
    Check "单击截图文件存在" $false "没有新文件"
}

Stop-Host
Start-Sleep -Milliseconds 600

# --- 用例 2：拖动选区（不做窗口吸附、不做动画） ---
$before = Get-NewestSavedFile $saveDir
$beforeName = if ($before) { $before.Name } else { "" }
$mark = Get-LogMark
$process = Start-Host -WarmupSeconds 6
Send-KeyCombo (Convert-HotkeyToVirtualKeys "Ctrl+Alt+A")
$shownAgain = Wait-LogPattern "已截取覆盖层背景" 20 -SinceLine $mark
Check "覆盖层再次显示" ($null -ne $shownAgain) ([string]$shownAgain)
Start-Sleep -Milliseconds 400
Invoke-MouseDrag 120 120 620 520
Start-Sleep -Milliseconds 300
$savedDrag = Wait-LogPattern "CapturePipeline: 已保存" 25 -SinceLine $mark
Check "拖动完成采集" ($null -ne $savedDrag) ([string]$savedDrag)
$dragFile = Get-NewestSavedFile $saveDir $beforeName
if ($dragFile) {
    $dragSize = Get-ImageSize $dragFile.FullName
    Check "拖动截图尺寸 ≈ 500x400" (([Math]::Abs($dragSize.Width - 500) -le 6) -and
        ([Math]::Abs($dragSize.Height - 400) -le 6)) ("{0}x{1}" -f $dragSize.Width, $dragSize.Height)
} else {
    Check "拖动截图文件存在" $false "没有新文件"
}

Stop-Host
if ($targetHandle -ne [IntPtr]::Zero -and -not $target.HasExited) { $target.Kill() }
Restore-Config
Complete-Verification
