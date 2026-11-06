# ============================================================================
#  run-tests.ps1 — 用户态单元测试（不依赖 cmd.exe / vcvars）
#  自己拼 MSVC + Windows SDK 的 INCLUDE/LIB/PATH，然后编 Avdtp.c + test_avdtp.c
# ============================================================================
$ErrorActionPreference = 'Stop'

$root  = Split-Path $PSScriptRoot -Parent
$src   = Join-Path $root 'src'
$shim  = Join-Path $PSScriptRoot 'shim'
$obj   = Join-Path $PSScriptRoot 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null

# --- 定位 MSVC ---
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
if (-not $vs) { throw '找不到装了 C++ 工具的 VS 实例' }

$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory |
        Sort-Object Name -Descending | Select-Object -First 1

# --- 定位 Windows SDK ---
$kit = 'C:\Program Files (x86)\Windows Kits\10'
$sdk = Get-ChildItem (Join-Path $kit 'Include') -Directory |
       Where-Object { Test-Path (Join-Path $_.FullName 'ucrt') } |
       Sort-Object Name -Descending | Select-Object -First 1

Write-Host "MSVC: $($msvc.Name)" -ForegroundColor Cyan
Write-Host "SDK : $($sdk.Name)" -ForegroundColor Cyan

$env:INCLUDE = (@(
    (Join-Path $msvc.FullName 'include')
    (Join-Path $kit "Include\$($sdk.Name)\ucrt")
    (Join-Path $kit "Include\$($sdk.Name)\um")
    (Join-Path $kit "Include\$($sdk.Name)\shared")
) -join ';')

$env:LIB = (@(
    (Join-Path $msvc.FullName 'lib\x64')
    (Join-Path $kit "Lib\$($sdk.Name)\ucrt\x64")
    (Join-Path $kit "Lib\$($sdk.Name)\um\x64")
) -join ';')

$env:PATH = (Join-Path $msvc.FullName 'bin\Hostx64\x64') + ';' + $env:PATH

# --- 编译 ---
Push-Location $PSScriptRoot
try {
    Write-Host "`n=== 编译 ===" -ForegroundColor Cyan
    & cl.exe /nologo /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS `
        "/I$shim" "/I$src" "/Fo$obj\" `
        (Join-Path $src 'Avdtp.c') 'test_avdtp.c' `
        /Fe:test_avdtp.exe

    if ($LASTEXITCODE -ne 0) { throw "编译失败（cl 退出码 $LASTEXITCODE）" }

    Write-Host "`n=== 运行 ===" -ForegroundColor Cyan
    & '.\test_avdtp.exe'
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($code -eq 0) { Write-Host "`n全部通过 ✅" -ForegroundColor Green }
else            { Write-Host "`n有失败用例 ❌" -ForegroundColor Red }
exit $code
