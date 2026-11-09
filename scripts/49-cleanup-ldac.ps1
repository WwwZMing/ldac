# ============================================================================
#  49-cleanup-ldac.ps1 -- 清理我们的驱动在真机上留下的所有多余残留
# ============================================================================
#  清什么：
#    1. 多余的设备实例（hwid = ROOT\LdacA2dp 的旧 root 设备）
#    2. 幽灵音频端点（Status = Unknown 且名字含 LDAC / VsToy）
#    3. 驱动库里没有被任何设备引用的旧包（ldaca2dp.inf / vstoy.inf）
#
#  绝不动什么：
#    - 当前绑在 BTHENUM A2DP 节点上的那一个实例（它的 DriverInfPath 会被跳过）
#
#  判据用 DEVPKEY_Device_DriverInfPath：包"是否在用"以设备实际引用为准，
#  不靠猜。删除在用的包 = marked-for-delete，会把耳机端点搞没（踩过两次）。
# ============================================================================
param([switch]$DryRun)

$ErrorActionPreference = 'Continue'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "必须管理员运行"
    }
}
Assert-Admin

function Get-Prop([string]$id, [string]$key) {
    try { return (Get-PnpDeviceProperty -InstanceId $id -KeyName $key -ErrorAction Stop).Data }
    catch { return $null }
}

Write-Host "=========== 当前状态 ===========" -ForegroundColor Cyan
Write-Host "-- 我们的设备实例 --"
$all = @(Get-PnpDevice -ErrorAction SilentlyContinue)
$ours = @()
foreach ($d in ($all | Where-Object { $_.InstanceId -like 'ROOT\*' -or $_.InstanceId -like 'BTHENUM\*' })) {
    $hw = Get-Prop $d.InstanceId 'DEVPKEY_Device_HardwareIds'
    if ($hw -and (($hw -join ',') -match 'LdacA2dp|VsToy')) {
        $inf = Get-Prop $d.InstanceId 'DEVPKEY_Device_DriverInfPath'
        $svc = Get-Prop $d.InstanceId 'DEVPKEY_Device_Service'
        $ours += [pscustomobject]@{ Dev = $d; Hwid = ($hw -join ','); Inf = $inf; Svc = $svc }
        Write-Host "  [$($d.Status)] $($d.FriendlyName)"
        Write-Host "        id=$($d.InstanceId)"
        Write-Host "        hwid=$($hw -join ',')  inf=$inf  svc=$svc"
    }
}
if ($ours.Count -eq 0) { Write-Host "  (none)" }

Write-Host "`n-- 音频端点 --"
$eps = @(Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue)
foreach ($e in $eps) { Write-Host "  [$($e.Status)] $($e.FriendlyName)" }

Write-Host "`n-- 驱动库里的我们的包 --"
$lines = & pnputil /enum-drivers 2>&1
$pub = $null
$pkgs = @()
foreach ($l in $lines) {
    if ($l -match ':\s*(oem\d+\.inf)\s*$') { $pub = $Matches[1] }
    elseif ($l -match ':\s*(\S*(ldaca2dp|vstoy)\.inf)\s*$' -and $pub) {
        $pkgs += [pscustomobject]@{ Published = $pub; Original = $Matches[1] }
        Write-Host "  $pub  <-  $($Matches[1])"
    }
}
if ($pkgs.Count -eq 0) { Write-Host "  (none)" }

# ---------------------------------------------------------------- 1) 设备
Write-Host "`n=========== 1) 删多余设备实例 ===========" -ForegroundColor Cyan
$keep = @()
$drop = @()
foreach ($o in $ours) {
    # 只保留"绑在 BTHENUM 上、且当前就是功能驱动"的那一个
    if ($o.Dev.InstanceId -like 'BTHENUM\*' -and $o.Dev.Status -eq 'OK') {
        $keep += $o
    } else {
        $drop += $o
    }
}
if ($drop.Count -eq 0) { Write-Host "  (没有要删的)" }
foreach ($o in $drop) {
    Write-Host "  remove $($o.Dev.InstanceId)  [$($o.Dev.Status)] $($o.Dev.FriendlyName)"
    if (-not $DryRun) { & pnputil /remove-device $o.Dev.InstanceId 2>&1 | ForEach-Object { Write-Host "    $_" } }
}
Write-Host "  保留：$(if ($keep.Count) { ($keep | ForEach-Object { $_.Dev.InstanceId }) -join '; ' } else { '(无)' })"

# ---------------------------------------------------------------- 2) 端点
Write-Host "`n=========== 2) 删幽灵音频端点 ===========" -ForegroundColor Cyan
$epDrop = @($eps | Where-Object {
    $_.Status -ne 'OK' -and ($_.FriendlyName -match 'LDAC A2DP|VsToy')
})
if ($epDrop.Count -eq 0) { Write-Host "  (没有要删的)" }
foreach ($e in $epDrop) {
    Write-Host "  remove [$($e.Status)] $($e.FriendlyName)"
    if (-not $DryRun) { & pnputil /remove-device $e.InstanceId 2>&1 | ForEach-Object { Write-Host "    $_" } }
}

# ---------------------------------------------------------------- 3) 包
Write-Host "`n=========== 3) 删没人用的旧包 ===========" -ForegroundColor Cyan
Start-Sleep -Seconds 2
$inUse = @()
foreach ($d in @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'OK' })) {
    $inf = Get-Prop $d.InstanceId 'DEVPKEY_Device_DriverInfPath'
    if ($inf) { $inUse += $inf.ToLower() }
}
foreach ($p in $pkgs) {
    if ($inUse -contains $p.Published.ToLower()) {
        Write-Host "  KEEP $($p.Published)  ($($p.Original)) —— 有设备在用"
    } else {
        Write-Host "  delete $($p.Published)  ($($p.Original))"
        if (-not $DryRun) { & pnputil /delete-driver $p.Published /uninstall /force 2>&1 | ForEach-Object { Write-Host "    $_" } }
    }
}

# ---------------------------------------------------------------- 4) 复核
Write-Host "`n=========== 4) 复核 ===========" -ForegroundColor Cyan
Start-Sleep -Seconds 2
Write-Host "-- 设备 --"
foreach ($d in @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '0000110B' })) {
    $svc = Get-Prop $d.InstanceId 'DEVPKEY_Device_Service'
    Write-Host "  [$($d.Status)] $($d.FriendlyName)  service=$svc  problem=$($d.Problem)"
}
Write-Host "-- 端点 --"
Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "  [$($_.Status)] $($_.FriendlyName)" }
Write-Host "-- 驱动库 --"
$lines2 = & pnputil /enum-drivers 2>&1
$pub2 = $null
$n = 0
foreach ($l in $lines2) {
    if ($l -match ':\s*(oem\d+\.inf)\s*$') { $pub2 = $Matches[1] }
    elseif ($l -match ':\s*(\S*(ldaca2dp|vstoy)\.inf)\s*$' -and $pub2) {
        Write-Host "  $pub2  <-  $($Matches[1])"; $n++
    }
}
Write-Host "  我们剩 $n 个包（期望 1）"
