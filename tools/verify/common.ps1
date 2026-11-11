#region 说明
# ---------------------------------------------------------------------------
#  common.ps1 —— tools/verify 下所有端到端脚本共用的工具函数
#
#  这些脚本是**真机（交互桌面）验证**，不是 CI 单元测试：它们会启动宿主、模拟键鼠、
#  读剪贴板、接管前台窗口。运行纪律：
#    · 同一时间只跑一个脚本（多开宿主会互相抢 config.json 与前台焦点）；
#    · 脚本会备份/恢复 %LOCALAPPDATA%\screen-shot\config.json，异常中断时手动跑
#      Restore-Config 或从 build\verify\config.backup.json 拷回；
#    · 截图/日志等产物统一落 build\verify\（gitignored），不入库。
#
#  兼容 Windows PowerShell 5.1：本文件是 UTF-8 **带 BOM**（否则中文会乱码）。
# ---------------------------------------------------------------------------
#endregion

$ErrorActionPreference = "Stop"

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$script:HostExe = Join-Path $script:RepoRoot "build\Windows\bin\Plugin-shot.exe"
$script:PluginDir = Join-Path $script:RepoRoot "build\Windows\bin\plugins"
$script:OutDir = Join-Path $script:RepoRoot "build\verify"
$script:DataDir = Join-Path $env:LOCALAPPDATA "screen-shot"
$script:ConfigPath = Join-Path $script:DataDir "config.json"
$script:LogPath = Join-Path $script:DataDir "logs\host.log"
$script:ConfigBackup = Join-Path $script:OutDir "config.backup.json"
$script:CheckResults = New-Object System.Collections.ArrayList

New-Item -ItemType Directory -Force $script:OutDir | Out-Null

# 把当前进程声明成 per-monitor DPI aware（V2）：否则 CopyFromScreen / Screen.Bounds
# 拿到的是 DPI 虚拟化后的逻辑坐标，双屏混合 DPI 下会截错区域。
if (-not ([System.Management.Automation.PSTypeName]"DshVerifyDpi").Type) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class DshVerifyDpi {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
}

function Initialize-PmAwareness {
    if (-not ("DshVerifyWindow" -as [type])) {
        Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class DshVerifyWindow {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr lparam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
"@
    }
    [DshVerifyDpi]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
}

# 物理虚拟桌面矩形（PMv2 下 GetSystemMetrics 返回物理像素）。
function Get-VirtualDesktopRect {
    return [pscustomobject]@{
        X = [DshVerifyDpi]::GetSystemMetrics(76)
        Y = [DshVerifyDpi]::GetSystemMetrics(77)
        Width = [DshVerifyDpi]::GetSystemMetrics(78)
        Height = [DshVerifyDpi]::GetSystemMetrics(79)
    }
}

# 物理主显示器尺寸（写断言时用）。
function Get-PrimaryScreenSize {
    $rect = Get-VirtualDesktopRect
    return [pscustomobject]@{ Width = $rect.Width; Height = $rect.Height }
}

function Write-Step([string]$message) {
    Write-Host ""
    Write-Host "== $message ==" -ForegroundColor Cyan
}

function Check([string]$name, [bool]$ok, [string]$detail = "") {
    $status = if ($ok) { "PASS" } else { "FAIL" }
    $color = if ($ok) { "Green" } else { "Red" }
    $line = "[$status] $name"
    if ($detail) { $line += " —— $detail" }
    Write-Host $line -ForegroundColor $color
    [void]$script:CheckResults.Add([pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail })
}

function Get-FailedCount {
    return @($script:CheckResults | Where-Object { -not $_.Ok }).Count
}

function Complete-Verification {
    $failed = Get-FailedCount
    Write-Host ""
    Write-Host ("结果：{0}/{1} 通过" -f ($script:CheckResults.Count - $failed), $script:CheckResults.Count) -ForegroundColor $(if ($failed -eq 0) { "Green" } else { "Red" })
    if ($failed -gt 0) { exit 1 }
    exit 0
}

#region 宿主进程与窗口
function Stop-Host {
    Get-Process Plugin-shot -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 700
}

function Start-Host([string[]]$Arguments = @(), [int]$WarmupSeconds = 5) {
    if (-not (Test-Path $script:HostExe)) {
        throw "找不到宿主：$script:HostExe（先跑 tools/verify/build.sh）"
    }
    # Start-Process 不接受"空数组"作为 -ArgumentList：无参数时不要传这个键。
    $startArgs = @{
        FilePath = $script:HostExe
        WorkingDirectory = (Split-Path $script:HostExe)
        PassThru = $true
    }
    if ($Arguments -and @($Arguments).Count -gt 0) { $startArgs["ArgumentList"] = $Arguments }
    $process = Start-Process @startArgs
    Start-Sleep -Seconds $WarmupSeconds
    return $process
}

# 按窗口类名/标题找顶层窗口（GLFW 在 Win32 上的类名固定是 GLFW30）。
function Find-Window([string]$ClassName, [string]$TitleLike, [int]$TimeoutSeconds = 20) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $script:foundWindow = [IntPtr]::Zero
        [DshVerifyWindow]::EnumWindows({
            param($hwnd, $lparam)
            $cls = New-Object System.Text.StringBuilder 256
            [DshVerifyWindow]::GetClassNameW($hwnd, $cls, 256) | Out-Null
            $title = New-Object System.Text.StringBuilder 256
            [DshVerifyWindow]::GetWindowTextW($hwnd, $title, 256) | Out-Null
            if ($cls.ToString() -eq $ClassName -and ($TitleLike -eq "" -or $title.ToString() -like $TitleLike)) {
                $script:foundWindow = $hwnd
            }
            return $true
        }, [IntPtr]::Zero) | Out-Null
        if ($script:foundWindow -ne [IntPtr]::Zero) { return $script:foundWindow }
        Start-Sleep -Milliseconds 250
    }
    return [IntPtr]::Zero
}

