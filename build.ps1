# ============================================================================
#  build.ps1 -- 一键构建整个解决方案（标准入口）
#
#    .\build.ps1                    # Debug
#    .\build.ps1 -Configuration Release
#
#  项目构成（ldac-driver.sln）：
#    LdacA2dp      内核最小通道   -> bin-a2dp\x64\<cfg>\LdacA2dp\ (sys+inf+cat)
#    ldac-send     用户态发送端   -> bin-host\<cfg>\ldac-send.exe
#    ldac-enc-test libldac 自检   -> bin-tests\<cfg>\ldac-enc-test.exe
#
#  环境坑（照抄自 experiments/build-dk.ps1，都是撞出来的）：
#    1. 进程环境块同时有 Path/PATH/path 三条 -> MSB6001，只能 Start-Process
#       -UseNewEnvironment（子进程环境从注册表重建）
#    2. 不能用 Start-Process -Wait（等整棵进程树会挂），自己轮询 HasExited
# ============================================================================
param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Debug',
    [string[]]$ExtraArgs = @(),
    [string]$Target = ''
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "找不到 vswhere.exe" }

$vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "没找到装了 C++ 工具的 VS 实例" }
$msb = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msb)) { $msb = 'MSBuild.exe' }

Write-Host "VS     : $vsPath" -ForegroundColor Cyan
Write-Host "Config : $Configuration|x64" -ForegroundColor Cyan

$log = Join-Path $root "log\build-sln.log"
New-Item -ItemType Directory -Path (Join-Path $root 'log') -Force | Out-Null

$msbArgs = @(
    "`"$(Join-Path $root 'ldac-driver.sln')`""
    '/nologo', '/m:1', '/nodeReuse:false', '/v:minimal'
    "/p:Configuration=$Configuration"
    "/p:Platform=x64"
    '/p:SkipPackageVerification=true'
    '/p:TrackFileAccess=false'
    "/flp:logfile=$log;verbosity=normal;encoding=utf-8"
)
if ($Target)    { $msbArgs += "/t:$Target" }
if ($ExtraArgs) { $msbArgs += $ExtraArgs }

Write-Host "构建中 ..." -ForegroundColor Cyan
$proc = Start-Process -FilePath $msb -ArgumentList $msbArgs -UseNewEnvironment -PassThru -NoNewWindow
$sw = [Diagnostics.Stopwatch]::StartNew()
$deadline = (Get-Date).AddMinutes(10)
while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
}
$sw.Stop()
if (-not $proc.HasExited) {
    try { $proc.Kill() } catch { }
    throw "编译超时（>10 分钟），已强杀 msbuild"
}
$code = $proc.ExitCode
Write-Host ("耗时 {0:N1} 秒，退出码 {1}" -f $sw.Elapsed.TotalSeconds, $code) -ForegroundColor Cyan

if ($code -ne 0) {
    Write-Host "`n---- 日志尾部 ----" -ForegroundColor Yellow
    if (Test-Path $log) { Get-Content $log -Tail 40 -Encoding UTF8 | Write-Host }
    throw "编译失败（$code）"
}

Write-Host "`n产物：" -ForegroundColor Green
foreach ($d in @('bin-a2dp', 'bin-host', 'bin-tests')) {
    Get-ChildItem (Join-Path $root $d) -Recurse -Include *.sys, *.cat, *.inf, *.exe -EA 0 |
        Where-Object { $_.DirectoryName -match "\\$Configuration(\}|\\)" -or $_.DirectoryName -match "\\$Configuration\\" } |
        Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
}
exit $code
