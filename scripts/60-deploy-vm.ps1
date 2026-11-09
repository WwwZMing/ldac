# ============================================================================
#  60-deploy-vm.ps1 -- 把当前构建完整部署到测试虚拟机（AGENTS.md 清单的脚本版）
# ============================================================================
#  用法：
#    .\scripts\60-deploy-vm.ps1                     # 内核路径（UserModeAvdtp=0）
#    .\scripts\60-deploy-vm.ps1 -UserModeAvdtp 1    # 用户态 AVDTP 路径
#    .\scripts\60-deploy-vm.ps1 -VerifyOnly         # 只验证，不动任何东西
#    .\scripts\60-deploy-vm.ps1 -SkipReboot         # 部署但不重启
# ============================================================================
[CmdletBinding()]
param(
    [ValidateSet(0, 1)][int]$UserModeAvdtp = 0,
    [switch]$SkipReboot,
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'
$root   = 'C:\Developer\ldac'
$guest  = 'test@192.168.144.130'
$sshKey = Join-Path $env:USERPROFILE '.ssh\id_ed25519_vm_test'
$remote = Join-Path $root 'scripts\remote.ps1'

function Guest([string]$Command, [int]$TimeoutSec = 150) {
    & $remote -TimeoutSec $TimeoutSec -Command $Command
}

function Fail([string]$Message) {
    Write-Host "FAIL: $Message" -ForegroundColor Red
    exit 1
}

Write-Host '== 0) 预检 ==' -ForegroundColor Cyan
foreach ($f in @(
    "$root\bin-a2dp\x64\Debug\LdacA2dp\LdacA2dp.inf",
    "$root\bin-a2dp\x64\Debug\LdacA2dp\LdacA2dp.sys",
    "$root\bin-a2dp\x64\Debug\LdacA2dp\ldaca2dp.cat",
    "$root\bin-a2dp\x64\Debug\LdacA2dp.cer",
    "$root\panel\publish\LdacPanel.exe",
    "$root\bin-host\Debug\ldac-send.exe",
    "$root\scripts\50-install-service.ps1")) {
    if (-not (Test-Path $f)) { Fail "缺少构建产物：$f（先跑 .\build.ps1 和 dotnet publish）" }
}
Write-Host '   构建产物齐全'

$sshUp = $false
for ($i = 0; $i -lt 20; $i++) {
    $r = Guest '"up"' 8
    if ($r -match 'up') { $sshUp = $true; break }
    Start-Sleep -Seconds 5
}
if (-not $sshUp) { Fail '客户机 SSH 不通（先启动虚拟机）' }
Write-Host '   SSH 通'

if (-not $VerifyOnly) {
    Write-Host '== 1) 推包 ==' -ForegroundColor Cyan
    $files = @(
        "$root\bin-a2dp\x64\Debug\LdacA2dp\LdacA2dp.inf",
        "$root\bin-a2dp\x64\Debug\LdacA2dp\LdacA2dp.sys",
        "$root\bin-a2dp\x64\Debug\LdacA2dp\ldaca2dp.cat",
        "$root\bin-a2dp\x64\Debug\LdacA2dp.cer")
    & scp -i $sshKey -o BatchMode=yes @files "$($guest):C:/ldac/ldacdrv/" | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail '驱动包推送失败' }

    # 面板服务在跑时 exe 被锁：必须先停服务再推（踩过的坑）
    Guest '& sc.exe stop LdacA2dpSvc 2>&1 | Out-Null; Start-Sleep -Seconds 2; Get-Process ldac-send,LdacPanel -EA 0 | Stop-Process -Force; & sc.exe delete LdacA2dpSvc 2>&1 | Out-Null; "svc removed"' 120 | Out-Null
    & scp -i $sshKey -o BatchMode=yes "$root\panel\publish\LdacPanel.exe" "$($guest):C:/ldac/panel/LdacPanel.exe" | Out-Null
    & scp -i $sshKey -o BatchMode=yes "$root\bin-host\Debug\ldac-send.exe" "$($guest):C:/ldac/panel/ldac-send.exe" | Out-Null
    & scp -i $sshKey -o BatchMode=yes "$root\scripts\50-install-service.ps1" "$($guest):C:/ldac/install-service.ps1" | Out-Null
    Write-Host '   推送完成'

    Write-Host '== 2) 换驱动包（先卸载再安装）==' -ForegroundColor Cyan
    Guest '& C:\ldac\ldacdrv\vm-install-ldac.ps1 -Target uninstall -PkgDir C:\ldac\ldacdrv *> C:\ldac\un.log; "uninstalled"' 180 | Out-Null
    Guest '& C:\ldac\ldacdrv\vm-install-ldac.ps1 -Target bt -PkgDir C:\ldac\ldacdrv *> C:\ldac\inst.log; "installed"' 180 | Out-Null
    Write-Host '   驱动包已切换'

    Write-Host '== 3) 面板服务 + 开关 ==' -ForegroundColor Cyan
    Guest '& C:\PROGRA~1\PowerShell\7\pwsh.exe -NoProfile -ExecutionPolicy Bypass -File C:\ldac\install-service.ps1 -Action install -Exe C:\ldac\panel\LdacPanel.exe *> C:\ldac\svc.log; "svc installed"' 180 | Out-Null
    $switchCmd = "New-Item -Path 'HKLM:\SOFTWARE\LdacPanel' -Force | Out-Null; " +
                 "New-ItemProperty -Path 'HKLM:\SOFTWARE\LdacPanel' -Name UserModeAvdtp -Value " +
                 $UserModeAvdtp + " -PropertyType DWord -Force | Out-Null; " +
                 "(Get-ItemProperty 'HKLM:\SOFTWARE\LdacPanel').UserModeAvdtp"
    Guest $switchCmd 60 | Out-Null
    Write-Host "   UserModeAvdtp=$UserModeAvdtp"

    if (-not $SkipReboot) {
        Write-Host '== 4) 重启客户机（新映像必须重启才加载）==' -ForegroundColor Cyan
        Guest 'shutdown /r /t 0 /f' 20 | Out-Null
        Start-Sleep -Seconds 35
        $up = $false
        for ($i = 0; $i -lt 30; $i++) {
            $r = Guest '"up"' 8
            if ($r -match 'up') { $up = $true; break }
            Start-Sleep -Seconds 6
        }
        if (-not $up) { Fail '重启后 SSH 没起来' }
        Start-Sleep -Seconds 10
    }
}