# 设置窗口标题随界面语言变化（"Plugin-shot 设置" / "Plugin-shot Settings"），
# 语言无关的脚本用这个函数找窗口；只匹配 GLFW 窗口类，避免抓到别的窗口。
function Find-SettingsWindow([int]$TimeoutSeconds = 20) {
    return Find-WindowByTitles "GLFW30" @("*设置*", "*Settings*") $TimeoutSeconds
}

# 按窗口类 + 多个标题通配符查找；任一标题命中即返回。语义与 Find-Window 相同。
function Find-WindowByTitles([string]$ClassName, [string[]]$TitlePatterns, [int]$TimeoutSeconds = 20) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $script:foundWindow = [IntPtr]::Zero
        [DshVerifyWindow]::EnumWindows({
            param($hwnd, $lparam)
            $cls = New-Object System.Text.StringBuilder 256
            [DshVerifyWindow]::GetClassNameW($hwnd, $cls, 256) | Out-Null
            if ($cls.ToString() -ne $ClassName) { return $true }
            $title = New-Object System.Text.StringBuilder 256
            [DshVerifyWindow]::GetWindowTextW($hwnd, $title, 256) | Out-Null
            foreach ($pattern in $TitlePatterns) {
                if ($title.ToString() -like $pattern) {
                    $script:foundWindow = $hwnd
                    break
                }
            }
            return $true
        }, [IntPtr]::Zero) | Out-Null
        if ($script:foundWindow -ne [IntPtr]::Zero) { return $script:foundWindow }
        Start-Sleep -Milliseconds 250
    }
    return [IntPtr]::Zero
}

function Focus-Window([IntPtr]$Window) {
    if ($Window -eq [IntPtr]::Zero) { return }
    [DshVerifyWindow]::ShowWindow($Window, 5) | Out-Null
    # 截图时窗口可能被别的程序挡住：临时置顶（HWND_TOPMOST + SWP_NOMOVE|SWP_NOSIZE）。
    [DshVerifyWindow]::SetWindowPos($Window, [IntPtr](-1), 0, 0, 0, 0, 3) | Out-Null
    [DshVerifyWindow]::SetForegroundWindow($Window) | Out-Null
    Start-Sleep -Milliseconds 800
}

