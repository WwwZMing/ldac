# ============================================================================
#  vm-restore-inbox.ps1 - guest: 把我们驱动从耳机节点撤掉，恢复 inbox btha2dp
# ============================================================================
#  目的：拿到一个"干净基线"—— 用 Windows 自带的蓝牙栈去连耳机。
#  只有基线能连上，才能反证"我们的驱动/VMware 虚拟蓝牙"哪个是问题。
#
#  ⚠ 撤驱动会把端点一起带走（这正是它该做的事）。别在这之后还指望有声音。
# ============================================================================
$ErrorActionPreference = 'Continue'

"=== 1) 当前 A2DP 节点的函数驱动 ==="
$node = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '0000110B' } | Select-Object -First 1
if ($node) {
    $svc = $null
    try { $svc = (Get-PnpDeviceProperty -InstanceId $node.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop).Data } catch {}
    "  $($node.InstanceId)"
    "  friendly = $($node.FriendlyName)"
    "  service  = $svc"
} else { "  !! no A2DP node" }

"=== 2) 删除我们的驱动包（/uninstall 会把设备退回 inbox） ==="
$lines = & pnputil /enum-drivers 2>&1
$pub = $null
foreach ($l in $lines) {
    if ($l -match ':\s*(oem\d+\.inf)\s*$') { $pub = $Matches[1] }
    elseif ($l -match ':\s*(\S*ldaca2dp\.inf)\s*$' -and $pub) {
        "  delete $pub"
        & pnputil /delete-driver $pub /uninstall /force 2>&1 | ForEach-Object { "    $_" }
    }
}

"=== 3) 剩下的 ldac 包（应为空） ==="
& pnputil /enum-drivers 2>&1 | Select-String -Pattern 'ldaca2dp|LdacA2dp' | ForEach-Object { "  $_" }

"=== 4) 节点现在的 service（期望 BthA2dp） ==="
if ($node) {
    $d2 = Get-PnpDevice -InstanceId $node.InstanceId -ErrorAction SilentlyContinue
    if ($d2) {
        $svc2 = $null
        try { $svc2 = (Get-PnpDeviceProperty -InstanceId $d2.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop).Data } catch {}
        "  [$($d2.Status)] $($d2.FriendlyName)   service=$svc2"
    }
}

"=== 5) 音频端点 ==="
Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue |
    ForEach-Object { "  [$($_.Status)] $($_.FriendlyName)" }

"=== 6) 控制设备应该也没了 ==="
"  \\.\LdacCtl present: $(Test-Path '\\.\LdacCtl')"

"NOTE: 需要重启客户机让 btha2dp 完整接管。"