Write-Host '== 5) 逐项验证 ==' -ForegroundColor Cyan
$check = @'
$ProgressPreference = "SilentlyContinue"
$sys   = (Get-Item C:\Windows\System32\drivers\LdacA2dp.sys -EA 0).LastWriteTime
$build = ((Get-Content C:\Users\Public\ldac-probe.log -Tail 200 -EA 0 | Select-String "build \[") | Select-Object -Last 1).Line
$dsvc  = ((& sc.exe query LdacA2dp 2>&1 | Out-String) -match "RUNNING")
$psvc  = ((& sc.exe query LdacA2dpSvc 2>&1 | Out-String) -match "RUNNING")
$auto  = (((& sc.exe qc LdacA2dpSvc 2>&1 | Out-String) -match "AUTO_START"))
$send  = @(Get-Process ldac-send -EA 0).Count
$um    = (Get-ItemProperty "HKLM:\SOFTWARE\LdacPanel" -EA 0).UserModeAvdtp
$node  = (Get-PnpDevice -EA 0 | Where-Object { $_.InstanceId -match "0000110B" } |
          Select-Object -First 1 | ForEach-Object { (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName "DEVPKEY_Device_Service" -EA 0).Data })
"sys    = $sys"
"build  = $build"
"driver = $dsvc"
"panel  = $psvc auto=$auto"
"senders= $send"
"switch = $um"
"node   = $node"
'@
Guest $check 90