function Save-ScreenShot([string]$Path) {
    Add-Type -AssemblyName System.Drawing
    $rect = Get-VirtualDesktopRect
    $bitmap = New-Object System.Drawing.Bitmap $rect.Width, $rect.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($rect.X, $rect.Y, 0, 0, (New-Object System.Drawing.Size $rect.Width, $rect.Height))
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $graphics.Dispose()
    $bitmap.Dispose()
    return $Path
}
#endregion

#region 日志
function Get-LogTail([int]$Count = 60) {
    if (-not (Test-Path $script:LogPath)) { return @() }
    return @(Get-Content $script:LogPath -Tail $Count -Encoding UTF8 -ErrorAction SilentlyContinue)
}

function Get-LogLines {
    if (-not (Test-Path $script:LogPath)) { return @() }
    return @(Get-Content $script:LogPath -Encoding UTF8 -ErrorAction SilentlyContinue)
}

# 记录"当前日志有多少行"：之后只匹配这行之后的新内容，避免命中上一轮运行的旧日志。
function Get-LogMark {
    return (Get-LogLines).Count
}

# 等一条"新出现的"日志行（自 SinceLine 之后）。返回匹配行或 $null。
# SinceLine 用 Get-LogMark 在动作前记录；这样上一条命令的日志不会造成假阳性。
function Wait-LogPattern([string]$Pattern, [int]$TimeoutSeconds = 20, [int]$SinceLine = 0) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $lines = Get-LogLines
        if ($lines.Count -gt $SinceLine) {
            $fresh = $lines[$SinceLine..($lines.Count - 1)]
            $match = $fresh | Select-String -Pattern $Pattern -SimpleMatch | Select-Object -Last 1
            if ($match) { return $match.Line }
        }
        Start-Sleep -Milliseconds 300
    }
    return $null
}

# 把 "Ctrl+Alt+S" / "Shift+Q" / "F9" 这类热键文本转成 keybd_event 需要的虚拟键序列。
# 解析失败返回 $null（调用方应跳过按键断言并给出提示）。
function Convert-HotkeyToVirtualKeys([string]$Spec) {
    if (-not $Spec) { return $null }
    $keys = New-Object System.Collections.ArrayList
    foreach ($raw in $Spec.Split("+")) {
        $token = $raw.Trim()
        if (-not $token) { return $null }
        switch -Regex ($token) {
            "^(?i)(ctrl|control)$" { [void]$keys.Add(0x11); $key = $true; break }
            "^(?i)alt$"            { [void]$keys.Add(0x12); $key = $true; break }
            "^(?i)(shift)$"        { [void]$keys.Add(0x10); $key = $true; break }
            "^(?i)(win|windows)$"  { [void]$keys.Add(0x5B); $key = $true; break }
            "^(?i)^[A-Z]$"         { [void]$keys.Add([int][char]$token.ToUpperInvariant()); $key = $true; break }
            "^(?i)^[0-9]$"         { [void]$keys.Add([int][char]$token); $key = $true; break }
            "^(?i)^F([1-9]|1[0-2])$" { [void]$keys.Add(0x6F + [int]$token.Substring(1)); $key = $true; break }
            default { return $null }
        }
    }
    if ($keys.Count -eq 0) { return $null }
    return $keys.ToArray()
}
#endregion

