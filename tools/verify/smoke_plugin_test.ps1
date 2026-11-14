#region 说明
# ---------------------------------------------------------------------------
#  smoke_plugin_test.ps1 —— `--test-plugin` 插件自检冒烟
#
#  验证两件事：
#    1. example_processor.dll 自检通过：exit 0、JSON result=pass、plugins 数量 >= 2、
#       summary.fail = 0；
#    2. 不存在的 DLL：exit 2（用法/IO 错误，不是"插件失败"）。
#
#  与其它 e2e 脚本不同：本脚本**不启动常驻宿主、不读写用户 config、不需要交互桌面**
#  的后台操作（自检内部用隔离数据目录，日志/配置都落在系统临时目录并在结束时删除）。
#  因此也不会 Stop-Host / Backup-Config——请不要照抄本脚本的前置方式去跑别的 e2e。
#
#  失败时打印子进程 stdout / stderr；stderr 里已经包含自检模式回显的隔离目录
#  host.log 末尾（见 document/PLUGIN_TEST.md §4.1），不需要再去临时目录里找日志。
#
#  兼容 Windows PowerShell 5.1：本文件是 UTF-8 **带 BOM**（否则中文会乱码）。
# ---------------------------------------------------------------------------
#endregion

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "common.ps1")

# 子进程输出是 UTF-8（宿主启动时 SetConsoleOutputCP(CP_UTF8)），显式切一下解码方式，
# 否则 PowerShell 5.1 会按 ANSI 代码页读，中文明细变成乱码。
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$script:PluginTestJson = Join-Path $script:OutDir "plugin_test.json"
$script:PluginTestStdout = Join-Path $script:OutDir "plugin_test.stdout.txt"
$script:PluginTestStderr = Join-Path $script:OutDir "plugin_test.stderr.txt"

function Read-CapturedText([string]$Path) {
    if (-not (Test-Path $Path)) { return "" }
    return (Get-Content $Path -Raw -Encoding UTF8)
}

function Show-OutputContext([string]$Label) {
    Write-Host ""
    Write-Host "---- $Label stdout ----" -ForegroundColor DarkYellow
    Write-Host (Read-CapturedText $script:PluginTestStdout)
    Write-Host "---- $Label stderr（含自检回显的 host.log 末尾）----" -ForegroundColor DarkYellow
    Write-Host (Read-CapturedText $script:PluginTestStderr)
    Write-Host "---- 输出结束 ----" -ForegroundColor DarkYellow
}

# 运行一次宿主 CLI 并捕获 stdout / stderr / exit code。
# 不用 `& $exe ... 2>&1`：那会把两条流混在一起，看不出哪条是错误输出。
function Invoke-HostCli([string[]]$Arguments) {
    foreach ($path in @($script:PluginTestStdout, $script:PluginTestStderr)) {
        if (Test-Path $path) { Remove-Item $path -Force }
    }
    $process = Start-Process -FilePath $script:HostExe `
        -WorkingDirectory $script:RepoRoot `
        -ArgumentList $Arguments `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $script:PluginTestStdout `
        -RedirectStandardError $script:PluginTestStderr
    return [int]$process.ExitCode
}

Write-Step "插件自检：example_processor.dll"

$exampleDll = Join-Path $script:PluginDir "example_processor.dll"
if (Test-Path $script:PluginTestJson) { Remove-Item $script:PluginTestJson -Force }
$exitCode = Invoke-HostCli @("--test-plugin", $exampleDll, "--json-out", $script:PluginTestJson)
Check "example_processor 自检 exit 0" ($exitCode -eq 0) "exit=$exitCode"

$document = $null
if (Test-Path $script:PluginTestJson) {
    try {
        $document = Get-Content $script:PluginTestJson -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        $document = $null
    }
}
Check "JSON 输出存在且可解析" ($null -ne $document) $script:PluginTestJson

if ($null -ne $document) {
    Check "JSON result = pass" ($document.result -eq "pass") "result=$($document.result)"
    $pluginCount = @($document.plugins).Count
    Check "JSON plugins 数量 >= 2" ($pluginCount -ge 2) "count=$pluginCount"
    Check "JSON summary.fail = 0" ([int]$document.summary.fail -eq 0) `
        "fail=$($document.summary.fail)"
}

$exampleFailed = ($exitCode -ne 0) -or ($null -eq $document) -or ($document.result -ne "pass")
if ($exampleFailed) {
    Show-OutputContext "example_processor"
}

Write-Step "负例：不存在的 DLL"

$missingDll = Join-Path $script:OutDir "plugin_that_does_not_exist.dll"
if (Test-Path $missingDll) { Remove-Item $missingDll -Force }
$missingExitCode = Invoke-HostCli @("--test-plugin", $missingDll)
# 参数/IO 错误固定 exit 2：不存在的 DLL 不属于"插件检查失败"（那才是 1）。
Check "不存在的 DLL 退出码 2" ($missingExitCode -eq 2) "exit=$missingExitCode"
if ($missingExitCode -ne 2) {
    Show-OutputContext "missing-dll"
}

Complete-Verification
