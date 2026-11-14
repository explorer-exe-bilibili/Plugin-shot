# ============================================================================
#  tools/package/make_host_package.ps1 —— 打 Windows 便携发行包
#
#  用法（在仓库根、Release 构建完成之后）：
#    powershell -NoProfile -ExecutionPolicy Bypass -File tools/package/make_host_package.ps1
#    powershell ... -Version 0.2.0 -BinDir build\Windows-release\bin -OutDir dist
#
#  产物：dist\Plugin-shot-<版本>-win64.zip 与同目录 .sha256（sha256sum 格式）。
#
#  只打包宿主运行必需的东西（对齐 0.1.0 发布包）：
#    Plugin-shot.exe / Plugin-shot.ico / 顶层 *.dll（vcpkg 运行库）
#    files\（内置语言包目录结构，含外部包与 README）
#    plugins\*.dll（插件自身产物；调试符号与导入库在构建期已剪枝）
#    LICENSE / NOTICE / THIRD_PARTY_LICENSES\
#
#  明确不打包：*.pdb、*.ilk、*.lib、host.log、config.json、verify 产物。
#
#  说明：PowerShell 5.1 读 UTF-8 脚本需要 BOM（AGENTS.md §7.11），本文件为
#  UTF-8 BOM + CRLF；路径一律用 Join-Path 拼，避免中文路径下字符串拼接出错。
# ============================================================================
param(
    [string]$Version = '0.2.0',
    [string]$BinDir = 'build\Windows-release\bin',
    [string]$OutDir = 'dist',
    [switch]$KeepStage
)

$ErrorActionPreference = 'Stop'

# 仓库根：本脚本位于 tools\package\ 下，向上一级是 tools，再上一级是仓库根。
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
function Resolve-Under([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
}

$BinFull = Resolve-Under $BinDir
$OutFull = Resolve-Under $OutDir
$Exe = Join-Path $BinFull 'Plugin-shot.exe'
if (-not (Test-Path -LiteralPath $Exe)) {
    throw "找不到宿主产物：$Exe（先做 Release 构建：见 AGENTS.md §6）"
}

$PackageName = "Plugin-shot-$Version-win64"
$StageRoot = Join-Path $OutFull 'staging'
$Stage = Join-Path $StageRoot $PackageName
$ZipPath = Join-Path $OutFull "$PackageName.zip"
$ShaPath = "$ZipPath.sha256"

if (Test-Path -LiteralPath $StageRoot) { Remove-Item -LiteralPath $StageRoot -Recurse -Force }
New-Item -ItemType Directory -Path $Stage -Force | Out-Null
New-Item -ItemType Directory -Path $OutFull -Force | Out-Null

function Copy-Required([string]$Path, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "发行包缺少必需文件：$Path"
    }
    Copy-Item -LiteralPath $Path -Destination $Destination -Force
}

# --- 1. 宿主 exe / 图标 / 顶层运行库 -----------------------------------------
Copy-Required $Exe $Stage
$Icon = Join-Path $BinFull 'Plugin-shot.ico'
if (Test-Path -LiteralPath $Icon) { Copy-Item -LiteralPath $Icon -Destination $Stage -Force }

# 顶层 *.dll 是 vcpkg 运行库（release 变体）；按扩展名拷贝，不挑名字，
# 避免白名单漏掉新依赖（漏一个就是启动即失败）。
Get-ChildItem -LiteralPath $BinFull -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $Stage -Force
}

# --- 2. files\（语言包目录）---------------------------------------------------
$FilesDir = Join-Path $BinFull 'files'
if (Test-Path -LiteralPath $FilesDir) {
    Copy-Item -LiteralPath $FilesDir -Destination $Stage -Recurse -Force
} else {
    Write-Warning "bin 下没有 files\，发行包不含外部语言包目录"
}

# --- 3. plugins\（插件自身 DLL）-----------------------------------------------
$PluginsDir = Join-Path $BinFull 'plugins'
if (-not (Test-Path -LiteralPath $PluginsDir)) {
    throw "bin 下没有 plugins\：宿主无法加载任何能力，拒绝打包"
}
$PluginStage = Join-Path $Stage 'plugins'
New-Item -ItemType Directory -Path $PluginStage -Force | Out-Null
$pluginCount = 0
Get-ChildItem -LiteralPath $PluginsDir -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $PluginStage -Force
    $pluginCount++
}
if ($pluginCount -eq 0) { throw "plugins\ 里没有任何 DLL，拒绝打包" }

# --- 4. 许可证 -----------------------------------------------------------------
Copy-Required (Join-Path $RepoRoot 'LICENSE') $Stage
Copy-Required (Join-Path $RepoRoot 'NOTICE') $Stage
# 第三方许可证：优先用 SDK 导出的 THIRD_PARTY_LICENSES（nlohmann-json / stb），
# 没有导出过时退到 tools\plugin_sdk\licenses\（同一批文件的源）。
$Licenses = Join-Path $RepoRoot 'sdk\THIRD_PARTY_LICENSES'
if (-not (Test-Path -LiteralPath $Licenses)) {
    $Licenses = Join-Path $RepoRoot 'tools\plugin_sdk\licenses'
}
if (Test-Path -LiteralPath $Licenses) {
    Copy-Item -LiteralPath $Licenses -Destination (Join-Path $Stage 'THIRD_PARTY_LICENSES') -Recurse -Force
} else {
    Write-Warning "找不到第三方许可证目录，发行包不含 THIRD_PARTY_LICENSES"
}

# 简短的包内说明：用户解压后第一眼知道怎么打开。
$QuickStart = @"
Plugin-shot $Version（Windows x64 便携版）
1. 解压到任意目录（不要只把 exe 拖出来，插件与语言包在子目录里）。
2. 运行 Plugin-shot.exe。默认在托盘里，右键托盘图标截屏/设置。
3. 插件在 plugins\ 目录；语言包在 files\lang\。插件说明见 README。
许可证：Apache-2.0（LICENSE）；第三方组件见 NOTICE 与 THIRD_PARTY_LICENSES\。
"@
Set-Content -LiteralPath (Join-Path $Stage 'QUICKSTART.txt') -Value $QuickStart -Encoding UTF8

# --- 5. 压缩 + sha256 ----------------------------------------------------------
if (Test-Path -LiteralPath $ZipPath) { Remove-Item -LiteralPath $ZipPath -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $Stage, $ZipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)

$hash = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$zipName = Split-Path -Leaf $ZipPath
# 与 `sha256sum -c` 兼容的格式：<hash>  <文件名>
Set-Content -LiteralPath $ShaPath -Value "$hash  $zipName" -Encoding ASCII

if (-not $KeepStage) { Remove-Item -LiteralPath $StageRoot -Recurse -Force -ErrorAction SilentlyContinue }

$sizeMb = [math]::Round((Get-Item -LiteralPath $ZipPath).Length / 1MB, 2)
Write-Host ""
Write-Host "发行包：$ZipPath"
Write-Host "  大小：$sizeMb MB，插件 $pluginCount 个"
Write-Host "  sha256：$hash"
Write-Host "  校验文件：$ShaPath"