#region 输入注入
function Send-KeyCombo([int[]]$VirtualKeys) {
    foreach ($vk in $VirtualKeys) {
        [DshVerifyDpi]::keybd_event([byte]$vk, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 40
    }
    for ($i = $VirtualKeys.Count - 1; $i -ge 0; $i--) {
        [DshVerifyDpi]::keybd_event([byte]$VirtualKeys[$i], 0, 2, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 40
    }
}

# 鼠标拖拽（屏幕物理像素坐标）。
function Invoke-MouseDrag([int]$FromX, [int]$FromY, [int]$ToX, [int]$ToY, [int]$Steps = 8) {
    [DshVerifyDpi]::SetCursorPos($FromX, $FromY) | Out-Null
    Start-Sleep -Milliseconds 200
    [DshVerifyDpi]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
    Start-Sleep -Milliseconds 150
    for ($i = 1; $i -le $Steps; $i++) {
        $x = $FromX + [int](($ToX - $FromX) * $i / $Steps)
        $y = $FromY + [int](($ToY - $FromY) * $i / $Steps)
        [DshVerifyDpi]::SetCursorPos($x, $y) | Out-Null
        Start-Sleep -Milliseconds 60
    }
    [DshVerifyDpi]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
}
#endregion

#region 配置
# 备份时记下"原本有没有配置文件"：本来没有的话，恢复时必须删掉本次运行生成的配置，
# 否则第一次运行的默认值会被下一次脚本当成"已存在"，旧的测试值又会被恢复回来。
$script:ConfigAbsentMarker = Join-Path $script:OutDir "config.backup.absent"

function Backup-Config {
    Remove-Item $script:ConfigAbsentMarker -Force -ErrorAction SilentlyContinue
    if (Test-Path $script:ConfigPath) {
        Copy-Item $script:ConfigPath $script:ConfigBackup -Force
    } else {
        Remove-Item $script:ConfigBackup -Force -ErrorAction SilentlyContinue
        New-Item -ItemType File -Force $script:ConfigAbsentMarker | Out-Null
    }
}

function Restore-Config {
    if (Test-Path $script:ConfigAbsentMarker) {
        Remove-Item $script:ConfigPath -Force -ErrorAction SilentlyContinue
        Remove-Item $script:ConfigAbsentMarker -Force -ErrorAction SilentlyContinue
        return
    }
    if (Test-Path $script:ConfigBackup) {
        Copy-Item $script:ConfigBackup $script:ConfigPath -Force
    }
}

function Get-ConfigValue([string]$Key) {
    if (-not (Test-Path $script:ConfigPath)) { return "<no-config>" }
    $doc = Get-Content $script:ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($item in $doc.configItems) {
        if ($item.name -eq $Key) { return $item.value }
    }
    return "<missing>"
}

# 写一个配置项（只改 config.json；调用前必须 Stop-Host，改完再启动宿主读取）。
# 保持 version/configItems 结构；缺文件时建最小骨架。其它已有项原样保留。
function Set-ConfigValue([string]$Key, [string]$Value) {
    $doc = $null
    if (Test-Path $script:ConfigPath) {
        $doc = Get-Content $script:ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json
    }
    if ($null -eq $doc) {
        $doc = [pscustomobject]@{ version = "0.1.0"; configItems = @() }
    }
    if ($null -eq $doc.configItems) {
        $doc | Add-Member -NotePropertyName configItems -NotePropertyValue @() -Force
    }
    $found = $false
    foreach ($item in $doc.configItems) {
        if ($item.name -eq $Key) {
            $item.value = $Value
            $found = $true
        }
    }
    if (-not $found) {
        $doc.configItems += [pscustomobject]@{ name = $Key; value = $Value }
    }
    ($doc | ConvertTo-Json -Depth 8) | Set-Content $script:ConfigPath -Encoding UTF8
}

# 保存插件的输出目录（配置为空 = 系统图片目录）。
function Get-SaveDirectory {
    $configured = Get-ConfigValue "image_saver.main/directory"
    if ($configured -and $configured -ne "<missing>" -and $configured -ne "<no-config>") {
        return $configured
    }
    return (Join-Path $env:USERPROFILE "Pictures")
}

function Get-NewestSavedFile([string]$Directory, [string]$BeforeName = "") {
    $files = @(Get-ChildItem -Path $Directory -Filter "shot_*.*" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    if ($files.Count -eq 0) { return $null }
    if ($BeforeName -and $files[0].Name -eq $BeforeName) { return $null }
    return $files[0]
}

function Get-ImageSize([string]$Path) {
    Add-Type -AssemblyName System.Drawing
    $image = [System.Drawing.Image]::FromFile($Path)
    try { return [pscustomobject]@{ Width = $image.Width; Height = $image.Height } }
    finally { $image.Dispose() }
}
#endregion
