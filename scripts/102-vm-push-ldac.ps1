# ============================================================================
#  102-vm-push-ldac.ps1  —  主机侧：把 LdacA2dp 驱动包推进客户机
# ============================================================================
param(
    [string]$Vmx        = 'D:\VMs\Win11_26H2\Win11_26H2_DriverTest.vmx',
    [string]$GuestUser  = 'test',
    [string]$GuestPass  = 'Ldac2026!',
    [string]$Configuration = 'Debug',
    [string]$GuestDir   = 'C:\ldac\ldacdrv'
)

$ErrorActionPreference = 'Stop'

$vmrun  = 'C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe'
$root   = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $root 'AGENTS.md'))) { $root = 'C:\Developer\ldac' }

$pkg = Join-Path $root "bin-a2dp\x64\$Configuration\LdacA2dp"
$cer = Join-Path $root "bin-a2dp\x64\$Configuration\LdacA2dp.cer"

#  INF + .sys + .cat 必须同目录，pnputil 才认签名
$files = @(
    @{ Local = (Join-Path $pkg 'LdacA2dp.inf'); Name = 'LdacA2dp.inf' },
    @{ Local = (Join-Path $pkg 'LdacA2dp.sys'); Name = 'LdacA2dp.sys' },
    @{ Local = (Join-Path $pkg 'ldaca2dp.cat'); Name = 'ldaca2dp.cat' },
    @{ Local = $cer;                            Name = 'LdacA2dp.cer' }
)

foreach ($f in $files) {
    if (-not (Test-Path $f.Local)) { throw "缺文件：$($f.Local)（先跑 build.ps1）" }
}

function Vmr { param([string[]]$A) & $vmrun -T ws -gu $GuestUser -gp $GuestPass @A 2>&1 }

Write-Host "=== 客户机目录 $GuestDir" -ForegroundColor Cyan
$null = Vmr @('createDirectoryInGuest', $Vmx, $GuestDir)

Write-Host "=== 上传驱动包" -ForegroundColor Cyan
foreach ($f in $files) {
    $null = Vmr @('copyFileFromHostToGuest', $Vmx, $f.Local, "$GuestDir\$($f.Name)")
    "  {0,-16} {1,7} bytes" -f $f.Name, (Get-Item $f.Local).Length
}

$devcon = 'D:\VMs\_pkg\devcon.exe'
if (Test-Path $devcon) {
    $null = Vmr @('copyFileFromHostToGuest', $Vmx, $devcon, 'C:\ldac\devcon.exe')
    Write-Host "  devcon.exe      已就位" -ForegroundColor DarkGray
}

Write-Host "`n推进完成。下一步（客户机侧）：" -ForegroundColor Green
Write-Host "  .\experiments\scripts\100-vm-run.ps1 -Script .\experiments\scripts\vm-install-ldac.ps1 -OutName ldacroot" -ForegroundColor DarkGray
