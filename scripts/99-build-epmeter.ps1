# ============================================================================
#  99-build-epmeter.ps1 -- 编译端点音量/电平仪表 epmeter.exe
#
#  与 98-build-host.ps1 同一套环境构造（显式指路 MSVC/SDK）。
# ============================================================================
param(
    [string]$VcRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231',
    [string]$SdkRoot = 'C:\Program Files (x86)\Windows Kits\10',
    [string]$SdkVer = '10.0.28000.0'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot          # 仓库根（本脚本在 scripts\）
$outDir = Join-Path $root 'bin-host'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }

$cl = Join-Path $VcRoot 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $cl)) { throw "cl.exe not found: $cl" }

$env:PATH = (Join-Path $VcRoot 'bin\Hostx64\x64') + ';' + $env:PATH
$env:INCLUDE = @(
    (Join-Path $VcRoot 'include'),
    (Join-Path $SdkRoot "Include\$SdkVer\ucrt"),
    (Join-Path $SdkRoot "Include\$SdkVer\shared"),
    (Join-Path $SdkRoot "Include\$SdkVer\um")
) -join ';'
$env:LIB = @(
    (Join-Path $VcRoot 'lib\x64'),
    (Join-Path $SdkRoot "Lib\$SdkVer\ucrt\x64"),
    (Join-Path $SdkRoot "Lib\$SdkVer\um\x64")
) -join ';'

$src = Join-Path $root 'host\epmeter.cpp'
if (-not (Test-Path $src)) { throw "missing source: $src" }

$exe = Join-Path $outDir 'epmeter.exe'
$cargs = @(
    '/nologo', '/O2', '/W3', '/EHsc', '/MD', '/utf-8',
    '/D_CRT_SECURE_NO_WARNINGS',
    $src,
    '/link', 'ole32.lib', 'propsys.lib', 'uuid.lib', ('/out:' + $exe)
)

Push-Location $outDir
try { & $cl @cargs; $code = $LASTEXITCODE } finally { Pop-Location }

Write-Host ""
if ($code -eq 0) {
    Write-Host "BUILD OK -> $exe" -ForegroundColor Green
    Write-Host ("size = " + (Get-Item $exe).Length + " bytes")
} else {
    Write-Host "BUILD FAILED (exit $code)" -ForegroundColor Red
}
exit $code
