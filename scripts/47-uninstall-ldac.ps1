# ============================================================================
#  47-uninstall-ldac.ps1 -- 把 LdacA2dp 从真机撤干净，恢复 inbox btha2dp
# ============================================================================
#  建立背景：这个驱动是「功能驱动顶替 BTHENUM A2DP 子设备上的 btha2dp」，
#  所以撤不干净的直接后果是**耳机彻底没有音频端点**。出事后第一件事就是跑这个。
#
#  用法（管理员 PowerShell）：
#      .\scripts\47-uninstall-ldac.ps1
#      .\scripts\47-uninstall-ldac.ps1 -Reboot      # 撤完直接重启
# ============================================================================
param(
    [switch]$Reboot
)

$ErrorActionPreference = 'Continue'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "必须用管理员权限运行"
    }
}
Assert-Admin

Write-Host "=== 1) 找 A2DP 节点 ===" -ForegroundColor Cyan
$nodes = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '0000110B' })
if ($nodes.Count -eq 0) { Write-Host "  (没有 A2DP 节点)" -ForegroundColor Yellow }
foreach ($n in $nodes) {
    $svc = $null
    try { $svc = (Get-PnpDeviceProperty -InstanceId $n.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop).Data } catch {}
    Write-Host "  [$($n.Status)] $($n.FriendlyName)  service=$svc"
    Write-Host "        $($n.InstanceId)"
}

Write-Host "`n=== 2) 删掉我们的驱动包（/uninstall 会让设备退回 inbox） ===" -ForegroundColor Cyan
$lines = & pnputil /enum-drivers 2>&1
$pub = $null
$deleted = 0
foreach ($l in $lines) {
    if ($l -match ':\s*(oem\d+\.inf)\s*$') { $pub = $Matches[1] }
    elseif ($l -match ':\s*(\S*ldaca2dp\.inf)\s*$' -and $pub) {
        Write-Host "  delete $pub"
        & pnputil /delete-driver $pub /uninstall /force 2>&1 | ForEach-Object { Write-Host "    $_" }
        $deleted++
    }
}
if ($deleted -eq 0) { Write-Host "  (驱动库里没有 ldaca2dp.inf)" }

Write-Host "`n=== 3) 确认节点已回到 inbox ===" -ForegroundColor Cyan
Start-Sleep -Seconds 2
foreach ($n in $nodes) {
    $d = Get-PnpDevice -InstanceId $n.InstanceId -ErrorAction SilentlyContinue
    if ($d) {
        $svc = $null
        try { $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop).Data } catch {}
        Write-Host "  [$($d.Status)] $($d.FriendlyName)  service=$svc"
        if ($svc -ne 'BthA2dp') { Write-Host "  !! 还不是 BthA2dp，需要重启" -ForegroundColor Yellow }
    }
}

Write-Host "`n=== 4) 服务/控制设备 ===" -ForegroundColor Cyan
$s = Get-Service LdacA2dp -ErrorAction SilentlyContinue
if ($s) {
    Write-Host "  LdacA2dp service = $($s.Status)（若是 Stopped 且无设备引用，可 sc delete）"
} else { Write-Host "  LdacA2dp service = (absent)" }
Write-Host "  \\.\LdacCtl 可打开: $(if (Test-Path '\\.\LdacCtl') { 'YES' } else { 'no' })"

Write-Host "`n=== 5) 音频端点 ===" -ForegroundColor Cyan
Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "  [$($_.Status)] $($_.FriendlyName)" }

Write-Host "`n=== 6) 蓝牙类设备 ===" -ForegroundColor Cyan
Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'USB\*' } |
    ForEach-Object { Write-Host "  [$($_.Status)] $($_.FriendlyName)" }

Write-Host "`n完成。若耳机仍无端点，重启一次即可恢复 inbox 接管。" -ForegroundColor Green

if ($Reboot) {
    Write-Host "正在重启 ..." -ForegroundColor Yellow
    Restart-Computer -Force
}
